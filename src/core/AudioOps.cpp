#include "AudioOps.h"
#include <mutex>
#include <thread>
#include <atomic>
#include <set>
#include "Dither.h"

namespace td { namespace audioops
{
std::unique_ptr<juce::AudioFormatReader> openReader (juce::AudioFormatManager& fm, const juce::File& f)
{
    return std::unique_ptr<juce::AudioFormatReader> (fm.createReaderFor (f));
}

bool readRange (juce::AudioFormatReader& r, juce::int64 start, juce::int64 n, std::vector<std::vector<float>>& out)
{
    const int nc = (int) r.numChannels;
    out.assign ((size_t) nc, std::vector<float> ((size_t) juce::jmax ((juce::int64) 0, n), 0.0f));
    if (n <= 0 || nc < 1) return n <= 0;
    const juce::int64 s = juce::jmax ((juce::int64) 0, start), e = juce::jmin (r.lengthInSamples, start + n);
    if (e <= s) return true;
    std::vector<float*> ptrs ((size_t) nc);
    constexpr int block = 1 << 16;
    for (juce::int64 pos = s; pos < e; pos += block)
    {
        const int cnt = (int) juce::jmin ((juce::int64) block, e - pos);
        for (int c = 0; c < nc; ++c) ptrs[(size_t) c] = out[(size_t) c].data() + (pos - start);
        if (! r.read (ptrs.data(), nc, pos, cnt)) return false;
    }
    return true;
}

/** A name that is not used by a file AND has not been handed out before in this run. (Several new files are named first and written later, e.g. the pieces of one
    fix on either side of an edit point: they must not all get the same name just because none of them exists yet.) */
juce::File uniqueFile (const juce::File& wanted)
{
    static std::mutex lock; static std::set<juce::String> handedOut;
    const std::lock_guard<std::mutex> g (lock);
    auto taken = [&] (const juce::File& f) { return f.exists() || handedOut.count (f.getFullPathName().toLowerCase()) > 0; };
    juce::File result = wanted;
    if (taken (wanted))
    {
        const auto base = wanted.getFileNameWithoutExtension(), ext = wanted.getFileExtension();
        for (int i = 2; i < 100000; ++i)
        {
            auto f = wanted.getParentDirectory().getChildFile (base + " (" + juce::String (i) + ")" + ext);
            if (! taken (f)) { result = f; break; }
        }
    }
    handedOut.insert (result.getFullPathName().toLowerCase());
    return result;
}

std::unique_ptr<juce::AudioFormatWriter> makeWavWriter (const juce::File& f, double sr, int numChannels, bool floatData)
{
    f.getParentDirectory().createDirectory();
    f.deleteFile();                                  // (a file stream opens an existing file at its END: a new WAV would be added to the old one)
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (f);
    if (static_cast<juce::FileOutputStream*> (stream.get())->failedToOpen()) return nullptr;
    auto opts = juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (numChannels).withBitsPerSample (floatData ? 32 : 24)
                    .withSampleFormat (floatData ? juce::AudioFormatWriterOptions::SampleFormat::floatingPoint : juce::AudioFormatWriterOptions::SampleFormat::integral);
    return wav.createWriterFor (stream, opts);
}

bool writeWav (const juce::File& f, const std::vector<std::vector<float>>& ch, double sr, bool floatData)
{
    if (ch.empty()) return false;
    auto w = makeWavWriter (f, sr, (int) ch.size(), floatData);
    if (w == nullptr) return false;
    std::vector<const float*> ptrs; for (auto& c : ch) ptrs.push_back (c.data());
    DitherOut dout (*w, ! floatData);
    const bool ok = dout.write (ptrs.data(), (int) ch.size(), (int) ch[0].size());
    w.reset();
    return ok;
}

bool exportRange (juce::AudioFormatManager& fm, const juce::File& src, juce::int64 fileStart, juce::int64 a, juce::int64 b, const juce::File& dest, juce::String& error)
{
    auto rd = openReader (fm, src);
    if (rd == nullptr) { error = "Could not read " + src.getFileName(); return false; }
    auto w = makeWavWriter (dest, rd->sampleRate, (int) rd->numChannels);
    if (w == nullptr) { error = "Could not create " + dest.getFullPathName(); return false; }
    constexpr juce::int64 block = 1 << 17;
    std::vector<std::vector<float>> ch;
    for (juce::int64 pos = a; pos < b; pos += block)
    {
        const auto n = juce::jmin (block, b - pos);
        if (! readRange (*rd, pos - fileStart, n, ch)) { error = "Could not read " + src.getFileName(); w.reset(); dest.deleteFile(); return false; }
        std::vector<const float*> p; for (auto& c : ch) p.push_back (c.data());
        if (! w->writeFromFloatArrays (p.data(), (int) p.size(), (int) n)) { error = "Could not write " + dest.getFileName() + " (is the disk full?)"; w.reset(); dest.deleteFile(); return false; }
    }
    w.reset();                                    // closed: no file stays open, so another program can open and overwrite it
    rd.reset();
    return true;
}

juce::String checkReplacement (juce::AudioFormatManager& fm, const juce::File& f, juce::int64 frames, int channels, double rate)
{
    if (! f.existsAsFile()) return "The file " + f.getFileName() + " is missing.";
    auto rd = openReader (fm, f);
    if (rd == nullptr) return "The file " + f.getFileName() + " could not be read as audio.";
    if ((int) rd->numChannels != channels) return f.getFileName() + " has " + juce::String ((int) rd->numChannels) + " channel(s) but " + juce::String (channels) + " were sent out.";
    if (std::abs (rd->sampleRate - rate) > 0.5) return f.getFileName() + " is at " + juce::String (rd->sampleRate, 0) + " Hz but it was sent out at " + juce::String (rate, 0) + " Hz.";
    if (rd->lengthInSamples != frames) return f.getFileName() + " is " + juce::String (rd->lengthInSamples) + " samples long but it must stay " + juce::String (frames) + " (the length must not change).";
    return {};
}

bool spliceFile (juce::AudioFormatManager& fm, const juce::File& src, const juce::File& repl, juce::int64 s0, juce::int64 s1, const juce::File& dest, double xfadeSec, juce::String& error)
{
    auto rs = openReader (fm, src); auto rr = openReader (fm, repl);
    if (rs == nullptr || rr == nullptr) { error = "Could not read the audio."; return false; }
    if (rs->numChannels != rr->numChannels) { error = "The corrected file has a different number of channels."; return false; }
    auto w = makeWavWriter (dest, rs->sampleRate, (int) rs->numChannels);
    if (w == nullptr) { error = "Could not create " + dest.getFullPathName(); return false; }
    const auto xf = juce::jmax ((juce::int64) 1, (juce::int64) std::llround (xfadeSec * rs->sampleRate));
    constexpr juce::int64 block = 1 << 17;
    std::vector<std::vector<float>> a, b;
    for (juce::int64 pos = 0; pos < rs->lengthInSamples; pos += block)
    {
        const auto n = juce::jmin (block, rs->lengthInSamples - pos);
        if (! readRange (*rs, pos, n, a)) { error = "Could not read the audio."; w.reset(); dest.deleteFile(); return false; }
        if (pos + n > s0 && pos < s1)
        {
            readRange (*rr, pos - s0, n, b);
            for (juce::int64 i = 0; i < n; ++i)
            {
                const auto t = pos + i;
                if (t < s0 || t >= s1) continue;
                const auto d = juce::jmin (t - s0, s1 - 1 - t);
                const float wt = d >= xf ? 1.0f : (float) (d + 1) / (float) (xf + 1);
                for (size_t c = 0; c < a.size(); ++c) a[c][(size_t) i] = a[c][(size_t) i] * (1.0f - wt) + b[c][(size_t) i] * wt;
            }
        }
        std::vector<const float*> p; for (auto& c : a) p.push_back (c.data());
        if (! w->writeFromFloatArrays (p.data(), (int) p.size(), (int) n)) { error = "Could not write " + dest.getFileName() + " (is the disk full?)"; w.reset(); dest.deleteFile(); return false; }
    }
    return true;
}

/** A box small enough to be round 'the click itself' (250 ms or less) that reaches down into the bass: lost samples inside it are put back before the box is treated (turning the
    box down, or rebuilding it, cannot do that: the music on the two sides of the jump no longer lines up in time, which is the low half of the click). Not when the strength
    is 0: nothing is to be changed. */
bool boxRoundClick (const FixSpec& spec, double sr, long a, long b)
{
    return spec.kind == FixSpec::Kind::Patch && spec.repair.mode != RepairMode::Gain && spec.repair.strength > 0.0 && spec.f0 <= 1000.0 && b - a <= (long) (0.25 * sr);
}

bool findsLost (const FixSpec& spec, double sr, juce::int64 s0, juce::int64 s1)
{
    return spec.kind == FixSpec::Kind::Declick || boxRoundClick (spec, sr, 0, (long) (s1 - s0));
}

juce::int64 lostReach (double sr) { return (juce::int64) std::ceil (0.5 * sr) + 64; }

std::vector<FixSpec::Lost> findLost (const std::vector<std::vector<float>>& tracks, double sr, long s0, long s1, double sensitivity)
{
    std::vector<const std::vector<float>*> tp; for (auto& t : tracks) tp.push_back (&t);
    std::vector<FixSpec::Lost> out;
    for (auto& f : LostSamples::find (tp, s0, s1, sensitivity, sr)) out.push_back ({ (juce::int64) f.at - s0, f.lost });
    return out;
}

void applyFix (std::vector<std::vector<float>>& chans, double sr, long s0, long s1, const FixSpec& spec, long xfade)
{
    int lostDone = -1;
    if (! chans.empty() && findsLost (spec, sr, s0, s1))
    {
        // lost samples first: one event on every channel (and every track), put back the same way everywhere
        const long L = (long) chans[0].size(); const long a = juce::jlimit (0L, L, s0), b = juce::jlimit (0L, L, s1);
        std::vector<LostSamples::Splice> sp;
        if (spec.lostKnown) { for (auto& l : spec.lost) { const auto at = (juce::int64) s0 + l.at; if (at > 8 && at < (juce::int64) L - 8) sp.push_back ({ (long) at, l.count }); } }
        else if (b - a >= 4) { std::vector<const std::vector<float>*> tp; for (auto& c : chans) tp.push_back (&c); sp = LostSamples::find (tp, a, b, spec.sensitivity, sr); }
        const auto reach = lostReach (sr);
        const long lim0 = (long) juce::jlimit ((juce::int64) 0, (juce::int64) L, juce::jmax ((juce::int64) s0 + spec.lostLo, (juce::int64) s0 - reach)),
                   lim1 = (long) juce::jlimit ((juce::int64) 0, (juce::int64) L, juce::jmin ((juce::int64) s0 + spec.lostHi, (juce::int64) s1 + reach));
        lostDone = 0;
        for (auto& x : chans) lostDone = juce::jmax (lostDone, LostSamples::mendAll (x, sp, sr, lim0, lim1));
        // a box round a click that turned out to be lost samples: putting them back WAS the repair (the box is clean now; rebuilding it again would only blur the sound)
        if (spec.kind == FixSpec::Kind::Patch && ! sp.empty()) return;
    }
    if (spec.kind == FixSpec::Kind::Patch && spec.repair.mode == RepairMode::Attenuate && spec.repair.linkChannels && chans.size() > 1)
    {
        std::vector<std::vector<float>*> xs; for (auto& c : chans) xs.push_back (&c);
        const long L = (long) chans[0].size(); const long a = juce::jlimit (0L, L, s0), b = juce::jlimit (0L, L, s1);
        if (b - a >= 2) SpectralRepair::attenuate (xs, sr, a, b, spec.f0, spec.f1, spec.repair);
        return;
    }
    for (auto& x : chans)
    {
        const long L = (long) x.size();
        const long a = juce::jlimit (0L, L, s0), b = juce::jlimit (0L, L, s1);
        if (b - a < 2) continue;
        switch (spec.kind)
        {
            case FixSpec::Kind::Pitch:
            {
                const long ctx = (long) (0.4 * sr);                                    // sound around the part, so the shift is clean right to its edges
                const long c0 = juce::jmax (0L, a - ctx), c1 = juce::jmin (L, b + ctx);
                std::vector<float> seg (x.begin() + c0, x.begin() + c1);
                std::vector<float> shifted;
                if (spec.useCurve)
                {
                    // curve time 0 sits spec.curveZero samples after the start of the fixed part (s0)
                    const double zero = (double) s0 + spec.curveZero - (double) c0;
                    const auto& cv = spec.curve;
                    shifted = PitchShifter::shiftCurve (seg, sr, [&cv, zero, sr] (double i) { return cv.centsAt ((i - zero) / sr); });
                }
                else shifted = PitchShifter::shift (seg, sr, spec.cents);
                const long xf = juce::jmin (xfade, (b - a) / 2);
                for (long n = a; n < b; ++n)
                {
                    double w = 1.0;
                    if (xf > 0) w = juce::jmin (1.0, (double) (n - a) / (double) xf, (double) (b - 1 - n) / (double) xf);
                    // a straight (linear) blend: the two sounds are almost the same, so the two gains must add up to exactly 1
                    x[(size_t) n] = (float) ((1.0 - w) * (double) x[(size_t) n] + w * (double) shifted[(size_t) (n - c0)]);
                }
                break;
            }
            case FixSpec::Kind::Patch: SpectralRepair::repair (x, sr, a, b, spec.f0, spec.f1, spec.repair); break;
            case FixSpec::Kind::Declick: SpectralRepair::declick (x, a, b, spec.sensitivity, nullptr, sr, juce::jmax (0, lostDone)); break;
            case FixSpec::Kind::Reharmonise: break;        // (made by the Re-HarmoniSer window, not here)
        }
    }
}

static juce::int64 marginFor (const FixSpec& spec, double sr, double patchSeconds, double otherSeconds)
{
    return (juce::int64) ((spec.kind == FixSpec::Kind::Patch ? patchSeconds : otherSeconds) * sr);
}

juce::File fixWholeFile (juce::AudioFormatManager& fm, const juce::File& source, juce::int64 s0, juce::int64 s1, const FixSpec& spec,
                         const juce::File& dest, juce::String& error)
{
    return fixWholeFile (fm, source, std::vector<FixOp> { FixOp { spec, s0, s1 } }, dest, error);
}

juce::File fixWholeFile (juce::AudioFormatManager& fm, const juce::File& source, const std::vector<FixOp>& opsIn, const juce::File& dest, juce::String& error)
{
    auto reader = openReader (fm, source);
    if (reader == nullptr) { error = "Cannot read " + source.getFileName(); return {}; }
    const juce::int64 len = reader->lengthInSamples;
    const double sr = reader->sampleRate;
    std::vector<FixOp> ops;
    juce::int64 lo = len, hi = 0, margin = 0;
    for (auto op : opsIn)
    {
        op.s0 = juce::jlimit ((juce::int64) 0, len, op.s0); op.s1 = juce::jlimit ((juce::int64) 0, len, op.s1);
        if (op.s1 <= op.s0) continue;
        const auto reach = findsLost (op.spec, sr, op.s0, op.s1) ? lostReach (sr) : 0;            // (the time squeeze that puts lost samples back reaches past the part)
        lo = juce::jmin (lo, juce::jmax ((juce::int64) 0, op.s0 - reach)); hi = juce::jmax (hi, juce::jmin (len, op.s1 + reach)); margin = juce::jmax (margin, marginFor (op.spec, sr, 8.0, 1.0));
        ops.push_back (op);
    }
    if (ops.empty()) { lo = hi = 0; }
    const juce::int64 r0 = juce::jmax ((juce::int64) 0, lo - margin), r1 = juce::jmin (len, hi + margin);
    std::vector<std::vector<float>> seg;
    if (! readRange (*reader, r0, r1 - r0, seg)) { error = "Cannot read " + source.getFileName(); return {}; }
    for (auto& op : ops) applyFix (seg, sr, (long) (op.s0 - r0), (long) (op.s1 - r0), op.spec, (long) (0.02 * sr));

    auto w = makeWavWriter (dest, sr, (int) reader->numChannels);
    if (w == nullptr) { error = "Cannot create " + dest.getFileName(); return {}; }
    const int nc = (int) reader->numChannels;
    DitherOut dout (*w);          // the repaired file is 24 bit: the float result is dithered, not truncated
    std::vector<std::vector<float>> blk;
    constexpr juce::int64 block = 1 << 16;
    for (juce::int64 pos = 0; pos < len; pos += block)
    {
        const auto cnt = juce::jmin (block, len - pos);
        readRange (*reader, pos, cnt, blk);
        for (juce::int64 n = juce::jmax (pos, lo); n < juce::jmin (pos + cnt, hi); ++n)
            for (int c = 0; c < nc; ++c) blk[(size_t) c][(size_t) (n - pos)] = seg[(size_t) c][(size_t) (n - r0)];
        std::vector<const float*> ptrs; for (auto& c : blk) ptrs.push_back (c.data());
        if (! dout.write (ptrs.data(), nc, (int) cnt)) { error = "Disk full or write error."; w.reset(); dest.deleteFile(); return {}; }
    }
    w.reset();
    return dest;
}

PieceResult fixPiece (juce::AudioFormatManager& fm, const juce::File& source, juce::int64 fileStart, juce::int64 outFrom, juce::int64 outTo,
                      juce::int64 fixFrom, juce::int64 fixTo, const FixSpec& spec, const juce::File& dest, juce::String& error)
{
    return fixPiece (fm, source, fileStart, outFrom, outTo, std::vector<FixOp> { FixOp { spec, fixFrom, fixTo } }, dest, error);
}

PieceResult fixPiece (juce::AudioFormatManager& fm, const juce::File& source, juce::int64 fileStart, juce::int64 outFrom, juce::int64 outTo,
                      const std::vector<FixOp>& opsIn, const juce::File& dest, juce::String& error)
{
    PieceResult res;
    auto reader = openReader (fm, source);
    if (reader == nullptr) { error = "Cannot read " + source.getFileName(); return res; }
    const double sr = reader->sampleRate;
    const juce::int64 fileEnd = fileStart + reader->lengthInSamples;
    outFrom = juce::jmax (fileStart, outFrom); outTo = juce::jmin (fileEnd, outTo);
    if (outTo - outFrom < 16) { error = "Nothing to process in " + source.getFileName(); return res; }
    std::vector<FixOp> ops; juce::int64 margin = 0;
    for (auto op : opsIn)
    {
        op.s0 = juce::jlimit (outFrom, outTo, op.s0); op.s1 = juce::jlimit (outFrom, outTo, op.s1);
        if (op.s1 <= op.s0) continue;
        margin = juce::jmax (margin, marginFor (op.spec, sr, 8.0, 0.5));
        ops.push_back (op);
    }
    const juce::int64 r0 = juce::jmax (fileStart, outFrom - margin), r1 = juce::jmin (fileEnd, outTo + margin);
    std::vector<std::vector<float>> seg;
    if (! readRange (*reader, r0 - fileStart, r1 - r0, seg)) { error = "Cannot read " + source.getFileName(); return res; }
    for (auto& op : ops) applyFix (seg, sr, (long) (op.s0 - r0), (long) (op.s1 - r0), op.spec, op.spec.kind == FixSpec::Kind::Pitch ? 0 : (long) (0.02 * sr));
    std::vector<std::vector<float>> out;
    for (auto& c : seg) out.emplace_back (c.begin() + (outFrom - r0), c.begin() + (outTo - r0));
    if (! writeWav (dest, out, sr)) { error = "Cannot write " + dest.getFileName(); return res; }
    res.file = dest; res.from = outFrom; res.to = outTo; res.ok = true;
    return res;
}

TakeFixResult fixTakeFiles (juce::AudioFormatManager& fm, const std::vector<juce::File>& sources, const std::vector<juce::File>& dests,
                            juce::int64 s0, juce::int64 s1, const FixSpec& spec, AudioJob* job)
{
    return fixTakeFiles (fm, sources, dests, std::vector<FixOp> { FixOp { spec, s0, s1 } }, job);
}

TakeFixResult fixTakeFiles (juce::AudioFormatManager& fm, const std::vector<juce::File>& sources, const std::vector<juce::File>& dests,
                            const std::vector<FixOp>& ops, AudioJob* job)
{
    TakeFixResult r;
    for (size_t i = 0; i < sources.size(); ++i)
    {
        if (job != nullptr) { if (job->cancelled()) { r.error = "Cancelled."; break; } job->setProgress ((float) i / (float) juce::jmax ((size_t) 1, sources.size())); }
        juce::String err;
        auto f = fixWholeFile (fm, sources[i], ops, dests[i], err);
        if (f == juce::File()) { r.error = err; break; }
        r.newFiles.push_back (f);
    }
    if (r.error.isNotEmpty()) { for (auto& f : r.newFiles) f.deleteFile(); r.newFiles.clear(); }
    return r;
}
}} // namespace td::audioops

// ======================================================================================================== the background denoiser on files
namespace td { namespace audioops {

/** fn (i) for i in 0..n on up to 'threads' threads; an exception in one becomes an error text (a thread that throws would end the program). */
static juce::String parallelFor (int n, int threads, const std::function<juce::String (int)>& fn)
{
    std::vector<juce::String> errs ((size_t) juce::jmax (0, n));
    std::atomic<int> next { 0 };
    auto worker = [&]
    {
        for (;;)
        {
            const int i = next.fetch_add (1); if (i >= n) return;
            try { errs[(size_t) i] = fn (i); }
            catch (const std::bad_alloc&) { errs[(size_t) i] = "Not enough memory for this (try fewer tracks at once, or a shorter preview)."; }
            catch (...) { errs[(size_t) i] = "Something went wrong while processing."; }
        }
    };
    std::vector<std::thread> pool;
    const int count = juce::jlimit (1, juce::jmax (1, n), threads);
    try { for (int t = 0; t < count; ++t) pool.emplace_back (worker); }
    catch (...) {}                                           // (a thread that could not be made: the ones made do the work)
    if (pool.empty()) worker();
    for (auto& t : pool) t.join();
    for (auto& e : errs) if (e.isNotEmpty()) return e;
    return {};
}

DenoiseProfiles learnDenoise (juce::AudioFormatManager& fm, const juce::File& file, juce::int64 s0, juce::int64 s1, const denoise::Params& p, juce::String& error, double decaySeconds)
{
    DenoiseProfiles out;
    auto r = openReader (fm, file);
    if (r == nullptr) { error = "Could not read " + file.getFileName(); return out; }
    s0 = juce::jmax<juce::int64> (0, s0); s1 = juce::jmin (r->lengthInSamples, s1);
    if (s1 - s0 < (juce::int64) (0.5 * r->sampleRate)) { error = "The marked room tone is too short (mark at least half a second, better two or more)."; return out; }
    s1 = juce::jmin (s1, s0 + (juce::int64) (60.0 * r->sampleRate));            // (a minute says everything; the marks may well span a movement)
    std::vector<std::vector<float>> ch;
    if (! readRange (*r, s0, s1 - s0, ch)) { error = "Could not read " + file.getFileName(); return out; }
    out.sampleRate = r->sampleRate; out.channels = (int) r->numChannels;
    for (auto& c : ch) out.perChannel.push_back (denoise::learnProfile ({ c }, r->sampleRate, p));
    for (auto& pr : out.perChannel) if (! pr.valid()) { error = "The marked room tone is too short for the analysis window."; out = {}; return out; }
    if (decaySeconds > 0.0)
    {
        // the room's decay: over the stretch after the room tone (or before it, at the end of a take), up to decaySeconds
        const juce::int64 len = r->lengthInSamples, want = (juce::int64) (decaySeconds * r->sampleRate);
        juce::int64 d0 = s1, d1 = juce::jmin (len, s1 + want);
        if (d1 - d0 < (juce::int64) (5.0 * r->sampleRate)) { d1 = s0; d0 = juce::jmax<juce::int64> (0, s0 - want); }
        if (d1 - d0 >= (juce::int64) (5.0 * r->sampleRate))
        {
            bool bad = false;
            for (int c = 0; c < out.channels; ++c)
                out.decay.push_back (denoise::measureDecay (out.perChannel[(size_t) c], p, d1 - d0, [&] (juce::int64 start, int n, float* dst)
                {
                    std::vector<std::vector<float>> tmp; if (! readRange (*r, d0 + start, n, tmp)) bad = true;
                    std::copy (tmp[(size_t) c].begin(), tmp[(size_t) c].end(), dst);
                }));
            if (bad) out.decay.clear();
        }
    }
    return out;
}

bool denoiseFiles (juce::AudioFormatManager& fm, const std::vector<juce::File>& sources, const std::vector<juce::File>& dests, const std::vector<juce::File>& removedFiles,
                   const std::vector<DenoiseProfiles>& profiles, const denoise::Params& p, const DenoiseRun& run, const std::function<bool (float)>& report, juce::String& error)
{
    const size_t nf = sources.size();
    if (nf == 0 || dests.size() != nf || profiles.size() != nf || (run.removed && removedFiles.size() != nf)) { error = "Nothing to do."; return false; }
    struct Src { std::unique_ptr<juce::AudioFormatReader> r; int nc = 0; juce::int64 len = 0; };
    std::vector<Src> src (nf);
    double sr = 0.0;
    for (size_t f = 0; f < nf; ++f)
    {
        src[f].r = openReader (fm, sources[f]);
        if (src[f].r == nullptr) { error = "Could not read " + sources[f].getFileName(); return false; }
        src[f].nc = (int) src[f].r->numChannels; src[f].len = src[f].r->lengthInSamples;
        if (f == 0) sr = src[f].r->sampleRate; else if (std::abs (src[f].r->sampleRate - sr) > 0.5) { error = "The files have different sample rates."; return false; }
        if (! profiles[f].valid() || profiles[f].channels != src[f].nc || std::abs (profiles[f].sampleRate - sr) > 0.5) { error = "The room-tone profile does not fit " + sources[f].getFileName() + " (channels or sample rate)."; return false; }
    }
    juce::int64 longest = 0; for (auto& x : src) longest = juce::jmax (longest, x.len);
    const juce::int64 a = juce::jlimit<juce::int64> (0, longest, run.a), b = juce::jlimit<juce::int64> (a, longest, run.b);
    std::atomic<bool> stopped { false };
    auto progress = [&] (double v) { if (report && ! report ((float) v)) stopped = true; return ! stopped.load(); };
    std::vector<std::pair<size_t, int>> chans;                  // every (file, channel)
    for (size_t f = 0; f < nf; ++f) for (int c = 0; c < src[f].nc; ++c) chans.push_back ({ f, c });
    const int nch = (int) chans.size();

    // pass 1: the room's decay per channel over the stretch, unless the profiles brought it
    std::vector<std::vector<denoise::Decay>> decay (nf);
    for (size_t f = 0; f < nf; ++f) decay[f] = profiles[f].decay.size() == (size_t) src[f].nc ? profiles[f].decay : std::vector<denoise::Decay> ((size_t) src[f].nc);
    {
        std::mutex readLock;
        bool readFailed = false;
        const auto err = parallelFor (nch, run.threads, [&] (int i) -> juce::String
        {
            const size_t f = chans[(size_t) i].first; const int c = chans[(size_t) i].second;
            if (! decay[f][(size_t) c].rates.empty()) return {};
            auto rd = [&] (juce::int64 start, int n, float* dst)
            {
                std::vector<std::vector<float>> tmp;
                { std::lock_guard<std::mutex> l (readLock); if (! readRange (*src[f].r, a + start, n, tmp)) readFailed = true; }
                std::copy (tmp[(size_t) c].begin(), tmp[(size_t) c].end(), dst);
            };
            decay[f][(size_t) c] = denoise::measureDecay (profiles[f].perChannel[(size_t) c], p, b - a, rd, [&] (double) { return ! stopped.load(); });
            progress (0.1 * (double) (i + 1) / (double) nch);
            return {};
        });
        if (err.isNotEmpty()) { error = err; return false; }
        if (readFailed) { error = "Could not read the audio files."; return false; }
        if (stopped) { error = "Cancelled."; return false; }
    }

    // the writers
    struct Dst { std::unique_ptr<juce::AudioFormatWriter> w, wr; std::unique_ptr<DitherOut> dither, plain, rdither, rplain; };
    std::vector<Dst> dst (nf);
    auto fail = [&] (const juce::String& e)
    {
        for (size_t f = 0; f < nf; ++f) { dst[f].dither.reset(); dst[f].plain.reset(); dst[f].rdither.reset(); dst[f].rplain.reset(); dst[f].w.reset(); dst[f].wr.reset(); dests[f].deleteFile(); if (run.removed) removedFiles[f].deleteFile(); }
        error = e; return false;
    };
    for (size_t f = 0; f < nf; ++f)
    {
        dst[f].w = makeWavWriter (dests[f], sr, src[f].nc);
        if (dst[f].w == nullptr) return fail ("Cannot write " + dests[f].getFileName());
        dst[f].dither = std::make_unique<DitherOut> (*dst[f].w); dst[f].plain = std::make_unique<DitherOut> (*dst[f].w, false);
        if (run.removed)
        {
            dst[f].wr = makeWavWriter (removedFiles[f], sr, src[f].nc);
            if (dst[f].wr == nullptr) return fail ("Cannot write " + removedFiles[f].getFileName());
            dst[f].rdither = std::make_unique<DitherOut> (*dst[f].wr); dst[f].rplain = std::make_unique<DitherOut> (*dst[f].wr, false);
        }
    }
    // the parts outside the stretch: copied bit for bit (a preview: written as silence, quickly, and nothing after the stretch at all)
    std::vector<std::vector<float>> block; std::vector<const float*> ptrs;
    auto writeSilence = [&] (Dst& d, int nc, juce::int64 n)
    {
        constexpr int chunk = 1 << 16; std::vector<float> z ((size_t) chunk, 0.0f); std::vector<const float*> zp ((size_t) nc, z.data());
        for (juce::int64 pos = 0; pos < n; pos += chunk)
        {
            const int m = (int) juce::jmin<juce::int64> (chunk, n - pos);
            if (! d.plain->write (zp.data(), nc, m)) return false;
            if (d.rplain && ! d.rplain->write (zp.data(), nc, m)) return false;
        }
        return true;
    };
    auto copyRange = [&] (size_t f, juce::int64 s0, juce::int64 s1, double f0, double f1)
    {
        s1 = juce::jmin (s1, src[f].len);
        if (run.preview) return writeSilence (dst[f], src[f].nc, s1 - s0);
        constexpr int chunk = 1 << 16;
        for (juce::int64 pos = s0; pos < s1; pos += chunk)
        {
            const int n = (int) juce::jmin<juce::int64> (chunk, s1 - pos);
            if (! readRange (*src[f].r, pos, n, block)) return false;
            ptrs.assign ((size_t) src[f].nc, nullptr); for (int c = 0; c < src[f].nc; ++c) ptrs[(size_t) c] = block[(size_t) c].data();
            if (! dst[f].plain->write (ptrs.data(), src[f].nc, n)) return false;
            if (dst[f].rplain) { std::vector<float> z ((size_t) n, 0.0f); std::vector<const float*> zp ((size_t) src[f].nc, z.data()); if (! dst[f].rplain->write (zp.data(), src[f].nc, n)) return false; }
            if (! progress (f0 + (f1 - f0) * ((double) f + (double) (pos + n - s0) / (double) juce::jmax<juce::int64> (1, s1 - s0)) / (double) nf)) return false;
        }
        return true;
    };
    for (size_t f = 0; f < nf; ++f) if (! copyRange (f, 0, a, 0.10, 0.12)) return fail (stopped ? "Cancelled." : "Could not read or write " + dests[f].getFileName());

    // the stretch, in pieces with context on both sides; all channels of all files together, so each can use what the others hear
    const juce::int64 piece = (juce::int64) (10.0 * sr), before = (juce::int64) (6.0 * sr), after = (juce::int64) (2.0 * sr);
    const bool group = p.groupMode > 0 && nch > 1;
    std::vector<std::vector<float>> x ((size_t) nch), y ((size_t) nch);
    std::vector<std::vector<unsigned char>> maps ((size_t) nch), others;
    for (juce::int64 start = a; start < b; start += piece)
    {
        const juce::int64 end = juce::jmin (b, start + piece);
        const juce::int64 s0 = juce::jmax<juce::int64> (0, start - before), s1 = juce::jmin (longest, end + after);
        const int segLen = (int) (s1 - s0), outLen = (int) (end - start), off = (int) (start - s0);
        for (size_t f = 0; f < nf; ++f)                       // read every file's segment once (all its channels)
        {
            std::vector<std::vector<float>> seg;
            if (! readRange (*src[f].r, s0, segLen, seg)) return fail ("Could not read " + sources[f].getFileName());
            for (int i = 0; i < nch; ++i) if (chans[(size_t) i].first == f) { x[(size_t) i] = std::move (seg[(size_t) chans[(size_t) i].second]); y[(size_t) i].assign ((size_t) segLen, 0.0f); }
        }
        if (group)
        {   // stage 1: what each channel hears; stage 2 (below): each channel's gains, knowing what the others hear
            auto err = parallelFor (nch, run.threads, [&] (int i) -> juce::String
            {
                const size_t f = chans[(size_t) i].first; const int c = chans[(size_t) i].second;
                int bb = 0, ff = 0;
                denoise::presenceSegment (profiles[f].perChannel[(size_t) c], p, decay[f][(size_t) c], x[(size_t) i].data(), segLen, maps[(size_t) i], bb, ff);
                denoise::dilate (maps[(size_t) i], bb, ff, 2, 1);
                return stopped ? "Cancelled." : juce::String();
            });
            if (err.isNotEmpty()) return fail (err);
            std::vector<int> sourceOf ((size_t) nch); for (int i = 0; i < nch; ++i) sourceOf[(size_t) i] = (int) chans[(size_t) i].first;
            denoise::othersAgreed (maps, sourceOf, others);             // (a sound only one mic hears - a rustle at one stand - does not count)
        }
        const auto err = parallelFor (nch, run.threads, [&] (int i) -> juce::String
        {
            const size_t f = chans[(size_t) i].first; const int c = chans[(size_t) i].second;
            denoise::Analysis A;
            denoise::analyse (profiles[f].perChannel[(size_t) c], p, decay[f][(size_t) c], x[(size_t) i].data(), segLen, A);
            denoise::finish (profiles[f].perChannel[(size_t) c], p, A, group ? others[(size_t) i].data() : nullptr, y[(size_t) i].data());
            return stopped ? "Cancelled." : juce::String();
        });
        if (err.isNotEmpty()) return fail (err);
        for (size_t f = 0; f < nf; ++f)
        {
            const int nc = src[f].nc;
            std::vector<const float*> op ((size_t) nc); std::vector<std::vector<float>> rem ((size_t) nc); std::vector<const float*> rp ((size_t) nc);
            for (int i = 0; i < nch; ++i)
                if (chans[(size_t) i].first == f)
                {
                    const int c = chans[(size_t) i].second;
                    op[(size_t) c] = y[(size_t) i].data() + off;
                    if (run.removed) { rem[(size_t) c].resize ((size_t) outLen); for (int k = 0; k < outLen; ++k) rem[(size_t) c][(size_t) k] = x[(size_t) i][(size_t) (off + k)] - y[(size_t) i][(size_t) (off + k)]; rp[(size_t) c] = rem[(size_t) c].data(); }
                }
            if (! dst[f].dither->write (op.data(), nc, outLen)) return fail ("Cannot write " + dests[f].getFileName());
            if (run.removed && ! dst[f].rdither->write (rp.data(), nc, outLen)) return fail ("Cannot write " + removedFiles[f].getFileName());
        }
        if (! progress (0.12 + 0.86 * (double) (end - a) / (double) juce::jmax<juce::int64> (1, b - a))) return fail ("Cancelled.");
    }
    if (! run.preview)
        for (size_t f = 0; f < nf; ++f) if (! copyRange (f, b, src[f].len, 0.98, 1.0)) return fail (stopped ? "Cancelled." : "Could not read or write " + dests[f].getFileName());
    for (size_t f = 0; f < nf; ++f) { dst[f].dither.reset(); dst[f].plain.reset(); dst[f].rdither.reset(); dst[f].rplain.reset(); dst[f].w.reset(); dst[f].wr.reset(); }
    return true;
}

bool denoiseFile (juce::AudioFormatManager& fm, const juce::File& source, const juce::File& dest, const juce::File& removed, juce::int64 a, juce::int64 b,
                  const DenoiseProfiles& prof, const denoise::Params& p, const std::function<bool (float)>& progress, juce::String& error)
{
    DenoiseRun run; run.a = a; run.b = b; run.removed = removed != juce::File();
    denoise::Params single = p; single.groupMode = 0;
    return denoiseFiles (fm, { source }, { dest }, run.removed ? std::vector<juce::File> { removed } : std::vector<juce::File>(), { prof }, single, run, progress, error);
}
}} // namespace td::audioops
