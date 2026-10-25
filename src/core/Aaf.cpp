#include "Aaf.h"
#include <map>
#include <unordered_map>
#include <set>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <string>

namespace td
{
namespace
{
using u8 = std::uint8_t;
constexpr std::uint32_t kNoStream = 0xFFFFFFFFu;

inline std::uint32_t rd32 (const u8* p) { return (std::uint32_t) p[0] | ((std::uint32_t) p[1] << 8) | ((std::uint32_t) p[2] << 16) | ((std::uint32_t) p[3] << 24); }
inline std::uint16_t rd16 (const u8* p) { return (std::uint16_t) (p[0] | (p[1] << 8)); }
inline juce::int64  rd64 (const u8* p) { return (juce::int64) (((std::uint64_t) rd32 (p + 4) << 32) | rd32 (p)); }

juce::String utf16 (const u8* p, size_t bytes)
{
    std::u16string s;
    for (size_t i = 0; i + 1 < bytes; i += 2) { const char16_t c = (char16_t) rd16 (p + i); if (c == 0) break; s.push_back (c); }
    return juce::String (juce::CharPointer_UTF16 (reinterpret_cast<const juce::CharPointer_UTF16::CharType*> (s.c_str())));
}

// ============================================================================ the compound file (Microsoft structured storage) an AAF is made of
struct Cfb
{
    juce::MemoryBlock block;
    const u8* d = nullptr; size_t size = 0;
    std::uint32_t ss = 512, mss = 64, cutoff = 4096;
    std::vector<std::uint32_t> fat, minifat;
    std::vector<u8> mini;
    struct Ent { juce::String name; u8 type = 0; std::uint32_t left = kNoStream, right = kNoStream, child = kNoStream, start = 0; std::uint64_t size = 0; u8 clsid[16] = {}; };
    std::vector<Ent> ents;

    juce::String open (const juce::File& f)
    {
        juce::FileInputStream in (f);
        if (! in.openedOk()) return "The file could not be opened.";
        in.readIntoMemoryBlock (block);
        d = (const u8*) block.getData(); size = block.getSize();
        static const u8 magic[8] = { 0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1 };
        if (size < 512 || std::memcmp (d, magic, 8) != 0) return "This is not an AAF file (it is not a structured storage file).";
        const int ssShift = rd16 (d + 30), mssShift = rd16 (d + 32);
        if (ssShift < 7 || ssShift > 16 || mssShift < 4 || mssShift > 12) return "The AAF file is damaged (header).";
        ss = 1u << ssShift; mss = 1u << mssShift;
        const bool v3 = rd16 (d + 26) == 3;
        const std::uint32_t nFat = rd32 (d + 44), firstDir = rd32 (d + 48), firstMini = rd32 (d + 60), nMini = rd32 (d + 64), firstDif = rd32 (d + 68), nDif = rd32 (d + 72);
        cutoff = rd32 (d + 56);
        std::vector<std::uint32_t> difat;
        for (int i = 0; i < 109; ++i) difat.push_back (rd32 (d + 76 + 4 * i));
        std::uint32_t s = firstDif;
        // (a damaged index can say it has billions of parts, or point back at itself: never more parts than the file has sectors - each adds 127
        // entries, and without this a looping chain filled the memory until the program died. 3.0.0)
        const std::uint32_t sectors = (std::uint32_t) juce::jmin<size_t> (size / ss, 0xFFFFFFF0u);
        for (std::uint32_t k = 0; k < nDif && k < sectors && s < 0xFFFFFFFAu; ++k)
        {
            const size_t off = ((size_t) s + 1) * ss;
            if (off + ss > size) return "The AAF file is damaged (index).";
            for (std::uint32_t j = 0; j + 1 < ss / 4; ++j) difat.push_back (rd32 (d + off + 4 * j));
            s = rd32 (d + off + ss - 4);
        }
        for (std::uint32_t i = 0; i < nFat && i < difat.size() && fat.size() <= (size_t) sectors; ++i)     // (the file's sectors are all the sector index needs to cover)
        {
            const size_t off = ((size_t) difat[i] + 1) * ss;
            if (off + ss > size) continue;
            for (std::uint32_t j = 0; j < ss / 4; ++j) fat.push_back (rd32 (d + off + 4 * j));
        }
        std::vector<u8> dirRaw; chain (firstDir, dirRaw);
        for (size_t i = 0; i + 128 <= dirRaw.size(); i += 128)
        {
            const u8* e = dirRaw.data() + i; Ent en;
            const int nl = rd16 (e + 64);
            en.name = utf16 (e, (size_t) juce::jmin (64, juce::jmax (0, nl)));
            en.type = e[66]; en.left = rd32 (e + 68); en.right = rd32 (e + 72); en.child = rd32 (e + 76); std::memcpy (en.clsid, e + 80, 16);
            en.start = rd32 (e + 116); en.size = v3 ? (std::uint64_t) rd32 (e + 120) : (std::uint64_t) rd64 (e + 120);
            ents.push_back (std::move (en));
        }
        if (ents.empty()) return "The AAF file is damaged (directory).";
        if (ents[0].size > 0) { chain (ents[0].start, mini); mini.resize ((size_t) juce::jmin<std::uint64_t> (ents[0].size, mini.size())); }
        if (nMini > 0)
        {
            std::vector<u8> mf; chain (firstMini, mf);
            for (size_t i = 0; i + 4 <= mf.size(); i += 4) minifat.push_back (rd32 (mf.data() + i));
        }
        return {};
    }

    void chain (std::uint32_t s, std::vector<u8>& out) const
    {
        size_t guard = 0;
        while (s < 0xFFFFFFFAu && s < fat.size() && guard++ < fat.size() + 4)
        {
            const size_t off = ((size_t) s + 1) * ss;
            if (off + ss > size) break;
            out.insert (out.end(), d + off, d + off + ss);
            s = fat[s];
        }
    }

    std::vector<u8> stream (std::uint32_t idx) const
    {
        std::vector<u8> out;
        if (idx >= ents.size()) return out;
        const auto& e = ents[idx];
        if (e.size < cutoff)
        {
            std::uint32_t s = e.start; size_t guard = 0;
            while (s < 0xFFFFFFFAu && s < minifat.size() && guard++ < minifat.size() + 4)
            {
                const size_t off = (size_t) s * mss;
                if (off + mss > mini.size()) break;
                out.insert (out.end(), mini.begin() + (long) off, mini.begin() + (long) (off + mss));
                s = minifat[s];
            }
        }
        else chain (e.start, out);
        if (out.size() > e.size) out.resize ((size_t) e.size);
        return out;
    }

    /** The entries inside a storage (in the order of the tree). */
    void children (std::uint32_t idx, std::vector<std::uint32_t>& out) const
    {
        out.clear();
        if (idx >= ents.size()) return;
        std::vector<std::uint32_t> stack; std::uint32_t cur = ents[idx].child; size_t guard = 0;
        while ((cur != kNoStream && cur < ents.size()) || ! stack.empty())
        {
            if (guard++ > ents.size() * 2 + 10) break;
            if (cur != kNoStream && cur < ents.size()) { stack.push_back (cur); cur = ents[cur].left; }
            else { cur = stack.back(); stack.pop_back(); out.push_back (cur); cur = ents[cur].right; }
        }
    }
};

// ============================================================================ the objects of an AAF
struct Obj
{
    std::uint32_t idx = 0;
    std::uint16_t classTag = 0;                    // the third part of the class id: tells a Sequence from a SourceClip, a master mob from a source mob ...
    std::map<std::uint16_t, std::vector<u8>> p;    // property id -> data
    bool kidsRead = false;
    std::map<juce::String, std::uint32_t> kids;
};

// the class tags (the same in every AAF; they are the 3rd part of the class's id 0d010101-0101-XXXX-...)
constexpr std::uint16_t kCompositionMob = 0x3500, kMasterMob = 0x3600, kSourceMob = 0x3700;
constexpr std::uint16_t kSequence = 0x0f00, kSourceClip = 0x1100, kFiller = 0x0900, kTransition = 0x1700;
// the property ids
constexpr std::uint16_t pContent = 0x3b03, pMobs = 0x1901, pMobId = 0x4401, pMobName = 0x4402, pMobSlots = 0x4403, pDescriptor = 0x4701;
constexpr std::uint16_t pSlotId = 0x4801, pSlotName = 0x4802, pSegment = 0x4803, pEditRate = 0x4b01;
constexpr std::uint16_t pLength = 0x0202, pComponents = 0x1001;
constexpr std::uint16_t pSourceId = 0x1101, pStart = 0x1201, pFadeInLen = 0x1202, pFadeOutLen = 0x1204;
constexpr std::uint16_t pCutPoint = 0x1802, pLocators = 0x2f01, pUrl = 0x4001;

struct Aaf
{
    Cfb c;
    std::unordered_map<std::uint32_t, Obj> cache;
    std::uint32_t header = 0;

    Obj& obj (std::uint32_t idx)
    {
        auto it = cache.find (idx);
        if (it != cache.end()) return it->second;
        Obj o; o.idx = idx;
        o.classTag = (std::uint16_t) rd16 (c.ents[idx].clsid + 6);
        std::vector<std::uint32_t> kids; c.children (idx, kids);
        for (auto k : kids)
            if (c.ents[k].name == "properties")
            {
                const auto data = c.stream (k);
                if (data.size() >= 4 && data[0] == 0x4c)
                {
                    const int n = rd16 (data.data() + 2);
                    size_t off = 4 + (size_t) n * 6;
                    for (int i = 0; i < n; ++i)
                    {
                        const size_t h = 4 + (size_t) i * 6;
                        if (h + 6 > data.size()) break;
                        const auto pid = rd16 (data.data() + h); const size_t ln = rd16 (data.data() + h + 4);
                        if (off + ln > data.size()) break;
                        o.p[pid] = std::vector<u8> (data.begin() + (long) off, data.begin() + (long) (off + ln));
                        off += ln;
                    }
                }
            }
        return cache.emplace (idx, std::move (o)).first->second;
    }
    void readKids (Obj& o)
    {
        if (o.kidsRead) return;
        o.kidsRead = true;
        std::vector<std::uint32_t> kids; c.children (o.idx, kids);
        for (auto k : kids) o.kids[c.ents[k].name] = k;
    }
    bool has (const Obj& o, std::uint16_t pid) const { return o.p.count (pid) != 0; }
    juce::String str (const Obj& o, std::uint16_t pid) const { auto it = o.p.find (pid); return it == o.p.end() ? juce::String() : utf16 (it->second.data(), it->second.size()); }
    juce::int64 i64 (const Obj& o, std::uint16_t pid, juce::int64 def = 0) const
    {
        auto it = o.p.find (pid);
        if (it == o.p.end()) return def;
        if (it->second.size() >= 8) return rd64 (it->second.data());
        if (it->second.size() >= 4) return (std::int32_t) rd32 (it->second.data());
        return def;
    }
    /** A strongly referenced object (the property holds the name of its storage). */
    Obj* single (Obj& o, std::uint16_t pid)
    {
        if (! has (o, pid)) return nullptr;
        readKids (o);
        auto it = o.kids.find (str (o, pid));
        return it == o.kids.end() ? nullptr : &obj (it->second);
    }
    /** A strongly referenced vector / set (the property holds the name; the objects are "name{index}"), in order. */
    std::vector<Obj*> collection (Obj& o, std::uint16_t pid)
    {
        std::vector<Obj*> out;
        if (! has (o, pid)) return out;
        readKids (o);
        const auto name = str (o, pid); const auto pre = name + "{";
        std::vector<std::pair<std::uint32_t, std::uint32_t>> items;
        for (auto& kv : o.kids)
            if (kv.first.startsWith (pre) && kv.first.endsWith ("}"))
                items.push_back ({ (std::uint32_t) kv.first.substring (pre.length(), kv.first.length() - 1).getHexValue64(), kv.second });
        std::sort (items.begin(), items.end());
        for (auto& it : items) out.push_back (&obj (it.second));
        return out;
    }
};

std::string hexOf (const std::vector<u8>& v)
{
    static const char* h = "0123456789abcdef";
    std::string s; for (auto b : v) { s.push_back (h[b >> 4]); s.push_back (h[b & 15]); }
    return s;
}

struct SourceInfo { juce::String master, fileName, url; juce::int64 offset = 0; bool found = false; };

struct RawClip
{
    juce::int64 start = 0, length = 0, src = 0;
    std::string sid;
    juce::int64 tIn = 0, cutIn = 0, tOut = 0, cutOut = 0, fIn = 0, fOut = 0;
};

struct Region3 { AafRegion r; juce::int64 end = 0; juce::String key; };

juce::String stripTrailingNumber (juce::String s)
{
    s = s.trimEnd();
    int i = s.length();
    while (i > 0 && juce::CharacterFunctions::isDigit (s[i - 1])) --i;
    if (i < s.length() && i > 0 && s[i - 1] == ' ') return s.substring (0, i).trimEnd();
    return s;
}
} // namespace

juce::String aafFileNameOf (const juce::String& url)
{
    auto u = juce::URL::removeEscapeChars (url).replaceCharacter ('\\', '/');
    return u.fromLastOccurrenceOf ("/", false, false);
}

AafResult readAaf (const juce::File& file, double gapSeconds)
{
    AafResult res;
    Aaf a;
    res.error = a.c.open (file);
    if (res.error.isNotEmpty()) return res;

    // header -> content storage -> mobs
    std::uint32_t headerIdx = 0, rootChild = 0;
    {
        std::vector<std::uint32_t> kids; a.c.children (0, kids);
        for (auto k : kids) if (a.c.ents[k].name.startsWith ("Header")) { headerIdx = k; rootChild = k; }
        if (rootChild == 0) { res.error = "This does not look like an AAF file (no header)."; return res; }
    }
    auto& hdr = a.obj (headerIdx);
    auto* content = a.single (hdr, pContent);
    if (content == nullptr) { res.error = "The AAF has no content."; return res; }
    const auto mobs = a.collection (*content, pMobs);

    std::unordered_map<std::string, Obj*> mobById;
    Obj* composition = nullptr; std::vector<Obj*> compositions;
    for (auto* m : mobs)
    {
        if (a.has (*m, pMobId)) mobById[hexOf (m->p[pMobId])] = m;
        if (m->classTag == kCompositionMob) compositions.push_back (m);
    }
    if (compositions.empty()) { res.error = "The AAF has no timeline (no composition)."; return res; }
    // the timeline is the composition with the most audio slots
    size_t best = 0;
    for (auto* m : compositions) { const auto n = a.collection (*m, pMobSlots).size(); if (n > best) { best = n; composition = m; } }
    if (composition == nullptr) composition = compositions.front();
    if (compositions.size() > 1) res.notes.add ("The AAF holds " + juce::String ((int) compositions.size()) + " timelines; the biggest (\"" + a.str (*composition, pMobName) + "\") was read.");

    // which file does a clip's source mob stand for? (clip -> master mob -> its clip -> source mob -> its descriptor's locator)
    std::unordered_map<std::string, SourceInfo> infoCache;
    auto resolve = [&] (const std::string& sid, const std::vector<u8>& sidBytes) -> const SourceInfo&
    {
        auto it = infoCache.find (sid);
        if (it != infoCache.end()) return it->second;
        SourceInfo si;
        (void) sidBytes;
        auto mit = mobById.find (sid);
        if (mit != mobById.end())
        {
            Obj* mm = mit->second;
            si.master = a.str (*mm, pMobName);
            std::vector<Obj*> clips;
            const auto ms = a.collection (*mm, pMobSlots);
            if (! ms.empty())
                if (auto* seg = a.single (*ms.front(), pSegment))
                {
                    if (seg->classTag == kSequence) clips = a.collection (*seg, pComponents);
                    else clips.push_back (seg);
                }
            for (auto* cl : clips)
            {
                if (cl->classTag != kSourceClip || ! a.has (*cl, pSourceId)) continue;
                auto sit = mobById.find (hexOf (cl->p[pSourceId]));
                if (sit == mobById.end()) continue;
                Obj* sm = sit->second;
                if (auto* desc = a.single (*sm, pDescriptor))
                {
                    for (auto* loc : a.collection (*desc, pLocators))
                        if (a.has (*loc, pUrl)) si.url = a.str (*loc, pUrl);
                    if (si.url.isEmpty() && si.master.isEmpty()) continue;
                }
                si.offset = a.i64 (*cl, pStart);
                si.fileName = si.url.isNotEmpty() ? aafFileNameOf (si.url) : juce::String();
                si.found = true; break;
            }
        }
        return infoCache.emplace (sid, std::move (si)).first->second;
    };

    // the audio tracks
    struct Track { int slotId = 0; juce::String name; std::vector<RawClip> clips; double rate = 0.0; };
    std::vector<Track> tracks;
    int skipped = 0;
    for (auto* slot : a.collection (*composition, pMobSlots))
    {
        auto* seg = a.single (*slot, pSegment);
        if (seg == nullptr) continue;
        if (seg->classTag != kSequence && seg->classTag != kSourceClip) { ++skipped; continue; }       // the timecode track, markers ...
        Track t; t.slotId = (int) a.i64 (*slot, pSlotId); t.name = a.str (*slot, pSlotName);
        if (a.has (*slot, pEditRate) && slot->p[pEditRate].size() >= 8)
        {
            const double num = (std::int32_t) rd32 (slot->p[pEditRate].data()), den = (std::int32_t) rd32 (slot->p[pEditRate].data() + 4);
            t.rate = den > 0 ? num / den : 0.0;
        }
        const auto comps = seg->classTag == kSequence ? a.collection (*seg, pComponents) : std::vector<Obj*> { seg };
        juce::int64 pos = 0; bool any = false;
        for (size_t i = 0; i < comps.size(); ++i)
        {
            auto* cobj = comps[i]; const auto len = a.i64 (*cobj, pLength);
            if (cobj->classTag == kTransition) { pos -= len; continue; }
            if (cobj->classTag == kSourceClip && a.has (*cobj, pSourceId))
            {
                RawClip rc; rc.start = pos; rc.length = len; rc.src = a.i64 (*cobj, pStart); rc.sid = hexOf (cobj->p[pSourceId]);
                rc.fIn = a.i64 (*cobj, pFadeInLen); rc.fOut = a.i64 (*cobj, pFadeOutLen);
                if (i > 0 && comps[i - 1]->classTag == kTransition) { rc.tIn = a.i64 (*comps[i - 1], pLength); rc.cutIn = a.i64 (*comps[i - 1], pCutPoint); }
                if (i + 1 < comps.size() && comps[i + 1]->classTag == kTransition) { rc.tOut = a.i64 (*comps[i + 1], pLength); rc.cutOut = a.i64 (*comps[i + 1], pCutPoint); }
                t.clips.push_back (std::move (rc)); any = true;
            }
            pos += len;
        }
        if (any) tracks.push_back (std::move (t));
    }
    if (tracks.empty()) { res.error = "The AAF's timeline has no audio clips."; return res; }
    res.sampleRate = tracks.front().rate > 1000.0 ? tracks.front().rate : 48000.0;
    for (auto& t : tracks)
        if (std::abs (t.rate - tracks.front().rate) > 0.5) { res.notes.add ("The tracks do not all have the same rate; the first track's rate (" + juce::String (tracks.front().rate, 0) + ") was used."); break; }
    if (tracks.front().rate <= 1000.0) res.notes.add ("The AAF's edit rate is " + juce::String (tracks.front().rate, 3) + " (video frames?): the positions were read as samples at 48000 Hz and may be wrong.");
    if (skipped > 0) res.notes.add (juce::String (skipped) + " non-audio track(s) in the AAF (timecode, markers ...) were left out.");

    // each track's name: the slot's name, else the track part of the clips' master names ("LV01 - Gravity 003 (1) Omni-L" -> "Omni-L")
    auto splitMaster = [] (const juce::String& m, juce::String& take, juce::String& track)
    {
        const int close = m.lastIndexOf (") ");
        const int open = close > 0 ? m.substring (0, close).lastIndexOf (" (") : -1;
        if (open > 0 && close > open && m.substring (open + 2, close).containsOnly ("0123456789") && close + 2 <= m.length())
        {
            take = m.substring (0, open).trim(); track = m.substring (close + 2).trim();
            return true;
        }
        take = m; track = {};
        return false;
    };
    std::set<juce::String> usedNames;
    for (size_t ti = 0; ti < tracks.size(); ++ti)
    {
        auto& t = tracks[ti];
        if (t.name.isEmpty())
            for (auto& rc : t.clips)
            {
                const auto& si = resolve (rc.sid, {});
                juce::String take, tr;
                if (si.found && splitMaster (si.master, take, tr) && tr.isNotEmpty()) { t.name = tr; break; }
            }
        if (t.name.isEmpty()) t.name = "Track " + juce::String ((int) ti + 1);
        if (usedNames.count (t.name) != 0) t.name << " " << juce::String ((int) ti + 1);
        usedNames.insert (t.name);
        res.trackNames.push_back (t.name);
    }

    // the clips as regions (the same piece of the same take on several tracks = one region)
    const double R = res.sampleRate;
    std::map<juce::String, Region3> regionsByKey;
    int missing = 0;
    for (size_t ti = 0; ti < tracks.size(); ++ti)
        for (auto& rc : tracks[ti].clips)
        {
            ++res.clips;
            const auto& si = resolve (rc.sid, {});
            if (! si.found || si.fileName.isEmpty()) { ++missing; continue; }
            juce::String take, tr; splitMaster (si.master, take, tr);
            const auto base = rc.src + si.offset;
            juce::int64 j0 = rc.start, srcIn = base, j1 = rc.start + rc.length;
            double inS = 0.0, inE = 0.0, outS = 0.0, outE = 0.0;
            if (rc.tIn > 0) { j0 += rc.cutIn; srcIn += rc.cutIn; inS = -(double) rc.cutIn / R; inE = (double) (rc.tIn - rc.cutIn) / R; }
            else if (rc.fIn > 0) inE = (double) rc.fIn / R;
            if (rc.tOut > 0) { j1 = rc.start + rc.length - rc.tOut + rc.cutOut; outS = -(double) rc.cutOut / R; outE = (double) (rc.tOut - rc.cutOut) / R; }
            else if (rc.fOut > 0) outS = -(double) rc.fOut / R;
            if (j1 <= j0) continue;
            const auto key = take + "|" + juce::String (j0) + "|" + juce::String (j1) + "|" + juce::String (srcIn) + "|" + juce::String (inS, 6) + "|" + juce::String (inE, 6) + "|" + juce::String (outS, 6) + "|" + juce::String (outE, 6);
            auto& reg = regionsByKey[key];
            if (reg.r.files.empty())
            {
                reg.r.takeName = take; reg.r.startSample = j0; reg.r.srcIn = srcIn; reg.r.srcOut = srcIn + (j1 - j0);
                reg.r.inStart = inS; reg.r.inEnd = inE; reg.r.outStart = outS; reg.r.outEnd = outE; reg.end = j1;
            }
            AafRegionFile rf; rf.trackName = res.trackNames[ti]; rf.trackOrder = (int) ti; rf.fileName = si.fileName; rf.url = si.url;
            reg.r.files.push_back (std::move (rf));
        }
    if (missing > 0) res.notes.add (juce::String (missing) + " clip(s) have no audio file in the AAF (embedded audio or a generated clip) and were left out.");
    if (regionsByKey.empty()) { res.error = "The AAF's clips do not name any audio files. (Export the AAF with the audio linked, not embedded.)"; return res; }

    std::vector<Region3> all;
    for (auto& kv : regionsByKey) all.push_back (kv.second);
    std::sort (all.begin(), all.end(), [] (const Region3& x, const Region3& y) { return x.r.startSample != y.r.startSample ? x.r.startSample < y.r.startSample : x.r.takeName < y.r.takeName; });

    // the pieces: a gap longer than gapSeconds with nothing on any track starts a new one
    const auto gap = (juce::int64) (gapSeconds * R);
    const auto timelineZero = all.front().r.startSample;
    std::vector<std::vector<Region3>> groups;
    juce::int64 maxEnd = 0;
    for (auto& r : all)
    {
        if (groups.empty() || r.r.startSample - maxEnd > gap) { groups.emplace_back(); maxEnd = r.end; }
        groups.back().push_back (r); maxEnd = juce::jmax (maxEnd, r.end);
    }
    std::set<juce::String> usedPiece;
    for (auto& g : groups)
    {
        AafPiece p;
        const auto origin = g.front().r.startSample;
        juce::int64 end = 0; std::map<juce::String, int> takeCount; std::set<int> tracksUsed;
        for (auto& r : g)
        {
            AafRegion ar = r.r; ar.startSample -= origin;
            end = juce::jmax (end, r.end - origin);
            takeCount[ar.takeName]++;
            for (auto& f : ar.files) tracksUsed.insert (f.trackOrder);
            p.regions.push_back (std::move (ar));
        }
        juce::String top; int n = 0;
        for (auto& kv : takeCount) if (kv.second > n) { n = kv.second; top = kv.first; }
        p.name = stripTrailingNumber (top);
        if (p.name.isEmpty()) p.name = "Piece " + juce::String ((int) res.pieces.size() + 1);
        if (usedPiece.count (p.name) != 0) { int k = 2; while (usedPiece.count (p.name + " " + juce::String (k)) != 0) ++k; p.name << " " << juce::String (k); }
        usedPiece.insert (p.name);
        p.startSeconds = (double) (origin - timelineZero) / R; p.lengthSeconds = (double) end / R;
        for (int t : tracksUsed) p.trackNames.push_back (res.trackNames[(size_t) t]);
        res.pieces.push_back (std::move (p));
    }
    return res;
}
} // namespace td
