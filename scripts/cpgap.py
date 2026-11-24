#!/usr/bin/env python3
"""The gap between two primaries split by CP time (turnip-houji-diag.patch's cpstamps.bin, CP_REG_TO_MEM of CP_ALWAYS_ON_COUNTER when
the CP reaches the packet) against the pass ring's RB_DONE_TS stamps (frames.trace): for each pair of consecutive primaries,
  rb_gap   the next head's RB stamp less the previous end's RB stamp (what diagan.py calls jump)
  cp_end   the previous primary's CP end less its RB end (the CP reached its last packet before or after the RB drained)
  cp_gap   the next head's CP time less the previous end's CP time (the CP between the two command buffers: the IB switch)
  rb_lag   the next head's RB stamp less its CP time (the RB draining before the head's stamp)
by the pair's kinds and whether both came from one command pool. Usage: cpgap.py CPSTAMPS.bin TL.log..."""
import bisect, collections, re, statistics, struct, sys
import tracebytes

cp = {}   # (kind, pass ring seq) -> (ticks, pool)
d = open(sys.argv[1], 'rb').read()
for o in range(0, len(d) - 15, 16):
    ticks, seq, tag = struct.unpack_from('<QII', d, o)
    if ticks and seq:
        cp[(tag >> 28, seq)] = (ticks * 52 / 1000.0, tag & 0x0fffffff)   # us, as tu_device_ticks_to_ns / 1000
rows = collections.defaultdict(list)
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
    cbs = [e for e in E if e[2] == 7]; heads = [e for e in E if e[2] == 8]; inner = [e for e in E if e[2] not in (7, 8)]
    hs = [h[0] for h in heads]; st = [e[0] for e in inner]
    def kind(cb):
        i = bisect.bisect_left(st, cb[0]); m = []
        while i < len(inner) and inner[i][0] < cb[1]:
            m.append(inner[i][2]); i += 1
        return 'P' if any(x <= 2 for x in m) else 'C' if 3 in m else 'T' if 4 in m else 'f' if 5 in m else 'e'
    seq = [(cb, kind(cb)) for cb in cbs]
    for (p, k1), (n, k2) in zip(seq, seq[1:]):
        gap = n[0] - p[1]
        if not 0 <= gap < 60:
            continue
        j = bisect.bisect_right(hs, n[0]) - 1
        if j < 0 or heads[j][0] < p[1]:
            continue
        h = heads[j]
        pe, hd = cp.get((2, p[3])), cp.get((1, h[3]))
        if not pe or not hd:
            continue
        rows[(k1 + k2, 'same pool' if pe[1] == hd[1] else 'other pool')].append(
            (h[0] - p[1], pe[0] - p[1], hd[0] - pe[0], h[0] - hd[0]))
print(f'{len(cp)} CP stamps joined over {sum(len(v) for v in rows.values())} boundaries')
print('pair, pools: n | medians us: rb_gap  cp_end  cp_gap  rb_lag   (p90 cp_gap, rb_lag)')
for k, v in sorted(rows.items(), key=lambda kv: -len(kv[1])):
    med = [statistics.median(x[i] for x in v) for i in range(4)]
    p90 = lambda i: sorted(x[i] for x in v)[9 * len(v) // 10]
    print(f'  {k[0]} {k[1]:10s} {len(v):5d} | {med[0]:6.1f} {med[1]:7.1f} {med[2]:7.1f} {med[3]:7.1f}   ({p90(2):.1f}, {p90(3):.1f})')
