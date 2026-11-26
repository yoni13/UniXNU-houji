#!/bin/bash
# Traced warm Settings launches (zero build): K warm launches, each under a console tl (3 s: the resident Preferences' main thread,
# SpringBoard's main, backboardd's compositing and encoding threads; the tl also keeps every CPU's runs, the four domains' clocks
# every 10 ms and the display's swap events), with the reference binds and floors of t-launch-ab.sh. Each launch's tl is saved
# as logs/N-ltr-K.log for launchclk.py (busy domains against their clock from the tap on). LOADSUMS="0 1 0 1": the governor's
# loadsum (dvfs set loadsum: a domain's load the sum of its cores' busy shares) for launch k, taken in turn, saved as -ltr-lsV-K.
# Usage: t-launch-trace.sh LOG [K]
L=$1; K=${2:-3}; D=/mnt/6f99f95d-0b93-4a42-84ea-c848405a2812/unixnu-work/perf-test; N=$(basename $L .log)
c() { timeout 6 python3 $D/phone.py cmd "$1" >/dev/null 2>&1; }
hot() { grep -a 'HOUJI THERMAL:' $L | grep -a -o 'hottest [0-9]*' | tail -1 | grep -o '[0-9]*' || echo 0; }
guard() { [ -e /dev/ttyACM0 ] || { echo "PHONE GONE"; exit 1; }; [ "$(hot)" -ge ${GUARD_C:-74} ] && { echo "GUARD $(hot) C"; c 'reboot bootloader'; exit 1; }; return 0; }
cool() { guard; for i in $(seq 1 40); do [ "$(hot)" -le ${COOL_C:-55} ] && return 0; sleep 10; done; echo "never cooled: $(hot) C"; }
settings_pid() { n=$(wc -l < $L); c 'ps 300'; sleep 2.5; tail -n +$n $L | grep -a 'HOUJI PS ' | sed 's/.*HOUJI PS \([0-9]*\) Preferences.*/\1/p;d' | tail -1; }
disp=$(grep -a 'compositing thread tid' /tmp/opencode/$N.out 2>/dev/null | tail -1 | grep -o 'tid [0-9]*' | grep -o '[0-9]*')
[ -z "$disp" ] && { echo "no compositing tid"; exit 1; }
c "bind 65:#$disp cpu5"; c 'bind 65:com.apple.coreanimation cpu7'; c 'bind 65:native-metal-encode cpu7'; c 'bind 30:0 cpu7'
c 'dvfs set framepct 100'; c 'dvfs set touchpct 100'; c 'dvfs set p7cap 0'; c 'dvfs set p7pin 0'
c home; sleep 3
# the app resident: one launch first (unmeasured), back home
c 'tap 740 2000'; sleep 4; c home; sleep 3
for k in $(seq 1 $K); do
  cool
  pid=$(settings_pid); [ -z "$pid" ] && { echo "launch $k: Preferences not resident"; c 'tap 740 2000'; sleep 4; c home; sleep 3; pid=$(settings_pid); }
  LS=(${LOADSUMS:-0}); ls=${LS[$(( (k - 1) % ${#LS[@]} ))]}; c "dvfs set loadsum $ls"; sleep 0.3
  m=$(wc -l < $L)
  c "tl 3000 $pid:0 30:0 65:#$disp 65:native-metal-encode"; sleep 0.3
  c 'tap 740 2000'
  timeout 120 tail -n +$m -f $L | grep -a -m1 -q 'HOUJI TL: end'; sleep 1
  tail -n +$m $L > logs/$N-ltr-ls$ls-$k.log
  sk=$(tail -n +$m $L | grep -a 'policy stock' | tail -1 | sed -n 's/.* skin \([0-9.]*\) C caps \([0-9,]*\) .*/\1 C caps \2/p')
  echo "launch $k loadsum $ls: Preferences pid $pid, tl saved ($(grep -a -c 'HOUJI TL C ' logs/$N-ltr-ls$ls-$k.log) CPU runs), skin ${sk:-?}, hottest $(hot) C"
  c home; sleep 4
done
c 'dvfs set loadsum 0'; c 'dvfs set framepct 45'; c 'dvfs set touchpct 65'
n=$(wc -l < $L); c gpuf; sleep 3; tail -n +$n $L | grep -a 'HOUJI GPUF:' | tail -1 | sed 's/^ *[0-9.]* /  /'
echo "crashes: backboardd $(grep -a -c 'sheng-exc: backboardd' $L) SpringBoard $(grep -a -c 'sheng-exc: SpringBoard' $L) Preferences $(grep -a -c 'sheng-exc: Preferences' $L)"
