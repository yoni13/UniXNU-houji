#!/usr/bin/env python3
"""Compile the houji board package (kits/houji) against this source kernel's own compile commands into one archive."""
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OBJ = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
OUT = ROOT / 'build/houji-kit'
KIT = ROOT / 'kits/houji'
OUT.mkdir(parents=True, exist_ok=True)
# A BSD-side C file and an IOKit C++ file whose flags the package sources reuse.
RECORDS = {'.c': OBJ / 'bsd/DEVELOPMENT/kern_exec.o.json', '.cpp': OBJ / 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'}
SOURCES = ['HoujiKernelHooks.c', 'HoujiDebugRing.c', 'HoujiUsbConsole.c', 'houji_ram_upload.c', 'HoujiStage0.c', 'HoujiSmmuRoute.cpp', 'HoujiRebootReason.cpp', 'HoujiTouchHid.c']


def command(record, source, obj):
    out, args, i = [], record['arguments'], 0
    while i < len(args):
        arg = args[i]
        if arg == record['file']:
            out.append(str(source))
        elif arg in ('-o', '-MF', '-MT', '-MJ'):
            if arg == '-o':
                out += ['-o', str(obj)]
            i += 1
        elif arg == '-Xclang' and args[i + 1].startswith('-W'):
            i += 1
        elif not arg.startswith('-W'):
            out.append(arg)
        i += 1
    # The kernel's C++ headers need the kernel's own warning set; only the C sources are held to -Wall -Werror.
    strict = ['-Wall', '-Werror', '-Wno-unused-function'] if source.suffix == '.c' else ['-Wno-c99-designator', '-Wno-reorder-init-list']
    # The prelinked collection leaves about 220 KiB for native __DATA growth: the RAM log ring is 64 KiB instead of the 1 MiB of
    # the iOS 16 port until it moves to memory allocated at boot.
    return out + ['-I', str(KIT), '-DVL_RING_SIZE=0x10000u'] + strict


objects = []
for name in SOURCES:
    source = KIT / name
    record = json.loads(RECORDS[source.suffix].read_text().strip().rstrip(','))
    obj = OUT / (source.stem + '.o')
    subprocess.run(command(record, source, obj), cwd=record['directory'], check=True, timeout=180)
    objects.append(str(obj))
archive = OUT / 'libHoujiKit.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *objects], check=True, timeout=30)
if os.environ.get('UNIXNU_HOUJI_STORAGE_LINK') == '1':
    # Explicit CI/local experiment only; does not enable native-source-storage at boot.
    device = Path(os.environ['UNIXNU_DEVICE']).resolve()
    framework = Path(os.environ['UNIXNU_HOUJI_FRAMEWORK']).resolve()
    subprocess.run(['python3', str(device / 'scripts/compile-houji-ufs.py'), '--archive',
                    '--framework', str(framework), '--tree', str(ROOT),
                    '--ufs', os.environ['UNIXNU_HOUJI_UFS_SOURCE'],
                    '--storage-source', os.environ['UNIXNU_HOUJI_STORAGE_SOURCE']], check=True, timeout=600)
    storage = ROOT / 'build/houji-ufs-compile/libHoujiSourceStorage.a'
    combined = OUT / 'libHoujiKit-storage.a'
    subprocess.run(['xcrun', 'libtool', '-static', '-o', str(combined), str(archive), str(storage)],
                   check=True, timeout=60)
    combined.replace(archive)
    print('PASS: storage runtime included in force-loaded houji kit; boot activation remains opt-in')
print(f'PASS: houji kit, {len(objects)} objects')
