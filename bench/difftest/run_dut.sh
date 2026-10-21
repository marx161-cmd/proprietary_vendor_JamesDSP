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
for m in gate comp limiter; do echo "== $m ($TAG vs real LSP)"; python3 compare.py "ref_$m.wav" "dut_${TAG}_$m.wav"; done
