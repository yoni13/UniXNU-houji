#!/usr/bin/env python3
"""gpu42 gtr=1 diagnostics from traced opens (150-800 ms after the gesture), turnip-houji-diag.patch's ring entries:
- mode 8, each primary's head (tu_init_hw): the gap between two primaries split into the jump (previous end stamp -> head begin), the
  head itself and the head's end -> the primary's begin stamp; by the pair's kinds (P pass, T transfer, f flushes only, C compute).
- mode 9, each vkCmdCopyImage's images: in the transfer-only primaries (T), by source/destination tiling and external memory,
  the first region's extent, the source's extent, usage and format.
Usage: diagan.py TL.log..."""
import bisect, collections, re, statistics, struct, sys
import tracebytes

USAGE = ['TRANSFER_SRC', 'TRANSFER_DST', 'SAMPLED', 'STORAGE', 'COLOR_ATT', 'DS_ATT', 'TRANSIENT', 'INPUT_ATT']


def usage(u):
    return '|'.join(n for i, n in enumerate(USAGE) if u >> i & 1) + (f'|{u & ~0xff:#x}' if u & ~0xff else '')


import os
W0, W1 = int(os.environ.get("W0", 150000)), int(os.environ.get("W1", 800000))
frames = 0
gaps = collections.defaultdict(lambda: ([], [], []))
copies = collections.Counter(); copy_us = collections.defaultdict(list); where = collections.Counter(); heads_seen = 0
for f in sys.argv[1:]:
    L = open(f, 'rb').read().decode('latin-1').splitlines()
    gm = [float(m[1]) * 1e6 for m in (re.search(r'GESTURE AT t=([0-9.]+)s', l) for l in L) if m]
    if not gm:
        continue
    g = gm[0]
    sw = sorted({float(m[1]) * 1e6 for m in (re.search(r'SWAPS: W\d+ t=([0-9.]+)s', l) for l in L) if m})
    frames += sum(1 for x in sw if g + W0 <= x < g + W1)
    ch = tracebytes.chunks(L)
    if not ch:
        continue
    b0 = min(ch); data = bytearray(max(o + len(b) for o, b in ch.items()) - b0)
    for o, b in ch.items():
        data[o - b0:o - b0 + len(b)] = b
    E = []; det = {}
    for o in range((-b0) % 72, len(data) - 71, 72):
        t, idx, t0, t1, t2, t3, a, b, c, d, tid, c0, c1, tid2 = struct.unpack_from('<IIQQQQIIIIIIII', data, o)
        if t == 7 and t1 and g + W0 <= t0 < g + W1 and t3 >= t2 > 0:
            E.append(dict(b=t2, e=t3, mode=b & 0xff, kind=(b >> 8) & 0xff, w=a & 0xffff, h=a >> 16, z=c & 0xffff, seq=t1, rec=(a, b, c, d)))
        elif t == 13:
            det[t1] = (d, c0)   # bandwidth (here the source's usage), hash (here the destination's usage)
    E.sort(key=lambda x: x['b'])
    cbs = [x for x in E if x['mode'] == 7]; heads = [x for x in E if x['mode'] == 8]; heads_seen += len(heads)
    inner = [x for x in E if x['mode'] not in (7, 8)]; starts = [x['b'] for x in inner]; hstarts = [x['b'] for x in heads]
    seq = []
    for cb in cbs:
        i = bisect.bisect_left(starts, cb['b']); got = []
        while i < len(inner) and inner[i]['b'] < cb['e']:
            got.append(inner[i]); i += 1
        modes = [x['mode'] for x in got]
        k = 'P' if any(m <= 2 for m in modes) else 'C' if 3 in modes else 'T' if 4 in modes else 'f' if 5 in modes else 'e'
        seq.append((cb, k, got))
    for (c1_, k1, _), (c2, k2, _) in zip(seq, seq[1:]):
        gap = c2['b'] - c1_['e']
        if not 0 <= gap < 60:
            continue
        j = bisect.bisect_right(hstarts, c2['b']) - 1
        if j < 0 or heads[j]['b'] < c1_['e']:
            continue
        hd = heads[j]
        jump, head, after = hd['b'] - c1_['e'], hd['e'] - hd['b'], c2['b'] - hd['e']
        for lst, v in zip(gaps[k1 + k2], (jump, head, after)):
            lst.append(v)
    for cb, k, got in seq:
        for x in got:
            if x['mode'] != 9:
                continue
            a, b, c, d = x['rec']
            kind = x['kind']; su, du = det.get(x['seq'], (0, 0))
            # as gpuops.py reads the records: draws (the source's width) c & 0xffff, format c >> 16, kpx (the source's height) d;
            # the detail record (type 13) of the same sequence number: bandwidth (the source's usage) and hash (the destination's)
            key = (k, 'src ' + ('linear' if kind & 1 else 'tiled') + (' ext' if kind & 2 else ''),
                   'dst ' + ('linear' if kind & 4 else 'tiled') + (' ext' if kind & 8 else ''),
                   f'region {x["w"]}x{x["h"]}', f'src {x["z"]}x{d}', f'fmt {c >> 16}', 'src use ' + usage(su), 'dst use ' + usage(du))
            copies[key] += 1
for k, (j, h, a) in sorted(gaps.items(), key=lambda kv: -len(kv[1][0])):
    if not j:
        continue
    print(f'{k}: {len(j) / max(frames, 1):6.1f} a frame  jump {statistics.median(j):5.1f}  init_hw {statistics.median(h):5.1f}  '
          f'after {statistics.median(a):5.1f} us (medians; p90 init_hw {sorted(h)[9 * len(h) // 10]:.1f})')
print(f'{frames} frames, {heads_seen} head entries')
print('image copies by primary kind and images (a frame):')
for k, v in copies.most_common(16):
    print(f'  {v / max(frames, 1):6.1f}  {" | ".join(k)}')
