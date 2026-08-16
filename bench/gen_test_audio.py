import numpy as np, struct, sys

sr = 48000

def log_sweep(dur, f0=20.0, f1=20000.0):
    n = int(sr*dur)
    t = np.arange(n)/sr
    k = np.log(f1/f0)/dur
    phase = 2*np.pi*f0*(np.exp(k*t)-1)/k
    return 0.5*np.sin(phase).astype(np.float32)

def pink_noise(dur):
    n = int(sr*dur)
    white = np.random.randn(n)
    b = [0.049922035,-0.095993537,0.050612699,-0.004408786]
    a = [1,-2.494956002,2.017265875,-0.522189400]
    # simple Voss-McCartney-ish fallback (no scipy dependency)
    x = white.copy()
    y = np.zeros(n)
    b0=b1=b2=0.0
    for i in range(n):
        b0 = 0.99765*b0 + white[i]*0.0990460
        b1 = 0.96300*b1 + white[i]*0.2965164
        b2 = 0.57000*b2 + white[i]*1.0526913
        y[i] = b0+b1+b2+white[i]*0.1848
    y = y/np.max(np.abs(y))*0.4
    return y.astype(np.float32)

sweep = log_sweep(6.0)
silence = np.zeros(int(sr*0.5), dtype=np.float32)
noise = pink_noise(4.0)
mono = np.concatenate([sweep, silence, noise])
left = mono
right = mono * 0.9  # slight L/R difference so stereo width isn't degenerate
interleaved = np.empty(len(left)*2, dtype=np.float32)
interleaved[0::2] = left
interleaved[1::2] = right

def write_wav_f32(path, sr, data):
    n = len(data)
    data_bytes = n*4
    with open(path, 'wb') as f:
        f.write(b'RIFF'); f.write(struct.pack('<I', 36+data_bytes)); f.write(b'WAVE')
        f.write(b'fmt '); f.write(struct.pack('<IHHIIHH', 16, 3, 2, sr, sr*2*4, 8, 32))
        f.write(b'data'); f.write(struct.pack('<I', data_bytes))
        f.write(data.tobytes())

write_wav_f32(sys.argv[1] if len(sys.argv)>1 else 'test_sweep.wav', sr, interleaved)
print("wrote", len(mono)/sr, "seconds")
