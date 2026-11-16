#!/usr/bin/env python3
"""houji (A750) GMU register and setup inputs for UniXNU Adreno740, ported from UniXNU-sheng scripts/prepare-sheng-gmu.py.

Generated from source only, no device I/O: register offsets from Linux registers/adreno/a6xx.xml and a6xx_gmu.xml, the RSC/PDC
setup writes from a6xx_gmu.c a6xx_gmu_rpmh_init() (the A740 family branch, which includes the A750 / ADRENO_7XX_GEN3; for A7xx
the PDC sequencer is configured by AOP and the host writes only the RSC handshake, the RSC sequencer and the two PDC enables),
the GMU firmware chip id from a7xx_catalog.c ADRENO_CHIP_IDS(0x43051401), the register windows from this phone's mainline DTB
reg-names, and the size and SHA-256 of this phone's gmu_gen70900.bin.
Usage: prepare-houji-gmu.py LINUX_SOURCE_DIR DTB GMU_FIRMWARE OUT"""
import ast, hashlib, json, re, subprocess, sys, xml.etree.ElementTree as ET
from pathlib import Path


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    cache, dtb, firmware_path, out = (Path(a) for a in sys.argv[1:5])
    sources = ['drivers/gpu/drm/msm/adreno/' + s for s in ['a6xx_gmu.c', 'a6xx_gmu.h', 'a6xx_gpu.c', 'a6xx_gpu.h', 'a7xx_catalog.c']]
    sources += ['drivers/gpu/drm/msm/registers/adreno/' + s for s in ['a6xx.xml', 'a6xx_gmu.xml']]

    def cells(node, name):
        return [int(v, 16) for v in subprocess.check_output(['fdtget', '-t', 'x', str(dtb), node, name], text=True).split()]

    ranges = {}
    for node in ['/soc@0/gpu@3d00000', '/soc@0/gmu@3d6a000']:
        names = subprocess.check_output(['fdtget', '-t', 's', str(dtb), node, 'reg-names'], text=True).split()
        reg = cells(node, 'reg')
        if len(reg) != 4 * len(names):
            raise SystemExit('DT register-name binding differs: ' + node)
        for i, name in enumerate(names):
            ranges[name] = dict(base=(reg[4 * i] << 32) | reg[4 * i + 1], bytes=(reg[4 * i + 2] << 32) | reg[4 * i + 3])
    ordered = ['gmu', 'rscc', 'gmu_pdc', 'kgsl_3d0_reg_memory', 'cx_mem']
    windows = [ranges[name] for name in ordered]
    gpu = (cache / 'drivers/gpu/drm/msm/adreno/a6xx_gpu.c').read_text()
    if 'msm_ioremap(pdev, "cx_mem")' not in gpu:
        raise SystemExit('Actual CX binding differs')

    ns = {'r': 'http://nouveau.freedesktop.org/'}
    trees = [ET.parse(cache / ('drivers/gpu/drm/msm/registers/adreno/' + s)).getroot() for s in ['a6xx.xml', 'a6xx_gmu.xml']]
    regs = {}

    def register(symbol, domain, name, variant=None):
        matches = []
        for tree in trees:
            for d in tree.findall('r:domain', ns):
                if d.attrib['name'] == domain:
                    matches += [n for n in d.findall('r:reg32', ns)
                                if n.attrib.get('name') == name and (variant is None or n.attrib.get('variants') == variant)]
        if len(matches) != 1:
            raise SystemExit('Ambiguous/missing source register ' + symbol)
        regs[symbol] = int(matches[0].attrib['offset'], 0)

    core = ['GMU_CM3_ITCM_START', 'GMU_CM3_DTCM_START', 'GMU_CM3_SYSRESET', 'GMU_CM3_BOOT_CONFIG', 'GMU_CM3_FW_INIT_RESULT',
            'GMU_CM3_CFG', 'GMU_HFI_QTBL_ADDR', 'GMU_HFI_QTBL_INFO', 'GMU_HFI_CTRL_INIT', 'GMU_HFI_CTRL_STATUS',
            'GMU_HOST2GMU_INTR_SET', 'GMU_SYS_BUS_CONFIG', 'GMU_ICACHE_CONFIG', 'GMU_DCACHE_CONFIG', 'GMU_AHB_FENCE_RANGE_0',
            'GMU_GMU2HOST_INTR_INFO', 'GMU_AO_HOST_INTERRUPT_STATUS']
    for name in core:
        register('REG_' + name, 'A6XX', name)
    for name in ['GPU_GMU_CX_GMU_CX_FALNEXT_INTF', 'GPU_GMU_CX_GMU_CX_FAL_INTF']:
        register('REG_' + name, 'A6XX', name, 'A6XX')
    for i in [8, 9, 10]:
        register('REG_GMU_GENERAL_' + str(i), 'A6XX', 'GMU_GENERAL_' + str(i), 'A7XX')
    rsc = ['GPU_RSCC_RSC_STATUS0_DRV0', 'RSCC_PDC_SLAVE_ID_DRV0', 'RSCC_HIDDEN_TCS_CMD0_ADDR', 'RSCC_HIDDEN_TCS_CMD0_DATA',
           'RSCC_OVERRIDE_START_ADDR', 'RSCC_PDC_SEQ_START_ADDR', 'RSCC_PDC_MATCH_VALUE_LO', 'RSCC_PDC_MATCH_VALUE_HI',
           'RSCC_SEQ_BUSY_DRV0']
    for name in rsc:
        register('REG_' + name, 'A6XX', name)
    register('REG_RSCC_SEQ_MEM_0_DRV0_A740', 'A6XX', 'RSCC_SEQ_MEM_0_DRV0_A740', 'A7XX')
    register('REG_CX_MISC_TCM_RET_CNTL', 'A6XX_CX_MISC', 'CX_MISC_TCM_RET_CNTL', 'A7XX-')
    register('REG_GBIF_QSB_SIDE0', 'A6XX', 'GBIF_QSB_SIDE0')
    for name in ['SEQ_START_ADDR', 'ENABLE_PDC']:
        register('REG_PDC_GPU_' + name, 'A6XX_PDC', 'GPU_' + name)

    def field(domain, name, bit):
        for tree in trees:
            for d in tree.findall('r:domain', ns):
                if d.attrib['name'] != domain:
                    continue
                for reg in d.findall('r:reg32', ns):
                    if reg.attrib.get('name') == name:
                        for f in reg.findall('r:bitfield', ns):
                            if f.attrib.get('name') == bit:
                                return 1 << int(f.attrib['pos'])
        raise SystemExit('Missing actual fatal bit ' + bit)

    fatal = sum(field('A6XX', 'GMU_AO_HOST_INTERRUPT_STATUS', b) for b in ['WDOG_BITE', 'FENCE_ERR', 'HOST_AHB_BUS_ERROR'])
    cm3 = field('A6XX', 'GMU_GMU2HOST_INTR_INFO', 'CM3_FAULT')
    source = (cache / 'drivers/gpu/drm/msm/adreno/a6xx_gmu.c').read_text()
    init = source[source.index('static void a6xx_gmu_rpmh_init('):source.index('/*\n * The lowest 16 bits')]
    if 'adreno_is_a7xx(adreno_gpu))\n\t\tpdc_in_aop = true;' not in init:
        raise SystemExit('A7xx PDC-in-AOP policy changed')

    def number(expression):
        expression = expression.strip()
        if '?' in expression:
            # The A750 (ADRENO_7XX_GEN3) is in adreno_is_a740_family(): only conditions whose first operand is that test are taken.
            condition = re.sub(r'\s+', ' ', expression.split('?', 1)[0]).strip().lstrip('(')
            if not condition.startswith('adreno_is_a740_family(adreno_gpu)'):
                raise SystemExit('Unrecognized conditional')
            expression = expression.split('?', 1)[1].split(':', 1)[0].strip()
        expression = re.sub(r'BIT\((\d+)\)', lambda m: str(1 << int(m[1])), expression)
        expression = expression.replace('seqmem0_drv0_reg', str(regs['REG_RSCC_SEQ_MEM_0_DRV0_A740']))
        for name, value in regs.items():
            original = name.replace('REG_', 'REG_A6XX_', 1)
            expression = re.sub(r'\b' + original + r'\b', str(value), expression)
        parsed = ast.parse(expression, mode='eval')
        if any(not isinstance(n, (ast.Expression, ast.Constant, ast.BinOp, ast.Add)) for n in ast.walk(parsed)):
            raise SystemExit('Unsupported source expression ' + expression)
        return eval(compile(parsed, '<pinned-source-constant>', 'eval'), {'__builtins__': {}})

    setup = []
    prefix = init[init.index('/* Disable SDE clock gating */'):init.index('/* The second spin')]
    for reg, value in re.findall(r'gmu_write_rscc\(gmu,\s*([^,]+),\s*([\s\S]*?)\);', prefix):
        setup.append((1, 4 * number(reg), number(value)))
    if 'seqmem0_drv0_reg = REG_A7XX_RSCC_SEQ_MEM_0_DRV0_A740;' not in init:
        raise SystemExit('A740 family sequencer selection absent')
    start = init.index('gmu_write_rscc(gmu, seqmem0_drv0_reg,')
    sequence = init[start:init.index('} else {', start)]
    for reg, value in re.findall(r'gmu_write_rscc\(gmu,\s*([^,]+),\s*([^;]+)\);', sequence):
        setup.append((1, 4 * number(reg), number(value)))
    tail = init[init.index('setup_pdc:'):]
    for reg, value in re.findall(r'pdc_write\(pdcptr,\s*([^,]+),\s*([^;]+)\);', tail):
        setup.append((2, 4 * number(reg), number(value)))
    if len(setup) != 19:
        raise SystemExit('A7xx RSC/PDC sequence length differs: ' + str(len(setup)))
    for window, offset, value in setup:
        if offset & 3 or offset + 4 > windows[window]['bytes'] or not 0 <= value <= 0xffffffff:
            raise SystemExit('Generated setup outside actual resource')
    catalog = (cache / 'drivers/gpu/drm/msm/adreno/a7xx_catalog.c').read_text()
    record = re.search(r'\.chip_ids = ADRENO_CHIP_IDS\(0x43051401\)([\s\S]*?)\.preempt_record_size', catalog)
    if '"gmu_gen70900.bin"' not in record[1]:
        raise SystemExit('A750 GMU firmware name differs')
    chip = int(re.search(r'\.gmu_chipid = (0x[0-9a-f]+)', record[1])[1], 0)
    firmware = dict(bytes=firmware_path.stat().st_size, sha256=sha(firmware_path))

    out.mkdir(parents=True, exist_ok=True)
    defines = '\n'.join('#define ' + key + ' UINT32_C(' + hex(value) + ')' for key, value in regs.items())
    text = '/* Generated by prepare-houji-gmu.py from pinned inputs. Do not commit. */\n#include <stdint.h>\n#include <stddef.h>\n' + defines + '\n'
    text += '#define SHENG_GMU_CHIP_ID UINT32_C(0x43051401)\n#define SHENG_GMU_FW_CHIP_ID UINT32_C(' + hex(chip) + ')\n'
    text += '#define SHENG_GMU_CORE_GPU_DELTA ' + hex(windows[0]['base'] - windows[3]['base']) + 'u\n'
    text += '#define SHENG_GMU_FIRMWARE_BYTES ' + str(firmware['bytes']) + 'u\n'
    text += '#define SHENG_GMU_AO_FATAL_MASK ' + hex(fatal) + 'u\n#define SHENG_GMU_CM3_FATAL_MASK ' + hex(cm3) + 'u\n'
    text += 'static const uint8_t sheng_gmu_firmware_sha256[32]={' + ','.join('0x' + firmware['sha256'][i:i + 2] for i in range(0, 64, 2)) + '};\n'
    text += 'static const uint64_t sheng_gmu_window_physical[5]={' + ','.join(hex(w['base']) for w in windows) + '};\n'
    text += 'static const size_t sheng_gmu_window_bytes[5]={' + ','.join(hex(w['bytes']) for w in windows) + '};\n'
    text += 'typedef struct {unsigned window;size_t byte_offset;uint32_t value;} ShengGmuSetupWrite;\n'
    text += '#define SHENG_GMU_RSC_SETUP_COUNT 19u\nstatic const ShengGmuSetupWrite sheng_gmu_rsc_setup[19]={\n'
    text += '\n'.join('{' + str(window) + ',' + hex(offset) + ',' + hex(value) + '},' for window, offset, value in setup) + '\n};\n'
    header = out / 'ShengGmuSource.h'
    header.write_text(text)
    report = dict(source_sha256={n: sha(cache / n) for n in sources}, dtb_sha256=sha(dtb), registers=regs, window_names=ordered,
                  windows=windows, rsc_setup=setup, gmu_firmware_chip_id=hex(chip), firmware=firmware,
                  generated_header_sha256=sha(header), source_analysis_only=True, hardware_registers_observed=False)
    (out / 'gmu-source-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: A750 GMU fw chip %s, %d registers, windows %s, %d RSC/PDC writes, firmware %d bytes %s…' % (
        hex(chip), len(regs), ','.join(hex(w['base']) for w in windows), len(setup), firmware['bytes'], firmware['sha256'][:16]))


if __name__ == '__main__':
    main()
