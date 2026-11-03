// SPDX-License-Identifier: BSD-2-Clause-Patent
// Wi-Fi bring-up on houji (Xiaomi 14, SM8650): the WCN7850 ("Kiwi v2") on PCIe0. Adapted from UniXNU-sheng's WifiProbe.cpp (SM8550, the same
// chip on the same controller). Boot argument houji_wifi: 0 off, 1 a read-only survey, 2 power and link training, 3 also the
// endpoint's configuration read, 4 also its BAR and MHI registers, 5 (default) the Wi-Fi module (firmware, scan, en0; USB console "wifi ..." commands).
// What differs from sheng, from Linux gcc-sm8650.c, tcsrcc-sm8650.c, icc sm8650.c and the board's devicetree:
// - GDSC collapse votes at 0x5214c (0x52020 on SM8550); the NoC branches report at 0x10058 / 0x10064 / 0x10074;
// - the PCIe0 reference clock gate is TCSR 0x1fc0000 + 0x31100; the PCIe memory NoC bus clock manager is SN4 (SN7 on SM8550);
// - WLAN_EN is GPIO 16 (80 on sheng); PERST# 94, CLKREQ# 95, WAKE# 96 as on sheng;
// - the WCN7850 PMU supplies (vdd smpi4 0.94 V, vddio ldob15 1.8 V, vddio1p2 ldoc3 1.2 V, vddaon smpc2 0.976 V, vdddig smpc3 0.916 V,
//   vddrfa1p2 smpc1 1.316 V, vddrfa1p8 smpc6 1.864 V), the shared antenna rail ldok6 1.8 V and the PMU's reference clock RF_CLK1.
// The QMP PHY tables, PARF/DBI/ATU/config windows, the stream IDs (0x1400 root port, 0x1401 endpoint) and the MSI line (SPI 141) are the same.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include <stdint.h>
#include "WifiPcieTables.h"
#include "../houji-adsp/QcomRpmh.h"
#include "nw_abi.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <stdarg.h>
#include <libkern/libkern.h>
#include <kern/task.h>

extern "C" int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *);   // HoujiAdsp.cpp: the 2 ms poller
extern "C" const char *houji_smmu_route_stream(uint16_t stream, uint64_t dma, uint64_t bytes, uint32_t report[3]);   // HoujiUfsRead.cpp
extern "C" void sheng_wifi_net_start(const uint8_t mac[6]);
extern "C" void nw_wifi_ps_set(int on);          // the module (houji patch 0003)
extern "C" uint64_t houji_dvfs_touch_time(void);  // HoujiCpuDvfs.inc: the last touch   // WifiNet.cpp: en0 and the registry entries the system looks for
extern "C" int houji_modem_file(const char *path, uint8_t **data, size_t *size, size_t max);   // AdspFirmware.c

// [0] stage (negative: the step that failed), [1] vote 0x52008, [2] vote 0x52000, [3] GDSC, [4] halt bits (bit i: branch i of kBranches off),
// [5] pins (WLAN_EN | PERST# << 8 | CLKREQ# << 16, bit 0 input level, bit 1 output), [6] PARF LTSSM, [7] root port vendor/device,
// [8] link control/status, [9] PARF PHY control, [10] pins after WLAN_EN, [11] GDSC bits | halt bits << 8, [12] PCS_STATUS1 | polls << 16,
// [13] port debug 0, [14] port debug 1, [15] endpoint vendor/device, [16] link ms, [17] rails found (bit i), [18] RF clock | BCM vote,
// [19] RSC id, [20] SID table hash | bypass register << 8, [21] pipe branch after the PHY, [22..31] endpoint: BAR0 mask, BAR0, command,
// MHIREGLEN, MHIVER, MHICFG, BHIOFF, BHIEOFF, MHICTRL, MHISTATUS.
extern "C" {volatile uint32_t native_wifi_stats[32];}

namespace {
constexpr uint64_t kGcc = 0x100000, kTlmm = 0xf100000, kParf = 0x1c00000, kDbi = 0x60000000, kPhy = 0x1c06000, kTcsr = 0x1fc0000;
constexpr uint64_t kAtu = 0x60001000, kConfig = 0x60100000;
constexpr uint32_t kTcsrPcie0Clkref = 0x31100;
constexpr uint32_t kVote8 = 0x52008, kVote0 = 0x52000, kGdsc = 0x6b004, kPhyGdsc = 0x6c000, kCollapse = 0x5214c;
// slv_axi, mstr_axi, cfg_ahb, aux, pipe, slv_q2a, cnoc_sf, aggre_noc, ddrss_sf, phy_rchng
constexpr uint32_t kBranches[] = {0x6b020, 0x6b02c, 0x6b038, 0x6b03c, 0x6b048, 0x6b01c, 0x10058, 0x10064, 0x10074, 0x6b054};
constexpr uint32_t kNeeded = 0xf;   // slv_axi, mstr_axi, cfg_ahb, aux
constexpr unsigned kWlanEn = 16, kPerst = 94, kClkReq = 95;
constexpr unsigned kPins[] = {kWlanEn, kPerst, kClkReq};
constexpr uint32_t kParfLtssm = 0x1b0, kParfPhyCtrl = 0x40, kLinkControl = 0x80;
constexpr uint32_t kPcieBcr = 0x6b000, kPhyBcr = 0x6c01c, kPipeMux = 0x6b070, kAuxRcg = 0x6b074, kRchngRcg = 0x6b058;
constexpr uint32_t kVote8Bits = 0x7f, kVote0Bits = 1u << 12 | 1u << 19 | 1u << 22;
constexpr uint32_t kParfSysCtrl = 0x0, kParfPmCtrl = 0x20, kParfDbiBase = 0x350, kParfDbiBaseHi = 0x354, kParfMhiClockReset = 0x174;
constexpr uint32_t kParfAxiHalt = 0x1a8, kParfAtuBase = 0x634, kParfAtuBaseHi = 0x638, kParfDeviceType = 0x1000;
constexpr uint32_t kParfSlvSpace = 0x358, kParfSlvSpaceHi = 0x35c, kSlvSpaceBytes = 0x80000000;
constexpr uint32_t kPortDebug0 = 0x728, kPortDebug1 = 0x72c, kBusNumbers = 0x18, kCommand = 0x4;
constexpr uint64_t kMemBase = 0x60300000, kMemSize = 0x3d00000;

struct Window {IOMemoryMap *map; volatile uint32_t *va;};
bool mapWindow(Window &w, uint64_t base, uint64_t size) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}
void release(Window &w) {if (w.map) w.map->release(); w = {};}

// TLMM pin config: pull in bits 0-1 (1 down, 3 up), function in 2-5, drive (mA / 2 - 1) in 6-8, output enable bit 9; io bit 1 drives
void setPin(volatile uint32_t *tlmm, unsigned pin, uint32_t config, int level) {
    if (level >= 0) tlmm[(pin * 0x1000 + 4) / 4] = level ? 2u : 0u;
    tlmm[pin * 0x1000 / 4] = config;
    if (level >= 0) tlmm[(pin * 0x1000 + 4) / 4] = level ? 2u : 0u;
}
uint32_t pinWord(volatile uint32_t *tlmm) {
    uint32_t w = 0;
    for (unsigned i = 0; i < 3; ++i) w |= (tlmm[(kPins[i] * 0x1000 + 4) / 4] & 3u) << (8 * i);
    return w;
}
bool waitClear(volatile uint32_t *reg, uint32_t mask, unsigned tries, unsigned *used = nullptr) {
    unsigned i = 0;
    for (; i < tries && (*reg & mask); ++i) IODelay(100);
    if (used) *used = i;
    return !(*reg & mask);
}
void pulseReset(volatile uint32_t *gcc, uint32_t bcr) {
    gcc[bcr / 4] |= 1u;
    IODelay(1000);
    gcc[bcr / 4] &= ~1u;
    IODelay(1000);
}

// The PMU's supplies in power sequencer order, then the antenna rail; each a VRM resource of the command database with its voltage in mV at
// +0, enable at +4 and mode at +8 (7: high power mode for these SMPSs and LDOs).
struct Rail {const char *name; uint32_t millivolts;};
constexpr Rail kRails[] = {{"smpi4", 940}, {"ldob15", 1800}, {"ldoc3", 1200}, {"smpc2", 976}, {"smpc3", 916}, {"smpc1", 1316},
                           {"smpc6", 1864}, {"ldok6", 1800}};
constexpr uint64_t kRscDrv = 0x17a20000, kRscTcs = 0xd00, kCmdDb = 0x81c60000, kCmdDbSize = 0x20000;

int powerRails() {
    uint8_t *db = static_cast<uint8_t *>(IOMallocData(kCmdDbSize));
    if (!db) return -1;
    Window dbw = {}, rsc = {};
    if (!mapWindow(dbw, kCmdDb, kCmdDbSize)) {IOFreeData(db, kCmdDbSize); return -1;}
    for (uint64_t i = 0; i < kCmdDbSize; i += 4) {const uint32_t w = dbw.va[i / 4]; memcpy(db + i, &w, 4);}
    release(dbw);
    if (!mapWindow(rsc, kRscDrv, 0x10000)) {IOFreeData(db, kCmdDbSize); return -2;}
    volatile uint32_t *r = rsc.va;
    const QcomRscLayout layout = qcom_rsc_layout(r[0]);
    const unsigned tcs = 0;   // active TCS 0; the ADSP's votes go through TCS 1
    native_wifi_stats[19] = r[0];
    auto send = [&](uint32_t address, uint32_t value) {
        QcomRpmhWrite writes[16];
        const unsigned n = qcom_rpmh_active_request(&layout, tcs, &address, &value, 1, writes, 16);
        for (unsigned i = 0; i < n; ++i) r[(kRscTcs + writes[i].offset) / 4] = writes[i].value;
        __asm__ volatile("dsb sy" ::: "memory");
        IOSleep(2);
        const unsigned m = qcom_rpmh_finish(&layout, tcs, writes, 16);
        for (unsigned i = 0; i < m; ++i) r[(kRscTcs + writes[i].offset) / 4] = writes[i].value;
    };
    uint32_t found = 0;
    for (unsigned i = 0; i < sizeof kRails / sizeof kRails[0]; ++i) {
        QcomRpmhResource res;
        if (qcom_cmd_db_find(db, kCmdDbSize, kRails[i].name, &res)) continue;
        found |= 1u << i;
        send(res.address + 8, 7);
        send(res.address, kRails[i].millivolts);
        send(res.address + 4, 1);
        IOSleep(5);
    }
    native_wifi_stats[17] = found;
    // the PMU's reference clock (RF_CLK1, a VRM-type clock: enable at +4)
    static const char *const clocks[] = {"clka1", "rfclka1"};
    for (const char *name : clocks) {
        QcomRpmhResource res;
        if (qcom_cmd_db_find(db, kCmdDbSize, name, &res)) continue;
        send(res.address + 4, 1);
        native_wifi_stats[18] = 0x80000000u | (name[0] == 'r' ? 2u : 1u) << 24;
        break;
    }
    // The endpoint's DMA crosses the PCIe memory NoC: its bus clock manager (SN4 on SM8650) gets the vote the interconnect driver makes when
    // the link comes up. vote = ceil(ceil(kBps * width / buswidth) * 1000 / unit), unit and width from the resource's aux data, buswidth 16.
    {
        QcomRpmhResource bcm;
        if (!qcom_cmd_db_find(db, kCmdDbSize, "SN4", &bcm)) {
            uint32_t vote = 0x400;
            if (bcm.level_count >= 3 && (bcm.levels[0] | (uint32_t)bcm.levels[1] << 16) && bcm.levels[2]) {
                const uint64_t unit = bcm.levels[0] | (uint32_t)bcm.levels[1] << 16;
                const uint64_t scaled = (2000000ull * bcm.levels[2] + 15) / 16;
                const uint64_t x = (scaled * 1000 + unit - 1) / unit;
                vote = x < 1 ? 1 : x > 0x3fff ? 0x3fff : (uint32_t)x;
            }
            send(bcm.address, 1u << 30 | 1u << 29 | vote << 14 | vote);
            native_wifi_stats[18] |= vote | 0x8000u;
        }
    }
    release(rsc);
    IOFreeData(db, kCmdDbSize);
    return (found & 0x7fu) == 0x7fu ? 0 : -3;   // the seven PMU supplies; the antenna rail is best effort
}

// ---- stage 5: the Wi-Fi module (UniXNU Ath12kWifi) on this board ----
// It does its own chip power-up, MHI boot, QRTR/QMI and driver start; the board gives it register access, configuration space, DMA memory,
// firmware files, a clock and a log. DMA: one physically contiguous buffer below 4 GiB (the endpoint addresses 32 bits), seen by the
// endpoint at its physical address through a pass-through SMMU context of its own (stream 0x1401, houji_smmu_route_stream), and reached by
// the CPU through an uncached mapping, so neither side needs cache maintenance.
constexpr unsigned kArenaBlocks = 16384;
struct Arena {uint8_t *va; uint64_t iova; size_t bytes; struct Block {uint32_t off, size; bool used;} *blocks; unsigned count;} gArena;
IOBufferMemoryDescriptor *gArenaBuffer;
IOMemoryMap *gArenaMap;
void pLog(int level, const char *line);
void wlog(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void wlog(int level, const char *fmt, ...) {
    char line[160]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    pLog(level, line);
}
// When no such buffer is free (the usual case 50 s into the boot) the buffer may sit anywhere and the endpoint's context bank gets a stage 1
// table instead (as UniXNU-sheng's storage driver gives it): 4 KiB granule, 32-bit input, L1 -> L2 with 2 MiB blocks mapping kIova upward.
constexpr uint64_t kIova = 0x10000000ull;
uint64_t gArenaPa;   // physical address of the buffer; gArena.iova is what the endpoint uses
bool gTranslated;
IOBufferMemoryDescriptor *gTables;
bool arenaTranslated() {
    static const uint32_t sizes[] = {48u << 20, 32u << 20, 24u << 20};
    constexpr uint32_t kBlock = 2u << 20;
    for (uint32_t bytes : sizes) {
        gArenaBuffer = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
            kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone, bytes + kBlock, kBlock);
        if (!gArenaBuffer) continue;
        IOByteCount contiguous = 0;
        uint64_t pa = gArenaBuffer->getPhysicalSegment(0, &contiguous, kIOMemoryMapperNone);
        if (!pa || contiguous < bytes + kBlock) {gArenaBuffer->release(); gArenaBuffer = nullptr; continue;}
        const uint64_t shift = (kBlock - (pa & (kBlock - 1))) & (kBlock - 1);
        uint8_t *cached = static_cast<uint8_t *>(gArenaBuffer->getBytesNoCopy());
        bzero(cached, bytes + kBlock);
        for (uint64_t off = 0; off < bytes + kBlock; off += 64) __asm__ volatile("dc civac, %0" :: "r"(cached + off) : "memory");
        __asm__ volatile("dsb sy" ::: "memory");
        gArenaMap = gArenaBuffer->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache);
        if (!gArenaMap) {gArenaBuffer->release(); gArenaBuffer = nullptr; continue;}
        gArena.va = reinterpret_cast<uint8_t *>(gArenaMap->getVirtualAddress()) + shift;
        gArenaPa = pa + shift;
        gArena.iova = kIova;
        gArena.bytes = bytes;
        gTranslated = true;
        return true;
    }
    return false;
}
// The routed context bank (report: slot | bank << 16) translates kIova.. onto the buffer.
const char *arenaMapStage1(uint32_t report) {
    constexpr uint32_t kBlock = 2u << 20;
    gTables = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone, 8192, 4096);
    if (!gTables) return "no table memory";
    uint64_t *l1 = static_cast<uint64_t *>(gTables->getBytesNoCopy()), *l2 = l1 + 512;
    bzero(l1, 8192);
    const uint64_t tablesPa = gTables->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    l1[0] = (tablesPa + 4096) | 3;
    for (uint32_t i = 0; i < gArena.bytes / kBlock; ++i)   // block, non-global, AF, inner shareable, EL0 RW, attribute 0
        l2[(kIova >> 21) + i] = (gArenaPa + (uint64_t)i * kBlock) | (1ull << 11) | (1ull << 10) | (3ull << 8) | (1ull << 6) | 1;
    for (uint32_t off = 0; off < 8192; off += 64) __asm__ volatile("dc civac, %0" :: "r"(reinterpret_cast<uint8_t *>(l1) + off) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
    Window smmu = {};
    if (!mapWindow(smmu, 0x15000000, 0xd0000)) return "SMMU map failed";
    const uint32_t pages = 1u << (((smmu.va[0x24 / 4] >> 28) & 7) + 1);
    volatile uint32_t *cb = smmu.va + (pages + (report >> 16)) * 4096 / 4;
    cb[0x0 / 4] = 0;                                         // stage 1 off while the table is set
    cb[0x20 / 4] = uint32_t(tablesPa); cb[0x24 / 4] = uint32_t(tablesPa >> 32);
    cb[0x10 / 4] = 5;                                        // TCR2: 48-bit output
    cb[0x30 / 4] = 32u | (1u << 23);                         // T0SZ 32, non-cacheable walks, 4 KiB granule, no TTBR1
    cb[0x38 / 4] = 0xff;                                     // MAIR attribute 0: write-back
    cb[0x618 / 4] = 0; cb[0x7f0 / 4] = 0;                    // invalidate the context's TLB, sync
    __asm__ volatile("dsb sy" ::: "memory");
    cb[0x0 / 4] = 1u | (1u << 5) | (1u << 6);                // SCTLR: M, CFRE, CFIE
    __asm__ volatile("dsb sy; isb" ::: "memory");
    // check the first and last page through the bank's address translation (stage 1 privileged read)
    // (recorded only: the translation operation registers may not be usable from here; the endpoint's own DMA is the real test)
    const uint64_t probes[2] = {kIova, kIova + gArena.bytes - 4096};
    for (unsigned i = 0; i < 2; ++i) {
        cb[0x800 / 4] = uint32_t(probes[i]); cb[0x804 / 4] = 0;
        unsigned spin = 0;
        while ((cb[0x8f0 / 4] & 1) && ++spin < 100000) {}
        const uint64_t par = cb[0x50 / 4] | (uint64_t)cb[0x54 / 4] << 32;
        wlog(5, "wifi: smmu bank %u probe %llx: par %llx (want %llx) atsr %x spin %u sctlr %x fsr %x idr0 %x", report >> 16,
             (unsigned long long)probes[i], (unsigned long long)par, (unsigned long long)(gArenaPa + (probes[i] - kIova)), cb[0x8f0 / 4], spin,
             cb[0x0 / 4], cb[0x58 / 4], smmu.va[0x20 / 4]);
    }
    release(smmu);
    return nullptr;
}
bool arenaInit() {
    static const uint32_t sizes[] = {48u << 20, 32u << 20, 24u << 20};
    for (uint32_t bytes : sizes) {
        gArenaBuffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone, bytes, 0x00000000fffff000ull);
        if (!gArenaBuffer) continue;
        IOByteCount contiguous = 0;
        const uint64_t pa = gArenaBuffer->getPhysicalSegment(0, &contiguous, kIOMemoryMapperNone);
        if (!pa || contiguous < bytes || pa + bytes > 0x100000000ull) {gArenaBuffer->release(); gArenaBuffer = nullptr; continue;}
        uint8_t *cached = static_cast<uint8_t *>(gArenaBuffer->getBytesNoCopy());
        bzero(cached, bytes);
        for (uint64_t off = 0; off < bytes; off += 64) __asm__ volatile("dc civac, %0" :: "r"(cached + off) : "memory");
        __asm__ volatile("dsb sy" ::: "memory");
        gArenaMap = gArenaBuffer->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache);
        if (!gArenaMap) {gArenaBuffer->release(); gArenaBuffer = nullptr; continue;}
        gArena.va = reinterpret_cast<uint8_t *>(gArenaMap->getVirtualAddress());
        gArena.iova = gArenaPa = pa;
        gArena.bytes = bytes;
        break;
    }
    if (!gArenaBuffer && !arenaTranslated()) return false;
    if (!gArena.blocks) gArena.blocks = static_cast<Arena::Block *>(IOMallocData(kArenaBlocks * sizeof(Arena::Block)));
    if (!gArena.blocks) return false;
    gArena.blocks[0] = {4096, (uint32_t)(gArena.bytes - 4096), false}; gArena.count = 1;   // the first page is the MSI target
    return true;
}
// Buffers the endpoint uses sit at addresses aligned to their size rounded up to a power of two (the boot loader refuses an image address
// that is only page aligned).
void *arenaAlloc(size_t n, uint64_t *bus) {
    n = (n + 63) & ~(size_t)63;
    if (n >= 4096) n = (n + 4095) & ~(size_t)4095;
    uint32_t align = 64;
    if (n >= 4096) {align = 4096; while (align < n) align <<= 1;}
    for (unsigned i = 0; i < gArena.count; ++i) {
        Arena::Block &b = gArena.blocks[i];
        if (b.used) continue;
        // alignment is of the bus address
        const uint64_t busStart = (gArena.iova + b.off + align - 1) & ~(uint64_t)(align - 1);
        const uint32_t start = (uint32_t)(busStart - gArena.iova), pad = start - b.off;
        if (b.size < pad + n) continue;
        const bool tail = b.size > pad + n;
        const unsigned extra = (pad ? 1u : 0u) + (tail ? 1u : 0u);
        if (gArena.count + extra > kArenaBlocks) continue;
        const Arena::Block whole = b;
        memmove(&gArena.blocks[i + extra], &gArena.blocks[i], (gArena.count - i) * sizeof(Arena::Block));
        gArena.count += extra;
        unsigned at = i;
        if (pad) {gArena.blocks[at] = {whole.off, pad, false}; ++at;}
        gArena.blocks[at] = {start, (uint32_t)n, true};
        if (tail) gArena.blocks[at + 1] = {start + (uint32_t)n, whole.size - pad - (uint32_t)n, false};
        uint8_t *p = gArena.va + start;
        bzero(p, n);
        if (bus) *bus = gArena.iova + start;
        return p;
    }
    uint32_t freeTotal = 0, freeMax = 0;
    for (unsigned i = 0; i < gArena.count; ++i) if (!gArena.blocks[i].used) {freeTotal += gArena.blocks[i].size; if (gArena.blocks[i].size > freeMax) freeMax = gArena.blocks[i].size;}
    wlog(3, "dma window full: want %zu free %u largest %u blocks %u", n, freeTotal, freeMax, gArena.count);
    return nullptr;
}
void arenaFree(void *p) {
    if (!p || (uint8_t *)p < gArena.va || (uint8_t *)p >= gArena.va + gArena.bytes) return;
    const uint32_t off = (uint32_t)((uint8_t *)p - gArena.va);
    for (unsigned i = 0; i < gArena.count; ++i) {
        if (gArena.blocks[i].off != off || !gArena.blocks[i].used) continue;
        gArena.blocks[i].used = false;
        if (i + 1 < gArena.count && !gArena.blocks[i + 1].used) {gArena.blocks[i].size += gArena.blocks[i + 1].size; memmove(&gArena.blocks[i + 1], &gArena.blocks[i + 2], (gArena.count - i - 2) * sizeof(Arena::Block)); --gArena.count;}
        if (i > 0 && !gArena.blocks[i - 1].used) {gArena.blocks[i - 1].size += gArena.blocks[i].size; memmove(&gArena.blocks[i], &gArena.blocks[i + 1], (gArena.count - i - 1) * sizeof(Arena::Block)); --gArena.count;}
        return;
    }
}

uint64_t pNow() {uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000000;}
void pSleep(unsigned ms) {IOSleep(ms);}
void pPoll() {}
// The module's log goes to the kernel log (and so to the USB console): levels up to 6 always, debug lines (7) with houji_wifi_debug=1 and
// only the first 2000. A line equal to one of the last sixteen is only counted, as are the per-channel and per-frame lines.
uint32_t gLogRepeats, gDebugLines, gDebug;
bool has(const char *s, const char *w) {for (; *s; ++s) {size_t k = 0; while (w[k] && s[k] == w[k]) ++k; if (!w[k]) return true;} return false;}
void pLog(int level, const char *line) {
    if (has(line, "chan scan list") || has(line, "mac channel [")) {++gLogRepeats; return;}
    if (level >= 7 && (!gDebug || gDebugLines >= 2000 || has(line, "htc rx completion") || has(line, "rx ce pipe") || has(line, "ext irq:") ||
                       has(line, "event mgmt rx") || has(line, "mgmt rx event") || has(line, "chan info vdev_id") ||
                       has(line, "scan event foreign channel"))) {++gLogRepeats; return;}
    static uint32_t recent[16];
    uint32_t h = 2166136261u;
    for (const char *c = line; *c; ++c) h = (h ^ static_cast<uint8_t>(*c)) * 16777619u;
    h ^= static_cast<uint32_t>(level);
    for (uint32_t r : recent) if (r == h) {++gLogRepeats; return;}
    memmove(&recent[1], &recent[0], sizeof recent - sizeof recent[0]);
    recent[0] = h;
    if (level >= 7) ++gDebugLines;
    static uint64_t first;
    const uint64_t now = pNow();
    if (!first) first = now;
    IOLog("HOUJI WIFI LOG %u %d %.120s\n", static_cast<uint32_t>(now - first), level, line);
}
void pHalt() {for (;;) IOSleep(1000);}
void *hAlloc(size_t n) {
    uint8_t *p = static_cast<uint8_t *>(IOMallocData(n + 16));
    if (!p) return nullptr;
    *reinterpret_cast<size_t *>(p) = n + 16;
    return p + 16;
}
void hFree(void *q) {
    if (!q) return;
    uint8_t *p = static_cast<uint8_t *>(q) - 16;
    IOFreeData(p, *reinterpret_cast<size_t *>(p));
}
void *pDmaAlloc(size_t n, uint64_t *bus) {return arenaAlloc(n, bus);}
void pDmaFree(size_t, void *p, uint64_t) {arenaFree(p);}
uint64_t pDmaMap(void *p, size_t, int) {
    if ((uint8_t *)p < gArena.va || (uint8_t *)p >= gArena.va + gArena.bytes) return 0;
    return gArena.iova + ((uint8_t *)p - gArena.va);
}
void pDmaSync(uint64_t, size_t, int) {}

// Firmware: the stock files in the firmware partition's image/kiwi (Kiwi v2: amss20.bin, phy_ucode20.elf as m3.bin, the global N3 board
// data bd_n3gl.elf as board.bin, Xiaomi's regdb), read once and kept. The radio's address (wlan_mac.bin) is made from the chip ID.
struct FwCache {const char *want, *path; uint8_t *data; size_t size; int rc;};
FwCache gFw[] = {{"amss.bin", "image/kiwi/amss20.bin"}, {"m3.bin", "image/kiwi/phy_ucode20.elf"}, {"board.bin", "image/kiwi/bd_n3gl.elf"},
                 {"regdb.bin", "image/kiwi/regdb_xiaomi.bin"}};
bool boardMac(uint8_t m[6]) {
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    OSData *id = chosen ? OSDynamicCast(OSData, chosen->getProperty("unique-chip-id")) : nullptr;
    uint64_t seed = 0;
    if (id && id->getLength() >= sizeof seed) memcpy(&seed, id->getBytesNoCopy(), sizeof seed);
    OSSafeReleaseNULL(chosen);
    if (!seed) seed = 0x686f756a69ull;   // no chip ID published: a fixed seed ("houji")
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned i = 0; i < 8; ++i) h = (h ^ ((seed >> (8 * i)) & 0xff)) * 0x100000001b3ull;
    for (unsigned i = 0; i < 6; ++i) m[i] = (uint8_t)(h >> (8 * i));
    m[0] = (uint8_t)((m[0] & 0xfe) | 0x02);   // locally administered, unicast
    return true;
}
int pFwGet(const char *path, const uint8_t **data, size_t *size) {
    const char *base = path;
    for (const char *c = path; *c; ++c) if (*c == '/') base = c + 1;
    if (!strcmp(base, "wlan_mac.bin")) {
        static char text[40]; static size_t len;
        if (!len) {
            uint8_t m[6];
            boardMac(m);
            len = (size_t)snprintf(text, sizeof text, "address=%02x%02x%02x%02x%02x%02x\n", m[0], m[1], m[2], m[3], m[4], m[5]);
        }
        *data = (const uint8_t *)text; *size = len;
        return 0;
    }
    for (FwCache &f : gFw) {
        if (strcmp(base, f.want)) continue;
        if (!f.data && !f.rc) {
            uint8_t *bytes = nullptr; size_t n = 0;
            f.rc = houji_modem_file(f.path, &bytes, &n, 16u << 20);
            if (!f.rc) {f.data = bytes; f.size = n;}
            wlog(6, "wifi: firmware %s from %s: %d, %zu bytes", f.want, f.path, f.rc, n);
        }
        if (f.rc) return -1;
        *data = f.data; *size = f.size;
        return 0;
    }
    return -1;
}

// Configuration space of the endpoint: 32-bit accesses through the iATU window; offsets with bit 31 set are relative to the PCIe capability.
volatile uint32_t *gCfg;
uint32_t gPcieCap, gMsiCap;
bool findCaps() {
    uint8_t cap = (uint8_t)(gCfg[0x34 / 4] & 0xff);
    for (unsigned guard = 0; cap >= 0x40 && guard < 48; ++guard) {
        const uint32_t w = gCfg[cap / 4];
        if ((uint8_t)w == 0x10) gPcieCap = cap;
        if ((uint8_t)w == 0x05) gMsiCap = cap;
        cap = (uint8_t)(w >> 8);
    }
    return gPcieCap && gMsiCap;
}
int pCfgRead(int off, int size, uint32_t *v) {
    uint32_t o = (uint32_t)off;
    if (o & 0x80000000u) o = gPcieCap + (o & 0x7fffffffu);
    const uint32_t w = gCfg[(o & ~3u) / 4];
    *v = size == 4 ? w : size == 2 ? (w >> ((o & 2) * 8)) & 0xffff : (w >> ((o & 3) * 8)) & 0xff;
    return 0;
}
int pCfgWrite(int off, int size, uint32_t v) {
    uint32_t o = (uint32_t)off;
    if (o & 0x80000000u) o = gPcieCap + (o & 0x7fffffffu);
    if (size == 4) {gCfg[o / 4] = v; return 0;}
    const unsigned shift = (size == 2 ? (o & 2) : (o & 3)) * 8;
    const uint32_t mask = (size == 2 ? 0xffffu : 0xffu) << shift;
    gCfg[(o & ~3u) / 4] = (gCfg[(o & ~3u) / 4] & ~mask) | ((v << shift) & mask);
    return 0;
}
int pMsiTarget(uint32_t *lo, uint32_t *hi, uint32_t *data) {*lo = (uint32_t)gArena.iova; *hi = (uint32_t)(gArena.iova >> 32); *data = 0; return 0;}

// The endpoint's MSIs: the root complex's own MSI receiver takes inbound writes to MSI_ADDR (the arena's first page), sets bit <data> of
// controller 0's status and raises PCIe0's msi0 line (SPI 141). This kernel has no GIC attach, so the handler runs from the 2 ms poller
// (armcpu_spi_attach in HoujiAdsp.cpp); it clears the status and wakes the Wi-Fi thread.
constexpr uint32_t kMsiAddrLo = 0x820, kMsiAddrHi = 0x824, kMsiEnable0 = 0x828, kMsiMask0 = 0x82c, kMsiStatus0 = 0x830, kMsiSpi = 141;
volatile uint32_t *gMsiDbi;
volatile uint32_t gMsiCount, gMsiBits, gMsiWake;
bool gMsiAttached;
void msiIrq(void *) {
    const uint32_t status = gMsiDbi[kMsiStatus0 / 4];
    if (!status) return;
    gMsiDbi[kMsiStatus0 / 4] = status;
    gMsiCount = gMsiCount + 1;
    gMsiBits = gMsiBits | status;
    gMsiWake = 1;
    thread_wakeup(event_t(&gMsiWake));
}
int msiAttach(volatile uint32_t *dbi) {
    gMsiDbi = dbi;
    dbi[kMsiAddrLo / 4] = uint32_t(gArena.iova);
    dbi[kMsiAddrHi / 4] = uint32_t(gArena.iova >> 32);
    dbi[kMsiMask0 / 4] = 0;
    dbi[kMsiEnable0 / 4] = 0xffffffffu;
    const int rc = armcpu_spi_attach(32 + kMsiSpi, false, msiIrq, nullptr, nullptr, nullptr);
    gMsiAttached = rc == 0;
    return rc;
}
void msiWait(unsigned ms) {
    if (__atomic_exchange_n(&gMsiWake, 0u, __ATOMIC_ACQ_REL)) return;
    assert_wait_timeout(event_t(&gMsiWake), THREAD_UNINT, ms, NSEC_PER_MSEC);
    if (__atomic_exchange_n(&gMsiWake, 0u, __ATOMIC_ACQ_REL)) {clear_wait(current_thread(), THREAD_AWAKENED); return;}
    thread_block(THREAD_CONTINUE_NULL);
    gMsiWake = 0;
}

// Requests from the USB console (houji_wifi_command), taken by the Wi-Fi thread between steps.
struct Command {char text[128]; volatile uint32_t pending;} gCommand;
volatile uint32_t gScanDone;
void onScanDone() {gScanDone = 1;}
void onConnectDone(int result) {wlog(5, "wifi: connect done: result %d", result);}
void onLinkLost(int reason) {wlog(5, "wifi: link lost: reason %d", reason);}
void onRx(const uint8_t *, size_t) {}
void listScan() {
    const int n = nw_wifi_bss_count();
    wlog(5, "wifi: scan found %d networks", n);
    nw_bss_info *b = static_cast<nw_bss_info *>(IOMallocData(sizeof(nw_bss_info)));
    if (!b) return;
    for (int i = 0; i < n && i < 40; ++i) {
        if (nw_wifi_bss_get(i, b)) continue;
        char ssid[33];
        memcpy(ssid, b->ssid, b->ssid_len > 32 ? 32 : b->ssid_len); ssid[b->ssid_len > 32 ? 32 : b->ssid_len] = 0;
        wlog(5, "wifi: bss %02x:%02x:%02x:%02x:%02x:%02x %u MHz %d dBm '%s'", b->bssid[0], b->bssid[1], b->bssid[2], b->bssid[3], b->bssid[4],
             b->bssid[5], b->freq_mhz, b->signal_dbm, ssid);
    }
    IOFreeData(b, sizeof(nw_bss_info));
}
void runCommand(char *t) {
    // "scan", "join SSID PASSWORD" (no password: an open network), "leave", "status"
    char *argv[3] = {}; unsigned argc = 0;
    for (char *p = t; *p && argc < 3;) {
        while (*p == ' ') *p++ = 0;
        if (!*p) break;
        argv[argc++] = p;
        if (argc == 3) break;
        while (*p && *p != ' ') ++p;
    }
    if (!argc) return;
    if (!strcmp(argv[0], "scan")) wlog(5, "wifi: scan requested: %d", nw_wifi_scan(nullptr, 0));
    else if (!strcmp(argv[0], "join") && argc >= 2) {
        const int r = nw_wifi_connect((const uint8_t *)argv[1], strlen(argv[1]), nullptr, argc == 3 ? argv[2] : nullptr, argc == 3 ? strlen(argv[2]) : 0);
        wlog(5, "wifi: join '%s' (%s) requested: %d", argv[1], argc == 3 ? "WPA2" : "open", r);
    } else if (!strcmp(argv[0], "leave")) {nw_wifi_disconnect(); wlog(5, "wifi: leave requested");}
    else if (!strcmp(argv[0], "status")) {
        nw_wifi_status st; nw_wifi_status_get(&st);
        char ssid[33];
        memcpy(ssid, st.ssid, st.ssid_len > 32 ? 32 : st.ssid_len); ssid[st.ssid_len > 32 ? 32 : st.ssid_len] = 0;
        wlog(5, "wifi: state %u result %d '%s' %u MHz %d dBm mac %02x:%02x:%02x:%02x:%02x:%02x channels %u", st.state, st.result, ssid,
             st.freq_mhz, st.signal_dbm, st.mac[0], st.mac[1], st.mac[2], st.mac[3], st.mac[4], st.mac[5], st.channels);
    } else wlog(4, "wifi: unknown command '%s'", argv[0]);
    bzero(t, sizeof gCommand.text);   // a password does not stay in memory
}

void wifiStack(volatile uint32_t *m, uint64_t barBytes, volatile uint32_t *cfg, volatile uint32_t *dbi) {
    gCfg = cfg;
    PE_parse_boot_argn("houji_wifi_debug", &gDebug, sizeof gDebug);
    if (!arenaInit()) {native_wifi_stats[0] = (uint32_t)-20; return;}
    uint32_t route[3] = {};
    const char *why = houji_smmu_route_stream(0x1401, gArenaPa, gArena.bytes, route);
    if (!why && gTranslated) why = arenaMapStage1(route[0]);
    wlog(5, "wifi: DMA window %zu MiB at %llx (bus %llx, %s), stream 0x1401 route %s slot %u bank %u", gArena.bytes >> 20,
         (unsigned long long)gArenaPa, (unsigned long long)gArena.iova, gTranslated ? "stage 1 table" : "pass-through", why ? why : "ok",
         route[0] & 0xffff, route[0] >> 16);
    if (why) {native_wifi_stats[0] = (uint32_t)-21; return;}
    if (!findCaps()) {native_wifi_stats[0] = (uint32_t)-22; return;}
    static const struct lx_platform plat = {pNow, pSleep, pPoll, pLog, pHalt, hAlloc, hFree, pDmaAlloc, pDmaFree, pDmaMap, pDmaSync, pFwGet};
    static struct nw_board board;
    board.bar = const_cast<uint32_t *>(m); board.bar_len = barBytes; board.device_id = (uint16_t)(gCfg[0] >> 16); board.msi_cap = (uint16_t)gMsiCap;
    board.irq_base = 1000;
    board.cfg_read = pCfgRead; board.cfg_write = pCfgWrite; board.msi_target = pMsiTarget; board.map_phys = nullptr;
    static const struct nw_wifi_events events = {onScanDone, onConnectDone, onLinkLost, onRx};
    nw_wifi_set_events(&events);
    native_wifi_stats[0] = 20;
    const int r = nw_wifi_start(&plat, &board);
    native_wifi_stats[0] = r ? (uint32_t)(0x80000000u | (uint32_t)(-r & 0xffff)) : 21;
    wlog(5, "wifi: module start %d", r);
    if (r) return;
    const int msi = msiAttach(dbi);
    wlog(6, "wifi: MSI receiver %s", msi ? "not attached, polling" : "attached (SPI 141 through the 2 ms poller)");
    bool scanned = false;
    uint64_t nextReport = 0;
    for (uint64_t n = 0;; ++n) {
        const int busy = nw_wifi_step();
        if (!scanned && nw_wifi_stats.stage >= 4) {
            scanned = true;
            unsigned net = 1;
            PE_parse_boot_argn("houji_wifi_net", &net, sizeof net);
            if (net) {nw_wifi_status st; nw_wifi_status_get(&st); sheng_wifi_net_start(st.mac);}   // en0; its events chain to the ones above
            wlog(5, "wifi: driver up, first scan: %d", nw_wifi_scan(nullptr, 0));
        }
        if (__atomic_exchange_n(&gScanDone, 0u, __ATOMIC_ACQ_REL)) listScan();
        if (__atomic_load_n(&gCommand.pending, __ATOMIC_ACQUIRE)) {runCommand(gCommand.text); __atomic_store_n(&gCommand.pending, 0u, __ATOMIC_RELEASE);}
        if (!busy) {if (gMsiAttached) msiWait(10); else IOSleep(1);}
        const uint64_t now = pNow();
        if (now >= nextReport) {
            nextReport = now + 30000;
            wlog(6, "wifi: stage %u scans %u frames %u beacons %u bss %u err %d; %u MSIs, vectors %08x, %u repeats",
                 nw_wifi_stats.stage, nw_wifi_stats.scans, nw_wifi_stats.frames, nw_wifi_stats.beacons, nw_wifi_stats.bss_seen,
                 nw_wifi_stats.last_err, gMsiCount, gMsiBits, gLogRepeats);
        }
        if ((n & 63) == 0) native_wifi_stats[1] = nw_wifi_stats.stage | nw_wifi_stats.scans << 8 | nw_wifi_stats.bss_seen << 16;
        // Power save while the screen sits untouched for a minute (the radio dozes between beacons: about 100 ms more latency, much
        // less heat); off at the next touch. Only on a connected link (stage 8); houji_wifi_ps=0 keeps it off.
        static int psOn = 0; static uint64_t psCheck;
        if (now >= psCheck) {
            psCheck = now + 1000;
            unsigned allow = 1; PE_parse_boot_argn("houji_wifi_ps", &allow, sizeof allow);
            const uint64_t touch = houji_dvfs_touch_time();
            uint64_t idleNs = UINT64_MAX;
            if (touch) absolutetime_to_nanoseconds(mach_absolute_time() - touch, &idleNs);
            const int want = allow && nw_wifi_stats.stage >= 8 && idleNs > 60ull * 1000000000ull;
            if (nw_wifi_stats.stage < 8) psOn = 0;   // a new join starts with power save off (patch 0002)
            else if (want != psOn) {nw_wifi_ps_set(want); psOn = want;}
        }
    }
}

// Stage 4: forward the memory window to bus 1, give BAR0 an address in it, memory decoding and bus mastering on, read the MHI registers.
void endpointProbe(volatile uint32_t *a, volatile uint32_t *c, volatile uint32_t *d, unsigned stage) {
    volatile uint32_t *r1 = a + 0x200 / 4;   // outbound iATU region 1: memory, the same PCI addresses
    r1[0x08 / 4] = uint32_t(kMemBase);
    r1[0x0c / 4] = uint32_t(kMemBase >> 32);
    r1[0x10 / 4] = uint32_t(kMemBase + kMemSize - 1);
    r1[0x20 / 4] = uint32_t((kMemBase + kMemSize - 1) >> 32);
    r1[0x14 / 4] = uint32_t(kMemBase);
    r1[0x18 / 4] = uint32_t(kMemBase >> 32);
    r1[0x00 / 4] = 0x0;
    r1[0x04 / 4] = 1u << 31;
    for (unsigned i = 0; i < 5 && !(r1[0x04 / 4] & (1u << 31)); ++i) IODelay(10);
    d[0x20 / 4] = uint32_t(((kMemBase + kMemSize - 1) & 0xfff00000u) | ((kMemBase >> 16) & 0xfff0u));
    const uint32_t saved = c[0x10 / 4];
    c[0x10 / 4] = 0xffffffffu;
    const uint32_t mask = c[0x10 / 4];
    c[0x10 / 4] = saved;
    native_wifi_stats[22] = mask;
    const uint32_t size = ~(mask & ~0xfu) + 1;
    if (!(mask & ~0xfu) || !size || size > kMemSize) {native_wifi_stats[0] = (uint32_t)-18; return;}
    const uint32_t address = uint32_t((kMemBase + size - 1) & ~uint64_t(size - 1));
    c[0x10 / 4] = address | (mask & 0xfu);
    if ((mask & 6u) == 4u) c[0x14 / 4] = 0;
    c[0x04 / 4] = (c[0x04 / 4] & 0xffffu) | 0x6u;
    native_wifi_stats[23] = c[0x10 / 4];
    native_wifi_stats[24] = c[0x04 / 4];
    Window bar = {};
    if (!mapWindow(bar, address, size < 0x200000 ? size : 0x200000)) {native_wifi_stats[0] = (uint32_t)-19; return;}
    static const uint32_t regs[7] = {0x00, 0x08, 0x10, 0x28, 0x2c, 0x38, 0x48};
    for (unsigned i = 0; i < 7; ++i) native_wifi_stats[25 + i] = bar.va[regs[i] / 4];
    native_wifi_stats[0] = 19;
    if (stage >= 5) {wifiStack(bar.va, size < 0x200000 ? size : 0x200000, c, d); return;}   // runs for good: the windows stay mapped
    release(bar);
}

void bringUp(Window &gcc, Window &tlmm, unsigned stage) {
    volatile uint32_t *g = gcc.va, *t = tlmm.va;
    const int rails = powerRails();
    IOSleep(20);
    if (rails) {native_wifi_stats[0] = (uint32_t)(-9 * 100 + rails); return;}
    // PERST# held low, CLKREQ# to its PCIe function, then WLAN_EN low (a clean start) and high, and the WCN7850 power-up delay
    setPin(t, kPerst, 1u | 1u << 9, 0);
    setPin(t, kClkReq, 3u | 1u << 2, -1);
    setPin(t, kWlanEn, 1u | 7u << 6 | 1u << 9, 0);
    IOSleep(150);
    setPin(t, kWlanEn, 1u | 7u << 6 | 1u << 9, 1);
    IOSleep(60);
    native_wifi_stats[10] = pinWord(t);
    native_wifi_stats[0] = 10;

    g[kCollapse / 4] &= ~(1u << 0 | 1u << 3);
    unsigned tries = 0;
    while (tries++ < 100 && (!(g[kGdsc / 4] & (1u << 31)) || !(g[kPhyGdsc / 4] & (1u << 31)))) IODelay(10);
    const uint32_t power = (g[kGdsc / 4] >> 31) | (g[kPhyGdsc / 4] >> 31) << 1;
    native_wifi_stats[11] = power;
    if (power != 3) {native_wifi_stats[0] = (uint32_t)-11; return;}
    native_wifi_stats[0] = 11;

    g[(kAuxRcg + 4) / 4] = 0;              // TCXO
    g[kAuxRcg / 4] |= 1u;
    g[(kRchngRcg + 4) / 4] = 6u << 8 | 5u;   // GPLL0 even / 3: 100 MHz
    g[kRchngRcg / 4] |= 1u;
    waitClear(&g[kAuxRcg / 4], 1u, 100);
    waitClear(&g[kRchngRcg / 4], 1u, 100);
    g[kPipeMux / 4] = (g[kPipeMux / 4] & ~3u) | 2u;
    g[kVote8 / 4] |= kVote8Bits;
    g[kVote0 / 4] |= kVote0Bits;
    Window tcsr = {};
    if (!mapWindow(tcsr, kTcsr + kTcsrPcie0Clkref, 0x1000)) {native_wifi_stats[0] = (uint32_t)-12; return;}
    tcsr.va[0] |= 1u;
    release(tcsr);
    IODelay(200);
    uint32_t off = 0;
    for (unsigned i = 0; i < sizeof kBranches / sizeof kBranches[0]; ++i)
        if (g[kBranches[i] / 4] & (1u << 31)) off |= 1u << i;
    native_wifi_stats[11] = power | off << 8;
    // The pipe branch runs only once the PHY drives it. On SM8650 the NoC branches (cnoc_sf voted, aggre_noc and ddrss_sf halt-skip in
    // gcc-sm8650.c) read off here, as Linux never checks them; only the controller's own branches must run before its registers are read.
    if (off & kNeeded) {native_wifi_stats[0] = (uint32_t)-12; return;}
    pulseReset(g, kPcieBcr);
    pulseReset(g, kPhyBcr);
    native_wifi_stats[0] = 12;

    Window parf = {}, dbi = {};
    if (!mapWindow(parf, kParf, 0x3000) || !mapWindow(dbi, kDbi, 0x1000)) {release(parf); native_wifi_stats[0] = (uint32_t)-13; return;}
    volatile uint32_t *f = parf.va, *d = dbi.va;
    f[kParfDeviceType / 4] = 4u;
    f[kParfPhyCtrl / 4] &= ~1u;
    f[kParfDbiBase / 4] = uint32_t(kDbi);
    f[kParfDbiBaseHi / 4] = uint32_t(kDbi >> 32);
    f[kParfAtuBase / 4] = uint32_t(kAtu);
    f[kParfAtuBaseHi / 4] = uint32_t(kAtu >> 32);
    f[kParfSlvSpace / 4] = 0;
    f[kParfSlvSpaceHi / 4] = kSlvSpaceBytes;
    f[kParfSysCtrl / 4] &= ~(1u << 29);
    f[kParfMhiClockReset / 4] |= 1u << 4;
    f[kParfPmCtrl / 4] &= ~(1u << 5);
    f[kParfAxiHalt / 4] |= 1u << 31;
    native_wifi_stats[0] = 13;

    Window phy = {};
    if (!mapWindow(phy, kPhy, 0x2000)) {release(dbi); release(parf); native_wifi_stats[0] = (uint32_t)-14; return;}
    volatile uint32_t *p = phy.va;
    using namespace wifi_phy;
    p[kPowerDownControl / 4] |= 3u;
    for (const Init &e : kSerdes) p[e.offset / 4] = e.value;
    for (const Init &e : kTx) {p[e.offset / 4] = e.value; p[(e.offset + kTx2) / 4] = e.value;}
    for (const Init &e : kRx) {p[e.offset / 4] = e.value; p[(e.offset + kRx2) / 4] = e.value;}
    for (const Init &e : kPcs) p[e.offset / 4] = e.value;
    for (const Init &e : kPcsMisc) p[e.offset / 4] = e.value;
    g[kPipeMux / 4] &= ~3u;
    p[kSwReset / 4] &= ~1u;
    p[kStartControl / 4] |= 3u;
    IODelay(1000);
    unsigned polls = 0;
    const bool ready = waitClear(&p[kPcsStatus1 / 4], 1u << 6, 100, &polls);
    native_wifi_stats[12] = (p[kPcsStatus1 / 4] & 0xffffu) | polls << 16;
    native_wifi_stats[21] = g[kBranches[4] / 4];
    release(phy);
    if (!ready) {release(dbi); release(parf); native_wifi_stats[0] = (uint32_t)-14; return;}

    // qcom_pcie_config_sid_1_9_0: requester 0x100 (the endpoint) -> stream offset 1 (0x1401); CRC-8 (poly 7) of the requester ID's bytes
    for (unsigned i = 0; i < 256; ++i) f[(0x2000 + 4 * i) / 4] = 0;
    f[0x2c00 / 4] &= ~1u;
    {
        uint8_t crc = 0;
        const uint8_t bytes[2] = {0x01, 0x00};
        for (uint8_t byte : bytes) {
            crc ^= byte;
            for (int bit = 0; bit < 8; ++bit) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 7) : (uint8_t)(crc << 1);
        }
        f[(0x2000 + 4 * crc) / 4] = 0x100u << 16 | 1u << 8;
        __asm__ volatile("dsb sy" ::: "memory");
        native_wifi_stats[20] = crc | f[0x2c00 / 4] << 8;
    }

    d[kBusNumbers / 4] = 0x00010100;
    d[kCommand / 4] |= 0x6u;
    native_wifi_stats[7] = d[0];
    native_wifi_stats[0] = 14;

    IOSleep(100);
    setPin(t, kPerst, 1u | 1u << 9, 1);
    IODelay(1000);
    native_wifi_stats[0] = 15;
    f[kParfLtssm / 4] |= 1u << 8;
    const uint64_t start = mach_absolute_time();
    unsigned ms = 0;
    for (; ms < 1000; ++ms) {
        if ((d[kPortDebug1 / 4] & (1u << 4)) && (d[kPortDebug0 / 4] & 0x3f) == 0x11) break;
        IOSleep(1);
    }
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
    native_wifi_stats[16] = uint32_t(ns / 1000000);
    native_wifi_stats[13] = d[kPortDebug0 / 4];
    native_wifi_stats[14] = d[kPortDebug1 / 4];
    native_wifi_stats[6] = f[kParfLtssm / 4];
    native_wifi_stats[8] = d[kLinkControl / 4];
    native_wifi_stats[9] = f[kParfPhyCtrl / 4];
    if (ms == 1000) {release(dbi); release(parf); native_wifi_stats[0] = (uint32_t)-16; return;}
    native_wifi_stats[0] = 16;
    if (stage < 3) {release(dbi); release(parf); return;}

    Window atu = {}, config = {};
    if (mapWindow(atu, kAtu, 0x1000) && mapWindow(config, kConfig, 0x1000)) {
        volatile uint32_t *a = atu.va;
        a[0x08 / 4] = uint32_t(kConfig);
        a[0x0c / 4] = uint32_t(kConfig >> 32);
        a[0x10 / 4] = uint32_t(kConfig + 0xfffff);
        a[0x20 / 4] = uint32_t((kConfig + 0xfffff) >> 32);
        a[0x14 / 4] = 1u << 24;
        a[0x18 / 4] = 0;
        a[0x00 / 4] = 0x4;
        a[0x04 / 4] = 1u << 31;
        for (unsigned i = 0; i < 5 && !(a[0x04 / 4] & (1u << 31)); ++i) IODelay(10);
        native_wifi_stats[15] = config.va[0];
        native_wifi_stats[0] = 17;
        if (stage >= 4 && (config.va[0] & 0xffff) == 0x17cb) endpointProbe(a, config.va, d, stage);
    } else native_wifi_stats[0] = (uint32_t)-17;
    release(config); release(atu); release(dbi); release(parf);
}

void wifiThread(void *arg, wait_result_t) {
    thread_set_thread_name(current_thread(), "houji-wifi");
    const unsigned stage = (unsigned)reinterpret_cast<uintptr_t>(arg);
    unsigned delay = 50;
    PE_parse_boot_argn("houji_wifi_delay", &delay, sizeof delay);
    IOSleep(delay * 1000);
    Window gcc = {}, tlmm = {};
    if (!mapWindow(gcc, kGcc, 0x80000) || !mapWindow(tlmm, kTlmm, 0x100000)) {native_wifi_stats[0] = (uint32_t)-1; release(gcc); release(tlmm); return;}
    native_wifi_stats[0] = 1;
    native_wifi_stats[1] = gcc.va[kVote8 / 4];
    native_wifi_stats[2] = gcc.va[kVote0 / 4];
    native_wifi_stats[3] = gcc.va[kGdsc / 4];
    uint32_t off = 0;
    for (unsigned i = 0; i < sizeof kBranches / sizeof kBranches[0]; ++i)
        if (gcc.va[kBranches[i] / 4] & (1u << 31)) off |= 1u << i;
    native_wifi_stats[4] = off;
    native_wifi_stats[5] = pinWord(tlmm.va);
    native_wifi_stats[0] = 2;
    if (stage >= 2) bringUp(gcc, tlmm, stage);
    release(tlmm); release(gcc);
    thread_terminate(current_thread());
}
}  // namespace

// launchd loads the wireless daemon only on hardware whose device tree has a node named wlan (its _LimitLoadToDeviceTree), as Apple's
// boot loaders describe the radio at /arm-io/wlan. Added here, during bsd_init and so before launchd reads its job list, with the radio's
// address, which the daemon also wants in /chosen (mac-address-wifi0) to derive the private addresses of networks.
static void publishWlanNode(void) {
    uint8_t mac[6];
    boardMac(mac);
    if (IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane)) {
        if (!chosen->getProperty("mac-address-wifi0")) chosen->setProperty("mac-address-wifi0", mac, sizeof mac);
        chosen->release();
    }
    IORegistryEntry *armio = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
    if (!armio) {IOLog("HOUJI WIFI: no /arm-io in the device tree; the wireless daemon will not load\n"); return;}
    if (IORegistryEntry *existing = IORegistryEntry::fromPath("/arm-io/wlan", gIODTPlane)) {existing->release(); armio->release(); return;}
    IORegistryEntry *wlan = OSTypeAlloc(IORegistryEntry);
    if (wlan && wlan->init()) {
        static const char name[] = "wlan", compatible[] = "wlan-pcie,wcn7850", type[] = "wlan";
        wlan->setName(name);
        wlan->setProperty("name", (void *)name, sizeof name);
        wlan->setProperty("compatible", (void *)compatible, sizeof compatible);
        wlan->setProperty("device_type", (void *)type, sizeof type);
        wlan->setProperty("local-mac-address", mac, sizeof mac);
        const bool attached = wlan->attachToParent(armio, gIODTPlane);
        IOLog("HOUJI WIFI: /arm-io/wlan %s, address %02x:%02x:%02x:%02x:%02x:%02x\n", attached ? "added" : "not attached", mac[0], mac[1], mac[2],
              mac[3], mac[4], mac[5]);
    }
    OSSafeReleaseNULL(wlan);
    armio->release();
}

extern "C" void houji_wifi_start(void) {
    unsigned stage = 5;   // Wi-Fi on unless houji_wifi=0 (or a lower stage for bring-up work)
    PE_parse_boot_argn("houji_wifi", &stage, sizeof stage);
    if (!stage || native_wifi_stats[0]) return;
    if (stage >= 5) publishWlanNode();
    thread_t thread = nullptr;
    if (kernel_thread_start(wifiThread, reinterpret_cast<void *>(uintptr_t(stage)), &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else native_wifi_stats[0] = (uint32_t)-7;
}

extern "C" void houji_wifi_report(void) {
    const volatile uint32_t *s = native_wifi_stats;
    if (!s[0]) return;
    IOLog("HOUJI WIFI: stage=%d votes=%x/%x gdsc=%x off=%x pins=%x/%x power=%x phy=%x ltssm=%x/%x link-ms=%u rp=%x ep=%x\n",
          (int)s[0], s[1], s[2], s[3], s[4], s[5], s[10], s[11], s[12], s[13], s[14], s[16], s[7], s[15]);
    IOLog("HOUJI WIFI RAILS: found=%x clk=%x rsc=%x sid=%x pipe=%x lnk=%x parf=%x/%x bar=%x/%x cmd=%x mhi=%x %x %x %x %x %x %x\n",
          s[17], s[18], s[19], s[20], s[21], s[8], s[6], s[9], s[22], s[23], s[24], s[25], s[26], s[27], s[28], s[29], s[30], s[31]);
}

// A request from the USB console ("wifi scan", "wifi join SSID PASSWORD", "wifi leave", "wifi status"): handed to the Wi-Fi thread.
extern "C" void houji_wifi_command(const char *text) {
    if (__atomic_load_n(&gCommand.pending, __ATOMIC_ACQUIRE) || native_wifi_stats[0] != 21) {IOLog("HOUJI WIFI: not ready for a command\n"); return;}
    strlcpy(gCommand.text, text, sizeof gCommand.text);
    __atomic_store_n(&gCommand.pending, 1u, __ATOMIC_RELEASE);
    gMsiWake = 1;
    thread_wakeup(event_t(&gMsiWake));
}

// For WifiNet.cpp: its log lines, a prompt send of a queued frame, and the module's events it receives once en0 exists (they keep this
// file's log and the console's scan listing).
extern "C" void native_wifi_logf(int level, const char *fmt, ...) {
    char line[160]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    pLog(level, line);
}
extern "C" void sheng_wifi_wake(void) {gMsiWake = 1; thread_wakeup(event_t(&gMsiWake));}
extern "C" void houji_wifi_on_scan_done(void) {onScanDone();}
extern "C" void houji_wifi_on_connect_done(int result) {onConnectDone(result);}
extern "C" void houji_wifi_on_link_lost(int reason) {onLinkLost(reason);}
