"""Makes tests/data/denoise (golden_in.wav, golden_out.wav, golden.json): the reference engine's output for a 6 s, 48 kHz case that the C++ port
(src/core/Denoise.h) must reproduce - see the "2.77 background denoiser" section of tests/core_test.cpp. Run from this folder: python3 make_golden.py"""
import sys, os, json, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import synth, engine, metrics
from audio import write_wav24
SR = 48000
DEST = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tests', 'data', 'denoise')
os.makedirs(DEST, exist_ok=True)
sig = synth.chord(SR, 1.2, [196.0, 247.0, 330.0], nharm=60, brightness=0.8, vib_cents=10.0) * 10 ** ((-24.0 + 6.0) / 20.0)
ir = synth.church_ir(SR, rt60_by_band=((63, 5.0), (125, 5.0), (250, 4.5), (500, 4.0), (1000, 3.5), (2000, 2.8), (4000, 2.0), (8000, 1.3), (16000, 0.8)))
wet = synth.reverb(sig, ir, wet=0.6)
clean = np.concatenate([np.zeros(int(1.4 * SR)), wet])[:6 * SR]
clean = np.concatenate([clean, np.zeros(6 * SR - len(clean))])
noise = synth.at_db(synth.synthetic_room_tone(SR, 6.1, 'distant', seed=11), -48.0)[:6 * SR]
q = np.round(np.clip(clean + noise, -1, 1 - 1 / 8388608) * 8388608.0) / 8388608.0      # (24-bit, as the C++ side reads it)
region = q[int(0.1 * SR):int(1.3 * SR)]
p = engine.Params(); prof = engine.learn_profile([region], SR, p); info = {}
y, rem = engine.process(q, SR, prof, p, info)
write_wav24(os.path.join(DEST, 'golden_in.wav'), SR, q); write_wav24(os.path.join(DEST, 'golden_out.wav'), SR, y)
psd = prof.npsd; N = prof.n_fft; s = psd[0] + psd[-1] + 2 * psd[1:-1].sum()
lead = lambda x: 20 * np.log10(np.sqrt(np.mean(x[int(0.1 * SR):int(1.3 * SR)] ** 2)))
gold = dict(sampleRate=SR, nfft=N, region=[0.1, 1.3], rates=[None if np.isnan(r) else round(float(r), 3) for r in info['rates']],
            edges=[round(float(e), 3) for e in info['edges']], noiseReductionDb=round(metrics.noise_reduction(q, y, SR, 0.1, 1.3), 3),
            inLeadDb=round(float(lead(q)), 3), outLeadDb=round(float(lead(y)), 3), profileLevelDb=round(float(10 * np.log10(s / (N * 0.375 * N))), 3),
            outRmsDb=round(float(20 * np.log10(np.sqrt(np.mean(y ** 2)))), 4))
json.dump(gold, open(os.path.join(DEST, 'golden.json'), 'w'), indent=1)
print(json.dumps(gold))
