#!/usr/bin/env python3
"""Build the "Defaults (all modules)" JamesDSP preset (.tar for the app's preset library).

Values: real LSP plugin defaults for the ported modules (gate, compander, expander,
autogain), JamesDSP card defaults for everything else; every module off except the
output limiter (the clip guard, default on). Push the result to
/sdcard/Android/data/com.termux.jdsp/files/Presets/ and load it from the app.
Format = RootlessJamesDSP Preset.kt v3: flat ustar tar of dsp_*.xml + a `metadata` entry.
"""
import io, os, sys, tarfile, time
HDR = "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n<map>\n"
def xml(items):
    out = HDR
    for k, v in items:
        if isinstance(v, bool): out += f'    <boolean name="{k}" value="{"true" if v else "false"}" />\n'
        elif isinstance(v, (int, float)): out += f'    <float name="{k}" value="{float(v)}" />\n'
        else: out += f'    <string name="{k}">{v}</string>\n'
    return out + "</map>\n"
FILES = {
 'dsp_autogain.xml': [('autogain_enable', False), ('autogain_level', -23), ('autogain_long_period', 400), ('autogain_short_period', 20), ('autogain_drift', 12), ('autogain_silence', -72), ('autogain_max_gain_on', False), ('autogain_max_gain', 36)],
 'dsp_bass.xml': [('bass_enable', False), ('bass_max_gain', 5)],
 'dsp_compander.xml': [('compander_enable', False), ('compander_threshold', -12), ('compander_ratio', 4), ('compander_knee', 0.501), ('compander_attack', 20), ('compander_release', 100), ('compander_hold', 0)],
 'dsp_convolver.xml': [('convolver_enable', False), ('convolver_file', ''), ('convolver_mode', '0'), ('convolver_adv_imp', '-80;-100;0;0;0;0')],
 'dsp_crossfeed.xml': [('bs2b_crossfeed_enable', False), ('bs2b_crossfeed_mode', '5')],
 'dsp_ddc.xml': [('ddc_enable', False), ('ddc_file', '')],
 'dsp_equalizer.xml': [('eq_enable', False), ('eq_filter_type', '0'), ('eq_interpolation', '0'), ('eq_bands', '25.0;40.0;63.0;100.0;160.0;250.0;400.0;630.0;1000.0;1600.0;2500.0;4000.0;6300.0;10000.0;16000.0;0.0;0.0;0.0;0.0;0.0;0.0;0.0;0.0;0.0;0.0;0.0;0.0;0.0;0.0;0.0')],
 'dsp_expander.xml': [('expander_enable', False), ('expander_mode', '1'), ('expander_threshold', -12), ('expander_ratio', 4), ('expander_knee', 0.501), ('expander_attack', 20), ('expander_release', 100), ('expander_hold', 0)],
 'dsp_gate.xml': [('gate_enable', False), ('gate_open_threshold', -24), ('gate_close_threshold', -36), ('gate_attack', 20), ('gate_release', 100), ('gate_hold', 0), ('gate_reduction', -24)],
 'dsp_graphiceq.xml': [('geq_enable', False), ('geq_nodes', 'GraphicEQ: ')],
 'dsp_liveprog.xml': [('liveprog_enable', False), ('liveprog_file', ''), ('lua_script', '')],
 'dsp_output_control.xml': [('limiter_enable', True), ('limiter_threshold', -0.1), ('limiter_attack', 5), ('limiter_release', 5), ('limiter_knee', 1.0), ('output_postgain', 0)],
 'dsp_parametriceq.xml': [('peq_enable', False), ('peq_bands', 'PEQ: ')],
 'dsp_reverb.xml': [('reverb_enable', False), ('reverb_preset', '15')],
 'dsp_stereowide.xml': [('stereowide_enable', False), ('stereowide_mode', 60)],
 'dsp_tube.xml': [('tube_enable', False), ('tube_drive', 2)],
}
META = "version=3\napp_version=1.6.15\napp_flavor=rootFdroid\nliveprog_included=false\nmin_version_code=26"
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), 'Defaults (all modules).tar')
with tarfile.open(out, 'w', format=tarfile.USTAR_FORMAT) as tf:
    for n, data in list((n, xml(v)) for n, v in FILES.items()) + [('metadata', META)]:
        b = data.encode(); ti = tarfile.TarInfo(n); ti.size = len(b); ti.mtime = int(time.time()); ti.mode = 0o644
        tf.addfile(ti, io.BytesIO(b))
print(out)
