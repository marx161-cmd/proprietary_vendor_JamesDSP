#!/usr/bin/env bash
# One full on-device benchmark run for one .so variant:
#   run_variant_bench.sh <tag> <path/to/libjamesdspaidl.so> [seconds=60]
# swap .so (bind-mount + HAL restart) -> media volume 0 -> play bench track via
# Termux:API -> sample HAL CPU + battery -> stop -> pull CSV to bench/results/<tag>.csv
set -eo pipefail
cd "$(dirname "$0")"
TAG="$1"; SO="$2"; SECS="${3:-60}"
DEV="${JDSP_BENCH_DEVICE:?set JDSP_BENCH_DEVICE to the phone adb address, e.g. 100.x.y.z:5555}"
ADB="adb -s $DEV"
SSH="ssh -o ConnectTimeout=8 ${JDSP_BENCH_SSH:?set JDSP_BENCH_SSH to the Termux ssh target, e.g. -p 8022 user@100.x.y.z}"
mkdir -p results
$ADB push sample_hal.sh /data/local/tmp/jdsp_variants/sample_hal.sh >/dev/null
./swap_effect.sh "$SO"
PREV_VOL=$($SSH 'termux-volume' | python3 -c 'import sys,json; print([s["volume"] for s in json.load(sys.stdin) if s["stream"]=="music"][0])')
$SSH "termux-volume music 0" >/dev/null 2>&1
echo "media volume ${PREV_VOL:-?} -> $($SSH 'termux-volume' | python3 -c 'import sys,json; print([s["volume"] for s in json.load(sys.stdin) if s["stream"]=="music"][0])')"
# Killing the audio HAL takes audioserver down with it, which orphans the root
# JamesDSP service's effect handles and every app's AudioTrack. Restart both the
# JamesDSP app (its root service re-registers) and the player (a real media app
# that announces OPEN_AUDIO_EFFECT_CONTROL_SESSION -- VLC does; Termux players
# and ffplay do not, so the effect never attaches to them). Then refuse to
# sample unless audio_flinger really shows the JamesDSP effect.
JDSP_ACT=com.termux.jdsp/me.timschneeberger.rootlessjamesdsp.activity.MainActivity
VLC_ACT=com.termux.vlc/org.videolan.vlc.StartActivity
# audioserver dies with the HAL and needs a few seconds; the JamesDSP app's
# root service checks for the effect at launch ("Plugin not installed" if too early).
for i in $(seq 1 30); do
  if $ADB shell "dumpsys media.audio_flinger" 2>/dev/null | grep -q "Output thread"; then break; fi; sleep 1
done
sleep 6
$ADB shell "am force-stop com.termux.vlc; am force-stop com.termux.jdsp" >/dev/null 2>&1
# The streaming client (Diana / Moonlight fork) keeps a dead audioserver
# connection after the HAL restart ("Cannot create AudioTrack" on the next
# stream start, seen 2026-10-22); stop it so it comes back fresh when opened.
$ADB shell "am force-stop com.termux.diana.root.noir" >/dev/null 2>&1
$ADB shell "am start -W -n $JDSP_ACT" >/dev/null 2>&1
sleep 4
$ADB shell "am start -W -a android.intent.action.VIEW -d file:///sdcard/Music/bench_track.wav -t audio/x-wav -n $VLC_ACT" >/dev/null 2>&1
vlc_state() { $ADB shell 'dumpsys media_session' 2>/dev/null | tr -d '\r' | grep -A14 'package=com.termux.vlc' | grep -oE 'state=[A-Z]+' | head -1; }
for i in $(seq 1 90); do
  sleep 1
  AF=$($ADB shell "dumpsys media.audio_flinger" 2>/dev/null | tr -d '\r')
  M=$(echo "$AF" | grep -c -E "JamesDSP|f27317f4" || true)
  ACT=$(echo "$AF" | grep -c 'Standby: no' || true)   # an output thread is actually mixing
  if [[ $((i % 10)) -eq 0 ]]; then echo "  wait $i s: dump ${#AF} bytes, effect lines $M, active threads $ACT, vlc $(vlc_state)"; fi
  if [[ "$M" -gt 0 && "$ACT" -gt 0 ]]; then break; fi
  # The JamesDSP root service gives up ("Plugin not installed") if it started
  # before audioserver finished coming back; kick it again every 25 s.
  if [[ $((i % 25)) -eq 0 ]]; then
    echo "  effect still missing, relaunching the JamesDSP app"
    $ADB shell "am force-stop com.termux.jdsp; am start -W -n $JDSP_ACT" >/dev/null 2>&1
  fi
done
if [[ "${M:-0}" -eq 0 || "${ACT:-0}" -eq 0 ]]; then
  echo "ABORT: no JamesDSP effect / no active output in audio_flinger after 90 s (vlc $(vlc_state)). JamesDSP service log:"
  $ADB shell "logcat -d -t 400 --pid=\$(pidof com.termux.jdsp)" 2>/dev/null | grep -E "RootAudioProcessor|SessionReceiver" | tail -5 | cut -c1-160
  [[ -n "$PREV_VOL" ]] && $SSH "termux-volume music $PREV_VOL" >/dev/null 2>&1
  exit 3
fi
echo "effect attached: $(echo "$AF" | grep -c -E "JamesDSP|f27317f4") line(s); active output thread(s): $(echo "$AF" | grep -c 'Standby: no')"
sleep 5
$ADB shell "su -c 'sh /data/local/tmp/jdsp_variants/sample_hal.sh $SECS'" | tr -d '\r' > "results/$TAG.csv"
[[ -n "$PREV_VOL" ]] && $SSH "termux-volume music $PREV_VOL" >/dev/null 2>&1
$ADB shell "am force-stop com.termux.vlc" >/dev/null 2>&1
python3 summarize.py "results/$TAG.csv"
