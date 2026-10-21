#!/usr/bin/env python3
import sys, csv
rows=[r for r in csv.DictReader(open(sys.argv[1]))]
rows=[r for r in rows if r['hal_jiffies'] and r['hal_pid']]
pids={r['hal_pid'] for r in rows}
j0,j1=int(rows[0]['hal_jiffies']),int(rows[-1]['hal_jiffies']); secs=int(rows[-1]['t'])-int(rows[0]['t'])
cpu=100.0*(j1-j0)/100.0/secs           # CLK_TCK=100 on Android
p=[abs(int(r['current_ua']))*int(r['voltage_uv'])/1e9 for r in rows if r['current_ua'] and r['voltage_uv']]
khz=[int(r['cpu7_khz']) for r in rows if r['cpu7_khz']]
st={r.get('batt_status','') for r in rows}
batt=f"battery {sum(p)/len(p):.0f} mW avg" if st=={'Discharging'} else f"battery n/a (status {sorted(st)}, plug out for power numbers)"
print(f"{sys.argv[1]}: HAL pid(s) {sorted(pids)}  window {secs}s  HAL CPU {cpu:.1f}% of one core  "
      f"{batt}  cpu7 {sum(khz)/len(khz)/1000:.0f} MHz avg")
if len(pids)>1: print("  WARNING: HAL pid changed during the window (restart) - rerun")
