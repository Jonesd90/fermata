#pragma once
#include "Common.h"
#include <vector>
#include <cmath>
#include <cstring>
#include <numeric>

namespace td
{
/** Live sample-rate conversion (3.8.0): playing a project on an audio device that runs at another sample rate - a laptop's built-in sound, which only runs
    at 48 kHz, with a 96 kHz project, say.

    One device has one clock, so it is the whole device that is bridged, never single outputs: the engine (playback, the mixers, plug-ins, automation, LTC,
    the click and the pitch pipe) runs at the project's rate, and what it sends to the device's outputs is converted to the device's rate as it leaves;
    what comes in from the device's inputs is converted to the project's rate as it arrives (so meters, monitoring and the talkback mic still work).
    Recording is not possible like this (a take is always recorded at the device's own rate): see AudioEngine::startRecording.

    RateConverter: one fixed ratio between two rates, any two of the usual ones (the 44.1 kHz and 48 kHz families and their multiples). Real-time safe once
    prepared: nothing is allocated and nothing waits. It is a polyphase windowed-sinc filter with a Kaiser window: flat (within 0.0001 dB) up to 86% of the
    lower rate's half (20.6 kHz when one side is 48 kHz), and 120 dB down from that half upwards, so nothing folds back into the audio band. It delays the
    sound by about 1.2 ms when one side is 48 kHz. Every channel keeps its own history; a channel that has been digital silence for longer than the filter
    gives silence without any work (unused outputs cost nothing). */
class RateConverter
{
public:
    static constexpr int kMaxPhases = 2048;          // the most filter phases allowed (the rates' ratio, reduced, must be simpler than 2048 / anything)
    static constexpr double kPassShare = 0.86;       // flat up to this share of the lower rate's half
    static constexpr double kStopDb = 120.0;         // how far down everything above the lower rate's half is

    /** The ratio of two rates as whole numbers, reduced: out / in = up / down. False when either is not a whole number of Hz, or the ratio is too odd. */
    static bool ratio (double inRate, double outRate, int& up, int& down) noexcept
    {
        const long long a = std::llround (inRate), b = std::llround (outRate);
        if (a <= 0 || b <= 0 || std::abs ((double) a - inRate) > 0.01 || std::abs ((double) b - outRate) > 0.01) return false;
        const long long g = std::gcd (a, b);
        if (b / g > kMaxPhases || a / g > (1LL << 24)) return false;
        up = (int) (b / g); down = (int) (a / g);
        return true;
    }
    static bool canConvert (double inRate, double outRate) noexcept { int u = 0, d = 0; return ratio (inRate, outRate, u, d); }

    /** Makes the filter and the channels' memories. maxInput: the most input samples one process() call is given. False when the rates have no simple ratio. */
    bool prepare (double inRate, double outRate, int numChannels, int maxInput)
    {
        ready = false;
        if (! ratio (inRate, outRate, L, M) || maxInput <= 0) return false;
        rateIn = inRate; rateOut = outRate;
        const double low = juce::jmin (inRate, outRate);
        const double pass = kPassShare * 0.5 * low, stop = 0.5 * low;
        const double fc = 0.5 * (pass + stop) / (0.5 * inRate);                       // the cut-off as a share of the input's half rate
        const double width = (stop - pass) / low;                                     // the transition as a share of the lower rate
        const double beta = 0.1102 * (kStopDb - 8.7);
        const int lowTaps = (int) std::ceil ((kStopDb - 7.95) / (2.285 * 2.0 * juce::MathConstants<double>::pi * width));
        taps = (int) std::ceil ((double) lowTaps * inRate / low);
        taps = (taps + 7) & ~7;                                                       // (a multiple of 8: the inner loop vectorises)
        if (taps > 8192) return false;
        coef.assign ((size_t) L * (size_t) taps, 0.0f);
        const double Lh = (double) taps * L, tc = 0.5 * (Lh - 1.0), half = 0.5 * Lh;
        const double i0b = bessel0 (beta);
        for (int p = 0; p < L; ++p)
        {
            double sum = 0.0;
            std::vector<double> v ((size_t) taps);
            for (int k = 0; k < taps; ++k)                                             // tap k multiplies the input k samples before the newest one
            {
                const double i = (double) p + (double) k * L;
                const double x = (i - tc) / L;                                       // input samples from the filter's centre
                const double a = (i - tc) / half;
                const double w = std::abs (a) < 1.0 ? bessel0 (beta * std::sqrt (1.0 - a * a)) / i0b : 0.0;
                const double px = juce::MathConstants<double>::pi * fc * x;
                const double s = std::abs (px) < 1.0e-12 ? fc : fc * std::sin (px) / px;
                v[(size_t) k] = s * w; sum += s * w;
            }
            for (int k = 0; k < taps; ++k)                                             // each phase passes DC exactly; stored oldest input first
                coef[(size_t) p * (size_t) taps + (size_t) (taps - 1 - k)] = (float) (sum != 0.0 ? v[(size_t) k] / sum : 0.0);
        }
        maxIn = maxInput;
        channels = juce::jmax (0, numChannels);
        hist.assign ((size_t) channels, std::vector<float> ((size_t) (taps - 1 + maxIn), 0.0f));
        quiet.assign ((size_t) channels, taps);
        acc = 0;
        ready = true;
        return true;
    }
    int numChannels() const noexcept { return channels; }
    int up() const noexcept { return L; }
    int down() const noexcept { return M; }
    /** How late the output is, in seconds (the filter's centre). */
    double delaySeconds() const noexcept { return ready ? 0.5 * ((double) taps * L - 1.0) / L / rateIn : 0.0; }

    /** Input samples needed for 'outputs' more output samples (from where it stands now). */
    int inputFor (int outputs) const noexcept { return outputs <= 0 ? 0 : (int) ((acc + (juce::int64) (outputs - 1) * M) / L + 1); }
    /** The most input samples inputFor can ask for, for 'outputs' outputs. */
    int maxInputFor (int outputs) const noexcept { return outputs <= 0 ? 0 : (int) (((juce::int64) outputs * M - 1) / L + 1); }
    /** The most output samples 'inputs' input samples can give. */
    int maxOutputFor (int inputs) const noexcept { return inputs <= 0 ? 0 : (int) (((juce::int64) inputs * L + M - 1) / M); }
    /** How many outputs the next process() call of n samples gives. */
    int outputsFor (int n) const noexcept
    {
        const juce::int64 end = (juce::int64) n * L;
        return end > acc ? (int) ((end - acc + M - 1) / M) : 0;
    }
    /** Starts a channel again from silence (it was not being converted for a while: its memory is old). */
    void clearChannel (int c) noexcept
    {
        if (c < 0 || c >= channels) return;
        std::fill (hist[(size_t) c].begin(), hist[(size_t) c].end(), 0.0f);
        quiet[(size_t) c] = taps;
    }

    /** Converts n (<= maxInput) input samples of the first nch channels and writes the outputs from out[c][0]; returns how many outputs (the same for every
        channel, see outputsFor). in[c] == nullptr is silence. active (may be null): a channel whose flag is false is skipped altogether - nothing is
        written to its output and its memory is not kept up (clearChannel it before using it again). (char rather than bool: see RateBridge::wanted.) */
    int process (const float* const* in, int n, float* const* out, int nch, const char* active = nullptr) noexcept
    {
        if (! ready || n <= 0) return 0;
        n = juce::jmin (n, maxIn);
        const int count = outputsFor (n);
        nch = juce::jmin (nch, channels);
        for (int c = 0; c < nch; ++c)
        {
            if (active != nullptr && active[c] == 0) continue;
            float* h = hist[(size_t) c].data();
            const float* src = in != nullptr ? in[c] : nullptr;
            bool silent = src == nullptr;
            if (! silent) { const auto mm = juce::FloatVectorOperations::findMinAndMax (src, n); silent = mm.getStart() == 0.0f && mm.getEnd() == 0.0f; }
            if (silent && quiet[(size_t) c] >= taps - 1)                          // silence in, and the memory is silent: silence out, no work
            {
                if (out[c] != nullptr && count > 0) juce::FloatVectorOperations::clear (out[c], count);
                continue;
            }
            if (src != nullptr) std::memcpy (h + taps - 1, src, sizeof (float) * (size_t) n);
            else std::memset (h + taps - 1, 0, sizeof (float) * (size_t) n);
            if (out[c] != nullptr)
            {
                juce::int64 a = acc;
                float* o = out[c];
                for (int j = 0; j < count; ++j, a += M)
                {
                    const int base = (int) (a / L), ph = (int) (a % L);
                    const float* x = h + base;                                     // the newest input this output uses is x[taps - 1]
                    const float* cf = coef.data() + (size_t) ph * (size_t) taps;
                    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
                    for (int i = 0; i < taps; i += 4)
                    {
                        s0 += x[i] * cf[i]; s1 += x[i + 1] * cf[i + 1]; s2 += x[i + 2] * cf[i + 2]; s3 += x[i + 3] * cf[i + 3];
                    }
                    o[j] = (s0 + s1) + (s2 + s3);
                }
            }
            std::memmove (h, h + n, sizeof (float) * (size_t) (taps - 1));       // the last taps - 1 inputs are kept for the next call
            quiet[(size_t) c] = silent ? juce::jmin (taps, quiet[(size_t) c] + n) : 0;
        }
        acc += (juce::int64) count * M - (juce::int64) n * L;
        return count;
    }

private:
    static double bessel0 (double x) noexcept
    {
        double sum = 1.0, term = 1.0; const double q = x * x * 0.25;
        for (int k = 1; k < 80; ++k) { term *= q / ((double) k * k); sum += term; if (term < sum * 1.0e-17) break; }
        return sum;
    }
    bool ready = false;
    double rateIn = 48000.0, rateOut = 48000.0;
    int L = 1, M = 1, taps = 8, maxIn = 0, channels = 0;
    std::vector<float> coef;                         // [phase][tap], oldest input first
    std::vector<std::vector<float>> hist;            // per channel: the last taps - 1 inputs, then room for a block
    std::vector<int> quiet;                          // per channel: how many of the newest inputs were digital silence (at most taps)
    juce::int64 acc = 0;                             // where the next output falls, in 1/up input samples after the start of the next block (0 <= acc < down)
};

/** The engine and the device at two rates (3.8.0): every block the device asks for, the device's inputs are converted to the project's rate, the engine
    renders as much at the project's rate as the outputs need, and that is converted to the device's rate. The engine's blocks then vary by a sample or
    so (always within engineBlock()); for 96 kHz on a 48 kHz device they are exactly twice the device's. Real-time safe once prepared. */
class RateBridge
{
public:
    /** projectRate: what the engine runs at; deviceRate: what the device runs at; inputs / outputs: the device's channels; deviceBlock: the largest block
        expected from the device (a bigger one is done in pieces). False when the two rates cannot be bridged. */
    bool prepare (double projectRate, double deviceRate, int inputs, int outputs, int deviceBlock)
    {
        ok = false;
        rateProject = projectRate; rateDevice = deviceRate;
        devBlock = juce::jmax (16, deviceBlock);
        numIn = juce::jlimit (0, kMaxInputs, inputs); numOut = juce::jlimit (0, kMaxInputs, outputs);
        int up = 1, down = 1;
        if (! RateConverter::ratio (projectRate, deviceRate, up, down)) return false;
        engBlock = (int) (((juce::int64) devBlock * down - 1) / up + 1);                       // the most the engine is asked for (see RateConverter::maxInputFor)
        if (! fromEngine.prepare (projectRate, deviceRate, numOut, engBlock)) return false;
        if (! toEngine.prepare (deviceRate, projectRate, numIn, devBlock)) return false;
        const int perPiece = toEngine.maxOutputFor (devBlock);
        prime = 2;                                                                            // (one sample is always enough: see the 3.8.0 notes)
        inCap = prime + perPiece + engBlock + 16;
        outCap = devBlock + fromEngine.maxOutputFor (1) + 16;
        inFifo.assign ((size_t) numIn, std::vector<float> ((size_t) inCap, 0.0f));
        outFifo.assign ((size_t) numOut, std::vector<float> ((size_t) outCap, 0.0f));
        engOut.assign ((size_t) numOut, std::vector<float> ((size_t) engBlock, 0.0f));
        srcPtr.assign ((size_t) juce::jmax (1, numIn), nullptr); dstPtr.assign ((size_t) juce::jmax (1, juce::jmax (numIn, numOut)), nullptr);
        engInPtr.assign ((size_t) juce::jmax (1, numIn), nullptr); engOutPtr.assign ((size_t) juce::jmax (1, numOut), nullptr);
        wanted.assign ((size_t) juce::jmax (1, numIn), (char) 0);
        active.assign ((size_t) juce::jmax (1, numIn), (char) 0);
        inCount = prime; outCount = 0; shortfalls = 0;
        ok = true;
        return true;
    }
    double projectRate() const noexcept { return rateProject; }
    double deviceRate() const noexcept { return rateDevice; }
    /** The largest block the engine is ever asked to render (prepare it for this). */
    int engineBlock() const noexcept { return engBlock; }
    /** How much later the outputs sound than they would without the bridge (seconds). */
    double outputDelaySeconds() const noexcept { return fromEngine.delaySeconds(); }
    /** How much later the inputs reach the engine (seconds). */
    double inputDelaySeconds() const noexcept { return toEngine.delaySeconds() + (double) prime / juce::jmax (1.0, rateProject); }
    /** Times the engine had to be given silence because the converted inputs had not arrived (never happens: a test watches it). */
    int inputShortfalls() const noexcept { return shortfalls; }
    int inputFifoFill() const noexcept { return inCount; }
    int outputFifoFill() const noexcept { return outCount; }

    /** One device block. wantIn (may be null = all): per device input, whether the engine uses it (an input nothing listens to is not converted; the
        engine gets nullptr for it). render (const float* const* in, int numIn, float* const* out, int numOut, int n): the engine at the project rate. */
    template <typename Render>
    void run (const float* const* in, int nIn, float* const* out, int nOut, int n, const bool* wantIn, Render&& render) noexcept
    {
        if (! ok) { for (int c = 0; c < nOut; ++c) if (out[c] != nullptr) juce::FloatVectorOperations::clear (out[c], n); return; }
        const int ins = juce::jmin (nIn, numIn), outs = juce::jmin (nOut, numOut);
        for (int c = outs; c < nOut; ++c) if (out[c] != nullptr) juce::FloatVectorOperations::clear (out[c], n);     // (channels the bridge was not made for)
        for (int c = 0; c < ins; ++c)
        {
            const bool want = (wantIn == nullptr || wantIn[c]) && in[c] != nullptr;
            if (want && ! wanted[(size_t) c])                                              // it was not being converted: it starts again from silence
            {
                toEngine.clearChannel (c);
                std::memset (inFifo[(size_t) c].data(), 0, sizeof (float) * (size_t) inCount);
            }
            wanted[(size_t) c] = (char) want;
            active[(size_t) c] = (char) want;
        }
        int done = 0;
        while (done < n)
        {
            const int piece = juce::jmin (devBlock, n - done);
            // 1. the inputs, at the device's rate, converted onto the end of the input queue
            for (int c = 0; c < ins; ++c) { srcPtr[(size_t) c] = wanted[(size_t) c] ? in[c] + done : nullptr; dstPtr[(size_t) c] = inFifo[(size_t) c].data() + inCount; }
            const int made = toEngine.process (srcPtr.data(), piece, dstPtr.data(), ins, active.data());
            inCount += made;
            // 2. the engine renders what the outputs need
            const int need = piece - outCount;
            if (need > 0)
            {
                const int K = fromEngine.inputFor (need);
                if (inCount < K)                                                          // (cannot happen: the inputs always arrive in time)
                {
                    const int gap = K - inCount;
                    for (int c = 0; c < ins; ++c)
                    {
                        if (! wanted[(size_t) c]) continue;
                        float* f = inFifo[(size_t) c].data();
                        std::memmove (f + gap, f, sizeof (float) * (size_t) inCount);
                        std::memset (f, 0, sizeof (float) * (size_t) gap);
                    }
                    inCount = K; ++shortfalls;
                }
                for (int c = 0; c < ins; ++c) engInPtr[(size_t) c] = wanted[(size_t) c] ? inFifo[(size_t) c].data() : nullptr;
                for (int c = 0; c < outs; ++c) engOutPtr[(size_t) c] = engOut[(size_t) c].data();
                render (static_cast<const float* const*> (engInPtr.data()), ins, engOutPtr.data(), outs, K);
                for (int c = 0; c < ins; ++c)
                    if (wanted[(size_t) c] && inCount > K) std::memmove (inFifo[(size_t) c].data(), inFifo[(size_t) c].data() + K, sizeof (float) * (size_t) (inCount - K));
                inCount -= K;
                for (int c = 0; c < outs; ++c) dstPtr[(size_t) c] = outFifo[(size_t) c].data() + outCount;
                outCount += fromEngine.process (engOutPtr.data(), K, dstPtr.data(), outs);
            }
            // 3. one piece to the device
            const int give = juce::jmin (piece, outCount);
            for (int c = 0; c < outs; ++c)
            {
                float* f = outFifo[(size_t) c].data();
                if (out[c] != nullptr)
                {
                    std::memcpy (out[c] + done, f, sizeof (float) * (size_t) give);
                    if (give < piece) std::memset (out[c] + done + give, 0, sizeof (float) * (size_t) (piece - give));
                }
                if (outCount > give) std::memmove (f, f + give, sizeof (float) * (size_t) (outCount - give));
            }
            outCount -= give;
            done += piece;
        }
    }

private:
    bool ok = false;
    double rateProject = 48000.0, rateDevice = 48000.0;
    int devBlock = 512, engBlock = 512, numIn = 0, numOut = 0, prime = 2, inCap = 0, outCap = 0;
    RateConverter toEngine, fromEngine;
    std::vector<std::vector<float>> inFifo, outFifo, engOut;     // per channel: inputs at the project rate waiting for the engine; outputs at the device rate waiting for the device; the engine's outputs
    int inCount = 0, outCount = 0, shortfalls = 0;               // (the same count for every channel)
    std::vector<const float*> srcPtr, engInPtr;
    std::vector<float*> dstPtr, engOutPtr;
    std::vector<char> wanted, active;                           // per input: converted in this block (char, not bool: a vector<bool> has no data())
};
} // namespace td
