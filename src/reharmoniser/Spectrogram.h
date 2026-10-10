#pragma once
/** Re-HarmoniSer core: the spectrogram picture (plain C++17, no JUCE).
    Two analysis windows are blended: a long one (fine pitch) for low and middle pitches, a short one (sharp in time) for the top, with a smooth
    cross-over. Every channel has its own picture stored as 0..255 = -127.5..0 dBFS; the picture you see is the channels added by POWER, each with
    its own "see" gain (the visual mixer). It reproduces the Python prototype's Spectrogram. */
#include "Dsp.h"
#include <atomic>
#include <cstdint>

namespace rehar
{
struct Spectrogram
{
    static constexpr double kFmax = 8000.0, kShortS = 0.043, kCrossLo = 500.0, kCrossHi = 3000.0;
    static constexpr int kMaxFrames = 7000;
    enum class Detail { Pitch, Balanced, Time };

    int sr = 0, sr2 = 0;               // file rate and analysis rate (<= 24 kHz)
    size_t N = 0, hop = 0, nb = 0, nf = 0;
    double df = 0.0;                   // Hz per bin
    double ref = 0.0;                  // dB level that is shown as the top of the colour scale
    std::vector<std::vector<uint8_t>> db;   // [channel][frame * nb + bin]
    Vec gain;                          // linear "see" gain per channel

    /** Analyses the channels (all the same length). Throws Cancelled / std::runtime_error. */
    void build (const std::vector<Span>& chans, int sampleRate, Detail d = Detail::Balanced, const Hooks& hooks = {});
    /** Changes the see gains (linear, one per channel) and re-finds the colour reference. */
    void setGains (const Vec& g);
    /** The channels added by power at one frame, in dB, bins 0..nb-1. */
    void combineFrame (size_t frame, std::vector<float>& outDb) const;
    double frameTime (size_t frame) const { return (double) (frame * hop) / sr2; }
    double binHz (size_t bin) const { return (double) bin * df; }
private:
    void findRef();
};
} // namespace rehar
