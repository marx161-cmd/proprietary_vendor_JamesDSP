#!/usr/bin/env bash
# Reference outputs from the REAL LSP plugins (Fedora lsp-plugins-ladspa) with
# exactly the settings the JamesDSP wrappers pin or expose. Every port that the
# port leaves at a fixed value is set explicitly here, so plugin-version default
# drift cannot leak in. Mirrors run_dut.sh's --cmd payloads one-to-one.
set -eo pipefail
cd "$(dirname "$0")"
R=./ladspa_run.py
lin() { python3 -c "print(10**($1/20))"; }

# Common: bypass off, unity trims, premix off
COMMON=('Bypass=0' 'Input gain (G)=1' 'Output gain (G)=1' 'Input to Link mix (G)=0' 'Link to Input mix (G)=0' 'Link to Sidechain mix (G)=0')
# Sidechain as pinned by lsp_wrap*.cpp: internal, RMS, Middle, 10 ms reactivity, 0 dB preamp, no lookahead, no filters
SC=('Stereo split=0' 'Split sidechain source=0' 'Sidechain mode=1' 'Sidechain lookahead (ms)=0' 'Sidechain listen=0' 'Sidechain source=0' 'Sidechain reactivity (ms)=10' 'Sidechain preamp (G)=1' 'High-pass filter mode=0' 'Low-pass filter mode=0')

# Always-on JamesDSP output limiter, pinned as transparent as it gets via cmd 1500 [-0.09 5 10 1 0] in run_dut.sh.
# ALR is ON (real plugin default, port since 2026-10-21) with knee slider 1.0 = ALR knee level 0 dB,
# so ALR starts easing at -0.09-9 = -9.1 dBFS: gate.wav (-11.8 dBFS peak) never reaches it, comp.wav
# (-8.5 dBFS) grazes it -- both sides of the comparison include the identical ALR stage.
ALR=('Automatic level regulation=1' 'Automatic level regulation attack time (ms)=5' 'Automatic level regulation release time (ms)=50')
LIM_TRANSPARENT=("${COMMON[@]}" 'Sidechain preamp (G)=1' "${ALR[@]}" 'Operating mode=0' "Threshold (G)=$(lin -0.09)" 'Knee level (G)=1' 'Gain boost=0' 'Lookahead (ms)=5' 'Attack time (ms)=5' 'Release time (ms)=10' 'Oversampling=21' 'Dithering=0' 'Sidechain type=0' 'Stereo linking (%)=100' 'Knee smooth (dB)=-5')

# --- gate: cmd 10100 [0 1 -20 -30 5 50 10 -40]  (open, close, attack, release, hold, reduction dB)
$R run gate_stereo gate.wav tmp_gate.wav "${COMMON[@]}" "${SC[@]}" 'Sidechain input=0' \
  'Hysteresis=1' "Curve threshold (G)=$(lin -20)" "Curve zone size (G)=$(lin -6)" "Hysteresis threshold (G)=$(lin -10)" "Hysteresis zone size (G)=$(lin -6)" \
  'Attack (ms)=5' 'Release (ms)=50' 'Hold time (ms)=10' "Reduction (G)=$(lin -40)" 'Makeup gain (G)=1' 'Dry gain (G)=0' 'Wet gain (G)=1' 'Dry/Wet balance (%)=100'
$R run limiter_stereo tmp_gate.wav ref_gate.wav "${LIM_TRANSPARENT[@]}"

# --- compressor: cmd 10101 [0 1 -24 4 0.501187 5 100 10]  (threshold dB, ratio, knee LINEAR, attack, release, hold)
$R run compressor_stereo comp.wav tmp_comp.wav "${COMMON[@]}" "${SC[@]}" 'Sidechain type=0' \
  'Compression mode=0' "Attack threshold (G)=$(lin -24)" 'Attack time (ms)=5' 'Release threshold (G)=0' 'Release time (ms)=100' 'Hold time (ms)=10' \
  'Ratio=4' 'Knee (G)=0.501187' "Boost threshold (G)=$(lin -72)" 'Makeup gain (G)=1' 'Dry gain (G)=0' 'Wet gain (G)=1' 'Dry/Wet balance (%)=100'
$R run limiter_stereo tmp_comp.wav ref_comp.wav "${LIM_TRANSPARENT[@]}"

# --- limiter: cmd 1500 [-6 5 10 0.5 0]  (threshold dB, attack, release, knee SLIDER 0..1 -> ALR knee level -12+12*k dB, postgain dB)
$R run limiter_stereo limiter.wav ref_limiter.wav "${COMMON[@]}" 'Sidechain preamp (G)=1' "${ALR[@]}" 'Operating mode=0' \
  "Threshold (G)=$(lin -6)" "Knee level (G)=$(lin -6)" 'Gain boost=0' 'Lookahead (ms)=5' 'Attack time (ms)=5' 'Release time (ms)=10' \
  'Oversampling=21' 'Dithering=0' 'Sidechain type=0' 'Stereo linking (%)=100' 'Knee smooth (dB)=-5'
# --- expander (downward): cmd 10103 [0 1 0 -20 2 0.5 20 100 0]
#     (band, enabled, mode 0=down, threshold dB, ratio, knee LINEAR, attack, release, hold)
$R run expander_stereo comp.wav tmp_exp.wav "${COMMON[@]}" "${SC[@]}" 'Sidechain input=0' \
  'Expander mode=0' "Attack threshold (G)=$(lin -20)" 'Attack time (ms)=20' 'Release threshold (G)=0' 'Release time (ms)=100' 'Hold time (ms)=0' \
  'Ratio=2' 'Knee (G)=0.5' 'Makeup gain (G)=1' 'Dry gain (G)=0' 'Wet gain (G)=1' 'Dry/Wet balance (%)=100'
$R run limiter_stereo tmp_exp.wav ref_expander.wav "${LIM_TRANSPARENT[@]}"
# --- autogain: cmd 10104 [0 1 -23 400 20 12 -72 36 0]
#     (band, enabled, target LUFS, long ms, short ms, drift dB, silence LUFS, max gain dB, max on)
#     everything else pinned to the real defaults: internal sidechain, 0 dB preamp, 0 lookahead,
#     K weighting (5), quick amp off, grow/fall amounts 12 dB (index 7), long 500/500 ms, short 40/10 ms
$R run autogain_stereo comp.wav tmp_ag.wav 'Bypass=0' 'Sidechain preamp (dB)=0' 'Sidechain lookahead (ms)=0' 'Sidechain mode=0' \
  'Loudness measuring long period (ms)=400' 'Loudness measuring short period (ms)=20' 'Weighting function=5' \
  'Desired loudness level (LUFS)=-23' 'Level drift (dB)=12' 'The level of silence (LUFS)=-72' \
  'Enable maximum amplification gain limitation=0' 'The maximum amplification gain (dB)=36' 'Enable quick amplifier=0' \
  'Long gain grow amount=7' 'Long gain grow time (ms)=500' 'Long gain fall amount=7' 'Long gain fall time (ms)=500' \
  'Short gain grow amount=7' 'Short gain grow time (ms)=40' 'Short gain fall amount=7' 'Short gain fall time (ms)=10'
$R run limiter_stereo tmp_ag.wav ref_autogain.wav "${LIM_TRANSPARENT[@]}"
rm -f tmp_gate.wav tmp_comp.wav tmp_exp.wav tmp_ag.wav
