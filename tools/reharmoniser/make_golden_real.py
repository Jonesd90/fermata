"""Reference results of the Python engine on REAL clips (96 kHz), for checking the C++ port.
Usage: python3 make_golden_real.py <folder with the unzipped Test_audio_* folders> <output folder>"""
import glob, os, sys
import numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "python_reference"))
import engine as E

src, out = sys.argv[1], sys.argv[2]
# name, folder, note box (excerpt seconds), pitch range Hz, midi, A4 of the choir, find voices?
CASES = [
    ("iceshining", "Test_audio_IceShining_57-71s", 4.5, 6.5, 150.0, 175.0, 52, 432.0, True),
    ("051", "Test_audio_051_33-47s", 6.0, 7.2, 200.0, 242.0, 57, 440.0, False),
    ("006", "Test_audio_006_8s-to-end", 4.9, 6.3, 330.0, 370.0, 65, 440.0, True),
    ("diarists_sop", "Test_audio_Diarists_9-20s", 8.5, 9.5, 490.0, 560.0, 72, 441.5, False),
    ("diarists_ten", "Test_audio_Diarists_9-20s", 1.2, 2.2, 180.0, 210.0, 55, 441.5, False),
]


def put(folder, name, arr):
    np.asarray(arr, dtype="<f8").tofile(os.path.join(folder, name + ".f64"))


for name, sub, t0, t1, flo, fhi, midi, a4, voices in CASES:
    folder = os.path.join(out, name); os.makedirs(folder, exist_ok=True)
    paths = sorted(glob.glob(os.path.join(src, sub, "*.wav")))
    parts = [E.load_wav(p) for p in paths]
    sr = parts[0][0]; n = max(p[1].shape[0] for p in parts)
    data = np.zeros((n, sum(p[1].shape[1] for p in parts)), np.float32); c = 0
    for _, x, _ in [(p[0], p[1], p[2]) for p in parts]:
        data[:x.shape[0], c:c + x.shape[1]] = x; c += x.shape[1]
    nch = data.shape[1]; dur = n / sr
    data.astype("<f4").tofile(os.path.join(folder, "input.f32"))

    class Fake(E.Session):
        def __init__(self):
            self.sr, self.orig, self.n, self.ch = sr, data, n, nch
            self.cur = data.copy(); self.dur = dur; self.vis_gain = [0.0] * nch
    s = Fake()
    lines = ["sr %d" % sr, "n %d" % n, "ch %d" % nch, "t0 %.17g" % t0, "t1 %.17g" % t1, "flo %.17g" % flo, "fhi %.17g" % fhi, "midi %d" % midi, "a4 %.17g" % a4, "voices %d" % int(voices)]
    seg, s0 = s.seg(t0 - 0.8, t1 + 0.8)
    chans, ws = s._tracking_inputs(seg, flo, fhi)
    order = []
    for ch in chans:
        for k in range(nch):
            if np.array_equal(ch, seg[:, k]): order.append(k); break
    lines.append("tracking " + " ".join(map(str, order)))
    put(folder, "weights", ws)
    ts, f0 = E.track_note(chans, ws, sr, s0, t0, t1, flo, fhi); put(folder, "track_f0", f0)
    f0r = E.refine_track(chans, ws, sr, s0, ts, f0); put(folder, "refined_f0", f0r)
    lines.append("hint %d" % int(E.detect_partial(chans, ws, sr, s0, ts, f0r)))
    params = dict(move=0.0, snap=0.8, keep=0.2, smooth=0.25, ease=0.10, strength=1.0, a4=a4)
    pl = E.plan_curve(ts, f0r, params, midi, None)
    put(folder, "plan_off", pl["off"])
    p = {**E.DEFAULTS, **params}
    a, b = max(0.0, ts[0] - 0.15 - 0.3), min(dur, ts[-1] + p["hold"] + p["fade_out"] + 0.3)
    seg2, s02 = s.seg(a, b)
    new = E.retune_segment(seg2.copy(), sr, s02, ts, f0r, pl["off"], p)
    put(folder, "retuned", new)
    lines += ["ra %.17g" % a, "rb %.17g" % b]
    if voices:
        f_t = a4 * 2 ** ((midi - 69) / 12)
        win = min(0.6, max(0.3, (t1 - t0) / 3.0))
        seg3, s03 = s.seg(t0 - 0.6, t1 + 0.6)
        vch, vws, sr2 = s._voice_inputs(seg3, f_t)
        vts, fp, grid, hist, cl = E.find_voices(vch, vws, sr2, s03, t0, t1, f_t, H=8, span=60.0, win=win, hop=win / 4)
        put(folder, "voice_ts", vts); put(folder, "voice_hist", hist); put(folder, "voice_cl", cl)
        C = E.track_voices(vts, fp, cl); put(folder, "voice_C", C)
        offs = [(-float(np.mean(C[j])) if abs(float(np.mean(C[j]))) > 8 else 0.0) for j in range(len(cl))]
        put(folder, "voice_offs", offs)
        a3, b3 = max(0.0, t0 - 0.8), min(dur, t1 + 0.8)
        seg4, s04 = s.seg(a3, b3)
        trk = s._voice_inputs(seg4, f_t)
        nv = E.retune_voices(seg4.copy(), sr, s04, f_t, np.asarray(vts, float), np.asarray(C, float), offs, t0, t1, trk)
        put(folder, "voices_retuned", nv)
        lines += ["va %.17g" % a3, "vb %.17g" % b3, "nvoices %d" % len(cl)]
    T, D, W = s._ref_points(0.0, dur)
    put(folder, "ref_T", T); put(folder, "ref_D", D)
    st = s._ref_stats(D, W)
    lines.append("ref %.17g %.17g" % (st["a4"] if st else 0.0, st["spread"] if st else 0.0) if st else "ref 0 0")
    open(os.path.join(folder, "case.txt"), "w").write("\n".join(lines) + "\n")
    print("done", name, flush=True)
