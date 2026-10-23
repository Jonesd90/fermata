#pragma once
#include "Common.h"
#include <array>
#include <vector>

namespace td
{
/** The pitch pipe's sound: a soft free reed, like a real pitch pipe (a harmonica reed blown gently). Every note is a band-limited wavetable read at
    its exact frequency (the pitch must be true: no vibrato, no drift), with a gentle 45 ms swell, a little breath noise and a "chiff" as the reed
    speaks, and a 150 ms release. Up to kSlots notes at once (a chord); the message thread turns slots on and off, the audio thread renders.
    Never recorded: the engine adds it to the outputs chosen for it, after the mixers. */
class PitchPipeVoices
{
public:
    static constexpr int kSlots = 48;
    static constexpr int kBands = 10;                       // wavetables per octave of fundamental, from C1 up
    static constexpr int kTableSize = 4096;

    /** Builds the wavetables for this sample rate (allocates: not while the audio thread renders). */
    void prepare (double sampleRate)
    {
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        tables.assign ((size_t) kBands, std::vector<float> ((size_t) kTableSize + 1, 0.0f));
        for (int b = 0; b < kBands; ++b)
        {
            const double fTop = 32.703 * std::pow (2.0, (double) b + 1.0);            // the highest fundamental this table is used for
            const double fMid = fTop / std::sqrt (2.0);
            const int harmonics = juce::jlimit (1, 96, (int) std::floor (0.45 * sr / fTop));
            auto& t = tables[(size_t) b];
            std::vector<double> acc ((size_t) kTableSize, 0.0);
            for (int h = 1; h <= harmonics; ++h)
            {
                const double f = fMid * h;
                double a = std::pow ((double) h, -1.05) * (h % 2 == 1 ? 1.0 : 0.82);   // a free reed: every harmonic, the odd ones a little stronger
                a /= std::sqrt (1.0 + std::pow (f / 3800.0, 4.0));                    // soft above ~4 kHz: a gentle pipe, not a buzz
                const double ph = 0.71 * (double) (h * h);                              // spread phases (a lower peak for the same loudness)
                for (int i = 0; i < kTableSize; ++i)
                    acc[(size_t) i] += a * std::sin (juce::MathConstants<double>::twoPi * (double) h * (double) i / (double) kTableSize + ph);
            }
            double peak = 1.0e-9;
            for (double v : acc) peak = juce::jmax (peak, std::abs (v));
            for (int i = 0; i < kTableSize; ++i) t[(size_t) i] = (float) (acc[(size_t) i] / peak);
            t[(size_t) kTableSize] = t[0];                                               // (the guard point for the interpolation)
        }
        attackStep = 1.0 / (0.045 * sr);
        releaseStep = 1.0 / (0.150 * sr);
        chiffDecay = std::exp (-1.0 / (0.025 * sr));
        hpA = std::exp (-juce::MathConstants<double>::twoPi * 900.0 / sr);
        lpA = std::exp (-juce::MathConstants<double>::twoPi * 6000.0 / sr);
        for (auto& v : voices) v = Voice();
        prepared.store (true);
    }

    // ---- message thread
    /** Turns a slot on at this frequency (a sounding slot moves to the new pitch), or off (it fades out). */
    void setSlot (int slot, double hz, bool on) noexcept
    {
        if (! juce::isPositiveAndBelow (slot, kSlots)) return;
        slotHz[(size_t) slot].store ((float) hz, std::memory_order_relaxed);
        slotOn[(size_t) slot].store (on && hz > 0.0, std::memory_order_release);
    }
    void allOff() noexcept { for (auto& s : slotOn) s.store (false, std::memory_order_release); }
    void setLevelDb (float db) noexcept { level.store (db <= -60.0f ? 0.0f : juce::Decibels::decibelsToGain (db), std::memory_order_relaxed); }
    /** True while any note is sounding (including a release that is still dying away). */
    bool sounding() const noexcept { return soundingFlag.load (std::memory_order_relaxed); }

    // ---- audio thread
    /** Renders n samples into dst (overwrites it). False (and dst untouched) when nothing is sounding. */
    bool render (float* dst, int n) noexcept
    {
        if (! prepared.load (std::memory_order_acquire) || tables.empty()) return false;
        int active = 0;
        for (int s = 0; s < kSlots; ++s)
        {
            auto& v = voices[(size_t) s];
            const bool on = slotOn[(size_t) s].load (std::memory_order_acquire);
            const double hz = slotHz[(size_t) s].load (std::memory_order_relaxed);
            if (on && ! v.gate)
            {
                if (! v.active) { v.phase = 0.0; v.env = 0.0; v.chiff = 1.0; v.hp = v.lp = v.lastNoise = 0.0; }
                v.gate = true; v.active = true;
            }
            else if (! on && v.gate) v.gate = false;
            if (v.active && hz > 0.0)
            {
                v.inc = hz / sr;
                v.table = tableFor (hz);
            }
            if (v.active) ++active;
        }
        soundingFlag.store (active > 0, std::memory_order_relaxed);
        if (active == 0) return false;
        juce::FloatVectorOperations::clear (dst, n);
        const double gain = level.load (std::memory_order_relaxed) / std::sqrt ((double) juce::jmax (1, active));
        smoothGain = smoothGain <= 0.0 ? gain : smoothGain;
        const double gStep = (gain - smoothGain) / (double) juce::jmax (1, n);
        for (int s = 0; s < kSlots; ++s)
        {
            auto& v = voices[(size_t) s];
            if (! v.active || v.table == nullptr) continue;
            const float* t = v.table;
            double g = smoothGain;
            for (int i = 0; i < n; ++i)
            {
                // the envelope: a raised-cosine swell while the gate is open, the same curve back down after it closes (a release that starts
                // half way through the swell carries on from where it was: no step)
                if (v.gate) { v.env = juce::jmin (1.0, v.env + attackStep); }
                else        { v.env -= releaseStep; if (v.env <= 0.0) { v.env = 0.0; v.active = false; break; } }
                const double shaped = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * v.env);
                // the reed
                const double pos = v.phase * (double) kTableSize;
                const int i0 = (int) pos;
                const double fr = pos - (double) i0;
                const double tone = (double) t[i0] + fr * ((double) t[i0 + 1] - (double) t[i0]);
                v.phase += v.inc; if (v.phase >= 1.0) v.phase -= 1.0;
                // breath: band-passed noise, stronger for a moment as the reed speaks
                rng = rng * 1664525u + 1013904223u;
                const double white = (double) (int) (rng >> 8) / 8388608.0 - 1.0;
                v.hp = hpA * (v.hp + white - v.lastNoise); v.lastNoise = white;
                v.lp = (1.0 - lpA) * v.hp + lpA * v.lp;
                const double breath = v.lp * (0.035 + 0.22 * v.chiff);
                v.chiff *= chiffDecay;
                dst[i] += (float) ((tone + breath) * shaped * g);
                g += gStep;
            }
        }
        smoothGain = gain;
        for (int i = 0; i < n; ++i)                                      // a soft ceiling: a big chord at a high level never clips
        {
            const float x = dst[i];
            if (std::abs (x) > 0.8f) dst[i] = (x > 0.0f ? 1.0f : -1.0f) * (0.8f + 0.2f * std::tanh ((std::abs (x) - 0.8f) / 0.2f));
        }
        return true;
    }

private:
    struct Voice
    {
        bool active = false, gate = false;
        double phase = 0.0, inc = 0.0, env = 0.0, chiff = 0.0, hp = 0.0, lp = 0.0, lastNoise = 0.0;
        const float* table = nullptr;
    };
    const float* tableFor (double hz) const noexcept
    {
        int b = (int) std::ceil (std::log2 (juce::jmax (1.0, hz) / 32.703)) - 1;
        b = juce::jlimit (0, kBands - 1, b);
        while (b < kBands - 1 && hz > 32.703 * std::pow (2.0, (double) b + 1.0)) ++b;
        return tables[(size_t) b].data();
    }

    double sr = 48000.0, attackStep = 0.001, releaseStep = 0.001, chiffDecay = 0.99, hpA = 0.9, lpA = 0.5, smoothGain = 0.0;
    std::vector<std::vector<float>> tables;
    std::array<Voice, kSlots> voices {};
    std::array<std::atomic<float>, kSlots> slotHz {};
    std::array<std::atomic<bool>, kSlots> slotOn {};
    std::atomic<float> level { 0.2f };
    std::atomic<bool> prepared { false }, soundingFlag { false };
    juce::uint32 rng = 22222u;
};

/** The click: a steady beat at a tempo, the first beat of every bar accented (if wanted), in one of four sounds. It runs when switched on and stops
    when switched off (it is not tied to Play or Record). The hits are rendered once for the sample rate and mixed in sample-exactly. */
class ClickTrack
{
public:
    enum Sound : int { Click = 0, Woodblock, Soft, LowPulse, NumSounds };
    static const char* soundName (int s)
    {
        static const char* n[] = { "Click", "Woodblock", "Soft", "Low pulse" };
        return juce::isPositiveAndBelow (s, (int) NumSounds) ? n[s] : "";
    }
    static const char* soundInfo (int s)
    {
        static const char* n[] = { "A short, sharp tick: the usual metronome.",
                                   "A wooden knock: clear without being hard.",
                                   "A rounded \"bum\" with a slight swell into it: easier to play with than a hard tick.",
                                   "A low thud, felt as much as heard." };
        return juce::isPositiveAndBelow (s, (int) NumSounds) ? n[s] : "";
    }

    /** Renders the hits for this sample rate (allocates: not while the audio thread renders). */
    void prepare (double sampleRate)
    {
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        for (int s = 0; s < NumSounds; ++s)
            for (int a = 0; a < 2; ++a) hits[(size_t) s][(size_t) a] = makeHit (s, a == 1, sr);
        for (auto& v : playing) v = Playing();
        toNext = 0.0; beat = 0;
        prepared.store (true);
    }

    // ---- message thread
    void setRunning (bool on) noexcept
    {
        if (on && ! runFlag.load()) restart.store (true, std::memory_order_relaxed);
        runFlag.store (on, std::memory_order_release);
    }
    bool running() const noexcept { return runFlag.load (std::memory_order_relaxed); }
    void setTempo (double bpm) noexcept { tempo.store (juce::jlimit (20.0, 400.0, bpm), std::memory_order_relaxed); }
    void setBeatsPerBar (int b) noexcept { beats.store (juce::jlimit (1, 32, b), std::memory_order_relaxed); }
    void setAccent (bool on) noexcept { accent.store (on, std::memory_order_relaxed); }
    void setSound (int s) noexcept { sound.store (juce::jlimit (0, (int) NumSounds - 1, s), std::memory_order_relaxed); }
    void setLevelDb (float db) noexcept { level.store (db <= -60.0f ? 0.0f : juce::Decibels::decibelsToGain (db), std::memory_order_relaxed); }
    /** Goes up by one at every beat (for a flashing light); beatInBar() is the beat it was (0 = the first of the bar). */
    int beatCount() const noexcept { return beatCounter.load (std::memory_order_relaxed); }
    int beatInBar() const noexcept { return lastBeat.load (std::memory_order_relaxed); }

    // ---- audio thread
    /** Renders n samples into dst (overwrites it). False (dst untouched) when nothing sounds. */
    bool render (float* dst, int n) noexcept
    {
        if (! prepared.load (std::memory_order_acquire)) return false;
        const bool on = runFlag.load (std::memory_order_acquire);
        if (restart.exchange (false, std::memory_order_relaxed)) { toNext = 0.0; beat = 0; }
        bool any = false;
        for (auto& p : playing) any = any || p.data != nullptr;
        if (! on && ! any) return false;
        juce::FloatVectorOperations::clear (dst, n);
        const double interval = sr * 60.0 / tempo.load (std::memory_order_relaxed);
        const int perBar = beats.load (std::memory_order_relaxed);
        const bool acc = accent.load (std::memory_order_relaxed);
        const int snd = sound.load (std::memory_order_relaxed);
        const float g = level.load (std::memory_order_relaxed);
        if (on) toNext = juce::jmin (toNext, interval);                 // (a faster tempo takes effect at the next beat)
        int start = 0;
        while (start < n)
        {
            int until = n;
            if (on && toNext < (double) (n - start)) until = start + juce::jmax (0, (int) std::ceil (toNext));
            mix (dst, start, until - start, g);
            if (on) toNext -= (double) (until - start);
            start = until;
            if (on && start < n && toNext <= 0.0)
            {
                if (beat >= perBar) beat = 0;
                const bool accented = acc && beat == 0 && perBar > 1;
                trigger (snd, accented);
                lastBeat.store (beat, std::memory_order_relaxed); beatCounter.fetch_add (1, std::memory_order_relaxed);
                beat = (beat + 1) % juce::jmax (1, perBar);
                toNext += interval;
            }
        }
        return true;
    }

    /** One hit (exposed for the tests): the sound, plain or accented, at this rate. */
    static std::vector<float> makeHit (int s, bool accented, double rate)
    {
        const double len = s == Click ? 0.05 : s == Woodblock ? 0.18 : 0.42;
        const int n = juce::jmax (8, (int) std::lround (len * rate));
        std::vector<double> x ((size_t) n, 0.0);
        juce::uint32 rng = 12345u + (juce::uint32) s * 77u + (accented ? 5u : 0u);
        auto noise = [&rng] { rng = rng * 1664525u + 1013904223u; return (double) (int) (rng >> 8) / 8388608.0 - 1.0; };
        const double twoPi = juce::MathConstants<double>::twoPi;
        switch (s)
        {
            case Click:
            {
                const double f = accented ? 2600.0 : 1800.0, tau = accented ? 0.005 : 0.004;
                for (int i = 0; i < n; ++i) { const double t = i / rate; x[(size_t) i] = std::sin (twoPi * f * t) * std::exp (-t / tau) + (t < 0.0006 ? 0.5 * noise() : 0.0); }
                break;
            }
            case Woodblock:
            {
                const double k = accented ? 1.26 : 1.0;                                   // the accent a major third higher
                const double fr[] = { 820.0 * k, 2160.0 * k, 3400.0 * k }, tau[] = { 0.030, 0.012, 0.006 }, amp[] = { 1.0, 0.5, 0.25 };
                double lp = 0.0;
                for (int i = 0; i < n; ++i)
                {
                    const double t = i / rate;
                    double v = 0.0;
                    for (int m = 0; m < 3; ++m) v += amp[m] * std::sin (twoPi * fr[m] * t) * std::exp (-t / tau[m]);
                    if (t < 0.0008) { lp += 0.35 * (noise() - lp); v += 0.6 * lp; }       // the knock of the stick
                    x[(size_t) i] = v;
                }
                break;
            }
            case Soft:
            {
                // "bum": a rounded low-mid note with a breath of air at its front and a short swell (22 ms) before it blooms and fades
                const double f = accented ? 262.0 : 196.0;
                const double h[] = { 1.0, 0.45, 0.18, 0.08 };
                double lp = 0.0;
                for (int i = 0; i < n; ++i)
                {
                    const double t = i / rate;
                    const double rise = t < 0.022 ? 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * t / 0.022) : 1.0;
                    double v = 0.0;
                    for (int m = 0; m < 4; ++m) v += h[m] * std::sin (twoPi * f * (m + 1) * t);
                    v *= rise * std::exp (-juce::jmax (0.0, t - 0.022) / 0.11);
                    lp += 0.12 * (noise() - lp);                                             // the "f" of "fum": soft, dark air
                    const double air = t < 0.03 ? std::sin (juce::MathConstants<double>::pi * t / 0.03) : 0.0;
                    x[(size_t) i] = v + 0.5 * air * lp;
                }
                break;
            }
            default:     // LowPulse
            {
                const double f = accented ? 90.0 : 70.0;
                for (int i = 0; i < n; ++i)
                {
                    const double t = i / rate;
                    const double env = (t < 0.006 ? t / 0.006 : 1.0) * std::exp (-juce::jmax (0.0, t - 0.006) / 0.13);
                    x[(size_t) i] = env * (std::sin (twoPi * f * t) + 0.35 * std::sin (twoPi * 2.0 * f * t) + 0.12 * std::sin (twoPi * 3.0 * f * t));
                }
                break;
            }
        }
        const int fadeN = juce::jmin (n / 4, (int) (0.01 * rate));                          // the last 10 ms fade to nothing (no click at the end of the click)
        for (int i = 0; i < fadeN; ++i) x[(size_t) (n - 1 - i)] *= (double) i / (double) fadeN;
        double peak = 1.0e-9; for (double v : x) peak = juce::jmax (peak, std::abs (v));
        const double target = accented ? 1.0 : 0.72;
        std::vector<float> out ((size_t) n);
        for (int i = 0; i < n; ++i) out[(size_t) i] = (float) (x[(size_t) i] / peak * target);
        return out;
    }

private:
    struct Playing { const float* data = nullptr; int len = 0, pos = 0; };
    void trigger (int snd, bool accented) noexcept
    {
        const auto& h = hits[(size_t) snd][accented ? 1u : 0u];
        Playing* slot = &playing[0];
        for (auto& p : playing) if (p.data == nullptr) { slot = &p; break; }
        slot->data = h.data(); slot->len = (int) h.size(); slot->pos = 0;
    }
    void mix (float* dst, int from, int count, float g) noexcept
    {
        if (count <= 0) return;
        for (auto& p : playing)
        {
            if (p.data == nullptr) continue;
            const int m = juce::jmin (count, p.len - p.pos);
            for (int i = 0; i < m; ++i) dst[from + i] += p.data[p.pos + i] * g;
            p.pos += m;
            if (p.pos >= p.len) p = Playing();
        }
    }

    double sr = 48000.0, toNext = 0.0;
    int beat = 0;
    std::array<std::array<std::vector<float>, 2>, NumSounds> hits;
    std::array<Playing, 4> playing {};
    std::atomic<bool> runFlag { false }, restart { false }, accent { true }, prepared { false };
    std::atomic<double> tempo { 120.0 };
    std::atomic<int> beats { 4 }, sound { (int) Soft }, beatCounter { 0 }, lastBeat { 0 };
    std::atomic<float> level { 0.25f };
};

/** The two sources together, as the engine adds them to the driver outputs: each to the outputs ticked for it, after everything else (they are
    heard, never recorded, and never reach a bounce). */
class AuxSounds
{
public:
    PitchPipeVoices pipe;
    ClickTrack click;

    void prepare (double sampleRate, int maxBlock)
    {
        pipe.prepare (sampleRate); click.prepare (sampleRate);
        bufPipe.assign ((size_t) juce::jmax (1, maxBlock), 0.0f); bufClick.assign ((size_t) juce::jmax (1, maxBlock), 0.0f);
    }
    /** The outputs (0-based driver output channels) each source goes to. */
    void setPipeOutputs (const std::vector<int>& outs) noexcept  { setMask (pipeTo, outs); }
    void setClickOutputs (const std::vector<int>& outs) noexcept { setMask (clickTo, outs); }
    bool pipeGoesTo (int ch) const noexcept  { return juce::isPositiveAndBelow (ch, kMaxInputs) && pipeTo[(size_t) ch].load (std::memory_order_relaxed); }
    bool clickGoesTo (int ch) const noexcept { return juce::isPositiveAndBelow (ch, kMaxInputs) && clickTo[(size_t) ch].load (std::memory_order_relaxed); }

    /** Audio thread: adds both sources to the outputs. */
    void process (float* const* out, int numOut, int n) noexcept
    {
        if (n > (int) bufPipe.size()) return;
        if (pipe.render (bufPipe.data(), n))
            for (int c = 0; c < numOut && c < kMaxInputs; ++c)
                if (out[c] != nullptr && pipeTo[(size_t) c].load (std::memory_order_relaxed)) juce::FloatVectorOperations::add (out[c], bufPipe.data(), n);
        if (click.render (bufClick.data(), n))
            for (int c = 0; c < numOut && c < kMaxInputs; ++c)
                if (out[c] != nullptr && clickTo[(size_t) c].load (std::memory_order_relaxed)) juce::FloatVectorOperations::add (out[c], bufClick.data(), n);
    }

private:
    static void setMask (std::array<std::atomic<bool>, kMaxInputs>& m, const std::vector<int>& outs) noexcept
    {
        bool want[kMaxInputs] = {};
        for (int o : outs) if (juce::isPositiveAndBelow (o, kMaxInputs)) want[o] = true;
        for (int c = 0; c < kMaxInputs; ++c) m[(size_t) c].store (want[c], std::memory_order_relaxed);
    }
    std::array<std::atomic<bool>, kMaxInputs> pipeTo {}, clickTo {};
    std::vector<float> bufPipe, bufClick;
};
} // namespace td
