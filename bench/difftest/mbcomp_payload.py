#!/usr/bin/env python3
"""Single source of truth for the mbcomp difftest settings (LSP Multiband
Compressor stereo x8 port, AIDL cmd 10106, layout in lsp_wrap_mbcomp.h).
  mbcomp_payload.py dut    classic|modern|lp -> the 218 payload floats, one per line
  mbcomp_payload.py ladspa classic|modern|lp -> LADSPA 'port name=value' args, one per line
Both views are generated from the same settings dict so the reference and the
DUT can never drift apart. Settings: Classic, Modern or Linear Phase mode, the
default splits 2/4/6 enabled but split 4 moved 632 -> 800 Hz, four active
bands (0, 2, 4, 6) with distinct thresholds/ratios/timings so every band is
compressing, everything else at the real plugin defaults.
"""
import sys
NB, NS = 8, 7
FE = [10, 40, 100, 252, 632, 1587, 3984, 10000]
FS = [40, 100, 252, 632, 1587, 3984, 10000, 20000]
MINUS_INF = -150.0                      # LSP_MBCOMP_DB_MINUS_INF
def lin(db): return 10 ** (db / 20)
def lin0(db): return 0.0 if db <= MINUS_INF else lin(db)

def settings(variant):
    g = dict(mode={'classic': 0, 'modern': 1, 'lp': 2}[variant], g_in=0.0, g_out=0.0, g_dry=MINUS_INF, g_wet=0.0,
             drywet=100.0, react=0.2, shift=0.0, envb=1, ssplit=0)
    splits = [(0, 40.0), (1, 100.0), (0, 252.0), (1, 800.0), (0, 1587.0), (1, 3984.0), (0, 10000.0)]  # split 4: 632 -> 800 Hz
    bands = []
    for b in range(NB):
        bands.append(dict(ce=1, bs=0, bm=0, cm=0, scm=1, scs=0, sscs=0, sla=0.0, scr=10.0, scp=0.0, sclc=0, schc=0,
                          sclf=float(FE[b]), schf=float(FS[b]), al=-12.0, at=20.0, rrl=MINUS_INF, rt=100.0, cr=1.0,
                          kn=-6.0, mk=0.0, bth=-72.0, bsa=6.0, ht=0.0))
    over = {0: dict(al=-30.0, cr=4.0, at=10.0, rt=150.0),
            2: dict(al=-28.0, cr=3.0, at=15.0, rt=120.0),
            4: dict(al=-26.0, cr=6.0, at=5.0, rt=200.0),
            6: dict(al=-32.0, cr=2.5, at=8.0, rt=100.0)}
    for b, o in over.items(): bands[b].update(o)
    return g, splits, bands

BAND_ORDER = ['ce', 'bs', 'bm', 'cm', 'scm', 'scs', 'sscs', 'sla', 'scr', 'scp', 'sclc', 'schc', 'sclf', 'schf',
              'al', 'at', 'rrl', 'rt', 'cr', 'kn', 'mk', 'bth', 'bsa', 'ht']

def dut(variant):
    g, splits, bands = settings(variant)
    v = [0, 1, g['mode'], g['g_in'], g['g_out'], g['g_dry'], g['g_wet'], g['drywet'], g['react'], g['shift'], g['envb'], g['ssplit']]
    for en, f in splits: v += [en, f]
    for d in bands: v += [d[k] for k in BAND_ORDER]
    assert len(v) == 218, len(v)
    return [repr(float(x)) for x in v]

def ladspa(variant):
    g, splits, bands = settings(variant)
    a = ['Show pre-mix overlay=0', 'Input to Link mix (G)=0', 'Link to Input mix (G)=0', 'Link to Sidechain mix (G)=0',
         'Bypass=0', f"Compressor mode={g['mode']}", f"Input gain (G)={lin0(g['g_in'])}", f"Output gain (G)={lin0(g['g_out'])}",
         f"Dry gain (G)={lin0(g['g_dry'])}", f"Wet gain (G)={lin0(g['g_wet'])}", f"Dry/Wet balance (%)={g['drywet']}",
         f"FFT reactivity (ms)={g['react']}", f"Shift gain (G)={lin0(g['shift'])}", 'Graph zoom (G)=1',
         f"Envelope boost={g['envb']}", 'Band selection=0', 'Band filter curves=1', f"Stereo split={g['ssplit']}",
         'Input FFT graph enable Left=0', 'Output FFT graph enable Left=0', 'Input FFT graph enable Right=0', 'Output FFT graph enable Right=0']
    for i, (en, f) in enumerate(splits):
        a += [f'Compression band enable {i+1}={en}', f'Split frequency {i+1} (Hz)={f}']
    for b, d in enumerate(bands):
        a += [f'External sidechain source {b}=0', f"Sidechain source {b}={d['scs']}", f"Split sidechain source {b}={d['sscs']}",
              f"Sidechain mode {b}={d['scm']}", f"Sidechain lookahead {b} (ms)={d['sla']}", f"Sidechain reactivity {b} (ms)={d['scr']}",
              f"Sidechain preamp {b} (G)={lin0(d['scp'])}", f"Sidechain custom lo-cut {b}={d['sclc']}", f"Sidechain custom hi-cut {b}={d['schc']}",
              f"Sidechain lo-cut frequency {b} (Hz)={d['sclf']}", f"Sidechain hi-cut frequency {b} (Hz)={d['schf']}",
              f"Compression mode {b}={d['cm']}", f"Compressor enable {b}={d['ce']}", f"Solo band {b}={d['bs']}", f"Mute band {b}={d['bm']}",
              f"Attack threshold {b} (G)={lin(d['al'])}", f"Attack time {b} (ms)={d['at']}", f"Release threshold {b} (G)={lin0(d['rrl'])}",
              f"Release time {b} (ms)={d['rt']}", f"Hold time {b} (ms)={d['ht']}", f"Ratio {b}={d['cr']}", f"Knee {b} (G)={lin(d['kn'])}",
              f"Boost threshold {b} (G)={lin(d['bth'])}", f"Boost signal amount {b} (G)={lin(d['bsa'])}", f"Makeup gain {b} (G)={lin(d['mk'])}",
              f"Hue  {b}=0"]
    return a

if __name__ == "__main__":
    view, variant = sys.argv[1], sys.argv[2]
    print("\n".join(dut(variant) if view == "dut" else ladspa(variant)))
