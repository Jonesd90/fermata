// The readers' fuzz test: damaged and random input, many thousands of times over, for every part of Fermata that reads something it did not
// write itself in this session - files from other programs (AAF, WAV headers, iXML track names), files people send (listeners' comments),
// saved projects and settings (JSON), the control surface (MIDI), the Stream Deck (command lines), the Merging devices and ANEMAN (JSON),
// typed times, the names of files being imported, and timecode audio.
//
// It is built with AddressSanitizer and UndefinedBehaviorSanitizer (tools/tool-checks.sh fuzz). A crash, a memory error, undefined behaviour
// or one input that takes more than 30 seconds stops it with a report, and the input is kept as fuzz-crash-<target>.bin (or fuzz-hang-...;
// after undefined behaviour it is last-input.bin in the work folder, which tools/tool-checks.sh copies out as last-input-<target>.bin).
// A broken rule (a mended WAV changing length, a project that saves into a file it cannot read back) is printed as FINDING and counted; the
// first few such inputs are kept as fuzz-finding-<target>-<n>.bin.
//
// Usage: fuzz_readers <seconds per target> [target ...]      targets: wav ixml aaf feedback mcu remote json devices text import ltc caches (default: all)
//        (3.18.3: "caches" = the take-matching fingerprints (.fpr) and waveform outlines (.fwave2) Fermata keeps; the json target's seed project now has
//        every field added since 3.1.1; the aaf target starts from an AAF Fermata itself exports when no FUZZ_AAF file is given)
//        fuzz_readers --replay <target> <file>               runs one kept input again
// The random choices start from a fixed seed (the FUZZ_SEED environment variable changes it), so a run can be repeated exactly. The AAF target
// needs a real AAF file to start from: FUZZ_AAF=<file> (it is skipped without one).

#include "../src/core/Project.h"
#include "../src/core/WavRepair.h"
#include "../src/core/ImportPlan.h"
#include "../src/core/Aaf.h"
#include "../src/core/Feedback.h"
#include "../src/core/Mcu.h"
#include "../src/core/RemoteProtocol.h"
#include "../src/core/Ravenna.h"
#include "../src/core/PreampMemory.h"
#include <cmath>
#include "../src/core/ScreenLayout.h"
#include "../src/core/MasterDef.h"
#include "../src/core/TimeFormat.h"
#include "../src/core/SessionClock.h"
#include "../src/core/Automation.h"
#include "../src/core/Ltc.h"
#include "../src/core/TextBytes.h"
#include "../src/core/AudioFormats.h"
#include "../src/core/TakeMatch.h"
#include "../src/core/AafExport.h"
#include "../src/core/AudioOps.h"
#include "../src/core/Ltc.h"
#include "../src/ui/WaveThumb.h"
#include <sanitizer/common_interface_defs.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <cstring>
#include <functional>
#include <random>
#include <set>
#include <thread>

using namespace td;

namespace
{
using Bytes = std::vector<std::uint8_t>;

std::mt19937_64 rng;
int rnd (int n) { return n <= 0 ? 0 : (int) (rng() % (std::uint64_t) n); }
size_t rndz (size_t n) { return n == 0 ? 0 : (size_t) (rng() % (std::uint64_t) n); }
bool oneIn (int n) { return rnd (n) == 0; }

// ---------------------------------------------------------------- what is kept when something goes wrong
std::atomic<const Bytes*> current { nullptr };
std::atomic<const char*> currentTarget { "" };
std::atomic<long long> inputStartedMs { 0 };
std::atomic<bool> allDone { false };
juce::File workDir;
int findings = 0, keptFindings = 0;

long long nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now().time_since_epoch()).count(); }

void keep (const juce::String& name, const Bytes& b)
{
    juce::File (juce::File::getCurrentWorkingDirectory().getChildFile (name)).replaceWithData (b.data(), b.size());
}

extern "C" void onDeath()
{
    if (auto* b = current.load())
    {
        const auto name = juce::String ("fuzz-crash-") + currentTarget.load() + ".bin";
        keep (name, *b);
        std::fprintf (stderr, "\n*** the input that did it is kept in %s\n", name.toRawUTF8());
    }
}

void finding (const juce::String& what)
{
    ++findings;
    std::printf ("FINDING [%s]: %s\n", currentTarget.load(), what.toRawUTF8());
    if (auto* b = current.load(); b != nullptr && keptFindings < 12)
        keep (juce::String ("fuzz-finding-") + currentTarget.load() + "-" + juce::String (++keptFindings) + ".bin", *b);
}

/** One input: remembered for the death callback and timed by the watchdog. */
/** Each input is also written to last-input.bin in the work folder (in memory, /dev/shm, when there is one) before it is tried: undefined
    behaviour stops the program through UBSan's own exit, which runs no callback, so that file is what is left to reproduce it from
    (tools/tool-checks.sh copies it out, with last-target.txt). */
std::string lastInputPath;
struct Trying
{
    explicit Trying (const Bytes& b)
    {
        current = &b; inputStartedMs = nowMs();
        if (! lastInputPath.empty())
            if (auto* fp = std::fopen (lastInputPath.c_str(), "wb")) { if (! b.empty()) std::fwrite (b.data(), 1, b.size(), fp); std::fclose (fp); }
    }
    ~Trying() { inputStartedMs = 0; current = nullptr; }
};
void noteTarget (const char* name) { workDir.getChildFile ("last-target.txt").replaceWithText (name); }

// ---------------------------------------------------------------- mutations
const std::uint32_t k32[] = { 0, 1, 2, 3, 4, 7, 8, 16, 28, 36, 44, 52, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x10000,
                              0x7fffffff, 0x80000000u, 0xfffffff0u, 0xfffffffau, 0xfffffffeu, 0xffffffffu };
const std::uint64_t k64[] = { 0, 1, 0xffffffffull, 0x100000000ull, 0x7fffffffffffffffull, 0x8000000000000000ull, 0xffffffffffffffffull };

/** Mostly near the start (where headers are) when 'focus' says how many bytes the start is. */
size_t where (size_t n, size_t focus) { return focus > 0 && focus < n && ! oneIn (3) ? rndz (focus) : rndz (n); }

void mutate (Bytes& d, size_t focus, const Bytes* other = nullptr)
{
    const int k = 1 + rnd (oneIn (4) ? 24 : 5);
    for (int i = 0; i < k; ++i)
    {
        const size_t n = d.size();
        switch (rnd (11))
        {
            case 0: if (n) d[where (n, focus)] ^= (std::uint8_t) (1u << rnd (8)); break;
            case 1: if (n) d[where (n, focus)] = (std::uint8_t) rng(); break;
            case 2: if (n) d[where (n, focus)] = (std::uint8_t) k32[rndz (std::size (k32))]; break;
            case 3: if (n >= 4) { const auto p = where (n - 3, focus); const auto v = k32[rndz (std::size (k32))]; std::memcpy (&d[p], &v, 4); } break;
            case 4: if (n >= 8) { const auto p = where (n - 7, focus); const auto v = k64[rndz (std::size (k64))]; std::memcpy (&d[p], &v, 8); } break;
            case 5: if (n) { const auto p = where (n, focus); const auto len = 1 + rndz (std::min<size_t> (n - p, 64)); d.erase (d.begin() + (long) p, d.begin() + (long) std::min (n, p + len)); } break;
            case 6: { const auto p = n ? where (n, focus) : 0; Bytes ins (1 + rndz (64)); for (auto& b : ins) b = (std::uint8_t) rng(); d.insert (d.begin() + (long) p, ins.begin(), ins.end()); } break;
            case 7: if (n > 1 && oneIn (3)) d.resize (rndz (n)); break;
            case 8: if (n > 2) { const auto a = rndz (n), b = rndz (n); const auto len = 1 + rndz (std::min<size_t> (n - std::max (a, b), 32)); std::memmove (&d[b], &d[a], len); } break;
            case 9: if (n) { const auto p = where (n, focus); d[p] = (std::uint8_t) (d[p] + (oneIn (2) ? 1 : -1) * (1 + rnd (16))); } break;
            case 10: if (other != nullptr && other->size() > 1 && n > 1)
                     { const auto a = rndz (other->size()), p = rndz (n); const auto len = 1 + rndz (std::min (other->size() - a, n - p)); std::memcpy (&d[p], other->data() + a, len); } break;
            default: break;
        }
    }
}

const char* const kTokens[] = { "0", "1", "-1", "7", "1e308", "-1e308", "1e309", "-1e309", "4.9e-324", "99999999999999999999", "2147483647", "2147483648",
                                "-2147483649", "9223372036854775808", "0.0", "-0", ".5", "5.", "NaN", "inf", ":", "::", ".", ",", ";", "\t", "\n", "\r\n", "\r",
                                "\"", "\\", "{", "}", "[", "]", "null", "true", "false", "\xef\xbb\xbf", "\xff\xfe", "\xc3\xa9", "\xe2\x9c\x93", "\xf0\x9f\x8e\xbb",
                                "\x80", "\xc0\xaf", "\xed\xa0\x80", "+", "-", "e", "E", "h", "m", "s", "f", "ms", "00:00:00:00", "23:59:59:29", "99:99:99:99",
                                "1:2:3:4:5", "12:34.567", ";;", "  ", "", "\0", "%", "&", "<", ">", "{\"a\":", "[[[[[[[[", "]]]]]]]]" };

void mutateText (Bytes& d)
{
    const int k = 1 + rnd (6);
    for (int i = 0; i < k; ++i)
    {
        if (oneIn (2)) { mutate (d, 0); continue; }
        const char* t = kTokens[rndz (std::size (kTokens))];
        const size_t len = std::strlen (t) + (t[0] == 0 ? 1 : 0);
        const size_t p = rndz (d.size() + 1);
        if (oneIn (3) && p < d.size())
        {   // replace a run of digits or letters with the token
            size_t e = p; while (e < d.size() && std::isalnum (d[e])) ++e;
            d.erase (d.begin() + (long) p, d.begin() + (long) e);
        }
        d.insert (d.begin() + (long) p, t, t + len);
    }
}

// ---- JSON: change the values and the shape of a parsed var, not its text (text changes are mostly refused by the JSON reader first)
juce::StringArray namePool;

void collectNames (const juce::var& v, int depth = 0)
{
    if (depth > 30 || namePool.size() > 4000) return;
    if (auto* a = v.getArray()) for (auto& e : *a) collectNames (e, depth + 1);
    else if (auto* o = v.getDynamicObject()) for (auto& nv : o->getProperties()) { namePool.addIfNotAlreadyThere (nv.name.toString()); collectNames (nv.value, depth + 1); }
}

void collectNodes (juce::var& v, std::vector<juce::var*>& out, int depth = 0)
{
    out.push_back (&v);
    if (depth > 40) return;
    if (auto* a = v.getArray()) for (auto& e : *a) collectNodes (e, out, depth + 1);
    else if (auto* o = v.getDynamicObject())
    {
        auto& props = o->getProperties();
        for (int i = 0; i < props.size(); ++i) if (auto* p = props.getVarPointerAt (i)) collectNodes (*p, out, depth + 1);
    }
}

juce::var randomScalar()
{
    static const int ints[] = { 0, 1, -1, 2, 3, 7, 8, 16, 63, 64, 65, 255, 1000, 65535, 99999, 2147483647, -2147483647 - 1 };
    static const double doubles[] = { 0.0, -0.0, 0.5, -1.5, 1.0e9, -1.0e9, 1.0e300, -1.0e300, 1.0e-300, 4.9e-324, 44100.0, 96000.0, 2.5e10, 1.0e19, -1.0e19 };
    switch (rnd (8))
    {
        case 0: return juce::var();
        case 1: return ints[rndz (std::size (ints))];
        case 2: return (juce::int64) (oneIn (2) ? (juce::int64) 0x7fffffffffffffffLL : (juce::int64) (rng() >> rnd (64)));
        case 3: return doubles[rndz (std::size (doubles))];
        case 4: return oneIn (2);
        case 5:
        {
            static const char* const strs[] = { "", "x", "0", "-3", "1e400", "12:00", "{00000000-0000-0000-0000-000000000000}", "not-a-uuid", "../../x", "\\\\server\\share\\a.wav",
                                                "C:\\Audio\\a.wav", "/x/y.wav", "\xc3\xa9t\xc3\xa9 \xe2\x9c\x93", "\xff\xfe\x80", "Main", "Takes", "true" };
            if (oneIn (8)) return juce::String::repeatedString ("a", 1 + rnd (20000));
            return juce::String::fromUTF8 (strs[rndz (std::size (strs))]);
        }
        case 6: return juce::var (juce::Array<juce::var>());
        default: return juce::var (new juce::DynamicObject());
    }
}

void mutateVar (juce::var& root)
{
    std::vector<juce::var*> nodes; collectNodes (root, nodes);
    if (nodes.empty()) return;
    juce::var& x = *nodes[rndz (nodes.size())];
    switch (rnd (10))
    {
        case 0: case 1: x = randomScalar(); break;
        case 2: if (x.isInt() || x.isInt64() || x.isDouble())                                              // a nearby or a far-off number
                { const double v = (double) x; x = oneIn (2) ? juce::var (v + (oneIn (2) ? 1 : -1) * (1 + rnd (3))) : juce::var (v * (oneIn (2) ? -1.0 : 1000.0)); }
                else x = randomScalar();
                break;
        case 3: if (auto* a = x.getArray(); a != nullptr && ! a->isEmpty()) a->remove (rnd (a->size())); else x = randomScalar(); break;
        case 4: if (auto* a = x.getArray(); a != nullptr && ! a->isEmpty())
                { const auto e = (*a)[rnd (a->size())]; const int times = oneIn (4) ? 50 + rnd (200) : 1 + rnd (3); for (int t = 0; t < times; ++t) a->insert (rnd (a->size() + 1), e.clone()); }
                break;
        case 5: if (auto* a = x.getArray(); a != nullptr && a->size() > 1) a->swap (rnd (a->size()), rnd (a->size())); else if (auto* ar = x.getArray()) ar->clear(); break;
        case 6: if (auto* o = x.getDynamicObject(); o != nullptr && o->getProperties().size() > 0) o->removeProperty (o->getProperties().getName (rnd (o->getProperties().size())));
                break;
        case 7: if (auto* o = x.getDynamicObject(); o != nullptr && ! namePool.isEmpty()) o->setProperty (juce::Identifier (namePool[rnd (namePool.size())]), randomScalar());
                break;
        case 8: { const auto other = nodes[rndz (nodes.size())]->clone(); x = other; } break;           // something of another kind where this was
        default: if (auto* o = x.getDynamicObject(); o != nullptr && o->getProperties().size() > 0)     // a property given another kind of value
                 { auto& props = o->getProperties(); auto* p = props.getVarPointerAt (rnd (props.size())); if (p != nullptr) *p = randomScalar(); }
                 break;
    }
}

Bytes bytesOf (const juce::String& s) { const auto* p = s.toRawUTF8(); return Bytes (p, p + std::strlen (p)); }
/** Text as the program gets it: lines and files are read with textFromBytes (bytes that are not UTF-8 are taken as the Windows code page), and
    what is typed is always valid. (A juce::String made from bytes that are not valid UTF-8 reads past its own end: never done with outside data.) */
juce::String textOf (const Bytes& b) { return textFromBytes (b.data(), b.size()); }
Bytes bytesOfFile (const juce::File& f) { juce::MemoryBlock mb; f.loadFileAsData (mb); return Bytes ((const std::uint8_t*) mb.getData(), (const std::uint8_t*) mb.getData() + mb.getSize()); }
void write (const juce::File& f, const Bytes& b) { f.replaceWithData (b.data(), b.size()); }

// ---------------------------------------------------------------- seeds that are made here
void put32 (Bytes& b, std::uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back ((std::uint8_t) (v >> (8 * i))); }
void put16 (Bytes& b, std::uint16_t v) { b.push_back ((std::uint8_t) v); b.push_back ((std::uint8_t) (v >> 8)); }
void put64 (Bytes& b, std::uint64_t v) { put32 (b, (std::uint32_t) v); put32 (b, (std::uint32_t) (v >> 32)); }
void tag (Bytes& b, const char* t) { b.insert (b.end(), t, t + 4); }

/** A WAV as Fermata and others write it: 'stopped' = as left by a recording that never finished (sizes 0), 'rf64' = with a ds64 chunk,
    'junk' = the placeholder Fermata leaves for the RF64 header, 'tail' = a LIST chunk after the audio, 'ixml' = an iXML chunk with track names. */
Bytes makeWav (int ch, int bits, bool isFloat, int frames, bool junk, bool rf64, bool stopped, bool tail, bool extensible, const juce::String& ixml = {})
{
    const int ba = ch * bits / 8;
    const auto dataBytes = (std::uint32_t) (frames * ba);
    Bytes b; tag (b, rf64 ? "RF64" : "RIFF"); put32 (b, 0); tag (b, "WAVE");
    if (rf64) { tag (b, "ds64"); put32 (b, 28); put64 (b, 0); put64 (b, dataBytes); put64 (b, (std::uint64_t) frames); put32 (b, 0); }
    else if (junk) { tag (b, "JUNK"); put32 (b, 28); b.insert (b.end(), 28, 0); }
    tag (b, "fmt "); put32 (b, extensible ? 40 : 16);
    put16 (b, extensible ? 0xFFFE : (isFloat ? 3 : 1)); put16 (b, (std::uint16_t) ch); put32 (b, 48000); put32 (b, (std::uint32_t) (48000 * ba)); put16 (b, (std::uint16_t) ba); put16 (b, (std::uint16_t) bits);
    if (extensible)
    {
        put16 (b, 22); put16 (b, (std::uint16_t) bits); put32 (b, ch == 1 ? 4u : 3u);
        const std::uint8_t guid[16] = { (std::uint8_t) (isFloat ? 3 : 1), 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71 };
        b.insert (b.end(), guid, guid + 16);
    }
    if (ixml.isNotEmpty())
    {
        const auto x = bytesOf (ixml); tag (b, "iXML"); put32 (b, (std::uint32_t) x.size()); b.insert (b.end(), x.begin(), x.end()); if (x.size() & 1) b.push_back (0);
    }
    tag (b, "data"); put32 (b, rf64 ? 0xFFFFFFFFu : (stopped ? 0u : dataBytes));
    for (std::uint32_t i = 0; i < dataBytes; ++i) b.push_back ((std::uint8_t) (i * 37 + 11));
    if (tail) { tag (b, "LIST"); put32 (b, 13); tag (b, "INFO"); tag (b, "ISFT"); put32 (b, 1); b.push_back ('F'); b.push_back (0); }
    if (! stopped && ! rf64) { const auto riff = (std::uint32_t) (b.size() - 8); std::memcpy (&b[4], &riff, 4); }
    if (rf64) { const auto riff = (std::uint64_t) (b.size() - 8); std::memcpy (&b[20], &riff, 8); std::uint32_t ff = 0xFFFFFFFFu; std::memcpy (&b[4], &ff, 4); }
    return b;
}

// ---------------------------------------------------------------- the targets
struct Target
{
    const char* name;
    std::function<bool()> prepare;                 // false: cannot run here (says why)
    std::function<void (const Bytes*)> one;        // one input: a new random one, or the one given (replay)
};

juce::File scratch (const char* name) { return workDir.getChildFile (name); }

// ---- WAV headers: the mending of files a recording left behind
std::vector<Bytes> wavSeeds;
void wavOneWith (const Bytes& d, bool extend);
void wavOne (const Bytes* given)
{
    if (given != nullptr) { wavOneWith (*given, false); wavOneWith (*given, true); return; }
    Bytes d = wavSeeds[rndz (wavSeeds.size())];
    mutate (d, 160, &wavSeeds[rndz (wavSeeds.size())]);
    wavOneWith (d, oneIn (2));
}
void wavOneWith (const Bytes& d, bool extend)
{
    Trying t (d);
    const auto f = scratch ("w.wav");
    write (f, d);
    const auto look = wavrepair::inspect (f, false, extend);
    if (bytesOfFile (f) != d) finding ("looking at a file (no repair) changed it");
    const auto mended = wavrepair::inspect (f, true, extend);
    const auto after = bytesOfFile (f);
    if (after.size() != d.size()) finding ("mending changed the file's length from " + juce::String ((int) d.size()) + " to " + juce::String ((int) after.size()));
    if (look.ok != mended.ok || look.frames != mended.frames) finding ("looking and mending disagree about the audio in the file");
    if (mended.wasBroken)
    {
        const auto again = wavrepair::inspect (f, false, extend);
        if (! again.ok || again.wasBroken || again.frames != mended.frames)
            finding ("a mended file is not whole when looked at again (ok " + juce::String ((int) again.ok) + ", broken " + juce::String ((int) again.wasBroken)
                     + ", frames " + juce::String (again.frames) + " vs " + juce::String (mended.frames) + ")");
        // the audio itself is never touched by mending: only the bytes of the header
        size_t firstAudio = d.size();
        for (size_t i = 12; i + 8 <= d.size(); ) { if (std::memcmp (&d[i], "data", 4) == 0) { firstAudio = i + 8; break; } std::uint32_t s; std::memcpy (&s, &d[i + 4], 4); if (s > d.size()) break; i += 8 + s + (s & 1); }
        for (size_t i = firstAudio; i < d.size() && i < after.size(); ++i) if (d[i] != after[i]) { finding ("mending changed audio bytes (at " + juce::String ((int) i) + ")"); break; }
    }
    // and what the program does next with such a file: read it (through the reader every part of the program uses)
    CheckedWavAudioFormat wav;
    if (auto* in = new juce::FileInputStream (f); in->openedOk())
    {
        std::unique_ptr<juce::AudioFormatReader> r (wav.createReaderFor (in, true));
        if (r != nullptr && r->numChannels > 0 && r->numChannels <= 64)
        {
            juce::AudioBuffer<float> buf ((int) r->numChannels, 512);
            r->read (&buf, 0, 512, 0, true, true);
            // a mended file with an ordinary format chunk must read as long as the mending said (that is what makes the take play again)
            if (mended.wasBroken && mended.ok && mended.frames < 0xfffffff0LL / 64)
            {
                // (only a file JUCE's reader can walk: it stops at an unknown chunk of size 0, and it takes the last format chunk - such files
                //  read as nothing before mending as well, so they say nothing about the mending)
                int fmtTag = 0, ch = 0, align = 0, bits = 0, formats = 0; bool emptyChunk = false;
                for (size_t i = 12; i + 8 <= after.size(); )
                {
                    std::uint32_t sz; std::memcpy (&sz, &after[i + 4], 4);
                    const bool isFmt = std::memcmp (&after[i], "fmt ", 4) == 0, isData = std::memcmp (&after[i], "data", 4) == 0;
                    if (isFmt && sz >= 16 && i + 24 <= after.size())
                    { ++formats; fmtTag = after[i + 8] | (after[i + 9] << 8); ch = after[i + 10] | (after[i + 11] << 8); align = after[i + 20] | (after[i + 21] << 8); bits = after[i + 22] | (after[i + 23] << 8); }
                    if (isData || sz > after.size()) break;
                    if (! isFmt && sz == 0) emptyChunk = true;
                    i += 8 + sz + (sz & 1);
                }
                const bool ordinary = ! emptyChunk && formats == 1 && (fmtTag == 1 || fmtTag == 3) && ch >= 1 && (bits == 16 || bits == 24 || bits == 32) && align == ch * bits / 8;
                if (ordinary && (int) r->numChannels == ch && r->lengthInSamples != mended.frames)
                    finding ("a mended file reads as " + juce::String (r->lengthInSamples) + " frames, but mending kept " + juce::String (mended.frames));
            }
        }
    }
    else delete in;
}

// ---- iXML track names in field recorders' files
std::vector<Bytes> ixmlSeeds;
void ixmlOne (const Bytes* given)
{
    Bytes d = given != nullptr ? *given : ixmlSeeds[rndz (ixmlSeeds.size())];
    if (given == nullptr) { if (oneIn (2)) mutate (d, 0); else mutateText (d); }
    Trying t (d);
    const auto f = scratch ("x.wav");
    write (f, d);
    static const int chans[] = { 0, 1, 2, 4, 8, 32, 64, -1 };
    const auto names = readIXmlTrackNames (f, chans[rndz (std::size (chans))]);
    (void) names;
}

// ---- AAF from other editing programs
Bytes aafSeed; std::vector<size_t> aafHotSpots;      // the header, the sector index (FAT) and the directory: where damage matters most
void aafPrepareHotSpots()
{
    const auto* d = aafSeed.data(); const size_t n = aafSeed.size();
    auto rd32 = [&] (size_t o) -> std::uint32_t { return o + 4 <= n ? (std::uint32_t) d[o] | ((std::uint32_t) d[o + 1] << 8) | ((std::uint32_t) d[o + 2] << 16) | ((std::uint32_t) d[o + 3] << 24) : 0u; };
    const size_t ss = (size_t) 1 << juce::jlimit (7, 16, (int) (d[30] | (d[31] << 8)));
    for (size_t i = 0; i < 512; i += 4) aafHotSpots.push_back (i);
    for (int i = 0; i < 109; ++i) { const auto s = rd32 (76 + 4 * (size_t) i); if (s < 0xFFFFFFFAu) for (size_t k = 0; k < ss; k += 16) aafHotSpots.push_back ((s + 1) * ss + k); }
    const auto dir = rd32 (48);
    if (dir < 0xFFFFFFFAu) for (size_t k = 0; k < ss * 8; k += 4) aafHotSpots.push_back ((dir + 1) * ss + k);
    aafHotSpots.erase (std::remove_if (aafHotSpots.begin(), aafHotSpots.end(), [&] (size_t o) { return o + 8 > n; }), aafHotSpots.end());
}
void aafOne (const Bytes* given)
{
    // the file is big (tens of MB): change a few bytes of a copy in place, and put them back afterwards
    const auto f = scratch ("a.aaf");
    static bool written = false;
    if (given != nullptr)
    {   // a kept input is the list of changes (8 bytes where, 8 bytes what), made to the AAF named by FUZZ_AAF
        Bytes d = aafSeed;
        for (size_t i = 0; i + 16 <= given->size(); i += 16)
        {
            std::uint64_t at; std::memcpy (&at, given->data() + i, 8);
            if (at + 8 <= d.size()) std::memcpy (d.data() + at, given->data() + i + 8, 8);
        }
        write (f, d); written = false;
        Trying t (*given);
        (void) readAaf (f, 10.0);
        return;
    }
    if (! written) { write (f, aafSeed); written = true; }
    struct Patch { size_t at; Bytes old; };
    std::vector<Patch> patches;
    Bytes record;                                                      // what was changed, kept if it goes wrong: offset (8 bytes) + 8 new bytes, per change
    juce::FileOutputStream out (f);
    if (out.failedToOpen()) return;
    const int k = 1 + rnd (oneIn (3) ? 40 : 6);
    for (int i = 0; i < k; ++i)
    {
        const size_t at = (! aafHotSpots.empty() && ! oneIn (4)) ? aafHotSpots[rndz (aafHotSpots.size())] + rndz (4) : rndz (aafSeed.size() - 8);
        Bytes nb (aafSeed.begin() + (long) at, aafSeed.begin() + (long) at + 8);
        Bytes before = nb;
        switch (rnd (4))
        {
            case 0: nb[rndz (8)] ^= (std::uint8_t) (1u << rnd (8)); break;
            case 1: { const auto v = k32[rndz (std::size (k32))]; std::memcpy (nb.data(), &v, 4); } break;
            case 2: nb[rndz (8)] = (std::uint8_t) rng(); break;
            default: { const auto v = (std::uint32_t) rnd (64); std::memcpy (nb.data(), &v, 4); } break;   // small numbers: sector and entry numbers that exist
        }
        out.setPosition ((juce::int64) at); out.write (nb.data(), 8);
        patches.push_back ({ at, before });
        put64 (record, at); record.insert (record.end(), nb.begin(), nb.end());
    }
    out.flush();
    {
        Trying t (record);
        const auto r = readAaf (f, oneIn (5) ? (double) rnd (100000) : 10.0);
        (void) r;
    }
    for (auto& p : patches) { out.setPosition ((juce::int64) p.at); out.write (p.old.data(), 8); }
    out.flush();
}

// ---- listeners' comments (marker lists and Fermata's review JSON)
std::vector<Bytes> feedbackSeeds;
void feedbackOne (const Bytes* given)
{
    Bytes d = given != nullptr ? *given : feedbackSeeds[rndz (feedbackSeeds.size())];
    if (given == nullptr) { if (d.size() > 20000 && oneIn (2)) d.resize (rndz (20000)); mutateText (d); }
    Trying t (d);
    static const double rates[] = { 25.0, 29.97, 30.0, 24.0, 0.0, -1.0, 1.0e9 };
    feedback::Parsed out;
    const auto err = feedback::parse (d.data(), d.size(), rates[rndz (std::size (rates))], "fuzz", out);
    std::vector<FeedbackNote> into;
    feedback::merge (into, out.notes);
    feedback::merge (into, out.notes);
    for (auto& p : out.parts) feedback::merge (into, p.notes);
    (void) err;
}

// ---- the control surface (Mackie Control)
void mcuOne (const Bytes* given)
{
    Bytes d;
    if (given != nullptr) d = *given;
    else
    {
        const int kind = rnd (4);
        if (kind == 0) { d = { 0x00, 0x00, 0x66, (std::uint8_t) (oneIn (2) ? 0x14 : 0x15), (std::uint8_t) rnd (0x20) }; const int n = rnd (40); for (int i = 0; i < n; ++i) d.push_back ((std::uint8_t) (rng() & 0x7F)); d.insert (d.begin(), 0xF0); d.push_back (0xF7); }
        else if (kind == 1) { const int n = 1 + rnd (3); for (int i = 0; i < n; ++i) d.push_back ((std::uint8_t) rng()); d[0] |= 0x80; }
        else if (kind == 2) { d = { (std::uint8_t) (0x80 | (rng() & 0x7F)), (std::uint8_t) (rng() & 0x7F), (std::uint8_t) (rng() & 0x7F) }; }
        else { const int n = 1 + rnd (24); d.push_back (0xF0); for (int i = 0; i < n; ++i) d.push_back ((std::uint8_t) rng()); if (oneIn (2)) d.push_back (0xF7); }
    }
    if (d.empty()) return;
    Trying t (d);
    juce::MidiMessage m;
    if (d[0] == 0xF0 && d.size() >= 2) m = juce::MidiMessage::createSysExMessage (d.data() + 1, (int) d.size() - (d.back() == 0xF7 ? 2 : 1));
    else
    {
        int used = 0;
        m = juce::MidiMessage (d.data(), (int) d.size(), used, 0, 0.0, false);
    }
    const auto e = mcu::decode (m);
    (void) e; (void) mcu::describe (m); (void) mcu::hexOf (m);
}

// ---- the Stream Deck's command lines
std::vector<Bytes> remoteSeeds;
void remoteOne (const Bytes* given)
{
    Bytes d = given != nullptr ? *given : remoteSeeds[rndz (remoteSeeds.size())];
    if (given == nullptr) mutateText (d);
    Trying t (d);
    const auto c = remote::parseCommand (textOf (d));
    (void) remote::isWindowAct (c.what); (void) remote::isOpenWin (c.what);
    // (3.1.5) whatever the line said, the numbers that reach the engine are finite and in range: a "nan" passed every clamp before, and went into a fader
    if (! std::isfinite (c.delta) || c.delta < -100.0 || c.delta > 100.0) finding ("a delta out of range came through parseCommand: " + juce::String (c.delta));
    if (c.index < 0 || c.input < -1) finding ("an index or input below its range came through parseCommand");
}

// ---- saved projects (with their edits and take windows), and the settings files
std::vector<juce::var> projectSeeds;
juce::var masteringSeed, preampSeed, layoutSeed;
void jsonOne (const Bytes* given)
{
    juce::var v;
    Bytes text;
    if (given != nullptr) { text = *given; if (juce::JSON::parse (textOf (text), v).failed()) return; }
    else
    {
        const int which = rnd (10);
        v = (which < 7 ? projectSeeds[rndz (projectSeeds.size())] : which == 7 ? masteringSeed : which == 8 ? preampSeed : layoutSeed).clone();
        const int k = 1 + rnd (oneIn (4) ? 16 : 3);
        for (int i = 0; i < k; ++i) mutateVar (v);
        text = bytesOf (juce::JSON::toString (v, true));
    }
    Trying t (text);
    if (v["format"].toString() == "Fermata preamp memory")
    {
        PreampMemory pm;
        if (PreampMemory::fromVar (v, pm))
        {
            for (auto& kv : pm.inputs)                 // (3.1.5) every gain read from the file is finite and within the preamps' range, whatever the file said
                if (! std::isfinite (kv.second.gainDb) || kv.second.gainDb < 0.0f || kv.second.gainDb > 66.0f || kv.first < 0) finding ("a preamp memory gain or input out of range came through fromVar");
            (void) PreampMemory::fromVar (pm.toVar(), pm);
        }
        return;
    }
    if (v.hasProperty ("shape") && v.hasProperty ("cells")) { const auto l = LayoutDef::fromVar (v); (void) LayoutDef::fromVar (l.toVar()); return; }
    if (v.hasProperty ("tail") && v.hasProperty ("view") && ! v.hasProperty ("format")) { MasteringDef md; md.fromVar (v); MasteringDef again; again.fromVar (md.toVar()); return; }
    Project p;
    p.projectFile = scratch ("proj").getChildFile ("Fuzz.fermata");
    if (! p.fromVar (v)) return;
    // what was read must save into a file that reads back
    const auto saved = juce::JSON::toString (p.toVar (true), true);
    juce::var back;
    if (juce::JSON::parse (saved, back).failed()) { finding ("a project read from this input saves into text that cannot be read back"); return; }
    Project q;
    q.projectFile = p.projectFile;
    if (! q.fromVar (back)) finding ("a project read from this input saves into a file that is then refused");
}

// ---- what the Merging devices and ANEMAN send
juce::var moduleSeed, anemanDevices, anemanConnections;
void devicesOne (const Bytes* given)
{
    juce::var v;
    Bytes text;
    const int kind = given != nullptr ? (given->empty() ? 0 : given->front() % 3) : rnd (3);
    if (given != nullptr) { text = *given; if (juce::JSON::parse (textOf (Bytes (text.begin() + 1, text.end())), v).failed()) return; }
    else
    {
        v = (kind == 0 ? moduleSeed : kind == 1 ? anemanDevices : anemanConnections).clone();
        const int k = 1 + rnd (oneIn (4) ? 12 : 3);
        for (int i = 0; i < k; ++i) mutateVar (v);
        text = bytesOf (juce::JSON::toString (v, true)); text.insert (text.begin(), (std::uint8_t) kind);
    }
    Trying t (text);
    if (kind == 0)
    {
        (void) RavennaDevice::isMT48 (v);
        const auto* chans = v["custom"]["ins"]["channels"].getArray();
        const int n = chans != nullptr ? chans->size() : 0;
        for (int i = 0; i < n; ++i) (void) RavennaDevice::readChannel (v, i, oneIn (2));
        static const char* const paths[] = { ".custom.ins.channels[1]", ".custom.ins.channels", ".custom.ins.channels[0].micGain", ".custom", "", ".", "..", "[0]", "[",
                                             "]", ".custom.ins.channels[99999999999]", ".custom.ins.channels[-1]", ".custom.ins.channels[2147483647]", ".a.b.c[0][1]",
                                             ".custom[0]", ".custom.ins.channels[x]", ".custom.ins.channels[0]]", "[[0]]", ".custom.ins.channels[1].", ".name" };
        juce::var m2 = v.clone();
        (void) RavennaDevice::applyPath (m2, paths[rndz (std::size (paths))], randomScalar());
        (void) RavennaDevice::applyPath (m2, ".custom.ins.channels", juce::JSON::parse ("[{\"micGain\":7}]"));
        const auto* c2 = m2["custom"]["ins"]["channels"].getArray();
        for (int i = 0; c2 != nullptr && i < c2->size(); ++i) (void) RavennaDevice::readChannel (m2, i, false);
    }
    else
    {
        const auto plan = kind == 1 ? planFromAneman (v, anemanConnections) : planFromAneman (anemanDevices, v);
        (void) plan;
    }
}

// ---- what people type as a time, a clock time, an automation lane's name
void textOne (const Bytes* given)
{
    static const char* const seeds[] = { "01:02:03:04", "1:02.345", "12.5", "-0:00:01", "123456", "00:00:00;29", "10:00:00:00", "3600", "1h", "0", "12:30",
                                         "23:59", "7", "plugin:2:15", "plugin:0:0", "1:23:45.678", "99:99", "5 min", "" };
    Bytes d = given != nullptr ? *given : bytesOf (seeds[rndz (std::size (seeds))]);
    if (given == nullptr) mutateText (d);
    Trying t (d);
    const auto s = textOf (d);
    for (int mode = 0; mode < 4; ++mode)
        for (int code : { 0, 1, 2, 3, 4, 5, 99, -1 })
        {
            timefmt::Format f; f.mode = (timefmt::Mode) mode; f.ltcCode = code;
            f.sampleRate = (code == 99 ? 0.0 : code == -1 ? 1.0e12 : 48000.0); f.offsetSeconds = code == 5 ? 3600.0 : code == -1 ? -1.0e15 : 0.0;
            if (const auto r = timefmt::parse (f, s)) (void) timefmt::format (f, *r);
        }
    int h = 0, m = 0; (void) sessionclock::parseHHMM (s, h, m, false); (void) sessionclock::parseHHMM (s, h, m, true);
    int slot = 0, index = 0; (void) autoparam::parsePlugin (s, slot, index);
    double sec = 0.0; for (double fr : { 25.0, 29.97, 0.0, -5.0 }) (void) feedback::parseTime (s, fr, sec);
}

// ---- the names of files being imported (piece, take and track are read from them): odd names, many takes, pairs that are not pairs
void importOne (const Bytes* given)
{
    static const char* const names[] = { "LV01 - Gravity 003_Omni-L", "LV01 - Gravity 003_Omni-R", "LV01 - Gravity 002_Sop1-C", "LV02 - I Wake 002_PnL", "LV02 - I Wake 002_PnR",
                                         "Take 4_1", "Take 4_2", "Piano4099L", "Piano4099R", "L", "R", "Mic 1", "Mic 2", "BWF_0012_T03.A1", "Audio 1-01", "Kyrie (3) Omni-L",
                                         "a", "", "003", "_", "--", "LV01 - Gravity 999999999999_X", "\xc3\xa9t\xc3\xa9 1_L" };
    Bytes d;
    if (given != nullptr) d = *given;
    else
    {   // one name per line (made of the names above, changed), then a line of numbers for the channels and lengths
        const int n = 1 + rnd (oneIn (4) ? 60 : 8);
        for (int i = 0; i < n; ++i)
        {
            Bytes nm = bytesOf (juce::String::fromUTF8 (names[rndz (std::size (names))]));
            if (oneIn (2)) mutateText (nm);
            nm.erase (std::remove (nm.begin(), nm.end(), (std::uint8_t) '\n'), nm.end());
            d.insert (d.end(), nm.begin(), nm.end()); d.push_back ('\n');
        }
        d.push_back (0);
        for (int i = 0; i < n; ++i) { d.push_back ((std::uint8_t) rnd (9)); d.push_back ((std::uint8_t) rnd (256)); }
    }
    Trying t (d);
    std::vector<ImportSource> src;
    const auto zero = std::find (d.begin(), d.end(), (std::uint8_t) 0);
    const auto lines = juce::StringArray::fromLines (textOf (Bytes (d.begin(), zero)));
    size_t k = zero == d.end() ? d.size() : (size_t) (zero - d.begin()) + 1;
    for (auto& l : lines)
    {
        if (l.isEmpty()) continue;
        ImportSource x; x.file = juce::File ("/x").getChildFile (juce::File::createLegalFileName (l) + ".wav");
        x.channels = k < d.size() ? 1 + (int) (d[k] % 64) : 1;           // (as a reader reports them: at least one; poly files of 64 exist)
        x.length = k + 1 < d.size() ? 1 + 1000 * (juce::int64) d[k + 1] : 1000; x.sampleRate = 48000.0;
        k += 2; src.push_back (x);
    }
    for (auto naming : { ImportNaming::Auto, ImportNaming::Pyramix, ImportNaming::ProTools, ImportNaming::Sadie, ImportNaming::ByLength })
    {
        const auto plan = buildImportPlan (src, naming);
        for (bool numbers : { false, true })
        {
            const auto c = stereoPairCandidates (plan, numbers);
            const auto j = joinStereoPairs (plan, numbers);
            std::set<juce::String> some; if (! c.empty()) some.insert (c.front().key);
            (void) joinStereoPairs (plan, numbers, &some);
            (void) j;
        }
    }
}

// ---- timecode audio: noise, silence, square waves, numbers that are not numbers
void ltcOne (const Bytes* given)
{
    Bytes d;
    if (given != nullptr) d = *given;
    else { d.resize (4 + 4 * (size_t) (1 + rnd (20000))); for (auto& b : d) b = (std::uint8_t) rng(); }
    if (d.size() < 8) return;
    Trying t (d);
    static const double rates[] = { 48000.0, 44100.0, 96000.0, 192000.0, 8000.0, 1.0 };
    LtcDecoder dec (rates[d[0] % std::size (rates)]);
    const int n = (int) ((d.size() - 4) / 4);
    std::vector<float> s ((size_t) n);
    const int style = d[1] % 4;
    for (int i = 0; i < n; ++i)
    {
        float v; std::memcpy (&v, &d[4 + 4 * (size_t) i], 4);
        if (style == 1) v = ((i / (1 + d[2] % 40)) & 1) ? 0.5f : -0.5f;                                  // a square wave of some bit length
        else if (style == 2) v = (((i / (1 + d[2] % 40)) ^ d[4 + ((size_t) i * 4) % (d.size() - 4)]) & 1) ? 0.8f : -0.8f;     // random bits
        else if (style == 3) v = std::isfinite (v) ? juce::jlimit (-1.0f, 1.0f, v) : v;
        s[(size_t) i] = v;
    }
    int at = 0;
    while (at < n) { const int len = juce::jmin (n - at, 1 + (int) (d[3] % 200) * 37); dec.feed (s.data() + at, len); at += len; }
    (void) dec.frames();
}

// ---- a project with everything a project file can hold since 3.1.1 (the json target mutates it): consonants, a room tone, a lead-in, ignored
//      missing files, take matching on, bars and notes, a waiting mark, a visual guide, automation, markers, listeners' comments, the project's rate
juce::var richProjectSeed (const juce::File& folder)
{
    Project p; p.projectFile = folder.getChildFile ("Rich.fermata");
    p.setInputCount (4); p.setOutputCount (2); p.tracks.clear();
    const auto a = p.addTrack ("Main", TrackFormat::Stereo, 0).id;
    const auto b = p.addTrack ("Spot", TrackFormat::Mono, 2).id;
    auto& ltc = p.addTrack ("LTC", TrackFormat::Mono, 3); ltc.isLtc = true;
    p.sampleRate = 48000.0;
    auto& w = *p.takeWindows.front();
    w.name = "Kyrie"; w.pieceBars = 40; w.takeMatching = true; w.showLtc = true;
    Marker wm; wm.id = juce::Uuid(); wm.seconds = 3.0; wm.name = "Entry"; w.markers.push_back (wm);
    for (int i = 0; i < 3; ++i)
    {
        TakeGroup g; g.id = juce::Uuid(); g.number = i + 1; g.startSeconds = 30.0 * i; g.lengthSamples = 48000 * 20; g.sampleRate = 48000.0; g.recordedAt = juce::Time (1700000000000 + i);
        g.barIn = 1 + 10 * i; g.barOut = 12 + 10 * i; g.notes = i == 1 ? "flat in bar 9" : juce::String(); g.dud = i == 2; g.verified = true; g.overruns = i;
        if (i == 0) { g.roomTone = true; g.roomToneIn = 1.0; g.roomToneOut = 4.0; }
        for (auto& t : p.tracks) { TakeFile f; f.trackId = t.id; f.trackName = t.name; f.file = folder.getChildFile (t.name + juce::String (i) + ".wav"); f.numChannels = t.channelCount(); g.files.push_back (f); }
        if (i == 1) { WaitingPiece wp; wp.name = "bar 9 click"; wp.from = 48000; wp.to = 96000; wp.files.push_back (folder.getChildFile ("exp.wav")); wp.tracks.push_back (b); g.waiting.push_back (wp); }
        w.groups.push_back (g);
    }
    w.editTake = w.groups[0].id; w.editIn = 2.0; w.editOut = 8.0;
    auto& e = p.addEdit ("Kyrie edit", w.id);
    e.sampleRate = 48000.0; e.leadIn = true; e.leadInSeconds = 1.25; e.showGuide = true; e.automationOn = true;
    for (int i = 0; i < 2; ++i)
    {
        EditRegion r; r.id = juce::Uuid(); r.windowId = w.id; r.takeId = w.groups[(size_t) i].id; r.takeName = "00" + juce::String (i + 1) + " - Kyrie"; r.sampleRate = 48000.0;
        r.srcIn = 48000; r.srcOut = 48000 * 8; r.startSample = (juce::int64) i * 48000 * 7; r.sourceLength = 48000 * 20; r.barIn = 1 + 10 * i; r.barOut = 12 + 10 * i;
        r.inStart = i == 0 ? -0.5 : -0.05; r.inEnd = 0.05; r.outStart = -0.05; r.outEnd = 0.05;
        for (auto& t : p.tracks) { RegionFile f; f.trackId = t.id; f.trackName = t.name; f.file = folder.getChildFile (t.name + juce::String (i) + ".wav"); f.numChannels = t.channelCount(); r.files.push_back (f); }
        if (i == 1) { GainChange gc; gc.at = 500; gc.ramp = 0.1; gc.db = { -1.0f, -2.0f }; r.gains.push_back (gc); }
        e.regions.push_back (r);
    }
    { EditRegion od = e.regions[0]; od.id = juce::Uuid(); od.startSample = 48000 * 3; e.overdubs.push_back (od); }
    { AutoLane l; l.id = juce::Uuid(); l.trackId = a; l.param = autoparam::fader; AutoPoint pt; pt.id = juce::Uuid(); pt.time = 1000; pt.value = -3.0f; l.pts.push_back (pt); pt.id = juce::Uuid(); pt.time = 96000; pt.value = 2.0f; pt.region = e.regions[1].id; l.pts.push_back (pt); e.lanes.push_back (l); }
    { Marker m; m.id = juce::Uuid(); m.seconds = 5.0; m.name = "Alto entry"; e.markers.push_back (m); }
    { FeedbackNote n; n.key = "n1"; n.who = "Anna"; n.bar = "12"; n.text = "Lovely"; n.seconds = 6.5; n.colour = "red"; e.feedback.push_back (n); }
    e.guide.windowId = w.id; e.guide.takeId = w.groups[2].id; e.guide.trackId = a; e.guide.takeName = "003"; e.guide.trackName = "Main"; e.guide.file = folder.getChildFile ("Main2.wav");
    e.guide.numChannels = 2; e.guide.sampleRate = 48000.0; e.guide.srcIn = 0; e.guide.srcOut = 48000 * 10; e.guide.start = -24000;
    {
        ConsonantDef c; c.id = juce::Uuid(); c.name = "t of 'et'"; c.from = "Kyrie take 2"; c.sampleRate = 48000.0; c.length = 2400; c.lead = 240; c.made = juce::Time (1700000000000);
        for (auto& t : p.tracks) { ConsonantTrack ct; ct.trackId = t.id; ct.trackName = t.name; ct.file = folder.getChildFile ("Consonants").getChildFile ("c-" + t.name + ".wav"); ct.numChannels = t.channelCount(); c.tracks.push_back (ct); }
        p.consonants.push_back (c);
    }
    p.ignoredMissing.insert (folder.getChildFile ("gone.wav").getFullPathName());
    p.windowBounds["edit:" + e.id.toString()] = "10 20 800 600";
    return p.toVar (true);
}

// ---- an AAF of Fermata's own (3.3.0) for the aaf target when no FUZZ_AAF file is given: two pieces, a crossfade, an LTC track
Bytes ownAafSeed (const juce::File& folder)
{
    const double rate = 48000.0;
    auto S = [rate] (double s) { return (juce::int64) std::llround (s * rate); };
    std::vector<std::vector<float>> music (1, std::vector<float> ((size_t) S (6.0)));
    for (size_t i = 0; i < music[0].size(); ++i) music[0][i] = 0.3f * (float) std::sin (0.02 * (double) i);
    std::vector<std::vector<float>> code (1, std::vector<float> ((size_t) S (6.0)));
    { LtcGenerator gen; gen.prepare (rate); gen.start (1, 36000.0); gen.fill (code[0].data(), (int) code[0].size()); }
    const auto musicA = folder.getChildFile ("A-music.wav"), ltcA = folder.getChildFile ("A-ltc.wav");
    if (! audioops::writeWav (musicA, music, rate) || ! audioops::writeWav (ltcA, code, rate)) return {};
    Project vp; vp.projectFile = folder.getChildFile ("Aaf.fermata");
    vp.setInputCount (2); vp.setOutputCount (2); vp.tracks.clear();
    const auto tm = vp.addTrack ("music", TrackFormat::Mono, 0).id;
    auto& ltcTrack = vp.addTrack ("LTC", TrackFormat::Mono, 1); ltcTrack.isLtc = true; const auto tl = ltcTrack.id;
    auto& ed = vp.addEdit ("Concert", juce::Uuid::null());
    ed.sampleRate = rate;
    auto piece = [&] (double srcInSec, double srcOutSec, double startSec, double inStart, double inEnd, double outStart, double outEnd)
    {
        EditRegion r; r.id = juce::Uuid(); r.takeName = "Take A"; r.sampleRate = rate; r.sourceLength = S (6.0);
        r.srcIn = S (srcInSec); r.srcOut = S (srcOutSec); r.startSample = S (startSec); r.inStart = inStart; r.inEnd = inEnd; r.outStart = outStart; r.outEnd = outEnd;
        RegionFile a; a.trackId = tm; a.trackName = "music"; a.file = musicA; r.files.push_back (a);
        RegionFile b; b.trackId = tl; b.trackName = "LTC"; b.file = ltcA; r.files.push_back (b);
        ed.regions.push_back (r);
    };
    piece (0.5, 3.0, 0.0, 0.0, 0.02, -0.05, 0.05);
    piece (3.5, 5.5, 2.5, -0.05, 0.05, -0.3, 0.0);
    aafexport::Settings st; st.file = folder.getChildFile ("seed.aaf");
    auto plan = aafexport::planFor (vp, ed, st);
    if (plan.error.isNotEmpty()) return {};
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::atomic<float> prog { 0.0f }; std::atomic<bool> cancel { false };
    const auto res = aafexport::exportAaf (plan, st, fm, &prog, &cancel);
    if (res.error.isNotEmpty()) return {};
    return bytesOfFile (st.file);
}

// ---- the caches Fermata keeps in a project's Analysis and Waveforms folders (3.18.3): a damaged one must be refused (and made again), never crash
//      or ask for memory the file cannot be holding
std::vector<Bytes> fprSeeds, thumbSeeds;
void cachesOne (const Bytes* given)
{
    Bytes d;
    bool fpr = true;
    if (given != nullptr) { d = *given; fpr = d.size() >= 4 && std::memcmp (d.data(), "FMFP", 4) == 0; }
    else
    {
        fpr = oneIn (2);
        auto& seeds = fpr ? fprSeeds : thumbSeeds;
        d = seeds[rndz (seeds.size())];
        mutate (d, 40, &seeds[rndz (seeds.size())]);
    }
    Trying t (d);
    if (fpr)
    {
        const auto f = scratch ("c.fpr"); write (f, d);
        takematch::Fingerprint fp;
        if (takematch::load (fp, f))
        {
            if (fp.bins != takematch::Fingerprint::kNotes || fp.chroma.size() != (size_t) fp.frames() * (size_t) fp.bins || fp.loudness.size() != (size_t) fp.frames() || fp.fps <= 0.0)
                finding ("a fingerprint that does not add up was accepted");
            if ((juce::int64) fp.chroma.size() * 4 > (juce::int64) d.size()) finding ("a fingerprint holds more than its file could");
        }
    }
    else
    {
        juce::MemoryInputStream in (d.data(), d.size(), false);
        juce::AudioFormatManager fm; WaveThumb::Pool pool; WaveThumb th (fm, pool);
        if (th.loadFrom (in))
        {
            const double len = th.getTotalLength();
            if (! (len >= 0.0) || th.getNumChannels() < 1 || th.getNumChannels() > 64) finding ("a thumbnail that does not add up was accepted");
            float mn = 0.0f, mx = 0.0f; th.getApproximateMinMax (0.0, juce::jmax (0.001, len), 0, mn, mx);       // (drawn: every point read must be there)
            th.getApproximateMinMax (len * 0.5, len * 0.5 + 0.0001, th.getNumChannels() - 1, mn, mx);
        }
    }
}

bool loadJsonFile (const juce::File& f, juce::var& out)
{
    if (! f.existsAsFile()) return false;
    return ! juce::JSON::parse (f.loadFileAsString(), out).failed();
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    __sanitizer_set_death_callback (onDeath);
    const juce::File repo (juce::File (__FILE__).getParentDirectory().getParentDirectory());
    workDir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("fermata-fuzz-" + juce::String (juce::Time::currentTimeMillis()));
    if (juce::File ("/dev/shm").isDirectory()) workDir = juce::File ("/dev/shm").getChildFile (workDir.getFileName());     // (the AAF copy is big: keep it off the disk)
    workDir.createDirectory(); scratch ("proj").createDirectory();
    lastInputPath = workDir.getChildFile ("last-input.bin").getFullPathName().toStdString();
    std::printf ("work folder: %s\n", workDir.getFullPathName().toRawUTF8()); std::fflush (stdout);
    const char* seedText = std::getenv ("FUZZ_SEED");
    const std::uint64_t baseSeed = seedText != nullptr ? (std::uint64_t) std::strtoull (seedText, nullptr, 10) : 20261025ull;

    // ---- seeds
    for (int ch : { 1, 2, 6 })
        for (int bits : { 16, 24, 32 })
        {
            wavSeeds.push_back (makeWav (ch, bits, bits == 32, 300, true, false, true, false, false));          // left by a recording that stopped: sizes 0
            wavSeeds.push_back (makeWav (ch, bits, bits == 32, 300, true, false, false, true, ch > 2));         // finished, with a chunk after the audio
        }
    wavSeeds.push_back (makeWav (2, 24, false, 200, false, true, false, false, false));                          // RF64
    wavSeeds.push_back (makeWav (1, 16, false, 101, false, false, false, false, false));                         // plain, odd size
    const juce::String ixml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><BWFXML><IXML_VERSION>1.5</IXML_VERSION><TRACK_LIST><TRACK_COUNT>4</TRACK_COUNT>"
                              "<TRACK><CHANNEL_INDEX>1</CHANNEL_INDEX><INTERLEAVE_INDEX>1</INTERLEAVE_INDEX><NAME>Boom</NAME></TRACK>"
                              "<TRACK><CHANNEL_INDEX>2</CHANNEL_INDEX><INTERLEAVE_INDEX>2</INTERLEAVE_INDEX><NAME>Lav \xc3\xa9</NAME></TRACK>"
                              "<TRACK><INTERLEAVE_INDEX>3</INTERLEAVE_INDEX><NAME>Omni L</NAME></TRACK><TRACK><INTERLEAVE_INDEX>4</INTERLEAVE_INDEX><NAME>Omni R</NAME></TRACK>"
                              "</TRACK_LIST></BWFXML>";
    ixmlSeeds.push_back (makeWav (4, 24, false, 50, false, false, false, false, false, ixml));
    ixmlSeeds.push_back (makeWav (8, 16, false, 20, true, false, false, true, true, ixml));
    feedbackSeeds.push_back (bytesOfFile (repo.getChildFile ("tests/data/marker_list_sample.txt")));
    feedbackSeeds.push_back (bytesOf ("{\"fermata\":1,\"edit\":\"{6d1c}\",\"piece\":\"Kyrie\",\"offset\":12.5,\"who\":\"Anna\",\"notes\":[{\"t\":1.5,\"text\":\"Lovely\",\"bar\":\"12\",\"colour\":\"red\"},"
                                      "{\"time\":\"00:01:02:03\",\"comment\":\"Too fast\",\"author\":\"Bo\",\"done\":true},{\"seconds\":\"1:02.5\",\"message\":\"Cough\",\"id\":\"x1\"}]}"));
    feedbackSeeds.push_back (bytesOf ("{\"title\":\"Concert\",\"who\":\"Cy\",\"pieces\":[{\"edit\":\"a\",\"piece\":\"One\",\"offset\":0,\"notes\":[{\"t\":3,\"text\":\"A\"}]},{\"piece\":\"Two\",\"notes\":[{\"t\":\"0:04\",\"text\":\"B\"}]}]}"));
    feedbackSeeds.push_back (bytesOf ("Name\tStart\tDuration\tComment\r\nToo loud\t00:00:12:05\t00:00:00:00\tDan\r\nGreat\t00:01:00:00\t\t\r\n"));
    for (auto* l : { "{\"cmd\":\"record\"}", "{\"cmd\":\"talk\",\"down\":true}", "{\"cmd\":\"mixer\",\"index\":2}", "{\"cmd\":\"desk\",\"on\":false}",
                     "{\"cmd\":\"preamp\",\"input\":3,\"what\":\"mic\",\"delta\":1.5}", "{\"cmd\":\"stage\",\"what\":\"mic1\",\"delta\":-3}", "{\"cmd\":\"wact\",\"what\":\"mark1\"}",
                     "{\"cmd\":\"openwin\",\"what\":\"editwin\"}", "{\"cmd\":\"hello\",\"version\":3}" })
        remoteSeeds.push_back (bytesOf (l));
    // projects: the sample projects given on the command line's FUZZ_PROJECTS (folders searched for .fermata files), read whole (with their windows' files)
    if (const char* pj = std::getenv ("FUZZ_PROJECTS"))
        for (auto& folder : juce::StringArray::fromTokens (pj, ":", ""))
            for (auto& f : juce::File (folder).findChildFiles (juce::File::findFiles, true, "*.fermata"))
            {
                Project p; juce::String err;
                if (p.load (f, err)) { projectSeeds.push_back (p.toVar (true)); if (! p.mastering.items.empty()) masteringSeed = p.mastering.toVar(); }
            }
    {   // and one made here, so there is always one - and one with everything a project file can hold (3.18.3)
        Project p; p.projectFile = scratch ("proj").getChildFile ("Seed.fermata");
        projectSeeds.push_back (p.toVar (true));
        projectSeeds.push_back (richProjectSeed (scratch ("proj")));
    }
    {   // the caches (3.18.3): fingerprints from a tone and from a short silence; thumbnails of a few made-up files
        for (int k = 0; k < 2; ++k)
        {
            std::vector<float> s ((size_t) (k == 0 ? 48000 * 4 : 4096));
            for (size_t i = 0; i < s.size(); ++i) s[i] = k == 0 ? 0.3f * (float) std::sin (0.03 * (double) i) : 0.0f;
            auto fp = takematch::analyseSamples (s.data(), (juce::int64) s.size(), 48000.0);
            fp.sourceKey = "seed";
            const auto f = scratch ("seed.fpr");
            if (takematch::save (fp, f)) fprSeeds.push_back (bytesOfFile (f));
        }
        if (fprSeeds.empty()) { Bytes b; b.insert (b.end(), { 'F', 'M', 'F', 'P' }); put32 (b, 2); fprSeeds.push_back (b); }
        for (int ch : { 1, 2 })
        {
            const auto wav = scratch ("thumb.wav");
            write (wav, makeWav (ch, 24, false, 20000, false, false, false, false, false));
            juce::AudioFormatManager fm; WaveThumb::Pool pool; WaveThumb t (fm, pool);
            fm.registerBasicFormats();
            t.load (wav);
            for (int i = 0; i < 200 && ! t.isFullyLoaded(); ++i) juce::Thread::sleep (10);
            juce::MemoryOutputStream mo;
            if (t.isFullyLoaded() && t.saveTo (mo)) thumbSeeds.push_back (Bytes ((const std::uint8_t*) mo.getData(), (const std::uint8_t*) mo.getData() + mo.getDataSize()));
        }
        if (thumbSeeds.empty()) { Bytes b; put32 (b, 1); put32 (b, 256); put32 (b, 1); thumbSeeds.push_back (b); }
    }
    if (masteringSeed.isVoid()) { MasteringDef md; masteringSeed = md.toVar(); }
    { PreampMemory pm; PreampSettings a; a.gainDb = 30.0f; a.phantom = true; pm.inputs[0] = a; pm.inputs[5] = a; preampSeed = pm.toVar(); }
    { LayoutDef l; l.name = "Two"; l.fracs = { 0.5f }; layoutSeed = l.toVar(); }
    for (auto& s : projectSeeds) collectNames (s);
    collectNames (masteringSeed); collectNames (preampSeed); collectNames (layoutSeed);
    moduleSeed = juce::JSON::parse (R"({"id":154,"name":"Hapi MkII","custom":{"ins":{"channels":[{"m48V":false,"micGain":100,"pad":false,"lowCut":true,"lowCut_freq":80,"inputMode":1,"phase":false,"cut":false},
                                     {"m48V":true,"micGain":420,"lineGain":0,"pad":true,"lowCut":false,"phase":true,"z_in":false},{"lineGain":30,"inputMode":2,"instrumentGain":12}],
                                     "capabilities":{"channel":{"lowCut_info":[40,60,80],"pad":true,"lift":true,"instrument":true,"inputMode_ro":false,"micGain":true}}}},
                                     "identity":{"product":"MT48"}})");
    loadJsonFile (repo.getChildFile ("tests/data/aneman_devices.json"), anemanDevices);
    loadJsonFile (repo.getChildFile ("tests/data/aneman_connections.json"), anemanConnections);
    collectNames (moduleSeed); collectNames (anemanDevices); collectNames (anemanConnections);

    std::vector<Target> targets {
        { "wav",      [] { return true; }, wavOne },
        { "ixml",     [] { return true; }, ixmlOne },
        { "aaf",      [&] { if (const char* a = std::getenv ("FUZZ_AAF")) aafSeed = bytesOfFile (juce::File (a));
                            if (aafSeed.size() < 4096) { aafSeed = ownAafSeed (scratch ("proj")); if (! aafSeed.empty()) std::printf ("aaf: starting from an AAF Fermata exported itself (%d bytes)\n", (int) aafSeed.size()); }
                            if (aafSeed.size() < 4096) { std::printf ("aaf: skipped (no FUZZ_AAF file to start from)\n"); return false; }
                            aafPrepareHotSpots(); return true; }, aafOne },
        { "feedback", [] { return ! feedbackSeeds.front().empty(); }, feedbackOne },
        { "mcu",      [] { return true; }, mcuOne },
        { "remote",   [] { return true; }, remoteOne },
        { "json",     [] { return true; }, jsonOne },
        { "devices",  [] { return anemanDevices.isArray() || anemanDevices.isObject(); }, devicesOne },
        { "text",     [] { return true; }, textOne },
        { "import",   [] { return true; }, importOne },
        { "ltc",      [] { return true; }, ltcOne },
        { "caches",   [] { return true; }, cachesOne },
    };

    if (argc >= 4 && juce::String (argv[1]) == "--replay")
    {
        for (auto& t : targets)
            if (juce::String (argv[2]) == t.name)
            {
                currentTarget = t.name; t.prepare();
                const auto b = bytesOfFile (juce::File::getCurrentWorkingDirectory().getChildFile (argv[3]));
                t.one (&b);
                std::printf ("replayed %s: no crash\n", t.name);
                workDir.deleteRecursively();
                return 0;
            }
        std::printf ("no such target\n"); return 2;
    }

    const double seconds = argc >= 2 ? juce::jmax (1.0, juce::String (argv[1]).getDoubleValue()) : 30.0;
    juce::StringArray only; for (int i = 2; i < argc; ++i) only.add (argv[i]);

    std::thread watchdog ([] {
        while (! allDone.load())
        {
            std::this_thread::sleep_for (std::chrono::milliseconds (500));
            const auto t0 = inputStartedMs.load();
            if (t0 > 0 && nowMs() - t0 > 30000)
            {
                std::printf ("\nHANG [%s]: one input has taken more than 30 seconds\n", currentTarget.load());
                if (auto* b = current.load()) keep (juce::String ("fuzz-hang-") + currentTarget.load() + ".bin", *b);
                std::fflush (stdout);
                std::_Exit (3);
            }
        }
    });

    int run = 0;
    for (size_t ti = 0; ti < targets.size(); ++ti)
    {
        auto& t = targets[ti];
        if (! only.isEmpty() && ! only.contains (t.name)) continue;
        currentTarget = t.name; noteTarget (t.name);
        if (! t.prepare()) continue;
        rng.seed (baseSeed + 1000003ull * ti);
        const auto end = nowMs() + (long long) (seconds * 1000.0);
        long long n = 0; const int before = findings;
        while (nowMs() < end) { t.one (nullptr); ++n; }
        std::printf ("%-9s %8lld inputs, %d finding(s)\n", t.name, n, findings - before);
        std::fflush (stdout);
        ++run;
    }
    allDone = true; watchdog.join();
    workDir.deleteRecursively();
    std::printf (findings == 0 ? "FUZZ: %d target(s), nothing found\n" : "FUZZ: %d target(s), %d FINDING(S)\n", run, findings);
    return findings == 0 ? 0 : 1;
}
