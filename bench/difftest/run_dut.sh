#!/usr/bin/env bash
# Run the three differential cases on the phone against one libjamesdspaidl.so
# and compare each against the real-LSP reference. Payloads mirror make_reference.sh.
#   run_dut.sh <harness-binary> <libjamesdspaidl.so> <tag>
set -eo pipefail
cd "$(dirname "$0")"
H="$1"; SO="$2"; TAG="$3"
LIM_T=(--cmd 1500 -0.09 5 10 1 0)                       # output limiter at max threshold, knee slider 1.0 (ALR knee 0 dB)
./run_device.sh "$H" "$SO" gate.wav    "dut_${TAG}_gate.wav"    --cmd 10100 0 1 -20 -30 5 50 10 -40  "${LIM_T[@]}"
./run_device.sh "$H" "$SO" comp.wav    "dut_${TAG}_comp.wav"    --cmd 10101 0 1 -24 4 0.501187 5 100 10 "${LIM_T[@]}"
./run_device.sh "$H" "$SO" limiter.wav "dut_${TAG}_limiter.wav" --cmd 1500 -6 5 10 0.5 0
./run_device.sh "$H" "$SO" comp.wav    "dut_${TAG}_expander.wav" --cmd 10103 0 1 0 -20 2 0.5 20 100 0 "${LIM_T[@]}"
./run_device.sh "$H" "$SO" comp.wav    "dut_${TAG}_autogain.wav" --cmd 10104 0 1 -23 400 20 12 -72 36 0 "${LIM_T[@]}"
# loudcomp: [band, en, input gain dB, mode, std, fft idx, approx, volume phons, hclip, hcrange dB]
./run_device.sh "$H" "$SO" loudcomp.wav "dut_${TAG}_loudcomp.wav"     --cmd 10105 0 1 0 0 4 4 2 -30 0 6 "${LIM_T[@]}"
./run_device.sh "$H" "$SO" loudcomp.wav "dut_${TAG}_loudcomp_iir.wav" --cmd 10105 0 1 0 1 4 4 2 -30 0 6 "${LIM_T[@]}"
# mbcomp: 218 floats [band, en, 10 globals, 7 x (split en, freq), 8 x 24 band params] from mbcomp_payload.py
MBC_C=($(python3 mbcomp_payload.py dut classic)); MBC_LP=($(python3 mbcomp_payload.py dut lp))
./run_device.sh "$H" "$SO" mbcomp.wav "dut_${TAG}_mbcomp.wav"    --cmd 10106 "${MBC_C[@]}"  "${LIM_T[@]}"
./run_device.sh "$H" "$SO" mbcomp.wav "dut_${TAG}_mbcomp_lp.wav" --cmd 10106 "${MBC_LP[@]}" "${LIM_T[@]}"
for m in gate comp limiter expander autogain loudcomp loudcomp_iir mbcomp mbcomp_lp; do echo "== $m ($TAG vs real LSP)"; python3 compare.py "ref_$m.wav" "dut_${TAG}_$m.wav"; done
# Concurrency stress: random gate/compressor/limiter/loudcomp/mbcomp reconfigures ~1 kHz from a
# second thread for the whole run (not compared to a reference; pass = no crash, finite output).
echo "== stress ($TAG)"
./run_device.sh "$H" "$SO" comp.wav "dut_${TAG}_stress.wav" --stress --cmd 10100 0 1 -20 -30 5 50 10 -40 --cmd 10101 0 1 -24 4 0.5 5 100 10 --cmd 10105 0 1 0 0 4 4 2 -30 0 6 --cmd 10106 "${MBC_C[@]}" "${LIM_T[@]}" 2>&1 | grep -E "stress:|processed|Segmentation|Abort"
