"""Fermata background denoiser - offline, per track, prototype.

Learn: a noise profile (per-bin mean power and its spread) from one or more room-tone regions.
Process (whole file, two directions):
  1. long-window STFT (85 ms at 96 kHz: a sustained partial sits in one narrow bin, well above the per-bin noise)
  2. a priori SNR per bin by the decision-directed method run FORWARDS and BACKWARDS; the larger of the two is used
     (backwards sees a note coming, so its first frame is not softened; forwards follows a tail down without anticipating the silence after it)
  3. smoothing of the SNR estimate over neighbouring bins (a partial moving with vibrato is seen by its neighbours)
  4. harmonic protection: a spectral peak that persists over several frames is trusted at its own measured SNR
  5. gain = MMSE log-spectral-amplitude estimator (Ephraim-Malah): a soft curve with no threshold anywhere
  6. the gain never falls below the residual (room tone is reduced, not removed)
  7. tail rule: in each band the gain may not FALL faster than that band's own decay rate (measured from the file: how fast
     the room's reverberation dies away there) plus a small margin - so the processor cannot switch off faster than the room does
  8. optional high-frequency protection (a ceiling on attenuation above a chosen frequency)
The removed part is the time-domain difference (input - output), so it can be auditioned exactly.
"""
import numpy as np
from scipy.special import exp1
from audio import Stft, db


class Params:
    def __init__(self, **kw):
        self.win_s = 0.085            # STFT window in seconds (power of two samples nearest)
        self.amount_db = 0.0          # scales the noise profile: +3 dB removes more, -3 dB less
        self.residual_db = -12.0      # the gain floor: how much of the room tone is left
        self.tail_s = None            # None = measure the room's decay per band; a number = RT60-like seconds for every band
        self.tail_margin_db_s = 6.0   # the gain may fall this much faster (dB/s) than the room decays
        self.tail_prior = True        # the a priori SNR follows the room's decay down through the noise (the tail rule on the estimate itself)
        self.tail_seed_snr = 2.0      # ... starting only from an SNR of at least this (linear; 2 = 3 dB), so noise flicker never starts a 'tail'
        self.dd_alpha = 0.85          # decision-directed smoothing (per hop)
        self.dd_mode = 'max'          # 'max' (take the larger of forwards/backwards), 'mean' (geometric mean), 'fwd'
        self.freq_smooth_bins = 2     # +- bins of SNR smoothing
        self.time_smooth_frames = 1   # +- frames of final gain smoothing
        self.harmonic_protect = True
        self.peak_frames = 3          # a peak must persist this many frames
        self.peak_snr = 3.0           # ... at this a posteriori SNR (linear) or more
        self.peak_prominence_db = 8.0 # ... standing this far above the bins 4-7 away
        self.hf_protect_hz = None     # above this frequency, attenuation is limited to hf_protect_db (None = off)
        self.hf_protect_db = 12.0
        self.presence = True          # signal-presence weighting: where nothing but noise is heard, the gain goes to the residual (OM-LSA style)
        self.presence_local = (1.5, 6.0)   # a posteriori SNR (dB, averaged over +-2 bins x +-2 frames) that counts as 'nothing there' .. 'something there'
        self.presence_global = (0.8, 3.0)  # the same over +-16 bins
        self.presence_qmax = 0.98     # the most 'absent' a bin can be judged a priori
        for k, v in kw.items():
            if not hasattr(self, k): raise KeyError(k)
            setattr(self, k, v)

    def n_fft(self, sr): return int(2 ** round(np.log2(self.win_s * sr)))


class Profile:
    def __init__(self, npsd, nlogstd, n_fft, sr):
        self.npsd, self.nlogstd, self.n_fft, self.sr = npsd, nlogstd, n_fft, sr


def learn_profile(regions, sr, p):
    """regions: list of 1-D arrays of room tone (each at least ~0.5 s). Per-bin mean power (lightly smoothed across bins) and the spread of its log."""
    st = Stft(p.n_fft(sr))
    frames = []
    for r in regions:
        X = st.forward(np.asarray(r, dtype=np.float64))
        n_edge = st.n // st.hop                      # the padded edge frames are partly zeros: leave them out
        frames.append(np.abs(X[:, n_edge:-n_edge]) ** 2 if X.shape[1] > 2 * n_edge + 2 else np.abs(X) ** 2)
    P = np.concatenate(frames, axis=1).astype(np.float64)
    npsd = np.mean(P, axis=1)
    k = np.array([0.25, 0.5, 0.25])
    npsd = np.convolve(npsd, k, mode='same'); npsd[0] = P[0].mean(); npsd[-1] = P[-1].mean()
    nlogstd = np.std(db(P), axis=1)
    return Profile(np.maximum(npsd, 1e-20), nlogstd, st.n, sr)


def _smooth_bins(A, k):
    """triangular smoothing over +-k bins (axis 0)"""
    if k <= 0: return A
    w = np.concatenate([np.arange(1, k + 2), np.arange(k, 0, -1)]).astype(np.float64); w /= w.sum()
    out = np.zeros_like(A)
    for i, wi in enumerate(w):
        s = i - k
        if s >= 0: out[s:] += wi * A[:A.shape[0] - s]
        else: out[:s] += wi * A[-s:]
    # edges: renormalise
    norm = np.zeros(A.shape[0])
    for i, wi in enumerate(w):
        s = i - k
        if s >= 0: norm[s:] += wi
        else: norm[:s] += wi
    return out / norm[:, None]


def _smooth_frames(A, k):
    if k <= 0: return A
    w = np.concatenate([np.arange(1, k + 2), np.arange(k, 0, -1)]).astype(np.float64); w /= w.sum()
    out = np.zeros_like(A); norm = np.zeros(A.shape[1])
    for i, wi in enumerate(w):
        s = i - k
        if s >= 0: out[:, s:] += wi * A[:, :A.shape[1] - s]; norm[s:] += wi
        else: out[:, :s] += wi * A[:, -s:]; norm[:s] += wi
    return out / norm[None, :]


def _dd(gamma, alpha, backwards=False):
    """decision-directed a priori SNR, one direction"""
    T = gamma.shape[1]
    xi = np.zeros_like(gamma)
    order = range(T - 1, -1, -1) if backwards else range(T)
    prev_clean = None
    for t in order:
        inst = np.maximum(gamma[:, t] - 1.0, 0.0)
        if prev_clean is None: x = inst
        else: x = alpha * prev_clean + (1 - alpha) * inst
        xi[:, t] = x
        g = x / (1.0 + x)
        prev_clean = g * g * gamma[:, t]
    return xi


def _lsa_gain(xi, gamma):
    v = xi / (1.0 + xi) * gamma
    v = np.clip(v, 1e-8, 500.0)
    return np.minimum(xi / (1.0 + xi) * np.exp(0.5 * exp1(v)), 1.0)


def measure_decay(P, freqs, sr, hop, npsd, min_len_s=0.4, min_above_db=15.0):
    """Per 1/3-octave band: the room's decay rate in dB/s, measured from the decays in the file (the 80th percentile of the slopes of
    monotone decays that last >= min_len_s and start >= min_above_db above the band's noise; the mean of two). Returns (band_edges, rates[bands] or nan)."""
    edges = 50.0 * 2 ** (np.arange(0, 28) / 3.0); edges = edges[edges < sr * 0.49]
    rates = np.full(len(edges) - 1, np.nan)
    dt = hop / sr
    for b in range(len(edges) - 1):
        m = (freqs >= edges[b]) & (freqs < edges[b + 1])
        if not np.any(m): continue
        E = db(np.sum(P[m], axis=0)); Nf = db(np.sum(npsd[m]))
        # smooth a little (3 frames) so one noisy frame does not end a decay
        Es = np.convolve(E, np.ones(3) / 3, mode='same')
        slopes = []
        t = 1
        T = len(Es)
        while t < T - 1:
            if Es[t] >= Es[t - 1] and Es[t] >= Es[t + 1] and Es[t] > Nf + min_above_db:      # a local maximum well above the noise
                run_min = Es[t]; u = t + 1
                while u < T and Es[u] <= run_min + 1.5 and Es[u] > Nf + 6.0:
                    run_min = min(run_min, Es[u]); u += 1
                if (u - t) * dt >= min_len_s:
                    seg = Es[t:u]; x = np.arange(len(seg)) * dt
                    slope = -np.polyfit(x, seg, 1)[0]
                    if slope > 1.0: slopes.append(slope)
                t = max(u, t + 1)
            else: t += 1
        # the room's decay is the FASTEST a band can fall (a diminuendo is slower, never faster): the 80th percentile of the slopes (the mean
        # of two). The median took the diminuendos in and measured real takes at 7 - 9 s where their final decays showed 1 - 4 s.
        if len(slopes) == 2: rates[b] = float(np.mean(slopes))
        elif len(slopes) >= 3: rates[b] = float(np.percentile(slopes, 80))
    return edges, rates


def band_rate_per_bin(freqs, edges, rates, default_db_s, lo=6.0, hi=60.0):
    """rates per bin: unmeasured bands take the nearest measured one, each band is the median of itself and its neighbours (one odd
    band - a held note mistaken for the room - cannot steer it), and everything is kept within lo..hi dB/s (decay times 10 s .. 1 s)"""
    r = np.full(len(freqs), default_db_s)
    good = np.where(~np.isnan(rates))[0]
    filled = rates.copy()
    if len(good):
        for b in range(len(rates)):
            if np.isnan(filled[b]): filled[b] = rates[good[np.argmin(np.abs(good - b))]]
    else: filled[:] = default_db_s
    sm = filled.copy()
    for b in range(len(filled)): sm[b] = np.median(filled[max(0, b - 1):b + 2])
    filled = np.clip(sm, lo, hi)
    for b in range(len(edges) - 1):
        m = (freqs >= edges[b]) & (freqs < edges[b + 1]); r[m] = filled[b]
    r[freqs < edges[0]] = filled[0]; r[freqs >= edges[-1]] = filled[-1]
    return r


def process(x, sr, prof, p, info=None):
    """-> (output, removed). info (dict) receives gains, snr and decay rates when given."""
    x = np.asarray(x, dtype=np.float64)
    st = Stft(p.n_fft(sr)); assert st.n == prof.n_fft, "profile made with another window"
    X = st.forward(x)
    P = (np.abs(X) ** 2).astype(np.float64)
    N = prof.npsd[:, None] * 10 ** (p.amount_db / 10.0)
    gamma = P / N
    freqs = st.freqs(sr); dt = st.hop / sr

    # the room's decay per band (from the file itself): the a priori SNR may not fall faster than that - the noise is steady, so a
    # reverberation tail's SNR falls exactly as the tail does, and the estimate follows it down through the noise instead of letting go
    edges, rates = measure_decay(P, freqs, sr, st.hop, prof.npsd)
    if p.tail_s is not None: rates[:] = 60.0 / p.tail_s
    rate_bin = band_rate_per_bin(freqs, edges, rates, 60.0 / 2.0) + p.tail_margin_db_s
    fall = 10 ** (-rate_bin * dt / 10.0)                       # power ratio per hop

    # a priori SNR, both directions
    xi_f = _dd(gamma, p.dd_alpha, False)
    xi_prior = np.zeros_like(xi_f)
    if p.tail_prior:
        # seeded by the SNR averaged over a small patch (+-2 frames, +-2 bins): a lone noise spike is diluted to nothing, a real tail keeps its value
        seed = _smooth_bins(_smooth_frames(xi_f, 2), 2)
        seed = np.where(seed >= p.tail_seed_snr, seed, 0.0)                     # only something clearly above the noise starts a tail
        for t in range(1, xi_f.shape[1]): xi_prior[:, t] = np.maximum(seed[:, t], xi_prior[:, t - 1] * fall)
        xi_f = np.maximum(xi_f, xi_prior)
    if p.dd_mode == 'fwd': xi = xi_f
    else:
        xi_b = _dd(gamma, p.dd_alpha, True)
        xi = np.maximum(xi_f, xi_b) if p.dd_mode == 'max' else np.sqrt(xi_f * xi_b)
    # smoothing across bins (log domain)
    if p.freq_smooth_bins > 0:
        xi = 10 ** (_smooth_bins(db(xi + 1e-6), p.freq_smooth_bins) / 10.0)
    # harmonic protection: persistent spectral peaks keep their own measured SNR
    if p.harmonic_protect:
        Ps = _smooth_frames(P, 1)
        # a partial: a local maximum that stands well above the bins 4..7 away on both sides (a broadband burst, however loud, does not:
        # its neighbours are just as loud), persisting over several frames, and above the noise
        far = np.zeros_like(Ps)
        for d in (4, 5, 6, 7): far += np.roll(Ps, d, 0) + np.roll(Ps, -d, 0)
        far /= 8.0
        peak = (Ps > np.roll(Ps, 1, 0)) & (Ps >= np.roll(Ps, -1, 0)) & (Ps > np.roll(Ps, 2, 0)) & (Ps >= np.roll(Ps, -2, 0)) \
               & (Ps > far * 10 ** (p.peak_prominence_db / 10.0)) & (gamma > p.peak_snr)
        peak[:8] = False; peak[-8:] = False                     # (the rolls wrap round at the ends; nothing below ~100 Hz is a partial to protect)
        persist = peak.copy()
        for k in range(1, p.peak_frames):
            persist[:, k:] &= peak[:, :-k]
        for k in range(1, p.peak_frames):          # mark the whole run, not just its end
            persist[:, :-k] |= persist[:, k:]
        prot = persist | np.roll(persist, 1, 0) | np.roll(persist, -1, 0)
        xi = np.where(prot, np.maximum(xi, np.maximum(gamma - 1.0, 0.0)), xi)
    G = _lsa_gain(xi, gamma)
    gmin = 10 ** (p.residual_db / 20.0)
    if p.presence:
        # how likely is it that this bin holds anything but noise? Judged from the a posteriori SNR averaged over a small and a wide
        # neighbourhood (noise alone averages to 0 dB; anything real lifts it), then refined per bin by the Bayesian presence rule.
        gdb = db(np.maximum(gamma, 1.0 + xi_prior)) if p.tail_prior else db(gamma)    # (a tail carried by the decay prior counts as present)
        loc = _smooth_bins(_smooth_frames(gdb, 2), 2)
        glo = _smooth_bins(_smooth_frames(gdb, 2), 16)
        def smoothstep(a, lo, hi):
            u = np.clip((a - lo) / (hi - lo), 0.0, 1.0); return u * u * (3 - 2 * u)
        prior = smoothstep(loc, *p.presence_local) * smoothstep(glo, *p.presence_global)
        q = np.clip(1.0 - prior, 0.0, p.presence_qmax)
        v = np.clip(xi / (1.0 + xi) * gamma, 0.0, 500.0)
        pres = 1.0 / (1.0 + q / (1.0 - q) * (1.0 + xi) * np.exp(-v))
        if p.harmonic_protect: pres = np.where(prot, 1.0, pres)
        G = np.exp(pres * np.log(np.maximum(G, 1e-6)) + (1.0 - pres) * np.log(gmin))
    G = np.maximum(G, gmin)

    # the tail rule on the gain too: per band, the gain may not fall faster than the room decays there
    Gdb = 20 * np.log10(G)
    step = rate_bin * dt
    for t in range(1, Gdb.shape[1]):
        Gdb[:, t] = np.maximum(Gdb[:, t], Gdb[:, t - 1] - step)
    if p.hf_protect_hz is not None:
        Gdb[freqs >= p.hf_protect_hz] = np.maximum(Gdb[freqs >= p.hf_protect_hz], -p.hf_protect_db)
    G = 10 ** (Gdb / 20.0)
    if p.time_smooth_frames > 0: G = _smooth_frames(G, p.time_smooth_frames)
    G = np.clip(G, gmin, 1.0)

    y = st.inverse(X * G.astype(np.float32))
    if info is not None:
        info.update(dict(G=G, xi=xi, gamma=gamma, freqs=freqs, times=st.frame_times(sr, X.shape[1]), edges=edges, rates=rates, hop=st.hop, n=st.n))
    return y, x - y
