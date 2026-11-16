#!/usr/bin/env python3
"""Port UniXNU's reviewed IOAcceleratorES base fixture (scripts/data/ioaccelerator-base-abi-24A446.json) to another kernelcache.

    port-accelerator-fixture.py XNUPRELINK_DIR FROM_KC FROM_FIXTURE.json TO_KC OUT.json [--slide 0x0]

The class lives in com.apple.iokit.IOGPUFamily. Its functions are paired with XnuPrelink's matcher as port-legacy-display-fixture.py
does; each ported function must have the same masked code in the target, and the target getMetaClass must load the ported
metaclass address. Nothing that does not port is guessed: the script stops and names it.
"""
import argparse
import hashlib
import importlib.util
import json
import struct
import sys
from pathlib import Path

FAMILY = 'com.apple.iokit.IOGPUFamily'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('prelink', 'source', 'fixture', 'target', 'output'):
        p.add_argument(name, type=Path)
    p.add_argument('--slide', default='0x0')
    a = p.parse_args()
    sys.path.insert(0, str(a.prelink))
    spec = importlib.util.spec_from_file_location('pldf', Path(__file__).with_name('port-legacy-display-fixture.py'))
    pldf = importlib.util.module_from_spec(spec); spec.loader.exec_module(pldf)
    from kc_image import Image
    from fileset_match import Matcher, Side
    fx = json.loads(a.fixture.read_text())
    raw_a, raw_b = a.source.read_bytes(), a.target.read_bytes()
    if hashlib.sha256(raw_a).hexdigest() != fx['kernelcache_sha256']:
        sys.exit('the fixture names another source kernelcache')
    A, B = pldf.Collection(raw_a, Image), pldf.Collection(raw_b, Image)
    fix_a, fix_b = A.cache.fixups(), B.cache.fixups()

    def side(raw, fixups):
        cache = Image(raw)
        image = Image(raw, cache.entries[FAMILY][1])
        return Side(image, fixups, image.mapped())
    SA, SB = side(raw_a, fix_a), side(raw_b, fix_b)
    m = Matcher(SA, SB)
    m.anchor_strings(); m.anchor_checked_shapes()
    found = {}
    for _ in range(40):
        bound = m.anchor_shapes() + m.propagate()
        m.pair_data(found)
        bound += m.pair_calls() + m.pair_data_users(found) + m.pair_reference_shapes(found) + m.align_orders()
        if not bound:
            break
    print(f'{FAMILY}: {len(m.pairs)} of {len(SA.functions)} functions paired (target has {len(SB.functions)})')
    problems = []

    def masked(code):
        # port-legacy-display-fixture.py's mask, plus the page offsets that follow an ADRP: the 12-bit immediates of ADD (immediate)
        # and of LDR/STR (unsigned offset). Every other bit (registers, the PAC discriminator MOVK, sizes) must still match.
        out = []
        for w in pldf.masked(code):
            if w & 0x7f800000 == 0x11000000 or w & 0x3b000000 == 0x39000000:
                w &= ~(0xfff << 10)
            out.append(w)
        return out

    def function(va):
        va = int(va, 16)
        if va not in m.pairs:
            problems.append(f'{va:#x}: no matcher pair')
            return None
        t = m.pairs[va]
        n = min(SA.functions[va]['end'] - va, 256)
        if masked(A.read(va, n)) != masked(B.read(t, n)):
            problems.append(f'{va:#x} -> {t:#x}: masked code differs')
        return t

    ctors = [function(v) for v in fx['protected_compatible_constructor_aliases']]
    dtor = function(fx['nondeleting_destructor'])
    getmc = function(fx['get_meta_class'])
    # The metaclass: what the paired functions load, agreed by every paired reference.
    meta_a = int(fx['metaclass'], 16)
    votes = {}
    for s, fn in SA.functions.items():
        other = m.pairs.get(s)
        if other is None or len(fn['refs']) != len(SB.functions[other]['refs']):
            continue
        for (_, target), (_, theirs) in zip(fn['refs'], SB.functions[other]['refs']):
            if target == meta_a:
                votes[theirs] = votes.get(theirs, 0) + 1
    if len(votes) != 1:
        problems.append(f'metaclass {meta_a:#x}: {len(votes)} candidate addresses {sorted(map(hex, votes))}')
        meta_b = None
    else:
        meta_b = next(iter(votes))
    # getMetaClass is ADRP + ADD (+ RET): the address it returns must be the ported metaclass.
    if getmc and meta_b:
        at = getmc + 4 if struct.unpack('<I', B.read(getmc, 4))[0] == 0xd503245f else getmc   # BTI c
        w0, w1 = struct.unpack('<2I', B.read(at, 8))
        imm = ((w0 >> 5) & 0x7ffff) << 2 | ((w0 >> 29) & 3)
        if imm & (1 << 20):
            imm -= 1 << 21
        page = (at & ~0xfff) + (imm << 12)
        loaded = page + ((w1 >> 10) & 0xfff) if (w1 & 0xff800000) == 0x91000000 else None
        if w0 & 0x9f000000 != 0x90000000 or loaded != meta_b:
            problems.append(f'getMetaClass {getmc:#x} does not load the ported metaclass ({loaded and hex(loaded)} vs {meta_b:#x})')
    if problems:
        sys.exit('NOT PORTED:\n  ' + '\n  '.join(problems))
    out = dict(fx)
    out.update(kernelcache_sha256=hashlib.sha256(raw_b).hexdigest(), collection_slide=a.slide, metaclass=hex(meta_b),
               protected_compatible_constructor_aliases=[hex(c) for c in ctors], nondeleting_destructor=hex(dtor),
               get_meta_class=hex(getmc),
               ported_from=dict(kernelcache_sha256=fx['kernelcache_sha256'], fixture=a.fixture.name,
                                method='IOGPUFamily functions paired by the XnuPrelink matcher with identical masked code; the metaclass is the '
                                       'one address every paired reference agrees on and the target getMetaClass loads; instance size, '
                                       'vtable entry count and overrides carried from the reviewed source fixture'))
    a.output.write_text(json.dumps(out, indent=2) + '\n')
    print(f'PORTED: metaclass {meta_b:#x}, constructors {[hex(c) for c in ctors]}, destructor {dtor:#x}, getMetaClass {getmc:#x}')


if __name__ == '__main__':
    main()
