#pragma once
// 3.27.0: compressed waveforms. How big a waveform is DRAWN - never what is heard: quiet passages much bigger, loud ones a little smaller, like a compressor on
// the picture, so music that swings from fff to ppp in a moment shows both. The take, edit and trim windows draw every peak (and, zoomed right in, every sample)
// through apply().
#include <cmath>

namespace td { namespace waveshape
{
/** The strengths: 0 off (the plain waveform), 1 light, 2 medium, 3 strong. Something 40 dB down (ppp under fff) is drawn at 1/100 of the height plain,
    about 1/16 light, 1/6 medium and 1/3 strong; full level stays full height. */
inline float exponentFor (int strength) noexcept
{
    switch (strength) { case 1: return 0.6f; case 2: return 0.4f; case 3: return 0.25f; default: return 1.0f; }
}
inline const char* nameOf (int strength) noexcept
{
    switch (strength) { case 1: return "Light"; case 2: return "Medium"; case 3: return "Strong"; default: return "Off"; }
}
/** A sample value or a peak (-1 .. 1, more with a gain) as it is drawn: the same sign, its size raised to the exponent (1: unchanged). */
inline float apply (float v, float exponent) noexcept
{
    if (exponent >= 0.999f || v == 0.0f) return v;
    const float a = std::pow (std::abs (v), exponent);
    return v < 0.0f ? -a : a;
}
}} // namespace td::waveshape
