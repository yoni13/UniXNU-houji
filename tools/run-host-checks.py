#!/usr/bin/env python3
"""Run portable fixtures and Apple runtime checks; does not emulate a GPU."""
import argparse
from pathlib import Path
import platform
import subprocess
import tempfile
import sys

ROOT = Path(__file__).resolve().parents[1]
# yoni13 fork: checks of upstream subsystems this kernel does not ship (upstream's battery/ADSP stack, backlight, network-stack stub,
# display timing, CPU/GPU governor and thermal policies, sample cache) are retired; see docs/fork.md.
APPLE_CHECKS = ('gpu-stage-trace.py', 'gpu-cpu-profile.py', 'metal-surface-use.py', 'metal-async.py', 'metal-trace-id.py', 'metal-command-dictionary.py', 'statusbar-geometry.py')
CHECKS = (
    'battery-spi.py',   # yoni13: HoujiSpi (GIC SPI routing) is shipped; the name is upstream's
    'turnip-bo-retire.py',
    'turnip-bo-cache.py',
    'native-gpu/run.py',
    'native-gpu/transport.py',
    'native-gpu/board-gate.py',
    'native-gpu/dispatch-gate.py',
    'native-gpu/fence-poll.py',
    'gpu-address-space.py',
    'gpu-client-slots.py',
    'gpu-coherence.py',
    'gpu-latency.py',
    'gpu-issuer-trace.py',
    'shader-prime.py',
    'metal-constants.py',
    'texture-properties.py',
    'metal-render-pass.py',
    'texture-init-probe.py',
    'gpu-memory-pressure.py',
    'gpu-memory-cache.py',
    'gpu-room-worker.py',
    'gpu-stage-trace.py',
    'gpu-cpu-profile.py',
    'metal-device-answers.py',
    'metal-device-limits.py',
    'metal-no-copy-pages.py',
    'metal-surface-use.py',
    'display-queue.py',
    'display-snapshot.py',
    'boot-arguments.py',
    'build-inputs.py',
    'recovery-logs.py',
    'userland-logs.py',
    'metal-async.py',
    'metal-trace-id.py',
    'system-apps.py',
    'system-app-dyld.py',
    'tcc-media-compatibility.py',
    'metal-command-dictionary.py',
    'statusbar-geometry.py',
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--suite', choices=('auto', 'portable', 'apple', 'all'), default='auto',
                        help='auto runs all on Apple Silicon, portable elsewhere')
    suite = parser.parse_args().suite
    apple_host = platform.system() == 'Darwin' and platform.machine() == 'arm64'
    if suite == 'auto':
        suite = 'all' if apple_host else 'portable'
    if suite in ('apple', 'all') and not apple_host:
        parser.error('Apple runtime checks require an Apple Silicon macOS host with Xcode')
    if platform.machine() not in ('arm64', 'aarch64'):
        parser.error('Host checks compile AArch64 SMMU bindings; use an arm64 host')
    print(f'Host check suite: {suite} ({platform.system()} {platform.machine()})', flush=True)
    if suite == 'portable':
        print('Apple runtime checks are not covered here; CI runs them separately on macOS.', flush=True)
    if suite != 'apple':
        with tempfile.TemporaryDirectory(prefix='houji-check-') as temporary:
            for filename, compiler, standard in [
                ('gpu-hfi.c', 'clang', 'c11'),
            ]:
                binary = Path(temporary) / Path(filename).stem
                subprocess.run([compiler, '-std=' + standard, '-Wall', '-Wextra', '-Werror', str(ROOT / 'checks' / filename), '-o', str(binary)], check=True)
                subprocess.run([str(binary)], check=True)
    selected = [name for name in CHECKS
                if suite == 'all' or (name in APPLE_CHECKS) == (suite == 'apple')]
    for name in selected:
        print('CHECK: ' + name, flush=True)
        subprocess.run([sys.executable, str(ROOT / 'checks' / name)], check=True)
    print(f'PASS: {suite} suite completed; no GPU or frame-rate qualification', flush=True)


if __name__ == '__main__':
    main()
