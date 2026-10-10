"""Makes the reference ("golden") results of the Python Re-HarmoniSer engine on a made-up choir signal, so the C++ port can be
checked against them.   Usage:  python3 make_golden.py <output folder>
The signal: 8 channels, 48 kHz, 8 s. Five voices sing A3 with vibrato (one of them 35 cents sharp), each mic hears each voice
with its own level and delay, plus a little noise. Everything is deterministic (fixed seed)."""
import json, os, sys
import numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "python_reference"))
import engine as E

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
rng = np.random.default_rng(12345)
sr, dur, nch = 48000, 8.0, 8
n = int(sr * dur)
t = np.arange(n) / sr
f_note = 220.0
offs = [0.0, 3.0, -4.0, 35.0, 6.0]          # cents each voice is from A3
vib_rate = [5.2, 5.6, 5.9, 5.4, 6.1]
vib_dep = [12.0, 10.0, 14.0, 15.0, 9.0]     # cents
env = np.clip((t - 1.0) / 0.1, 0, 1) * np.clip((6.0 - t) / 0.3, 0, 1)
env = env * env * (3 - 2 * env)
voice = []
for v in range(5):
    cents = offs[v] + vib_dep[v] * np.sin(2 * np.pi * vib_rate[v] * t + v)
    # the sharp voice starts higher and settles (like the real example)
    if v == 3:
        cents = cents + 40.0 * np.exp(-np.clip(t - 1.0, 0, None) / 1.0) * (t > 1.0)
    f = f_note * 2 ** (cents / 1200)
    ph = 2 * np.pi * np.cumsum(f) / sr
    s = np.zeros(n)
    for h in range(1, 9):
        s += np.sin(h * ph + 0.3 * h * v) / h ** 1.2
    voice.append(s * env)
data = np.zeros((n, nch))
gains = rng.uniform(0.2, 1.0, (nch, 5))
gains[0, 3] = 1.0; gains[1, 3] = 0.8                   # the sharp voice is close to mics 1 and 2
delays = rng.integers(0, 40, (nch, 5))
for c in range(nch):
    for v in range(5):
        d = int(delays[c, v])
        data[d:, c] += gains[c, v] * voice[v][:n - d] * 0.15
data += rng.normal(0, 3e-4, data.shape)
data = data.astype(np.float32)
data.astype("<f4").tofile(os.path.join(out, "input.f32"))

man = {"sr": sr, "n": n, "ch": nch}


def save(name, arr):
    a = np.asarray(arr, dtype="<f8")
    a.tofile(os.path.join(out, name + ".f64"))
    man[name] = list(a.shape)


# ---- the engine, step by step, the way Session.select / apply / audition use it
class Fake(E.Session):
    def __init__(self):
        self.sr, self.orig, self.n, self.ch = sr, data, n, nch
        self.cur = data.copy(); self.dur = dur; self.vis_gain = [0.0] * nch

s = Fake()
t0, t1, f_lo, f_hi = 2.0, 5.0, 200.0, 242.0
seg, s0 = s.seg(t0 - 0.8, t1 + 0.8)
man["s0"] = s0
chans, ws = s._tracking_inputs(seg, f_lo, f_hi)
order = []
for c in chans:
    for k in range(nch):
        if np.shares_memory(c, seg[:, k]) and np.array_equal(c, seg[:, k]):
            order.append(k); break
man["tracking_channels"] = order
save("weights", ws)
ts, f0 = E.track_note(chans, ws, sr, s0, t0, t1, f_lo, f_hi)
save("track_ts", ts); save("track_f0", f0)
f0r = E.refine_track(chans, ws, sr, s0, ts, f0)
save("refined_f0", f0r)
man["partial_hint"] = int(E.detect_partial(chans, ws, sr, s0, ts, f0r))
params = dict(move=0.0, snap=0.8, keep=0.2, smooth=0.25, ease=0.10, strength=1.0)
midi = 57
man["params"] = {**E.DEFAULTS, **params}
pl = E.plan_curve(ts, f0r, params, midi, None)
for k in ("c", "wob", "off", "planned"):
    save("plan_" + k, pl[k])
man["midi"] = midi
p = {**E.DEFAULTS, **params}
a, b = max(0.0, ts[0] - 0.15 - 0.3), min(dur, ts[-1] + p["hold"] + p["fade_out"] + 0.3)
seg2, s02 = s.seg(a, b)
man["retune_s0"] = s02
new = E.retune_segment(seg2.copy(), sr, s02, ts, f0r, pl["off"], p)
save("retuned", new)
man["retune_n"] = int(len(new))
# the same note, move only (no snap) and without level matching
p2 = {**p, "move": -25.0, "snap": 0.0, "match": False}
pl2 = E.plan_curve(ts, f0r, p2, midi, None)
save("plan2_off", pl2["off"])
new2 = E.retune_segment(seg2.copy(), sr, s02, ts, f0r, pl2["off"], p2)
save("retuned2", new2)
# isolate (solo)
solo = E.solo_segment(seg2.copy(), sr, s02, ts, f0r * 2 ** (pl["off"] / 1200), p, ts[0], ts[-1])
save("solo", solo)
# level report
band = (max(40.0, 0.8 * float(f0r.min())), min(0.45 * sr, 2.2 * float(f0r.max())))
rep = E.level_report(seg2, new, sr, s02, ts[0], ts[-1], band)
man["level_report"] = rep
# ---- voices
f_t = f_note
win = min(0.6, max(0.3, (t1 - t0) / 3.0))
seg3, s03 = s.seg(t0 - 0.6, t1 + 0.6)
vch, vws, sr2 = s._voice_inputs(seg3, f_t)
man["voice_sr2"] = sr2
vts, fp, grid, hist, cl = E.find_voices(vch, vws, sr2, s03, t0, t1, f_t, H=8, span=60.0, win=win, hop=win / 4)
save("voice_ts", vts); save("voice_hist", hist); save("voice_cl", cl)
C = E.track_voices(vts, fp, cl)
save("voice_C", C)
offs_v = [0.0] * len(cl)
means = [float(np.mean(C[j])) for j in range(len(cl))]
j_sharp = int(np.argmax(means))
offs_v[j_sharp] = -means[j_sharp]
save("voice_offs", offs_v)
a3, b3 = max(0.0, t0 - 0.8), min(dur, t1 + 0.8)
seg4, s04 = s.seg(a3, b3)
trk = s._voice_inputs(seg4, f_t)
man["voice_s0"] = s04
nv = E.retune_voices(seg4.copy(), sr, s04, f_t, np.asarray(vts, float), np.asarray(C, float), offs_v, t0, t1, trk)
save("voices_retuned", nv)
# ---- brush
brush = dict(pts=[[3.0, 330.0], [3.5, 335.0], [4.0, 340.0]], rt=0.2, rc=60.0, amount=1.0, harm=True)
ba, bz = E.brush_region(brush)
ba, bz = max(0.0, ba), min(dur, bz)
seg5, s05 = s.seg(ba, bz)
nb = E.brush_segment(seg5.copy(), sr, s05, brush, E.DEFAULTS["nharm"])
save("brush_out", nb); man["brush"] = brush; man["brush_s0"] = s05
# ---- the choir's reference pitch
T, D, W = s._ref_points(0.5, 7.5)
save("ref_T", T); save("ref_D", D); save("ref_W", W)
st = s._ref_stats(D, W)
man["ref_stats"] = st
# ---- spectrogram (whole file)
sp = E.Spectrogram(data, sr)
man["spec"] = dict(N=int(sp.N), hop=int(sp.hop), nb=int(sp.nb), nf=int(sp.nf), df=float(sp.df), sr2=int(sp.sr2), ref=float(sp.ref))
for c in range(nch):
    sp.db[c].astype(np.float64).tofile(os.path.join(out, "spec_ch%d.f64" % c))
json.dump(man, open(os.path.join(out, "manifest.json"), "w"), indent=1, default=lambda o: o.item() if hasattr(o, "item") else str(o))
print("done", out)
