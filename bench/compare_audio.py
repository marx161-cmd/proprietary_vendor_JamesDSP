#!/usr/bin/env python3
"""Compare two float32 WAV files produced by the offline JDSP harness
(stock vs current), reporting sample-level and spectral differences.
Usage: compare_audio.py <a.wav> <b.wav> [--label-a stock] [--label-b current]
"""
import sys
import struct
import numpy as np


def read_wav_f32(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data[0:4] == b"RIFF" and data[8:12] == b"WAVE", f"{path}: not a RIFF/WAVE file"
    pos = 12
    fmt = None
    pcm = None
    while pos < len(data):
        cid = data[pos:pos + 4]
        (sz,) = struct.unpack_from("<I", data, pos + 4)
        body = data[pos + 8:pos + 8 + sz]
        if cid == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", body)
        elif cid == b"data":
            pcm = body
        pos += 8 + sz + (sz % 2)
    assert fmt is not None and pcm is not None, f"{path}: missing fmt/data chunk"
    audio_format, channels, sample_rate, _, _, bits = fmt
    if audio_format == 3 and bits == 32:
        arr = np.frombuffer(pcm, dtype=np.float32)
    elif audio_format == 1 and bits == 16:
        arr = np.frombuffer(pcm, dtype=np.int16).astype(np.float32) / 32768.0
    else:
        raise ValueError(f"{path}: unsupported format={audio_format} bits={bits}")
    return arr.reshape(-1, channels), sample_rate


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        print(__doc__)
        sys.exit(2)
    path_a, path_b = args[0], args[1]
    label_a = "A"
    label_b = "B"
    if "--label-a" in args:
        label_a = args[args.index("--label-a") + 1]
    if "--label-b" in args:
        label_b = args[args.index("--label-b") + 1]

    a, sr_a = read_wav_f32(path_a)
    b, sr_b = read_wav_f32(path_b)
    print(f"{label_a}: {path_a} — {a.shape[0]} frames, {a.shape[1]}ch, {sr_a}Hz")
    print(f"{label_b}: {path_b} — {b.shape[0]} frames, {b.shape[1]}ch, {sr_b}Hz")
    if sr_a != sr_b:
        print("WARNING: sample rates differ, comparison below is not meaningful")
    n = min(a.shape[0], b.shape[0])
    if a.shape[0] != b.shape[0]:
        print(f"WARNING: frame counts differ ({a.shape[0]} vs {b.shape[0]}), truncating to {n}")
    a, b = a[:n], b[:n]

    print()
    if np.array_equal(a, b):
        print("RESULT: bit-exact identical output.")
        return
    diff = a - b
    rms_a = np.sqrt(np.mean(a ** 2))
    rms_b = np.sqrt(np.mean(b ** 2))
    rms_diff = np.sqrt(np.mean(diff ** 2))
    peak_diff = np.max(np.abs(diff))
    print(f"RMS level:      {label_a}={rms_a:.6f}  {label_b}={rms_b:.6f}  "
          f"delta={20*np.log10(max(rms_b,1e-12)/max(rms_a,1e-12)):+.3f} dB")
    print(f"RMS difference: {rms_diff:.6f} "
          f"({20*np.log10(max(rms_diff,1e-12)/max(rms_a,1e-12)):+.2f} dB relative to {label_a} RMS)")
    print(f"Peak difference: {peak_diff:.6f}")

    # Spectral comparison (average magnitude spectrum, mono-summed)
    mono_a = a.mean(axis=1)
    mono_b = b.mean(axis=1)
    n_fft = 1 << (int(np.log2(min(len(mono_a), 65536))))
    window = np.hanning(n_fft)
    hop = n_fft // 2
    def avg_spectrum(x):
        specs = []
        for start in range(0, len(x) - n_fft, hop):
            seg = x[start:start + n_fft] * window
            specs.append(np.abs(np.fft.rfft(seg)))
        return np.mean(specs, axis=0) if specs else np.zeros(n_fft // 2 + 1)
    spec_a = avg_spectrum(mono_a)
    spec_b = avg_spectrum(mono_b)
    freqs = np.fft.rfftfreq(n_fft, d=1.0 / sr_a)
    eps = 1e-9
    db_a = 20 * np.log10(spec_a + eps)
    db_b = 20 * np.log10(spec_b + eps)
    db_delta = db_b - db_a

    bands = [(20, 60), (60, 250), (250, 2000), (2000, 6000), (6000, 20000)]
    print()
    print(f"Average spectral delta ({label_b} - {label_a}), by band:")
    for lo, hi in bands:
        mask = (freqs >= lo) & (freqs < hi)
        if mask.any():
            print(f"  {lo:>6}-{hi:<6} Hz: {np.mean(db_delta[mask]):+.2f} dB "
                  f"(max |delta|={np.max(np.abs(db_delta[mask])):.2f} dB)")

    max_idx = np.argmax(np.abs(db_delta))
    print(f"\nLargest single spectral delta: {db_delta[max_idx]:+.2f} dB at {freqs[max_idx]:.0f} Hz")


if __name__ == "__main__":
    main()
