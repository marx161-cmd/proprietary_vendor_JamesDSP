#!/usr/bin/env python3
"""Sample-level comparison of two stereo float WAVs (reference vs device-under-test).
Aligns by cross-correlation lag (DUT may carry fixed latency), skips the first
`--skip` seconds (bypass crossfades), then reports max |diff|, RMS diff relative to
the reference RMS (dB), and the worst 0.5 s window.
Usage: compare.py ref.wav dut.wav [--skip 0.25] [--maxlag 4800]
"""
import sys, argparse, numpy as np, soundfile as sf
ap = argparse.ArgumentParser(); ap.add_argument("ref"); ap.add_argument("dut")
ap.add_argument("--skip", type=float, default=0.25); ap.add_argument("--maxlag", type=int, default=4800)
a = ap.parse_args()
r, sr = sf.read(a.ref, dtype="float32"); d, sr2 = sf.read(a.dut, dtype="float32")
assert sr == sr2, "sample rates differ"
n = min(len(r), len(d)); r = r[:n]; d = d[:n]
# lag estimate on the mono sum via FFT cross-correlation
x = r.sum(1) - r.sum(1).mean(); y = d.sum(1) - d.sum(1).mean()
N = 1 << int(np.ceil(np.log2(2*n)))
cc = np.fft.irfft(np.fft.rfft(y, N) * np.conj(np.fft.rfft(x, N)), N)
lags = np.concatenate([np.arange(0, a.maxlag+1), np.arange(-a.maxlag, 0)])
cc = np.concatenate([cc[:a.maxlag+1], cc[-a.maxlag:]])
lag = int(lags[np.argmax(cc)])          # dut = ref delayed by `lag` samples
if lag >= 0: rr = r[:n-lag]; dd = d[lag:n]
else:        rr = r[-lag:n]; dd = d[:n+lag]
s0 = int(a.skip * sr); rr = rr[s0:]; dd = dd[s0:]
diff = dd - rr
ref_rms = np.sqrt(np.mean(rr**2)) + 1e-12
rms_rel = 20*np.log10(np.sqrt(np.mean(diff**2)) / ref_rms + 1e-12)
print(f"lag {lag} samples ({1000*lag/sr:.2f} ms)  compared {len(rr)/sr:.2f} s after skipping {a.skip} s")
print(f"max|diff| {np.max(np.abs(diff)):.3e}   rms(diff)/rms(ref) {rms_rel:+.1f} dB   ref peak {np.max(np.abs(rr)):.3f}")
w = int(0.5*sr); worst = (0, 0.0)
for i in range(0, len(diff)-w, w):
    m = float(np.max(np.abs(diff[i:i+w])))
    if m > worst[1]: worst = (i, m)
print(f"worst 0.5 s window starts at {a.skip + worst[0]/sr:.2f} s: max|diff| {worst[1]:.3e}")
