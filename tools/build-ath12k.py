#!/usr/bin/env python3
"""Compile the Wi-Fi module (UniXNU Ath12kWifi: Linux ath12k, MHI and QMI over its own compatibility layer) into one relocatable kernel
object for the houji kernel link. Usage: build-ath12k.py MODULE_DIR OUT_DIR
MODULE_DIR is a checkout of UniXNU/Ath12kWifi; its tools/fetch-sources.sh and tools/prepare.sh run first (pinned Linux v7.0.14 and FreeBSD
sources), then houji/kernel/ath12k/*.patch are applied to the prepared ath12k copy. Every global name of the module except the nw_wifi_*
entry points gets the prefix nwk_, then all objects are linked into OUT_DIR/houji_ath12k.o (printed on the last line).
Runs on macOS (xcrun clang, Homebrew LLVM tools, ld -r) and on Linux (clang, llvm-nm, llvm-objcopy; the object is then not linked)."""
import os, shutil, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

TRIPLE = 'arm64-apple-macos13.0'
module = Path(sys.argv[1]).resolve()
out = Path(sys.argv[2]).resolve()
out.mkdir(parents=True, exist_ok=True)
port = Path(__file__).resolve().parent.parent
darwin = sys.platform == 'darwin'

subprocess.run(['bash', str(module / 'tools/fetch-sources.sh')], check=True, stdout=subprocess.DEVNULL)
# houji patches to the module's own sources (houji/kernel/ath12k-module/*.patch, -p1 in the module directory; already applied ones are kept)
# (applied forward, then required to reverse cleanly: BSD and GNU patch differ in how they report an already applied patch)
for patch in sorted((port / 'kernel/ath12k-module').glob('*.patch')):
    r = subprocess.run(['patch', '-p1', '--forward', '-d', str(module), '-i', str(patch)], capture_output=True, text=True)
    print(f'module patch {patch.name}: {r.returncode} {r.stdout.strip()} {r.stderr.strip()}'.replace('\n', ' '))
    if subprocess.run(['patch', '-p1', '-R', '--dry-run', '-d', str(module), '-i', str(patch)], capture_output=True).returncode:
        raise SystemExit(f'module patch not in place: {patch.name}')
subprocess.run(['bash', str(module / 'tools/prepare.sh')], check=True, stdout=subprocess.DEVNULL)
# houji patches: applied to private copies of the files they touch (the prepared tree holds links into deps/linux)
for patch in sorted((port / 'kernel/ath12k').glob('*.patch')):
    files = [l[6:].strip() for l in patch.read_text().splitlines() if l.startswith('+++ b/')]
    for f in files:
        p = module / 'out/ath12k-src' / f
        if p.is_symlink():
            target = p.resolve()
            p.unlink()
            shutil.copyfile(target, p)
    subprocess.run(['patch', '-p1', '--forward', '-d', str(module / 'out/ath12k-src'), '-i', str(patch)], check=True)

clang = subprocess.run(['xcrun', '-f', 'clang'], check=True, capture_output=True, text=True).stdout.strip() if darwin else 'clang'
resource = subprocess.run([clang, '-print-resource-dir'], check=True, capture_output=True, text=True).stdout.strip() + '/include'
flags = ['-target', TRIPLE, '-mkernel', '-O2', '-g0', '-ffreestanding', '-fno-builtin', '-fno-common', '-fno-strict-aliasing', '-fwrapv',
         '-fno-stack-protector', '-Wno-incompatible-pointer-types', '-Wno-error', '-fno-delete-null-pointer-checks', '-mgeneral-regs-only',
         '-momit-leaf-frame-pointer', '-std=gnu11', '-w', '-DCONFIG_ATH12K_DEBUG', '-nostdinc', '-isystem', str(module / 'shim/libc'),
         '-isystem', resource, '-include', 'lx_pci.h', '-I', str(module / 'shim/include'), '-I', str(module / 'include'),
         '-I', str(module / 'out/pinned'), '-I', str(module / 'src')]
trees = {'ath12k': ['-I', str(module / 'out/ath12k-src'), '-I', str(module / 'out/ath12k-src/wifi7')],
         'mhi': ['-I', str(module / 'out/mhi-src')]}
jobs = []
for line in (module / 'tools/sources.txt').read_text().splitlines():
    line = line.strip()
    if not line or line.startswith('#'):
        continue
    tree, _, name = line.partition(':') if ':' in line else ('', '', line)
    src = module / 'out/ath12k-src' / name if tree == 'ath12k' else module / 'out/mhi-src' / name if tree == 'mhi' else module / name
    obj = out / (('w7_' if '/wifi7/' in str(src) else tree + '_' if tree else '') + src.stem + '.o')
    jobs.append((src, obj, trees.get(tree, trees['ath12k'] if name.startswith('src/') else [])))

def build(job):
    src, obj, extra = job
    r = subprocess.run([clang, '-c', *flags, *extra, str(src), '-o', str(obj)], capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stderr)
        raise SystemExit(f'compile failed: {src}')
    return obj

with ThreadPoolExecutor(os.cpu_count() or 8) as pool:
    list(pool.map(build, jobs))

def tool(name):
    if not darwin:
        return name
    for formula in ('llvm', 'llvm@18', 'llvm@19', 'llvm@20'):
        r = subprocess.run(['brew', '--prefix', formula], capture_output=True, text=True)
        p = Path(r.stdout.strip()) / 'bin' / name
        if r.returncode == 0 and p.exists():
            return str(p)
    raise SystemExit(f'{name} not found (brew install llvm)')

keep = {'_nw_wifi_start', '_nw_wifi_step', '_nw_wifi_stop', '_nw_wifi_stats', '_nw_wifi_set_events', '_nw_wifi_scan', '_nw_wifi_bss_count',
        '_nw_wifi_bss_get', '_nw_wifi_connect', '_nw_wifi_disconnect', '_nw_wifi_tx', '_nw_wifi_status_get', '_nw_wifi_where',
        '_nw_wifi_tx_reasons', '_nw_wifi_ps_set'}
objs = [o for _, o, _ in jobs]
names = set()
for o in objs:
    r = subprocess.run([tool('llvm-nm'), '-g', '--defined-only', str(o)], check=True, capture_output=True, text=True)
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] not in keep:
            names.add(parts[2])
mapping = out / 'rename.txt'
mapping.write_text(''.join(f'{n} _nwk{n}\n' for n in sorted(names)))
for o in objs:
    subprocess.run([tool('llvm-objcopy'), f'--redefine-syms={mapping}', str(o)], check=True)
# what the module needs from the kernel, after renaming: everything still undefined
undefined = set()
for o in objs:
    r = subprocess.run([tool('llvm-nm'), '-u', str(o)], check=True, capture_output=True, text=True)
    undefined |= {l.split()[-1] for l in r.stdout.splitlines() if l.strip()}
undefined -= {'_nwk' + n for n in names} | keep
(out / 'undefined.txt').write_text(''.join(f'{n}\n' for n in sorted(undefined)))
print(f'{len(objs)} objects, {len(names)} names renamed, {len(undefined)} undefined: {" ".join(sorted(undefined))}')
if darwin:
    combined = out / 'houji_ath12k.o'
    subprocess.run(['ld', '-r', '-arch', 'arm64', '-o', str(combined), *map(str, objs)], check=True)
    print(combined)
