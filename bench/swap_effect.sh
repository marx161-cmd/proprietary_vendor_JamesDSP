#!/usr/bin/env bash
# Bind-mount a libjamesdspaidl.so variant over the vendor one (RAM only, survives
# until reboot or `swap_effect.sh restore`), restart the audio HAL so it loads it,
# and verify the sha the HAL process actually sees.
#   swap_effect.sh <path/to/libjamesdspaidl.so>   |   swap_effect.sh restore
set -eo pipefail
DEV="${JDSP_BENCH_DEVICE:?set JDSP_BENCH_DEVICE to the phone's adb address, e.g. 100.x.y.z:5555}"
TARGET=/vendor/lib64/soundfx/libjamesdspaidl.so
HAL=android.hardware.audio.service-aidl.aoc
ADB="adb -s $DEV"
if [[ "$1" == "restore" ]]; then
  $ADB shell "su -c 'nsenter -t 1 -m -- umount $TARGET 2>/dev/null; echo unmounted'"
else
  SRC="$1"; NAME=$(basename "$(dirname "$SRC")")_$(basename "$SRC")
  $ADB push "$SRC" "/data/local/tmp/jdsp_variants/$NAME" >/dev/null
  $ADB shell "su -c 'chcon u:object_r:vendor_file:s0 /data/local/tmp/jdsp_variants/$NAME; chmod 644 /data/local/tmp/jdsp_variants/$NAME; nsenter -t 1 -m -- umount $TARGET 2>/dev/null; nsenter -t 1 -m -- mount --bind /data/local/tmp/jdsp_variants/$NAME $TARGET && echo bound'"
fi
old=$($ADB shell "pidof $HAL" | tr -d '\r')
$ADB shell "su -c 'kill $old'" >/dev/null
for i in $(seq 1 20); do sleep 0.5; new=$($ADB shell "pidof $HAL" | tr -d '\r'); [[ -n "$new" && "$new" != "$old" ]] && break; done
echo "HAL restarted: pid $old -> $new"
$ADB shell "su -c 'sha256sum /proc/$new/root$TARGET'" | tr -d '\r'
