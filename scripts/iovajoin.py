#!/usr/bin/env python3
"""The CP's switch between two primaries against where their IBs lie (gpu48, gtr=1): m2v/cbiova.bin (turnip-houji-diag.patch
tu_houji_note_iova, 32-byte records: the primary's pass ring entry, its IB entry count, its first entry's IOVA, its last entry's end,
the two entries' BO handles) joined with the traced opens' pass ring (mode 7 primaries, mode 8 heads). For each back-to-back pair
(the next head within 60 us of the previous primary's end): the switch (previous end -> next head, RB stamps), by the pair's kinds
and the jump from the previous primary's last entry to the next one's first: the same BO or another, the same 4 KiB page, the same
64 KiB, and its distance. Usage: iovajoin.py CBIOVA.bin TL.log..."""
import bisect, collections, statistics, struct, sys
import tracebytes

rec = {}
d = open(sys.argv[1], 'rb').read()
for o in range(0, len(d) - 31, 32):
    seq, n, first, last_end, fbo, lbo = struct.unpack_from('<IIQQII', d, o)
    if seq and first:
        rec[seq] = (n, first, last_end, fbo, lbo)
R = collections.defaultdict(list); joined = missed = 0
for f in sys.argv[2:]:
    L = open(f, 'rb').read().decode('latin-1').splitlines()
    ch = tracebytes.chunks(L)
    if not ch:
        continue
    b0 = min(ch); data = bytearray(max(o + len(b) for o, b in ch.items()) - b0)
    for o, b in ch.items():
        data[o - b0:o - b0 + len(b)] = b
    E = []
    for o in range((-b0) % 72, len(data) - 71, 72):
        t, idx, t0, t1, t2, t3, a, b, c, dd, tid, c0, c1, tid2 = struct.unpack_from('<IIQQQQIIIIIIII', data, o)
        if t == 7 and t1 and t3 >= t2 > 0:
            E.append((t2, t3, b & 0xff, t1))
    E.sort()
    cbs = [e for e in E if e[2] == 7]; heads = [e for e in E if e[2] == 8]
    inner = [e for e in E if e[2] not in (7, 8)]; st = [e[0] for e in inner]; hs = [h[0] for h in heads]
    tog = f.split('.t')[-1].split('-')[0] if '.t' in f else '-'

    def kind(cb):
        i = bisect.bisect_left(st, cb[0]); m = []
        while i < len(inner) and inner[i][0] < cb[1]:
            m.append(inner[i][2]); i += 1
        return 'P' if any(x <= 2 for x in m) else 'C' if 3 in m else 'T' if 4 in m else 'f' if 5 in m else 'e'
    for p, n in zip(cbs, cbs[1:]):
        j = bisect.bisect_right(hs, n[0]) - 1
        if j < 0 or heads[j][0] < p[1] or not 0 <= n[0] - p[1] < 60:
            continue
        a, b = rec.get(p[3]), rec.get(n[3])
        if not a or not b:
            missed += 1
            continue
        joined += 1
        gap = heads[j][0] - p[1]
        same_bo = a[4] == b[3]
        frm, to = a[2] - 4, b[1]
        rel = ('same BO' if same_bo else 'other BO') + (', same 4K' if frm >> 12 == to >> 12 else ', same 64K' if frm >> 16 == to >> 16 else ', far')
        R[(tog, kind(p) + kind(n), rel)].append((gap, abs(to - frm), a[0], b[0]))
print(f'{len(rec)} IB records; {joined} back-to-back pairs joined, {missed} without a record')
print('toggle pair jump: n | switch median/mean us | jump median bytes | entries prev/next median')
for k, v in sorted(R.items(), key=lambda kv: (kv[0][0], -len(kv[1]))):
    if len(v) < 20:
        continue
    g = [x[0] for x in v]
    print(f'  t{k[0]:2s} {k[1]} {k[2]:20s} {len(v):5d} | {statistics.median(g):5.1f} {statistics.mean(g):6.2f} | '
          f'{statistics.median(x[1] for x in v):10.0f} | {statistics.median(x[2] for x in v):.0f}/{statistics.median(x[3] for x in v):.0f}')
