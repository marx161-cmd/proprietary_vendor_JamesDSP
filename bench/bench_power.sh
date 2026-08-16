#!/usr/bin/env bash
# Polls battery current_now/voltage_now while audio plays, computes average
# instantaneous power draw over the run. Run once per variant with a
# distinct LABEL; appends rows to bench_power.csv.
#
# Usage: bench_power.sh <label> <duration_seconds> [poll_interval_seconds]
set -euo pipefail
DEV="${JDSP_BENCH_DEVICE:-100.69.13.12:5555}"
LABEL="${1:?label required (e.g. stock, current)}"
DURATION="${2:-30}"
INTERVAL="${3:-1}"
OUT="$(dirname "$0")/bench_power.csv"
BATT="/sys/class/power_supply/battery"

if [[ ! -f "$OUT" ]]; then
    echo "timestamp,label,current_ua,voltage_uv,power_mw" > "$OUT"
fi

echo "Sampling battery current/voltage every ${INTERVAL}s over ${DURATION}s (label=$LABEL)."
echo "Screen should be on, same brightness, same track playing, for every label you compare."

end=$((SECONDS + DURATION))
while [[ $SECONDS -lt $end ]]; do
    ts=$(date +%s.%N)
    cur=$(adb -s "$DEV" shell "cat $BATT/current_now 2>/dev/null" | tr -d '\r\n')
    volt=$(adb -s "$DEV" shell "cat $BATT/voltage_now 2>/dev/null" | tr -d '\r\n')
    if [[ -n "$cur" && -n "$volt" ]]; then
        # current_now is uA (can be negative = discharging on most kernels),
        # voltage_now is uV. power_mW = |current_uA| * voltage_uV / 1e9
        power=$(python3 -c "print(abs(${cur})*${volt}/1e9)" 2>/dev/null || echo "")
        echo "${ts},${LABEL},${cur},${volt},${power}" >> "$OUT"
    fi
    sleep "$INTERVAL"
done

echo "Done. Appended to $OUT"
echo "Summary for label=$LABEL:"
awk -F, -v l="$LABEL" '$2==l && $5!="" {sum+=$5; n++} END {if(n>0) printf "  avg=%.1fmW over %d samples\n", sum/n, n}' "$OUT"
