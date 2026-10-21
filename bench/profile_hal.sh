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
# Play through VLC like the bench does: Termux players never announce an audio
# session, so the JamesDSP effect does not attach to them.
VLC_ACT=com.termux.vlc/org.videolan.vlc.StartActivity
PREV_VOL=$($SSH 'termux-volume' | python3 -c 'import sys,json; print([s["volume"] for s in json.load(sys.stdin) if s["stream"]=="music"][0])')
$SSH "termux-volume music 0" >/dev/null 2>&1
$ADB shell "am force-stop com.termux.vlc" >/dev/null 2>&1
$ADB shell "am start -W -a android.intent.action.VIEW -d file:///sdcard/Music/bench_track.wav -t audio/x-wav -n $VLC_ACT" >/dev/null 2>&1
for i in $(seq 1 30); do
  sleep 1
  AF=$($ADB shell "dumpsys media.audio_flinger" 2>/dev/null | tr -d '\r')
  [[ $(echo "$AF" | grep -c -E "JamesDSP|f27317f4" || true) -gt 0 && $(echo "$AF" | grep -c 'Standby: no' || true) -gt 0 ]] && break
done
echo "effect lines $(echo "$AF" | grep -c -E "JamesDSP|f27317f4" || true), active threads $(echo "$AF" | grep -c 'Standby: no' || true)"
sleep 3
$ADB shell "su -c 'simpleperf record -p \$(pidof android.hardware.audio.service-aidl.aoc) -f 2000 -g --duration $SECS -o /data/local/tmp/jdsp_variants/perf-$TAG.data'" 2>&1 | tail -2
$ADB shell "am force-stop com.termux.vlc" >/dev/null 2>&1
[[ -n "$PREV_VOL" ]] && $SSH "termux-volume music $PREV_VOL" >/dev/null 2>&1
$ADB pull "/data/local/tmp/jdsp_variants/perf-$TAG.data" "results/perf-$TAG.data" >/dev/null
python3 "$NDK/simpleperf/report.py" -i "results/perf-$TAG.data" --symfs "$SYMFS" --sort dso,symbol -n 2>/dev/null | head -45 | tee "results/perf-$TAG.txt"
