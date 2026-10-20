# The denoiser's reference implementation and bench (Python)

`engine.py` is the reference implementation of Fermata's background denoiser; `src/core/Denoise.h` is its C++ port and must match it
(`tests/core_test.cpp`, "2.77 background denoiser", against `tests/data/denoise/` made by `make_golden.py`). Change the algorithm HERE first,
run the bench, then port the change and remake the golden files.

- `engine.py`  the denoiser: profile learning, the two-direction decision-directed SNR, the tail rule, presence weighting, harmonic protection
- `baseline.py` a conventional short-window threshold denoiser with a 350 ms release (the kind that "clicks off" in a reverb tail)
- `synth.py`   test signals: harmonically rich voices with vibrato, a church-like reverb with per-band decay, room tone (synthetic, or real from a clip)
- `metrics.py` what is measured when the truth is known: harmonic preservation, tail behaviour against an oracle Wiener mask, noise reduction,
               flicker (musical noise), SNR
- `bench.py`   the scenarios (`python3 bench.py chord tail spot write`): ours vs baseline vs oracle, tables and listening files in `out/`
- `real.py`    real clips without truth (set FERMATA_DN_CLIPS to a folder of WAVs, or put them in `clips/`): learn from a marked region, process,
               write input / output / removed

Needs numpy and scipy (matplotlib for the plots). 96 kHz clips take a few seconds each.
