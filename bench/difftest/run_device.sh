#!/usr/bin/env bash
# Run the offline AIDL harness on the phone against one libjamesdspaidl.so.
#   run_device.sh <harness-binary> <libjamesdspaidl.so> <in.wav> <out.wav> [--cmd ID v...]...
# Files go to /data/local/tmp/jdsp_difftest; the three older AIDL interface
# libs stock needs are reused from the 2026-08-16 jdsp_bench dir on-device.
set -eo pipefail
DEV="${JDSP_BENCH_DEVICE:?set JDSP_BENCH_DEVICE to the phone adb address, e.g. 100.x.y.z:5555}"
HARNESS="$1"; SO="$2"; IN="$3"; OUT="$4"; shift 4
D=/data/local/tmp/jdsp_difftest
adb -s "$DEV" shell "mkdir -p $D" >/dev/null
adb -s "$DEV" push "$HARNESS" "$D/harness" >/dev/null
adb -s "$DEV" push "$SO" "$D/effect.so" >/dev/null
adb -s "$DEV" push "$IN" "$D/in.wav" >/dev/null
adb -s "$DEV" shell "cd $D && rm -f out.wav && chmod 755 harness && LD_LIBRARY_PATH=/data/local/tmp/jdsp_bench ./harness ./effect.so in.wav out.wav $*" || echo "(harness exit nonzero -- teardown crash is known, output was written before STOP)"
adb -s "$DEV" pull "$D/out.wav" "$OUT" >/dev/null
