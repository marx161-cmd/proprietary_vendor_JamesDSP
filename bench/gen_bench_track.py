#!/usr/bin/env python3
"""75 s 48 kHz stereo 16-bit 'music-like' bench track: pink noise bed with slow
dynamics, a chord of sines and periodic transients. Deterministic. Level -14 dBFS
RMS-ish so the always-on limiter is mostly idle, convolver/EQ/DDC run on real content."""
import numpy as np, soundfile as sf, os
sr=48000; n=sr*75; t=np.arange(n)/sr; rng=np.random.default_rng(7)
w=rng.standard_normal(n); y=np.zeros(n); b0=b1=b2=0.0
for i in range(n):
    b0=0.99765*b0+w[i]*0.0990460; b1=0.96300*b1+w[i]*0.2965164; b2=0.57000*b2+w[i]*1.0526913
    y[i]=b0+b1+b2+w[i]*0.1848
y/=np.max(np.abs(y))
chord=sum(np.sin(2*np.pi*f*t) for f in (110,164.8,220,329.6))/4
am=0.6+0.4*np.sin(2*np.pi*t/6.0)
clicks=np.zeros(n); clicks[(np.arange(0,n,sr//2))]=1.0
sig=(0.5*y+0.35*chord)*am+0.3*np.convolve(clicks,np.exp(-np.arange(2400)/400.0),mode='same')
sig*=10**(-14/20)/np.sqrt(np.mean(sig**2)); sig=np.clip(sig,-0.98,0.98)
st=np.stack([sig,np.roll(sig,37)*0.95],axis=1).astype(np.float32)
out=os.path.join(os.path.dirname(os.path.abspath(__file__)),"bench_track.wav")
sf.write(out,st,sr,subtype='PCM_16'); print(out, "rms dBFS %.1f peak %.2f"%(20*np.log10(np.sqrt(np.mean(st**2))),np.max(np.abs(st))))
