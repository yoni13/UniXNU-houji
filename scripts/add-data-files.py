#!/usr/bin/env python3
"""Add files to an existing packed Data seed (make-data-seed-linux.py output) without changing its volume.

  add-data-files.py --unixnu UNIXNU --seed data.dbr --output new.dbr SRC:DEST [SRC:DEST ...]

DEST is relative to the Data volume (mounted at /private/var). The seed is unpacked to a sparse raw image, mounted read-write in the
same privileged container as make-data-seed-linux.py (linux-apfs-rw), the files are copied (owner root, mode 0755 when the source
is executable and 0644 otherwise, parent directories 0755), and the image is packed again into OUTPUT. The input seed is not modified, so its volume UUID (CONFIG_XNU_OWNER_DATA_VOLUME_UUID)
stays the same. The output is installation state: keep it local, never commit it.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

INNER = r'''
import os, stat, subprocess, sys, shutil
from pathlib import Path
seed, pairs = sys.argv[1], sys.argv[2:]
for index in range(8):
    if not os.path.exists(f'/dev/loop{index}'):
        os.mknod(f'/dev/loop{index}', 0o660 | stat.S_IFBLK, os.makedev(7, index))
dev = subprocess.check_output(['losetup', '--find', '--show', seed], text=True).strip()
mount = Path('/mnt/seed'); mount.mkdir(parents=True, exist_ok=True)
try:
    subprocess.run(['mount', '-t', 'apfs', '-o', 'readwrite', dev, mount], check=True)
    if b' rw,' not in subprocess.check_output(['grep', ' /mnt/seed ', '/proc/mounts']):
        sys.exit('the Data volume did not mount read-write')
    for pair in pairs:
        src, dest = pair.split(':', 1)
        target = mount / dest
        for parent in reversed(target.relative_to(mount).parents[:-1]):
            d = mount / parent
            if not d.exists():
                d.mkdir(); os.chown(d, 0, 0); d.chmod(0o755)
        shutil.copyfile(src, target); os.chown(target, 0, 0); target.chmod(0o755 if os.stat(src).st_mode & 0o111 else 0o644)
        print('ADDED', dest, target.stat().st_size)
finally:
    subprocess.run(['umount', mount]); subprocess.run(['losetup', '-d', dev])
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--unixnu', type=Path, required=True)
    parser.add_argument('--seed', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--image', default='unixnu-build-seed')
    parser.add_argument('files', nargs='+')
    args = parser.parse_args()
    if args.output.exists():
        parser.error('refuse to overwrite ' + str(args.output))
    sys.path.insert(0, str(args.unixnu / 'drivers/DarwinBoot/Test'))
    import packed_ramdisk
    blob = args.seed.read_bytes()
    logical, extents = packed_ramdisk.regions(blob)
    pairs = []
    for item in args.files:
        src, _, dest = item.partition(':')
        if not dest or dest.startswith('/') or '..' in Path(dest).parts or not Path(src).is_file():
            parser.error('bad SRC:DEST ' + item)
        pairs.append((Path(src).resolve(), dest))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='add-data-', dir=args.output.parent) as td:
        work = Path(td)
        raw = work / 'seed.raw'
        with raw.open('wb') as f:
            f.truncate(logical)
            for offset, data in extents:
                f.seek(offset); f.write(data)
        for index, (src, _) in enumerate(pairs):
            (work / f'in{index}').write_bytes(src.read_bytes())
            (work / f'in{index}').chmod(src.stat().st_mode & 0o777)
        (work / 'inner.py').write_text(INNER)
        subprocess.run(['docker', 'run', '--rm', '--privileged', '-v', f'{work}:/w', args.image, 'python3', '/w/inner.py', '/w/seed.raw',
                        *[f'/w/in{i}:{dest}' for i, (_, dest) in enumerate(pairs)]], check=True, timeout=600)
        candidate = work / 'data.dbr'
        packed_ramdisk.pack(raw, candidate)
        candidate.replace(args.output)
    print(json.dumps(dict(output=str(args.output), added={dest: hashlib.sha256(src.read_bytes()).hexdigest() for src, dest in pairs})))


if __name__ == '__main__':
    main()
