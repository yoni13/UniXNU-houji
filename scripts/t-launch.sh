#!/bin/bash
# Settings (com.apple.Preferences) launch benchmark: cold (process killed first) and warm (resident) launches of the app from its
# home-screen icon (the first page, fourth row, third column: tap 740 2000, root180's screenshot, root418's probe: launched
# Preferences pid 1139). Vision-free: no screenshots locate anything; a launch is verified by "ps" finding a new Preferences pid.
# Usage: t-launch.sh LOG [ROUNDS]. Each round: one cold and one warm launch, cooled between. Measured per launch (act, t-actions.sh):
#   tap -> first app frame swap (ms), gaps of the 0.8 s after it (the launch zoom animation: fps, <=9/12/17 ms shares), and the
#   swaps 2.5 s in total (settle). Cold: killpid on the pid ps found, verified gone, then tap. Warm: the same tap with it resident.
# Env: GAP seconds idle between launches (default 15), GUARD_C default 74, COLD=0 to skip the kill.
L=$1; R=${2:-3}; D=/mnt/6f99f95d-0b93-4a42-84ea-c848405a2812/unixnu-work/perf-test; N=$(basename $L .log)
c() { timeout 6 python3 $D/phone.py cmd "$1" >/dev/null 2>&1; }
hot() { grep -a 'HOUJI THERMAL:' $L | grep -a -o 'hottest [0-9]*' | tail -1 | grep -o '[0-9]*' || echo 0; }
guard() { [ -e /dev/ttyACM0 ] || { echo "PHONE GONE"; exit 1; }; [ "$(hot)" -ge ${GUARD_C:-74} ] && { echo "GUARD $(hot) C"; c 'reboot bootloader'; exit 1; }; return 0; }
cool() { guard; for i in $(seq 1 40); do [ "$(hot)" -le ${COOL_C:-55} ] && return 0; sleep 10; done; echo "never cooled: $(hot) C"; }
settings_pid() { n=$(wc -l < $L); c 'ps 300'; sleep 2.5; tail -n +$n $L | grep -a 'HOUJI PS ' | sed 's/.*HOUJI PS \([0-9]*\) Preferences.*/\1/p;d' | tail -1; }
act() {
  local name=$1; shift; guard
  c 'disp reset'; sleep 0.5; local n0=$(wc -l < $L)
  local first=1
  for x in "$@"; do if [[ $x =~ ^[0-9.]+$ ]]; then sleep $x; else c "$x"; [ $first = 1 ] && { first=0; sleep 2.8; c swaps; }; fi; done
  sleep 1; c swaps; c disp
  local iv=$(grep -a 'HOUJI DISP INTERVALS' $L | tail -1 | sed 's/.*INTERVALS: //')
  python3 - "$L" $n0 "$name" <<'EOF'
import re,sys
L=sys.argv[1]; n0=int(sys.argv[2]); name=sys.argv[3]
every=open(L,'rb').read().decode('latin-1').splitlines()
lines=every[n0-1:]
lt=lambda l: float(l.split()[0])
# The tap's time on the kernel clock (the swaps' clock): its own GESTURE AT line, the first within 2 s after this window's tap echo.
# Where the console dropped it (about a quarter of them), the echo's capture time plus the median capture->kernel offset of the
# whole boot's echo/GESTURE pairs ("inferred"). The capture side stamps lines in about 50 ms batches, so an echo-based time is
# good to +-25 ms only: that, with the window's own pairs alone (an offset of 0 where the window lost its line), made warm
# launches' "first frame" read -18 ms (root420).
_pairs=[(lt(x),float(m.group(1))) for x,y in zip(every,every[1:]) if (m:=re.search(r'GESTURE AT t=([0-9.]+)s',y)) and 'DEBUG: gesture tap ' in x]
off=sorted(k-t for t,k in _pairs)[len(_pairs)//2] if _pairs else 0.0
g=None; ts=[]; echo=None; inferred=False
for l in lines:
    if echo is None and 'DEBUG: gesture tap 740 2000' in l: echo=lt(l)
    m=re.search(r'GESTURE AT t=([0-9.]+)s',l)
    if m and echo is not None and g is None and lt(l)-echo<=2.0: g=float(m[1])
    m=re.search(r'SWAPS: W\d+ t=([0-9.]+)s',l)
    if m: ts.append(float(m[1]))
if g is None and echo is not None and _pairs: g=echo+off; inferred=True
if g is None:
    for l in lines:
        m=re.search(r'GESTURE AT t=([0-9.]+)s',l)
        if m: g=float(m[1]); break
iv=None
for l in lines:
    m=re.search(r'HOUJI DISP INTERVALS: (.*)',l)
    if m: iv=m[1]
a=[t for t in ts if g is not None and t>=g]
if not a: print(f'{name}: no swaps after the gesture'); sys.exit()
first_ms=round((a[0]-g)*1000)
# the animation's own window: frames up to the first idle gap over 100 ms (settle = that frame, not the capture window's end)
# the animation window: the long run of <=100 ms gaps among the frames after the first (leading stalls skipped)
run=[]; best=[]
for x,y in zip(a,a[1:]):
    if 0<y-x<=0.1: run.append((x,y))
    else:
        if len(run)>len(best): best=run
        run=[]
if len(run)>len(best): best=run
first=a[0]
if best: w=[x for x in a if best[0][0]<=x<=best[-1][1]]
else: w=[a[0]]
gaps=[round((y-x)*1000) for x,y in zip(w,w[1:]) if 0<y-x<=0.1]
anim=[x for x in gaps if 0<x<=100]
q=lambda ms: 100*sum(1 for x in anim if x<=ms)//len(anim) if anim else 0
s=sorted(anim)
settle=round((best[-1][1]-g)*1000) if best else first_ms
print(f'{name}: first frame +{first_ms} ms{" (tap inferred)" if inferred else ""}, settle +{settle} ms ({len(gaps)} frames over {(w[-1]-w[0])*1000:.0f} ms): '
      f'<=9 ms {q(9)}% <=12 ms {q(12)}% <=17 ms {q(17)}%'
      + (f', median {s[len(s)//2]} ms, {1000*len(anim)/sum(anim):.0f} fps' if anim else ''))
EOF
  # the launch's thermal state: the governor's last skin estimate and caps, and the hottest sensor
  local sk=$(tail -n +$n0 $L | grep -a 'policy stock' | tail -1 | sed -n 's/.* skin \([0-9.]*\) C caps \([0-9,]*\) .*/\1 C caps \2/p')
  echo "  [$name] skin ${sk:-?}, hottest $(hot) C"
}
c home; sleep 3
# The first launch of the boot is cold by itself, but against a seed the app was never opened: run it as a warm-up, unmeasured.
# NOWARMUP=1: a later call of the same boot (t-launch-ab.sh), the app opened already.
if [ "${NOWARMUP:-0}" != 1 ]; then
  pid=$(settings_pid)
  if [ -n "$pid" ]; then c "killpid $pid"; sleep 2; fi
  act warmup 'tap 740 2000' 4
  c home; sleep 3
fi
for r in $(seq 1 $R); do
  cool
  # cold: kill and verify
  if [ "${COLD:-1}" = 1 ]; then
    pid=$(settings_pid)
    if [ -z "$pid" ]; then echo "== round $r cold: no pid found, launching+retrying"; act cold-find 'tap 740 2000' 4; c home; sleep 3; pid=$(settings_pid); fi
    n=$(wc -l < $L); c "killpid $pid"; sleep 2.5
    tail -n +$n $L | grep -q 'HOUJI KILLPID: killing Preferences' || { echo "KILL FAILED (pid $pid)"; exit 1; }
    pid2=$(settings_pid); [ -n "$pid2" ] && { echo "STILL ALIVE pid $pid2"; exit 1; }
    echo "== round $r"
    act cold 'tap 740 2000' 3.5
    c home; sleep 3
  fi
  cool
  echo "== round $r (warm)"
  # WARM_BIND=cpuN (or a cluster): the resident Preferences' main thread (PID:0) bound there for the warm launch, unbound after
  # (the console's bind; "PID:NUMBER" would look for a thread NAMED so: root420's 30:314 / 30:4021 binds found none, tid 0, ret 4)
  if [ -n "${WARM_BIND:-}" ]; then
    wpid=$(settings_pid); n=$(wc -l < $L); [ -n "$wpid" ] && c "bind $wpid:0 $WARM_BIND"; sleep 0.5
    echo "  [bind] $(tail -n +$n $L | grep -a 'HOUJI BIND:' | tail -1 | sed 's/^ *[0-9.]* //')"
  fi
  act warm 'tap 740 2000' 3.5
  [ -n "${WARM_BIND:-}" ] && [ -n "$wpid" ] && c "bind $wpid:0 cpu-1"
  c home; sleep 3
  sleep ${GAP:-15}
done
guard; c gpuf; sleep 3; tail -30 $L | grep -a 'HOUJI GPUF:' | tail -1 | sed 's/^ *[0-9.]* /  /'
# PROF=1 (after the rounds): the launch's timeline traced once (tl 3000: SpringBoard's main thread, backboardd's compositing and
# encoding, the new Preferences' main thread once ps finds it), so the cold launch's spawn and first frames line up with the swaps.
# PROF=1 (after the rounds): one cold launch with a tl of SpringBoard's main thread and backboardd's compositing and encoding
# threads (the tl logs' own analysis scripts read it), plus a threads snapshot of the new Preferences after its first frame.
if [ "${PROF:-0}" = 1 ]; then
  cool
  pid=$(settings_pid); [ -n "$pid" ] && { c "killpid $pid"; sleep 2.5; }
  disp=$(grep -a 'compositing thread tid' $L | tail -1 | grep -o 'tid [0-9]*' | grep -o '[0-9]*')
  [ -z "$disp" ] && { echo "PROF: no compositing tid in the log"; exit 0; }
  c "tl 4000 30:0 65:#$disp 65:com.apple.coreanimation 65:native-metal-encode"
  sleep 0.2; c 'tap 740 2000'; sleep 4
  newpid=$(settings_pid); [ -n "$newpid" ] && c "threads $newpid"
  echo "PROF: tl and threads of Preferences pid ${newpid:-?} in the log"
fi
