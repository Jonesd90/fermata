#pragma once
#include "Common.h"
#include "Dsp.h"
#include <complex>

namespace td
{
/** The low cut of a mixer channel (audio tracks and Int Buses): a high-pass placed before the inserts. Off by default; 80 Hz at 48 dB / octave
    until changed. Two kinds, chosen per channel:
      - zero latency (the default): a Butterworth high-pass of the slope's order (6 dB per second-order section, 24th order for the wall), run as
        cascaded biquads in double precision - the classic console high-pass, with the phase shift any such filter has and no delay at all;
      - linear phase: a symmetric FIR (no smearing, the sound is only held back by half the filter's length, which the mixer's delay
        compensation makes up on every other path). Designed on the message thread from a target magnitude (a Butterworth-shaped curve of the
        slope's order, or a near-vertical wall) by inverse FFT and a Kaiser window; run on the audio thread by uniform partitioned convolution.
    Neither ever allocates on the audio thread. */
namespace lowcut
{
constexpr int kNumSlopes = 11;                                     // 6, 12, 18, 24, 36, 48, 60, 72, 84, 96 dB / octave, brickwall
constexpr int kBrickwall = 10, kDefaultSlope = 5;                  // 48 dB / octave
constexpr float kDefaultHz = 80.0f, kMinHz = 10.0f;
inline int slopeDb (int i) { static const int d[kNumSlopes] { 6, 12, 18, 24, 36, 48, 60, 72, 84, 96, 0 }; return d[juce::jlimit (0, kNumSlopes - 1, i)]; }
inline juce::String slopeName (int i) { return i >= kBrickwall ? juce::String ("Brickwall") : juce::String (slopeDb (i)) + " dB / oct"; }
/** The highest frequency the filter can be set to at this sample rate (just under Nyquist). */
inline double maxHz (double sr) { return juce::jmax (100.0, sr * 0.5 * 0.95); }

/** The order of the zero-latency (Butterworth) filter for a slope: 6 dB / octave per order; the wall is 24th order (144 dB / octave). */
inline int iirOrder (int slope) { return slope >= kBrickwall ? 24 : juce::jmax (1, slopeDb (slope) / 6); }

/** The target magnitude (0 .. 1) of the filter at frequency f for a cut at fc with this slope: what the FIR is designed to (linear = true), or
    what the Butterworth filter has (the same curve for every slope but the wall, where the Butterworth is a 24th-order roll-off). */
inline double targetMagnitude (double f, double fc, int slope, bool linear = true)
{
    if (f <= 0.0) return 0.0;
    if (slope >= kBrickwall && ! linear)
    {
        const double a = std::pow (f / fc, (double) iirOrder (slope));
        return a / std::sqrt (1.0 + a * a);
    }
    if (slope >= kBrickwall)
    {
        // a sixth of an octave of raised cosine round the cut (on a log scale): as vertical as a short filter allows without ringing for ever
        const double lo = fc * std::pow (2.0, -1.0 / 12.0), hi = fc * std::pow (2.0, 1.0 / 12.0);
        if (f <= lo) return 0.0;
        if (f >= hi) return 1.0;
        const double t = std::log (f / lo) / std::log (hi / lo);
        return 0.5 - 0.5 * std::cos (dsp::kPi * t);
    }
    const double m = (double) slopeDb (slope) / 6.0;                 // the order: 6 dB per octave each
    const double a = std::pow (f / fc, m);
    return a / std::sqrt (1.0 + a * a);                              // Butterworth magnitude: -3 dB at fc, m x 6 dB / octave below
}
/** The same in dB (floored at -120) for drawing the curve. */
inline double targetDb (double f, double fc, int slope, bool linear = true) { return juce::jmax (-120.0, 20.0 * std::log10 (juce::jmax (1.0e-6, targetMagnitude (f, fc, slope, linear)))); }

/** The zero-latency filter of one channel: a Butterworth high-pass as cascaded second-order sections (one first-order section when the order is
    odd), bilinear-transformed with the cut frequency pre-warped, run in double precision (a 10 Hz cut at 96 kHz puts the poles within 1e-6 of
    the unit circle: single precision would not hold them). */
struct Biquads
{
    struct Section { double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0, z1 = 0.0, z2 = 0.0; };
    std::vector<Section> sections;

    /** Designs the cascade for a cut at fc with this slope at this sample rate (message thread). */
    void design (double fc, int slope, double sr)
    {
        sections.clear();
        const int order = iirOrder (slope);
        fc = juce::jlimit (1.0, sr * 0.499, fc);
        const double w0 = 2.0 * dsp::kPi * fc / sr, cw = std::cos (w0), sw = std::sin (w0);
        if (order % 2 == 1)                                              // the real pole: a first-order high-pass
        {
            const double k = std::tan (w0 * 0.5);
            Section s; s.b0 = 1.0 / (1.0 + k); s.b1 = -s.b0; s.b2 = 0.0; s.a1 = (k - 1.0) / (k + 1.0); s.a2 = 0.0;
            sections.push_back (s);
        }
        for (int i = 0; i < order / 2; ++i)
        {
            // the pole pairs of a Butterworth of this order, each as a second-order high-pass of quality q (RBJ)
            const double angle = order % 2 == 0 ? (2.0 * i + 1.0) * dsp::kPi / (2.0 * order) : (i + 1.0) * dsp::kPi / order;
            const double q = 1.0 / (2.0 * std::cos (angle));
            const double alpha = sw / (2.0 * q), a0 = 1.0 + alpha;
            Section s;
            s.b0 = (1.0 + cw) * 0.5 / a0; s.b1 = -(1.0 + cw) / a0; s.b2 = s.b0;
            s.a1 = -2.0 * cw / a0; s.a2 = (1.0 - alpha) / a0;
            sections.push_back (s);
        }
    }
    void reset() noexcept { for (auto& s : sections) s.z1 = s.z2 = 0.0; }
    /** Takes the shape of another cascade (the same order) without losing the sound in this one. */
    void setCoefficients (const Biquads& o) noexcept
    {
        if (o.sections.size() != sections.size()) { sections = o.sections; reset(); return; }
        for (size_t i = 0; i < sections.size(); ++i) { auto& s = sections[i]; const auto& t = o.sections[i]; s.b0 = t.b0; s.b1 = t.b1; s.b2 = t.b2; s.a1 = t.a1; s.a2 = t.a2; }
    }
    void process (float* x, int n) noexcept
    {
        for (auto& s : sections)
        {
            double z1 = s.z1, z2 = s.z2;
            for (int i = 0; i < n; ++i)
            {
                const double in = (double) x[i];
                const double y = s.b0 * in + z1;
                z1 = s.b1 * in - s.a1 * y + z2;
                z2 = s.b2 * in - s.a2 * y;
                x[i] = (float) y;
            }
            if (! std::isfinite (z1) || ! std::isfinite (z2)) { z1 = z2 = 0.0; }
            s.z1 = z1; s.z2 = z2;
        }
    }
    /** The magnitude at frequency f (for checking: the bilinear transform's curve). */
    double magnitudeAt (double f, double sr) const
    {
        const std::complex<double> z = std::polar (1.0, -2.0 * dsp::kPi * f / sr);
        std::complex<double> h (1.0, 0.0);
        for (auto& s : sections) h *= (s.b0 + s.b1 * z + s.b2 * z * z) / (1.0 + s.a1 * z + s.a2 * z * z);
        return std::abs (h);
    }
};

/** How long the FIR is (a power of two): enough cycles of fc to shape the slope, more for the wall, never more than a quarter of a million taps. */
inline int tapsFor (double fc, int slope, double sr)
{
    const double cycles = slope >= 5 ? 24.0 : slope >= 2 ? 12.0 : 6.0;    // (the steep slopes need the finer resolution, or the window rounds them off)
    const double want = cycles * sr / juce::jmax (1.0, fc);
    return juce::jlimit (1024, 262144, (int) juce::nextPowerOfTwo ((int) std::ceil (want)));
}
/** The partition the convolver works in for a filter this long (the sound is held back by this much on top of half the FIR). */
inline int partitionFor (int taps) { return juce::jlimit (256, 4096, (int) juce::nextPowerOfTwo (taps / 32)); }

/** Designs the FIR: zero-phase from the target magnitude by inverse FFT, centred, Kaiser-windowed, the passband at unity. Message thread. */
inline std::vector<float> design (double fc, int slope, double sr)
{
    const int n = tapsFor (fc, slope, sr);
    std::vector<std::complex<double>> spec ((size_t) n);
    for (int k = 0; k <= n / 2; ++k)
    {
        const double h = targetMagnitude ((double) k * sr / (double) n, fc, slope);
        spec[(size_t) k] = h;
        if (k > 0 && k < n / 2) spec[(size_t) (n - k)] = h;
    }
    dsp::FFT fft (n);
    fft.inverse (spec);                                              // real and even: the zero-phase impulse response round sample 0
    std::vector<double> h ((size_t) n);
    for (int i = 0; i < n; ++i) h[(size_t) i] = spec[(size_t) ((i + n / 2) % n)].real();      // centred on n / 2
    // Kaiser window (beta 8: sidelobes under -80 dB, the cut's shape kept)
    auto bessel0 = [] (double x) { double s = 1.0, t = 1.0; const double q = x * x / 4.0; for (int k = 1; k < 60; ++k) { t *= q / ((double) k * (double) k); s += t; if (t < 1.0e-12 * s) break; } return s; };
    const double beta = 8.0, denom = bessel0 (beta);
    for (int i = 0; i < n; ++i)
    {
        const double r = (double) (i - n / 2) / (double) (n / 2);          // centred on the middle tap, so the response stays exactly symmetric
        h[(size_t) i] *= bessel0 (beta * std::sqrt (juce::jmax (0.0, 1.0 - r * r))) / denom;
    }
    double nyq = 0.0;                                                // the gain at Nyquist (in the passband): made exactly 1
    for (int i = 0; i < n; ++i) nyq += (i & 1) ? -h[(size_t) i] : h[(size_t) i];
    const double norm = std::abs (nyq) > 1.0e-9 ? 1.0 / nyq : 1.0;
    std::vector<float> out ((size_t) n);
    for (int i = 0; i < n; ++i) out[(size_t) i] = (float) (h[(size_t) i] * norm);
    return out;
}

/** A radix-2 complex FFT in single precision, with its tables made once: the convolver's (the audio thread never allocates). */
class FftF
{
public:
    explicit FftF (int sizePow2 = 0) { if (sizePow2 > 0) init (sizePow2); }
    void init (int sizePow2)
    {
        n = sizePow2; tw.resize ((size_t) n / 2); rev.resize ((size_t) n);
        for (int i = 0; i < n / 2; ++i) tw[(size_t) i] = std::polar (1.0f, (float) (-2.0 * dsp::kPi * (double) i / (double) n));
        int bits = 0; while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i) { int r = 0; for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b); rev[(size_t) i] = r; }
    }
    int size() const noexcept { return n; }
    void forward (std::complex<float>* a) const noexcept { run (a, false); }
    void inverse (std::complex<float>* a) const noexcept { run (a, true); const float s = 1.0f / (float) n; for (int i = 0; i < n; ++i) a[i] *= s; }
private:
    void run (std::complex<float>* a, bool inv) const noexcept
    {
        for (int i = 0; i < n; ++i) { const int j = rev[(size_t) i]; if (j > i) std::swap (a[i], a[j]); }
        for (int len = 2; len <= n; len <<= 1)
        {
            const int half = len / 2, step = n / len;
            for (int i = 0; i < n; i += len)
                for (int k = 0; k < half; ++k)
                {
                    auto w = tw[(size_t) (k * step)]; if (inv) w = std::conj (w);
                    const auto u = a[i + k], v = a[i + k + half] * w;
                    a[i + k] = u + v; a[i + k + half] = u - v;
                }
        }
    }
    int n = 0; std::vector<std::complex<float>> tw; std::vector<int> rev;
};

/** Uniform partitioned convolution of one channel with an FIR: partitions of B samples, each kept as its spectrum (size 2B); the input is
    gathered B samples at a time, so the output comes B samples late on top of the FIR's own delay. prepare() allocates; process() never does. */
class Convolver
{
public:
    void prepare (const std::vector<float>& fir, int partition)
    {
        B = partition; fftN = 2 * B; fft.init (fftN);
        P = juce::jmax (1, ((int) fir.size() + B - 1) / B);
        H.assign ((size_t) P * (size_t) fftN, 0.0f);
        std::vector<std::complex<float>> tmp ((size_t) fftN);
        for (int p = 0; p < P; ++p)
        {
            std::fill (tmp.begin(), tmp.end(), std::complex<float> (0.0f));
            for (int i = 0; i < B; ++i) { const size_t k = (size_t) p * (size_t) B + (size_t) i; if (k < fir.size()) tmp[(size_t) i] = fir[k]; }
            fft.forward (tmp.data());
            std::copy (tmp.begin(), tmp.end(), H.begin() + (size_t) p * (size_t) fftN);
        }
        fdl.assign ((size_t) P * (size_t) fftN, std::complex<float> (0.0f));
        work.assign ((size_t) fftN, std::complex<float> (0.0f));
        prev.assign ((size_t) B, 0.0f); inRing.assign ((size_t) B, 0.0f); outRing.assign ((size_t) B, 0.0f);
        head = 0; pos = 0;
    }
    /** Keeps the filter's shape but forgets the sound in it. */
    void reset() noexcept
    {
        std::fill (fdl.begin(), fdl.end(), std::complex<float> (0.0f));
        std::fill (prev.begin(), prev.end(), 0.0f); std::fill (inRing.begin(), inRing.end(), 0.0f); std::fill (outRing.begin(), outRing.end(), 0.0f);
        head = 0; pos = 0;
    }
    /** Puts new partition spectra in (the same B and P): the sound in the filter carries on, with the new shape. */
    void setKernel (const Convolver& designed) noexcept { if (designed.P == P && designed.fftN == fftN) std::copy (designed.H.begin(), designed.H.end(), H.begin()); }
    bool sameLayout (int partition, int numPartitions) const noexcept { return B == partition && P == numPartitions; }
    int partition() const noexcept { return B; }
    int partitions() const noexcept { return P; }
    int extraLatency() const noexcept { return B; }

    void process (float* x, int n) noexcept
    {
        if (B <= 0) return;
        for (int i = 0; i < n; ++i)
        {
            const float o = outRing[(size_t) pos];
            inRing[(size_t) pos] = x[i];
            x[i] = o;
            if (++pos == B) { runPartition(); pos = 0; }
        }
    }

private:
    void runPartition() noexcept
    {
        // overlap-save: FFT of [the previous B samples | these B], the product with every partition summed, the last B of the result is the output
        for (int i = 0; i < B; ++i) { work[(size_t) i] = prev[(size_t) i]; work[(size_t) (B + i)] = inRing[(size_t) i]; }
        std::copy (inRing.begin(), inRing.end(), prev.begin());
        fft.forward (work.data());
        head = (head + 1) % P;
        std::copy (work.begin(), work.end(), fdl.begin() + (size_t) head * (size_t) fftN);
        const int half = fftN / 2;                                   // real input: the upper half of the spectrum is the mirror of the lower
        for (int k = 0; k <= half; ++k) work[(size_t) k] = std::complex<float> (0.0f);
        for (int p = 0; p < P; ++p)
        {
            const auto* X = fdl.data() + (size_t) ((head - p + P) % P) * (size_t) fftN;
            const auto* Hp = H.data() + (size_t) p * (size_t) fftN;
            for (int k = 0; k <= half; ++k) work[(size_t) k] += X[k] * Hp[k];
        }
        for (int k = 1; k < half; ++k) work[(size_t) (fftN - k)] = std::conj (work[(size_t) k]);
        fft.inverse (work.data());
        for (int i = 0; i < B; ++i) outRing[(size_t) i] = work[(size_t) (B + i)].real();
    }
    int B = 0, fftN = 0, P = 0, head = 0, pos = 0;
    FftF fft;
    std::vector<std::complex<float>> H, fdl, work;
    std::vector<float> prev, inRing, outRing;
};
} // namespace lowcut

/** The low cut of one channel: its settings (saved with the mixer), and the filter that runs them. The audio thread only try-locks; the message
    thread's changes (a new frequency or slope, a new sample rate) build the new filter outside the lock and swap it in. */
class LowCut
{
public:
    Flag on;                                                   // the LC key
    Param hz { lowcut::kDefaultHz };
    std::atomic<int> slope { lowcut::kDefaultSlope };
    Flag linear { false };                                     // true: the linear-phase FIR (held back by half its length); false: the zero-latency Butterworth

    /** How many samples this low cut holds the sound back by right now: 0 while it is off (then it is not in the sound at all), and 0 for the zero-latency filter. */
    int latencySamples() const noexcept { return on.get() && ready.load (std::memory_order_relaxed) ? latency.load (std::memory_order_relaxed) : 0; }
    double preparedRate() const noexcept { return sampleRate; }
    int filterTaps() const noexcept { return taps; }           // (0 for the zero-latency filter)
    /** What the linear-phase filter WOULD hold the sound back by at these settings (the panel's DELAY while linear phase is unticked). */
    static int linearLatencyFor (double fc, int slope, double sr) { const int n = lowcut::tapsFor (fc, slope, sr); return n / 2 + lowcut::partitionFor (n); }

    /** Message thread: the sample rate / block size (the engine's prepare). The filter is designed again if it was ever used. */
    void prepare (double sr, int maxBlock, int numChannels)
    {
        sampleRate = sr; block = maxBlock; channels = juce::jlimit (1, (int) kMaxTrackChannels, numChannels);
        if (on.get() || ready.load()) rebuild();
    }
    /** Message thread: a new frequency / slope (the panel), or the key: the filter follows. */
    void setDesign (float newHz, int newSlope) { setDesign (newHz, newSlope, linear.get()); }
    void setDesign (float newHz, int newSlope, bool linearPhase)
    {
        hz.set (juce::jlimit (lowcut::kMinHz, (float) lowcut::maxHz (sampleRate > 0.0 ? sampleRate : 48000.0), newHz));
        slope.store (juce::jlimit (0, lowcut::kNumSlopes - 1, newSlope));
        linear.set (linearPhase);
        rebuild();
    }
    void setOn (bool b)
    {
        on.set (b);
        if (b && ! ready.load()) rebuild();
    }
    /** Message thread (the engine, as it builds its plan): a track wider than the filter was made for gets a filter per channel. */
    void ensureChannels (int numChannels)
    {
        numChannels = juce::jlimit (1, (int) kMaxTrackChannels, numChannels);
        if (numChannels <= channels) return;
        channels = numChannels;
        if (ready.load()) { ready.store (false); rebuild(); }       // (a fresh set: the old filters had too few channels)
    }

    /** Audio thread: the channel's sound through the filter. While the key is off the filter still hears the sound (on a copy), so switching
        it on is clean - as with a bypassed plug-in. */
    void process (juce::AudioBuffer<float>& b) noexcept
    {
        if (! ready.load (std::memory_order_relaxed)) return;
        const juce::SpinLock::ScopedTryLockType sl (lock);
        if (! sl.isLocked()) return;
        const bool fir = usingFir;
        const int nc = juce::jmin (b.getNumChannels(), fir ? (int) conv.size() : (int) iir.size()), n = b.getNumSamples();
        if (on.get())
        {
            for (int c = 0; c < nc; ++c) { if (fir) conv[(size_t) c].process (b.getWritePointer (c), n); else iir[(size_t) c].process (b.getWritePointer (c), n); }
            return;
        }
        if (n > (int) scratch.size()) return;
        for (int c = 0; c < nc; ++c)
        {
            std::copy (b.getReadPointer (c), b.getReadPointer (c) + n, scratch.begin());
            if (fir) conv[(size_t) c].process (scratch.data(), n); else iir[(size_t) c].process (scratch.data(), n);
        }
    }

private:
    void rebuild()
    {
        const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        if (! linear.get())                                        // the zero-latency filter: a few numbers, made and swapped in at once
        {
            lowcut::Biquads designed; designed.design ((double) hz.get(), slope.load(), sr);
            const bool sameOrder = usingFir == false && ! iir.empty() && (int) iir.size() == channels && iir[0].sections.size() == designed.sections.size();
            std::vector<lowcut::Biquads> fresh;
            if (! sameOrder) fresh.assign ((size_t) channels, designed);
            std::vector<float> newScratch ((size_t) juce::jmax (block, 16), 0.0f);
            std::vector<lowcut::Convolver> oldConv;
            {
                TimedLock tl (lock);
                if (! tl.locked) return;
                if (sameOrder) for (auto& c : iir) c.setCoefficients (designed);   // the same order: the sound in the filter carries on under the new shape
                else iir.swap (fresh);
                oldConv.swap (conv);                                   // (the FIR, if there was one, is let go of)
                usingFir = false; taps = 0;
                scratch.swap (newScratch);
                latency.store (0, std::memory_order_relaxed);
                ready.store (true, std::memory_order_relaxed);
            }
            return;                                                    // 'fresh' / 'oldConv' are freed here, outside the lock
        }
        const auto fir = lowcut::design ((double) hz.get(), slope.load(), sr);
        const int part = lowcut::partitionFor ((int) fir.size());
        lowcut::Convolver designed; designed.prepare (fir, part);
        const int newLat = (int) fir.size() / 2 + designed.extraLatency();
        // the new filters are made here, outside the lock (the audio thread only misses the block in which they are swapped in)
        const bool sameLayout = usingFir && ! conv.empty() && (int) conv.size() == channels && conv[0].sameLayout (part, designed.partitions());
        std::vector<lowcut::Convolver> fresh;
        if (! sameLayout) { fresh.resize ((size_t) channels); for (auto& c : fresh) c.prepare (fir, part); }
        std::vector<float> newScratch ((size_t) juce::jmax (block, 16), 0.0f);
        std::vector<lowcut::Biquads> oldIir;
        {
            TimedLock tl (lock);
            if (! tl.locked) return;
            if (sameLayout) for (auto& c : conv) c.setKernel (designed);  // same length: the sound in the filter carries on under the new shape
            else conv.swap (fresh);
            oldIir.swap (iir);
            usingFir = true;
            taps = (int) fir.size();
            scratch.swap (newScratch);
            latency.store (newLat, std::memory_order_relaxed);
            ready.store (true, std::memory_order_relaxed);
        }
        // 'fresh' (now the old filters) is freed here, outside the lock
    }
    struct TimedLock
    {
        explicit TimedLock (juce::SpinLock& l) : lk (l)
        {
            const auto until = juce::Time::getMillisecondCounter() + 2000u;
            while (! (locked = lk.tryEnter()) && juce::Time::getMillisecondCounter() < until) juce::Thread::sleep (1);
        }
        ~TimedLock() { if (locked) lk.exit(); }
        juce::SpinLock& lk; bool locked = false;
    };
    mutable juce::SpinLock lock;
    std::vector<lowcut::Convolver> conv;                       // the linear-phase filter, one per channel (empty while the zero-latency one is in use)
    std::vector<lowcut::Biquads> iir;                          // the zero-latency filter, one per channel
    bool usingFir = false;                                     // which of the two the audio thread runs (changed under the lock)
    std::vector<float> scratch;
    std::atomic<bool> ready { false };
    std::atomic<int> latency { 0 };
    double sampleRate = 0.0; int block = 512, channels = 2, taps = 0;
};
} // namespace td
