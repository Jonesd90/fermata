"""Real clips (no ground truth): learn the room tone from a marked region, process, report what can be measured without truth,
write listening files (input / output / removed)."""
import os, sys, time, glob, numpy as np
from audio import read_wav, write_wav24, UPLOADS
import engine, baseline, metrics
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'out') + os.sep

CLIPS = {
    'omni':  ('Ice_Shining_020_OMNI-L', (1.5, 2.5), (0.0, 73.0)),
    'spotC': ('Solo_Vocal_for_tuning_-_C', (4.75, 5.75), (0.0, 38.9)),
    'spot3': ('Solo_Vocal_for_tuning_-_Spot3', (4.75, 5.75), (0.0, 38.9)),
    'ice4':  ('Ice_Shining_020_4-C', (0.2, 2.3), (0.0, 73.0)),
}


def run(key, p=None, floor_db=-12.0, write=True, with_baseline=True, seconds=None):
    name, (n0, n1), (s0, s1) = CLIPS[key]
    sr, x = read_wav(glob.glob(UPLOADS + '*' + name + '.wav')[0])
    if x.ndim > 1: x = x[:, 0]
    if seconds: s1 = min(s1, s0 + seconds)
    x = x[int(s0 * sr):int(s1 * sr)]
    region = x[int(n0 * sr):int(n1 * sr)]
    p = p or engine.Params(); p.residual_db = floor_db
    rows = []
    def report(label, y, rem, secs):
        row = dict(clip=key, which=label, secs=round(secs, 1))
        row['nr_in_region'] = round(metrics.noise_reduction(x, y, sr, n0, n1), 1)
        row['b1-4k_vs_in'] = round(metrics.band_energy_ratio(x, y, sr, 1000, 4000), 2)
        row['b4-8k_vs_in'] = round(metrics.band_energy_ratio(x, y, sr, 4000, 8000), 2)
        row['b8-16k_vs_in'] = round(metrics.band_energy_ratio(x, y, sr, 8000, 16000), 2)
        mn, sa, sb = metrics.musical_noise(x, y, sr, n0, n1); row['flicker+'] = round(mn, 2)
        rows.append(row)
        if write: write_wav24(OUT + f'{key}__{label}.wav', sr, y); write_wav24(OUT + f'{key}__{label}_removed.wav', sr, rem)
    if write: write_wav24(OUT + f'{key}__input.wav', sr, x)
    if with_baseline:
        t = time.time(); y, rem = baseline.process(x, sr, [region], floor_db=floor_db); report('baseline', y, rem, time.time() - t)
    t = time.time(); prof = engine.learn_profile([region], sr, p); info = {}
    y, rem = engine.process(x, sr, prof, p, info); report('ours', y, rem, time.time() - t)
    rates = info['rates']
    print(key, 'decay rates dB/s per 1/3 oct (50 Hz up):', [round(r, 1) if not np.isnan(r) else None for r in rates])
    return rows


if __name__ == '__main__':
    keys = sys.argv[1:] or ['omni', 'spotC', 'spot3']
    allrows = []
    for k in keys: allrows += run(k)
    import bench; bench.table(allrows)
