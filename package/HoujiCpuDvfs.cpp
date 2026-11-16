// SPDX-License-Identifier: BSD-2-Clause-Patent
// houji CPU frequency for iOS 27: the iOS 16 kernel's SM8650 EPSS governor (fork/kernel/overlay/iokit/Kernel/arm/HoujiCpuDvfs.inc;
// register facts from MiCode qcom-cpufreq-hw, policy from UniXNU-sheng CpuFreqProbe). The factory LUT validation (init) and the
// request/readback vote are unchanged; no raw voltage or PLL writes, EPSS keeps voltage control. This first iOS 27 version has no load
// input yet (the iOS 27 kernel does not publish per-core idle time): every 200 ms each domain is set to the lower of its thermal cap
// (the iOS 16 levels: triggers 70/75/80/85/90 C, big 100/70/50/30/10/0 %, little 100/90/70/50/25/0 %, 3 C hysteresis) and
// houji_cpu_max_pct (default 70); triggers from houji_thermal_trigger_c (default 55 C). Opt-in: houji_dvfs=1. Domains and caps are this phone's FDT and recovery cpufreq inventory.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>

extern "C" int houji_thermal_hottest(void);   // HoujiThermal.cpp: deci-C, -1000 without a reading

namespace {
constexpr unsigned kDomains = 4, kRows = 40;
constexpr unsigned kCounts[kDomains] = {2, 2, 1, 3};
constexpr uint32_t kCaps[kDomains] = {2265600, 2956800, 3052800, 3148800};
constexpr uint32_t kMinima[kDomains] = {364800, 499200, 480000, 499200};
// Level triggers in deci-C: level n starts at first + 50 * (n - 1). The iOS 16 governor began at 70 C with a load-driven floor;
// without load input yet the iOS 27 one begins at houji_thermal_trigger_c (default 55 C), so all-core boot load is throttled early.
int kTrigger[] = {0, 550, 600, 650, 700, 750};
constexpr unsigned kBigPct[] = {100, 70, 50, 30, 10, 0};
constexpr unsigned kLittlePct[] = {100, 90, 70, 50, 25, 0};
constexpr unsigned kLevels = sizeof(kTrigger) / sizeof(kTrigger[0]);

struct Domain {
    IOMemoryMap *map; uintptr_t va; uint32_t khz[kRows]; uint8_t index[kRows];
    unsigned n, cur, changes, initial; bool bad, ok;
};
Domain d[kDomains];
unsigned gMaxPct = 70;
volatile unsigned gLevel, gHottest, gStarted;

uint32_t rd(Domain &w, unsigned off) {return *(volatile uint32_t *)(w.va + off);}

bool init(unsigned id) {
    Domain &w = d[id];
    if (!w.va) {   // mapped once; a domain whose cluster is still off is validated again later
        IODeviceMemory *m = IODeviceMemory::withRange(0x17d91000ULL + id * 0x1000, 0x1000);
        w.map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        if (m) m->release();
        w.va = w.map ? w.map->getVirtualAddress() : 0;
    }
    w.n = 0;
    if (!w.va) {IOLog("HOUJI CPU DVFS REFUSED: domain=%u not mapped\n", id); return false;}
    if (!(rd(w, 0) & 1)) {IOLog("HOUJI CPU DVFS REFUSED: domain=%u enable %x clear\n", id, rd(w, 0)); return false;}
    uint32_t prev = 0;
    for (unsigned i = 0; i < kRows; i++) {
        const uint32_t raw = rd(w, 0x100 + i * 4), src = raw >> 30, khz = (raw & 255) * 19200;
        if (!src || khz < kMinima[id] || khz > 4000000) {
            IOLog("HOUJI CPU DVFS REFUSED: domain=%u row %u raw %08x (%u kHz, source %u; minimum %u)\n", id, i, raw, khz, src, kMinima[id]);
            return false;
        }
        if (i && khz == prev) break;
        if (i && khz < prev) {IOLog("HOUJI CPU DVFS REFUSED: domain=%u row %u %u kHz below %u\n", id, i, khz, prev); return false;}
        prev = khz;
        if (khz > kCaps[id]) continue;
        w.khz[w.n] = khz; w.index[w.n++] = (uint8_t)i;
    }
    if (w.n < 2 || w.khz[0] != kMinima[id] || w.khz[w.n - 1] != kCaps[id]) {
        IOLog("HOUJI CPU DVFS REFUSED: domain=%u %u rows, first %u kHz (want %u), last %u kHz (want cap %u)\n", id, w.n, w.n ? w.khz[0] : 0,
              kMinima[id], w.n ? w.khz[w.n - 1] : 0, kCaps[id]);
        return false;
    }
    w.initial = rd(w, 0x320);
    if (w.initial >= kRows) {IOLog("HOUJI CPU DVFS REFUSED: domain=%u initial index %u\n", id, w.initial); return false;}
    w.ok = true;
    w.cur = w.n - 1;
    IOLog("HOUJI CPU DVFS INIT: domain=%u cpus=%u rows=%u cap-kHz=%u initial-index=%u initial-request-kHz=%u control=%x; factory LUT, "
          "EPSS voltage control preserved\n", id, kCounts[id], w.n, kCaps[id], w.initial, (rd(w, 0x100 + w.initial * 4) & 255) * 19200,
          rd(w, 0xb0));
    return true;
}

bool vote(unsigned id, unsigned at) {
    Domain &w = d[id];
    if (at >= w.n || !(rd(w, 0) & 1)) return false;
    const unsigned n = (rd(w, 0xb0) & 1) ? kCounts[id] : 1;
    for (unsigned c = 0; c < n; c++) *(volatile uint32_t *)(w.va + 0x320 + c * 4) = w.index[at];
    __asm__ volatile("dsb sy" ::: "memory");
    for (unsigned c = 0; c < n; c++) if (rd(w, 0x320 + c * 4) != w.index[at]) return false;
    w.cur = at; ++w.changes;
    return true;
}

unsigned capIndex(const Domain &w, unsigned pct) {
    unsigned at = 0;
    while (at + 1 < w.n && (uint64_t)w.khz[at + 1] * 100 <= (uint64_t)w.khz[w.n - 1] * pct) ++at;
    return at;
}

unsigned nextLevel(unsigned cur, int hottest) {
    unsigned want = 0;
    while (want + 1 < kLevels && hottest >= kTrigger[want + 1]) ++want;
    if (want >= cur) return want;
    if (hottest < kTrigger[cur] - 30) return cur - 1;
    return cur;
}

void apply(unsigned level) {
    for (unsigned id = 0; id < kDomains; id++) {
        Domain &w = d[id];
        if (!w.ok || w.bad) continue;
        unsigned pct = id ? kBigPct[level] : kLittlePct[level];
        if (pct > gMaxPct) pct = gMaxPct;
        const unsigned at = capIndex(w, pct);
        if (at != w.cur && !vote(id, at)) {
            w.bad = true;
            IOLog("HOUJI CPU DVFS FAIL: domain=%u request/readback mismatch; domain writes disabled\n", id);
        }
    }
}

void worker(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "houji-cpu-dvfs");
    unsigned ticks = 0;
    for (;;) {
        const int hottest = houji_thermal_hottest();
        // Without a reading, hold level 2 (big cores at half), as the iOS 16 governor did.
        const unsigned level = hottest > -1000 ? nextLevel(gLevel, hottest) : (gLevel < 2 ? 2 : gLevel);
        if (level != gLevel) IOLog("HOUJI CPU DVFS: thermal level %u -> %u at %d.%d C\n", gLevel, level, hottest / 10, hottest % 10);
        gLevel = level; gHottest = (unsigned)hottest;
        // A cluster brought up later (smp=1 starts the secondary cores after this governor) enables its EPSS domain then.
        if (ticks % 5 == 0) {
            for (unsigned id = 0; id < kDomains; id++) {
                if (d[id].ok || !d[id].va || !(rd(d[id], 0) & 1)) continue;
                if (init(id)) IOLog("HOUJI CPU DVFS: domain=%u validated after its cluster started\n", id);
            }
        }
        apply(level);
        if (++ticks % 25 == 0) {   // every 5 s
            uint32_t req[kDomains], state[kDomains];
            for (unsigned id = 0; id < kDomains; id++) {
                req[id] = d[id].ok ? d[id].khz[d[id].cur] : 0;
                state[id] = d[id].ok ? (rd(d[id], 0x20) & 255) * 19200 : 0;
            }
            IOLog("HOUJI CPU DVFS: hottest %d.%d C level %u req-kHz %u,%u,%u,%u state-kHz %u,%u,%u,%u changes %u,%u,%u,%u\n",
                  hottest / 10, hottest % 10, level, req[0], req[1], req[2], req[3], state[0], state[1], state[2], state[3],
                  d[0].changes, d[1].changes, d[2].changes, d[3].changes);
        }
        IOSleep(200);
    }
}
}  // namespace

extern "C" void houji_cpu_dvfs_start(void) {
    unsigned enabled = 0;
    if (gStarted || !PE_parse_boot_argn("houji_dvfs", &enabled, sizeof(enabled)) || !(enabled & 1)) return;
    gStarted = 1;
    unsigned pct = 0;
    if (PE_parse_boot_argn("houji_cpu_max_pct", &pct, sizeof(pct)) && pct >= 10 && pct <= 100) gMaxPct = pct;
    unsigned first = 0;
    if (PE_parse_boot_argn("houji_thermal_trigger_c", &first, sizeof(first)) && first >= 40 && first <= 80)
        for (unsigned n = 1; n < kLevels; n++) kTrigger[n] = (int)(first * 10 + 50 * (n - 1));
    IOLog("HOUJI CPU DVFS: thermal triggers %d %d %d %d %d deci-C\n", kTrigger[1], kTrigger[2], kTrigger[3], kTrigger[4], kTrigger[5]);
    unsigned usable = 0;
    for (unsigned id = 0; id < kDomains; id++) {
        if (init(id)) ++usable;
        else IOLog("HOUJI CPU DVFS: domain=%u factory validation refused; no frequency writes to it\n", id);
    }
    if (!usable) return;
    gLevel = 2;
    apply(gLevel);
    IOLog("HOUJI CPU DVFS: started, max %u%%, initial level 2\n", gMaxPct);
    thread_t t = nullptr;
    if (kernel_thread_start(worker, nullptr, &t) == KERN_SUCCESS) thread_deallocate(t);
}

extern "C" void houji_cpu_dvfs_report(void) {
    if (!gStarted) {IOLog("HOUJI CPU DVFS: not started (houji_dvfs=1)\n"); return;}
    for (unsigned id = 0; id < kDomains; id++) {
        if (!d[id].ok) {IOLog("HOUJI CPU DVFS: d=%u not in use (validation refused)\n", id); continue;}
        IOLog("HOUJI CPU DVFS: d=%u req-kHz=%u state-kHz=%u changes=%u bad=%u\n", id, d[id].khz[d[id].cur],
              (rd(d[id], 0x20) & 255) * 19200, d[id].changes, d[id].bad);
    }
}
