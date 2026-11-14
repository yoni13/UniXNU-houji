#!/usr/bin/env python3
"""Port XnuPrelink's reviewed fixture of one kernelcache to another build of the same kernel.

    port-kc-fixture.py XNUPRELINK_DIR FROM_KERNELCACHE FROM_FIXTURE.json TO_KERNELCACHE OUT_FIXTURE.json [--report FILE]

The fixture names functions and data of the original (stripped) kernel by address. The two kernels are paired function by
function with XnuPrelink's own matcher (unique strings, relocation-masked shapes, call neighbours); a fixture function is
carried over only when its pair exists, and a data address only when every paired function that refers to it agrees on the
address at the same instruction. Everything that does not port is listed, never guessed.
"""
import argparse
import hashlib
import json
import sys
from bisect import bisect_left, bisect_right
from collections import Counter, defaultdict
from pathlib import Path


def side_of(raw, Image, Side, entry='com.apple.kernel'):
    cache = Image(raw)
    image = Image(raw, cache.entries[entry][1])
    return cache, Side(image, cache.fixups(), image.mapped())


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('prelink', type=Path)
    p.add_argument('source', type=Path)
    p.add_argument('fixture', type=Path)
    p.add_argument('target', type=Path)
    p.add_argument('output', type=Path)
    p.add_argument('--report', type=Path)
    a = p.parse_args()
    sys.path.insert(0, str(a.prelink))
    from kc_image import Image
    from fileset_match import Matcher, Side

    raw_a, raw_b = a.source.read_bytes(), a.target.read_bytes()
    fixture = json.loads(a.fixture.read_text())
    if fixture['inputs']['kernelcache_sha256'] != hashlib.sha256(raw_a).hexdigest():
        sys.exit('the fixture names another kernelcache')
    cache_a, A = side_of(raw_a, Image, Side)
    cache_b, B = side_of(raw_b, Image, Side)
    m = Matcher(A, B)
    print('string anchors', m.anchor_strings(), 'checked shapes', m.anchor_checked_shapes())
    data = {}

    def gaps():
        """Between neighbouring pairs the linker keeps the source order: a gap with the same number of unpaired functions on
        both sides pairs them by position when each keeps its size within a quarter and no paired callee contradicts it."""
        bound = sorted(m.pairs)
        added = 0
        for left, right in zip(bound, bound[1:]):
            if m.pairs[right] <= m.pairs[left]:
                continue
            gap = [o for o in A.starts[bisect_right(A.starts, left):bisect_left(A.starts, right)]
                   if o in A.functions and o not in m.pairs]
            fill = [n for n in B.starts[bisect_right(B.starts, m.pairs[left]):bisect_left(B.starts, m.pairs[right])]
                    if n in B.functions and n not in m.reverse]
            if not gap or len(gap) != len(fill) or len(gap) > 8:
                continue
            ok = True
            for o, n in zip(gap, fill):
                lo, ln = A.functions[o]['length'], B.functions[n]['length']
                if min(lo, ln) * 4 < max(lo, ln) * 3:
                    ok = False
                want = {m.pairs[c] for c in A.callees[o] if c in m.pairs}
                have = {c for c in B.callees[n] if c in m.reverse}
                if want and have and not (want & have):
                    ok = False
            if ok:
                added += sum(m.bind(o, n, 'gap-order') for o, n in zip(gap, fill))
        return added

    for _ in range(40):
        bound = m.anchor_shapes() + m.propagate()
        m.pair_data(data)
        bound += m.pair_calls() + m.pair_data_users(data) + m.pair_reference_shapes(data) + m.align_orders()
        if not bound:
            bound = gaps()
        if not bound:
            break
    print(f'functions paired: {len(m.pairs)} of {len(A.functions)} (target has {len(B.functions)}); '
          f'by method {dict(Counter(m.how.values()))}; data paired {len(data)}')

    def data_pair(address):
        """The matcher's own answer first; otherwise the address paired users read at the same reference index, or, for a
        pointer slot, the slot in the target whose pointer leads to the paired object."""
        if address in data:
            return Counter({data[address][0]: 1})
        votes = Counter()
        for start, fn in A.functions.items():
            other = m.pairs.get(start)
            if other is None:
                continue
            left, right = fn, B.functions[other]
            if len(left['refs']) != len(right['refs']):
                continue
            for (_, target), (_, theirs) in zip(left['refs'], right['refs']):
                if target == address:
                    votes[theirs] += 1
        if not votes and address in A.fixups:
            pointee = A.fixups[address][0]
            other = m.pairs.get(pointee) or (data.get(pointee) or (None,))[0]
            if other is not None:
                for site, fix in B.fixups.items():
                    if fix[0] == other and fix[1:] == A.fixups[address][1:] and not B.in_exec(site) and B.in_mapped(site):
                        votes[site] += 1
        if len(votes) > 1:
            # Several slots lead to the same object (a class's metaClass pointer and the superclass links of its
            # subclasses): the fixture's address is one kexts refer to, so its counterpart is too.
            facing = Counter({k: v for k, v in votes.items() if k in kext_targets})
            if len(facing) == 1:
                return facing
        return votes

    inside_b = B.image.mapped()
    kext_targets = {fix[0] for site, fix in B.fixups.items()
                    if any(b <= fix[0] < e for b, e in inside_b) and not any(b <= site < e for b, e in inside_b)}

    out = {'inputs': {'kernelcache_sha256': hashlib.sha256(raw_b).hexdigest(), 'entry': fixture['inputs']['entry']},
           'ported_from': {'kernelcache_sha256': fixture['inputs']['kernelcache_sha256'], 'fixture': a.fixture.name},
           'functions': [], 'data': [], 'sites': []}
    failed = {'functions': [], 'data': [], 'sites': []}
    for entry in fixture['functions']:
        orig = int(entry['original'], 16)
        other = m.pairs.get(orig)
        if other is None:
            failed['functions'].append({**entry, 'reason': 'no pair'})
            continue
        fn = B.functions[other]
        same = A.functions[orig]['digest'] == fn['digest']
        out['functions'].append({
            'original': f'{other:#x}', 'original_size': fn['end'] - other,
            'original_sha256': hashlib.sha256(B.image.read(other, fn['end'] - other)).hexdigest(),
            'native_symbol': entry['native_symbol'],
            'evidence': entry['evidence'] + f" Ported from {entry['original']} ({m.how[orig]} pair, "
                        f"{'identical' if same else 'different'} masked instruction shape)."})
    for entry in fixture.get('data', []):
        orig = int(entry['original'], 16)
        votes = data_pair(orig)
        if len(votes) == 1:
            (other, count), = votes.items()
            out['data'].append({'original': f'{other:#x}', 'native_symbol': entry['native_symbol'],
                                'addend': entry.get('addend', 0),
                                'evidence': entry['evidence'] + f" Ported from {entry['original']}: {count} paired reference(s) agree."})
        else:
            failed['data'].append({**entry, 'reason': 'no paired user' if not votes else
                                   'users disagree: ' + ', '.join(f'{k:#x}x{v}' for k, v in votes.most_common(4))})
    # Objects of one table keep their spacing: an unported address whose ported neighbours (within 1 KiB, at least two)
    # all moved by the same distance moved by that distance too.
    moved = {int(e['original'], 16): None for e in fixture.get('data', [])}
    for entry in out['data']:
        source = int(entry['evidence'].split('Ported from ')[1].split(':')[0], 16)
        moved[source] = int(entry['original'], 16) - source
    still = []
    for entry in failed['data']:
        orig = int(entry['original'], 16)
        deltas = {d for s, d in moved.items() if d is not None and abs(s - orig) <= 0x400}
        count = sum(1 for s, d in moved.items() if d is not None and abs(s - orig) <= 0x400)
        if len(deltas) == 1 and count >= 2:
            delta, = deltas
            out['data'].append({'original': f'{orig + delta:#x}', 'native_symbol': entry['native_symbol'],
                                'addend': entry.get('addend', 0),
                                'evidence': entry['evidence'].split(' reason')[0] + f" Ported from {entry['original']}: "
                                            f"{count} ported neighbours within 1 KiB moved by the same distance."})
        else:
            still.append(entry)
    for entry in out['data']:
        entry.pop('reason', None)
    failed['data'] = still
    for entry in fixture.get('sites', []):
        failed['sites'].append({**entry, 'reason': 'sites are ported by hand'})
    a.output.write_text(json.dumps(out, indent=2) + '\n')
    print(f"ported: {len(out['functions'])}/{len(fixture['functions'])} functions, "
          f"{len(out['data'])}/{len(fixture.get('data', []))} data")
    for kind, entries in failed.items():
        for entry in entries:
            print(f"  NOT PORTED {kind} {entry.get('original', entry.get('site'))} {entry['native_symbol']}: {entry['reason']}")
    if a.report:
        pairs = {f'{k:#x}': f'{v:#x}' for k, v in m.pairs.items()}
        a.report.write_text(json.dumps({'pairs': pairs, 'how': {f'{k:#x}': v for k, v in m.how.items()}, 'failed': failed}) + '\n')


if __name__ == '__main__':
    main()
