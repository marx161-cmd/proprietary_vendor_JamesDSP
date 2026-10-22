#!/usr/bin/env python3
"""Deterministic 48 kHz stereo float32 test signals for the LSP differential test.
One file per module so each comparison isolates one algorithm:
  gate.wav     tone bursts (-12 dBFS) over a -55 dBFS noise floor; gate opens/closes
  comp.wav     pink noise, slow AM sweep -40 .. -6 dBFS; compressor rides the envelope
  limiter.wav  pink noise at -12 dBFS with 2 ms clicks to -1 dBFS; limiter catches peaks
  loudcomp.wav pink noise at -30 dBFS under a 20 Hz..20 kHz exponential sine sweep at
               -24 dBFS; the sweep walks the whole equal-loudness curve, peak ~-20 dBFS
               so even +10 dB of curve gain stays clear of the output limiter's ALR knee
  mbcomp.wav   pink noise at -30 dBFS plus three independently gated tone bursts (80 Hz
               -14 dBFS, 1 kHz -16 dBFS, 6 kHz -20 dBFS, incommensurate periods) so the
               multiband compressor's bands 1/2/3/4 (0..100, 100..800, 800..3984, >3984 Hz)
               each see their own level swings; peak ~-6.2 dBFS
Peaks stay < -6 dBFS in gate.wav/comp.wav so an always-on limiter at -0.09 dB never engages.
"""
import numpy as np, soundfile as sf, os
sr = 48000
rng = np.random.default_rng(161)

def pink(n):
    w = rng.standard_normal(n)
    b0=b1=b2=0.0; y=np.empty(n)
    for i in range(n):
        b0 = 0.99765*b0 + w[i]*0.0990460
        b1 = 0.96300*b1 + w[i]*0.2965164
        b2 = 0.57000*b2 + w[i]*1.0526913
        y[i] = b0+b1+b2+w[i]*0.1848
    return y/np.max(np.abs(y))

def db(x): return 10**(x/20)

def stereo(mono, width=0.9):
    return np.stack([mono, mono*width], axis=1).astype(np.float32)

out = os.path.dirname(os.path.abspath(__file__))

# gate: 8 s; 0.4 s bursts of 440 Hz at -12 dBFS every 1.0 s, floor noise at -55 dBFS
n = sr*8; t = np.arange(n)/sr
env = ((t % 1.0) < 0.4).astype(float)
env = np.convolve(env, np.ones(96)/96, mode='same')      # 2 ms edges
g = db(-12)*np.sin(2*np.pi*440*t)*env + db(-55)*rng.standard_normal(n)
sf.write(f"{out}/gate.wav", stereo(g), sr, subtype='FLOAT')

# comp: 10 s pink noise, AM triangle -40 .. -6 dBFS with 2.5 s period
n = sr*10; t = np.arange(n)/sr
tri = np.abs(((t/2.5) % 1.0)*2-1)                         # 0..1..0
lvl = -40 + 34*tri
c = pink(n)*db(lvl)
sf.write(f"{out}/comp.wav", stereo(c), sr, subtype='FLOAT')

# limiter: 8 s pink at -12 dBFS plus 2 ms clicks to -1 dBFS every 0.5 s
n = sr*8; t = np.arange(n)/sr
l = pink(n)*db(-12)
for k in range(1, 16):
    i0 = int(k*0.5*sr); seg = slice(i0, i0+96)
    l[seg] = db(-1)*np.sign(l[seg] + 1e-9)
sf.write(f"{out}/limiter.wav", stereo(l), sr, subtype='FLOAT')

# loudcomp: 10 s pink at -30 dBFS + exponential sweep 20 Hz -> 20 kHz at -24 dBFS
# (appended after the others so the rng stream, hence the files above, is unchanged)
n = sr*10; t = np.arange(n)/sr
f0, f1, T = 20.0, 20000.0, 10.0
phase = 2*np.pi*f0*T/np.log(f1/f0)*(np.exp(t/T*np.log(f1/f0)) - 1)
lc = pink(n)*db(-30) + db(-24)*np.sin(phase)
sf.write(f"{out}/loudcomp.wav", stereo(lc), sr, subtype='FLOAT')
# mbcomp: 10 s pink at -30 dBFS + gated 80 Hz / 1 kHz / 6 kHz bursts (appended after
# loudcomp so the rng stream, hence every file above, is unchanged)
n = sr*10; t = np.arange(n)/sr
def gate_env(period, on):
    e = ((t % period) < on).astype(float)
    return np.convolve(e, np.ones(96)/96, mode='same')      # 2 ms edges
mb = pink(n)*db(-30)
mb += db(-14)*np.sin(2*np.pi*80*t)*gate_env(1.0, 0.3)
mb += db(-16)*np.sin(2*np.pi*1000*t)*gate_env(0.7, 0.25)
mb += db(-20)*np.sin(2*np.pi*6000*t)*gate_env(0.45, 0.2)
sf.write(f"{out}/mbcomp.wav", stereo(mb), sr, subtype='FLOAT')
for f in ("gate","comp","limiter","loudcomp","mbcomp"):
    d, _ = sf.read(f"{out}/{f}.wav"); print(f, d.shape, "peak dBFS %.1f" % (20*np.log10(np.max(np.abs(d)))))
