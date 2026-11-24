#!/bin/bash
# Thread placement A/B in one boot (seed SEED, the best Control Center config, tt=0, no census), unless NOBOOT=1. Sides (SIDES, default
# "P4 PC P4 PC P4u PCu P4u PCu"): P4 SpringBoard's main thread, backboardd's compositing thread (com.apple.coreanimation.display, by
# id), its render-server thread and the encoding thread all on the prime core; PC the same but SpringBoard's main thread on cpu3 alone;
# P7 / C7 as P4 / PC with the prime core's cap lifted (dvfs set p7cap CAP7, default 3053). Every side has the frame and touch floors at
# 100 %. A side ending in u is untraced: no tl and no GPU stamps, the opens' swaps only (openfps.py). Traced opens take tl (its sampler
# on cpu0-1) and the GPU driver's job timeline (console gtl). Before each side the phone cools to COOL_C (default 55 C); past STOP_C
# (default 70 C) the run stops. First (before the sides) the CPU table dump (dvfs lut) and cpu7 timed at each vote of SWEEP (default 500 1500 2631 3053).
# NOLBOX= (set, even empty): without m2v-lbox=1, for 11 bytes of command line in a diagnostic boot.
# NOBK= (set, even empty): without tu-bk=64, for 9 bytes (the gpu-preempt=0 A/B: one GPU ring).
# Usage: [SIDES=...] [K=3] t-bal.sh rootNNN
cd /mnt/6f99f95d-0b93-4a42-84ea-c848405a2812/unixnu-work/ios27; W=$PWD; D=$W/../perf-test; N=${1:?rootNNN}; L=logs/$N.log
A="sko=${SKIN:-4} no-lc=1 houji_cpu_interactive=1 gpu-purge-age-s=60 native-data=userdata gpu-opp=stock vsync=120 tu-bc=8 m2v-async=1 metal-flow=6 m2v-wake=1${NOBK- tu-bk=64}${NOLBOX- m2v-lbox=1} tu-ac=1 $EXTRA"
# DIAG=1: boot with diag=1 (the GPU stack's diagnostics: Turnip's pass stamps in the render server's frames.trace, read after each
# traced open; the driver's CP stamps from boot), for the GPU's time by pass; fps then carry its overhead.
[ "${DIAG:-0}" = 1 ] && unset NODIAG || export NODIAG=
if [ "${NOBOOT:-0}" != 1 ]; then
  timeout 6 python3 $D/phone.py cmd 'reboot bootloader' >/dev/null 2>&1; timeout 10 adb -s c0d531c0 reboot bootloader >/dev/null 2>&1
  for i in $(seq 1 60); do timeout 3 fastboot devices | grep -q c0d531c0 && break; sleep 5; done
  ONLYCC=1 NOSLS=1 NOUFP=1 SETTLE=${SETTLE:-300} SETTLE_C=${SETTLE_C:-55} COOL=0 RAM_GIB=8 MAXVNODES=132096 UNIQUE_CHIP_ID=1 \
    DATA=$W/data/data-24B156-gpu8-${SEED:-leanlc33}.dbr TRUST=$W/tmp/trust-24B156-gpu8-${SEED:-leanlc33}.bin SOAK=0 MAXRUN=2400 \
    timeout 2500 ./t-round.sh $N "$A" 2>&1 | grep -v FRAMEINFO | grep 'GUARD\|settled\|PACK\|NO CON' | cut -c1-120
fi
[ -e /dev/ttyACM0 ] || { echo "no console"; exit 1; }
c() { timeout 6 python3 $D/phone.py cmd "$1" >/dev/null 2>&1; }
hottest() { local n=$(wc -l < $L); c temp; sleep 2; tail -n +$n $L | grep -a 'HOUJI THERMAL: cpu' | tail -1 | sed 's/.*hottest \([0-9]*\)\..*/\1/'; }
c 'l3 mode 1'
n=$(wc -l < $L); c 'ps 1'; sleep 8
bb=$(tail -n +$n $L | grep -a "HOUJI PS [0-9]* backboardd " | awk '{print $4}' | tail -1)
[ -n "$bb" ] || { echo "no backboardd"; exit 1; }
n=$(wc -l < $L); c "threads $bb"; sleep 3
disp=$(tail -n +$n $L | grep -a "HOUJI THREADS $bb .*com.apple.coreanimation.display" | sed 's/^.*HOUJI /HOUJI /' | awk '{print $6, $4}' | sort -n | tail -1 | awk '{print $2}')
echo "backboardd $bb, compositing thread tid ${disp:-?}"
[ -n "$disp" ] || exit 1
B="30:0 $bb:#$disp $bb:com.apple.coreanimation $bb:native-metal-encode"
# The CPU table (dvfs lut) and cpu7 timed at each vote of SWEEP first: root369 reset reading registers past a domain's votes.
n=$(wc -l < $L); c 'dvfs lut'; sleep 5; tail -n +$n $L | grep -a 'HOUJI CPU LUT' | sed 's/^ *[0-9.]* /  /' > logs/$N-lut.txt
echo "LUT lines: $(grep -c LUT logs/$N-lut.txt)"; [ -e /dev/ttyACM0 ] || { echo "console gone after dvfs lut"; exit 1; }
for v in ${SWEEP-500 1500 2631 3053}; do
  [ $v -gt 2631 ] && c "dvfs set p7cap $v" || c 'dvfs set p7cap 0'
  c "dvfs set p7pin $v"; sleep 1; n=$(wc -l < $L); c 'cpub 7'
  timeout 120 tail -n +$n -f $L | grep -a -m1 'HOUJI CPUB: cpu7 alu' | sed "s/^ *[0-9.]* /  [pin $v] /"
  n=$(wc -l < $L); c temp; sleep 2; tail -n +$n $L | grep -a 'HOUJI THERMAL: cpu' | tail -1 | sed "s/^ *[0-9.]* /  [pin $v] /" | cut -c1-120
done
c 'dvfs set p7pin 0'; c 'dvfs set p7cap 0'
P=/private/var/mobile/Library/Caches/com.apple.backboardd/m2v/frames.trace
# The stock policy's caps depend on the skin estimate (from the battery's temperature): under 25.0 C it lifts every domain's cap
# (root370: 22.7 C, cpu2-4 at 3148.8 MHz; root371's boot reached 76 C). SKIN (default 4, also on the boot line as sko=4) is added to it,
# so the stock 2630.4 MHz caps hold in every side (25-32 C; past 32 C the second step caps cpu7 at 2169.6) and only p7cap lifts cpu7's.
c "dvfs set skin ${SKIN:-4}"
c 'swipe 1120 15 1120 700 200'; sleep 2.5; c 'swipe 600 2669 600 2000 150'; sleep 8
# HOLD=S: before the sides, Control Center opened and held S seconds, twice (the panel checked by eye, a boot with sbl=1), with a
# screenshot of each (what the GPU wrote: a difference between the two is the display's blending)
if [ -n "${HOLD:-}" ]; then
  for k in 1 2; do
    echo "== HOLD $k at $(date +%H:%M:%S): Control Center open for $HOLD s"; c 'swipe 1120 15 1120 700 200'; sleep 3
    timeout 90 python3 $D/phone.py shot $L logs/$N-hold-$k.png >/dev/null 2>&1; sleep $HOLD; c 'swipe 600 2669 600 2000 150'; sleep 8
  done
fi
# The reference placement is Q4 (root405/406: +10 fps over P4, its first frame as P4's); the default sides compare it with P4.
for s in ${SIDES:-Q4u P4u Q4u P4u}; do
  # A side ending in n or w is untraced too (logged as ut-), for what about a traced open changes the first frame: n starts a tl of
  # SpringBoard's main thread alone (TLN, default 30:0) 0.2 s before the swipe, its output not used; w only waits those 0.2 s.
  x=${s: -1}; case $x in n|w) b=${s%?}; s0x=$x;; *) b=${s%u}; s0x=;; esac
  for i in $(seq 1 30); do h=$(hottest); [ -n "$h" ] && [ "$h" -le ${COOL_C:-55} ] && break; sleep 10; done
  [ -n "$h" ] && [ "$h" -gt ${STOP_C:-70} ] && { echo "STOP: hottest $h C"; break; }
  # Q4: as P4 but the compositing thread on cpu5 alone (the scheduler's clusters are cpu0-1 and cpu2-7: no cpu5-6 pair to bind to)
  case $b in Q4) c "bind $bb:#$disp cpu5";; Q3) c "bind $bb:#$disp cpu3";; *) c "bind $bb:#$disp cpu7";; esac
  c "bind $bb:com.apple.coreanimation cpu7"; c "bind $bb:native-metal-encode cpu7"
  case $b in PC|C7) c 'bind 30:0 cpu3';; *) c 'bind 30:0 cpu7';; esac
  case $b in P7|C7) c "dvfs set p7cap ${CAP7:-3053}";; *) c 'dvfs set p7cap 0';; esac
  c 'dvfs set framepct 100'; c 'dvfs set touchpct 100'
  # GTLU=1: the GPU's job stamps (gtl) on in untraced sides too, dumped after each open's swaps (CP stamps only, no sampler)
  [ $b = $s ] || [ "${GTLU:-0}" = 1 ] && c 'gtl on' || c 'gtl off'
  echo "== side $s (start ${h:-?} C)"
  s0=$(wc -l < $L)
  for k in $(seq 1 ${K:-3}); do
    m=$(wc -l < $L)
    if [ $b = $s ]; then
      # THREADS=1: backboardd's threads' CPU time (user + system, and system alone) before and after the open (threads-delta.py)
      [ "${THREADS:-0}" = 1 ] && { c "threads $bb"; sleep 1; }
      # swaps twice: the console keeps the newest 128, and at Q4's rate those read 1.3 s or more after the swipe all came after it
      # (root403: the open's first frames were missing, its "first frame" the oldest swap kept)
      c "tl 2500 $B"; sleep 0.2; c 'swipe 1120 15 1120 700 200'; sleep 0.7; c swaps; sleep 0.6; c swaps; c 'gtl dump 500'
      # NOTES=1 (a boot with m2v-count=2): the bridge's per-commit work rows of this open, for cb-split.py
      [ "${NOTES:-0}" = 1 ] && c 'notes 400'
      # the plugin's trace writer runs 1-2 s behind in an open (root408: read 2.1 s after the gesture, its records reached +0.3 to +1.1 s):
      # the trace is read after the tl's end and TRACE_WAIT more seconds (default 3)
      if [ "${DIAG:-0}" = 1 ] || [ "${TRACE:-0}" = 1 ]; then timeout 240 tail -n +$m -f $L | grep -a -m1 -q 'HOUJI TL: end'; sleep ${TRACE_WAIT:-3}; fi
      # TRACE=1 (a boot with gtr=1) or DIAG=1: the newest TRACE_LEN (hex, default 0x100000) bytes of frames.trace, read at once: this open's
      # passes and, with turnip-houji-op-trace.patch, its compute dispatches, transfers, flushes and command buffers
      if [ "${DIAG:-0}" = 1 ] || [ "${TRACE:-0}" = 1 ]; then
        n=$(wc -l < $L); c "readfile $P"; sleep 2
        sz=$(tail -n +$n $L | grep -a "HOUJI READFILE: $P" | tail -1 | sed -n 's/.* \([0-9]*\) bytes.*/\1/p')
        if [ -n "$sz" ] && [ "$sz" -gt 0 ]; then
          B=$((16#${TRACE_LEN:-100000})); st=0; [ "$sz" -gt $B ] && st=$(( (sz - B) / 72 * 72 ))
          # console hex reads at most 8 MiB at once (HoujiUsbConsole.c): a longer TRACE_LEN is read in 8 MiB pieces from st on (root408/409
          # asked for 16 MiB in one read and got the older 8 MiB of it, not the open's)
          if [ "${TRACE_HEX:-0}" = 1 ]; then
            for ((o = st; o < sz; o += 0x800000)); do
              kk=$(( sz - o < 0x800000 ? sz - o : 0x800000 ))
              n=$(wc -l < $L); c "hex $P $(printf %x $o) $(printf %x $kk)"
              timeout 900 tail -n +$n -f $L | grep -a -m1 -q "HOUJI HEX: .*bytes from $(printf %x $o)"
            done
          else
            # console dump (the bytes in bulk transfers; capture.py saves them as logs/N-dump-K.bin, "HOUJI DUMP SAVED" in the log),
            # checked against the phone's CRC-32
            n=$(wc -l < $L); t=$(date +%s.%N); c "dump $P $(printf %x $st) $(printf %x $(( sz - st )))"
            timeout 120 tail -n +$n -f $L | grep -a -m1 -q 'HOUJI DUMP: .*error'; sleep 0.5
            dc=$(tail -n +$n $L | grep -a 'HOUJI DUMP: ' | tail -1 | sed -n 's/.*crc \([0-9a-f]*\),.*/\1/p')
            hc=$(tail -n +$n $L | grep -a 'HOUJI DUMP SAVED' | tail -1 | sed -n 's/.* crc \([0-9a-f]*\) in.*/\1/p')
            echo "  [dump] $(tail -n +$n $L | grep -a 'HOUJI DUMP SAVED' | tail -1 | sed 's/.*SAVED //') phone crc ${dc:-?} $([ -n "$dc" ] && [ "$dc" = "$hc" ] && echo match || echo MISMATCH), $(echo "$(date +%s.%N) - $t" | bc | cut -c1-5) s"
          fi
        fi
      fi
      timeout 240 tail -n +$m -f $L | grep -a -m1 -q 'HOUJI TL: end'
      [ "${THREADS:-0}" = 1 ] && { c "threads $bb"; timeout 30 tail -n +$m -f $L | grep -a -m2 -q 'HOUJI THREADS: pid'; sleep 1; }
      tail -n +$m $L > logs/$N-tl-$s-$k-$(date +%s).log
    else
      [ "${THREADS:-0}" = 1 ] && { c "threads $bb"; sleep 1; }
      # USTD=MS: the compositing thread's user stacks for MS ms from the swipe (console ustack, read from a kernel thread: no sampler on
      # the CPUs), whether it is blocked or runnable while the first frame is late
      [ -n "${USTD:-}" ] && c "ustack $bb $disp $USTD"
      case $s0x in n) c "tl 2500 ${TLN:-30:0}"; sleep 0.2;; w) sleep 0.2;; esac
      c 'swipe 1120 15 1120 700 200'; sleep 0.8; c swaps; sleep 0.7; c swaps
      [ "$s0x" = n ] && timeout 240 tail -n +$m -f $L | grep -a -m1 -q 'HOUJI TL: end'
      [ "${THREADS:-0}" = 1 ] && { c "threads $bb"; timeout 30 tail -n +$m -f $L | grep -a -m2 -q 'HOUJI THREADS: pid'; }
      # SHOT=1: a screenshot with Control Center open, the first untraced open of each side (a visual check of what it renders)
      [ "${SHOT:-0}" = 1 ] && [ $k = 1 ] && timeout 90 python3 $D/phone.py shot $L logs/$N-shot-$s-$(date +%s).png >/dev/null 2>&1
      # FB=1 (a kernel with console fbdump): the whole panel at full resolution, 8-bit RGB, the first untraced open of each side, for
      # the visual A/B (fbshot.py diff)
      [ "${FB:-0}" = 1 ] && [ $k = 1 ] && { sleep 1; timeout 90 python3 fbshot.py grab $L logs/$N-fb-$s-$(date +%s).png 2>&1 | sed 's/^/  [fb] /'; }
      [ "${GTLU:-0}" = 1 ] && c 'gtl dump 500'; sleep 3; tail -n +$m $L > logs/$N-ut-$s-$k-$(date +%s).log
      [ -n "${USTD:-}" ] && tail -n +$m $L | grep -a 'HOUJI USTACK' | cut -c10- > logs/$N-ust-$s-$k.txt
    fi
    c 'swipe 600 2669 600 2000 150'; sleep 8
    hk=$(tail -n +$m $L | grep -a 'policy stock' | sed 's/.*hottest \([0-9]*\)\..*/\1/' | sort -n | tail -1)
    [ -n "$hk" ] && [ "$hk" -gt ${STOP_C:-70} ] && { echo "  STOP: hottest $hk C in open $k of $s"; break 2; }
  done
  n=$(wc -l < $L); c gpuf; c gpus; c gpup; c dvfs; sleep 3
  tail -n +$n $L | grep -a 'HOUJI GPUF\|policy stock' | tail -2 | sed "s/^ *[0-9.]* /  [$s after] /" | cut -c1-240
  tail -n +$n $L | grep -a 'HOUJI GPUS: fence' | tail -1 | sed "s/^ *[0-9.]* /  [$s after] /" | cut -c1-160
  echo "  [$s] peak hottest $(tail -n +$s0 $L | grep -a 'policy stock' | sed 's/.*hottest \([0-9.]*\) C.*/\1/' | sort -n | tail -1) C"
done
# PROF=K: K more opens in P4 (SpringBoard's main thread on cpu7, cpu7's cap stock) with the encoding thread's user stacks sampled
# (console ustack, every 2 ms for 1.5 s from the swipe), for plugin-sym.py with the build's link map
if [ "${PROF:-0}" -gt 0 ]; then
  c 'bind 30:0 cpu7'; c 'dvfs set p7cap 0'; c 'gtl off'
  for k in $(seq 1 $PROF); do
    for i in $(seq 1 30); do h=$(hottest); [ -n "$h" ] && [ "$h" -le ${COOL_C:-55} ] && break; sleep 10; done
    n=$(wc -l < $L); c "ustack $bb native-metal-encode 1500"; sleep 0.1; c 'swipe 1120 15 1120 700 200'
    timeout 120 tail -n +$n -f $L | grep -a -m1 -q 'HOUJI USTACK.*samples'
    sleep 2; tail -n +$n $L | grep -a 'HOUJI USTACK' | cut -c10- > logs/$N-ustack-$k.txt
    echo "  [prof $k] $(grep -a -c 'USTACK [0-9]* run' logs/$N-ustack-$k.txt) stacks"
    c 'swipe 600 2669 600 2000 150'; sleep 8
  done
fi
c 'gtl off'; c 'dvfs set framepct 45'; c 'dvfs set touchpct 65'
c "bind $bb:native-metal-encode cpu-1"; c 'bind 30:0 cpu-1'; c "bind $bb:com.apple.coreanimation cpu-1"; c "bind $bb:#$disp cpu-1"
n=$(wc -l < $L); c gpuf; c gpup; sleep 4; tail -n +$n $L | grep -a 'HOUJI GPUF\|HOUJI GPUP' | grep -v 'GPUP E' | sed 's/^ *[0-9.]* /  /'
# hs bit 3: Turnip's counts of lazily allocated BOs kept mapped (turnip-houji-lazy-defer.patch)
n=$(wc -l < $L); c 'cat /private/var/mobile/Library/Caches/com.apple.backboardd/m2v/lazy-aged.txt'; sleep 3; tail -n +$n $L | grep -a 'lazy BOs freed' | tail -1 | sed 's/^ *[0-9.]* /  /'
echo "crashes: backboardd $(grep -a -c 'sheng-exc: backboardd' $L) SpringBoard $(grep -a -c 'sheng-exc: SpringBoard' $L)"
# The kernel console drops about a quarter of the GESTURE AT lines: inferred from the swipe echoes (fixgesture.py), for the fps only
ls logs/$N-ut-*.log logs/$N-tl-*.log >/dev/null 2>&1 && timeout 120 python3 fixgesture.py $(ls logs/$N-ut-*.log logs/$N-tl-*.log 2>/dev/null) | sed 's/^/  /'
# DPU=1 (last, after every measurement and gpuf: a read that resets the phone costs nothing measured): the display pipes, read only (console dpu: active CTLs, the pipes they fetch, their format and address, the mixers' blend)
if [ "${DPU:-0}" = 1 ]; then
  n=$(wc -l < $L); c dpu; sleep 4; tail -n +$n $L | grep -a 'HOUJI DPU' | sed 's/^ *[0-9.]* /  /' | tee logs/$N-dpu.txt
  [ -e /dev/ttyACM0 ] || { echo "console gone after dpu"; exit 1; }
fi
