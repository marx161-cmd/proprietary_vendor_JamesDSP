#!/usr/bin/env bash
# Polls CPU% for audioserver (and any process matching a name filter) while
# audio plays, over a fixed duration. Run once per variant (stock/current)
# with a distinct LABEL; appends a row per sample to bench_compute.csv.
#
# Usage: bench_compute.sh <label> <duration_seconds> [poll_interval_seconds]
set -euo pipefail
DEV="${JDSP_BENCH_DEVICE:-100.69.13.12:5555}"
LABEL="${1:?label required (e.g. stock, current)}"
DURATION="${2:-30}"
INTERVAL="${3:-1}"
OUT="$(dirname "$0")/bench_compute.csv"

if [[ ! -f "$OUT" ]]; then
    echo "timestamp,label,pid,name,cpu_pct" > "$OUT"
fi

echo "Sampling CPU for audioserver every ${INTERVAL}s over ${DURATION}s (label=$LABEL)."
echo "Make sure audio is actively playing on the device now."

end=$((SECONDS + DURATION))
while [[ $SECONDS -lt $end ]]; do
    ts=$(date +%s.%N)
    # top -n 1: single snapshot; grep audioserver row (contains PID, CPU%, name)
    line=$(adb -s "$DEV" shell "top -n 1 -o PID,ARGS,%CPU 2>/dev/null | grep -i audioserver | head -1" | tr -d '\r')
    if [[ -n "$line" ]]; then
        pid=$(echo "$line" | awk '{print $1}')
        cpu=$(echo "$line" | awk '{print $NF}' | tr -d '%')
        echo "${ts},${LABEL},${pid},audioserver,${cpu}" >> "$OUT"
    fi
    sleep "$INTERVAL"
done

echo "Done. Appended to $OUT"
echo "Summary for label=$LABEL:"
awk -F, -v l="$LABEL" '$2==l {sum+=$5; n++} END {if(n>0) printf "  avg=%.2f%% over %d samples\n", sum/n, n}' "$OUT"
