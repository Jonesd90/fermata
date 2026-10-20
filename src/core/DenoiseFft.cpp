// The denoiser's FFT: pocketfft (already in the repository for the ReHarmoniSer), kept in its own translation unit so that the 3,800-line
// header is compiled once and Denoise.h stays light for everything that includes it.
#define POCKETFFT_NO_MULTITHREADING 1
#include "../reharmoniser/third_party/pocketfft_hdronly.h"
#include "Denoise.h"

namespace td::denoise::detail
{
void realForward (const double* in, int n, std::complex<double>* out)
{
    pocketfft::shape_t shape { (size_t) n };
    pocketfft::stride_t sIn { (std::ptrdiff_t) sizeof (double) }, sOut { (std::ptrdiff_t) sizeof (std::complex<double>) };
    pocketfft::r2c (shape, sIn, sOut, 0, true, in, out, 1.0);
}

void realInverse (const std::complex<double>* in, int n, double* out)
{
    pocketfft::shape_t shape { (size_t) n };
    pocketfft::stride_t sIn { (std::ptrdiff_t) sizeof (std::complex<double>) }, sOut { (std::ptrdiff_t) sizeof (double) };
    pocketfft::c2r (shape, sIn, sOut, 0, false, in, out, 1.0 / (double) n);
}
} // namespace td::denoise::detail
