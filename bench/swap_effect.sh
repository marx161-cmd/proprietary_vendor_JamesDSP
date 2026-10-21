#!/usr/bin/env bash
# Bind-mount a libjamesdspaidl.so variant over the vendor one (RAM only, survives
# until reboot or `swap_effect.sh restore`), restart the audio HAL so it loads it,
# and verify the sha the HAL process actually sees.
#   swap_effect.sh <path/to/libjamesdspaidl.so>   |   swap_effect.sh restore
set -eo pipefail
DEV="${JDSP_BENCH_DEVICE:?set JDSP_BENCH_DEVICE to the phone adb address, e.g. 100.x.y.z:5555}"
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
# Optional: libraries the variant needs that the firmware no longer ships (the
# stock crDroid prebuilt links android.hardware.audio.effect-V2-ndk.so and
# android.media.audio.common.types-V3-ndk.so; the device has V3/V4 only).
# /odm/lib64 is the first search path of the vendor linker namespace and on
# this device /odm holds only dangling symlinks into a non-existent
# /vendor/odm, so a directory bind-mounted over /odm is harmless and
# reversible. JDSP_ODM_LIBS="path1 path2" (on-device paths) enables it; a swap
# without it (or `restore`) unmounts it again.
ODM=/data/local/tmp/jdsp_variants/odm
$ADB shell "su -c 'nsenter -t 1 -m -- umount /odm 2>/dev/null; true'" >/dev/null 2>&1
if [[ -n "${JDSP_ODM_LIBS:-}" && "$1" != "restore" ]]; then
  $ADB shell "su -c 'rm -rf $ODM; mkdir -p $ODM/lib64; for f in $JDSP_ODM_LIBS; do cp \$f $ODM/lib64/; done; chmod 755 $ODM $ODM/lib64; chmod 644 $ODM/lib64/*; chcon -R u:object_r:vendor_file:s0 $ODM; nsenter -t 1 -m -- mount --bind $ODM /odm && echo odm-overlay: \$(ls $ODM/lib64 | tr "\n" " ")'" | tr -d '\r'
fi
old=$($ADB shell "pidof $HAL" | tr -d '\r')
$ADB shell "su -c 'kill $old'" >/dev/null
for i in $(seq 1 20); do sleep 0.5; new=$($ADB shell "pidof $HAL" | tr -d '\r'); [[ -n "$new" && "$new" != "$old" ]] && break; done
echo "HAL restarted: pid $old -> $new"
$ADB shell "su -c 'sha256sum /proc/$new/root$TARGET'" | tr -d '\r'
