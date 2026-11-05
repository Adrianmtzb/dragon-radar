#!/usr/bin/env python3
"""Analyse board microphone recordings (tools/record.py output).

Usage: tools/analyse_audio.py file.wav [file.wav ...]
Per file: DC, noise floor, peak and RMS of the loud part, clipping, then the tonal segments found
(10 ms frames with a stable pitch): start, length, median pitch, level, 2nd and 3rd harmonic
relative to the fundamental, and "clicks" (sample-to-sample jumps far above the signal's own slope).
Mic 1 (left channel) is analysed; the right channel's level is reported for comparison.
"""
import sys
import wave

import numpy as np


def load(path):
    with wave.open(path) as w:
        rate, ch = w.getframerate(), w.getnchannels()
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64) / 32768.0
    return rate, x.reshape(-1, ch)


def frame_pitch(seg, rate):
    """Fundamental by FFT peak with parabolic interpolation (Hann, zero-padded)."""
    n = 1 << 14
    spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), n))
    lo = int(300 * n / rate)
    k = lo + int(np.argmax(spec[lo:]))
    if 1 <= k < len(spec) - 1:
        a, b, c = np.log(spec[k - 1:k + 2] + 1e-12)
        k = k + 0.5 * (a - c) / (a - 2 * b + c)
    return k * rate / n, spec


def harmonic(spec, rate, f0, h):
    n = (len(spec) - 1) * 2
    k = int(round(f0 * h * n / rate))
    if k + 3 >= len(spec):
        return None
    return spec[k - 3:k + 4].max()


def analyse(path):
    rate, x = load(path)
    m = x[:, 0]
    dc = m.mean()
    m = m - dc
    hop = rate // 100  # 10 ms
    frames = [m[i:i + hop] for i in range(0, len(m) - hop, hop)]
    rms = np.array([np.sqrt(np.mean(f ** 2)) for f in frames])
    floor = np.percentile(rms, 10)
    loud = rms > max(4 * floor, 0.003)
    print(f"\n{path}: {len(m) / rate * 1000:.0f} ms at {rate} Hz")
    print(f"  DC {dc * 100:+.2f} % FS, noise floor {20 * np.log10(floor + 1e-9):.1f} dBFS, "
          f"peak {np.abs(m).max() * 100:.1f} % FS, clipped samples {(np.abs(x[:, 0]) > 0.995).sum()}")
    if not loud.any():
        print("  silent")
        return
    # Segments of consecutive loud frames with a pitch within 3 % of each other.
    segs, cur = [], None
    for i, f in enumerate(frames):
        if not loud[i]:
            if cur:
                segs.append(cur)
                cur = None
            continue
        f0, _ = frame_pitch(f, rate)
        if cur and abs(f0 - cur["f"][-1]) / cur["f"][-1] < 0.03:
            cur["f"].append(f0)
            cur["end"] = i
        else:
            if cur:
                segs.append(cur)
            cur = {"start": i, "end": i, "f": [f0]}
    if cur:
        segs.append(cur)
    print("  segment   start  len   pitch Hz  spread  rms dBFS  2nd dB  3rd dB")
    for s in segs:
        a, b = s["start"] * hop, (s["end"] + 1) * hop
        seg = m[a:b]
        if len(seg) < hop * 2:
            continue
        f0, spec = frame_pitch(seg, rate)
        h1 = harmonic(spec, rate, f0, 1)
        h2, h3 = harmonic(spec, rate, f0, 2), harmonic(spec, rate, f0, 3)
        db = lambda h: f"{20 * np.log10(h / h1):6.1f}" if h is not None else "   n/a"
        spread = (max(s["f"]) - min(s["f"])) / f0 * 100
        level = 20 * np.log10(np.sqrt(np.mean(seg ** 2)) + 1e-9)
        print(f"  {'':8s} {a / rate * 1000:6.0f} {(b - a) / rate * 1000:4.0f}   {f0:8.1f}  {spread:5.1f} %  "
              f"{level:7.1f}  {db(h2)}  {db(h3)}")
    # Pops: 1 ms spikes where no tone plays (more than 8x the noise floor, with quiet on both sides).
    h1 = rate // 1000
    pk = np.array([np.abs(m[i:i + h1]).max() for i in range(0, len(m) - h1, h1)])
    tone = np.zeros(len(pk), bool)
    rp = np.repeat(loud, 10)[:len(pk)]
    tone[:len(rp)] = rp
    pops = [i for i in range(3, len(pk) - 3) if not tone[max(0, i - 5):i + 6].any() and pk[i] > 8 * max(floor, 1e-5) * 1.4]
    # Splatter: broadband energy in 14-20 kHz (no harmonic of these sounds falls there) in 2 ms
    # windows, against its median over the loud part. Only meaningful at 48 kHz.
    splat = []
    if rate >= 44100:
        h2 = rate // 500
        ws = [m[i:i + h2] for i in range(0, len(m) - h2, h2)]
        fr = np.fft.rfftfreq(h2, 1 / rate)
        band = (fr > 14000) & (fr < 20000)
        hb = np.array([np.sqrt(np.sum(np.abs(np.fft.rfft(w * np.hanning(h2)))[band] ** 2)) for w in ws])
        loud2 = np.zeros(len(hb), bool)
        rep = np.repeat(loud, 5)[:len(hb)]
        loud2[:len(rep)] = rep
        if loud2.any():
            ref = np.median(hb[loud2]) + 1e-9
            splat = [i * 2 for i in range(len(hb)) if hb[i] > 10 * ref and hb[i] > 1e-4]
    print(f"  pops: {len(pops)}" + (f" ({', '.join(f'{t} ms' for t in pops[:8])})" if pops else ""))
    print(f"  splatter (14-20 kHz bursts): " + ("n/a at this rate" if rate < 44100 else
          f"{len(splat)}" + (f" ({', '.join(f'{t} ms' for t in splat[:8])})" if splat else "")))
    mask = np.zeros(len(m), bool)
    mask[:len(loud) * hop] = np.repeat(loud, hop)
    right = x[:, 1] - x[:, 1].mean()
    print(f"  loud part rms: left {20 * np.log10(np.sqrt(np.mean(m[mask] ** 2)) + 1e-9):.1f} dBFS, "
          f"right {20 * np.log10(np.sqrt(np.mean(right[mask] ** 2)) + 1e-9):.1f} dBFS")

for p in sys.argv[1:]:
    analyse(p)
