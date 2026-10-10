#include "Dsp.h"
#include "third_party/pocketfft_hdronly.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <numeric>
#include <thread>
#include <atomic>

namespace rehar
{
double midiOf (double hz, double a4) { return 69.0 + 12.0 * std::log2 (hz / a4); }
double midiHz (double midi, double a4) { return a4 * std::pow (2.0, (midi - 69.0) / 12.0); }

std::string noteLabel (int m)
{
    static const char* names[] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
    const int k = ((m % 12) + 12) % 12;
    const int oct = (m - k) / 12 - 1;
    return std::string (names[k]) + std::to_string (oct);
}

Vec hanning (size_t n)
{
    Vec w (n, 1.0);
    if (n > 1) for (size_t i = 0; i < n; ++i) w[i] = 0.5 - 0.5 * std::cos (2.0 * kPi * (double) i / (double) (n - 1));
    return w;
}

double interp1 (double x, const Vec& xp, const Vec& fp)
{
    const size_t n = xp.size();
    if (n == 0) return 0.0;
    if (x <= xp.front()) return fp.front();
    if (x >= xp.back()) return fp.back();
    const size_t j = (size_t) (std::upper_bound (xp.begin(), xp.end(), x) - xp.begin()) - 1;      // xp[j] <= x < xp[j+1]
    if (j + 1 >= n) return fp.back();
    const double dx = xp[j + 1] - xp[j];
    if (dx == 0.0) return fp[j];
    return fp[j] + (fp[j + 1] - fp[j]) * (x - xp[j]) / dx;
}

Vec interp (const Vec& x, const Vec& xp, const Vec& fp)
{
    Vec out (x.size());
    if (xp.empty()) return out;
    // x is usually increasing: walk instead of searching
    size_t j = 0;
    const size_t n = xp.size();
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double v = x[i];
        if (v <= xp.front()) { out[i] = fp.front(); continue; }
        if (v >= xp.back())  { out[i] = fp.back(); continue; }
        if (j >= n - 1 || xp[j] > v) j = (size_t) (std::upper_bound (xp.begin(), xp.end(), v) - xp.begin()) - 1;
        while (j + 1 < n && xp[j + 1] <= v) ++j;
        const double dx = xp[j + 1] - xp[j];
        out[i] = dx == 0.0 ? fp[j] : fp[j] + (fp[j + 1] - fp[j]) * (v - xp[j]) / dx;
    }
    return out;
}

static long mapIndex (long i, long n, Edge mode)
{
    if (i >= 0 && i < n) return i;
    if (mode == Edge::Nearest) return i < 0 ? 0 : n - 1;
    const long period = 2 * n;                          // 'reflect': d c b a | a b c d | d c b a
    long j = i % period; if (j < 0) j += period;
    return j < n ? j : period - 1 - j;
}

Vec gaussianFilter1d (const Vec& in, double sigma, Edge mode)
{
    const long n = (long) in.size();
    if (n == 0 || sigma <= 0.0) return in;
    const int radius = (int) (4.0 * sigma + 0.5);
    Vec k ((size_t) (2 * radius + 1));
    const double s2 = sigma * sigma;
    double sum = 0.0;
    for (int x = -radius; x <= radius; ++x) { const double v = std::exp (-0.5 / s2 * (double) (x * x)); k[(size_t) (x + radius)] = v; sum += v; }
    for (auto& v : k) v /= sum;
    Vec out ((size_t) n);
    for (long i = 0; i < n; ++i)
    {
        double acc = 0.0;
        if (i - radius >= 0 && i + radius < n) { for (int x = -radius; x <= radius; ++x) acc += k[(size_t) (x + radius)] * in[(size_t) (i + x)]; }
        else for (int x = -radius; x <= radius; ++x) acc += k[(size_t) (x + radius)] * in[(size_t) mapIndex (i + x, n, mode)];
        out[(size_t) i] = acc;
    }
    return out;
}

Vec medianFilter (const Vec& in, int size)
{
    const long n = (long) in.size(); const int h = size / 2;
    Vec out ((size_t) n), w ((size_t) size);
    for (long i = 0; i < n; ++i)
    {
        for (int k = -h; k <= h; ++k) w[(size_t) (k + h)] = in[(size_t) mapIndex (i + k, n, Edge::Nearest)];
        std::nth_element (w.begin(), w.begin() + h, w.end());
        out[(size_t) i] = w[(size_t) h];
    }
    return out;
}

Vec gradient (const Vec& x)
{
    const size_t n = x.size();
    Vec g (n, 0.0);
    if (n < 2) return g;
    g[0] = x[1] - x[0];
    g[n - 1] = x[n - 1] - x[n - 2];
    for (size_t i = 1; i + 1 < n; ++i) g[i] = (x[i + 1] - x[i - 1]) / 2.0;
    return g;
}

Vec unwrap (const Vec& p)
{
    Vec out (p);
    if (p.size() < 2) return out;
    double corr = 0.0;
    for (size_t i = 1; i < p.size(); ++i)
    {
        const double dd = p[i] - p[i - 1];
        double m = std::fmod (dd + kPi, 2.0 * kPi); if (m < 0) m += 2.0 * kPi; m -= kPi;
        if (m == -kPi && dd > 0) m = kPi;
        double c = m - dd;
        if (std::abs (dd) < kPi) c = 0.0;
        corr += c;
        out[i] = p[i] + corr;
    }
    return out;
}

double median (Vec v)
{
    if (v.empty()) return 0.0;
    const size_t n = v.size();
    std::nth_element (v.begin(), v.begin() + (long) (n / 2), v.end());
    const double hi = v[n / 2];
    if (n % 2) return hi;
    const double lo = *std::max_element (v.begin(), v.begin() + (long) (n / 2));
    return 0.5 * (lo + hi);
}

double percentile (Vec v, double q)
{
    if (v.empty()) return 0.0;
    std::sort (v.begin(), v.end());
    const double pos = q / 100.0 * (double) (v.size() - 1);
    const size_t lo = (size_t) std::floor (pos), hi = std::min (v.size() - 1, lo + 1);
    return v[lo] + (v[hi] - v[lo]) * (pos - (double) lo);
}

size_t nextFastLen (size_t n)
{
    if (n <= 6) return std::max<size_t> (n, 1);
    for (size_t m = n;; ++m)
    {
        size_t r = m;
        for (size_t f : { (size_t) 2, (size_t) 3, (size_t) 5, (size_t) 7, (size_t) 11 }) while (r % f == 0) r /= f;
        if (r == 1) return m;
    }
}

void rfftMagnitude (const double* x, size_t n, size_t nfft, Vec& out)
{
    std::vector<double> in (nfft, 0.0);
    std::copy (x, x + std::min (n, nfft), in.begin());
    const size_t nb = nfft / 2 + 1;
    CVec spec (nb);
    pocketfft::shape_t shape { nfft };
    pocketfft::stride_t sin_ { (std::ptrdiff_t) sizeof (double) }, sout { (std::ptrdiff_t) sizeof (cplx) };
    pocketfft::r2c (shape, sin_, sout, 0, true, in.data(), spec.data(), 1.0);
    out.resize (nb);
    for (size_t i = 0; i < nb; ++i) out[i] = std::abs (spec[i]);
}

void fftComplex (CVec& data, bool inverse)
{
    const size_t n = data.size();
    if (n == 0) return;
    pocketfft::shape_t shape { n };
    pocketfft::stride_t st { (std::ptrdiff_t) sizeof (cplx) };
    pocketfft::c2c (shape, st, st, pocketfft::shape_t { 0 }, ! inverse, data.data(), data.data(), inverse ? 1.0 / (double) n : 1.0);
}

// ---------------------------------------------------------------------------------------------------- resampling
static double besselI0 (double x)
{
    double sum = 1.0, term = 1.0; const double q = x * x / 4.0;
    for (int k = 1; k < 200; ++k) { term *= q / ((double) k * (double) k); sum += term; if (term < 1e-17 * sum) break; }
    return sum;
}

static double sincPi (double x) { if (x == 0.0) return 1.0; const double a = kPi * x; return std::sin (a) / a; }

Vec resamplePoly (const float* x, size_t nIn, int down)
{
    const int up = 1;
    const size_t nOut = (nIn * (size_t) up + (size_t) down - 1) / (size_t) down;
    if (down == 1) return Vec (x, x + nIn);
    const int halfLen = 10 * down;
    const int taps = 2 * halfLen + 1;
    // firwin (taps, 1 / down, window = ('kaiser', 5.0)), scaled to unity gain at DC
    Vec h ((size_t) taps);
    const double cutoff = 1.0 / (double) down, beta = 5.0, alpha = 0.5 * (double) (taps - 1), i0b = besselI0 (beta);
    double sum = 0.0;
    for (int i = 0; i < taps; ++i)
    {
        const double m = (double) i - alpha;
        const double r = (alpha > 0) ? m / alpha : 0.0;
        const double win = besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0b;
        h[(size_t) i] = cutoff * sincPi (cutoff * m) * win;
        sum += h[(size_t) i];
    }
    for (auto& v : h) v /= sum;
    const int prePad = down - halfLen % down;
    const int preRemove = (halfLen + prePad) / down;
    Vec hp ((size_t) (taps + prePad), 0.0);
    std::copy (h.begin(), h.end(), hp.begin() + prePad);
    Vec out (nOut, 0.0);
    const long hl = (long) hp.size(), nn = (long) nIn;
    for (size_t k = 0; k < nOut; ++k)
    {
        const long m = ((long) k + preRemove) * down;                       // y[m] = sum_j hp[j] x[m - j]
        const long jLo = std::max<long> (0, m - (nn - 1)), jHi = std::min<long> (hl - 1, m);
        double acc = 0.0;
        for (long j = jLo; j <= jHi; ++j) acc += hp[(size_t) j] * (double) x[m - j];
        out[k] = acc;
    }
    return out;
}

int decimateTo (const float* x, size_t n, int sr, std::vector<float>& out, int target)
{
    const int q = (sr + target - 1) / target;
    if (q <= 1) { out.assign (x, x + n); return sr; }
    const Vec y = resamplePoly (x, n, q);
    out.resize (y.size());
    for (size_t i = 0; i < y.size(); ++i) out[i] = (float) y[i];
    return sr / q;
}

// ---------------------------------------------------------------------------------------------------- Butterworth band-pass
std::vector<Biquad> butterBandpass (int order, double loHz, double hiHz, double fs)
{
    const double fs2 = 2.0 * fs;
    const double w0 = fs2 * std::tan (kPi * loHz / fs), w1 = fs2 * std::tan (kPi * hiHz / fs);        // pre-warped band edges
    const double bw = w1 - w0, wo = std::sqrt (w0 * w1);
    // analogue low-pass prototype poles, moved to a band-pass
    CVec poles;
    for (int k = 1; k <= order; ++k)
    {
        const double ang = kPi * (double) (2 * k + order - 1) / (double) (2 * order);
        const cplx p = std::polar (1.0, ang);
        const cplx a = p * bw / 2.0, d = std::sqrt (a * a - wo * wo);
        poles.push_back (a + d);
        poles.push_back (a - d);
    }
    // bilinear transform; the analogue zeros (order of them at 0) become +1, the missing ones -1
    double gain = std::pow (bw, order);
    cplx num (1.0, 0.0), den (1.0, 0.0);
    for (int i = 0; i < order; ++i) num *= (fs2 - cplx (0.0, 0.0));
    for (auto& p : poles) den *= (fs2 - p);
    gain *= (num / den).real();
    CVec zp;
    for (auto& p : poles) zp.push_back ((fs2 + p) / (fs2 - p));
    // pair each pole with its conjugate: one second-order section each, numerator (1 - z^-2)
    std::vector<Biquad> sos;
    std::vector<bool> used (zp.size(), false);
    for (size_t i = 0; i < zp.size(); ++i)
    {
        if (used[i]) continue;
        used[i] = true;
        size_t best = i; double bestd = 1e300;
        for (size_t j = i + 1; j < zp.size(); ++j) if (! used[j]) { const double d = std::abs (zp[j] - std::conj (zp[i])); if (d < bestd) { bestd = d; best = j; } }
        if (best != i) used[best] = true;
        const cplx p1 = zp[i], p2 = (best != i) ? zp[best] : std::conj (zp[i]);
        Biquad s { 1.0, 0.0, -1.0, -(p1 + p2).real(), (p1 * p2).real() };
        sos.push_back (s);
    }
    if (! sos.empty()) { sos[0].b0 *= gain; sos[0].b1 *= gain; sos[0].b2 *= gain; }
    return sos;
}

Vec sosfilt (const std::vector<Biquad>& sos, const Vec& x)
{
    Vec y (x);
    for (auto& s : sos)
    {
        double z0 = 0.0, z1 = 0.0;
        for (size_t i = 0; i < y.size(); ++i)
        {
            const double in = y[i];
            const double out = s.b0 * in + z0;
            z0 = s.b1 * in - s.a1 * out + z1;
            z1 = s.b2 * in - s.a2 * out;
            y[i] = out;
        }
    }
    return y;
}

// ---------------------------------------------------------------------------------------------------- find_peaks
std::vector<int> findPeaks (const Vec& x, double prominence, double height, double distance)
{
    std::vector<int> peaks;
    const int n = (int) x.size();
    // local maxima (a flat top counts once, at its middle)
    for (int i = 1; i + 1 < n; ++i)
    {
        if (x[(size_t) i - 1] < x[(size_t) i])
        {
            int ahead = i + 1;
            while (ahead < n - 1 && x[(size_t) ahead] == x[(size_t) i]) ++ahead;
            if (x[(size_t) ahead] < x[(size_t) i]) { peaks.push_back ((i + ahead - 1) / 2); i = ahead; }
        }
    }
    if (height > -1e299) { std::vector<int> k; for (int p : peaks) if (x[(size_t) p] >= height) k.push_back (p); peaks.swap (k); }
    if (distance >= 1.0 && peaks.size() > 1)
    {
        const int dist = (int) std::ceil (distance);
        const size_t sz = peaks.size();
        std::vector<size_t> order (sz);
        std::iota (order.begin(), order.end(), (size_t) 0);
        std::stable_sort (order.begin(), order.end(), [&] (size_t a, size_t b) { return x[(size_t) peaks[a]] < x[(size_t) peaks[b]]; });
        std::vector<char> keep (sz, 1);
        for (size_t ii = sz; ii-- > 0;)
        {
            const size_t j = order[ii];
            if (! keep[j]) continue;
            long k = (long) j - 1;
            while (k >= 0 && peaks[j] - peaks[(size_t) k] < dist) { keep[(size_t) k] = 0; --k; }
            k = (long) j + 1;
            while (k < (long) sz && peaks[(size_t) k] - peaks[j] < dist) { keep[(size_t) k] = 0; ++k; }
        }
        std::vector<int> kept; for (size_t i = 0; i < sz; ++i) if (keep[i]) kept.push_back (peaks[i]);
        peaks.swap (kept);
    }
    if (prominence >= 0.0)
    {
        std::vector<int> kept;
        for (int p : peaks)
        {
            const double xp = x[(size_t) p];
            double leftMin = xp, rightMin = xp;
            for (int i = p; i >= 0 && x[(size_t) i] <= xp; --i) leftMin = std::min (leftMin, x[(size_t) i]);
            for (int i = p; i < n && x[(size_t) i] <= xp; ++i) rightMin = std::min (rightMin, x[(size_t) i]);
            if (xp - std::max (leftMin, rightMin) >= prominence) kept.push_back (p);
        }
        peaks.swap (kept);
    }
    return peaks;
}

// ---------------------------------------------------------------------------------------------------- Gaussian low-pass
CVec lowpassGaussian (const CVec& z, double sr, double bw)
{
    const size_t L = z.size();
    const size_t nfft = nextFastLen (L + (size_t) (0.3 * sr));
    CVec f (nfft, cplx (0.0, 0.0));
    std::copy (z.begin(), z.end(), f.begin());
    fftComplex (f, false);
    const double sig = bw / 2.355 / 2.0;
    for (size_t k = 0; k < nfft; ++k)
    {
        const double freq = (double) std::min (k, nfft - k) * sr / (double) nfft;       // |fftfreq|
        f[k] *= std::exp (-0.5 * (freq / sig) * (freq / sig));
    }
    fftComplex (f, true);
    CVec out (L);
    for (size_t i = 0; i < L; ++i) out[i] = f[i] * 2.0;
    return out;
}

void parallelFor (size_t count, const std::function<void (size_t)>& fn)
{
    unsigned hw = std::thread::hardware_concurrency();
    const size_t workers = std::min<size_t> (count, std::max<unsigned> (1u, std::min (hw, 8u)));
    if (workers <= 1) { for (size_t i = 0; i < count; ++i) fn (i); return; }
    std::atomic<size_t> next { 0 };
    std::exception_ptr error; std::mutex em;
    auto work = [&]
    {
        for (;;)
        {
            const size_t i = next.fetch_add (1);
            if (i >= count) return;
            try { fn (i); } catch (...) { std::lock_guard<std::mutex> l (em); if (! error) error = std::current_exception(); next.store (count); return; }
        }
    };
    std::vector<std::thread> ts;
    for (size_t w = 1; w < workers; ++w) ts.emplace_back (work);
    work();
    for (auto& t : ts) t.join();
    if (error) std::rethrow_exception (error);
}
} // namespace rehar
