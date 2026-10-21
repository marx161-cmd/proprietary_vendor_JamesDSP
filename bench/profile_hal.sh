#!/usr/bin/env bash
# Sample the audio HAL process (where the AIDL effect runs) with simpleperf while
# the phone plays; report the hottest symbols of libjamesdspaidl.so.
#   profile_hal.sh <tag> [seconds=30]
# Needs JDSP_BENCH_DEVICE and JDSP_BENCH_SSH (see run_variant_bench.sh); the .so
# currently loaded must be the one built from this tree (symbols come from
# out/target/product/blazer/symbols, i.e. the last `m libjamesdspaidl`).
set -eo pipefail
cd "$(dirname "$0")"
TAG="$1"; SECS="${2:-30}"
DEV="${JDSP_BENCH_DEVICE:?set JDSP_BENCH_DEVICE to the phone adb address}"; ADB="adb -s $DEV"
SSH="ssh -o ConnectTimeout=8 ${JDSP_BENCH_SSH:?set JDSP_BENCH_SSH to the Termux ssh target}"
NDK=~/Android/Sdk/ndk/27.0.12077973
SYMFS=~/homelab/crDroid/out/target/product/blazer/symbols
mkdir -p results
$SSH "termux-media-player stop >/dev/null 2>&1; termux-media-player play ~/bench_track.wav" | tr -d '\r'
sleep 5
$ADB shell "su -c 'simpleperf record -p \$(pidof android.hardware.audio.service-aidl.aoc) -f 2000 -g --duration $SECS -o /data/local/tmp/jdsp_variants/perf-$TAG.data'" 2>&1 | tail -2
$SSH "termux-media-player stop" >/dev/null 2>&1 || true
$ADB pull "/data/local/tmp/jdsp_variants/perf-$TAG.data" "results/perf-$TAG.data" >/dev/null
python3 "$NDK/simpleperf/report.py" -i "results/perf-$TAG.data" --symfs "$SYMFS" --sort dso,symbol -n 2>/dev/null | head -45 | tee "results/perf-$TAG.txt"
