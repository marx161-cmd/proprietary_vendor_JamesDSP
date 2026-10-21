#!/usr/bin/env bash
# One full on-device benchmark run for one .so variant:
#   run_variant_bench.sh <tag> <path/to/libjamesdspaidl.so> [seconds=60]
# swap .so (bind-mount + HAL restart) -> media volume 0 -> play bench track via
# Termux:API -> sample HAL CPU + battery -> stop -> pull CSV to bench/results/<tag>.csv
set -eo pipefail
cd "$(dirname "$0")"
TAG="$1"; SO="$2"; SECS="${3:-60}"
DEV="${JDSP_BENCH_DEVICE:?set JDSP_BENCH_DEVICE to the phone's adb address, e.g. 100.x.y.z:5555}"; ADB="adb -s $DEV"
SSH="ssh -o ConnectTimeout=8 ${JDSP_BENCH_SSH:?set JDSP_BENCH_SSH to the Termux ssh target, e.g. -p 8022 system@100.x.y.z}"
mkdir -p results
./swap_effect.sh "$SO"
$ADB shell "cmd media_session volume --stream 3 --set 0" >/dev/null 2>&1 || $ADB shell "media volume --stream 3 --set 0" >/dev/null 2>&1 || true
$SSH "termux-media-player stop >/dev/null 2>&1; termux-media-player play ~/bench_track.wav" | tr -d '\r'
sleep 8
$ADB shell "su -c 'sh /data/local/tmp/jdsp_variants/sample_hal.sh $SECS'" | tr -d '\r' > "results/$TAG.csv"
$SSH "termux-media-player stop" >/dev/null 2>&1 || true
python3 summarize.py "results/$TAG.csv"
