#!/system/bin/sh
# Runs ON THE PHONE (as root). Samples the audio HAL process (where the AIDL
# effect actually runs -- NOT audioserver) and battery draw once per second.
#   sample_hal.sh <seconds> > samples.csv
# Columns: t, hal_pid, hal_cpu_jiffies (utime+stime cumulative), current_ua, voltage_uv, big_cpu_khz, batt_status
# Battery columns only mean anything while batt_status == Discharging (unplugged).
SECS="${1:-60}"
HAL=android.hardware.audio.service-aidl.aoc
echo "t,hal_pid,hal_jiffies,current_ua,voltage_uv,cpu7_khz,batt_status"
i=0
while [ "$i" -lt "$SECS" ]; do
  pid=$(pidof "$HAL")
  set -- $(cat /proc/$pid/stat 2>/dev/null | sed 's/^.*) //')
  # after stripping "pid (comm) ", fields: state=1 ... utime=12 stime=13
  j=$(( ${12:-0} + ${13:-0} ))
  cur=$(cat /sys/class/power_supply/battery/current_now 2>/dev/null)
  volt=$(cat /sys/class/power_supply/battery/voltage_now 2>/dev/null)
  khz=$(cat /sys/devices/system/cpu/cpu7/cpufreq/scaling_cur_freq 2>/dev/null)
  st=$(cat /sys/class/power_supply/battery/status 2>/dev/null)
  echo "$i,$pid,$j,$cur,$volt,$khz,$st"
  i=$((i+1)); sleep 1
done
