#!/usr/bin/env python3
"""Compile-only gate for houji UFS and the pinned public storage base.

Uses the actual arm64e kernel compile record on macOS. Does not link, register
classes, qualify the retained Apple family ABI, or enable hardware access.
"""
import argparse
import hashlib
import importlib.util
import json
import shutil
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
    parser.add_argument('--archive', action='store_true', help='Build complete runtime archive, still not linked into XNU')
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
    inputs, objects = {}, []
    base = storage / 'IOBlockStorageDevice.cpp'
    sources = [base, ufs / 'NativeUfs.cpp']
    if args.archive:
        base = out / 'IOBlockStorageDevice.cpp'
        original = (storage / 'IOBlockStorageDevice.cpp').read_text()
        macro = 'OSDefineMetaClassAndAbstractStructors(IOBlockStorageDevice,IOService)'
        if original.count(macro) != 1:
            raise SystemExit('Storage registration macro changed')
        base.write_text(original.replace(macro, '''
extern "C" const OSMetaClass *native_source_storage_parent(void);
OSDefineMetaClassWithInit(IOBlockStorageDevice, IOService, )
IOBlockStorageDevice::MetaClass::MetaClass()
    : OSMetaClass("UniXNUBlockStorageDevice", native_source_storage_parent(), sizeof(IOBlockStorageDevice)) {}
OSDefineAbstractStructors(IOBlockStorageDevice, IOService)
OSDefineOperatorMethods(IOBlockStorageDevice)
'''))
        package = Path(__file__).resolve().parents[1] / 'package'
        sources = [base, ufs / 'NativeUfs.cpp', framework / 'drivers/RamBlock/NativeRamBlock.cpp']
        sources += [package / name for name in ('SourceStorage.cpp', 'SourceRootPolicies.cpp', 'SourceRootBinding.cpp')]
        sources += [framework / 'kits/VerifiedRoot/src/verified_root.c', framework / 'kits/VerifiedRoot/adapters/xnu_crypto.c']
        policies = Path(__file__).resolve().parent / 'policies/ios-27.2.1-24B156'
        key = (policies / 'OS/trusted-key.bin').read_bytes()
        if len(key) != 395 or key != (policies / 'SystemOS/trusted-key.bin').read_bytes():
            raise SystemExit('Policies require the same complete owner key')
        anchor = out / 'RootAnchor.c'
        anchor.write_text('#include <stdint.h>\nconst uint8_t verified_root_key[395] = {' + ','.join(map(str, key)) + '};\n')
        sources.append(anchor)
    c_record = json.loads((tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json').read_text().strip().rstrip(',')) if args.archive else None
    for source in sources:
        obj = out / (source.stem + '.o')
        selected = c_record if source.suffix == '.c' else record
        command = build.compile_command(selected, source, obj, includes, flags)
        if source.name == 'IOBlockStorageDevice.cpp':
            command.append('-Wno-error')  # Same public-source compatibility as the archive builder.
        (out / (source.stem + '.command.json')).write_text(json.dumps(command, indent=2) + '\n')
        result = subprocess.run(command, cwd=selected['directory'], stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, timeout=180)
        (out / (source.stem + '.log')).write_text(result.stdout)
        if result.returncode:
            raise SystemExit(result.stdout[-8000:])
        target.check_object(obj.read_bytes(), source.name)
        if args.archive and source.name in ('IOBlockStorageDevice.cpp', 'NativeUfs.cpp', 'NativeRamBlock.cpp'):
            late = obj.with_suffix('.late.o')
            objcopy = shutil.which('llvm-objcopy') or '/opt/homebrew/opt/llvm/bin/llvm-objcopy'
            subprocess.run([objcopy, '--remove-section=__DATA,__mod_init_func',
                            f'--globalize-symbol=__GLOBAL__sub_I_{source.name}', str(obj), str(late)], check=True, timeout=30)
            obj = late
        objects.append(obj)
        inputs[source.name] = dict(source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                                   object_sha256=hashlib.sha256(obj.read_bytes()).hexdigest())
        print(f'PASS: {source.name} compiled and arm64e object checked', flush=True)
    if args.archive:
        archive = out / 'libHoujiSourceStorage.a'
        archive.unlink(missing_ok=True)
        subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True, timeout=30)
        print(f'PASS: complete houji source-storage archive ({len(objects)} objects); not kernel-linked', flush=True)
    (out / 'report.json').write_text(json.dumps(dict(inputs=inputs, abi_qualified=False,
        linked=False, hardware_tested=False), indent=2) + '\n')


if __name__ == '__main__':
    main()
