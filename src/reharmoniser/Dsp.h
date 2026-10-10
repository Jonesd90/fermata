#pragma once
/** Re-HarmoniSer core: signal-processing helpers (plain C++17, no JUCE, no file or UI code).
    Each function reproduces the numpy / scipy call of the Python prototype it is named after, so the results can be compared with it. */
#include <complex>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace rehar
{
using cplx = std::complex<double>;
using Vec = std::vector<double>;
using CVec = std::vector<cplx>;

constexpr double kPi = 3.14159265358979323846;

/** A read-only view of one channel of audio. */
struct Span
{
    const float* p = nullptr;
    size_t n = 0;
    const float& operator[] (size_t i) const { return p[i]; }
};

/** Thrown by the long operations when the caller's cancel hook says so. */
struct Cancelled : std::runtime_error { Cancelled() : std::runtime_error ("Cancelled.") {} };

/** Progress and cancel hooks for a long operation. Both are optional. They are called from the thread that does the work. */
struct Hooks
{
    std::function<void (const char* stage, double fraction)> progress;
    std::function<bool()> cancelled;
    void report (const char* stage, double f) const { if (progress) progress (stage, f); }
    void check() const { if (cancelled && cancelled()) throw Cancelled(); }
};

// ---------------------------------------------------------------------------------------------------- pitch
double midiOf (double hz, double a4 = 440.0);
double midiHz (double midi, double a4 = 440.0);
std::string noteLabel (int midi);

// ---------------------------------------------------------------------------------------------------- numpy / scipy equivalents
Vec hanning (size_t n);                                                   // np.hanning
Vec interp (const Vec& x, const Vec& xp, const Vec& fp);                  // np.interp (clamped at both ends)
double interp1 (double x, const Vec& xp, const Vec& fp);
enum class Edge { Nearest, Reflect };
Vec gaussianFilter1d (const Vec& in, double sigma, Edge mode = Edge::Reflect);       // scipy.ndimage.gaussian_filter1d (truncate 4)
Vec medianFilter (const Vec& in, int size);                               // scipy.ndimage.median_filter, mode 'nearest' (odd size)
Vec gradient (const Vec& in);                                             // np.gradient
Vec unwrap (const Vec& phase);                                            // np.unwrap
double median (Vec v);                                                    // np.median
double percentile (Vec v, double q);                                      // np.percentile (linear), q in 0..100
size_t nextFastLen (size_t n);                                            // scipy.fft.next_fast_len (complex transforms)
/** Magnitudes |rfft (x[0..n) padded with zeros to nfft)| for bins 0..nfft/2. */
void rfftMagnitude (const double* x, size_t n, size_t nfft, Vec& out);
/** Full spectrum of x[0..n) padded with zeros to nfft (np.fft.fft / ifft: the inverse is divided by nfft). */
void fftComplex (CVec& data, bool inverse);

/** scipy.signal.resample_poly (x, 1, q): a zero-phase Kaiser-window low-pass, then every q-th sample. */
Vec resamplePoly (const float* x, size_t n, int q);
/** decimate_to of the prototype: down to at most 24 kHz. Returns the new rate; 'out' is rounded to float like the prototype's. */
int decimateTo (const float* x, size_t n, int sr, std::vector<float>& out, int target = 24000);

/** scipy.signal.butter (order, [lo, hi], 'bp', fs = fs, output = 'sos') and sosfilt. */
struct Biquad { double b0, b1, b2, a1, a2; };
std::vector<Biquad> butterBandpass (int order, double loHz, double hiHz, double fs);
Vec sosfilt (const std::vector<Biquad>& sos, const Vec& x);

/** scipy.signal.find_peaks with the options the prototype uses. A negative value means "not set". */
std::vector<int> findPeaks (const Vec& x, double prominence = -1.0, double height = -1e300, double distance = -1.0);

/** The zero-phase Gaussian low-pass of the prototype (_lowpass): z is filtered with a Gaussian of full width ~bw Hz. */
CVec lowpassGaussian (const CVec& z, double sr, double bw);

/** Runs fn (0) .. fn (count - 1), on several threads when there is more than one (the jobs must not touch each other's data). */
void parallelFor (size_t count, const std::function<void (size_t)>& fn);
} // namespace rehar
