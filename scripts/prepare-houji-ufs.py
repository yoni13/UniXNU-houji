#!/usr/bin/env python3
"""Prepare a houji adaptation of pinned ShengUfs, with an opt-in Data store.

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


def replace_section(text, start, end, new):
    if text.count(start) != 1 or text.count(end) != 1 or text.index(start) >= text.index(end):
        raise ValueError(f'upstream section changed: {start}')
    return text[:text.index(start)] + new + text[text.index(end):]


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
    # 64 MiB instead of 48: houji's ath12k peaked at 49.9 MB and failed its REO destination ring on a 69632-byte allocation (root61,
    # 'dma window full: want 69632 free 438976 largest 61440'). The stage 1 table maps 2 MiB blocks at IOVA 0x10000000 upward.
    native = replace_once(native, 'kExtraIova = 0x10000000U, kExtraBlocks = 24, kBlock = 2U << 20;',
                          'kExtraIova = 0x10000000U, kExtraBlocks = 32, kBlock = 2U << 20;   /* houji: 64 MiB */')
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
    native = replace_once(native, '#include <pexpert/pexpert.h>', '#include <pexpert/pexpert.h>\n'
                          '#include <pexpert/boot.h>\n#include "PersistentDataCore.h"')
    native = replace_once(native, '#include "RootCommands.h"',
                          'static bool houjiDataArmed ()\n{\n'
                          '  return houji_data::optedIn (PE_boot_args (), BOOT_LINE_LENGTH);\n}\n\n'
                          '#include "RootCommands.h"')
    native = replace_section(native, '// ---- Persistent Data store window', '// ---- Read-only access to other partitions',
                             '#include "HoujiUfsDataStore.h"\n\n')
    # No Sheng log/export window or emergency unlocked panic submissions on this board.
    native = replace_section(native, '// ---- Diagnostic log window', '// One read-only window of the root image', '')
    # The legacy probe uses another doorbell path. Keep it read-only regardless of the Data flag.
    command = 'unsigned cdbLength, bool toHost, uint32_t length) -> UfsResult {\n'
    native = replace_once(native, command, command +
        '    if ((length && !toHost) || (cdb[0] != 0x00 && cdb[0] != 0x1b && cdb[0] != 0x12 &&\n'
        '        cdb[0] != 0x25 && cdb[0] != 0x28)) panic ("HOUJI UFS: legacy probe command refused");\n'
    )
    native = replace_once(native,
        '// Nothing here writes to the storage: only INQUIRY-class and READ(10) commands are built, plus\n'
        '// the single START STOP UNIT that wakes the device from the power-down the firmware left.',
        '// Root I/O stays read-only. Only an explicitly opened houji Data store can submit WRITE(10).')
    commands = (source / 'RootCommands.h').read_text()
    commands = replace_once(commands, 'struct HardwareTransferPort {',
        'static bool houjiCommandAllowed (const uint8_t *, uint32_t, uint32_t);\n\nstruct HardwareTransferPort {')
    commands = replace_once(commands, '  if (gHost.poisoned) return kIOReturnNotReady;\n',
        '  if (gHost.poisoned) return kIOReturnNotReady;\n'
        '  if (!houjiCommandAllowed (gHost.virt + kUcdOffset, direction, dataBytes))\n'
        '    return kIOReturnNotPermitted;\n')
    commands = replace_once(commands, 'if (!gHost.ready || !dst || !blocks', 'if (!dst || !blocks')
    commands = replace_once(commands, '  IOLockLock (gHost.lock);\n  gRootDiagnostic.lba = lba;',
        '  IOLockLock (gHost.lock);\n'
        '  if (!gHost.ready || gHost.poisoned) {\n'
        '    IOLockUnlock (gHost.lock);\n'
        '    return kIOReturnNotReady;\n'
        '  }\n  gRootDiagnostic.lba = lba;')
    tail = '// ---- Persistent store and log windows'
    if commands.count(tail) != 1:
        raise ValueError('upstream persistent command section changed')
    commands = commands[:commands.index(tail)] + (
        '#include "HoujiUfsWriteCommands.h"\n\n'
        'static bool houjiCommandAllowed (const uint8_t *ucd, uint32_t dir, uint32_t bytes)\n{\n'
        '  const bool armed = ucd[0] == 1 && (ucd[16] == 0x2a || ucd[16] == 0x35) && houjiDataArmed ();\n'
        '  return houji_data::commandAllowed (armed, gHoujiStoreWindow, ucd, dir, bytes);\n}\n')
    # The public firmware reader may read only a partition extent first granted by an exact GPT-name lookup. The GPT survey itself is
    # internal and read-only. This covers all six normal LUNs; on houji modem_a is expected on LUN 4.
    native = replace_once(native,
        'extern "C" int native_ufs_part_find (const char *name, uint32_t *lun, uint64_t *first, uint64_t *blocks)\n{',
        'namespace {\nstruct HoujiPartGrant { uint32_t lun; uint64_t first, blocks; };\n'
        'HoujiPartGrant gHoujiPartGrants[8] = {};\nunsigned gHoujiPartGrantCount = 0;\n}\n\n'
        'extern "C" int native_ufs_part_find (const char *name, uint32_t *lun, uint64_t *first, uint64_t *blocks)\n{')
    native = replace_once(native,
        '    if (size < 128 || size > 4096 || count > 512) continue;',
        '    if (entriesLba != 2 || size != 128 || count > 512) continue; /* houji primary GPT entry array: LBA 2..17 */')
    native = replace_once(native,
        '        *blocks = lastLba - firstLba + 1;\n        found = 1;',
        '        *blocks = lastLba - firstLba + 1;\n'
        '        bool granted = false;\n'
        '        for (unsigned g = 0; g < gHoujiPartGrantCount; ++g)\n'
        '          if (gHoujiPartGrants[g].lun == l && gHoujiPartGrants[g].first == firstLba &&\n'
        '              gHoujiPartGrants[g].blocks == *blocks) granted = true;\n'
        '        if (!granted && gHoujiPartGrantCount < 8) {\n'
        '          gHoujiPartGrants[gHoujiPartGrantCount++] = {l, firstLba, *blocks};\n'
        '          granted = true;\n'
        '        }\n'
        '        found = granted;')
    native = replace_once(native,
        '  if (!gHost.ready || !dst || !blocks || lun > 5) return 0;\n  while (blocks) {',
        '  if (!gHost.ready || !dst || !blocks || lun > 5 || lba > 0xffffffffULL || blocks - 1 > 0xffffffffULL - lba) return 0;\n'
        '  bool granted = false;\n'
        '  for (unsigned i = 0; i < gHoujiPartGrantCount; ++i) {\n'
        '    const HoujiPartGrant &g = gHoujiPartGrants[i];\n'
        '    if (g.lun == lun && lba >= g.first && lba - g.first < g.blocks && blocks <= g.blocks - (lba - g.first)) granted = true;\n'
        '  }\n'
        '  if (!granted) return 0;\n'
        '  while (blocks) {')
    output.mkdir(parents=True, exist_ok=False)
    for path in source.glob('*.h'):
        shutil.copyfile(path, output / path.name)
    package = Path(__file__).resolve().parents[1] / 'package'
    for name in ('PersistentDataCore.h', 'HoujiUfsDataStore.h', 'HoujiUfsWriteCommands.h'):
        shutil.copyfile(package / name, output / name)
    shutil.copyfile(legacy / 'GiuliaSmmuTakeover.h', output / 'GiuliaSmmuTakeover.h')
    (output / 'NativeUfs.cpp').write_text(native)
    (output / 'RootCommands.h').write_text(commands)
    report = dict(upstream_revision=revision, write_opt_in='native-data=userdata', storage_abi_qualified=False,
                  hardware_tested=False, sources={p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                                  for p in sorted(output.iterdir()) if p.is_file()})
    (output / 'houji-adaptation.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: pinned ShengUfs adapted with houji quiescent SMMU retargeting')
    print('PASS: Data-only write policy installed at hostWrite4k and hostSubmit; Sheng disk logging removed')
    print('NOT QUALIFIED: arm64e 24B156 compile/link/ABI or hardware I/O')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--legacy', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    prepare(args.source, args.legacy, args.output)
