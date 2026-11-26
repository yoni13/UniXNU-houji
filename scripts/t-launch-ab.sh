#!/bin/bash
# Settings launch A/B in one boot, zero build: arms interleaved round by round (A B C D, ROUNDS times), each a t-launch.sh round
# (one cold and one warm launch, cooled to COOL_C before each), on a phone t-bal.sh brought up (its log holds the compositing
# thread's tid). Reference binds re-applied before every arm: backboardd's compositing thread on cpu5 (Q4), its Core Animation and
# Metal encoding threads and SpringBoard's main thread on cpu7, frame and touch floors 100 %.
#   A  reference
#   B  A + cpu7's cap lifted to its own top (dvfs set p7cap 3053: the stock policy's first step only; its hotter steps, the
#      emergency cut and the 74 C guard still apply)
#   C  B + the resident Preferences' main thread on cpu7 for the warm launch (WARM_BIND, t-launch.sh; a cold launch has no pid
#      before its tap, so C's cold launch is B's)
#   D  A with SpringBoard's main thread unbound (bind 30:0 cpu-1: the earlier "30:314" binds found no thread)
# Usage: t-launch-ab.sh LOG [ROUNDS] [ARMS]
L=$1; R=${2:-3}; ARMS=${3:-"A B C D"}; D=/mnt/6f99f95d-0b93-4a42-84ea-c848405a2812/unixnu-work/perf-test
c() { timeout 6 python3 $D/phone.py cmd "$1" >/dev/null 2>&1; }
disp=$(grep -a 'compositing thread tid' /tmp/opencode/$(basename $L .log).out 2>/dev/null | tail -1 | grep -o 'tid [0-9]*' | grep -o '[0-9]*')
[ -z "$disp" ] && disp=$(grep -a 'compositing thread tid' $L | tail -1 | grep -o 'tid [0-9]*' | grep -o '[0-9]*')
[ -z "$disp" ] && { echo "no compositing tid"; exit 1; }
ref() {
  c "bind 65:#$disp cpu5"; c 'bind 65:com.apple.coreanimation cpu7'; c 'bind 65:native-metal-encode cpu7'; c 'bind 30:0 cpu7'
  c 'dvfs set framepct 100'; c 'dvfs set touchpct 100'; c 'dvfs set p7cap 0'; c 'dvfs set p7pin 0'
}
first=1
for r in $(seq 1 $R); do
  for arm in $ARMS; do
    ref; unset WARM_BIND
    case $arm in
      B) c 'dvfs set p7cap 3053';;
      C) c 'dvfs set p7cap 3053'; export WARM_BIND=cpu7;;
      D) c 'bind 30:0 cpu-1';;
    esac
    n=$(wc -l < $L); sleep 1
    echo "=== arm $arm round $r: $(tail -n +$n $L | grep -a 'HOUJI BIND:\|DVFS SET' | sed 's/^ *[0-9.]* HOUJI //' | tr '\n' ';' | cut -c1-300)"
    # (the pipe would run an if in a subshell, where first=0 does not stay: root443 warmed up before every arm)
    nw=$([ $first = 1 ] && echo 0 || echo 1); first=0
    NOWARMUP=$nw bash t-launch.sh $L 1 | grep -v '^$'
    [ -e /dev/ttyACM0 ] || { echo "PHONE GONE"; exit 1; }
  done
done
ref; c 'dvfs set framepct 45'; c 'dvfs set touchpct 65'
n=$(wc -l < $L); c gpuf; sleep 3; tail -n +$n $L | grep -a 'HOUJI GPUF:' | tail -1 | sed 's/^ *[0-9.]* /  /'
echo "crashes: backboardd $(grep -a -c 'sheng-exc: backboardd' $L) SpringBoard $(grep -a -c 'sheng-exc: SpringBoard' $L) Preferences $(grep -a -c 'sheng-exc: Preferences' $L)"
