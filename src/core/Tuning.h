#pragma once
#include "Common.h"
#include <algorithm>
#include <array>
#include <vector>

namespace td
{
/** The pitch pipe's tunings. Every tuning is a set of small offsets (in cents) from equal temperament, one per note of the octave, built on a KEY
    (the note the tuning is centred on: just intonation and the historical temperaments sound different in every key). The whole tuning is then
    moved so that A is exactly the reference pitch (A = 440, 415 ...), as a tuning fork would set it.

    The temperaments:
      - Equal: every semitone the same (what a piano tuner does today).
      - Just: the scale of the key in pure ratios (5-limit: major third 5/4, fifth 3/2 ...). Lovely in the key, out in remote keys.
      - Pythagorean: a chain of pure fifths from the key (wide, bright thirds).
      - Quarter-comma meantone: fifths narrowed so the major thirds are pure (the Renaissance and early Baroque organ).
      - Werckmeister III, Vallotti, Kirnberger III: the 18th-century "well" temperaments (every key usable, each with its own colour).
    Pure chords: in the pitch pipe's chord mode a chord can be tuned PURE on its own root (a major chord's third 13.7 cents below the piano's, a
    minor chord's 15.6 cents above, the fifth 2 cents wide), whatever the tuning: the root stays where the tuning puts it. */
namespace tuning
{
enum Temperament : int { Equal = 0, Just, Pythagorean, Meantone, Werckmeister3, Vallotti, Kirnberger3, NumTemperaments };
enum ChordTuning : int { AsTuned = 0, PureMajor, PureMinor, NumChordTunings };

inline const char* temperamentName (int t)
{
    static const char* n[] = { "Equal temperament", "Just intonation", "Pythagorean", "Quarter-comma meantone", "Werckmeister III", "Vallotti", "Kirnberger III" };
    return juce::isPositiveAndBelow (t, (int) NumTemperaments) ? n[t] : "";
}
inline const char* temperamentInfo (int t)
{
    static const char* n[] = {
        "Every semitone the same size: a modern piano.",
        "The key's scale in pure ratios (major third 5/4, fifth 3/2). Beautiful in the key, poor in distant keys.",
        "A chain of pure fifths from the key: bright, wide thirds. Medieval music.",
        "Fifths a quarter of a syntonic comma narrow, so the major thirds are pure: Renaissance and early Baroque keyboards.",
        "Andreas Werckmeister's \"well\" temperament (1691): every key usable, the home keys smoother.",
        "Francesco Vallotti's temperament: six fifths narrowed by a sixth of a comma. Much used for Baroque music today.",
        "Johann Kirnberger's third temperament: four narrowed fifths, pure major third on the key." };
    return juce::isPositiveAndBelow (t, (int) NumTemperaments) ? n[t] : "";
}
inline const char* chordTuningName (int c)
{
    static const char* n[] = { "As the tuning", "Pure major chord", "Pure minor chord" };
    return juce::isPositiveAndBelow (c, (int) NumChordTunings) ? n[c] : "";
}
/** "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B". */
inline const char* pitchClassName (int pc)
{
    static const char* n[] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
    return n[((pc % 12) + 12) % 12];
}
/** "C4" (MIDI 60), "A4" (69) ... */
inline juce::String noteName (int midi) { return juce::String (pitchClassName (midi)) + juce::String (midi / 12 - 1); }
inline int pitchClass (int midi) noexcept { return ((midi % 12) + 12) % 12; }

inline double centsOf (double ratio) { return 1200.0 * std::log2 (ratio); }

/** The just ratio of an interval of 'semis' semitones (0 .. 11) above a root. 'minorContext': the minor seventh is 9/5 (a pure fifth above the
    minor third) rather than 16/9. */
inline double justRatio (int semis, bool minorContext = false)
{
    static const double r[] = { 1.0, 16.0 / 15.0, 9.0 / 8.0, 6.0 / 5.0, 5.0 / 4.0, 4.0 / 3.0, 45.0 / 32.0, 3.0 / 2.0, 8.0 / 5.0, 5.0 / 3.0, 16.0 / 9.0, 15.0 / 8.0 };
    semis = ((semis % 12) + 12) % 12;
    return semis == 10 && minorContext ? 9.0 / 5.0 : r[semis];
}

/** The offset (cents from equal temperament) of the note 'semis' semitones above the KEY, before the tuning is moved to put A on the reference. */
inline double offsetAboveKey (int t, int semis)
{
    semis = ((semis % 12) + 12) % 12;
    switch (t)
    {
        case Just:
            return centsOf (justRatio (semis)) - 100.0 * semis;
        case Pythagorean:
        {
            static const double r[] = { 1.0, 256.0 / 243.0, 9.0 / 8.0, 32.0 / 27.0, 81.0 / 64.0, 4.0 / 3.0, 729.0 / 512.0, 3.0 / 2.0, 128.0 / 81.0, 27.0 / 16.0, 16.0 / 9.0, 243.0 / 128.0 };
            return centsOf (r[semis]) - 100.0 * semis;
        }
        case Meantone:
        {
            // a chain of meantone fifths (5^(1/4)) from three fifths below the key to eight above (the keyboard's Eb .. G# when the key is C)
            const double fifth = centsOf (5.0) / 4.0;                     // 696.578 cents
            int n = (semis * 7) % 12;
            if (n > 8) n -= 12;
            double c = std::fmod ((double) n * fifth, 1200.0);
            if (c < 0.0) c += 1200.0;
            return c - 100.0 * semis;
        }
        case Werckmeister3:
        {
            static const double d[] = { 0.0, -9.775, -7.820, -5.865, -9.775, -1.955, -11.730, -3.910, -7.820, -11.730, -3.910, -7.820 };
            return d[semis];
        }
        case Vallotti:
        {
            static const double d[] = { 0.0, -5.865, -3.910, -1.955, -7.820, 1.955, -7.820, -1.955, -3.910, -5.865, 0.0, -9.775 };
            return d[semis];
        }
        case Kirnberger3:
        {
            static const double d[] = { 0.0, -9.775, -6.843, -5.865, -13.686, -1.955, -9.776, -3.422, -7.820, -10.265, -3.910, -11.731 };
            return d[semis];
        }
        default: return 0.0;
    }
}

/** The offset (cents from equal temperament at the reference A) of pitch class 'pc' in tuning t built on 'key': A is always 0. */
inline double offsetCents (int t, int key, int pc)
{
    auto raw = [t, key] (int p) { return offsetAboveKey (t, p - key); };
    return raw (pc) - raw (9);
}

/** The frequency of a MIDI note in the tuning (A4 = refA). */
inline double frequency (int midi, double refA, int t, int key)
{
    return refA * std::pow (2.0, ((double) midi - 69.0) / 12.0) * std::pow (2.0, offsetCents (t, key, pitchClass (midi)) / 1200.0);
}

/** A chord's frequencies. With a pure chord tuning the chord's root is found (the note whose major / minor third and fifth are in the chord; failing
    that, the lowest note) and every note is tuned in pure ratios above it; the root keeps its pitch in the tuning. */
struct Chord
{
    std::vector<double> freqs;          // one per note, in the order given
    int root = -1;                      // the MIDI note the pure chord is built on (-1: the chord follows the tuning)
    bool triadFound = false;            // a major / minor triad was found in the notes (false: tuned pure on the lowest note)
};
inline Chord chordFrequencies (const std::vector<int>& notes, double refA, int t, int key, int chordTuning)
{
    Chord c;
    if (notes.empty()) return c;
    if ((chordTuning != PureMajor && chordTuning != PureMinor) || notes.size() < 2)
    {
        for (int n : notes) c.freqs.push_back (frequency (n, refA, t, key));
        return c;
    }
    const bool minor = chordTuning == PureMinor;
    const int third = minor ? 3 : 4;
    bool has[12] = {};
    for (int n : notes) has[pitchClass (n)] = true;
    const int lowest = *std::min_element (notes.begin(), notes.end());
    // the root: the pitch class with the chord's third above it, and best also its fifth and itself; ties go to the lowest note's
    int bestPc = -1, bestScore = -1;
    for (int k = 0; k < 12; ++k)
    {
        const int pc = (pitchClass (lowest) + k) % 12;                     // (starting from the bass: an inversion's root is found the same way)
        if (! has[(pc + third) % 12]) continue;
        const int score = (has[pc] ? 2 : 0) + (has[(pc + 7) % 12] ? 2 : 0) + 1;
        if (score > bestScore) { bestScore = score; bestPc = pc; }
    }
    c.triadFound = bestPc >= 0 && bestScore >= 3;
    if (bestPc < 0) bestPc = pitchClass (lowest);
    // the root note itself: the lowest note of that pitch class in the chord, or (a chord without its root) the one just below the bass
    int rootMidi = -1;
    for (int n : notes) if (pitchClass (n) == bestPc && (rootMidi < 0 || n < rootMidi)) rootMidi = n;
    if (rootMidi < 0) rootMidi = lowest - ((pitchClass (lowest) - bestPc + 12) % 12);
    c.root = rootMidi;
    const double fRoot = frequency (rootMidi, refA, t, key);
    for (int n : notes)
    {
        const int d = n - rootMidi;
        const int oct = (int) std::floor ((double) d / 12.0);
        const int semis = d - 12 * oct;
        c.freqs.push_back (fRoot * justRatio (semis, minor) * std::pow (2.0, (double) oct));
    }
    return c;
}

/** How far (cents) a frequency is from the equal-tempered note of the same name at the reference A. */
inline double centsFromEqual (double hz, int midi, double refA)
{
    return centsOf (hz / (refA * std::pow (2.0, ((double) midi - 69.0) / 12.0)));
}
} // namespace tuning
} // namespace td
