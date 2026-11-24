#!/usr/bin/env python3
"""What one mip-pyramid level costs on the GPU (fs df543221, traced opens): its pass, the copy before it, the primaries around it.
For each df543221 pass: the primary holding it (its duration, entries inside: passes, copies), the primary before it and the gap
between them, and the span from the end of the primary before the copy's (or the pass's own primary where the copy is inside it)
to the end of the pass's primary. Usage: pyrcost.py TL.log..."""
import bisect, collections, re, statistics, struct, sys
import tracebytes
R = collections.defaultdict(list)
for f in sys.argv[1:]:
    L = open(f, 'rb').read().decode('latin-1').splitlines()
    ch = tracebytes.chunks(L)
    if not ch:
        continue
    b0 = min(ch); data = bytearray(max(o + len(b) for o, b in ch.items()) - b0)
    for o, b in ch.items():
        data[o - b0:o - b0 + len(b)] = b
    E = []; det = {}
    for o in range((-b0) % 72, len(data) - 71, 72):
        t, idx, t0, t1, t2, t3, a, b, c, d, tid, c0, c1, tid2 = struct.unpack_from('<IIQQQQIIIIIIII', data, o)
        if t == 7 and t1 and t3 >= t2 > 0:
            E.append((t2, t3, b & 0xff, (b >> 8) & 0xff, t1, a & 0xffff, a >> 16))
        elif t == 13:
            det[t1] = c0
    E.sort()
    cbs = [e for e in E if e[2] == 7]; heads = [e for e in E if e[2] == 8]; inner = [e for e in E if e[2] not in (7, 8)]
    st = [e[0] for e in inner]; cbst = [c[0] for c in cbs]
    def content(cb):
        i = bisect.bisect_left(st, cb[0]); m = []
        while i < len(inner) and inner[i][0] < cb[1]:
            m.append(inner[i]); i += 1
        return m
    for p in inner:
        if p[2] > 2 or det.get(p[4]) != 0xdf543221:
            continue
        j = bisect.bisect_right(cbst, p[0]) - 1
        if j < 1 or not (cbs[j][0] <= p[0] <= cbs[j][1]):
            continue
        cb, prev = cbs[j], cbs[j - 1]
        m, mp = content(cb), content(prev)
        copies_in = [x for x in m if x[2] == 4]
        passes_in = [x for x in m if x[2] <= 2]
        prev_is_copy = mp and all(x[2] in (4, 5, 9) for x in mp) and any(x[2] == 4 for x in mp)
        start = cbs[j - 2][1] if prev_is_copy and j >= 2 else prev[1]
        R['pass us'].append(p[1] - p[0])
        R['its primary us'].append(cb[1] - cb[0])
        R['copies inside its primary'].append(len(copies_in))
        R['copy us inside'].append(sum(x[1] - x[0] for x in copies_in))
        R['passes in its primary'].append(len(passes_in))
        R['copy in a primary before (T)'].append(1 if prev_is_copy else 0)
        R['span: previous work end -> its primary end, us'].append(cb[1] - start)
        R['primary time not in pass or copy, us'].append(cb[1] - cb[0] - (p[1] - p[0]) - sum(x[1] - x[0] for x in copies_in))
for k, v in R.items():
    print(f'  {k:48s} n {len(v):5d}  median {statistics.median(v):7.1f}  mean {statistics.mean(v):7.1f}')
