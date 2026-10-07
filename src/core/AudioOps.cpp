#include "AudioOps.h"
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

juce::File uniqueFile (const juce::File& wanted)
{
    if (! wanted.exists()) return wanted;
    const auto base = wanted.getFileNameWithoutExtension(), ext = wanted.getFileExtension();
    for (int i = 2; i < 10000; ++i)
    {
        auto f = wanted.getParentDirectory().getChildFile (base + " (" + juce::String (i) + ")" + ext);
        if (! f.exists()) return f;
    }
    return wanted;
}

std::unique_ptr<juce::AudioFormatWriter> makeWavWriter (const juce::File& f, double sr, int numChannels, bool floatData)
{
    f.getParentDirectory().createDirectory();
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

void applyFix (std::vector<std::vector<float>>& chans, double sr, long s0, long s1, const FixSpec& spec, long xfade)
{
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
            case FixSpec::Kind::Patch:   SpectralRepair::repair (x, sr, a, b, spec.f0, spec.f1, spec.repair); break;
            case FixSpec::Kind::Declick: SpectralRepair::declick (x, a, b, spec.sensitivity); break;
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
        lo = juce::jmin (lo, op.s0); hi = juce::jmax (hi, op.s1); margin = juce::jmax (margin, marginFor (op.spec, sr, 8.0, 1.0));
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
