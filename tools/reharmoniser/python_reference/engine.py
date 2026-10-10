"""Engine for the Choral Pitch Tool (prototype).

Core idea
---------
A note is followed through its harmonic partials with a time-varying f0 track.
Each partial is demodulated per channel (so each channel keeps its own complex
gain, i.e. mic-to-mic level/phase relationships are preserved), subtracted, and
added back at the corrected pitch. Everything outside narrow bands around those
partials is left untouched. The correction is held (not tracked) through the
room's ring-out after the note ends.

Everything works on short segments of the file so long multitrack files are OK.
"""
import io
import os
import struct
import threading
import wave
import warnings
import zlib

import numpy as np
import scipy.fft as sfft
import scipy.io.wavfile as wavfile
import scipy.signal as sg
from scipy.ndimage import gaussian_filter1d, median_filter

warnings.filterwarnings("ignore")

NOTE_NAMES = ["C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"]


# ----------------------------------------------------------------- pitch utils
def midi_of(f, a4=440.0):
    return 69 + 12 * np.log2(f / a4)


def midi_hz(m, a4=440.0):
    return a4 * 2 ** ((m - 69) / 12)


def note_label(m):
    m = int(round(m))
    return f"{NOTE_NAMES[m % 12]}{m // 12 - 1}"


# ------------------------------------------------------------------- WAV I/O
def wav_info(path):
    with open(path, "rb") as f:
        head = f.read(12)
        if head[:4] not in (b"RIFF", b"RF64") or head[8:12] != b"WAVE":
            raise ValueError("Not a WAV file")
        while True:
            h = f.read(8)
            if len(h) < 8:
                break
            cid, sz = h[:4], struct.unpack("<I", h[4:])[0]
            if cid == b"fmt ":
                d = f.read(sz)
                tag, ch, sr, _, _, bits = struct.unpack("<HHIIHH", d[:16])
                if tag == 0xFFFE and sz >= 26:
                    tag = struct.unpack("<H", d[24:26])[0]
                return dict(tag=tag, channels=ch, sr=sr, bits=bits)
            f.seek(sz + (sz & 1), 1)
    raise ValueError("WAV has no fmt chunk")


def load_wav(path):
    info = wav_info(path)
    sr, x = wavfile.read(path)
    if x.ndim == 1:
        x = x[:, None]
    if x.dtype == np.uint8:
        f = (x.astype(np.float32) - 128) / 128
    elif x.dtype == np.int16:
        f = x.astype(np.float32) / 32768
    elif x.dtype == np.int32:  # 24-bit is left-aligned in an int32
        f = x.astype(np.float32) * np.float32(1 / 2147483648)
    else:
        f = x.astype(np.float32)
    return int(sr), np.ascontiguousarray(f), info


def write_wav(path, sr, data, info):
    bits, tag = info["bits"], info["tag"]
    if tag == 3:
        wavfile.write(path, sr, data.astype(np.float32))
    elif bits == 16:
        wavfile.write(path, sr, np.clip(np.round(data * 32768), -32768, 32767).astype(np.int16))
    elif bits == 24:
        v = np.clip(np.round(data.astype(np.float64) * 2 ** 23), -2 ** 23, 2 ** 23 - 1).astype("<i4")
        raw = v.reshape(-1).view(np.uint8).reshape(-1, 4)[:, :3].tobytes()
        with wave.open(path, "wb") as f:
            f.setnchannels(data.shape[1])
            f.setsampwidth(3)
            f.setframerate(sr)
            f.writeframes(raw)
    else:
        v = np.clip(np.round(data.astype(np.float64) * 2147483648), -2 ** 31, 2 ** 31 - 1).astype(np.int32)
        wavfile.write(path, sr, v)


def default_pans(ch, layout=None):
    """Where each channel sits in the monitor mix to begin with. Mono files go in the centre; a stereo file's
    left channel goes hard left and its right channel hard right. (layout = how many channels each source file
    has, e.g. [1, 1, 2, 1]; any other channel count sits in the centre for you to place.)"""
    layout = layout if layout and sum(layout) == ch else [ch]
    out = []
    for k in layout:
        out += [-1.0, 1.0] if k == 2 else [0.0] * k
    return out


def db_lin(db):
    db = np.asarray(db, float)
    return np.where(db <= -59.5, 0.0, 10 ** (db / 20))


def monitor_mix(data, gain_db=None, pan=None):
    """(n, ch) -> (n, 2): a gain and a constant-power pan for every channel."""
    n, ch = data.shape
    g = db_lin(gain_db if gain_db is not None else [0.0] * ch)
    pn = np.asarray(pan if pan is not None else default_pans(ch), float)
    th = (np.clip(pn, -1, 1) + 1) * np.pi / 4
    head = 1.0 if ch <= 2 else np.sqrt(2.0 / ch)
    cl, cr = g * np.cos(th) * head, g * np.sin(th) * head
    if ch == 1:  # a single channel: centre, same level as a stereo pair's channel at centre
        cl = cr = g * 0.7071 * 1.4142
    return np.stack([data @ cl, data @ cr], axis=1)


def wav_bytes(data, sr, max_sr=48000, mix=None):
    """Stereo 16-bit monitor mix of (n, ch) audio, for browser playback. mix = (gain_db, pan) lists."""
    st = monitor_mix(data.astype(np.float64), *(mix or (None, None)))
    if sr > max_sr:
        q = int(np.ceil(sr / max_sr))
        st = sg.resample_poly(st, 1, q, axis=0)
        sr = sr // q
    pcm = np.clip(np.round(st * 32767), -32768, 32767).astype("<i2")
    buf = io.BytesIO()
    with wave.open(buf, "wb") as f:
        f.setnchannels(2)
        f.setsampwidth(2)
        f.setframerate(sr)
        f.writeframes(pcm.tobytes())
    return buf.getvalue()


# ----------------------------------------------------------------- spectrogram
def decimate_to(x, sr, target=24000):
    q = int(np.ceil(sr / target))
    if q <= 1:
        return x.astype(np.float32), int(sr)
    return sg.resample_poly(x, 1, q).astype(np.float32), int(sr // q)


def _png(rgb):
    h, w, _ = rgb.shape
    raw = np.concatenate([np.zeros((h, 1), np.uint8), rgb.reshape(h, w * 3)], axis=1).tobytes()

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 3)) + chunk(b"IEND", b""))


def _png_gray(g):
    h, w = g.shape
    raw = np.concatenate([np.zeros((h, 1), np.uint8), g], axis=1).tobytes()

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 3)) + chunk(b"IEND", b""))


def _lut():
    """Fermata's 'Standard' heat palette (dark blue - purple - magenta - orange - pale yellow)."""
    stops = [(0.0, (0, 0, 13)), (0.25, (51, 0, 102)), (0.5, (191, 26, 89)), (0.75, (255, 140, 26)), (1.0, (255, 255, 204))]
    xs = [s[0] for s in stops]
    lut = np.zeros((256, 3), np.uint8)
    for k in range(3):
        lut[:, k] = np.interp(np.linspace(0, 1, 256), xs, [s[1][k] for s in stops])
    return lut


LUT = _lut()

# "Detail" choices: length (s) of the long analysis window used for the low and middle pitches.
DETAILS = {"pitch": 0.34, "balanced": 0.17, "time": 0.085}


POW = (10 ** ((np.arange(256) / 2.0 - 127.5) / 10)).astype(np.float32)  # uint8 level -> power


class Spectrogram:
    """Two analysis windows blended, the way Fermata's spectral windows do it: a long window
    (fine pitch resolution) for the low and middle pitches, a short one (sharp in time) for the
    top, with a smooth cross-over between them.

    Every channel gets its own picture, stored as 0..255 = -127.5..0 dBFS. The picture you see is
    the channels added up by POWER (not waveform, so spaced microphones cannot cancel each other),
    each with its own 'see' gain from the visual mixer."""
    FMAX = 8000.0
    SHORT_S = 0.043
    CROSS_LO, CROSS_HI = 500.0, 3000.0
    MAX_FRAMES = 7000

    def __init__(self, data, sr, detail="balanced"):
        self.sr = sr
        self.detail = detail
        self.ch = data.shape[1]
        self.xs = []
        for c in range(self.ch):
            x2, self.sr2 = decimate_to(data[:, c], sr)
            self.xs.append(x2)
        self.gain = np.ones(self.ch)
        self._build()

    def _one(self, x2, geo):
        NL, NS, hop, nb, wl, s0, sw, nf = geo
        pad = NL // 2
        xp = np.r_[np.zeros(pad, np.float32), x2, np.zeros(NL, np.float32)]
        winL, winS = np.hanning(NL).astype(np.float32), np.hanning(NS).astype(np.float32)
        nL, nS = 2.0 / winL.sum(), 2.0 / winS.sum()
        gain = 10 * np.log10(NL / NS) * 0.5  # noise reads lower in a long window: make up half of it
        out = np.empty((nf, nb), np.uint8)
        B = 128
        for a in range(0, nf, B):
            ks = np.arange(a, min(nf, a + B))
            iL = ks * hop
            LV = np.stack([xp[i:i + NL] for i in iL]) * winL
            SV = np.stack([xp[i + NL // 2 - NS // 2:i + NL // 2 + NS // 2] for i in iL]) * winS
            ml = np.abs(np.fft.rfft(LV, axis=1))[:, :nb] * nL
            ms = np.abs(np.fft.rfft(SV, axis=1)) * nS
            dl = 20 * np.log10(ml + 1e-9) + gain
            ds = 20 * np.log10(ms + 1e-9)
            ds_i = ds[:, s0] * (1 - sw) + ds[:, s0 + 1] * sw
            db = wl * dl + (1 - wl) * ds_i
            out[a:a + len(ks)] = np.clip((db + 127.5) * 2, 0, 255).astype(np.uint8)
        return out

    def _build(self):
        sr2 = self.sr2
        NL = int(2 ** round(np.log2(DETAILS.get(self.detail, 0.17) * sr2)))
        NS = min(int(2 ** round(np.log2(self.SHORT_S * sr2))), NL)
        self.N = NL
        n = len(self.xs[0])
        self.hop = max(int(round(0.01 * sr2)), int(np.ceil(n / self.MAX_FRAMES)))
        dfL, dfS = sr2 / NL, sr2 / NS
        self.df = dfL
        self.nb = int(min(self.FMAX, sr2 / 2) / dfL) + 2
        fax = np.arange(self.nb) * dfL
        u = np.clip(np.log(np.maximum(fax, 1) / self.CROSS_LO) / np.log(self.CROSS_HI / self.CROSS_LO), 0, 1)
        wl = (1 - u * u * (3 - 2 * u)).astype(np.float32)
        sp = np.clip(fax / dfS, 0, NS // 2 - 1.001)
        s0 = sp.astype(int)
        sw = (sp - s0).astype(np.float32)
        self.nf = n // self.hop + 1
        geo = (NL, NS, self.hop, self.nb, wl, s0, sw, self.nf)
        if self.ch > 1:
            from concurrent.futures import ThreadPoolExecutor
            cnt = [0]

            def work(x):
                r = self._one(x, geo)
                with _PL:
                    cnt[0] += 1
                    _prog("Analysing channel %d of %d" % (cnt[0], self.ch), 0.2 + 0.8 * cnt[0] / self.ch)
                return r
            with ThreadPoolExecutor(min(self.ch, 8)) as ex:
                self.db = list(ex.map(work, self.xs))
        else:
            self.db = [self._one(self.xs[0], geo)]
        self._ref()

    def _combine(self, cols):
        """list of (w, nb) uint8 -> (w, nb) float32 dBFS, channels added by power with their gains."""
        P = None
        for c, a in enumerate(cols):
            g2 = float(self.gain[c]) ** 2
            if g2 <= 0:
                continue
            p = POW[a] * g2
            P = p if P is None else P + p
        if P is None:
            return np.full(cols[0].shape, -200.0, np.float32)
        return 10 * np.log10(P + 1e-20)

    def _ref(self):
        step = max(1, self.nf // 400)
        d = self._combine([a[::step] for a in self.db])
        self.ref = float(np.percentile(d, 99.7))

    def set_gains(self, g):
        g = np.asarray(g, float)
        if len(g) == self.ch and not np.allclose(g, self.gain):
            self.gain = g
            self.rev = getattr(self, "rev", 0) + 1
            self._ref()

    def set_detail(self, detail):
        if detail in DETAILS and detail != self.detail:
            self.detail = detail
            self.rev = getattr(self, "rev", 0) + 1
            self._build()

    MASTER_FMIN, MASTER_PPO = 30.0, 360  # the whole-file picture the interface keeps in memory: lowest pitch, rows per octave

    def master_geo(self):
        fmax = float(min(self.FMAX, self.sr2 / 2 * 0.95))
        rows = int(round(np.log2(fmax / self.MASTER_FMIN) * self.MASTER_PPO))
        return dict(frames=int(self.nf), fps=float(self.sr2 / self.hop), fmin=self.MASTER_FMIN, fmax=fmax, rows=rows,
                    detail=self.detail, rev=getattr(self, "rev", 0))

    def master_cols(self, c0, c1):
        """Grey levels for frames c0..c1-1 over the whole master pitch range: (rows, c1-c0) uint8."""
        g = self.master_geo()
        c0, c1 = max(0, int(c0)), min(self.nf, int(c1))
        k = c1 - c0
        if k <= 0:
            return np.zeros((g["rows"], 0), np.uint8)
        fps = g["fps"]
        if k == 1:
            return self._grey(c0 / fps, c0 / fps + 1e-6, g["fmin"], g["fmax"], 1, g["rows"])
        return self._grey(c0 / fps, (c1 - 1) / fps, g["fmin"], g["fmax"], k, g["rows"])

    def render(self, t0, t1, fmin, fmax, w, h, rng=70.0):
        """Image of [t0,t1] x [fmin,fmax] (log frequency) as a grey-level PNG."""
        return _png_gray(self._grey(t0, t1, fmin, fmax, w, h))

    def _grey(self, t0, t1, fmin, fmax, w, h):
        fps = self.sr2 / self.hop
        t = np.linspace(t0, t1, w)
        fi = t * self.sr2 / self.hop
        valid = (fi >= -0.5) & (fi <= self.nf - 0.5)
        if (t1 - t0) * fps > w * 1.5:  # zoomed out: max over the frames each pixel covers
            edges = np.linspace(t0, t1, w + 1) * self.sr2 / self.hop
            st = np.clip(np.floor(edges[:-1]).astype(int), 0, self.nf - 1)
            st = np.maximum.accumulate(st)
            keep = np.r_[True, np.diff(st) > 0]
            ids = np.nonzero(keep)[0]
            cols = []
            for a in self.db:
                red = np.maximum.reduceat(a[st[ids[0]]:], st[ids] - st[ids[0]], axis=0)
                col = np.empty((w, self.nb), np.uint8)
                col[:] = red[np.cumsum(keep) - 1]
                cols.append(col)
        else:
            idx = np.clip(np.round(fi).astype(int), 0, self.nf - 1)
            cols = [a[idx] for a in self.db]
        f = fmax * (fmin / fmax) ** ((np.arange(h) + 0.5) / h)
        bi = np.clip(f / self.df, 0, self.nb - 1.001)
        b0 = np.floor(bi).astype(int)
        wt = (bi - b0).astype(np.float32)[None, :]
        cols = [c[:, b0] for c in cols], [c[:, b0 + 1] for c in cols]
        dB = self._combine_pair(cols, wt)
        # grey levels: 0..255 = 110 dB below the loudest sound .. 10 dB above it. The interface turns them into
        # colours (the colour roll), so changing colours or gain never needs a new picture from here.
        v = np.clip((dB - self.ref + 110.0) / 120.0, 0, 1)
        v[~valid] = 0
        return (v.T * 255).astype(np.uint8)

    def _combine_pair(self, cols, wt):
        lo, hi = cols
        d0, d1 = self._combine(lo), self._combine(hi)
        return d0 * (1 - wt) + d1 * wt


# --------------------------------------------------------------- note tracking
def _comb(sp, fax, fcs, nharm):
    s = np.zeros(len(fcs))
    for h in range(1, nharm + 1):
        s += np.log(np.interp(fcs * h, fax, sp) + 1e-9)
    return s


def track_note(chans, ws, sr, t_seg0, t0, t1, f_lo, f_hi, hop_s=0.02):
    """Harmonic-comb f0 track inside [t0, t1] for a fundamental somewhere in [f_lo, f_hi].
    chans: list of 1-D channels, ws: their weights. The spectra are added by power (not the waveforms)."""
    dec = [decimate_to(c, sr) for c in chans]
    sr2 = dec[0][1]
    x2s = [d[0] for d in dec]
    ws = np.asarray(ws, float)
    N = int(2 ** round(np.log2(0.34 * sr2)))
    pad = 4
    win = np.hanning(N)
    fax = np.arange(N * pad // 2 + 1) * sr2 / (N * pad)
    nharm = int(max(1, min(6, 0.42 * sr2 / f_hi)))

    def spec_at(t):
        i = int(round((t - t_seg0) * sr2)) - N // 2
        if i < 0 or i + N > len(x2s[0]):
            return None
        P = 0.0
        for x2, w in zip(x2s, ws):
            P = P + (w * np.abs(np.fft.rfft(x2[i:i + N] * win, N * pad))) ** 2
        return np.sqrt(P)

    span = max(1.0, 1200 * np.log2(f_hi / f_lo))
    cand = f_lo * 2 ** (np.arange(0, span + 3, 3) / 1200)
    acc, n = np.zeros(len(cand)), 0
    for t in np.arange(t0, t1, 0.05):
        sp = spec_at(t)
        if sp is not None:
            acc += _comb(sp, fax, cand, nharm)
            n += 1
    if n == 0:
        raise ValueError("Selection is too close to the start/end of the file.")
    fc = cand[int(np.argmax(acc))]
    grid = fc * 2 ** (np.arange(-70, 70.1, 0.5) / 1200)
    ts = np.arange(t0, t1, hop_s)
    f0 = np.full(len(ts), np.nan)
    for k, t in enumerate(ts):
        sp = spec_at(t)
        if sp is not None:
            f0[k] = grid[int(np.argmax(_comb(sp, fax, grid, nharm)))]
    ok = ~np.isnan(f0)
    if ok.sum() < 5:
        raise ValueError("Could not track a pitch in that selection.")
    f0 = np.interp(ts, ts[ok], f0[ok])
    f0 = gaussian_filter1d(median_filter(f0, 5, mode="nearest"), 1.5, mode="nearest")
    return ts, f0


def detect_partial(chans, ws, sr, t_seg0, ts, f0):
    """Does the measured line look like an overtone rather than the fundamental? Looks for energy below it at the
    spacing of a harmonic series (f/k, 2f/k ... for k = 4, 3, 2). Returns k (the partial it probably is) or 1."""
    try:
        dec = [decimate_to(c, sr) for c in chans]
        sr2 = dec[0][1]
        x2s = [d[0] for d in dec]
        N = int(2 ** round(np.log2(0.34 * sr2)))
        pad = 4
        win = np.hanning(N)
        fax = np.arange(N * pad // 2 + 1) * sr2 / (N * pad)
        fm = float(np.median(f0))
        P = np.zeros(len(fax))
        n = 0
        for t in np.linspace(ts[0], ts[-1], 30):
            i = int(round((t - t_seg0) * sr2)) - N // 2
            if i < 0 or i + N > len(x2s[0]):
                continue
            for x2, w in zip(x2s, ws):
                P += (w * np.abs(np.fft.rfft(x2[i:i + N] * win, N * pad))) ** 2
            n += 1
        if n < 3:
            return 1
        db = 10 * np.log10(P / n + 1e-18)

        def level(f):
            m = (fax >= f * 2 ** (-60 / 1200)) & (fax <= f * 2 ** (60 / 1200))
            return float(db[m].max()) if m.any() else -999.0

        for k in (4, 3, 2):
            if fm / k < 45.0:
                continue
            m = (fax >= 0.8 * fm / k) & (fax <= 1.1 * fm)
            floor = float(np.median(db[m]))
            Lk = level(fm)
            L = [level(fm * j / k) for j in range(1, k)]
            if Lk < floor + 6:
                continue
            if all(l >= floor + 6 for l in L) and L[0] >= Lk - 12 and all(l >= Lk - 20 for l in L):
                return k
    except Exception:
        pass
    return 1


def _lowpass(z, sr, bw):
    """Zero-phase Gaussian low-pass (bw = approx. full width, Hz) on a complex signal."""
    L = len(z)
    nfft = sfft.next_fast_len(L + int(0.3 * sr))
    F = sfft.fftfreq(nfft, 1 / sr)
    sig = bw / 2.355 / 2
    return sfft.ifft(sfft.fft(z, nfft, workers=-1) * np.exp(-0.5 * (F / sig) ** 2), workers=-1)[:L] * 2


def refine_track(chans, ws, sr, t_seg0, ts, f0, bw=30, nh=4, iters=4, smooth_s=0.12):
    """Keep demodulated partials centred: use the residual instantaneous frequency of
    partials 1..nh (power-weighted) to correct the f0 track. Envelopes are narrow-band,
    so the slow part runs at ~400 Hz instead of the full sample rate."""
    f0 = gaussian_filter1d(f0, 4, mode="nearest")
    i0 = max(0, int((ts[0] - t_seg0) * sr))
    i1 = min(len(chans[0]), int((ts[-1] - t_seg0) * sr))
    tt = t_seg0 + np.arange(i0, i1) / sr
    sigs = [c[i0:i1].astype(np.float64) for c in chans]
    ws = np.asarray(ws, float)
    dec = max(1, int(sr // 400))
    td = tt[::dec]
    rate = sr / dec
    for _ in range(iters):
        phi = 2 * np.pi * np.cumsum(np.interp(tt, ts, f0)) / sr
        num, den = np.zeros(len(td)), np.zeros(len(td))
        for h in range(1, nh + 1):
            if h * f0.max() > 0.45 * sr:
                break
            base = np.exp(-1j * h * phi)
            for sig, wc in zip(sigs, ws):
                a = _lowpass(sig * base, sr, bw)[::dec]
                inst = np.gradient(np.unwrap(np.angle(a))) * rate / (2 * np.pi)
                wgt = gaussian_filter1d(np.abs(a) ** 2, 0.05 * rate) * wc ** 2
                num += wgt * inst / h
                den += wgt
        d = gaussian_filter1d(num / (den + 1e-30), smooth_s * rate)
        f0 = f0 + np.interp(ts, td, d)
    return f0


# ------------------------------------------------------------- correction plan
DEFAULTS = dict(strength=1.0, smooth=0.25, ease=0.10, hold=0.25, fade_out=0.35, bw=45.0, nharm=10,
                match=True, a4=440.0, move=0.0, snap=0.0, keep=0.0)


def _smoothstep(u):
    u = np.clip(u, 0, 1)
    return u * u * (3 - 2 * u)


def plan_curve(ts, f0, p, midi, pull=None):
    """Work out the correction for one note from what the user did with it.

    midi : the note it is meant to be (cents are measured from it, at A4 = p['a4']).
    p    : move (cents, the whole note up/down), snap (0..1, how far to pull the note onto the
           written pitch), keep (0..1, how much of the natural wobble stays when snapping),
           smooth (s, what counts as wobble), ease (s, correction fades in at the start), strength.
    pull : cents per time step, the bits the user tucked up or down by hand.
    Returns the sung curve c, the offset to apply and the planned curve (all in cents).
    """
    p = {**DEFAULTS, **p}
    a4 = float(p["a4"])
    midi = int(midi)
    ref_hz = midi_hz(midi, a4)
    c = 1200 * np.log2(f0 / ref_hz)
    hop = float(np.median(np.diff(ts))) if len(ts) > 1 else 0.02
    base = gaussian_filter1d(c, max(0.5, float(p["smooth"]) / hop), mode="nearest")
    wob = c - base
    delta = float(p["move"]) + (np.asarray(pull, float) if pull is not None and len(pull) == len(ts) else 0.0)
    s = float(np.clip(p["snap"], 0, 1))
    planned = (1 - s) * (c + delta) + s * float(p["keep"]) * wob
    off = planned - c
    ease = float(p["ease"])
    if ease > 0:
        off = off * _smoothstep((ts - ts[0]) / ease)
    off = off * float(p["strength"])
    med = float(np.median(c))
    warning = ""
    if abs(med) > 60:
        warning = f"What was sung is about {med:+.0f} cents from {note_label(midi)}. Check the note name."
    return dict(ref_midi=midi, ref_hz=ref_hz, ref_label=note_label(midi), c=c, wob=wob, off=off, planned=c + off, warning=warning)


# -------------------------------------------------------------- the correction
def _band_energy_gain(x, removed, added, sr, band, win_s=0.06, smooth_s=0.05):
    sos = sg.butter(4, band, "bp", fs=sr, output="sos")
    xb = sg.sosfilt(sos, x, axis=0)
    rb = sg.sosfilt(sos, x - removed, axis=0)
    pb = sg.sosfilt(sos, added, axis=0)
    n = x.shape[0]
    fr = max(8, int(win_s * sr))
    hop = fr // 2
    centres, gs = [], []
    for i in range(0, max(1, n - fr), hop):
        s = slice(i, i + fr)
        E, pp = (xb[s] ** 2).sum(), (pb[s] ** 2).sum()
        rp, rr = (rb[s] * pb[s]).sum(), (rb[s] ** 2).sum()
        if pp < 1e-18:
            g = 1.0
        else:
            disc = rp * rp - pp * (rr - E)
            if disc < 0:
                g = -rp / pp
            else:
                g1, g2 = (-rp + np.sqrt(disc)) / pp, (-rp - np.sqrt(disc)) / pp
                g = g1 if abs(g1 - 1) < abs(g2 - 1) else g2
        gs.append(np.clip(g, 0.6, 1.5))
        centres.append(i + fr // 2)
    gs = gaussian_filter1d(np.array(gs), max(0.3, smooth_s * sr / hop), mode="nearest")
    return np.interp(np.arange(n), centres, gs)


def retune_segment(seg, sr, t_seg0, ts, f0, off, p):
    """seg: (n, ch) float; returns corrected copy. f0 in Hz, off in cents, on grid ts."""
    p = {**DEFAULTS, **p}
    n = seg.shape[0]
    t = t_seg0 + np.arange(n) / sr
    f0_t = np.interp(t, ts, f0)
    f0n_t = f0_t * 2 ** (np.interp(t, ts, off) / 1200)
    fi, fo, hold = 0.15, float(p["fade_out"]), float(p["hold"])
    w = np.clip((t - (ts[0] - fi)) / fi, 0, 1) * np.clip(((ts[-1] + hold + fo) - t) / fo, 0, 1)
    w = np.sin(0.5 * np.pi * w) ** 2
    idx = np.nonzero(w > 0)[0]
    if len(idx) == 0:
        return seg.copy()
    lo, hi = int(idx[0]), int(idx[-1]) + 1
    sl = slice(lo, hi)
    phi0 = 2 * np.pi * np.cumsum(f0_t[sl]) / sr
    phi1 = 2 * np.pi * np.cumsum(f0n_t[sl]) / sr
    x = seg[sl].astype(np.float64)
    ch = x.shape[1]
    removed, added = np.zeros_like(x), np.zeros_like(x)
    fmax_new = float(f0n_t[sl].max())
    for h in range(1, int(p["nharm"]) + 1):
        if h * fmax_new > 0.45 * sr:
            break
        base = np.exp(-1j * h * phi0)
        up0, up1 = np.conj(base), np.exp(1j * h * phi1)
        bw = float(p["bw"]) * np.sqrt(h)
        for c in range(ch):
            a = _lowpass(x[:, c] * base, sr, bw) * w[sl]
            removed[:, c] += (a * up0).real
            added[:, c] += (a * up1).real
    if p["match"]:
        band = (max(40.0, 0.8 * float(f0.min())), min(0.45 * sr, 2.2 * float(f0.max())))
        g = _band_energy_gain(x, removed, added, sr, band)
        y = x - removed + added * g[:, None]
    else:
        y = x - removed + added
    out = seg.copy()
    out[sl] = y.astype(seg.dtype)
    return out


def solo_segment(seg, sr, t_seg0, ts, f_pts, p, t_a, t_b, fade=0.06):
    """Keep only the picked note (its fundamental and overtone lines, following f_pts on the grid ts) between t_a and t_b;
    everything else in that stretch is removed. Outside t_a..t_b the audio is untouched, with short crossfades at the edges."""
    p = {**DEFAULTS, **p}
    n = seg.shape[0]
    t = t_seg0 + np.arange(n) / sr
    m = np.clip((t - t_a) / fade, 0, 1) * np.clip((t_b - t) / fade, 0, 1)
    m = np.sin(0.5 * np.pi * m) ** 2
    idx = np.nonzero(m > 0)[0]
    if len(idx) == 0:
        return seg.copy()
    lo, hi = int(idx[0]), int(idx[-1]) + 1
    sl = slice(lo, hi)
    f_t = np.interp(t[sl], ts, f_pts)
    phi = 2 * np.pi * np.cumsum(f_t) / sr
    x = seg[sl].astype(np.float64)
    solo = np.zeros_like(x)
    for h in range(1, int(p["nharm"]) + 1):
        if h * float(f_t.max()) > 0.45 * sr:
            break
        base = np.exp(-1j * h * phi)
        bw = float(p["bw"]) * np.sqrt(h)
        for c in range(x.shape[1]):
            a = _lowpass(x[:, c] * base, sr, bw)
            solo[:, c] += (a * np.conj(base)).real
    mm = m[sl][:, None]
    rf = float(np.sqrt(np.mean((x * mm) ** 2))) + 1e-12
    rs = float(np.sqrt(np.mean((solo * mm) ** 2))) + 1e-12
    g = float(np.clip(0.7 * rf / rs, 1.0, 4.0))
    out = seg.copy()
    out[sl] = (x * (1 - mm) + solo * g * mm).astype(seg.dtype)
    return out


def brush_region(b):
    ts = [p[0] for p in b["pts"]]
    rt = float(b["rt"])
    return min(ts) - rt - 0.1, max(ts) + rt + 0.1


def brush_segment(seg, sr, t_seg0, b, nharm=10):
    """Take the sound under a round brush out of every channel in the same way.
    The brush is a circle in (time, cents). Its centre follows the stroke; with b['harm'] the overtone lines above the
    brushed pitch are removed too (the circle is the same number of cents wide at each overtone)."""
    pts = sorted((float(t), float(f)) for t, f in b["pts"])
    rt, rc, amt = max(0.02, float(b["rt"])), max(5.0, float(b["rc"])), float(np.clip(b.get("amount", 1.0), 0, 1))
    n = seg.shape[0]
    t = t_seg0 + np.arange(n) / sr
    pt = np.array([p[0] for p in pts]); pf = np.log2([p[1] for p in pts])
    pt = pt + np.arange(len(pt)) * 1e-9   # strictly increasing for interp
    # width of the union of circles along the stroke (circles every rt/4 of stroke time, so a drag leaves a solid band)
    cen = np.arange(pt[0], pt[-1] + 1e-9, rt / 4) if pt[-1] > pt[0] else np.array([pt[0]])
    wd = np.zeros(n)
    for tc in cen:
        j0, j1 = max(0, int((tc - rt - t_seg0) * sr)), min(n, int((tc + rt - t_seg0) * sr) + 1)
        if j1 > j0:
            wd[j0:j1] = np.maximum(wd[j0:j1], np.sqrt(np.clip(1 - ((t[j0:j1] - tc) / rt) ** 2, 0, 1)))
    m = np.sin(0.5 * np.pi * np.clip(wd, 0, 1)) ** 2 * amt
    idx = np.nonzero(m > 1e-4)[0]
    if len(idx) == 0:
        return seg.copy()
    lo, hi = int(idx[0]), int(idx[-1]) + 1
    sl = slice(lo, hi)
    f_t = 2 ** np.interp(t[sl], pt, pf)
    phi = 2 * np.pi * np.cumsum(f_t) / sr
    x = seg[sl].astype(np.float64)
    gone = np.zeros_like(x)
    top = int(nharm) if b.get("harm", True) else 1
    k = 2 ** (rc / 1200) - 1
    for h in range(1, top + 1):
        if h * float(f_t.max()) > 0.45 * sr:
            break
        base = np.exp(-1j * h * phi)
        bw = max(1.5, 2 * h * float(f_t.mean()) * k)   # full width in Hz of the circle at this overtone
        for c in range(x.shape[1]):
            a = _lowpass(x[:, c] * base, sr, bw)
            gone[:, c] += (a * np.conj(base)).real
    out = seg.copy()
    out[sl] = (x - gone * m[sl][:, None]).astype(seg.dtype)
    return out


# ------------------------------------------------------------- voices: several singers on one note
def _vf_peaks(chans, ws, sr, t_seg0, t0, t1, f_t, H, span, win, hop):
    """Per frame, the steady peaks near every overtone of the written note, as (cents from the written note, level, partial).
    Magnitudes of the channels are added (not the waveforms), so mics that are a little apart in time do not cancel."""
    n = int(win * sr)
    w = np.hanning(n)
    ts = np.arange(t0 + win / 2, t1 - win / 2 + 1e-9, hop)
    k = 2 ** (span / 1200)
    out = []
    for tc in ts:
        i = int((tc - win / 2 - t_seg0) * sr)
        if i < 0 or i + n > len(chans[0]):
            out.append([])
            continue
        F = 0.0
        for c, wc in zip(chans, ws):
            F = F + wc * np.abs(np.fft.rfft(c[i:i + n] * w, n * 8))
        fr = np.fft.rfftfreq(n * 8, 1 / sr)
        pk_all = []
        for h in range(1, H + 1):
            lo, hi = h * f_t / k, h * f_t * k
            if hi > 0.45 * sr:
                break
            m = (fr > lo) & (fr < hi)
            Fm = 20 * np.log10(F[m] + 1e-9)
            frm = fr[m]
            if len(Fm) < 5:
                continue
            pk, _ = sg.find_peaks(Fm, prominence=6)
            for p in pk:
                d = 0.0
                if 0 < p < len(Fm) - 1:
                    a, b, c = Fm[p - 1], Fm[p], Fm[p + 1]
                    d = 0.5 * (a - c) / (a - 2 * b + c + 1e-12)
                f = frm[p] + d * (frm[1] - frm[0])
                pk_all.append((1200 * np.log2(f / (h * f_t)), 10 ** (Fm[p] / 20), h))
        out.append(pk_all)
    return ts, out


def find_voices(chans, ws, sr, t_seg0, t0, t1, f_t, H=8, span=60.0, win=0.6, hop=0.15):
    ts, fp = _vf_peaks(chans, ws, sr, t_seg0, t0, t1, f_t, H, span, win, hop)
    grid = np.arange(-span, span + 1e-9, 0.5)
    hist = np.zeros_like(grid)
    for pks in fp:
        for c, a, h in pks:
            sgm = float(np.clip(3.0 / h, 1.2, 3.0))
            hist += a * np.exp(-0.5 * ((grid - c) / sgm) ** 2)
    if hist.max() <= 0:
        return ts, [], grid, hist, np.zeros((0, len(ts)))
    pk, _ = sg.find_peaks(hist, prominence=0.12 * hist.max(), distance=8)
    cl = [float(grid[p]) for p in pk]
    return ts, fp, grid, hist, cl


def track_voices(ts, fp, cl, minsep=4.0):
    nv = len(cl)
    C = np.full((nv, len(ts)), np.nan)
    cur = np.array(cl, float)
    for k, pks in enumerate(fp):
        for j in range(nv):
            others = np.delete(cur, j)
            sep = float(np.min(np.abs(others - cur[j]))) if len(others) else 40.0
            wdt = min(max(minsep, 0.45 * sep), 18.0)
            num = den = 0.0
            for c, a, h in pks:
                if abs(c - cur[j]) < wdt:
                    num += a * c
                    den += a
            if den > 0:
                C[j, k] = num / den
                cur[j] = 0.7 * cur[j] + 0.3 * C[j, k]
    for j in range(nv):
        m = np.isfinite(C[j])
        C[j] = np.interp(ts, ts[m], C[j, m]) if m.sum() >= 2 else cl[j]
    return C


def _sep_hz(h, f_t, C, j):
    """smallest distance (Hz) at overtone h between voice j and any other voice, over time"""
    s = 1e9
    for i in range(C.shape[0]):
        if i != j:
            s = min(s, float(np.min(h * f_t * np.abs(2 ** (C[i] / 1200) - 2 ** (C[j] / 1200)))))
    return s


def _refine_voice(chans, ws, sr, t_seg0, f_t, ts, C, j, t_a, t_b, iters=6, smooth_s=0.012, bwmin=9.0, bwmax=30.0, H=14):
    """Frequency track of one voice that follows its vibrato, from the overtones far enough from the other voices to be
    cut out by a filter wide enough to hold the vibrato."""
    i0 = max(0, int((t_a - t_seg0) * sr))
    i1 = min(len(chans[0]), int((t_b - t_seg0) * sr))
    tt = t_seg0 + np.arange(i0, i1) / sr
    sigs = [c[i0:i1].astype(np.float64) for c in chans]
    f0 = f_t * 2 ** (np.interp(tt, ts, C[j]) / 1200)
    dec = max(1, int(sr // 400))
    td = tt[::dec]
    rate = sr / dec
    hs = []
    for h in range(1, H + 1):
        if h * f0.max() * 1.03 > 0.45 * sr:
            break
        bw = min(bwmax, 0.6 * _sep_hz(h, f_t, C, j))
        if bw >= bwmin:
            hs.append((h, bw))
    if not hs:
        return None, tt
    for _ in range(iters):
        phi = 2 * np.pi * np.cumsum(f0) / sr
        num = np.zeros(len(td))
        den = np.zeros(len(td))
        for h, bw in hs:
            base = np.exp(-1j * h * phi)
            for sig, wc in zip(sigs, ws):
                a = _lowpass(sig * base, sr, bw)[::dec]
                inst = np.gradient(np.unwrap(np.angle(a))) * rate / (2 * np.pi)
                wgt = gaussian_filter1d(np.abs(a) ** 2, 0.05 * rate) * wc ** 2
                num += wgt * inst / h
                den += wgt
        d = gaussian_filter1d(num / (den + 1e-30), smooth_s * rate)
        f0 = f0 + np.interp(tt, td, d)
    return f0, tt


def retune_voices(seg, sr, t_seg0, f_t, ts, C, offs, t_a, t_b, trk, H=12, bw_max=45.0, fade=0.15, bwlow=0.8):
    """seg (n, ch): move each voice j by offs[j] cents (0 = leave it), in every channel the same way.
    trk = (chans, weights, sr2) the decimated loudest channels used to follow each voice's vibrato."""
    n, ch = seg.shape
    t = t_seg0 + np.arange(n) / sr
    w = np.clip((t - (t_a - fade)) / fade, 0, 1) * np.clip(((t_b + fade) - t) / fade, 0, 1)
    w = np.sin(0.5 * np.pi * w) ** 2
    idx = np.nonzero(w > 0)[0]
    if len(idx) == 0:
        return seg.copy()
    lo, hi = int(idx[0]), int(idx[-1]) + 1
    sl = slice(lo, hi)
    x = seg[sl].astype(np.float64)
    rem, add = np.zeros_like(x), np.zeros_like(x)
    tt = t[sl]
    tch, tws, tsr = trk
    nv = C.shape[0]
    todo = [j for j in range(nv) if abs(offs[j]) >= 0.5]
    for n_done, j in enumerate(todo):
        _prog("Moving voice %d of %d" % (n_done + 1, len(todo)), 0.1 + 0.8 * n_done / max(1, len(todo)))
        fj, ttj = _refine_voice(tch, tws, tsr, t_seg0, f_t, ts, C, j, t_a, t_b)
        f0 = f_t * 2 ** (np.interp(tt, ts, C[j]) / 1200) if fj is None else np.interp(tt, ttj, fj)
        f1 = f0 * 2 ** (offs[j] / 1200)
        p0 = 2 * np.pi * np.cumsum(f0) / sr
        p1 = 2 * np.pi * np.cumsum(f1) / sr
        for h in range(1, H + 1):
            if h * f1.max() > 0.45 * sr:
                break
            bw = min(bw_max * np.sqrt(h), 0.7 * _sep_hz(h, f_t, C, j))
            if bw < bwlow:
                continue
            base = np.exp(-1j * h * p0)
            up = np.exp(1j * h * p1)
            for c in range(ch):
                a = _lowpass(x[:, c] * base, sr, bw) * w[sl]
                rem[:, c] += (a * np.conj(base)).real
                add[:, c] += (a * up).real
    out = seg.astype(np.float64).copy()
    out[sl] = x - rem + add
    return out.astype(seg.dtype)


def level_report(before, after, sr, t_seg0, t_a, t_b, band):
    sos = sg.butter(4, band, "bp", fs=sr, output="sos")
    xb = sg.sosfilt(sos, before.mean(axis=1))
    yb = sg.sosfilt(sos, after.mean(axis=1))
    fr = int(0.05 * sr)
    ts, d = [], []
    for t in np.arange(t_a, t_b, 0.025):
        i = int((t - t_seg0) * sr)
        if i < 0 or i + fr > len(xb):
            continue
        ex, ey = np.sqrt((xb[i:i + fr] ** 2).mean()) + 1e-12, np.sqrt((yb[i:i + fr] ** 2).mean()) + 1e-12
        ts.append(t)
        d.append(20 * np.log10(ey / ex))
    d = np.array(d)
    if len(d) == 0:
        return dict(rise=0.0, rise_t=0.0, dip=0.0, dip_t=0.0, std=0.0)
    return dict(rise=float(d.max()), rise_t=float(ts[int(d.argmax())]),
                dip=float(d.min()), dip_t=float(ts[int(d.argmin())]), std=float(d.std()))


# ------------------------------------------------------------------- session
def _pj(o):
    """Turn numpy values into plain JSON-able ones."""
    if isinstance(o, dict):
        return {str(k): _pj(v) for k, v in o.items()}
    if isinstance(o, (list, tuple)):
        return [_pj(v) for v in o]
    if isinstance(o, np.ndarray):
        return [_pj(v) for v in o.tolist()]
    if isinstance(o, (np.floating,)):
        return float(o)
    if isinstance(o, (np.integer,)):
        return int(o)
    if isinstance(o, (np.bool_,)):
        return bool(o)
    return o


PROG = {"stage": "", "frac": 0.0, "active": False}
_PL = threading.Lock()


def _prog(stage, frac, active=True):
    PROG["stage"], PROG["frac"], PROG["active"] = stage, float(frac), active


def load_many(paths):
    """Several WAV files -> one (n, ch) array: every channel of every file, in the order given.
    Files must share a sample rate; a shorter file is padded with silence at the end (and reported)."""
    parts = []
    for i, p in enumerate(paths):
        _prog("Reading file %d of %d" % (i + 1, len(paths)), 0.2 * i / len(paths))
        sr, x, info = load_wav(p)
        parts.append((p, sr, x, info))
        _prog("Reading file %d of %d" % (i + 1, len(paths)), 0.2 * (i + 1) / len(paths))
    sr0 = parts[0][1]
    for p, sr, _, _ in parts:
        if sr != sr0:
            raise ValueError("These files have different sample rates (%s is %d Hz, %s is %d Hz). "
                             "Export them all at the same rate." % (os.path.basename(parts[0][0]), sr0, os.path.basename(p), sr))
    n = max(x.shape[0] for _, _, x, _ in parts)
    warn = None
    if any(x.shape[0] != n for _, _, x, _ in parts):
        warn = "Files are different lengths - shorter ones were padded with silence at the end."
    cols, sources, c0 = [], [], 0
    for p, sr, x, info in parts:
        pad = np.zeros((n, x.shape[1]), np.float32)
        pad[:x.shape[0]] = x
        cols.append(pad)
        sources.append(dict(path=p, n=x.shape[0], c0=c0, c1=c0 + x.shape[1], info=info))
        c0 += x.shape[1]
    info = dict(parts[0][3])
    info["bits"] = max(pi["bits"] for _, _, _, pi in parts)
    return sr0, np.ascontiguousarray(np.concatenate(cols, axis=1)), info, sources, warn


class Session:
    def __init__(self, path):
        paths = [path] if isinstance(path, str) else list(path)
        _prog("Reading file", 0.0)
        if not paths:
            raise ValueError("No files given.")
        self.sources = None
        self.warning = None
        if len(paths) == 1:
            path = paths[0]
            self.sr, self.orig, self.info = load_wav(path)
        else:
            self.sr, self.orig, self.info, self.sources, self.warning = load_many(paths)
            path = paths[0]
        self.path = path
        self.cur = self.orig.copy()
        self.n, self.ch = self.orig.shape
        self.dur = self.n / self.sr
        _prog("Analysing channels", 0.2)
        self.spec = Spectrogram(self.orig, self.sr)
        _prog("Done", 1.0, False)
        self.mon_gain = [0.0] * self.ch   # dB, what you hear
        self.mute = [False] * self.ch
        self.brushes = []
        self._bnext = 1
        self.btrash = {}
        self.vedits = []
        self._vnext = 1
        self.vtrash = {}
        self.vcand = None
        self.solo = [False] * self.ch
        self.pan0 = default_pans(self.ch, [sc["c1"] - sc["c0"] for sc in self.sources] if self.sources else None)
        self.mon_pan = list(self.pan0)
        self.vis_gain = [0.0] * self.ch   # dB, what the picture and the measuring favour
        self._aud = None
        self.edits = []
        self.cand = None
        self.cands = {}
        self.trash = {}
        self._cid = 0
        self._next = 1

    # -- helpers
    def seg(self, a, b, src=None):
        src = self.cur if src is None else src
        i0, i1 = max(0, int(a * self.sr)), min(self.n, int(b * self.sr))
        return src[i0:i1], i0 / self.sr

    def set_mixer(self, gain=None, pan=None, vis=None, mute=None, solo=None):
        if mute is not None and len(mute) == self.ch:
            self.mute = [bool(v) for v in mute]
        if solo is not None and len(solo) == self.ch:
            self.solo = [bool(v) for v in solo]
        if gain is not None and len(gain) == self.ch:
            self.mon_gain = [float(v) for v in gain]
        if pan is not None and len(pan) == self.ch:
            self.mon_pan = [float(v) for v in pan]
        if vis is not None and len(vis) == self.ch:
            self.vis_gain = [float(v) for v in vis]
            self.spec.set_gains(db_lin(self.vis_gain))

    def mix(self):
        any_solo = any(self.solo)
        g = [(-99.0 if (self.mute[i] or (any_solo and not self.solo[i])) else self.mon_gain[i]) for i in range(self.ch)]
        return (g, self.mon_pan)

    def _tracking_inputs(self, seg, f_lo, f_hi, kmax=6):
        """The channels to measure a note with and their weights (from the visual mixer): the loudest few, in the
        note's own pitch range, after weighting."""
        w = db_lin(self.vis_gain)
        if seg.shape[1] == 1:
            return [seg[:, 0]], [1.0]
        sos = sg.butter(2, [max(20.0, 0.8 * f_lo), min(0.45 * 24000, 3.0 * f_hi)], "bp", fs=24000, output="sos")
        en = []
        for c in range(seg.shape[1]):
            x2, _ = decimate_to(seg[:, c], self.sr)
            en.append(float(np.mean(sg.sosfilt(sos, x2) ** 2)) * w[c] ** 2)
        order = [c for c in np.argsort(en)[::-1][:kmax] if en[c] > 0]
        if not order:
            raise ValueError("Every channel is turned down in the visual mixer.")
        top = max(w[c] for c in order)
        return [seg[:, c] for c in order], [w[c] / top for c in order]

    def meta(self):
        multi = self.sources is not None
        labels = None
        if multi:
            labels = []
            for sc in self.sources:
                stem = os.path.splitext(os.path.basename(sc["path"]))[0]
                k = sc["c1"] - sc["c0"]
                labels += [stem] if k == 1 else ["%s %d" % (stem, j + 1) for j in range(k)]
        return dict(path=self.path, labels=labels, warning=self.warning, files=len(self.sources) if multi else 1, mixer=dict(gain=self.mon_gain, pan=self.mon_pan, vis=self.vis_gain, pan0=self.pan0, mute=self.mute, solo=self.solo), sr=self.sr, channels=self.ch, duration=self.dur,
                    bits=self.info["bits"], detail=self.spec.detail, files_list=[sc["path"] for sc in self.sources] if multi else [self.path], master=self.spec.master_geo(), name=("%d files" % len(self.sources)) if multi else self.path.replace("\\", "/").split("/")[-1])

    # -- selecting a note
    def select(self, t0, t1, f_lo, f_hi, partial=1, a4=440.0, midi=None, refmode=None, refwin=10.0):
        """The user drew a box round a note: from t0 to t1, and between f_lo and f_hi (Hz).
        `partial` says which overtone the box was drawn round (1 = the fundamental).
        Measures the pitch curve of the note inside that box."""
        partial = max(1, int(partial))
        t0, t1 = max(0.0, min(t0, t1)), min(self.dur, max(t0, t1))
        if t1 - t0 < 0.15:
            raise ValueError("That box is too short. Draw it at least 0.15 s wide.")
        f_lo, f_hi = sorted((float(f_lo) / partial, float(f_hi) / partial))
        if f_hi / f_lo < 1.03:
            m = np.sqrt(f_hi * f_lo)
            f_lo, f_hi = m / 1.03 ** 0.5, m * 1.03 ** 0.5
        if f_hi / f_lo > 2.2:
            raise ValueError("That box covers more than an octave. Draw it closer round the note.")
        ref_info = None
        if refmode == "around":
            try:
                ref_info = self.detect_reference(t0 - float(refwin), t1 + float(refwin))
                a4 = ref_info["a4"]
            except ValueError:
                ref_info = None
        _prog("Measuring the note", 0.1)
        seg, s0 = self.seg(t0 - 0.8, t1 + 0.8)
        chans, ws = self._tracking_inputs(seg, f_lo, f_hi)
        ts, f0 = track_note(chans, ws, self.sr, s0, t0, t1, f_lo, f_hi)
        _prog("Measuring the note", 0.6)
        f0 = refine_track(chans, ws, self.sr, s0, ts, f0)
        _prog("Done", 1.0, False)
        self.last_channels = len(chans)
        hint = detect_partial(chans, ws, self.sr, s0, ts, f0) if partial == 1 else 1
        if midi is None:
            midi = int(round(midi_of(float(np.median(f0)), float(a4))))
        return self._register(dict(ts=ts, f0=f0, partial=partial, f_lo=f_lo, f_hi=f_hi, midi=int(midi), hint=int(hint), a4=float(a4), ref_info=ref_info))

    def _register(self, cand):
        """Remember a picked note under an id so the interface can go back to it (undo / redo)."""
        self._cid += 1
        cand["cid"] = self._cid
        self.cands[self._cid] = cand
        for k in [k for k in self.cands if k < self._cid - 300]:
            del self.cands[k]
        self.cand = cand
        return cand

    def use(self, cid):
        if cid is not None and int(cid) in self.cands:
            self.cand = self.cands[int(cid)]
        if self.cand is None:
            raise ValueError("Select a note first.")
        return self.cand

    # -- apply
    def _edit_region(self, e):
        p = e["params"]
        pad = 0.3
        return (max(0.0, e["ts"][0] - 0.15 - pad),
                min(self.dur, e["ts"][-1] + p["hold"] + p["fade_out"] + pad))

    def _run_edit(self, e):
        a, b = self._edit_region(e)
        seg, s0 = self.seg(a, b)
        seg = seg.copy()
        new = retune_segment(seg, self.sr, s0, e["ts"], e["f0"], e["off"], e["params"])
        i0 = int(round(s0 * self.sr))
        self.rev = getattr(self, "rev", 0) + 1
        self.cur[i0:i0 + len(new)] = new
        return seg, new, s0

    def _run_brush(self, b):
        a, z = brush_region(b)
        a, z = max(0.0, a), min(self.dur, z)
        seg, s0 = self.seg(a, z)
        new = brush_segment(seg.copy(), self.sr, s0, b, DEFAULTS["nharm"])
        i0 = int(round(s0 * self.sr))
        self.rev = getattr(self, "rev", 0) + 1
        self.cur[i0:i0 + len(new)] = new

    # -- voices: nudge the out-of-tune singers on a note onto the pitch, leaving the in-tune ones alone
    def _voice_inputs(self, seg, f_t):
        chans, ws = self._tracking_inputs(seg, f_t, 2.0 * f_t)
        out, sr2 = [], None
        for c in chans:
            x2, sr2 = decimate_to(c, self.sr)
            out.append(x2.astype(np.float64))
        return out, list(ws), sr2

    def detect_voices(self, t0, t1, midi, a4=440.0):
        t0, t1 = float(t0), float(t1)
        f_t = float(a4) * 2 ** ((int(midi) - 69) / 12)
        win = min(0.6, max(0.3, (t1 - t0) / 3.0))
        if t1 - t0 < win + 0.1:
            raise ValueError("That note is too short to tell the voices apart (it needs about half a second or more).")
        _prog("Finding the voices", 0.1)
        seg, s0 = self.seg(t0 - 0.6, t1 + 0.6)
        chans, ws, sr2 = self._voice_inputs(seg, f_t)
        ts, fp, grid, hist, cl = find_voices(chans, ws, sr2, s0, t0, t1, f_t, H=8, span=60.0, win=win, hop=win / 4)
        if not len(cl):
            _prog("Done", 1.0, False)
            raise ValueError("No steady voices were found round that note.")
        C = track_voices(ts, fp, cl)
        strength = [float(hist[int(np.argmin(np.abs(grid - c)))] / hist.max()) for c in cl]
        self.vcand = dict(t0=t0, t1=t1, midi=int(midi), a4=float(a4), f_t=f_t, ts=ts, C=C, strength=strength)
        _prog("Done", 1.0, False)
        return dict(t0=t0, t1=t1, midi=int(midi), a4=float(a4), f_t=f_t, ts=ts, voices=[
            dict(cents=float(np.mean(C[j])), strength=strength[j], track=C[j]) for j in range(len(cl))])

    def add_voice_edit(self, nudge, target="note"):
        """nudge: list of bools, one per voice found by the last detect_voices. target: 'note' (the written pitch) or 'main' (the strongest voice)."""
        vc = self.vcand
        if vc is None:
            raise ValueError("Find the voices first.")
        C = vc["C"]
        means = [float(np.mean(C[j])) for j in range(C.shape[0])]
        if len(nudge) != len(means) or not any(nudge):
            raise ValueError("Tick the voices you want to move.")
        tgt = 0.0
        if target == "main":
            tgt = means[int(np.argmax(vc["strength"]))]
        offs = [float(tgt - means[j]) if nudge[j] else 0.0 for j in range(len(means))]
        v = dict(id=self._vnext, enabled=True, t0=vc["t0"], t1=vc["t1"], midi=vc["midi"], a4=vc["a4"], f_t=vc["f_t"],
                 ts=np.array(vc["ts"], float), C=np.array(C, float), offs=offs, means=means)
        self._vnext += 1
        _prog("Moving the voices", 0.05)
        self._run_vedit(v)
        self.vedits.append(v)
        _prog("Done", 1.0, False)
        return v

    def _run_vedit(self, v):
        a, b = max(0.0, v["t0"] - 0.8), min(self.dur, v["t1"] + 0.8)
        seg, s0 = self.seg(a, b)
        trk = self._voice_inputs(seg, v["f_t"])
        new = retune_voices(seg.copy(), self.sr, s0, v["f_t"], np.asarray(v["ts"], float), np.asarray(v["C"], float),
                            v["offs"], v["t0"], v["t1"], trk)
        i0 = int(round(s0 * self.sr))
        self.rev = getattr(self, "rev", 0) + 1
        self.cur[i0:i0 + len(new)] = new

    def voice_edit(self, vid, enabled=None, delete=False, restore=False):
        vid = int(vid)
        if restore:
            v, i = self.vtrash.pop(vid)
            self.vedits.insert(min(i, len(self.vedits)), v)
        elif delete:
            for i, v in enumerate(self.vedits):
                if v["id"] == vid:
                    self.vedits.remove(v)
                    self.vtrash[vid] = (v, i)
                    break
        else:
            for v in self.vedits:
                if v["id"] == vid:
                    v["enabled"] = bool(enabled)
        self.rebuild()

    def add_brush(self, pts, rt, rc, amount=1.0, harm=True):
        if not pts:
            raise ValueError("Nothing was brushed.")
        pts = [[float(t), float(f)] for t, f in pts]
        if len(pts) > 400:   # thin a very long stroke
            keep = np.linspace(0, len(pts) - 1, 400).astype(int)
            pts = [pts[i] for i in keep]
        b = dict(id=self._bnext, enabled=True, pts=pts, rt=float(rt), rc=float(rc), amount=float(amount), harm=bool(harm))
        self._bnext += 1
        _prog("Brushing out the sound", 0.2)
        self.brushes.append(b)
        self.rebuild()
        _prog("Done", 1.0, False)
        return b

    def brush_edit(self, bid, enabled=None, delete=False, restore=False):
        bid = int(bid)
        if restore:
            b, i = self.btrash.pop(bid)
            self.brushes.insert(min(i, len(self.brushes)), b)
        elif delete:
            for i, b in enumerate(self.brushes):
                if b["id"] == bid:
                    self.brushes.remove(b)
                    self.btrash[bid] = (b, i)
                    break
        else:
            for b in self.brushes:
                if b["id"] == bid:
                    b["enabled"] = bool(enabled)
        self.rebuild()

    def apply(self, params, pull=None, midi=None):
        if self.cand is None:
            raise ValueError("Select a note first.")
        p = {**DEFAULTS, **params}
        cd = self.cand
        if midi is not None:
            cd["midi"] = int(midi)
        pl = plan_curve(cd["ts"], cd["f0"], p, cd["midi"], pull)
        if not np.any(np.abs(pl["off"]) > 0.5):
            raise ValueError("Nothing to correct yet: move the note, pull the line or raise Snap first.")
        e = dict(id=self._next, enabled=True, ts=cd["ts"], f0=cd["f0"], off=pl["off"], pull=None if pull is None else [float(v) for v in pull],
                 params=p, c0=pl["c"], ref_hz=pl["ref_hz"], ref_label=pl["ref_label"], midi=pl["ref_midi"],
                 partial=cd.get("partial", 1), f_lo=cd["f_lo"], f_hi=cd["f_hi"])
        self._next += 1
        _prog("Correcting the note", 0.1)
        before, new, s0 = self._run_edit(e)
        _prog("Checking the result", 0.7)
        # re-track the corrected audio (same search range) to show what the note now does
        try:
            seg2, s02 = self.seg(e["ts"][0] - 0.8, e["ts"][-1] + 0.8)
            ch2, ws2 = self._tracking_inputs(seg2, e["f_lo"], e["f_hi"])
            ts2, f02 = track_note(ch2, ws2, self.sr, s02, e["ts"][0], e["ts"][-1], e["f_lo"], e["f_hi"])
            f02 = refine_track(ch2, ws2, self.sr, s02, ts2, f02)
            f02 = np.interp(e["ts"], ts2, f02)
            e["c1"] = 1200 * np.log2(f02 / e["ref_hz"])
        except Exception:
            e["c1"] = pl["planned"]
        band = (max(40.0, 0.8 * float(e["f0"].min())), min(0.45 * self.sr, 2.2 * float(e["f0"].max())))
        e["report"] = level_report(before, new, self.sr, s0, e["ts"][0], e["ts"][-1], band)
        e["report"]["max_shift"] = float(np.abs(e["off"]).max())
        e["report"]["planned_med"] = float(np.median(pl["planned"]))
        e["report"]["after_med"] = float(np.median(e["c1"]))
        e["report"]["err_after"] = float(np.sqrt(np.mean((e["c1"] - pl["planned"]) ** 2)))
        self.edits.append(e)
        self.cand = None
        _prog("Done", 1.0, False)
        return e

    def reopen(self, eid):
        """Take an applied edit back out so it can be changed."""
        for e in self.edits:
            if e["id"] == int(eid):
                break
        else:
            raise ValueError("Edit not found.")
        self.remove_edit(e["id"])
        self._register(dict(ts=e["ts"], f0=e["f0"], partial=e.get("partial", 1), f_lo=e["f_lo"], f_hi=e["f_hi"], midi=e.get("midi")))
        return e

    def remove_edit(self, eid):
        for i, e in enumerate(self.edits):
            if e["id"] == int(eid):
                self.edits.remove(e)
                self.trash[int(eid)] = (e, i)
                self.rebuild()
                return e
        raise ValueError("Edit not found.")

    def restore_edit(self, eid):
        e, i = self.trash.pop(int(eid))
        self.edits.insert(min(i, len(self.edits)), e)
        self.rebuild()
        return e

    def audition(self, t0, t1, ver="fix", params=None, pull=None, midi=None, iso=None):
        """What the visible clip would sound like with the planned change, without committing it.
        The corrected clip is remembered, so moving a fader or a pan dial only re-mixes it."""
        t0, t1 = max(0.0, float(t0)), min(self.dur, float(t1))
        if t1 - t0 < 0.05:
            raise ValueError("Nothing to play.")
        cd = self.cand
        isok = bool(iso) and cd is not None
        _prog("Rendering the isolated note" if isok else "Rendering the audition", 0.1)
        if ver == "orig":
            seg, s0o = self.seg(t0, t1, self.orig)
            if isok:
                seg = solo_segment(seg, self.sr, s0o, cd["ts"], cd["f0"], {**DEFAULTS, **(params or {})}, float(iso["t0"]), float(iso["t1"]))
            _prog("Done", 1.0, False)
            return wav_bytes(seg, self.sr, mix=self.mix())
        key = (round(t0, 4), round(t1, 4), getattr(self, "rev", 0), (round(float(iso["t0"]), 3), round(float(iso["t1"]), 3)) if isok else None, None if cd is None or params is None else
               (cd.get("cid"), cd["midi"] if midi is None else int(midi), repr(sorted(params.items())), None if pull is None else tuple(np.round(pull, 3))))
        if self._aud is not None and self._aud[0] == key:
            _prog("Done", 1.0, False)
            return wav_bytes(self._aud[1], self.sr, mix=self.mix())
        seg, s0 = self.seg(t0, t1)
        out = seg.copy()
        fpts = None if cd is None else cd["f0"]
        if cd is not None and params is not None:
            p = {**DEFAULTS, **params}
            pl = plan_curve(cd["ts"], cd["f0"], p, cd["midi"] if midi is None else int(midi), pull)
            if np.any(np.abs(pl["off"]) > 0.05):
                _prog("Applying the planned change", 0.3)
                out = retune_segment(out, self.sr, s0, cd["ts"], cd["f0"], pl["off"], p)
                fpts = cd["f0"] * 2 ** (pl["off"] / 1200)
        if isok:
            _prog("Isolating the note", 0.65)
            out = solo_segment(out, self.sr, s0, cd["ts"], fpts, {**DEFAULTS, **(params or {})}, float(iso["t0"]), float(iso["t1"]))
        _prog("Done", 1.0, False)
        self._aud = (key, out)
        return wav_bytes(out, self.sr, mix=self.mix())

    def set_detail(self, detail):
        self.spec.set_detail(detail)

    # -- the choir's own reference pitch
    def _ref_points(self, t0, t1, kmax=4):
        """Steady spectral peaks (60-1100 Hz) of the loudest channels in [t0, t1]: arrays of time, offset in cents from the
        nearest A=440 semitone (-50..+50) and weight."""
        from scipy.signal import find_peaks
        t0, t1 = max(0.0, float(t0)), min(self.dur, float(t1))
        if t1 - t0 < 2.0:
            raise ValueError("Give it at least 2 seconds of music to measure the pitch from.")
        seg, s0 = self.seg(t0, t1, self.orig)
        w = db_lin(self.vis_gain)
        dec = []
        for c in range(seg.shape[1]):
            x2, sr2 = decimate_to(seg[:, c], self.sr)
            dec.append(x2)
        en = [float(np.mean(d.astype(np.float64) ** 2)) * w[c] ** 2 for c, d in enumerate(dec)]
        top = [c for c in np.argsort(en)[::-1][:kmax] if en[c] > 0]
        if not top:
            raise ValueError("Every channel is turned down in the visual mixer.")
        N = int(2 ** round(np.log2(0.34 * sr2)))
        pad = 2
        win = np.hanning(N)
        fax = np.arange(N * pad // 2 + 1) * sr2 / (N * pad)
        band = (fax >= 60) & (fax <= 1100)
        hop = int(0.1 * sr2)
        frames = []
        n_fr = (len(dec[0]) - N) // hop
        for k in range(max(0, n_fr)):
            i = k * hop
            P = 0.0
            for c in top:
                P = P + (w[c] * np.abs(np.fft.rfft(dec[c][i:i + N] * win, N * pad))) ** 2
            db = 10 * np.log10(P[band] + 1e-18)
            fb = fax[band]
            mx = float(db.max())
            pk, _ = find_peaks(db, prominence=10, height=mx - 45)
            cs, ws = [], []
            for j in pk:
                if 0 < j < len(db) - 1:
                    a, b, c2 = db[j - 1], db[j], db[j + 1]
                    d = 0.5 * (a - c2) / (a - 2 * b + c2 + 1e-12)
                    f = fb[j] + d * (fb[1] - fb[0])
                    cs.append(1200 * np.log2(f / 440.0))
                    ws.append(10 ** ((b - mx) / 20))
            frames.append((t0 + (i + N / 2) / sr2, np.array(cs), np.array(ws)))
            if k % 50 == 0 and n_fr:
                _prog("Measuring the choir's pitch", 0.05 + 0.85 * k / n_fr)
        T, D, W = [], [], []
        for k in range(1, len(frames) - 1):
            t, cs, ws = frames[k]
            if len(cs) == 0:
                continue
            ok = np.zeros(len(cs), bool)
            for nb in (frames[k - 1][1], frames[k + 1][1]):
                if len(nb) == 0:
                    ok[:] = False
                    break
            else:
                for q, c in enumerate(cs):
                    ok[q] = (np.min(np.abs(frames[k - 1][1] - c)) < 25) and (np.min(np.abs(frames[k + 1][1] - c)) < 25)
            for q in np.nonzero(ok)[0]:
                T.append(t)
                D.append(((cs[q] + 50.0) % 100.0) - 50.0)
                W.append(ws[q])
        return np.array(T), np.array(D), np.array(W)

    @staticmethod
    def _ref_stats(D, W, min_n=20):
        if len(D) < min_n:
            return None
        bins = np.zeros(100)
        idx = np.clip(np.round(D + 50).astype(int) % 100, 0, 99)
        np.add.at(bins, idx, W)
        k = np.arange(-30, 31)
        g = np.exp(-0.5 * (k / 5.0) ** 2)
        sm = np.zeros(100)
        for o, gv in zip(k, g):
            sm += gv * np.roll(bins, o)
        mode = int(np.argmax(sm)) - 50.0
        dd = ((D - mode + 50.0) % 100.0) - 50.0
        sel = np.abs(dd) < 20
        if sel.sum() < min_n:
            return None
        ww = W[sel]
        mean = mode + float(np.average(dd[sel], weights=ww))
        sd = float(np.sqrt(np.average((dd[sel] - (mean - mode)) ** 2, weights=ww)))
        mean = ((mean + 50.0) % 100.0) - 50.0
        return dict(cents=float(mean), a4=float(440.0 * 2 ** (mean / 1200)), spread=sd, n=int(sel.sum()))

    def detect_reference(self, t0, t1, series=False):
        """The pitch the choir is really singing at (as an A4 in Hz) over [t0, t1], from the steady notes in the loudest channels."""
        _prog("Measuring the choir's pitch", 0.02)
        T, D, W = self._ref_points(t0, t1)
        st = self._ref_stats(D, W)
        if st is None:
            _prog("Done", 1.0, False)
            raise ValueError("Not enough steady notes in that stretch to measure the pitch. Try a longer or busier range.")
        st["t0"], st["t1"] = float(max(0.0, t0)), float(min(self.dur, t1))
        if series:
            ser = []
            a = float(T.min()) if len(T) else 0.0
            while a < (float(T.max()) if len(T) else 0.0):
                m = (T >= a) & (T < a + 4.0)
                r = self._ref_stats(D[m], W[m], 12)
                if r is not None:
                    ser.append(dict(t=a + 2.0, cents=r["cents"], spread=r["spread"], n=r["n"]))
                a += 2.0
            st["series"] = ser
        _prog("Done", 1.0, False)
        return st

    # -- project files
    def project_dict(self):
        srcs = self.sources or [dict(path=self.path)]
        files = []
        for sc in srcs:
            p = sc["path"]
            st = os.stat(p) if os.path.exists(p) else None
            files.append(dict(path=p, size=st.st_size if st else None, mtime=st.st_mtime if st else None))
        return _pj(dict(files=files, edits=self.edits, next=self._next, overwritten=bool(getattr(self, "overwritten", False)),
                        mixer=dict(gain=self.mon_gain, pan=self.mon_pan, vis=self.vis_gain, mute=self.mute, solo=self.solo), brushes=self.brushes, bnext=self._bnext, vedits=self.vedits, vnext=self._vnext, detail=self.spec.detail))

    def load_project(self, d, skip_edits=False):
        if d.get("detail") and d["detail"] != self.spec.detail:
            self.set_detail(d["detail"])
        mx = d.get("mixer") or {}
        self.set_mixer(mx.get("gain"), mx.get("pan"), mx.get("vis"), mx.get("mute"), mx.get("solo"))
        if skip_edits:
            return
        self.vedits = []
        for v in d.get("vedits", []):
            v = dict(v)
            v["ts"] = np.array(v["ts"], float)
            v["C"] = np.array(v["C"], float)
            self.vedits.append(v)
        self._vnext = max([int(d.get("vnext", 1))] + [int(v["id"]) + 1 for v in self.vedits])
        self.brushes = [dict(b) for b in d.get("brushes", [])]
        self._bnext = max([int(d.get("bnext", 1))] + [int(b["id"]) + 1 for b in self.brushes])
        for e in d.get("edits", []):
            for k in ("ts", "f0", "off", "c0", "c1"):
                if e.get(k) is not None:
                    e[k] = np.array(e[k], float)
            e["params"] = {**DEFAULTS, **e.get("params", {})}
            self.edits.append(e)
        self._next = max([int(d.get("next", 1))] + [int(e["id"]) + 1 for e in self.edits])
        self.rebuild()

    def rebuild(self):
        self.rev = getattr(self, "rev", 0) + 1
        self.cur = self.orig.copy()
        for b in self.brushes:
            if b["enabled"]:
                self._run_brush(b)
        for e in self.edits:
            if e["enabled"]:
                self._run_edit(e)
        for v in self.vedits:
            if v["enabled"]:
                self._run_vedit(v)

    def export(self, mode="copy", folder=None, suffix="_retuned"):
        """mode 'copy': NAME<suffix>.wav in `folder` (default: next to each original).
        mode 'overwrite': replace each original file. Every file is written to a temporary name first and only
        swapped in once all of them were written, so a failure part-way leaves the originals alone."""
        srcs = self.sources or [dict(path=self.path, n=self.n, c0=0, c1=self.ch, info=self.info)]
        jobs, seen = [], set()
        for sc in srcs:
            if mode == "overwrite":
                dest = sc["path"]
            else:
                base = os.path.splitext(os.path.basename(sc["path"]))[0]
                dest = os.path.join(folder or os.path.dirname(os.path.abspath(sc["path"])), base + (suffix or "") + ".wav")
                if os.path.abspath(dest) == os.path.abspath(sc["path"]):
                    raise ValueError("That would overwrite the original %s. Add a suffix or choose another folder "
                                     "(or use 'Overwrite originals')." % os.path.basename(dest))
            key = os.path.normcase(os.path.abspath(dest))
            if key in seen:
                raise ValueError("Two files would be saved with the same name (%s)." % os.path.basename(dest))
            seen.add(key)
            jobs.append((dest, sc))
        if folder:
            os.makedirs(folder, exist_ok=True)
        tmps = []
        try:
            for i, (dest, sc) in enumerate(jobs):
                _prog("Exporting file %d of %d" % (i + 1, len(jobs)), 0.9 * i / len(jobs))
                tmp = dest + ".writing.tmp"
                write_wav(tmp, self.sr, self.cur[:sc["n"], sc["c0"]:sc["c1"]], sc["info"])
                tmps.append((tmp, dest))
            _prog("Finishing", 0.92)
            for tmp, dest in tmps:
                os.replace(tmp, dest)
            if mode == "overwrite":
                self.overwritten = True
            _prog("Done", 1.0, False)
        except Exception:
            for tmp, _ in tmps:
                try:
                    os.remove(tmp)
                except OSError:
                    pass
            raise
        return [d for d, _ in jobs]

    def audio(self, version, t0, t1):
        src = self.orig if version == "orig" else self.cur
        seg, _ = self.seg(t0, t1, src)
        return wav_bytes(seg, self.sr, mix=self.mix())
