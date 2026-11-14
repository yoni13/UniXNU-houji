#!/usr/bin/env python3
"""Add functions to a ported fixture from the automatic map of the source kernelcache.

    add-twin-identities.py XNUPRELINK_DIR TO_KERNELCACHE FIXTURE.json PORT_REPORT.json SOURCE_MAP.json ADDRESS...

ADDRESS is an unresolved function of the target kernelcache. Its twin in the source kernelcache (port-kc-fixture.py --report)
must be named by the source's own prelink map (kernel.map.json of a prelink of the same native kernel against the source
kernelcache); the name is recorded with that provenance.
"""
import hashlib, json, sys
sys.path.insert(0, sys.argv[1])
from kc_image import Image
from fileset_match import Side
raw = open(sys.argv[2], 'rb').read()
cache = Image(raw); image = Image(raw, cache.entries['com.apple.kernel'][1])
side = Side(image, cache.fixups(), image.mapped())
fixture = json.load(open(sys.argv[3])); report = json.load(open(sys.argv[4])); source = json.load(open(sys.argv[5]))
twin = {v: k for k, v in report['pairs'].items()}
have = {e['original'] for e in fixture['functions']}
for address in sys.argv[6:]:
    start = int(address, 16); other = twin.get(address); entry = source['functions'].get(other) if other else None
    if address in have or entry is None or entry.get('addend') or start not in side.functions:
        print('skipped', address); continue
    end = side.functions[start]['end']
    fixture['functions'].append({
        'original': address, 'original_size': end - start,
        'original_sha256': hashlib.sha256(image.read(start, end - start)).hexdigest(), 'native_symbol': entry['name'],
        'evidence': f"Twin of {other} in kernelcache {source['inputs']['kernelcache_sha256'][:12]} ({report['how'][other]} pair), which "
                    f"the matcher names {entry['name']} there by {entry['how']}; here the symbol order differs and no automatic match exists."})
    print('added', address, entry['name'])
json.dump(fixture, open(sys.argv[3], 'w'), indent=2)
