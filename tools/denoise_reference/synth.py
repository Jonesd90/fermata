"""Test-signal generators: harmonically rich voices, a church-like reverb with per-band decay, room tone (synthetic and real)."""
import numpy as np
from scipy import signal
from audio import read_wav, UPLOADS


def harmonic_voice(sr, dur, f0, nharm=40, brightness=0.8, vib_hz=5.5, vib_cents=12.0, formants=True, seed=0):
    """One voice: nharm partials at f0, amplitudes 1/k^brightness shaped by a choir-like formant curve, coherent vibrato,
    a little slow amplitude movement. Returns float64 at peak ~0.5."""
    rng = np.random.default_rng(seed)
    n = int(dur * sr); t = np.arange(n) / sr
    vib = 1.0 + (2 ** (vib_cents / 1200.0) - 1.0) * np.sin(2 * np.pi * vib_hz * t + rng.uniform(0, 6.28)) if vib_cents > 0 else np.ones(n)
    phase = 2 * np.pi * np.cumsum(f0 * vib) / sr
    out = np.zeros(n)
    for k in range(1, nharm + 1):
        f = k * f0
        if f > sr * 0.45: break
        a = 1.0 / k ** brightness
        if formants:   # three broad formant bumps (500, 1500, 2600 Hz) + gentle roll-off above 5 kHz, like a sung vowel
            a *= 1.0 + 1.5 * np.exp(-((f - 500) / 250) ** 2) + 1.0 * np.exp(-((f - 1500) / 400) ** 2) + 0.8 * np.exp(-((f - 2600) / 500) ** 2)
            a *= 1.0 / (1.0 + (f / 5000.0) ** 1.2)
        am = 1.0 + 0.08 * np.sin(2 * np.pi * rng.uniform(0.2, 0.7) * t + rng.uniform(0, 6.28))
        out += a * am * np.sin(k * phase + rng.uniform(0, 6.28))
    env = np.ones(n)
    a_len = int(0.08 * sr); r_len = int(0.12 * sr)
    env[:a_len] = np.linspace(0, 1, a_len) ** 2; env[-r_len:] = np.linspace(1, 0, r_len) ** 2
    out *= env
    return out / (np.max(np.abs(out)) + 1e-12) * 0.5


def chord(sr, dur, f0s, **kw):
    seed = kw.pop('seed', 0)
    out = np.zeros(int(dur * sr))
    for i, f0 in enumerate(f0s): out += harmonic_voice(sr, dur, f0, seed=seed + i, **kw)
    return out / (np.max(np.abs(out)) + 1e-12) * 0.5


def church_ir(sr, rt60_by_band=((63, 5.0), (125, 5.0), (250, 4.5), (500, 4.0), (1000, 3.5), (2000, 2.8), (4000, 2.0), (8000, 1.3), (16000, 0.8)),
              predelay=0.02, seed=1):
    """Synthetic concert-hall / church impulse response: octave-band noise, each band decaying with its own RT60 (seconds)."""
    rng = np.random.default_rng(seed)
    length = int(sr * (max(r for _, r in rt60_by_band) * 1.3 + predelay))
    t = np.arange(length) / sr
    ir = np.zeros(length)
    white = rng.standard_normal(length)
    for fc, rt in rt60_by_band:
        lo, hi = fc / np.sqrt(2), min(fc * np.sqrt(2), sr * 0.49)
        sos = signal.butter(4, [lo, hi], btype='band', fs=sr, output='sos')
        band = signal.sosfilt(sos, white)
        ir += band * np.exp(-6.908 * np.maximum(t - predelay, 0) / rt) * (t >= predelay)
    ir /= np.sqrt(np.sum(ir ** 2)) * 0.5                          # tail energy
    ir[0] += 1.0                                                  # direct sound
    return ir


def reverb(x, ir, wet=0.5):
    y = signal.fftconvolve(x, ir)[:len(x) + len(ir)]
    return y / (np.max(np.abs(y)) + 1e-12) * np.max(np.abs(x)) * (0.5 + wet)


def synthetic_room_tone(sr, dur, kind='distant', seed=3):
    """Room tone: brown-ish rumble + pink floor + a little white hiss + 50 Hz hum with harmonics. 'distant' is heavy at LF, 'close' is quiet."""
    rng = np.random.default_rng(seed); n = int(dur * sr); t = np.arange(n) / sr
    w = rng.standard_normal(n)
    pink = signal.lfilter([0.049922, -0.095993, 0.050612, -0.004408], [1, -2.494956, 2.017265, -0.522189], w)
    rumble = signal.sosfilt(signal.butter(2, 120, fs=sr, output='sos'), rng.standard_normal(n)) * 8.0
    hiss = rng.standard_normal(n)
    hum = sum(np.sin(2 * np.pi * 50 * k * t + rng.uniform(0, 6.28)) / k ** 1.5 for k in range(1, 8))
    if kind == 'distant': mix = 1.0 * rumble + 0.25 * pink + 0.01 * hiss + 0.03 * hum
    else: mix = 0.15 * rumble + 0.08 * pink + 0.02 * hiss + 0.004 * hum
    return mix / (np.sqrt(np.mean(mix ** 2)) + 1e-12)              # unit RMS; scale with level_db


def real_room_tone(sr_wanted, dur, src='Ice_Shining_020_OMNI-L', region=(0.0, 2.4), seed=4):
    """Real room tone from a clip's quiet region, tiled with 50 ms crossfades to the wanted length (unit RMS)."""
    import glob
    sr, x = read_wav(glob.glob(UPLOADS + '*' + src + '.wav')[0])
    if x.ndim > 1: x = x[:, 0]
    seg = x[int(region[0] * sr):int(region[1] * sr)]
    seg = seg - np.mean(seg)
    assert sr == sr_wanted
    rng = np.random.default_rng(seed)
    n = int(dur * sr); out = np.zeros(n + len(seg)); xf = int(0.05 * sr); pos = 0
    while pos < n:
        piece = seg.copy() if rng.uniform() < 0.5 else seg[::-1].copy()
        ramp = np.linspace(0, 1, xf)
        piece[:xf] *= ramp
        if pos > 0: out[pos:pos + xf] *= (1 - ramp)
        out[pos:pos + len(piece)] += piece
        pos += len(piece) - xf
    out = out[:n]
    return out / (np.sqrt(np.mean(out ** 2)) + 1e-12)


def at_db(noise_unit_rms, level_db):
    return noise_unit_rms * 10 ** (level_db / 20.0)
