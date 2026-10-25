# Signalsmith Stretch (in Fermata)

The ReTimEr (edit window, 2.103.0) uses this library to slow down or speed up the audio without changing its pitch.

* **Signalsmith Stretch** 1.3.2 - https://github.com/Signalsmith-Audio/signalsmith-stretch (commit 88061fd), MIT licence: `LICENSE.txt`
* **Signalsmith Linear** 0.6.6 - https://github.com/Signalsmith-Audio/linear (commit 7bb1c8f), MIT licence: `signalsmith-linear/LICENSE.txt`
  (only `stft.h` and `fft.h` are needed; the folder sits next to `signalsmith-stretch.h` so its `#include "signalsmith-linear/stft.h"` finds it without any include path)

The files are exactly as published. They are only included by `src/core/Retime.cpp` (a translation unit of their own, with the compiler's
warnings switched off around the include).

Why this library: all the channels of one stretcher are phase-locked to each other (for every frequency band the loudest channel is worked
out and the others keep their own phase difference to it), so the time differences between the microphones of a multitrack are kept - the
image does not smear. It measured about 15 times faster than Rubber Band's finer engine on the same material, and it is MIT licensed.
