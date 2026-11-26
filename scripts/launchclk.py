#!/usr/bin/env python3
"""A traced launch's clocks against its work (t-launch-trace.sh's logs/N-ltr-K.log, or any tl around a tap): from the tap on, every
10 ms clock sample (HOUJI TL CLK, the domains' hardware state) with which of the big domains had a CPU running a thread then (the
tl's CPU runs, HOUJI TL C; its own reader, a kernel_task thread, left out) and the display's swaps. A domain running a thread at a
clock under 90 % of the session's top is a low vote (the hardware lowers a busy cluster only under its thermal limit). Summary:
busy samples at a low clock before the first swap, in the animation's first 100 ms, and after. Usage: launchclk.py LOG [--rows]"""
import re, sys, collections
CPUS = {1: (5, 6), 2: (7,), 3: (2, 3, 4)}      # EPSS domains 1-3: cpu5-6, cpu7, cpu2-4
NAMES = {1: 'cpu5-6', 2: 'cpu7', 3: 'cpu2-4'}
BOOT = next((a[7:] for a in sys.argv if a.startswith('--boot=')), None)
OFF = None
if BOOT:   # the boot's median capture -> kernel offset of the tap echoes that kept their GESTURE AT line (as t-launch.sh)
    B = open(BOOT, 'rb').read().decode('latin-1').splitlines()
    p = sorted(float(m[1]) - float(x.split()[0]) for x, y in zip(B, B[1:])
               if (m := re.search(r'GESTURE AT t=([0-9.]+)s', y)) and 'DEBUG: gesture tap ' in x)
    OFF = p[len(p) // 2] if p else None
for f in [a for a in sys.argv[1:] if not a.startswith('--')]:
    L = open(f, 'rb').read().decode('latin-1').splitlines()
    base = next((int(m[1]) for m in (re.search(r'HOUJI TL BASE (\d+)', l) for l in L) if m), None)
    tap = next((float(m[1]) for m in (re.search(r'GESTURE AT t=([0-9.]+)s', l) for l in L) if m), None)
    if tap is None and OFF is not None:   # the line dropped: the echo's capture time plus the boot's offset (+-25 ms)
        tap = next((float(l.split()[0]) + OFF for l in L if 'DEBUG: gesture tap 740 2000' in l), None)
        if tap is not None:
            print(f'{f.split("/")[-1]}: tap inferred from its echo (+-25 ms)')
    if base is None or tap is None:
        print(f, 'no tl base or no gesture'); continue
    t0 = tap * 1e6 - base / 1000                                   # the tap in tl microseconds
    task = {m[1]: m[3] for m in (re.search(r'TL TASK (\w+) (\d+) (\S+)', l) for l in L) if m}
    runs = [(int(m[1]), int(m[2]), int(m[3]), int(m[4]), task.get(m[5], m[5])) for m in
            (re.search(r'TL C (\d) (\d+) (\d+) (\d+) (\w+)', l) for l in L) if m]
    runs = [r for r in runs if r[4] != 'kernel_task']
    clk = [(int(m[1]), [int(x) // 1000 for x in m[2].split()]) for m in (re.search(r'TL CLK (\d+) ([\d ]+)$', l.strip()) for l in L) if m]
    sw = sorted(int(m[1]) for m in (re.search(r'TL EV (\d+) 5 ', l) for l in L) if m)
    sw = [s for s in sw if s >= t0]
    if not clk or not sw:
        print(f, 'no clocks or no swaps after the tap'); continue
    top = {d: max(v[d] for t, v in clk) for d in CPUS}
    first = sw[0]
    gaps = [b - a for a, b in zip(sw, sw[1:])]
    end = next((i for i, g in enumerate(gaps) if g > 100000), len(gaps))
    anim_end = sw[end]
    an = gaps[:end]
    print(f'{f.split("/")[-1]}: tap at {t0/1000:.0f} ms (tl time), first swap +{(first-t0)/1000:.0f} ms, animation {len(an)} gaps '
          f'{len(an)/(sum(an)/1e6) if an else 0:.0f} fps, <=9 ms {sum(g <= 9000 for g in an)*100//max(1,len(an))} %, '
          f'gaps > 17 ms at +' + ', +'.join(f'{(sw[i]-t0)/1000:.0f}({g/1000:.0f})' for i, g in enumerate(an) if g > 17000))
    print('  tops (MHz) ' + ' '.join(f'{NAMES[d]} {top[d]}' for d in CPUS))
    S = collections.defaultdict(lambda: [0, 0])
    rows = []
    for t, v in clk:
        if t < t0 - 20000 or t > anim_end + 50000:
            continue
        phase = 'tap->first swap' if t < first else 'first 100 ms' if t < first + 100000 else 'rest of animation' if t <= anim_end else 'after'
        marks = []
        for d, cpus in CPUS.items():
            busy = [r for r in runs if r[0] in cpus and r[1] < t + 5000 and r[2] > t - 5000]
            if busy:
                low = v[d] < 0.9 * top[d]
                S[(phase, d)][0] += 1; S[(phase, d)][1] += low
                marks.append(f'{NAMES[d]} {v[d]}{"!" if low else ""} [{",".join(sorted({r[4][:10] for r in busy}))}]')
        nsw = sum(1 for s in sw if t - 5000 <= s < t + 5000)
        rows.append(f'   +{(t-t0)/1000:5.0f} ms {"S" if nsw else " "} ' + ' | '.join(marks))
    print('  busy domain samples at a low clock (< 90 % of its top), by phase:')
    for phase in ('tap->first swap', 'first 100 ms', 'rest of animation'):
        print(f'    {phase:18s} ' + '  '.join(f'{NAMES[d]} {S[(phase, d)][1]}/{S[(phase, d)][0]}' for d in CPUS))
    if '--rows' in sys.argv:
        print('\n'.join(rows))
