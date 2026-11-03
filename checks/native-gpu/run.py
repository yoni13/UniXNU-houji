#!/usr/bin/env python3
"""Compile active Submit/VM/Memory/SMMU sources against host peers, not a GPU."""
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SMMU = ROOT / 'kernel/overlay/iokit/Kernel/houji-ufs-root'
GPU = SMMU / 'Adreno740'
PM4 = ROOT / 'graphics/include'

with tempfile.TemporaryDirectory(prefix='houji-submit-') as temporary:
    tmp = Path(temporary)
    for name in ['IOKit/IOTypes.h', 'IOKit/IOBufferMemoryDescriptor.h', 'IOKit/IOLib.h', 'kern/task.h']:
        header = tmp / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "XnuMemoryFixture.h"\n')
    # Architectural barrier is the only instruction replaced in production
    # code. Register/cache operations use explicit peers; no hardware access.
    submit = (GPU / 'NativeAdrenoSubmit.cpp').read_text()
    barrier = '__asm__ volatile("dsb sy":::"memory");'
    assert submit.count(barrier) == 1
    (tmp / 'submit.cpp').write_text(submit.replace(barrier, '(void)0; /* host barrier peer */'))
    memory = (GPU / 'NativeAdrenoMemory.cpp').read_text()
    assert memory.count(barrier) == 1
    (tmp / 'memory.cpp').write_text(memory.replace(barrier, '(void)0; /* host barrier peer */'))
    provider = (SMMU / 'HoujiGpuNativeProvider.inc').read_text()
    start = provider.index('static int houjiProviderPublish(')
    (tmp / 'board-publish.inc').write_text(provider[start:provider.index('\nstatic int houjiProviderTimerRead', start)])
    flags = ['-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    flags += ['-I' + str(p) for p in [tmp, HERE, GPU, SMMU, PM4]]
    objects = []
    for source in sorted(SMMU.glob('ArmSmmu4K*.c')) + [PM4 / 'AdrenoPM4.c',
            HERE / 'sealed-arena.c', SMMU / 'vendor/AdrenoFirmware/AdrenoFirmware.c']:
        obj = tmp / (source.stem + '.o')
        # Cold boot headers intentionally zero the appended polling_faults
        # field through aggregate initialization; this fixture does not run them.
        extra = ['-Wno-unused-function', '-Wno-missing-field-initializers'] if source.name == 'sealed-arena.c' else []
        subprocess.run(['clang', '-std=c11', *flags, *extra, '-c', str(source), '-o', str(obj)], check=True)
        objects.append(str(obj))
    for page in (4096, 16384):
        binary = tmp / ('submit-' + str(page))
        sources = [GPU / name for name in ['NativeAdrenoSmmuMemory.cpp', 'NativeAdrenoVm.cpp',
                                          'NativeAdrenoUserClient.cpp', 'NativeAdrenoTimer.cpp']]
        sources += [SMMU / 'NativeAdrenoRing.cpp', tmp / 'memory.cpp']
        subprocess.run(['clang++', '-std=c++17', '-Wno-c99-designator', *flags, '-DTEST_CPU_PAGE=' + str(page),
                        *map(str, sources), str(tmp / 'submit.cpp'), str(HERE / 'submit.cpp'), str(HERE / 'surface-stubs.cpp'),
                        *objects, '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=60)
        pool_binary = tmp / ('pool-' + str(page))
        subprocess.run(['clang++', '-std=c++17', *flags, '-DTEST_CPU_PAGE=' + str(page),
                        str(tmp / 'memory.cpp'), str(HERE / 'pool.cpp'), str(HERE / 'surface-stubs.cpp'), '-o', str(pool_binary)], check=True)
        for scenario in ['invalid', 'reserve-fail', 'reserve-retain', 'wc-fail', 'metadata', 'reassert', 'basic']:
            subprocess.run([str(pool_binary), scenario], check=True, timeout=60)
