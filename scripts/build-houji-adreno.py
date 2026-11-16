#!/usr/bin/env python3
"""houji Adreno 750 inputs and kits for UniXNU Adreno740, ported from UniXNU-sheng 6880068 scripts/build-adreno740-kits.py.

UNIXNU_HOUJI_GPU_INPUTS names a local directory that never enters Git:
  include/   ShengGmuSource.h, ShengGmuVotesSource.h, ShengGmuCommandDbSource.h, ShengGpuSmmuSource.h, written by
             prepare-houji-gmu.py, prepare-houji-gmu-votes.py and prepare-houji-gpu-smmu.py (ios27/gpu-inputs.sh)
  firmware/  gmu_gen70900.bin, gen70900_sqe.fw, gen70900_zap.mbn from this phone's partitions, pinned in houji-gpu-firmware.json
Steps (board.toml [kernel] archives):
  inputs  before component:Adreno740: the generated headers into build/sheng-gmu and build/sheng-gpu-smmu, where
          build-arm-driver.py's NativeAdrenoService.cpp and the kits look for them
  kits    after it: the GMU, command-DB, vote, HFI, SMMU and PM4 kits, the remaining NativeAdreno objects and the three firmware
          images (strong definitions over NativeAdrenoFirmwareDefaults.cpp's weak empty ones) added to build/adreno740/libAdreno740.a
"""
import hashlib, json, os, shutil, subprocess, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from kernel_target import TRIPLE

HEADERS = {'ShengGmuSource.h': 'build/sheng-gmu', 'ShengGmuVotesSource.h': 'build/sheng-gmu',
           'ShengGmuCommandDbSource.h': 'build/sheng-gmu', 'ShengGpuSmmuSource.h': 'build/sheng-gpu-smmu'}
INPUTS = Path(os.environ.get('UNIXNU_HOUJI_GPU_INPUTS') or ROOT.parent / 'houji-gpu')
OUT = ROOT / 'build/adreno740'


def inputs():
    for name, directory in HEADERS.items():
        source = INPUTS / 'include' / name
        if not source.is_file():
            sys.exit(f'{source}: missing; run ios27/gpu-inputs.sh (UNIXNU_HOUJI_GPU_INPUTS={INPUTS})')
        text = source.read_text()
        # The headers must be this board's: the A750 chip id, never the A740 one the SoC kit was first written for.
        if name in ('ShengGmuSource.h', 'ShengGmuVotesSource.h') and '0x43051401' not in text:
            sys.exit(f'{source}: not the A750 (0x43051401) header')
        (ROOT / directory).mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, ROOT / directory / name)
    print(f'PASS: {len(HEADERS)} A750 board headers installed from {INPUTS}/include')


def kits():
    c_recipe = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/bsd_init.o.json').read_text().strip().rstrip(','))
    cxx_recipe = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
    OUT.mkdir(parents=True, exist_ok=True)
    sources = [ROOT / 'kits/Sm8550Platform' / name for name in ('ShengGmu.c', 'ShengGmuCommandDb.c')]
    sources += [ROOT / 'kits/houji/ShengGmuVotes.c']
    sources += [ROOT / 'kits/AdrenoFirmware' / name for name in ('AdrenoHfi.c', 'AdrenoHfiStartup.c', 'AdrenoHfiVotes.c', 'AdrenoFirmware.c')]
    sources += sorted((ROOT / 'kits/ArmSmmu4K').glob('ArmSmmu4K*.c'))
    sources += [ROOT / 'kits/AdrenoPM4/AdrenoPM4.c']
    trace_hash = hashlib.sha256()
    for path in sorted((ROOT / 'drivers/Adreno740').glob('Native*')):
        if path.is_file():
            trace_hash.update(path.name.encode())
            trace_hash.update(path.read_bytes())
    trace_id = trace_hash.hexdigest()[:16]
    (OUT / 'gpu-trace-build.json').write_text(json.dumps({'source_sha256': trace_hash.hexdigest(), 'trace_source_id': trace_id,
                                                          'target': TRIPLE}, indent=2) + '\n')
    # NativeAdrenoService.cpp is build-arm-driver.py's object; the accelerator (AdrenoMetal's staged build) is not built yet.
    sources += sorted(p for p in (ROOT / 'drivers/Adreno740').glob('NativeAdreno*.cpp')
                      if p.name not in ('NativeAdrenoClient.cpp', 'NativeAdrenoService.cpp', 'NativeAdrenoAccelerator.cpp'))
    objects = []
    for source in sources:
        recipe = c_recipe if source.suffix == '.c' else cxx_recipe
        obj = OUT / (source.stem + ('.kit.o' if source.suffix == '.c' else '.o'))
        command, i = [], 0
        while i < len(recipe['arguments']):
            value = recipe['arguments'][i]
            if value == recipe['file']:
                command.append(str(source))
            elif value == '-o':
                command += ['-o', str(obj)]
                i += 1
            elif value in ('-MF', '-MJ'):
                i += 1
            elif value == '-Xclang' and recipe['arguments'][i + 1].startswith('-W'):
                i += 1
            elif not value.startswith('-W') and value not in ('-MD', '-MP'):
                command.append(value)
            i += 1
        command += ['-I', str(ROOT / 'kits/houji'), '-I', str(ROOT / 'kits/Sm8550Platform'), '-I', str(ROOT / 'kits/AdrenoFirmware'),
                    '-I', str(ROOT / 'build/sheng-gmu'), '-I', str(ROOT / 'build/sheng-gpu-smmu'), '-I', str(ROOT / 'kits/AdrenoPM4'),
                    '-I', str(ROOT / 'kits/ArmSmmu4K'), '-I', str(ROOT / '.cache/xnu-work/osfmk'),
                    '-DNATIVE_GPU_BUILD_ID=0x' + trace_id + 'ULL', '-I', str(ROOT / 'drivers/Adreno740'),
                    '-I', str(OUT / 'include'), '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-incompatible-sysroot',
                    '-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
                    '-Wno-unnecessary-virtual-specifier']
        result = subprocess.run(command, cwd=recipe['directory'], capture_output=True, text=True)
        if result.returncode:
            sys.exit(str(source) + ':\n' + result.stderr[-2500:])
        objects.append(obj)
    # The firmware, checked against the committed pins before it is embedded. Read-only text constants: the prelinked collection
    # has no room for 170 KB more of native __DATA_CONST (build-houji-display.py).
    pins = json.loads((ROOT / 'scripts/houji-gpu-firmware.json').read_text())['files']
    embedded = [('_native_adreno_firmware_image', 'gmu_gen70900.bin'), ('_native_adreno_sqe_firmware_image', 'gen70900_sqe.fw'),
                ('_native_adreno_zap_image', 'gen70900_zap.mbn')]
    assembly = ['.section __TEXT,__const']
    for label, key in embedded:
        path = INPUTS / 'firmware' / key
        blob = path.read_bytes() if path.is_file() else b''
        if len(blob) != pins[key]['bytes'] or hashlib.sha256(blob).hexdigest() != pins[key]['sha256']:
            sys.exit(f'firmware {path} is missing or does not match its pin')
        assembly += ['.p2align 12', '.globl ' + label, label + ':', '.incbin ' + json.dumps(str(path)), label + '_end:',
                     '.p2align 3', '.globl ' + label + '_size', label + '_size:', '.quad ' + label + '_end-' + label]
    (OUT / 'firmware.S').write_text('\n'.join(assembly) + '\n')
    subprocess.run(['xcrun', 'clang', '-target', TRIPLE, '-Wno-incompatible-sysroot', '-c', OUT / 'firmware.S', '-o', OUT / 'firmware.o'],
                   check=True)
    objects.append(OUT / 'firmware.o')
    archive = OUT / 'libAdreno740.a'
    subprocess.run(['xcrun', 'ar', 'rcs', archive, *objects], check=True)
    defined = subprocess.run(['xcrun', 'nm', '-g', archive], capture_output=True, text=True).stdout
    missing = [s for s in ('_ShengGmuColdStart', '_ShengGmuBuildStartup', '_ShengGmuCommandDbInputs', '_AdrenoFirmwarePlanGmu',
                           '_ArmSmmu4KAttachStreams', '_ArmSmmu4KBindMmio', '_AdrenoPM4Type7', '_NativeAdrenoGmuContextBringup')
               if (' T ' + s) not in defined and (' S ' + s) not in defined]
    if missing:
        sys.exit('Required kit definitions absent: ' + ', '.join(missing))
    print('PASS: %d A750 kit objects archived with all eight required definitions present' % len(objects))


if __name__ == '__main__':
    {'inputs': inputs, 'kits': kits}[sys.argv[1]]()
