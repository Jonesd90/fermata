"""A stand-in for a conventional real-time spectral denoiser: short windows, a per-bin threshold against the noise profile,
instant attack and a release ("decay") of at most 350 ms, a reduction floor, light frequency smoothing. It is NOT RX, but it
switches off the way the user describes: once a bin falls under the threshold, its gain drops to the floor within the release time."""
import numpy as np
from audio import Stft, db


def process(x, sr, noise_regions, threshold_db=3.0, release_s=0.35, floor_db=-12.0, win_s=0.021, smooth_bins=2):
    n = int(2 ** round(np.log2(win_s * sr)))
    st = Stft(n)
    frames = []
    for r in noise_regions:
        Xn = st.forward(np.asarray(r, dtype=np.float64)); e = st.n // st.hop
        frames.append(np.abs(Xn[:, e:-e]) ** 2)
    npsd = np.maximum(np.mean(np.concatenate(frames, axis=1), axis=1), 1e-20)
    X = st.forward(np.asarray(x, dtype=np.float64))
    P = np.abs(X) ** 2
    above = P > npsd[:, None] * 10 ** (threshold_db / 10.0)
    gmin = 10 ** (floor_db / 20.0)
    target = np.where(above, 1.0, gmin)
    # release: gain falls towards the floor with a time constant (per bin), rises at once
    G = np.empty_like(target); a = np.exp(-st.hop / sr / (release_s / 3.0))
    g = np.full(target.shape[0], 1.0)
    for t in range(target.shape[1]):
        g = np.where(target[:, t] >= g, target[:, t], a * g + (1 - a) * target[:, t])
        G[:, t] = g
    if smooth_bins > 0:
        k = np.ones(2 * smooth_bins + 1) / (2 * smooth_bins + 1)
        G = np.apply_along_axis(lambda c: np.convolve(c, k, mode='same'), 0, G)
    y = st.inverse(X * G.astype(np.float32))
    return y, x - y
