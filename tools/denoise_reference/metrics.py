"""What is measured on the bench (ground truth known):
  harmonic preservation : energy kept in the clean signal's own partial bins above f_lo (dB; 0 = all of it, negative = dulled)
  tail                  : output envelope vs the ideal (clean + the residual share of the noise) through a decay: mean/max error,
                          the biggest sudden drop (a 'click-off'), and when the output first falls 6 dB under the ideal
  noise reduction       : level in a noise-only stretch, input vs output (target = -residual)
  musical noise         : how much more the residual noise's spectrum flickers than the input noise's (dB std over time, per bin)
  SNR                   : before / after
"""
import numpy as np
from audio import Stft, db


def _spec(x, sr, n=8192):
    st = Stft(n); X = st.forward(x); return np.abs(X) ** 2, st.freqs(sr), st


def harmonic_preservation(clean, out, sr, f_lo=4000.0, f_hi=None, peak_db=10.0):
    Pc, f, st = _spec(clean, sr); Po, _, _ = _spec(out, sr)
    f_hi = f_hi or sr * 0.45
    band = (f >= f_lo) & (f < f_hi)
    Pc = Pc[band]; Po = Po[band]
    med = np.median(Pc, axis=0, keepdims=True)
    # partial bins: local peaks well above the frame's median in this band
    peak = (Pc > np.roll(Pc, 1, 0)) & (Pc >= np.roll(Pc, -1, 0)) & (Pc > med * 10 ** (peak_db / 10))
    # only frames with real signal in this band
    frame_ok = np.sum(Pc, axis=0) > 10 * np.median(np.sum(Pc, axis=0))
    peak &= frame_ok[None, :]
    if not np.any(peak): return float('nan'), 0
    ec = np.sum(Pc[peak]); eo = np.sum(Po[peak])
    return float(10 * np.log10(eo / ec)), int(np.sum(peak))


def band_energy_ratio(clean, out, sr, f_lo, f_hi):
    """energy in a band, output vs clean, over frames where the clean has signal there (dB)"""
    Pc, f, _ = _spec(clean, sr); Po, _, _ = _spec(out, sr)
    m = (f >= f_lo) & (f < f_hi)
    ec = np.sum(Pc[m], axis=0); eo = np.sum(Po[m], axis=0)
    ok = ec > 10 * np.median(ec)
    if not np.any(ok): return float('nan')
    return float(10 * np.log10(np.sum(eo[ok]) / np.sum(ec[ok])))


def envelope_db(x, sr, frame_s=0.05, f_lo=None, f_hi=None):
    if f_lo is not None:
        from scipy import signal
        sos = signal.butter(4, [f_lo, min(f_hi, sr * 0.49)], btype='band', fs=sr, output='sos'); x = signal.sosfiltfilt(sos, x)
    b = int(frame_s * sr); k = len(x) // b
    return 10 * np.log10(np.mean(x[:k * b].reshape(k, b) ** 2, axis=1) + 1e-20), np.arange(k) * frame_s


def oracle(clean, noise, sr, residual_db, n=8192):
    """what a single-channel method could do at best: the noisy signal through a Wiener mask made from the TRUE clean and noise
    powers (each smoothed over +-2 frames and +-1 bin), with the same residual floor"""
    st = Stft(n); C = st.forward(clean); Nn = st.forward(noise); Y = C + Nn
    Pc = np.abs(C) ** 2; Pn = np.abs(Nn) ** 2
    k = np.ones(5) / 5
    sm = lambda A: np.apply_along_axis(lambda r: np.convolve(r, k, mode='same'), 1, A)
    Pc = sm(Pc); Pn = sm(Pn)
    Pc = 0.25 * np.roll(Pc, 1, 0) + 0.5 * Pc + 0.25 * np.roll(Pc, -1, 0); Pn = 0.25 * np.roll(Pn, 1, 0) + 0.5 * Pn + 0.25 * np.roll(Pn, -1, 0)
    G = np.maximum(Pc / (Pc + Pn + 1e-30), 10 ** (residual_db / 20.0))
    return st.inverse(Y * G.astype(np.float32))


def tail_metrics(clean, noise, out, sr, residual_db, t0, t1, f_lo=None, f_hi=None, ideal=None):
    """through the decay t0..t1: output envelope vs the oracle's (see oracle())"""
    if ideal is None: ideal = oracle(clean, noise, sr, residual_db)
    eo, t = envelope_db(out, sr, f_lo=f_lo, f_hi=f_hi); ei, _ = envelope_db(ideal, sr, f_lo=f_lo, f_hi=f_hi)
    m = (t >= t0) & (t < t1)
    e = eo[m] - ei[m]
    d_out = np.diff(eo[m]); d_ideal = np.diff(ei[m])
    drops = d_out - d_ideal                    # how much faster the output falls than the ideal, frame to frame
    under = np.where(e < -6.0)[0]
    cut = float(t[m][under[0]]) if len(under) else float('nan')
    k = 7                                      # 350 ms at 50 ms frames: the extra fall of the output over any such window
    eo_m, ei_m = eo[m], ei[m]
    d350 = (eo_m[:-k] - eo_m[k:]) - (ei_m[:-k] - ei_m[k:]) if len(eo_m) > k else np.zeros(1)
    return dict(mean_err=float(np.mean(e)), max_err=float(np.max(np.abs(e))), worst_drop=float(-np.min(drops)) if len(drops) else 0.0,
                worst_drop350=float(np.max(d350)), cutoff_t=cut, curve=(t[m], eo[m], ei[m]))


def audible(x, sr):
    """30 Hz high-pass: sub-sonic rumble carries most of a room tone's energy but none of what is heard"""
    from scipy import signal
    return signal.sosfiltfilt(signal.butter(2, 30.0, btype='high', fs=sr, output='sos'), x)


def noise_reduction(noisy, out, sr, t0, t1):
    a = audible(noisy[int(t0 * sr):int(t1 * sr)], sr); b = audible(out[int(t0 * sr):int(t1 * sr)], sr)
    return float(20 * np.log10(np.sqrt(np.mean(a ** 2)) / (np.sqrt(np.mean(b ** 2)) + 1e-12)))


def musical_noise(noisy, out, sr, t0, t1, n=2048):
    """std over time of each bin's log power in a noise-only stretch, output minus input (dB). Rayleigh noise: ~5.6 dB; musical noise is higher."""
    a = noisy[int(t0 * sr):int(t1 * sr)]; b = out[int(t0 * sr):int(t1 * sr)]
    Pa, _, _ = _spec(a, sr, n); Pb, _, _ = _spec(b, sr, n)
    e = n // (n // 4)
    sa = np.mean(np.std(db(Pa[:, e:-e]), axis=1)); sb = np.mean(np.std(db(Pb[:, e:-e]), axis=1))
    return float(sb - sa), float(sa), float(sb)


def snr_db(clean, other, sr=96000):
    c = audible(clean, sr); o = audible(other, sr)
    return float(10 * np.log10(np.sum(c ** 2) / (np.sum((o - c) ** 2) + 1e-20)))
