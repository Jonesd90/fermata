"""WAV reading / writing and STFT helpers for the denoiser bench (numpy only)."""
import numpy as np, wave, struct, warnings
from scipy.io import wavfile

import os
UPLOADS = os.environ.get('FERMATA_DN_CLIPS', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'clips')) + os.sep   # real clips (not in the repo)


def read_wav(path):
    """-> (sr, x[samples] or x[samples, ch]) as float64 in -1..1"""
    with warnings.catch_warnings():
        warnings.simplefilter('ignore')
        sr, d = wavfile.read(path)
    if d.dtype == np.int16: x = d.astype(np.float64) / 32768.0
    elif d.dtype == np.int32: x = d.astype(np.float64) / 2147483648.0
    elif d.dtype == np.uint8: x = (d.astype(np.float64) - 128) / 128.0
    else: x = d.astype(np.float64)
    return sr, x


def write_wav24(path, sr, x):
    """24-bit PCM WAV; x in -1..1, shape [n] or [n, ch]."""
    x = np.asarray(x, dtype=np.float64)
    if x.ndim == 1: x = x[:, None]
    x = np.clip(x, -1.0, 1.0 - 1.0 / 8388608)
    i = np.round(x * 8388608.0).astype(np.int32)
    b = i.astype('<i4').tobytes()
    arr = np.frombuffer(b, dtype=np.uint8).reshape(-1, 4)[:, :3]   # little-endian: drop the top byte
    with wave.open(path, 'wb') as w:
        w.setnchannels(x.shape[1]); w.setsampwidth(3); w.setframerate(int(sr))
        w.writeframes(arr.tobytes())


class Stft:
    """Hann analysis + synthesis, hop N/4, exact reconstruction (window-sum-square normalisation)."""
    def __init__(self, n=8192, hop=None):
        self.n = n; self.hop = hop or n // 4
        self.win = np.hanning(n + 1)[:-1]                       # periodic Hann

    def forward(self, x):
        n, hop = self.n, self.hop
        pad = n                                                 # (whole frames at both ends)
        xp = np.concatenate([np.zeros(pad), x, np.zeros(pad + n)])
        frames = 1 + (len(xp) - n) // hop
        idx = np.arange(n)[None, :] + hop * np.arange(frames)[:, None]
        X = np.fft.rfft(xp[idx] * self.win[None, :], axis=1)   # [frames, bins]
        self.len_x = len(x); self.pad = pad
        return X.T.astype(np.complex64)                         # [bins, frames]

    def inverse(self, X):
        n, hop = self.n, self.hop
        Xf = X.T.astype(np.complex128)                          # [frames, bins]
        frames = Xf.shape[0]
        y = np.zeros(n + hop * (frames - 1)); wss = np.zeros_like(y)
        fr = np.fft.irfft(Xf, n=n, axis=1) * self.win[None, :]
        for t in range(frames):
            y[t * hop:t * hop + n] += fr[t]; wss[t * hop:t * hop + n] += self.win ** 2
        y = y / np.maximum(wss, 1e-6)
        return y[self.pad:self.pad + self.len_x]

    def freqs(self, sr): return np.fft.rfftfreq(self.n, 1.0 / sr)
    def frame_times(self, sr, frames): return (np.arange(frames) * self.hop - self.pad + self.n / 2) / sr


def db(x): return 10.0 * np.log10(np.maximum(x, 1e-30))
def rms_db(x): return 20.0 * np.log10(np.sqrt(np.mean(np.asarray(x) ** 2)) + 1e-12)
