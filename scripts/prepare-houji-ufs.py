#!/usr/bin/env python3
"""Prepare a local, read-only houji adaptation of pinned ShengUfs sources.

Does not select a kernel archive or claim storage ABI/runtime qualification.
The proven houji SMMU planner is supplied from the legacy source checkout.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

PIN = '99d8bcd3e43dd24fc3c8437c67b5d4cc71ab94d0'


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'upstream interface changed: {old[:80]}')
    return text.replace(old, new)


def prepare(source, legacy, output):
    revision = subprocess.check_output(['git', '-C', source, 'rev-parse', 'HEAD'], text=True).strip()
    if revision != PIN:
        raise ValueError('unexpected ShengUfs source revision')
    # Reject modified tracked sources, not just a mismatching HEAD.
    subprocess.run(['git', '-C', source, 'diff', '--exit-code', 'HEAD', '--', '*.h', '*.cpp'], check=True)
    if (source / 'SmmuTakeover.h').read_bytes() != (legacy / 'SmmuTakeover.h').read_bytes():
        raise ValueError('legacy and new SMMU planner interfaces differ')
    native = (source / 'NativeUfs.cpp').read_text()
    native = replace_once(native, '#include "SmmuTakeover.h"', '#include "GiuliaSmmuTakeover.h"')
    native = replace_once(native, 'uint64_t dma, uint64_t bytes)\n{\n  uintptr_t smmu',
                          'uint64_t dma, uint64_t bytes, uintptr_t host)\n{\n'
                          '  if ((read32(host, 0x60) & 1) || read32(host, 0x58) || read32(host, 0x78))\n'
                          '    panic("HOUJI UFS: queue not quiescent before SMMU takeover");\n  uintptr_t smmu')
    native = replace_once(native,
        'UfsSmmuPlan plan = planUfsSmmu (stream, dma, bytes, [smmu](unsigned offset) { return read32 (smmu, offset); });',
        'GiuliaSmmuPlan selection = planGiuliaSmmu (stream, dma, bytes, [smmu](unsigned offset) { return read32 (smmu, offset); });\n'
        '  const UfsSmmuPlan &plan = selection.target;')
    native = replace_once(native, 'configureUfsSmmu (plan, stream,', 'configureGiuliaSmmu (selection, stream, true,')
    native = replace_once(native, 'smmuReport, phys, 16384);', 'smmuReport, phys, 16384, hc);')
    native = replace_once(native, 'report, gHost.phys, kDmaBytes);', 'report, gHost.phys, kDmaBytes, gHost.hc);')
    # The Wi-Fi endpoint's pass-through stream 0x1401 and its 32-bit DMA window (native_ufs_extra_*, read by WifiProbe.cpp), set up here as
    # on sheng because the stream matching registers are only safe at this moment; houji's boot argument is WifiProbe's own (wifi>=2).
    native = replace_once(native, 'if (PE_parse_boot_argn ("sheng-wifi", &wifi, sizeof (wifi)) && wifi >= 2) {',
                          'if (PE_parse_boot_argn ("wifi", &wifi, sizeof (wifi)) && wifi >= 2) { /* houji: wifi>=2 */')
    native = replace_once(native, 'PE_parse_boot_argn ("sheng-wifi-attr", &attr, sizeof (attr));',
                          'PE_parse_boot_argn ("wifi-attr", &attr, sizeof (attr));')
    native = replace_once(native, 'extraReport, 0x9c900000ULL, 8ULL << 20);',
                          'extraReport, 0x9c900000ULL, 8ULL << 20, gHost.hc);')
    native = replace_once(native, 'if (dpu >= 2 || scanout) {',
                          'if (false) { /* houji: sheng display mapping disabled */')
    # Bring-up checkpoints (thread context only): each stage reaches the USB console before the next hardware step.
    start = native.index('static bool hostBringUp (const char **why)\n{')
    end = native.index('\n}\n', start)
    body = native[start:end]
    staged = body.count('gRootDiagnostic.stage = "')
    if staged < 3:
        raise ValueError('upstream interface changed: hostBringUp stages')
    body = body.replace('gRootDiagnostic.stage = "', 'houjiStage ("')
    import re
    body = re.sub(r'houjiStage \("([^"]*)";', r'houjiStage ("\1");', body)
    for old, new in (('  if (!hostStopQueue (why)) return false;\n',
                      '  if (!hostStopQueue (why)) return false;\n  houjiStage ("request queue stopped; SMMU takeover next");\n'),
                     ('  gHost.smmu = gHost.smmuMap->getVirtualAddress ();\n',
                      '  gHost.smmu = gHost.smmuMap->getVirtualAddress ();\n  houjiStage ("SMMU stream 0x60 configured");\n'),
                     ('  if (!hostStartQueue (why)) return false;\n',
                      '  if (!hostStartQueue (why)) return false;\n  houjiStage ("request queue restarted");\n')):
        if body.count(old) != 1:
            raise ValueError(f'upstream interface changed: {old.strip()}')
        body = body.replace(old, new)
    native = native[:start] + ('static void houjiStage (const char *stage)\n{\n'
        '  gRootDiagnostic.stage = stage;\n  IOLog ("HOUJI UFS: %s\\n", stage);\n  IOSleep (300);\n}\n\n') + body + native[end:]
    # The read-only probes report by panic. Print the same text first and give the console time to send it, so the
    # result does not depend on the panic-time drain.
    for probe in ('10', '11'):
        start = native.index(f'  if (probe == {probe}) {{\n')
        end = native.index('\n  }\n', start)
        block = native[start:end]
        if 'panic (' not in block:
            raise ValueError(f'upstream interface changed: probe {probe}')
        block = block.replace('panic (', 'houjiReport (')
        block = block.replace('    RootLayout *layout', '    houjiStage ("reading GPT and the userdata manifest");\n    RootLayout *layout', 1)
        native = native[:start] + block + native[end:]
    native = native.replace('static void houjiStage (const char *stage)\n',
        '#define houjiReport(...) do { IOLog (__VA_ARGS__); IOLog ("\\n"); IOSleep (2000); panic (__VA_ARGS__); } while (0)\n'
        'static void houjiStage (const char *stage)\n', 1)
    commands = (source / 'RootCommands.h').read_text()
    commands = replace_once(commands, '  *ocs = 15;\n',
                            '  *ocs = 15;\n  if (direction == 1) return kIOReturnNotPermitted; // houji bring-up: no storage writes\n')
    # Also reject the public write entry before it copies or modifies DMA data.
    commands = replace_once(commands,
        '  if (!gHost.ready || !src || !blocks || blocks > kDataMax / 4096 || lun != 0 || !hostInWindow (lba, blocks)) {',
        '  return kIOReturnNotPermitted; // houji: persistence is not qualified\n'
        '  if (!gHost.ready || !src || !blocks || blocks > kDataMax / 4096 || lun != 0 || !hostInWindow (lba, blocks)) {')
    output.mkdir(parents=True, exist_ok=False)
    for path in source.glob('*.h'):
        shutil.copyfile(path, output / path.name)
    shutil.copyfile(legacy / 'GiuliaSmmuTakeover.h', output / 'GiuliaSmmuTakeover.h')
    (output / 'NativeUfs.cpp').write_text(native)
    (output / 'RootCommands.h').write_text(commands)
    report = dict(upstream_revision=revision, writes_enabled=False, storage_abi_qualified=False,
                  hardware_tested=False, sources={p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                                  for p in sorted(output.iterdir()) if p.is_file()})
    (output / 'houji-adaptation.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: pinned ShengUfs adapted with houji quiescent SMMU retargeting')
    print('PASS: hostWrite4k and hostSubmit(direction=1) reject writes')
    print('NOT QUALIFIED: arm64e 24B156 ABI, compilation, hardware block reads')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--legacy', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    prepare(args.source, args.legacy, args.output)
