#pragma once
#include "Common.h"
#include <array>
#include <vector>

namespace td
{
/** The pitch pipe's sound: a soft free reed, like a real pitch pipe (a harmonica reed blown gently). Every note is a band-limited wavetable read at
    its exact frequency (the pitch must be true: no vibrato, no drift), softened by a low-pass that follows the note's own pitch (the Tone setting),
    with a gentle 70 ms swell, a whisper of breath and a small "chiff" as the reed speaks, and a 150 ms release. Up to kSlots notes at once (a chord);
    the message thread turns slots on and off, the audio thread renders.

    THE PITCH NEVER MOVES BY ITSELF. A note takes its frequency when it starts and keeps it through the swell, the hold and the whole fade-out: letting
    go of a key (or of the chord's Play) cannot change it, whatever frequency the caller passes with the "off". Only a deliberate retune of a note that
    is still held (another tuning, a changed chord) moves it, and then in a smooth 50 ms glide that ends exactly on the new pitch.

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
        attackStep = 1.0 / (0.070 * sr);
        releaseStep = 1.0 / (0.150 * sr);
        glideSamples = 0.050 * sr;
        chiffDecay = std::exp (-1.0 / (0.025 * sr));
        hpA = std::exp (-juce::MathConstants<double>::twoPi * 700.0 / sr);
        lpA = std::exp (-juce::MathConstants<double>::twoPi * 3500.0 / sr);
        for (auto& v : voices) v = Voice();
        prepared.store (true);
    }

    // ---- message thread
    /** Turns a slot on at this frequency (a sounding slot glides to the new pitch), or off (it fades out on the pitch it has: the frequency is only
        looked at while the slot is on). */
    void setSlot (int slot, double hz, bool on) noexcept
    {
        if (! juce::isPositiveAndBelow (slot, kSlots)) return;
        const bool go = on && hz > 0.0;
        if (go) slotHz[(size_t) slot].store ((float) hz, std::memory_order_relaxed);
        slotOn[(size_t) slot].store (go, std::memory_order_release);
    }
    void allOff() noexcept { for (auto& s : slotOn) s.store (false, std::memory_order_release); }
    void setLevelDb (float db) noexcept { level.store (db <= -60.0f ? 0.0f : juce::Decibels::decibelsToGain (db), std::memory_order_relaxed); }
    /** The tone, 0 (soft: little above the note's first few harmonics) to 1 (bright: the reed as it comes). */
    void setTone (float t) noexcept { tone.store (juce::jlimit (0.0f, 1.0f, t), std::memory_order_relaxed); }
    /** True while any note is sounding (including a release that is still dying away). */
    bool sounding() const noexcept { return soundingFlag.load (std::memory_order_relaxed); }
    /** The frequency a slot is sounding at now (for the tests; read it only when the audio thread is not rendering). */
    double debugHz (int slot) const noexcept { return juce::isPositiveAndBelow (slot, kSlots) ? voices[(size_t) slot].inc * sr : 0.0; }

    /** How much of a reed's loudness the tone filter keeps when its corner is 'k' times the note's pitch (so that a softer tone is not a quieter one):
        the harmonic series 1/h^1.05 (the odd ones a little stronger) through two equal one-pole low-passes, RMS with and without. */
    static double toneKeep (double k) noexcept
    {
        double num = 0.0, den = 0.0;
        for (int h = 1; h <= 60; ++h)
        {
            const double a = std::pow ((double) h, -1.05) * (h % 2 == 1 ? 1.0 : 0.82);
            const double x = (double) h * 0.644 / k;
            const double g = 1.0 / (1.0 + x * x);                                 // |H|^2 of two equal poles = 1 / (1 + x^2)^2 ; amplitude 1 / (1 + x^2)
            num += a * a * g * g; den += a * a;
        }
        return std::sqrt (num / den);
    }
    /** The filter corner, in multiples of a note's pitch, for a tone setting. */
    static double toneCorner (double t) noexcept { return std::pow (2.0, 1.0 + 3.5 * juce::jlimit (0.0, 1.0, t)); }

    // ---- audio thread
    /** Renders n samples into dst (overwrites it). False (and dst untouched) when nothing is sounding. */
    bool render (float* dst, int n) noexcept
    {
        if (! prepared.load (std::memory_order_acquire) || tables.empty()) return false;
        const double kCorner = toneCorner (tone.load (std::memory_order_relaxed));
        const double comp = juce::jmin (3.0, 1.0 / juce::jmax (0.3, toneKeep (kCorner)));
        int active = 0;
        for (int s = 0; s < kSlots; ++s)
        {
            auto& v = voices[(size_t) s];
            const bool on = slotOn[(size_t) s].load (std::memory_order_acquire);
            const double hz = slotHz[(size_t) s].load (std::memory_order_relaxed);
            if (on && ! v.gate)
            {
                if (! v.active) { v.phase = 0.0; v.env = 0.0; v.chiff = 1.0; v.hp = v.lp = v.lastNoise = 0.0; v.f1 = v.f2 = 0.0; v.targetHz = 0.0; v.glideLeft = 0; }
                v.gate = true; v.active = true;
            }
            else if (! on && v.gate) v.gate = false;
            // the pitch is taken only while the key is held: a note let go of keeps the pitch it had for its whole fade-out
            if (v.active && v.gate && hz > 0.0)
            {
                if (v.targetHz <= 0.0) { v.inc = v.incTo = hz / sr; v.glideLeft = 0; v.targetHz = hz; v.table = tableFor (hz); }          // a new note: at once
                else if (std::abs (hz - v.targetHz) > 1.0e-9 * hz)                                                                       // a held note retuned: a smooth glide
                {
                    v.incFrom = v.inc; v.incTo = hz / sr; v.glideLeft = juce::jmax (1, (int) glideSamples); v.glideLen = v.glideLeft; v.targetHz = hz; v.table = tableFor (hz);
                }
            }
            if (v.active)
            {
                ++active;
                const double fc = juce::jlimit (500.0, 0.45 * sr, v.inc * sr * kCorner) / 0.644;      // (two equal poles are 3 dB down at 0.644 of each one's frequency)
                v.pole = std::exp (-juce::MathConstants<double>::twoPi * fc / sr);
            }
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
                // a retune glides (smoothstep, 50 ms) and ends exactly on the new pitch
                if (v.glideLeft > 0)
                {
                    --v.glideLeft;
                    const double u = 1.0 - (double) v.glideLeft / (double) juce::jmax (1, v.glideLen);
                    v.inc = v.glideLeft == 0 ? v.incTo : v.incFrom + (v.incTo - v.incFrom) * (u * u * (3.0 - 2.0 * u));
                }
                // the reed
                const double pos = v.phase * (double) kTableSize;
                const int i0 = (int) pos;
                const double fr = pos - (double) i0;
                const double reed = (double) t[i0] + fr * ((double) t[i0 + 1] - (double) t[i0]);
                v.phase += v.inc; if (v.phase >= 1.0) v.phase -= 1.0;
                // the tone: two poles that follow the note's own pitch (the same softness at every pitch)
                v.f1 = v.pole * v.f1 + (1.0 - v.pole) * reed;
                v.f2 = v.pole * v.f2 + (1.0 - v.pole) * v.f1;
                // breath: band-passed noise, a little stronger for a moment as the reed speaks
                rng = rng * 1664525u + 1013904223u;
                const double white = (double) (int) (rng >> 8) / 8388608.0 - 1.0;
                v.hp = hpA * (v.hp + white - v.lastNoise); v.lastNoise = white;
                v.lp = (1.0 - lpA) * v.hp + lpA * v.lp;
                const double breath = v.lp * (0.014 + 0.09 * v.chiff);
                v.chiff *= chiffDecay;
                dst[i] += (float) ((v.f2 * comp + breath) * shaped * g);
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
        double f1 = 0.0, f2 = 0.0, pole = 0.5;                          // the tone filter
        double targetHz = 0.0, incFrom = 0.0, incTo = 0.0;              // the pitch it holds / is gliding to
        int glideLeft = 0, glideLen = 1;
        const float* table = nullptr;
    };
    const float* tableFor (double hz) const noexcept
    {
        int b = (int) std::ceil (std::log2 (juce::jmax (1.0, hz) / 32.703)) - 1;
        b = juce::jlimit (0, kBands - 1, b);
        while (b < kBands - 1 && hz > 32.703 * std::pow (2.0, (double) b + 1.0)) ++b;
        return tables[(size_t) b].data();
    }

    double sr = 48000.0, attackStep = 0.001, releaseStep = 0.001, glideSamples = 2400.0, chiffDecay = 0.99, hpA = 0.9, lpA = 0.5, smoothGain = 0.0;
    std::vector<std::vector<float>> tables;
    std::array<Voice, kSlots> voices {};
    std::array<std::atomic<float>, kSlots> slotHz {};
    std::array<std::atomic<bool>, kSlots> slotOn {};
    std::atomic<float> level { 0.2f }, tone { 0.35f };
    std::atomic<bool> prepared { false }, soundingFlag { false };
    juce::uint32 rng = 22222u;
};

/** The click: a steady beat at a tempo, the first beat of every bar accented (if wanted), in one of five sounds, with an optional very quiet hi-hat on
    every quaver. It runs when switched on and stops when switched off (it is not tied to Play or Record). The hits are rendered once for the sample
    rate and mixed in sample-exactly.

    Nothing it plays has a pitch to follow (a click for singers must never fight their tuning), except the Click, Woodblock and Low pulse accents,
    which are the sounds they have always been. */
class ClickTrack
{
public:
    enum Sound : int { Click = 0, Woodblock, Soft, LowPulse, DrumKit, NumSounds };
    enum Drum : int { Kick = 0, Snare, Hat, NumDrums };
    static const char* soundName (int s)
    {
        static const char* n[] = { "Click", "Woodblock", "Soft", "Low pulse", "Drum kit" };
        return juce::isPositiveAndBelow (s, (int) NumSounds) ? n[s] : "";
    }
    static const char* soundInfo (int s)
    {
        static const char* n[] = { "A short, sharp tick: the usual metronome.",
                                   "A wooden knock: clear without being hard.",
                                   "A soft, rounded \"fum\" with a slight swell into it and no note to follow: easy to play with, and it never fights the tuning.",
                                   "A low thud, felt as much as heard.",
                                   "A little kit with no pitched notes: kick on the first beat, snare in the middle of the bar, hi-hat on the others." };
        return juce::isPositiveAndBelow (s, (int) NumSounds) ? n[s] : "";
    }
    /** The drum kit's part for a beat of the bar (0-based): the kick on the first beat; the snare in the middle of a bar with an even number of
        four or more beats (beat 3 of 4, beat 4 of 6, beat 5 of 8); the hi-hat on all the others. */
    static int drumFor (int beat, int perBar) noexcept
    {
        if (beat == 0) return Kick;
        if (perBar >= 4 && perBar % 2 == 0 && beat == perBar / 2) return Snare;
        return Hat;
    }
    /** How many quaver ticks fall in a beat of this note value (beat unit 4 = a quarter note: 2; 2 = a half note: 4; 8 = an eighth: 1). A beat shorter
        than a quaver (16) has a tick on every second beat. */
    static int quaversPerBeat (int beatUnit) noexcept { return beatUnit <= 8 ? juce::jmax (1, 8 / juce::jmax (1, beatUnit)) : 1; }

    /** Renders the hits for this sample rate (allocates: not while the audio thread renders). */
    void prepare (double sampleRate)
    {
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        for (int s = 0; s < NumSounds; ++s)
            for (int a = 0; a < 2; ++a) hits[(size_t) s][(size_t) a] = s == DrumKit ? std::vector<float>() : makeHit (s, a == 1, sr);
        for (int d = 0; d < NumDrums; ++d) drums[(size_t) d] = makeDrum (d, sr);
        tickHit = makeTick (sr);
        for (auto& v : playing) v = Playing();
        toNext = 0.0; beat = 0; subIdx = 1; subInBeat = 1;
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
    void setBeatUnit (int u) noexcept { unit.store (juce::jlimit (1, 32, u), std::memory_order_relaxed); }
    void setAccent (bool on) noexcept { accent.store (on, std::memory_order_relaxed); }
    void setSound (int s) noexcept { sound.store (juce::jlimit (0, (int) NumSounds - 1, s), std::memory_order_relaxed); }
    void setLevelDb (float db) noexcept { level.store (db <= -60.0f ? 0.0f : juce::Decibels::decibelsToGain (db), std::memory_order_relaxed); }
    /** The quiet hi-hat on every quaver (any sound): on or off, and how far below the beat it sits. */
    void setQuaverTicks (bool on) noexcept { ticks.store (on, std::memory_order_relaxed); }
    void setTickDb (float db) noexcept { tickGain.store (juce::Decibels::decibelsToGain (juce::jlimit (-60.0f, 0.0f, db)), std::memory_order_relaxed); }
    /** Goes up by one at every beat (for a flashing light); beatInBar() is the beat it was (0 = the first of the bar). */
    int beatCount() const noexcept { return beatCounter.load (std::memory_order_relaxed); }
    int beatInBar() const noexcept { return lastBeat.load (std::memory_order_relaxed); }

    // ---- audio thread
    /** Renders n samples into dst (overwrites it). False (dst untouched) when nothing sounds. */
    bool render (float* dst, int n) noexcept
    {
        if (! prepared.load (std::memory_order_acquire)) return false;
        const bool on = runFlag.load (std::memory_order_acquire);
        if (restart.exchange (false, std::memory_order_relaxed)) { toNext = 0.0; beat = 0; subIdx = 1; subInBeat = 1; }
        bool any = false;
        for (auto& p : playing) any = any || p.data != nullptr;
        if (! on && ! any) return false;
        juce::FloatVectorOperations::clear (dst, n);
        const double interval = sr * 60.0 / tempo.load (std::memory_order_relaxed);
        const int perBar = beats.load (std::memory_order_relaxed);
        const int beatUnit = unit.load (std::memory_order_relaxed);
        const bool acc = accent.load (std::memory_order_relaxed);
        const bool quavers = ticks.load (std::memory_order_relaxed);
        const float tg = tickGain.load (std::memory_order_relaxed);
        const int snd = sound.load (std::memory_order_relaxed);
        const float g = level.load (std::memory_order_relaxed);
        if (on) toNext = juce::jmin (toNext, interval / (double) juce::jmax (1, subInBeat));        // (a faster tempo takes effect at the next click)
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
                if (subIdx >= subInBeat)                                                           // a beat
                {
                    subInBeat = quavers ? quaversPerBeat (beatUnit) : 1;
                    if (beat >= perBar) beat = 0;
                    if (snd == DrumKit)
                    {
                        const int d = drumFor (beat, perBar);
                        trigger (drums[(size_t) d], d == Kick ? (acc ? 1.0f : 0.8f) : d == Snare ? 0.8f : 0.5f);
                    }
                    else trigger (hits[(size_t) snd][acc && beat == 0 && perBar > 1 ? 1u : 0u], 1.0f);
                    if (quavers && (beatUnit <= 8 || beat % 2 == 0)) trigger (tickHit, tg);        // (the quaver that falls on the beat)
                    lastBeat.store (beat, std::memory_order_relaxed); beatCounter.fetch_add (1, std::memory_order_relaxed);
                    beat = (beat + 1) % juce::jmax (1, perBar);
                    subIdx = 1;
                }
                else { trigger (tickHit, tg); ++subIdx; }                                           // a quaver between the beats
                toNext += interval / (double) juce::jmax (1, subInBeat);
            }
        }
        return true;
    }

    /** One hit (exposed for the tests): the sound, plain or accented, at this rate. For the drum kit: the kick (accented) or the hi-hat (plain). */
    static std::vector<float> makeHit (int s, bool accented, double rate)
    {
        const double twoPi = juce::MathConstants<double>::twoPi, pi = juce::MathConstants<double>::pi;
        if (s == DrumKit) return finish (makeDrumRaw (accented ? Kick : Hat, rate), rate, accented ? 1.0 : 0.72);
        const double len = s == Click ? 0.05 : s == Woodblock ? 0.18 : 0.42;
        const int n = juce::jmax (8, (int) std::lround (len * rate));
        std::vector<double> x ((size_t) n, 0.0);
        juce::uint32 rng = 12345u + (juce::uint32) s * 77u + (accented ? 5u : 0u);
        auto noise = [&rng] { rng = rng * 1664525u + 1013904223u; return (double) (int) (rng >> 8) / 8388608.0 - 1.0; };
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
                // "fum": a soft thump with no note in it. The body slides down from ~150 Hz to ~60 Hz in a few tens of ms and rides on dark noise, so the ear
                // hears a beat and not a pitch (a click for singers must not fight their tuning); a short swell (22 ms) before it blooms and fades, and a
                // breath of dark air at the front. The accent is the same sound, fuller and longer - never higher.
                const double f0 = 150.0, f1 = 60.0, tauF = 0.040, tauD = accented ? 0.135 : 0.11;
                double ph = 0.0, lpBody = 0.0, lpAir = 0.0;
                for (int i = 0; i < n; ++i)
                {
                    const double t = i / rate;
                    ph += twoPi * (f1 + (f0 - f1) * std::exp (-t / tauF)) / rate;
                    const double rise = t < 0.022 ? 0.5 - 0.5 * std::cos (pi * t / 0.022) : 1.0;
                    const double dec = std::exp (-juce::jmax (0.0, t - 0.022) / tauD);
                    lpBody += 0.05 * (noise() - lpBody);
                    const double body = std::sin (ph) + (accented ? 7.0 : 5.0) * lpBody;
                    lpAir += 0.12 * (noise() - lpAir);
                    const double air = t < 0.03 ? std::sin (pi * t / 0.03) : 0.0;
                    x[(size_t) i] = body * rise * dec + 0.5 * air * lpAir;
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
        return finish (std::move (x), rate, accented ? 1.0 : 0.72);
    }

    /** One part of the drum kit (peak 1.0): a kick, a snare or a hi-hat, all made from sweeps and noise - no sample, no pitch to follow. */
    static std::vector<float> makeDrum (int voice, double rate) { return finish (makeDrumRaw (voice, rate), rate, 1.0); }

    /** The quiet hi-hat tick that sits on every quaver (peak 1.0 here; the engine plays it a few dB under the beat). */
    static std::vector<float> makeTick (double rate)
    {
        const int n = juce::jmax (8, (int) std::lround (0.045 * rate));
        std::vector<double> x ((size_t) n, 0.0);
        juce::uint32 rng = 9091u;
        double n1 = 0.0, n2 = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double t = i / rate;
            rng = rng * 1664525u + 1013904223u;
            const double w = (double) (int) (rng >> 8) / 8388608.0 - 1.0;
            x[(size_t) i] = (w - 2.0 * n1 + n2) * (std::exp (-t / 0.007) + 0.2 * std::exp (-t / 0.02));         // (a second difference: only the top of the band)
            n2 = n1; n1 = w;
        }
        return finish (std::move (x), rate, 1.0);
    }

private:
    /** The last 10 ms fade to nothing (no click at the end of the click) and the peak is set to 'target'. */
    static std::vector<float> finish (std::vector<double> x, double rate, double target)
    {
        const int n = (int) x.size();
        const int fadeN = juce::jmin (n / 4, (int) (0.01 * rate));
        for (int i = 0; i < fadeN; ++i) x[(size_t) (n - 1 - i)] *= (double) i / (double) fadeN;
        double peak = 1.0e-9; for (double v : x) peak = juce::jmax (peak, std::abs (v));
        std::vector<float> out ((size_t) n);
        for (int i = 0; i < n; ++i) out[(size_t) i] = (float) (x[(size_t) i] / peak * target);
        return out;
    }
    static std::vector<double> makeDrumRaw (int voice, double rate)
    {
        const double twoPi = juce::MathConstants<double>::twoPi;
        const double len = voice == Kick ? 0.28 : voice == Snare ? 0.2 : 0.09;
        const int n = juce::jmax (8, (int) std::lround (len * rate));
        std::vector<double> x ((size_t) n, 0.0);
        juce::uint32 rng = 4242u + (juce::uint32) voice * 31u;
        auto noise = [&rng] { rng = rng * 1664525u + 1013904223u; return (double) (int) (rng >> 8) / 8388608.0 - 1.0; };
        if (voice == Kick)
        {
            // a soft kick: the body falls from ~160 Hz to ~48 Hz in a few tens of ms (so it is a thump, not a note), with a tiny beater tap
            double ph = 0.0, lp = 0.0;
            for (int i = 0; i < n; ++i)
            {
                const double t = i / rate;
                ph += twoPi * (48.0 + 112.0 * std::exp (-t / 0.018)) / rate;
                const double env = (t < 0.0015 ? t / 0.0015 : 1.0) * std::exp (-t / 0.10);
                lp += 0.25 * (noise() - lp);
                x[(size_t) i] = std::sin (ph) * env + (t < 0.004 ? 0.35 * lp * (1.0 - t / 0.004) : 0.0);
            }
        }
        else if (voice == Snare)
        {
            // a snare: a burst of mid-to-high noise with a quick, falling "body" under it (mostly noise, so it has no note either)
            double prev = 0.0, band = 0.0, ph = 0.0;
            for (int i = 0; i < n; ++i)
            {
                const double t = i / rate;
                const double w = noise();
                band += 0.45 * ((w - prev) - band); prev = w;
                ph += twoPi * (150.0 + 70.0 * std::exp (-t / 0.012)) / rate;
                x[(size_t) i] = band * std::exp (-t / 0.065) * 0.9 + std::sin (ph) * std::exp (-t / 0.022) * 0.35;
            }
        }
        else
        {
            // a closed hi-hat: the top of the noise band with a short "tss" tail
            double n1 = 0.0, n2 = 0.0;
            for (int i = 0; i < n; ++i)
            {
                const double t = i / rate;
                const double w = noise();
                x[(size_t) i] = (w - 2.0 * n1 + n2) * (std::exp (-t / 0.018) + 0.25 * std::exp (-t / 0.05));
                n2 = n1; n1 = w;
            }
        }
        return x;
    }

    struct Playing { const float* data = nullptr; int len = 0, pos = 0; float gain = 1.0f; };
    void trigger (const std::vector<float>& h, float gain) noexcept
    {
        if (h.empty()) return;
        Playing* slot = &playing[0];
        for (auto& p : playing) if (p.data == nullptr) { slot = &p; break; }
        slot->data = h.data(); slot->len = (int) h.size(); slot->pos = 0; slot->gain = gain;
    }
    void mix (float* dst, int from, int count, float g) noexcept
    {
        if (count <= 0) return;
        for (auto& p : playing)
        {
            if (p.data == nullptr) continue;
            const int m = juce::jmin (count, p.len - p.pos);
            const float gg = g * p.gain;
            for (int i = 0; i < m; ++i) dst[from + i] += p.data[p.pos + i] * gg;
            p.pos += m;
            if (p.pos >= p.len) p = Playing();
        }
    }

    double sr = 48000.0, toNext = 0.0;
    int beat = 0, subIdx = 1, subInBeat = 1;
    std::array<std::array<std::vector<float>, 2>, NumSounds> hits;
    std::array<std::vector<float>, NumDrums> drums;
    std::vector<float> tickHit;
    std::array<Playing, 8> playing {};
    std::atomic<bool> runFlag { false }, restart { false }, accent { true }, ticks { false }, prepared { false };
    std::atomic<double> tempo { 120.0 };
    std::atomic<int> beats { 4 }, unit { 4 }, sound { (int) Soft }, beatCounter { 0 }, lastBeat { 0 };
    std::atomic<float> level { 0.25f }, tickGain { 0.1f };
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
