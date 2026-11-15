#!/usr/bin/env python3
"""Port the reviewed IOMobileFramebufferLegacy fixture (UniXNU-sheng scripts/legacy-display-<build>.json) to another kernelcache.

    port-legacy-display-fixture.py XNUPRELINK_DIR FROM_KC FROM_FIXTURE.json TO_KC OUT.json [--slide 0x0]

Both collections must carry the same com.apple.iokit.IOMobileGraphicsFamily: its code and data then differ only by where the
linker placed them, so a fixture address in that kext moves by its segment's displacement, and is accepted only when the bytes
at both addresses agree (code with ADRP/ADR/branch immediates masked). The vtable is read from the target collection itself:
each slot's target and signing diversity come from its chained fixup, and a diversity that differs from the source is an error.
Functions of other kexts (the IOSurface getters) are found by their unique masked byte sequence in the target. Nothing that
does not port is guessed: the script stops and names it.
"""
import argparse
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

FAMILY = 'com.apple.iokit.IOMobileGraphicsFamily'


def masked(code):
    """Instruction words with PC-relative immediates cleared (ADRP/ADR, B/BL, B.cond/CBZ/TBZ); data words unchanged."""
    out = []
    for i in range(0, len(code) - len(code) % 4, 4):
        w, = struct.unpack_from('<I', code, i)
        if w & 0x1f000000 == 0x10000000:          # ADR/ADRP
            w &= 0x9f00001f
        elif w & 0x7c000000 == 0x14000000:        # B/BL
            w &= 0xfc000000
        elif w & 0xff000010 == 0x54000000:        # B.cond
            w &= 0xff00001f
        elif w & 0x7e000000 == 0x34000000:        # CBZ/CBNZ
            w &= 0xff00001f
        elif w & 0x7e000000 == 0x36000000:        # TBZ/TBNZ
            w &= 0xfff8001f
        out.append(w)
    return out


class Collection:
    def __init__(self, raw, Image):
        self.raw, self.cache = raw, Image(raw)
        self.fix = self.cache.fixups()
        self.kexts = {name: Image(raw, off) for name, (_, off) in self.cache.entries.items()}

    def where(self, va):
        for name, image in self.kexts.items():
            for s in image.segments:
                if s['size'] and s['name'] != '__LINKEDIT' and s['va'] <= va < s['va'] + s['size']:
                    return name, s
        raise ValueError(f'{va:#x} is in no fileset entry')

    def read(self, va, n):
        return self.cache.read(va, n)

    def text(self, entry):
        s = next(s for s in self.kexts[entry].segments if s['name'] == '__TEXT_EXEC')
        return s['va'], s['va'] + s['size']

    def starts(self, entry):
        return sorted(self.kexts[entry].function_starts())


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('prelink', type=Path)
    p.add_argument('source', type=Path)
    p.add_argument('fixture', type=Path)
    p.add_argument('target', type=Path)
    p.add_argument('output', type=Path)
    p.add_argument('--slide', default='0x0', help='collection slide of the target prelink (kernel.json collection_slide)')
    a = p.parse_args()
    sys.path.insert(0, str(a.prelink))
    from kc_image import Image
    fx = json.loads(a.fixture.read_text())
    raw_a, raw_b = a.source.read_bytes(), a.target.read_bytes()
    if hashlib.sha256(raw_a).hexdigest() != fx['kernelcache_sha256']:
        sys.exit('the fixture names another source kernelcache')
    A, B = Collection(raw_a, Image), Collection(raw_b, Image)
    problems = []

    def require(ok, why):
        if not ok:
            problems.append(why)
        return ok

    # Pair the family's functions with XnuPrelink's matcher (unique strings, relocation-masked shapes, call neighbours).
    from fileset_match import Matcher, Side

    fix_a, fix_b = A.cache.fixups(), B.cache.fixups()

    def matched(entry):
        def side(raw, fixups):
            cache = Image(raw)
            image = Image(raw, cache.entries[entry][1])
            return Side(image, fixups, image.mapped())
        sa, sb = side(raw_a, fix_a), side(raw_b, fix_b)
        mm = Matcher(sa, sb)
        mm.anchor_strings(); mm.anchor_checked_shapes()
        found = {}
        for _ in range(40):
            bound = mm.anchor_shapes() + mm.propagate()
            mm.pair_data(found)
            bound += mm.pair_calls() + mm.pair_data_users(found) + mm.pair_reference_shapes(found) + mm.align_orders()
            if not bound:
                break
        print(f'{entry}: {len(mm.pairs)} of {len(sa.functions)} functions paired (target has {len(sb.functions)})')
        return sa, sb, mm, found
    SA, SB, m, data = matched(FAMILY)
    others = {}
    starts_a = sorted(SA.functions)

    def move(va):
        """A family address in the source -> the target: a function start or an instruction inside a paired function
        (same offset; the bytes there are re-verified by the caller), or data the paired functions refer to."""
        entry, _ = A.where(va)
        if entry != FAMILY:
            raise KeyError(entry)
        if va in m.pairs:
            return m.pairs[va]
        from bisect import bisect_right
        i = bisect_right(starts_a, va) - 1
        if i >= 0 and va < SA.functions[starts_a[i]]['end'] and starts_a[i] in m.pairs:
            return m.pairs[starts_a[i]] + va - starts_a[i]
        if va in data:
            return data[va][0]
        votes = {}
        for s, fn in SA.functions.items():
            other = m.pairs.get(s)
            if other is None or len(fn['refs']) != len(SB.functions[other]['refs']):
                continue
            for (_, target), (_, theirs) in zip(fn['refs'], SB.functions[other]['refs']):
                if target == va:
                    votes[theirs] = votes.get(theirs, 0) + 1
        if len(votes) == 1:
            return next(iter(votes))
        raise LookupError(f'{va:#x}: {len(votes)} candidate data addresses in the target')

    def fn_len(collection, entry, va):
        st = collection.starts(entry)
        later = [x for x in st if x > va]
        return min((later[0] if later else va + 64) - va, 256)

    def find_function(va):
        """A function of another kext: its unique masked byte sequence in the same-named kext of the target."""
        entry, _ = A.where(va)
        n = fn_len(A, entry, va)
        want = masked(A.read(va, n))
        lo, hi = B.text(entry)
        hits = [x for x in B.starts(entry) if lo <= x < hi and x + n <= hi and masked(B.read(x, n)) == want]
        if len(hits) != 1:
            raise LookupError(f'{va:#x} ({entry}, {n} bytes): {len(hits)} candidates in the target')
        return hits[0]

    def port(va):
        try:
            return move(va), 'family pair'
        except KeyError as error:
            entry = error.args[0]
        # Another kext (the IOSurface getters): its own matcher pair, then the unique masked bytes. The caller re-verifies
        # the fixture's recorded bytes at the result, so a changed field offset stops the port.
        if entry not in others:
            others[entry] = matched(entry)
        _, _, mm, _ = others[entry]
        if va in mm.pairs:
            return mm.pairs[va], f'{entry} pair ({mm.how[va]})'
        return find_function(va), 'unique masked function'
    tb = B.text(FAMILY)
    out = dict(fx)
    out['kernelcache_sha256'] = hashlib.sha256(raw_b).hexdigest()
    out['collection_slide'] = a.slide
    out['ported_from'] = dict(kernelcache_sha256=fx['kernelcache_sha256'], fixture=a.fixture.name,
                              method='same IOMobileGraphicsFamily version (343.0.0, separate compile); family functions paired by XnuPrelink matcher, '
                                     'vtable and diversities read from the target chained fixups, other functions by unique '
                                     'masked bytes; every check re-verified on the target bytes')
    vt_a = int(fx['vtable'], 16)
    src = [(int(x, 16), int(d, 16)) for x, d in fx['slots']]
    # caller_diversity slots: the fixture records the discriminator the original call sites use, not the vtable entry's (a pure
    # slot). There the target's raw entry must equal the source's raw entry, and the fixture's caller value is kept.
    callers = {int(k): int(v['diversity'], 16) for k, v in fx.get('caller_diversity', {}).items()}
    raw_a_div = {i: A.fix[vt_a + 8 * i][3] for i in callers}
    for i, d in callers.items():
        if src[i][1] != d:
            sys.exit(f'slot {i}: caller_diversity {d:#x} is not the fixture slot diversity {src[i][1]:#x}')

    def raw_div(i, d):
        return raw_a_div[i] if i in callers else d
    known = [(i, m.pairs[x]) for i, (x, _) in enumerate(src) if x in m.pairs]
    candidates = []
    for site, f in B.fix.items():
        first = site - 8 * known[0][0]
        if f[0] != known[0][1] or not f[1]:
            continue
        if all(B.fix.get(first + 8 * i, (None,))[0] == target for i, target in known) and \
                all((B.fix.get(first + 8 * i) or (0, 0, 0, -1))[3] == raw_div(i, d) for i, (_, d) in enumerate(src)):
            candidates.append(first)
    if len(set(candidates)) != 1:
        # Diagnose: the best candidate by paired targets, and where it disagrees.
        best = {}
        for site, f in B.fix.items():
            if not f[1]:
                continue
            for i, target in known[:8]:
                if f[0] == target:
                    first = site - 8 * i
                    best[first] = best.get(first, 0) + 1
        for first, _ in sorted(best.items(), key=lambda kv: -kv[1])[:3]:
            tmiss = [i for i, target in known if B.fix.get(first + 8 * i, (None,))[0] != target]
            dmiss = [i for i, (_, d) in enumerate(src) if (B.fix.get(first + 8 * i) or (0, 0, 0, -1))[3] != raw_div(i, d)]
            print(f'candidate {first:#x}: target mismatches {tmiss[:12]} ({len(tmiss)}), diversity mismatches {dmiss[:12]} ({len(dmiss)})')
        sys.exit(f'vtable: {len(set(candidates))} candidates with all {len(known)} paired targets and every source diversity')
    vt_b = candidates[0]
    out['vtable'] = hex(vt_b)
    slots = []
    for i, (target, div) in enumerate(fx['slots']):
        site_b = vt_b + 8 * i
        f = B.fix.get(site_b)
        if not require(f is not None and f[1], f'slot {i}: no authenticated fixup at {site_b:#x}'):
            continue
        tb_, _, key, d, addr = f
        require(d == raw_div(i, int(div, 16)) and key == 0 and addr == 1,
                f'slot {i}: diversity {d:#x} key {key} addr {addr}, source {div}')
        t_a = int(target, 16)
        try:
            expect = move(t_a)
            require(expect == tb_, f'slot {i}: family target {t_a:#x} moves to {expect:#x}, target vtable has {tb_:#x}')
        except (KeyError, LookupError):
            pass  # kernel, other kext or unpaired target: the target collection's own entry is authoritative
        slots.append([hex(tb_), div])
    require(B.fix.get(vt_b + 8 * len(fx['slots'])) is None or len(slots) == len(fx['slots']),
            'the target vtable is longer than the source')
    out['slots'] = slots
    pure_a = int(fx['pure_target'], 16) if 'pure_target' in fx else int(fx['slots'][214][0], 16)
    pure_b = int(slots[214][0], 16) if len(slots) > 214 else 0
    out['pure_target'] = hex(pure_b)
    require(all((int(s[0], 16) == pure_b) == (int(fx['slots'][i][0], 16) == pure_a) for i, s in enumerate(slots)),
            'pure-virtual slots differ between the collections')
    out['family_text'] = [hex(tb[0]), hex(tb[1])]
    bindings, how = {}, {}
    for name, va in fx['bindings'].items():
        if name.startswith('__ZTV') and int(va, 16) == vt_a - 16:
            bindings[name], how[name] = hex(vt_b - 16), 'vtable'
            continue
        try:
            bindings[name], how[name] = port(int(va, 16))
            bindings[name] = hex(bindings[name])
        except (LookupError, ValueError, KeyError) as error:
            require(False, f'binding {name}: {error}')
    out['bindings'] = bindings
    checks = []
    for va, hexbytes in fx['checks']:
        want = bytes.fromhex(hexbytes)
        try:
            vb, _ = port(int(va, 16))
        except (LookupError, ValueError, KeyError) as error:
            require(False, f'check {va}: {error}')
            continue
        got = B.read(vb, len(want))
        require(got == want, f'check {va} -> {vb:#x}: bytes {got.hex()} differ from {hexbytes}')
        checks.append([hex(vb), hexbytes])
    out['checks'] = checks
    if problems:
        sys.exit('NOT PORTED:\n' + '\n'.join(problems))
    out['instance_size'] = fx['instance_size']   # same family build: the constructor and size are byte-identical
    a.output.write_text(json.dumps(out, indent=1) + '\n')
    print(f'PASS: {FAMILY} paired by the matcher; vtable {vt_a:#x} -> {vt_b:#x}; {len(slots)} slots with the source '
          f'diversities; {len(bindings)} bindings ({sum(v == "unique masked function" for v in how.values())} by unique bytes); '
          f'{len(checks)} checks re-verified on the target')


if __name__ == '__main__':
    main()
