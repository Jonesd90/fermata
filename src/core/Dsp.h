#pragma once
#include <vector>
#include <complex>
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace td { namespace dsp
{
constexpr double kPi = 3.14159265358979323846;

/** A plain radix-2 FFT (double precision). Size must be a power of two. */
class FFT
{
public:
    explicit FFT (int sizePow2) : n (sizePow2), tw ((size_t) sizePow2 / 2), rev ((size_t) sizePow2)
    {
        for (int i = 0; i < n / 2; ++i) tw[(size_t) i] = std::polar (1.0, -2.0 * kPi * (double) i / (double) n);
        int bits = 0; while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i) { int r = 0; for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b); rev[(size_t) i] = r; }
    }
    int size() const noexcept { return n; }
    void forward (std::vector<std::complex<double>>& a) const { run (a, false); }
    /** Inverse, scaled by 1/n. */
    void inverse (std::vector<std::complex<double>>& a) const { run (a, true); for (auto& v : a) v /= (double) n; }
private:
    void run (std::vector<std::complex<double>>& a, bool inv) const
    {
        for (int i = 0; i < n; ++i) { const int j = rev[(size_t) i]; if (j > i) std::swap (a[(size_t) i], a[(size_t) j]); }
        for (int len = 2; len <= n; len <<= 1)
        {
            const int half = len / 2, step = n / len;
            for (int i = 0; i < n; i += len)
                for (int k = 0; k < half; ++k)
                {
                    auto w = tw[(size_t) (k * step)]; if (inv) w = std::conj (w);
                    const auto u = a[(size_t) (i + k)], v = a[(size_t) (i + k + half)] * w;
                    a[(size_t) (i + k)] = u + v; a[(size_t) (i + k + half)] = u - v;
                }
        }
    }
    int n; std::vector<std::complex<double>> tw; std::vector<int> rev;
};

inline std::vector<double> hann (int n)
{
    std::vector<double> w ((size_t) n);
    for (int i = 0; i < n; ++i) w[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * kPi * (double) i / (double) n);   // periodic: sums to a constant at 75 % overlap
    return w;
}

inline double wrapPhase (double p) { return p - 2.0 * kPi * std::floor ((p + kPi) / (2.0 * kPi)); }
}} // namespace td::dsp
