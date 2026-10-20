"""The bench: scenarios with known truth, scored for ours (parameter variants) and the conventional baseline."""
import os, sys, time, glob, json, numpy as np
from audio import read_wav, write_wav24, rms_db, UPLOADS
import synth, engine, baseline, metrics

SR = 96000
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'out') + os.sep
os.makedirs(OUT, exist_ok=True)


def scenario_chord(noise_kind='synthetic', noise_db=-45.0, brightness=0.9, vib_cents=12.0, dur=5.0, lead=2.0, trail=3.0, sig_db=-6.0):
    sig = synth.chord(SR, dur, [196.0, 247.0, 330.0], nharm=80, brightness=brightness, vib_cents=vib_cents) * 10 ** ((sig_db + 6.0) / 20.0)
    clean = np.concatenate([np.zeros(int(lead * SR)), sig, np.zeros(int(trail * SR))])
    total = len(clean) / SR
    unit = synth.real_room_tone(SR, total + 0.1) if noise_kind == 'real' else synth.synthetic_room_tone(SR, total + 0.1, 'distant' if noise_db > -55 else 'close')
    noise = synth.at_db(unit, noise_db)[:len(clean)]
    return dict(name=f'chord_{noise_kind}_n{int(noise_db)}_s{int(sig_db)}_v{int(vib_cents)}', clean=clean, noise=noise,
                noise_region=(0.0, lead - 0.1), tail=None, sig_region=(lead, lead + dur), quiet_region=(lead + dur + 0.5, total - 0.05))


def scenario_tail(noise_kind='real', noise_db=-50.0, burst=1.5, lead=1.5):
    sig = synth.chord(SR, burst, [131.0, 196.0, 262.0, 330.0], nharm=60, brightness=0.8, vib_cents=0.0)
    ir = synth.church_ir(SR)
    wet = synth.reverb(sig, ir, wet=0.6)
    clean = np.concatenate([np.zeros(int(lead * SR)), wet])
    total = len(clean) / SR
    unit = synth.real_room_tone(SR, total + 0.1) if noise_kind == 'real' else synth.synthetic_room_tone(SR, total + 0.1, 'distant')
    noise = synth.at_db(unit, noise_db)[:len(clean)]
    return dict(name=f'tail_{noise_kind}_{int(noise_db)}dB', clean=clean, noise=noise, noise_region=(0.0, lead - 0.1),
                tail=(lead + burst + 0.1, total - 0.1), sig_region=(lead, lead + burst), quiet_region=None)


def scenario_real_spot(noise_db=-50.0, spot='Ice_Shining_020_4-C', seconds=30.0, start=0.0):
    sr, x = read_wav(glob.glob(UPLOADS + '*' + spot + '.wav')[0])
    clean = x[int(start * sr):int((start + seconds) * sr)]
    noise = synth.at_db(synth.real_room_tone(SR, len(clean) / SR + 0.1, seed=7), noise_db)[:len(clean)]
    return dict(name=f'realspot_{int(noise_db)}dB', clean=clean, noise=noise, noise_region=(0.0, 2.3), tail=None, sig_region=(3.0, seconds), quiet_region=(0.0, 2.3))


def run(sc, variants, with_baseline=True, write=False, floor_db=-12.0):
    clean, noise = sc['clean'], sc['noise']
    noisy = clean + noise
    sr = SR
    r0, r1 = sc['noise_region']
    regions = [noisy[int(r0 * sr):int(r1 * sr)]]
    rows = []
    def score(name, y, removed, secs):
        row = dict(name=name, secs=round(secs, 1))
        row['snr_in'] = round(metrics.snr_db(clean, noisy), 1); row['snr_out'] = round(metrics.snr_db(clean, y), 1)
        hp, nb = metrics.harmonic_preservation(clean, y, sr, f_lo=3000.0)
        row['harm3k+'] = round(hp, 2)
        row['b1-4k'] = round(metrics.band_energy_ratio(clean, y, sr, 1000, 4000), 2)
        row['b4-8k'] = round(metrics.band_energy_ratio(clean, y, sr, 4000, 8000), 2)
        row['b8-16k'] = round(metrics.band_energy_ratio(clean, y, sr, 8000, 16000), 2)
        q = sc['quiet_region'] or sc['noise_region']
        row['nr_dB'] = round(metrics.noise_reduction(noisy, y, sr, q[0], q[1]), 1)
        mn, sa, sb = metrics.musical_noise(noisy, y, sr, q[0], q[1]); row['flicker+'] = round(mn, 2)
        if sc['tail']:
            t0, t1 = sc['tail']
            if 'oracle' not in sc: sc['oracle'] = metrics.oracle(clean, noise, sr, floor_db)
            for lab, (lo, hi) in dict(full=(None, None), mid=(500, 2000), hi=(2000, 8000)).items():
                tm = metrics.tail_metrics(clean, noise, y, sr, floor_db, t0, t1, lo, hi, ideal=sc['oracle'])
                row[f'{lab}_err'] = round(tm['mean_err'], 1); row[f'{lab}_drop350'] = round(tm['worst_drop350'], 1); row[f'{lab}_cut'] = round(tm['cutoff_t'], 1)
                sc.setdefault('curves', {})[name + '_' + lab] = tm['curve']
        rows.append(row)
        if write:
            write_wav24(OUT + f"{sc['name']}__{name}.wav", sr, y); write_wav24(OUT + f"{sc['name']}__{name}_removed.wav", sr, removed)
    if write: write_wav24(OUT + f"{sc['name']}__noisy.wav", sr, noisy); write_wav24(OUT + f"{sc['name']}__clean.wav", sr, clean)
    if with_baseline:
        t = time.time(); y = metrics.oracle(clean, noise, sr, floor_db); sc['oracle'] = y; score('oracle', y, noisy - y, time.time() - t)
        t = time.time(); y, rem = baseline.process(noisy, sr, regions, floor_db=floor_db); score('baseline', y, rem, time.time() - t)
    for vname, p in variants.items():
        p.residual_db = floor_db
        t = time.time(); prof = engine.learn_profile(regions, sr, p); info = {}
        y, rem = engine.process(noisy, sr, prof, p, info); score(vname, y, rem, time.time() - t)
        sc.setdefault('info', {})[vname] = dict(rates=info['rates'].tolist(), edges=info['edges'].tolist())
    return rows


def table(rows):
    keys = list(rows[0].keys())
    print(' | '.join(f'{k:>12}' for k in keys))
    for r in rows: print(' | '.join(f'{str(r.get(k, "")):>12}' for k in keys))


if __name__ == '__main__':
    which = sys.argv[1:] or ['chord', 'tail']
    variants = {'ours': engine.Params(), 'ours_nopres': engine.Params(presence=False)}
    if 'chord' in which:
        for kind, lvl, sig in [('synthetic', -45.0, -6.0), ('synthetic', -45.0, -36.0), ('real', -40.0, -36.0), ('synthetic', -45.0, -48.0)]:
            sc = scenario_chord(kind, lvl, sig_db=sig); print('\n==', sc['name'], 'noise', round(rms_db(sc['noise']), 1), 'dB, signal', round(rms_db(sc['clean']), 1), 'dB')
            table(run(sc, variants, write='write' in which))
    if 'tail' in which:
        for kind, lvl, floor in [('real', -50.0, -12.0), ('real', -50.0, -24.0), ('synthetic', -45.0, -24.0)]:
            sc = scenario_tail(kind, lvl); print('\n==', sc['name'], 'floor', floor)
            rows = run(sc, variants, write='write' in which, floor_db=floor); table(rows)
            print('decay rates measured (dB/s per 1/3 oct from 50 Hz):', [round(r, 1) if not np.isnan(r) else None for r in sc['info']['ours']['rates']])
    if 'spot' in which:
        for lvl in [-50.0, -40.0]:
            sc = scenario_real_spot(lvl); print('\n==', sc['name'])
            table(run(sc, variants, write='write' in which))
