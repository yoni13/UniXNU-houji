#!/usr/bin/env python3
"""A within-boot toggle A/B (t-bal.sh TOGS): every untraced open of root N's side SIDE in time order, its toggle (the .tX in its log's
name), GPU busy ms a frame 0.1-0.7 s after the first swap (gpuwin.py's measure) and swaps/s; then per toggle the median, and each
open of toggle X (X != base) against the mean of the nearest base opens before and after it (paired: drift in CPU pacing or heat
between the opens cancels). Usage: abab.py rootN SIDE [BASE=0]"""
import glob, re, statistics, sys


def union(iv):
    t = 0; cur = None
    for s, e in sorted(iv):
        if cur and s <= cur[1]:
            cur = (cur[0], max(cur[1], e))
        else:
            if cur:
                t += cur[1] - cur[0]
            cur = (s, e)
    return t + (cur[1] - cur[0] if cur else 0)


def measure(T):
    g = re.search(r'GESTURE AT t=([0-9.]+)s', T)
    if not g:
        return None
    g = float(g[1]); sw = sorted({float(x) for x in re.findall(r'SWAPS: W\d+ t=([0-9.]+)s', T)})
    a = [x for x in sw if x >= g]
    if not a or sw[0] >= g:
        return None
    w0, w1 = a[0] + 0.1, a[0] + 0.7
    nf = sum(1 for x in sw if w0 <= x < w1)
    J = [(int(m[2]) / 19.2, int(m[3]) / 19.2, int(m[4])) for m in re.finditer(r'HOUJI GTL (\d+) (\d+) (\d+) (\d+) (\d+) (\d+)', T)
         if int(m[2]) and int(m[3]) >= int(m[2])]
    if len(J) < 20 or not nf:
        return None
    off = min(s - sub for s, e, sub in J)
    iv = [(max(s - off, w0 * 1e6), min(e - off, w1 * 1e6)) for s, e, sub in J if e - off > w0 * 1e6 and s - off < w1 * 1e6]
    return union(iv) / 1000 / nf, nf / 0.6


N, side = sys.argv[1], sys.argv[2]
base = sys.argv[3] if len(sys.argv) > 3 else '0'
rows = []
for f in glob.glob(f'logs/{N}-ut-{side}.t*-*.log'):
    m = re.search(rf'-ut-{re.escape(side)}\.t([0-9a-f])-(\d+)-(\d+)\.log$', f)
    T = open(f, 'rb').read().decode('latin-1')
    r = measure(T)
    if m and r:
        rows.append((int(m[3]), m[1], int(m[2]), *r))
rows.sort()
print(f'{N} {side}: {len(rows)} opens (time order)  tog  ms/frame  swaps/s')
for t, tg, k, ms, sps in rows:
    print(f'   open {k:2d}  t{tg}  {ms:6.2f}  {sps:5.0f}')
for tg in sorted({r[1] for r in rows}):
    v = [r[3] for r in rows if r[1] == tg]; s = [r[4] for r in rows if r[1] == tg]
    print(f'  t{tg}: n {len(v)}  median {statistics.median(v):.2f} ms  (mean {statistics.mean(v):.2f})  swaps {statistics.median(s):.0f}/s')
for tg in sorted({r[1] for r in rows} - {base}):
    d = []
    for i, r in enumerate(rows):
        if r[1] != tg:
            continue
        before = next((x for x in reversed(rows[:i]) if x[1] == base), None)
        after = next((x for x in rows[i + 1:] if x[1] == base), None)
        nb = [x[3] for x in (before, after) if x]
        if nb:
            d.append(r[3] - statistics.mean(nb))
    if d:
        sd = statistics.stdev(d) if len(d) > 1 else 0
        print(f'  t{tg} - t{base} paired: n {len(d)}  median {statistics.median(d):+.2f} ms  mean {statistics.mean(d):+.2f}'
              f' +- {sd / len(d) ** 0.5:.2f} (s.e.)  negative in {sum(x < 0 for x in d)}/{len(d)}')
