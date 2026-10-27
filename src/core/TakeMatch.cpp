#include "TakeMatch.h"
#include "Resampler.h"
#include "Dsp.h"
#include <cmath>
#include <algorithm>
#include <numeric>
#include <memory>
#include <cstring>
#include <cstdio>

namespace td::takematch
{
namespace
{
constexpr double kRate = 8000.0;             // the audio is brought down to this: the notes that matter are below 4 kHz
constexpr int kWindow = 4096;                // 0.512 s: fine enough to tell the low notes apart
constexpr int kHop = 400;                    // 50 ms: 20 frames a second
constexpr double kSilenceDb = -55.0;         // quieter than this is "silence": its chroma is the flat vector, so silence matches silence
constexpr int kSmooth = 5;                   // frames averaged into each (0.25 s): the course of the harmony, not every passing note
constexpr float kFlat = 0.28867513f;         // 1 / sqrt (12)
constexpr int kOnsetLook = 10;               // frames (0.5 s) a note must be new against to count as an onset
constexpr float kOnset = 2.0f;               // how much the onsets weigh against what sustains

/** Builds chroma frames from a stream of 8 kHz samples. */
class ChromaBuilder
{
public:
    ChromaBuilder() : fft (kWindow), window (dsp::hann (kWindow))
    {
        // which note class each FFT bin belongs to (-1: outside the range used)
        binClass.assign ((size_t) kWindow / 2, -1);
        for (int k = 1; k < kWindow / 2; ++k)
        {
            const double f = (double) k * kRate / kWindow;
            if (f < 80.0 || f > 4000.0) continue;
            const double pitch = 69.0 + 12.0 * std::log2 (f / 440.0);
            const int note = (int) std::lround (pitch);
            if (std::abs (pitch - (double) note) > 0.4) continue;      // a bin between two notes says nothing clear
            binClass[(size_t) k] = ((note % 12) + 12) % 12;
        }
    }
    void push (const float* in, size_t n, Fingerprint& out)
    {
        buf.insert (buf.end(), in, in + n);
        size_t off = 0;
        while (off + (size_t) kWindow <= buf.size())
        {
            frame (buf.data() + off, out);
            off += (size_t) kHop;
        }
        buf.erase (buf.begin(), buf.begin() + (long) off);
    }
    void finish (Fingerprint& out)
    {
        // the tail: padded with silence to a last frame
        if (! buf.empty()) { buf.resize ((size_t) kWindow, 0.0f); frame (buf.data(), out); buf.clear(); }
    }
private:
    void frame (const float* x, Fingerprint& out)
    {
        double rms = 0.0;
        for (int i = 0; i < kWindow; ++i) { const double v = (double) x[i] * window[(size_t) i]; spec[(size_t) i] = { v, 0.0 }; rms += (double) x[i] * x[i]; }
        rms = std::sqrt (rms / kWindow);
        const double db = 20.0 * std::log10 (rms + 1.0e-9);
        out.loudness.push_back ((float) db);
        float c[Fingerprint::kBins] = {};
        if (db >= kSilenceDb)
        {
            fft.forward (spec);
            for (int k = 1; k < kWindow / 2; ++k)
                if (const int cls = binClass[(size_t) k]; cls >= 0) c[cls] += (float) std::norm (spec[(size_t) k]);
            float sum = 0.0f;
            for (auto v : c) sum += v;
            if (sum > 0.0f)
            {
                float norm = 0.0f;
                for (auto& v : c) { v = std::log1p (20.0f * v / sum); norm += v * v; }     // the shape of the chord, not how loud it is
                norm = std::sqrt (norm);
                for (auto& v : c) v /= norm;
            }
            else for (auto& v : c) v = kFlat;
        }
        else for (auto& v : c) v = kFlat;
        out.chroma.insert (out.chroma.end(), c, c + Fingerprint::kBins);
    }
    dsp::FFT fft;
    std::vector<double> window;
    std::vector<std::complex<double>> spec = std::vector<std::complex<double>> ((size_t) kWindow);
    std::vector<int> binClass;
    std::vector<float> buf;
};

/** What is NEW in each frame - the notes that have just begun - is added to it with weight kOnset: a note's start is sharp even in a
    long reverb, where what sustains is smeared with what came before. Then each frame becomes the average of the frames around it, scaled
    back to length 1: the harmony's course. */
void smooth (Fingerprint& fp)
{
    const int n = fp.frames(), half = kSmooth / 2;
    if (n == 0) return;
    std::vector<float> sharp ((size_t) n * Fingerprint::kBins);
    for (int i = 0; i < n; ++i)
    {
        float before[Fingerprint::kBins] = {}; int cnt = 0;
        for (int k = juce::jmax (0, i - kOnsetLook); k < i; ++k, ++cnt) for (int b = 0; b < Fingerprint::kBins; ++b) before[b] += fp.frame (k)[b];
        float norm = 0.0f;
        for (int b = 0; b < Fingerprint::kBins; ++b)
        {
            const float v = fp.frame (i)[b], rise = cnt > 0 ? juce::jmax (0.0f, v - before[b] / (float) cnt) : 0.0f;
            sharp[(size_t) i * Fingerprint::kBins + (size_t) b] = v + kOnset * rise; norm += sharp[(size_t) i * Fingerprint::kBins + (size_t) b] * sharp[(size_t) i * Fingerprint::kBins + (size_t) b];
        }
        norm = std::sqrt (norm);
        if (norm > 0.0f) for (int b = 0; b < Fingerprint::kBins; ++b) sharp[(size_t) i * Fingerprint::kBins + (size_t) b] /= norm;
    }
    std::vector<float> out ((size_t) n * Fingerprint::kBins);
    for (int i = 0; i < n; ++i)
    {
        float acc[Fingerprint::kBins] = {};
        for (int k = juce::jmax (0, i - half); k <= juce::jmin (n - 1, i + half); ++k)
            for (int b = 0; b < Fingerprint::kBins; ++b) acc[b] += sharp[(size_t) k * Fingerprint::kBins + (size_t) b];
        float norm = 0.0f;
        for (auto v : acc) norm += v * v;
        norm = std::sqrt (norm);
        for (int b = 0; b < Fingerprint::kBins; ++b) out[(size_t) i * Fingerprint::kBins + (size_t) b] = norm > 0.0f ? acc[b] / norm : kFlat;
    }
    fp.chroma = std::move (out);
}

/** The fingerprint at fewer frames a second: blocks of 'factor' frames averaged. */
Fingerprint downsample (const Fingerprint& fp, int factor)
{
    Fingerprint out; out.fps = fp.fps / factor;
    const int n = (fp.frames() + factor - 1) / factor;
    for (int i = 0; i < n; ++i)
    {
        float acc[Fingerprint::kBins] = {}; float db = -200.0f; int cnt = 0;
        for (int k = i * factor; k < juce::jmin (fp.frames(), (i + 1) * factor); ++k, ++cnt)
        {
            for (int b = 0; b < Fingerprint::kBins; ++b) acc[b] += fp.frame (k)[b];
            db = juce::jmax (db, fp.loudness[(size_t) k]);
        }
        float norm = 0.0f;
        for (auto v : acc) norm += v * v;
        norm = std::sqrt (norm);
        for (auto v : acc) out.chroma.push_back (norm > 0.0f ? v / norm : kFlat);
        out.loudness.push_back (db);
    }
    return out;
}

inline float distance (const float* x, const float* y) noexcept
{
    float dot = 0.0f;
    for (int b = 0; b < Fingerprint::kBins; ++b) dot += x[b] * y[b];
    return juce::jlimit (0.0f, 1.0f, 1.0f - dot);
}

/** B's note classes turned by 'rotation' semitones. */
Fingerprint rotated (const Fingerprint& fp, int rotation)
{
    if (rotation == 0) return fp;
    Fingerprint out = fp;
    for (int i = 0; i < fp.frames(); ++i)
        for (int b = 0; b < Fingerprint::kBins; ++b) out.chroma[(size_t) i * Fingerprint::kBins + (size_t) ((b + rotation) % Fingerprint::kBins)] = fp.frame (i)[b];
    return out;
}

/** How many semitones B must be turned by to sound in A's key: from the two takes' average chroma. 0 unless clearly better. */
int bestRotation (const Fingerprint& a, const Fingerprint& b)
{
    auto mean = [] (const Fingerprint& fp)
    {
        std::vector<float> m ((size_t) Fingerprint::kBins, 0.0f); int n = 0;
        for (int i = 0; i < fp.frames(); ++i)
            if (fp.loudness[(size_t) i] >= kSilenceDb) { for (int k = 0; k < Fingerprint::kBins; ++k) m[(size_t) k] += fp.frame (i)[k]; ++n; }
        float norm = 0.0f;
        for (auto v : m) norm += v * v;
        norm = std::sqrt (norm);
        if (norm > 0.0f) for (auto& v : m) v /= norm;
        return m;
    };
    const auto ma = mean (a), mb = mean (b);
    float best = -1.0f, atZero = 0.0f; int bestR = 0;
    for (int r = 0; r < Fingerprint::kBins; ++r)
    {
        float dot = 0.0f;
        for (int k = 0; k < Fingerprint::kBins; ++k) dot += ma[(size_t) ((k + r) % Fingerprint::kBins)] * mb[(size_t) k];
        if (r == 0) atZero = dot;
        if (dot > best) { best = dot; bestR = r; }
    }
    return best > atZero + 0.05f ? bestR : 0;
}

struct PathPoint { int i, j; float d; };

/** Dynamic time warping of A against B, open at both ends: the path may begin anywhere on A's or B's first frame and end anywhere on
    either's last frame, so the two need only share a stretch. 'band' (fine level): for each i of A only the j within 'band' of
    'centre[i]' are tried (-1: no band, the whole of B). Steps: diagonal (weight 2), along A, along B (weight 1); the cost is the
    weighted sum of the local distances, and the end is chosen by cost per unit of weight among paths heavier than 'minWeight'. */
std::vector<PathPoint> dtw (const Fingerprint& A, const Fingerprint& B, const std::vector<int>* centre, int band, float minWeight, const std::atomic<bool>* cancel)
{
    const int n = A.frames(), m = B.frames();
    std::vector<PathPoint> path;
    if (n == 0 || m == 0) return path;
    const int width = centre != nullptr ? 2 * band + 1 : m;
    auto lo = [&] (int i) { return centre != nullptr ? juce::jmax (0, (*centre)[(size_t) i] - band) : 0; };
    auto hi = [&] (int i) { return centre != nullptr ? juce::jmin (m - 1, (*centre)[(size_t) i] + band) : m - 1; };
    auto col = [&] (int i, int j) { return centre != nullptr ? j - ((*centre)[(size_t) i] - band) : j; };     // where j sits in row i's storage
    const float inf = 1.0e30f;
    std::vector<float> dPrev ((size_t) width, inf), dCur ((size_t) width, inf), wPrev ((size_t) width, 0.0f), wCur ((size_t) width, 0.0f);
    std::vector<std::uint8_t> dir ((size_t) n * (size_t) width, 3);          // 0 diagonal, 1 from (i-1, j), 2 from (i, j-1), 3 a start
    struct End { int i, j; float norm, weight; };
    std::vector<End> ends;
    float bestNorm = inf; int bestI = -1, bestJ = -1;
    for (int i = 0; i < n; ++i)
    {
        if (cancel != nullptr && (i & 63) == 0 && cancel->load()) return path;
        std::fill (dCur.begin(), dCur.end(), inf);
        const int jl = lo (i), jh = hi (i);
        for (int j = jl; j <= jh; ++j)
        {
            const float d = distance (A.frame (i), B.frame (j));
            const int c = col (i, j);
            float best = inf, bestW = 0.0f; std::uint8_t from = 3;
            auto consider = [&] (float prev, float prevW, float add, float addW, std::uint8_t f)
            {
                if (prev >= inf) return;
                const float v = prev + add;
                if (v < best) { best = v; bestW = prevW + addW; from = f; }
            };
            if (i > 0)
            {
                const int pl = lo (i - 1), ph = hi (i - 1);
                if (j - 1 >= pl && j - 1 <= ph) consider (dPrev[(size_t) col (i - 1, j - 1)], wPrev[(size_t) col (i - 1, j - 1)], 2.0f * d, 2.0f, 0);
                if (j >= pl && j <= ph) consider (dPrev[(size_t) col (i - 1, j)], wPrev[(size_t) col (i - 1, j)], d, 1.0f, 1);
            }
            if (j > jl) consider (dCur[(size_t) (c - 1)], wCur[(size_t) (c - 1)], d, 1.0f, 2);
            const bool edge = i == 0 || j == 0;                 // (a cell with no predecessor inside the band is a start too: best stays at inf)
            if (best >= inf || (edge && d < best)) { best = d; bestW = 1.0f; from = 3; }
            dCur[(size_t) c] = best; wCur[(size_t) c] = bestW; dir[(size_t) i * (size_t) width + (size_t) c] = from;
            // a possible end: the last frame of A, the last frame of B, or (within a band) the band's far edge
            const bool end = i == n - 1 || j == m - 1 || (centre != nullptr && j == jh);
            if (end && bestW >= minWeight) ends.push_back ({ i, j, best / bestW, bestW });
        }
        std::swap (dPrev, dCur); std::swap (wPrev, wCur);
    }
    // the end: the longest path among those nearly as good as the best - a short stretch that happens to match very well must not win
    // over the whole shared stretch
    for (auto& e : ends) bestNorm = juce::jmin (bestNorm, e.norm);
    {
        float heaviestOk = -1.0f;
        for (auto& e : ends)
            if (e.norm <= bestNorm * 1.3f + 0.01f && e.weight > heaviestOk) { heaviestOk = e.weight; bestI = e.i; bestJ = e.j; }
    }
    static const bool debug = juce::SystemStats::getEnvironmentVariable ("FERMATA_TM_DEBUG", "").isNotEmpty();      // (FERMATA_TM_DEBUG=1: a line per pass on stderr, for tuning on real takes)
    if (debug)
        std::fprintf (stderr, "takematch dtw %s: %d x %d frames, %zu possible ends, best cost %.3f, chosen end (%d,%d)\n", centre != nullptr ? "fine" : "coarse", n, m, ends.size(), bestNorm, bestI, bestJ);
    if (bestI < 0) return path;
    // back along the path
    int i = bestI, j = bestJ;
    while (true)
    {
        path.push_back ({ i, j, distance (A.frame (i), B.frame (j)) });
        const auto f = dir[(size_t) i * (size_t) width + (size_t) col (i, j)];
        if (f == 3) break;
        if (f == 0) { --i; --j; } else if (f == 1) --i; else --j;
        if (i < 0 || j < 0) break;
    }
    std::reverse (path.begin(), path.end());
    return path;
}
} // namespace

// ============================================================================ the fingerprint
Fingerprint analyseSamples (const float* mono, juce::int64 numSamples, double sampleRate, const std::atomic<bool>* cancel)
{
    Fingerprint fp;
    fp.fps = kRate / kHop;
    ChromaBuilder builder;
    std::vector<float> out;
    if (std::abs (sampleRate - kRate) < 0.5)
        builder.push (mono, (size_t) numSamples, fp);
    else
    {
        SincResampler rs (sampleRate, kRate, 16);
        const size_t block = 65536;
        for (size_t off = 0; off < (size_t) numSamples; off += block)
        {
            if (cancel != nullptr && cancel->load()) return {};
            out.clear();
            rs.process (mono + off, juce::jmin (block, (size_t) numSamples - off), out);
            builder.push (out.data(), out.size(), fp);
        }
        out.clear(); rs.finish (out); builder.push (out.data(), out.size(), fp);
    }
    builder.finish (fp);
    smooth (fp);
    return fp;
}

juce::String sourceKeyFor (const std::vector<juce::File>& files)
{
    juce::String key;
    for (auto& f : files) key << f.getFileName() << "|" << juce::String (f.getSize()) << "|" << juce::String (f.getLastModificationTime().toMilliseconds()) << ";";
    return key;
}

Fingerprint analyseFiles (juce::AudioFormatManager& fm, const std::vector<juce::File>& files, const std::atomic<bool>* cancel, const std::function<void (float)>& progress, int maxFiles)
{
    Fingerprint fp;
    std::vector<std::unique_ptr<juce::AudioFormatReader>> readers;
    std::vector<juce::File> used;
    for (auto& f : files)
    {
        if ((int) readers.size() >= maxFiles) break;
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
        if (r != nullptr && r->lengthInSamples > 0) { readers.push_back (std::move (r)); used.push_back (f); }
    }
    if (readers.empty()) return fp;
    const double rate = readers.front()->sampleRate;
    juce::int64 length = 0; int channels = 0;
    for (auto& r : readers) { length = juce::jmax (length, r->lengthInSamples); channels += (int) r->numChannels; }
    fp.fps = kRate / kHop;
    ChromaBuilder builder;
    SincResampler rs (rate, kRate, 16);
    const int block = 65536;
    std::vector<float> mono ((size_t) block), out;
    juce::AudioBuffer<float> tmp;
    const float gain = 1.0f / (float) juce::jmax (1, channels);
    for (juce::int64 pos = 0; pos < length; pos += block)
    {
        if (cancel != nullptr && cancel->load()) return {};
        const int n = (int) juce::jmin ((juce::int64) block, length - pos);
        std::fill (mono.begin(), mono.begin() + n, 0.0f);
        for (auto& r : readers)
        {
            if (pos >= r->lengthInSamples) continue;
            const int here = (int) juce::jmin ((juce::int64) n, r->lengthInSamples - pos);
            tmp.setSize ((int) r->numChannels, here, false, false, true);
            if (! r->read (&tmp, 0, here, pos, true, true)) continue;
            for (int ch = 0; ch < (int) r->numChannels; ++ch)
            {
                const float* s = tmp.getReadPointer (ch);
                for (int i = 0; i < here; ++i) mono[(size_t) i] += s[i] * gain;
            }
        }
        out.clear(); rs.process (mono.data(), (size_t) n, out); builder.push (out.data(), out.size(), fp);
        if (progress) progress ((float) ((double) (pos + n) / (double) length));
    }
    out.clear(); rs.finish (out); builder.push (out.data(), out.size(), fp);
    builder.finish (fp);
    smooth (fp);
    fp.sourceKey = sourceKeyFor (files);           // ALL the files given, not only the ones mixed: the service compares against the take's whole file list (3.5.0: with
                                                   // only the mixed ones in the key a take of more than four files never matched its own key, so it was analysed again and again)
    return fp;
}

bool save (const Fingerprint& fp, const juce::File& file)
{
    juce::MemoryOutputStream mo;
    mo.write ("FMFP", 4); mo.writeInt (1);
    mo.writeDouble (fp.fps); mo.writeInt (fp.frames()); mo.writeString (fp.sourceKey);
    mo.write (fp.chroma.data(), fp.chroma.size() * sizeof (float));
    mo.write (fp.loudness.data(), fp.loudness.size() * sizeof (float));
    file.getParentDirectory().createDirectory();
    juce::TemporaryFile tmp (file);
    {
        juce::FileOutputStream fo (tmp.getFile());
        if (! fo.openedOk()) return false;
        fo.write (mo.getData(), mo.getDataSize());
        fo.flush();
        if (fo.getStatus().failed()) return false;
    }
    return tmp.overwriteTargetFileWithTemporary();
}

bool load (Fingerprint& fp, const juce::File& file)
{
    juce::FileInputStream in (file);
    if (! in.openedOk()) return false;
    char magic[4] = {};
    if (in.read (magic, 4) != 4 || std::memcmp (magic, "FMFP", 4) != 0) return false;
    if (in.readInt() != 1) return false;
    Fingerprint out;
    out.fps = in.readDouble();
    const int frames = in.readInt();
    out.sourceKey = in.readString();
    if (out.fps <= 0.0 || frames < 0 || frames > 100000000) return false;
    out.chroma.resize ((size_t) frames * Fingerprint::kBins); out.loudness.resize ((size_t) frames);
    if (in.read (out.chroma.data(), (int) (out.chroma.size() * sizeof (float))) != (int) (out.chroma.size() * sizeof (float))) return false;
    if (in.read (out.loudness.data(), (int) (out.loudness.size() * sizeof (float))) != (int) (out.loudness.size() * sizeof (float))) return false;
    fp = std::move (out);
    return true;
}

// ============================================================================ the alignment
double Alignment::map (double secondsInA, bool* inside) const
{
    if (a.empty()) { if (inside != nullptr) *inside = false; return 0.0; }
    if (inside != nullptr) *inside = secondsInA >= a.front() && secondsInA <= a.back();
    if (secondsInA <= a.front()) return b.front() + (secondsInA - a.front());
    if (secondsInA >= a.back()) return b.back() + (secondsInA - a.back());
    const auto it = std::lower_bound (a.begin(), a.end(), (float) secondsInA);
    const size_t k = (size_t) (it - a.begin());
    if (k == 0) return b.front();
    const float a0 = a[k - 1], a1 = a[k], b0 = b[k - 1], b1 = b[k];
    if (a1 <= a0) return 0.5 * (b0 + b1);                       // a vertical part of the path: the middle of the stretch it maps to
    return b0 + (b1 - b0) * ((secondsInA - a0) / (a1 - a0));
}

Alignment align (const Fingerprint& A, const Fingerprint& Bin, const std::atomic<bool>* cancel)
{
    Alignment out;
    if (A.empty() || Bin.empty() || A.fps <= 0.0) return out;
    out.rotation = bestRotation (A, Bin);
    const Fingerprint B = rotated (Bin, out.rotation);
    // the coarse pass: at most about 1500 frames of the longer take, so the whole of A against the whole of B is cheap
    const double longest = juce::jmax (A.lengthSeconds(), B.lengthSeconds());
    const int factor = juce::jmax (10, (int) std::ceil (longest * A.fps / 1500.0));      // at least 2 frames a second at 20 fps
    const auto cA = downsample (A, factor), cB = downsample (B, factor);
    const float minWeight = (float) (20.0 * cA.fps * 2.0);                                  // the shared stretch must be at least 20 s
    const auto coarse = dtw (cA, cB, nullptr, 0, juce::jmin (minWeight, (float) (juce::jmin (cA.frames(), cB.frames()))), cancel);
    static const bool debug = juce::SystemStats::getEnvironmentVariable ("FERMATA_TM_DEBUG", "").isNotEmpty();
    if (debug)
    {
        double sum = 0.0; for (auto& p : coarse) sum += p.d;
        std::fprintf (stderr, "coarse: factor %d, %d x %d frames, path %zu, mean cost %.3f, from (%d,%d) to (%d,%d)\n", factor, cA.frames(), cB.frames(), coarse.size(), coarse.empty() ? 0.0 : sum / coarse.size(),
                      coarse.empty() ? -1 : coarse.front().i, coarse.empty() ? -1 : coarse.front().j, coarse.empty() ? -1 : coarse.back().i, coarse.empty() ? -1 : coarse.back().j);
    }
    if (coarse.empty()) return out;
    // the fine pass: the full-rate frames of A along the coarse path, B tried within 2.5 s either side of where the coarse path put it
    const int i0 = coarse.front().i * factor, i1 = juce::jmin (A.frames() - 1, (coarse.back().i + 1) * factor - 1);
    std::vector<int> centre ((size_t) A.frames(), 0);
    {
        size_t k = 0;
        for (int i = 0; i < A.frames(); ++i)
        {
            const int ci = i / factor;
            while (k + 1 < coarse.size() && coarse[k + 1].i <= ci) ++k;
            // the j at this coarse i (the middle of the j's the coarse path spends at it: k is the last point of that run)
            size_t q0 = k;
            while (q0 > 0 && coarse[q0 - 1].i == coarse[k].i) --q0;
            const int jlo = coarse[q0].j, jhi = coarse[k].j;
            centre[(size_t) i] = juce::jlimit (0, B.frames() - 1, (int) std::lround (((jlo + jhi) * 0.5 + 0.5) * factor));
        }
    }
    const int band = (int) std::lround (2.5 * A.fps);
    // (only the part of A the coarse path covers takes part: a window into A's frames)
    Fingerprint Awin; Awin.fps = A.fps;
    Awin.chroma.assign (A.chroma.begin() + (long) i0 * Fingerprint::kBins, A.chroma.begin() + (long) (i1 + 1) * Fingerprint::kBins);
    Awin.loudness.assign (A.loudness.begin() + i0, A.loudness.begin() + i1 + 1);
    std::vector<int> centreWin (centre.begin() + i0, centre.begin() + i1 + 1);
    const auto fine = dtw (Awin, B, &centreWin, band, (float) (10.0 * A.fps * 2.0), cancel);
    if (fine.empty()) return out;
    // the ends: a stretch at either end where the two plainly differ (one take goes on after the other has stopped) is not part of the match
    size_t first = 0, last = fine.size();
    {
        const size_t win = (size_t) juce::jmax (1, (int) std::lround (A.fps));            // a second of path points
        auto meanCost = [&] (size_t from, size_t to) { double sum = 0.0; for (size_t k = from; k < to; ++k) sum += fine[k].d; return to > from ? sum / (double) (to - from) : 0.0; };
        while (last - first > 2 * win && meanCost (last - win, last) > 0.4) last -= win;
        while (last - first > 2 * win && meanCost (first, first + win) > 0.4) first += win;
    }
    out.a.reserve (last - first); out.b.reserve (last - first); out.cost.reserve (last - first);
    for (size_t k = first; k < last; ++k)
    {
        const auto& p = fine[k];
        out.a.push_back ((float) ((p.i + i0) / A.fps));
        out.b.push_back ((float) (p.j / B.fps));
        out.cost.push_back (p.d);
    }
    return out;
}

// ============================================================================ finding every version of a passage
Fingerprint analyseRange (juce::AudioFormatManager& fm, const std::vector<Source>& sources, juce::int64 from, juce::int64 to, int maxFiles)
{
    Fingerprint fp;
    if (to <= from) return fp;
    struct Open { std::unique_ptr<juce::AudioFormatReader> reader; juce::int64 fileStart; };
    std::vector<Open> readers;
    for (auto& s : sources)
    {
        if ((int) readers.size() >= maxFiles) break;
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (s.file));
        if (r != nullptr && r->lengthInSamples > 0) readers.push_back ({ std::move (r), s.fileStart });
    }
    if (readers.empty()) return fp;
    const double rate = readers.front().reader->sampleRate;
    int channels = 0;
    for (auto& r : readers) channels += (int) r.reader->numChannels;
    std::vector<float> mono ((size_t) (to - from), 0.0f);
    juce::AudioBuffer<float> tmp;
    const float gain = 1.0f / (float) juce::jmax (1, channels);
    const int block = 65536;
    for (auto& r : readers)
        for (juce::int64 pos = from; pos < to; pos += block)
        {
            const juce::int64 inFile = pos - r.fileStart;
            const int n = (int) juce::jmin ((juce::int64) block, to - pos);
            if (inFile + n <= 0 || inFile >= r.reader->lengthInSamples) continue;
            const juce::int64 a = juce::jmax ((juce::int64) 0, inFile), b = juce::jmin (r.reader->lengthInSamples, inFile + n);
            tmp.setSize ((int) r.reader->numChannels, (int) (b - a), false, false, true);
            if (! r.reader->read (&tmp, 0, (int) (b - a), a, true, true)) continue;
            for (int ch = 0; ch < (int) r.reader->numChannels; ++ch)
            {
                const float* src = tmp.getReadPointer (ch);
                for (int i = 0; i < (int) (b - a); ++i) mono[(size_t) (pos - from + (a - inFile) + i)] += src[i] * gain;
            }
        }
    return analyseSamples (mono.data(), (juce::int64) mono.size(), rate);
}

std::vector<Version> findVersions (const Fingerprint& passage, const Fingerprint& take, double maxCost)
{
    std::vector<Version> out;
    const int n = passage.frames(), m = take.frames();
    if (n < 3 || m < 3 || passage.fps <= 0.0) return out;
    // the passage must be matched whole (its first frame to its last); it may begin anywhere in the take. The steps keep the tempo within a
    // factor of two either way: diagonal (weight 2), two frames of the take to one of the passage (3), two of the passage to one of the take (3) -
    // without that the search squeezed the passage into a single chord of the take and called it a match.
    const float inf = 1.0e30f;
    std::vector<float> d2 ((size_t) m, inf), d1 ((size_t) m, inf), d0 ((size_t) m, inf), w2 ((size_t) m, 0.0f), w1 ((size_t) m, 0.0f), w0 ((size_t) m, 0.0f);
    std::vector<std::uint8_t> dir ((size_t) n * (size_t) m, 3);         // 0 from (i-1, j-1), 1 from (i-1, j-2), 2 from (i-2, j-1), 3 a start
    std::vector<float> row ((size_t) m), prevRow ((size_t) m, 0.0f);
    for (int i = 0; i < n; ++i)
    {
        for (int j = 0; j < m; ++j) row[(size_t) j] = distance (passage.frame (i), take.frame (j));
        for (int j = 0; j < m; ++j)
        {
            const float d = row[(size_t) j];
            float best = inf, bestW = 0.0f; std::uint8_t from = 3;
            if (i == 0) { best = d; bestW = 1.0f; }                                  // the passage's first frame may sit at any frame of the take
            else
            {
                if (j >= 1 && d1[(size_t) j - 1] < inf) { const float v = d1[(size_t) j - 1] + 2.0f * d; if (v < best) { best = v; bestW = w1[(size_t) j - 1] + 2.0f; from = 0; } }
                if (j >= 2 && d1[(size_t) j - 2] < inf) { const float v = d1[(size_t) j - 2] + 2.0f * row[(size_t) j - 1] + d; if (v < best) { best = v; bestW = w1[(size_t) j - 2] + 3.0f; from = 1; } }
                if (i >= 2 && j >= 1 && d2[(size_t) j - 1] < inf) { const float v = d2[(size_t) j - 1] + 2.0f * prevRow[(size_t) j] + d; if (v < best) { best = v; bestW = w2[(size_t) j - 1] + 3.0f; from = 2; } }
            }
            d0[(size_t) j] = best; w0[(size_t) j] = bestW; dir[(size_t) i * (size_t) m + (size_t) j] = from;
        }
        std::swap (d2, d1); std::swap (d1, d0); std::swap (w2, w1); std::swap (w1, w0);
        std::swap (prevRow, row);
    }
    // the cost of ending at each frame of the take (d1 is the last row now): the dips are the versions
    std::vector<float> endCost ((size_t) m);
    for (int j = 0; j < m; ++j) endCost[(size_t) j] = d1[(size_t) j] < inf && w1[(size_t) j] > 0.0f ? d1[(size_t) j] / w1[(size_t) j] : 1.0f;
    struct Cand { int j; float c; };
    std::vector<Cand> cands;
    for (int j = 0; j < m; ++j)
    {
        if (endCost[(size_t) j] > (float) maxCost) continue;
        bool dip = true;
        for (int k = juce::jmax (0, j - 3); k <= juce::jmin (m - 1, j + 3); ++k) if (endCost[(size_t) k] < endCost[(size_t) j]) { dip = false; break; }
        if (dip) cands.push_back ({ j, endCost[(size_t) j] });
    }
    std::sort (cands.begin(), cands.end(), [] (const Cand& a, const Cand& b) { return a.c < b.c; });
    auto startOf = [&] (int jEnd)          // back along the path to the frame the passage's first frame sits on
    {
        int i = n - 1, j = jEnd;
        while (i > 0 && j >= 0)
        {
            const auto f = dir[(size_t) i * (size_t) m + (size_t) j];
            if (f == 0) { --i; --j; } else if (f == 1) { --i; j -= 2; } else if (f == 2) { i -= 2; --j; } else break;
        }
        return juce::jmax (0, j);
    };
    const int minApart = juce::jmax (1, n / 2);
    for (auto& c : cands)
    {
        const int j0 = startOf (c.j);
        const double stretch = (double) (c.j + 1 - j0) / (double) n;
        if (stretch < 0.7 || stretch > 1.45) continue;                            // (a match that far from the passage's tempo is not the same music)
        bool clash = false;
        for (auto& v : out)
        {
            const int vs = (int) std::lround (v.start * take.fps), ve = (int) std::lround (v.end * take.fps);
            const int overlap = juce::jmin (ve, c.j + 1) - juce::jmax (vs, j0);
            if (overlap > minApart) { clash = true; break; }
        }
        if (clash) continue;
        Version v;
        v.start = (double) j0 / take.fps; v.end = (double) (c.j + 1) / take.fps; v.cost = c.c; v.stretch = stretch;
        out.push_back (v);
    }
    return out;
}

namespace
{
/** The first file's channels mixed, from sample 'from' (clipped to the file) for 'n' samples; silence where the file has none. */
std::vector<float> monoAround (juce::AudioFormatManager& fm, const Source& s, juce::int64 from, int n)
{
    std::vector<float> out ((size_t) n, 0.0f);
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (s.file));
    if (r == nullptr) return out;
    const juce::int64 a = juce::jmax ((juce::int64) 0, from - s.fileStart), b = juce::jmin (r->lengthInSamples, from - s.fileStart + n);
    if (b <= a) return out;
    juce::AudioBuffer<float> tmp ((int) r->numChannels, (int) (b - a));
    if (! r->read (&tmp, 0, (int) (b - a), a, true, true)) return out;
    const float gain = 1.0f / (float) juce::jmax (1, (int) r->numChannels);
    for (int ch = 0; ch < (int) r->numChannels; ++ch)
        for (int i = 0; i < (int) (b - a); ++i) out[(size_t) (a - (from - s.fileStart) + i)] += tmp.getReadPointer (ch)[i] * gain;
    return out;
}
} // namespace

juce::int64 refineJoin (juce::AudioFormatManager& fm, const Source& a, juce::int64 sampleA, const Source& b, juce::int64 sampleB, double windowSeconds, double searchSeconds, double* similarity)
{
    // Two performances never have the same waveform, so the samples themselves cannot be matched; what lines up is the shape of the sound - the
    // onsets. The loudness envelope (a millisecond at a time, log-compressed) of a's window is slid over b's, and the lag that fits best wins.
    if (similarity != nullptr) *similarity = 0.0;
    double rate = 48000.0;
    { std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (a.file)); if (r == nullptr) return sampleB; rate = r->sampleRate; }
    const int hop = juce::jmax (8, (int) std::lround (rate / 1000.0));                       // a millisecond
    const int win = juce::jmax (8, (int) std::lround (windowSeconds * rate / hop)), search = juce::jmax (1, (int) std::lround (searchSeconds * rate / hop));
    const auto x = monoAround (fm, a, sampleA - (juce::int64) win * hop / 2, win * hop);
    const auto y = monoAround (fm, b, sampleB - (juce::int64) (win / 2 + search) * hop, (win + 2 * search) * hop);
    auto envelope = [hop] (const std::vector<float>& v)
    {
        std::vector<double> e (v.size() / (size_t) hop);
        for (size_t k = 0; k < e.size(); ++k)
        {
            double sum = 0.0;
            for (int i = 0; i < hop; ++i) { const double s = v[k * (size_t) hop + (size_t) i]; sum += s * s; }
            e[k] = std::log1p (1000.0 * std::sqrt (sum / hop));
        }
        double mean = 0.0; for (auto v2 : e) mean += v2; mean /= juce::jmax ((size_t) 1, e.size());
        for (auto& v2 : e) v2 -= mean;                                                     // (the shape, not the level)
        return e;
    };
    const auto ex = envelope (x), ey = envelope (y);
    double xx = 0.0; for (auto v : ex) xx += v * v;
    if (xx <= 1.0e-9 || ey.size() < ex.size()) return sampleB;
    double best = -2.0; int bestLag = 0;
    const int lags = (int) (ey.size() - ex.size());
    for (int lag = 0; lag <= lags; ++lag)
    {
        double xy = 0.0, yy = 0.0;
        for (size_t i = 0; i < ex.size(); ++i) { xy += ex[i] * ey[(size_t) lag + i]; yy += ey[(size_t) lag + i] * ey[(size_t) lag + i]; }
        if (yy <= 1.0e-9) continue;
        const double c = xy / std::sqrt (xx * yy);
        if (c > best) { best = c; bestLag = lag; }
    }
    if (best < 0.0) return sampleB;
    if (similarity != nullptr) *similarity = best;
    juce::int64 result = sampleB + (juce::int64) (bestLag - search) * hop;
    // then, within a couple of milliseconds of that, the waveforms themselves: the two performances differ, but lining the cycles up at the
    // join (where they agree at all) is what makes the crossfade clean. Taken only when the waves do agree there.
    {
        const int fineWin = (int) std::lround (0.02 * rate), fineSearch = 2 * hop;
        const auto fx = monoAround (fm, a, sampleA - fineWin / 2, fineWin);
        const auto fy = monoAround (fm, b, result - fineWin / 2 - fineSearch, fineWin + 2 * fineSearch);
        double fxx = 0.0; for (auto v : fx) fxx += (double) v * v;
        if (fxx > 1.0e-9 && fy.size() >= fx.size())
        {
            double fbest = -2.0; int fbestLag = 0;
            for (int lag = 0; lag <= (int) (fy.size() - fx.size()); ++lag)
            {
                double xy = 0.0, yy = 0.0;
                for (size_t i = 0; i < fx.size(); ++i) { xy += (double) fx[i] * fy[(size_t) lag + i]; yy += (double) fy[(size_t) lag + i] * fy[(size_t) lag + i]; }
                if (yy <= 1.0e-9) continue;
                const double c = xy / std::sqrt (fxx * yy);
                if (c > fbest) { fbest = c; fbestLag = lag; }
            }
            if (fbest > 0.5) result += (juce::int64) (fbestLag - fineSearch);
        }
    }
    return result;
}

Match locate (const Alignment& al, double secondsInA)
{
    Match m;
    if (al.empty()) { m.why = "the two takes have no stretch of music in common"; return m; }
    bool inside = false;
    m.seconds = al.map (secondsInA, &inside);
    if (! inside) { m.why = "the other take does not reach this place"; return m; }
    // how alike the two are around here: the local costs within 4 s
    double sum = 0.0; int n = 0;
    for (size_t k = 0; k < al.a.size(); ++k)
        if (std::abs ((double) al.a[k] - secondsInA) <= 4.0) { sum += al.cost[k]; ++n; }
    m.confidence = n > 0 ? juce::jlimit (0.0, 1.0, 1.0 - sum / n) : 0.0;
    m.found = m.confidence >= kMinConfidence;
    if (! m.found) m.why = "the music does not match well enough here (" + juce::String (juce::roundToInt (m.confidence * 100.0)) + " %)";
    return m;
}
} // namespace td::takematch
