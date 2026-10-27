#include "AudioOps.h"
#include <mutex>
#include <set>
#include "Dither.h"
#include "AudioFormats.h"
#include "WavRepair.h"

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
    if (! ok || checkWrittenLength (nullptr, f, (juce::int64) ch[0].size()).isNotEmpty()) { f.deleteFile(); return false; }     // (a short file is worse than none: whoever asked is told)
    return true;
}

juce::String checkWrittenLength (juce::AudioFormatManager* fm, const juce::File& f, juce::int64 frames)
{
    auto holds = [&] (juce::int64 n) { return f.getFileName() + " holds " + juce::String (n) + " of the " + juce::String (frames) + " samples that were written to it: the drive did not take all of it (is it full?)."; };
    if (f.hasFileExtension ("wav;wave;bwf;rf64"))
    {
        const auto r = wavrepair::inspect (f, false);                 // (looks only; its 'frames' is what the bytes hold, 'wasBroken' that the header claims more)
        if (! r.ok) return f.getFileName() + " could not be read back after it was written (is the drive full, or gone?).";
        if (r.wasBroken || r.frames != frames) return holds (r.frames);
        return {};
    }
    juce::AudioFormatManager own;
    if (fm == nullptr) { registerAudioFormats (own); fm = &own; }
    auto rd = openReader (*fm, f);
    if (rd == nullptr) return f.getFileName() + " could not be read back after it was written (is the drive full, or gone?).";
    if (rd->lengthInSamples != frames) return holds (rd->lengthInSamples);
    if (! rd->usesFloatingPointData && f.hasFileExtension ("aif;aiff;aifc"))              // uncompressed: the file is at least as long as its audio
    {
        const auto payload = frames * (juce::int64) rd->numChannels * (juce::int64) (rd->bitsPerSample / 8);
        if (f.getSize() < payload) return holds (f.getSize() / juce::jmax ((juce::int64) 1, (juce::int64) rd->numChannels * (juce::int64) (rd->bitsPerSample / 8)));
    }
    return {};
}

struct CheckedAudioFile::Forwarder : public juce::OutputStream
{
    explicit Forwarder (CheckedAudioFile& o) : owner (o) {}
    void flush() override { if (owner.out != nullptr) owner.out->flush(); }
    bool setPosition (juce::int64 p) override { return owner.out != nullptr && owner.out->setPosition (p); }
    juce::int64 getPosition() override { return owner.out != nullptr ? owner.out->getPosition() : 0; }
    bool write (const void* d, size_t n) override
    {
        if (owner.out == nullptr) return false;
        const bool ok = owner.out->write (d, n);
        owner.maxPos = juce::jmax (owner.maxPos, owner.out->getPosition());
        return ok;
    }
    CheckedAudioFile& owner;
};

CheckedAudioFile::CheckedAudioFile (const juce::File& f) : file (f), out (std::make_unique<juce::FileOutputStream> (f))
{
    if (! out->failedToOpen()) { out->setPosition (0); out->truncate(); }          // (a file stream opens an existing file at its end)
}
CheckedAudioFile::~CheckedAudioFile() = default;
std::unique_ptr<juce::OutputStream> CheckedAudioFile::stream() { return std::make_unique<Forwarder> (*this); }
juce::String CheckedAudioFile::finish()
{
    if (out == nullptr) return "Could not write " + file.getFileName() + ".";
    out->flush();
    const bool failed = out->getStatus().failed(); const auto why = out->getStatus().getErrorMessage();
    out.reset();                                                                   // (closed: the file can be renamed over or deleted now)
    if (failed) return "Could not write " + file.getFileName() + ": " + why;
    if (file.getSize() != maxPos) return file.getFileName() + " is " + juce::String (file.getSize()) + " bytes on the disk but " + juce::String (maxPos) + " were written to it: the drive did not take all of it (is it full?).";
    return {};
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
    if (const auto e = checkWrittenLength (&fm, dest, b - a); e.isNotEmpty()) { error = e; dest.deleteFile(); return false; }
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
    const auto frames = rs->lengthInSamples;
    w.reset(); rs.reset(); rr.reset();
    if (const auto e = checkWrittenLength (&fm, dest, frames); e.isNotEmpty()) { error = e; dest.deleteFile(); return false; }
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

/** The background denoiser on one channel's box [a, b) with the profile given: the stretch with 6 s before and 2 s after as context, the profile limited
    to the box's pitch range (feathered a quarter of an octave inside it), the result faded in and out over 100 ms (less for a short box) at the box's ends.
    A long box is done in pieces with context (15 s, with 6 s before and 2 s after, as denoise::processChannel does; a box of up to 15 s is one piece, as
    before): in one go the analysis took about 100 bytes a sample, so a box over a 10-minute piece at 96 kHz needed some 6 GB a channel. */
static void denoiseBox (std::vector<float>& x, double sr, long a, long b, double f0, double f1, const FixSpec& spec, const denoise::Profile& profIn)
{
    if (! profIn.valid() || std::abs (profIn.sampleRate - sr) > 0.5 || b - a < 16) return;
    denoise::Profile prof = profIn;
    const int bins = (int) prof.psd.size();
    prof.weight.assign ((size_t) bins, 0.0f);
    for (int k = 0; k < bins; ++k) { const double f = (double) k * sr / prof.fftSize; if (f >= f0 && f <= f1) prof.weight[(size_t) k] = 1.0f; }
    bool any = false; for (auto w : prof.weight) any = any || w > 0.0f;
    if (! any) return;
    denoise::featherProfile (prof, 0.25);
    const long L = (long) x.size();
    a = juce::jlimit (0L, L, a); b = juce::jlimit (a, L, b);
    if (b - a < 16) return;
    const long c0 = juce::jmax (0L, a - (long) (6.0 * sr)), c1 = juce::jmin (L, b + (long) (2.0 * sr));
    const denoise::Params& p = spec.dn;
    std::vector<float> out ((size_t) (b - a), 0.0f);                              // (only the box is kept: the context is read, not changed)
    long at = c0;                                                                 // where the next sample processChannel hands over belongs
    denoise::processChannel (prof, p, {}, (juce::int64) (c1 - c0),
                             [&x, c0] (juce::int64 start, int n, float* dst) { std::copy_n (x.data() + c0 + start, n, dst); },
                             [&out, &at, a, b] (const float* src, int n)
                             {
                                 for (int i = 0; i < n; ++i, ++at) if (at >= a && at < b) out[(size_t) (at - a)] = src[i];
                             });
    const long xf = juce::jmin ((long) (0.1 * sr), (b - a) / 4);
    for (long n = a; n < b; ++n)
    {
        double w = 1.0;
        if (xf > 0) w = juce::jmin (1.0, (double) (n - a) / (double) xf, (double) (b - 1 - n) / (double) xf);
        w = 0.5 - 0.5 * std::cos (w * juce::MathConstants<double>::pi);
        const float xv = x[(size_t) n], yv = out[(size_t) (n - a)];
        x[(size_t) n] = xv + (float) w * (yv - xv);
    }
}

void addClip (std::vector<std::vector<float>>& chans, juce::int64 at, const std::vector<std::vector<float>>& clip, double gainDb, double sr)
{
    if (chans.empty() || clip.empty()) return;
    const juce::int64 n = (juce::int64) clip[0].size();
    if (n <= 0) return;
    const float g = (float) juce::Decibels::decibelsToGain (gainDb, -200.0);
    const juce::int64 fade = juce::jlimit ((juce::int64) 1, juce::jmax ((juce::int64) 1, n / 4), (juce::int64) std::llround (0.002 * sr));
    auto shape = [n, fade] (juce::int64 i)                                    // 2 ms raised-cosine fades at the clip's two ends
    {
        const juce::int64 d = juce::jmin (i, n - 1 - i);
        if (d >= fade) return 1.0f;
        return (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * (double) (d + 1) / (double) (fade + 1)));
    };
    const bool average = chans.size() == 1 && clip.size() > 1;
    for (size_t c = 0; c < chans.size(); ++c)
    {
        auto& x = chans[c];
        const juce::int64 L = (juce::int64) x.size();
        const juce::int64 i0 = juce::jmax ((juce::int64) 0, -at), i1 = juce::jmin (n, L - at);
        for (juce::int64 i = i0; i < i1; ++i)
        {
            float v;
            if (average) { v = 0.0f; for (auto& cc : clip) v += i < (juce::int64) cc.size() ? cc[(size_t) i] : 0.0f; v /= (float) clip.size(); }
            else { const auto& cc = clip[juce::jmin (c, clip.size() - 1)]; v = i < (juce::int64) cc.size() ? cc[(size_t) i] : 0.0f; }
            x[(size_t) (at + i)] += g * shape (i) * v;
        }
    }
}

void resolvePaste (FixSpec& spec, const juce::Uuid& track)
{
    spec.pasteChannels = nullptr;
    if (spec.kind != FixSpec::Kind::Paste || spec.paste == nullptr) return;
    if (const auto* t = spec.paste->forTrack (track))
        spec.pasteChannels = std::shared_ptr<const std::vector<std::vector<float>>> (spec.paste, t);     // (aliases the consonant: no copy, kept alive with it)
}

void applyFix (std::vector<std::vector<float>>& chans, double sr, long s0, long s1, const FixSpec& spec, long xfade)
{
    if (spec.kind == FixSpec::Kind::Paste)                                      // a kept consonant, added on top from s0 (s0 may be outside the audio: only the overlap)
    {
        juce::ignoreUnused (s1, xfade);
        if (spec.pasteChannels != nullptr) addClip (chans, (juce::int64) s0, *spec.pasteChannels, spec.pasteGainDb, sr);
        return;
    }
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
    for (size_t ci = 0; ci < chans.size(); ++ci)
    {
        auto& x = chans[ci];
        const long L = (long) x.size();
        const long a = juce::jlimit (0L, L, s0), b = juce::jlimit (0L, L, s1);
        if (b - a < 2) continue;
        switch (spec.kind)
        {
            case FixSpec::Kind::Denoise:
                if (spec.dnProfiles != nullptr && ! spec.dnProfiles->empty()) denoiseBox (x, sr, a, b, spec.f0, spec.f1, spec, (*spec.dnProfiles)[juce::jmin (ci, spec.dnProfiles->size() - 1)]);
                break;
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
            case FixSpec::Kind::Paste: break;              // (above)
        }
    }
}

static juce::int64 marginFor (const FixSpec& spec, double sr, double patchSeconds, double otherSeconds)
{
    return (juce::int64) ((spec.kind == FixSpec::Kind::Patch || spec.kind == FixSpec::Kind::Denoise ? patchSeconds : otherSeconds) * sr);
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
        if (op.spec.kind == FixSpec::Kind::Paste)                                  // a pasted consonant keeps its own start (only the part inside the file is added)
        {
            const auto a = juce::jlimit ((juce::int64) 0, len, op.s0), b = juce::jlimit ((juce::int64) 0, len, op.s1);
            if (b <= a) continue;
            lo = juce::jmin (lo, a); hi = juce::jmax (hi, b);
            ops.push_back (op);
            continue;
        }
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
    if (const auto e = checkWrittenLength (&fm, dest, len); e.isNotEmpty()) { error = e; dest.deleteFile(); return {}; }
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
        if (op.spec.kind == FixSpec::Kind::Paste)                                  // a pasted consonant keeps its own start (only the part inside the piece is added)
        {
            if (juce::jmin (outTo, op.s1) <= juce::jmax (outFrom, op.s0)) continue;
            ops.push_back (op);
            continue;
        }
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
                            const std::vector<FixOp>& ops, AudioJob* job, const std::function<std::shared_ptr<const std::vector<denoise::Profile>> (size_t, const FixSpec&)>& profilesFor,
                            const std::vector<juce::Uuid>& trackOf)
{
    TakeFixResult r;
    for (size_t i = 0; i < sources.size(); ++i)
    {
        if (job != nullptr) { if (job->cancelled()) { r.error = "Cancelled."; break; } job->setProgress ((float) i / (float) juce::jmax ((size_t) 1, sources.size())); }
        juce::String err;
        auto mine = ops;
        for (auto& o : mine) if (o.spec.kind == FixSpec::Kind::Denoise) o.spec.dnProfiles = profilesFor ? profilesFor (i, o.spec) : nullptr;
        for (auto& o : mine) if (o.spec.kind == FixSpec::Kind::Paste) resolvePaste (o.spec, i < trackOf.size() ? trackOf[i] : juce::Uuid::null());
        auto f = fixWholeFile (fm, sources[i], mine, dests[i], err);
        if (f == juce::File()) { r.error = err; break; }
        r.newFiles.push_back (f);
    }
    if (r.error.isNotEmpty()) { for (auto& f : r.newFiles) f.deleteFile(); r.newFiles.clear(); }
    return r;
}
}} // namespace td::audioops

// ======================================================================================================== the room-tone profiles of a file
namespace td { namespace audioops {

DenoiseProfiles learnDenoise (juce::AudioFormatManager& fm, const juce::File& file, juce::int64 s0, juce::int64 s1, const denoise::Params& p, juce::String& error)
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
    return out;
}

}} // namespace td::audioops
