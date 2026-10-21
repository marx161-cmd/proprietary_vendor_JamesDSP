#!/usr/bin/env python3
"""Minimal offline LADSPA host (ctypes) for the LSP reference plugins.
  ladspa_run.py list  <plugin_label>
  ladspa_run.py run   <plugin_label> <in.wav> <out.wav> [--block N] [port=value ...]
Control ports are addressed by their LADSPA port name (exact string) or by a
case-insensitive prefix that matches exactly one port. Audio in/out are the
first two input / first two output audio ports (stereo plugins only).
"""
import sys, ctypes as C, numpy as np, soundfile as sf
SO = "/usr/lib64/ladspa/lsp-plugins-ladspa.so"
LADSPA_PORT_INPUT, LADSPA_PORT_OUTPUT, LADSPA_PORT_CONTROL, LADSPA_PORT_AUDIO = 1, 2, 4, 8
HINT_BOUNDED_BELOW, HINT_BOUNDED_ABOVE, HINT_TOGGLED, HINT_SAMPLE_RATE, HINT_LOGARITHMIC, HINT_INTEGER = 1, 2, 4, 8, 0x10, 0x20
HINT_DEFAULT_MASK = 0x3C0
class Hint(C.Structure): _fields_ = [("HintDescriptor", C.c_int), ("LowerBound", C.c_float), ("UpperBound", C.c_float)]
class Desc(C.Structure): pass
Handle = C.c_void_p
Desc._fields_ = [
    ("UniqueID", C.c_ulong), ("Label", C.c_char_p), ("Properties", C.c_int), ("Name", C.c_char_p),
    ("Maker", C.c_char_p), ("Copyright", C.c_char_p), ("PortCount", C.c_ulong),
    ("PortDescriptors", C.POINTER(C.c_int)), ("PortNames", C.POINTER(C.c_char_p)), ("PortRangeHints", C.POINTER(Hint)),
    ("ImplementationData", C.c_void_p),
    ("instantiate", C.CFUNCTYPE(Handle, C.POINTER(Desc), C.c_ulong)),
    ("connect_port", C.CFUNCTYPE(None, Handle, C.c_ulong, C.POINTER(C.c_float))),
    ("activate", C.CFUNCTYPE(None, Handle)), ("run", C.CFUNCTYPE(None, Handle, C.c_ulong)),
    ("run_adding", C.c_void_p), ("set_run_adding_gain", C.c_void_p),
    ("deactivate", C.CFUNCTYPE(None, Handle)), ("cleanup", C.CFUNCTYPE(None, Handle))]
lib = C.CDLL(SO); lib.ladspa_descriptor.restype = C.POINTER(Desc); lib.ladspa_descriptor.argtypes = [C.c_ulong]
def find(label):
    i = 0
    while True:
        d = lib.ladspa_descriptor(i)
        if not d: break
        if d.contents.Label.decode() == label or d.contents.Label.decode().endswith("/" + label): return d
        i += 1
    labels = []; i = 0
    while True:
        d = lib.ladspa_descriptor(i)
        if not d: break
        labels.append(d.contents.Label.decode()); i += 1
    sys.exit(f"plugin {label!r} not found; have: " + ", ".join(l for l in labels if "stereo" in l)[:2000])
def default_of(h, sr):
    kind = h.HintDescriptor & HINT_DEFAULT_MASK; lo, hi = h.LowerBound, h.UpperBound
    if h.HintDescriptor & HINT_SAMPLE_RATE: lo *= sr; hi *= sr
    log = h.HintDescriptor & HINT_LOGARITHMIC
    mid = (np.sqrt(lo*hi) if log and lo > 0 and hi > 0 else (lo+hi)/2)
    return {0x40: lo, 0x80: (np.exp(np.log(lo)*0.75+np.log(hi)*0.25) if log and lo>0 else lo*0.75+hi*0.25),
            0xC0: mid, 0x100: (np.exp(np.log(lo)*0.25+np.log(hi)*0.75) if log and lo>0 else lo*0.25+hi*0.75),
            0x140: hi, 0x200: 0.0, 0x240: 1.0, 0x280: 100.0, 0x2C0: 440.0}.get(kind, 0.0)
def main():
    mode, label = sys.argv[1], sys.argv[2]; d = find(label).contents; sr = 48000
    ports = [(i, d.PortNames[i].decode(), d.PortDescriptors[i], d.PortRangeHints[i]) for i in range(d.PortCount)]
    if mode == "list":
        print(d.Label.decode(), "|", d.Name.decode())
        for i, n, pd, h in ports:
            kind = ("ctl" if pd & LADSPA_PORT_CONTROL else "aud") + ("-in" if pd & LADSPA_PORT_INPUT else "-out")
            if pd & LADSPA_PORT_CONTROL and pd & LADSPA_PORT_INPUT:
                print(f"{i:3d} {kind} {n!r:40s} default={default_of(h, sr):<12.6g} [{h.LowerBound:g}..{h.UpperBound:g}]"
                      + (" toggle" if h.HintDescriptor & HINT_TOGGLED else "") + (" int" if h.HintDescriptor & HINT_INTEGER else ""))
            else: print(f"{i:3d} {kind} {n!r}")
        return
    inp, outp = sys.argv[3], sys.argv[4]; block = 4096; sets = {}
    args = sys.argv[5:]
    while args:
        a = args.pop(0)
        if a == "--block": block = int(args.pop(0)); continue
        k, v = a.split("=", 1); sets[k] = float(v)
    x, fsr = sf.read(inp, dtype="float32"); assert fsr == sr and x.ndim == 2 and x.shape[1] == 2
    n = len(x); y = np.zeros_like(x)
    inst = d.instantiate(C.pointer(d), sr); assert inst
    ctl = {}; ain = []; aout = []
    for i, nme, pd, h in ports:
        if pd & LADSPA_PORT_CONTROL:
            val = default_of(h, sr)
            if pd & LADSPA_PORT_INPUT:
                for k in list(sets):
                    if k == nme or (nme.lower().startswith(k.lower()) and sum(1 for _, m, p2, _ in ports if p2 & LADSPA_PORT_CONTROL and p2 & LADSPA_PORT_INPUT and m.lower().startswith(k.lower())) == 1):
                        val = sets.pop(k)
            buf = C.c_float(val); ctl[i] = (nme, buf); d.connect_port(inst, i, C.pointer(buf))
        else:
            (ain if pd & LADSPA_PORT_INPUT else aout).append(i)
    if sets: sys.exit(f"unmatched/ambiguous control names: {list(sets)}")
    assert len(ain) >= 2 and len(aout) >= 2, (ain, aout)
    bufs = [np.zeros(block, dtype=np.float32) for _ in range(4)]
    zeros = np.zeros(block, dtype=np.float32)
    for k, i in enumerate(ain): d.connect_port(inst, i, (bufs[k] if k < 2 else zeros).ctypes.data_as(C.POINTER(C.c_float)))
    for k, i in enumerate(aout): d.connect_port(inst, i, (bufs[2+k] if k < 2 else zeros).ctypes.data_as(C.POINTER(C.c_float)))
    d.activate(inst)
    for s in range(0, n, block):
        m = min(block, n - s)
        bufs[0][:m] = x[s:s+m, 0]; bufs[1][:m] = x[s:s+m, 1]
        d.run(inst, m)
        y[s:s+m, 0] = bufs[2][:m]; y[s:s+m, 1] = bufs[3][:m]
    d.deactivate(inst); d.cleanup(inst)
    sf.write(outp, y, sr, subtype="FLOAT")
    used = {nme: b.value for nme, b in ctl.values()}
    print(f"wrote {outp}: {n/sr:.2f} s, block {block}, peak {np.max(np.abs(y)):.4f}")
    print("controls:", {k: round(v, 6) for k, v in used.items() if k in sys.argv[5:] or any(a.split('=')[0].lower() in k.lower() for a in sys.argv[5:] if '=' in a)})
main()
