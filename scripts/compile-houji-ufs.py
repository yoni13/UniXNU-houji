#!/usr/bin/env python3
"""Compile-only gate for houji UFS and the pinned public storage base.

Uses the actual arm64e kernel compile record on macOS. Does not link, register
classes, qualify the retained Apple family ABI, or enable hardware access.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--framework', type=Path, required=True)
    parser.add_argument('--tree', type=Path, required=True)
    parser.add_argument('--ufs', type=Path, required=True)
    parser.add_argument('--storage-source', type=Path, required=True)
    args = parser.parse_args()
    framework, tree, ufs, storage = (p.resolve() for p in
        (args.framework, args.tree, args.ufs, args.storage_source))
    out = tree / 'build/houji-ufs-compile'
    out.mkdir(parents=True, exist_ok=True)
    headers = load('storage_headers', framework / 'scripts/prepare-storage-headers.py')
    headers.prepare(storage, '7edb88fbae296fb7c8ce2f64e115e116e566d51c', out / 'headers')
    build = load('source_compile', framework / 'scripts/build-sheng-source-drivers.py')
    target = load('target', framework / 'scripts/kernel_target.py')
    record = json.loads((tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
    includes = ['-I', str(out / 'headers/include'), '-I', str(framework / 'drivers/VirtioMmioBlock'),
                '-I', str(framework / 'kits/VerifiedRoot/include'), '-I', str(tree / '.cache/xnu-work/osfmk')]
    flags = ['-DNATIVE_UFS_SOURCE_API=1', '-DNATIVE_VERIFIED_BLOCK=1',
             '-DNATIVE_VERIFIED_BLOCK_MULTI_POLICY=1', '-DNATIVE_VERIFIED_POLICY_HANDOFF=1']
    inputs = {}
    for source in (storage / 'IOBlockStorageDevice.cpp', ufs / 'NativeUfs.cpp'):
        obj = out / (source.stem + '.o')
        command = build.compile_command(record, source, obj, includes, flags)
        if source.name == 'IOBlockStorageDevice.cpp':
            command.append('-Wno-error')  # Same public-source compatibility as the archive builder.
        (out / (source.stem + '.command.json')).write_text(json.dumps(command, indent=2) + '\n')
        result = subprocess.run(command, cwd=record['directory'], stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, timeout=180)
        (out / (source.stem + '.log')).write_text(result.stdout)
        if result.returncode:
            raise SystemExit(result.stdout[-8000:])
        target.check_object(obj.read_bytes(), source.name)
        inputs[source.name] = dict(source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                                   object_sha256=hashlib.sha256(obj.read_bytes()).hexdigest())
        print(f'PASS: {source.name} compiled and arm64e object checked', flush=True)
    (out / 'report.json').write_text(json.dumps(dict(inputs=inputs, abi_qualified=False,
        linked=False, hardware_tested=False), indent=2) + '\n')


if __name__ == '__main__':
    main()
