#!/usr/bin/env python3
"""Copy a verified windows-only SHNGROOT image and append its policy hash tables.

Offline only: never opens a block device or flashes the phone. The destination
must not exist. Signed headers and prior C verification reports are required;
this layout check does not replace cryptographic verification by VerifiedRoot.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

MIB = 1 << 20


def digest_region(handle, offset, size):
    digest = hashlib.sha256()
    handle.seek(offset)
    while size:
        data = handle.read(min(8 * MIB, size))
        if not data:
            raise ValueError('short image read')
        digest.update(data)
        size -= len(data)
    return digest.digest()


def build(source, output, policies, tables, capacity):
    if not source.is_file() or source.is_block_device():
        raise ValueError('source must be a regular image file')
    with source.open('rb') as handle:
        header = bytearray(handle.read(4096))
        if len(header) != 4096 or header[:8] != b'SHNGROOT':
            raise ValueError('invalid root manifest')
        if hashlib.sha256(header[:4064]).digest() != header[4064:]:
            raise ValueError('invalid manifest checksum')
        version, count, total = struct.unpack_from('<IIQ', header, 8)
        if version != 1 or count != 2 or total != source.stat().st_size or total % MIB:
            raise ValueError('expected aligned version-1 OS/SystemOS image')
        entries, objects, cursor = [], [], total
        end = MIB
        for index, name in enumerate(('OS', 'SystemOS')):
            raw, offset, size, block, flags, expected = struct.unpack_from('<32sQQII32s', header, 24 + index * 128)
            if (raw.rstrip(b'\0') != name.encode() or block != 512 or flags != 1 or
                    offset < end or offset % MIB or not size or offset + size > total):
                raise ValueError(f'{name}: invalid window bounds or flags')
            end = offset + size
            if digest_region(handle, offset, size) != expected:
                raise ValueError(f'{name}: image hash mismatch')
            policy = policies / name
            signed = (policy / 'manifest.header.bin').read_bytes()
            report = json.loads((policy / 'verification.json').read_text())
            table = tables / f'verified-root-{name}/hashes.bin'
            table_size = table.stat().st_size
            with table.open('rb') as hashes:
                table_digest = digest_region(hashes, 0, table_size)
            if (len(signed) != 544 or signed[:8] != b'VROOT01\0' or
                    struct.unpack_from('<Q', signed, 24)[0] != size or
                    struct.unpack_from('<Q', signed, 32)[0] != (size + 4095) // 4096 or
                    table_size != ((size + 4095) // 4096) * 32 or
                    signed[56:88] != expected or signed[88:120] != table_digest or
                    report.get('real_c_signature_and_block_verification') is not True or
                    report['image_sha256'] != expected.hex()):
                raise ValueError(f'{name}: policy/table/image identity mismatch')
            entries.append(dict(name=name, offset=offset, size=size, sha256=expected.hex()))
            objects.append(dict(name=name + '.vroot', offset=cursor, size=table_size, sha256=table_digest.hex()))
            cursor += (table_size + MIB - 1) // MIB * MIB
        if cursor > capacity:
            raise ValueError('image exceeds userdata capacity')
    # Reserve exclusively before copying. Do not overwrite existing local work.
    with output.open('xb'):
        pass
    subprocess.run(['cp', '--reflink=auto', '--sparse=always', str(source), str(output)], check=True)
    struct.pack_into('<IIQ', header, 8, 2, 4, cursor)
    with output.open('r+b') as target:
        target.truncate(cursor)
        for index, obj in enumerate(objects, 2):
            table = tables / ('verified-root-' + obj['name'].removesuffix('.vroot')) / 'hashes.bin'
            target.seek(obj['offset'])
            with table.open('rb') as handle:
                while data := handle.read(8 * MIB):
                    target.write(data)
            struct.pack_into('<32sQQII32s40s', header, 24 + index * 128,
                             obj['name'].encode(), obj['offset'], obj['size'], 4096, 3,
                             bytes.fromhex(obj['sha256']), bytes(40))
        header[4064:] = hashlib.sha256(header[:4064]).digest()
        target.seek(0)
        target.write(header)
        target.flush()
        for obj in objects:
            if digest_region(target, obj['offset'], obj['size']).hex() != obj['sha256']:
                raise ValueError('written table verification failed')
    report = dict(image_bytes=cursor, userdata_bytes=capacity, windows=entries, objects=objects,
                  manifest_sha256=hashlib.sha256(header).hexdigest(), hardware_tested=False)
    output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS: both OS windows match their verified policy image hashes')
    for obj in objects:
        print(f'PASS: {obj["name"]} {obj["size"]} bytes at {obj["offset"]:#x}; signed table digest matches')
    print(f'PASS: SHNGROOT v2, 4 entries, {cursor} bytes <= {capacity}; offline only')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--policies', type=Path, required=True)
    parser.add_argument('--tables', type=Path, required=True)
    parser.add_argument('--capacity', type=int, required=True)
    args = parser.parse_args()
    build(args.source, args.output, args.policies, args.tables, args.capacity)
