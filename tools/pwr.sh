#!/bin/sh
# pwr.sh <seconds> [awake|freeze|mem] : average battery current over the interval
B=/sys/class/power_supply/max170xx_battery
FB=$(find /sys/devices -name force_battery)
N=${1:-60}; M=${2:-awake}
echo 1 > $FB; sleep 3
c0=$(cat $B/charge_counter); t0=$(date +%s)
case $M in awake) sleep $N;; *) rtcwake -m $M -s $N >/dev/null 2>&1;; esac
t1=$(date +%s); c1=$(cat $B/charge_counter)
echo 0 > $FB
dt=$((t1-t0)); [ $dt -lt 1 ] && dt=1
echo "$M ${dt}s: $(( (c0-c1)*3600/dt/1000 )) mA avg (counter $c0 -> $c1 uAh), now $(cat $B/current_now) uA, $(cat $B/voltage_now) uV"
