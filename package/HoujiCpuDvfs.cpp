// SPDX-License-Identifier: BSD-2-Clause-Patent
// houji CPU frequency for iOS 27: the iOS 16 kernel's SM8650 EPSS governor (fork/kernel/overlay/iokit/Kernel/arm/HoujiCpuDvfs.inc;
// register facts from MiCode qcom-cpufreq-hw, policy from UniXNU-sheng CpuFreqProbe). The factory LUT validation (init) and the
// request/readback vote are unchanged; no raw voltage or PLL writes, EPSS keeps voltage control. Every 200 ms each domain gets the
// lowest row that keeps its busiest core near 75 % (per-core idle time from HoujiCpuIdle.c), never above its thermal cap
// (the iOS 16 levels: triggers 70/75/80/85/90 C, big 100/70/50/30/10/0 %, little 100/90/70/50/25/0 %, 3 C hysteresis) and
// houji_cpu_max_pct (default 70); triggers from houji_thermal_trigger_c (default 55 C). Opt-in: houji_dvfs=1. Domains and caps are this phone's FDT and recovery cpufreq inventory.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>

extern "C" int houji_thermal_hottest(void);   // HoujiThermal.cpp: deci-C, -1000 without a reading
extern "C" uint64_t houji_cpu_idle_mach(int cpu);   // HoujiCpuIdle.c: accumulated idle time, 0 for a CPU not started
extern "C" uint64_t houji_cpu_touch_time(void);   // HoujiTouchHid.c: latest down/move contact
extern "C" uint64_t houji_cpu_frame_activity(void);   // LegacyDisplayProbe.cpp: accepted frame swaps
extern "C" volatile int32_t native_houji_battery_stats[16];   // HoujiBattery.cpp; temperature [4] is in centi-C

namespace {
constexpr unsigned kDomains = 4, kRows = 40;
constexpr unsigned kCounts[kDomains] = {2, 2, 1, 3};
constexpr unsigned kCpuDomain[8] = {0, 0, 3, 3, 3, 1, 1, 2};   // the iOS 16 governor's map (this phone's FDT clusters)
constexpr uint32_t kCaps[kDomains] = {2265600, 2956800, 3052800, 3148800};
constexpr uint32_t kMinima[kDomains] = {364800, 499200, 480000, 499200};
struct StockStep { int trigger; uint32_t khz; };   // trigger is deci-C
constexpr StockStep kStockCpu0[] = {{250,2035200},{390,1689600},{400,1459200},{450,1344000},{480,1017600}};
constexpr StockStep kStockCpu2[] = {{250,2630400},{340,2323200},{370,2188800},{390,2035200},{410,1824000},{420,1612800},
    {430,1612800},{440,1286400},{450,1286400},{460,1286400},{470,1190400},{480,1190400},{490,1075200},{500,960000}};
constexpr StockStep kStockCpu5[] = {{250,2630400},{340,2323200},{370,2188800},{390,1824000},{410,1497600},{415,1401600},
    {420,1286400},{430,1286400},{450,1286400},{460,1286400},{470,1190400},{480,1190400},{490,1075200},{500,960000}};
constexpr StockStep kStockCpu7[] = {{250,2630400},{320,2169600},{340,1939200},{370,1824000},{390,1708800},{400,1363200},
    {410,1248000},{415,1248000},{420,1248000},{430,1248000},{440,1248000},{450,1132800},{460,1132800},
    {470,1017600},{480,1017600},{490,1017600},{500,1017600}};
struct StockTable { const StockStep *step; unsigned n; };
#define STOCK_TABLE(a) {a, sizeof(a) / sizeof((a)[0])}
// EPSS/log domains 0,1,2,3 contain CPUs 0-1,5-6,7,2-4 respectively.
constexpr StockTable kStockByDomain[kDomains] = {
    STOCK_TABLE(kStockCpu0), STOCK_TABLE(kStockCpu5), STOCK_TABLE(kStockCpu7), STOCK_TABLE(kStockCpu2)
};
#undef STOCK_TABLE
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
uint64_t gLastIdle[8], gLastTime;
unsigned gLoad[kDomains];   // permille: the busiest started core of the domain over the last period
volatile unsigned gLevel, gHottest, gStarted;
bool gInteractive;
bool gStock, gEmergency, gSkinFallbackLogged;
unsigned gBatteryMode, gStockState[kDomains], gStockCaps[kDomains];
int gSkinOffsetC, gSkinDeci;
unsigned gSampleMs = 200, gFramePct = 45, gTouchPct = 65, gTouchMs = 300;

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

unsigned stockState(const StockTable &table, unsigned cur, int skin) {
    // cur is the number of active entries: zero means skin is below the first trigger.
    while (cur < table.n && skin >= table.step[cur].trigger) ++cur;
    while (cur && skin < table.step[cur - 1].trigger - 20) --cur;
    return cur;
}

unsigned khzCapIndex(const Domain &w, uint32_t khz) {
    unsigned at = 0;
    while (at + 1 < w.n && w.khz[at + 1] <= khz) ++at;
    return at;
}

int stockSkin(int hottest) {
    const int temp = native_houji_battery_stats[4];
    const bool battery = gBatteryMode >= 4 && native_houji_battery_stats[6] > 0 && temp != 0 && temp >= -4000 && temp <= 9000;
    if (battery) return temp / 10 + gSkinOffsetC * 10;
    if (!gSkinFallbackLogged) {
        gSkinFallbackLogged = true;
        IOLog("HOUJI CPU DVFS: stock battery temperature unavailable; skin falls back to hottest TSENS - 25 C\n");
    }
    return hottest > -1000 ? hottest - 250 : hottest;
}

void updateStockCaps(int hottest) {
    gSkinDeci = stockSkin(hottest);
    for (unsigned id = 0; id < kDomains; ++id)
        gStockState[id] = stockState(kStockByDomain[id], gStockState[id], gSkinDeci);
}

void apply(unsigned level, bool framing, bool touching) {
    for (unsigned id = 0; id < kDomains; id++) {
        Domain &w = d[id];
        if (!w.ok || w.bad) continue;
        unsigned pct = id ? kBigPct[level] : kLittlePct[level];
        if (pct > gMaxPct) pct = gMaxPct;
        unsigned cap = capIndex(w, pct);
        if (gStock) {
            const StockTable &table = kStockByDomain[id];
            if (gStockState[id]) {
                const unsigned stockCap = khzCapIndex(w, table.step[gStockState[id] - 1].khz);
                if (stockCap < cap) cap = stockCap;
            }
            if (gEmergency) cap = 0;
            gStockCaps[id] = w.khz[cap];
        }
        // Load: above 90 % straight to the cap; otherwise the lowest row that keeps the busiest core near 75 % at the current speed.
        unsigned at = cap;
        if (gLoad[id] < 900) {
            const uint64_t want = (uint64_t)w.khz[w.cur] * gLoad[id] / 750;
            at = 0;
            while (at < cap && w.khz[at] < want) ++at;
        }
        if (gInteractive) {
            unsigned floorPct = framing ? gFramePct : 0;
            if (id && touching && floorPct < gTouchPct) floorPct = gTouchPct;
            if (floorPct > gMaxPct) floorPct = gMaxPct;
            unsigned floor = 0;
            while (floor + 1 < w.n && (uint64_t)w.khz[floor] * 100 < (uint64_t)w.khz[w.n - 1] * floorPct) ++floor;
            // Both the user cap and thermal cap are represented by cap; interaction can never override either.
            if (floor > cap) floor = cap;
            if (at < floor) at = floor;
        }
        if (at != w.cur && !vote(id, at)) {
            w.bad = true;
            IOLog("HOUJI CPU DVFS FAIL: domain=%u request/readback mismatch; domain writes disabled\n", id);
        }
    }
}

void worker(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "houji-cpu-dvfs");
    unsigned ticks = 0;
    const unsigned validateTicks = gInteractive ? (1000 / gSampleMs ? 1000 / gSampleMs : 1) : 5;
    const unsigned reportTicks = gInteractive ? (5000 / gSampleMs ? 5000 / gSampleMs : 1) : 25;
    uint64_t touchWindow = 0, frameWindow = 0, frameUntil = 0;
    uint64_t frames = gInteractive ? houji_cpu_frame_activity() : 0;
    unsigned recentFrames = 0;
    if (gInteractive) {
        nanoseconds_to_absolutetime((uint64_t)gTouchMs * 1000000ULL, &touchWindow);
        nanoseconds_to_absolutetime(300000000ULL, &frameWindow);
    }
    for (;;) {
        const int hottest = houji_thermal_hottest();
        // Without a reading, hold level 2 (big cores at half), as the iOS 16 governor did.
        const unsigned level = hottest > -1000 ? nextLevel(gLevel, hottest) : (gLevel < 2 ? 2 : gLevel);
        if (level != gLevel) IOLog("HOUJI CPU DVFS: thermal level %u -> %u at %d.%d C\n", gLevel, level, hottest / 10, hottest % 10);
        gLevel = level; gHottest = (unsigned)hottest;
        if (gStock) {
            gEmergency = hottest >= 750;
            updateStockCaps(hottest);
        }
        {
            const uint64_t now = mach_absolute_time();
            unsigned load[kDomains] = {};
            if (gLastTime) {
                const uint64_t span = now - gLastTime;
                for (int cpu = 0; cpu < 8; cpu++) {
                    const uint64_t idle = houji_cpu_idle_mach(cpu);
                    if (idle && gLastIdle[cpu] && span) {
                        const uint64_t idleSpan = idle > gLastIdle[cpu] ? idle - gLastIdle[cpu] : 0;
                        const unsigned busy = idleSpan >= span ? 0 : (unsigned)(1000 - idleSpan * 1000 / span);
                        if (busy > load[kCpuDomain[cpu]]) load[kCpuDomain[cpu]] = busy;
                    }
                    gLastIdle[cpu] = idle;
                }
                for (unsigned id = 0; id < kDomains; id++) gLoad[id] = load[id];
            } else {
                for (int cpu = 0; cpu < 8; cpu++) gLastIdle[cpu] = houji_cpu_idle_mach(cpu);
                for (unsigned id = 0; id < kDomains; id++) gLoad[id] = 1000;   // no measurement yet: the cap
            }
            gLastTime = now;
        }
        bool framing = false, touching = false;
        if (gInteractive) {
            const uint64_t now = mach_absolute_time(), nextFrames = houji_cpu_frame_activity();
            recentFrames = ((recentFrames << 1) | (nextFrames != frames)) & 15;
            frames = nextFrames;
            if (__builtin_popcount(recentFrames) >= 2) frameUntil = now + frameWindow;
            framing = now < frameUntil;
            const uint64_t touch = houji_cpu_touch_time();
            touching = touch && now >= touch && now - touch < touchWindow;
        }
        // A cluster brought up later (smp=1 starts the secondary cores after this governor) enables its EPSS domain then.
        if (ticks % validateTicks == 0) {
            for (unsigned id = 0; id < kDomains; id++) {
                if (d[id].ok || !d[id].va || !(rd(d[id], 0) & 1)) continue;
                if (init(id)) IOLog("HOUJI CPU DVFS: domain=%u validated after its cluster started\n", id);
            }
        }
        apply(level, framing, touching);
        if (++ticks % reportTicks == 0) {   // every 5 s
            uint32_t req[kDomains], state[kDomains];
            const unsigned skinAbs = gSkinDeci < 0 ? (unsigned)-gSkinDeci : (unsigned)gSkinDeci;
            const char *skinSign = gSkinDeci < 0 ? "-" : "";
            for (unsigned id = 0; id < kDomains; id++) {
                req[id] = d[id].ok ? d[id].khz[d[id].cur] : 0;
                state[id] = d[id].ok ? (rd(d[id], 0x20) & 255) * 19200 : 0;
            }
            if (gStock && gInteractive) {
                const char *floor = touching ? "touch" : (framing ? "frame" : "none");
                IOLog("HOUJI CPU DVFS: policy stock skin %s%u.%u C caps %u,%u,%u,%u hottest %d.%d C level %u load %u,%u,%u,%u floor %s req-kHz %u,%u,%u,%u state-kHz %u,%u,%u,%u changes %u,%u,%u,%u\n",
                      skinSign, skinAbs / 10, skinAbs % 10, gStockCaps[0], gStockCaps[1], gStockCaps[2], gStockCaps[3],
                      hottest / 10, hottest % 10, level, gLoad[0], gLoad[1], gLoad[2], gLoad[3], floor, req[0], req[1], req[2], req[3],
                      state[0], state[1], state[2], state[3], d[0].changes, d[1].changes, d[2].changes, d[3].changes);
            } else if (gStock) {
                IOLog("HOUJI CPU DVFS: policy stock skin %s%u.%u C caps %u,%u,%u,%u hottest %d.%d C level %u load %u,%u,%u,%u req-kHz %u,%u,%u,%u state-kHz %u,%u,%u,%u changes %u,%u,%u,%u\n",
                      skinSign, skinAbs / 10, skinAbs % 10, gStockCaps[0], gStockCaps[1], gStockCaps[2], gStockCaps[3],
                      hottest / 10, hottest % 10, level, gLoad[0], gLoad[1], gLoad[2], gLoad[3], req[0], req[1], req[2], req[3],
                      state[0], state[1], state[2], state[3], d[0].changes, d[1].changes, d[2].changes, d[3].changes);
            } else if (gInteractive) {
                const char *floor = touching ? "touch" : (framing ? "frame" : "none");
                IOLog("HOUJI CPU DVFS: hottest %d.%d C level %u load %u,%u,%u,%u floor %s req-kHz %u,%u,%u,%u state-kHz %u,%u,%u,%u changes %u,%u,%u,%u\n",
                      hottest / 10, hottest % 10, level, gLoad[0], gLoad[1], gLoad[2], gLoad[3], floor, req[0], req[1], req[2], req[3],
                      state[0], state[1], state[2], state[3], d[0].changes, d[1].changes, d[2].changes, d[3].changes);
            } else {
                IOLog("HOUJI CPU DVFS: hottest %d.%d C level %u load %u,%u,%u,%u req-kHz %u,%u,%u,%u state-kHz %u,%u,%u,%u changes %u,%u,%u,%u\n",
                      hottest / 10, hottest % 10, level, gLoad[0], gLoad[1], gLoad[2], gLoad[3], req[0], req[1], req[2], req[3],
                      state[0], state[1], state[2], state[3], d[0].changes, d[1].changes, d[2].changes, d[3].changes);
            }
        }
        IOSleep(gSampleMs);
    }
}
}  // namespace

extern "C" void houji_cpu_dvfs_start(void) {
    unsigned enabled = 0;
    if (gStarted || !PE_parse_boot_argn("houji_dvfs", &enabled, sizeof(enabled)) || !(enabled & 1)) return;
    gStarted = 1;
    char policy[16] = {};
    gStock = PE_parse_boot_argn("houji_cpu_policy", policy, sizeof(policy)) && !strcmp(policy, "stock");
    if (gStock) gMaxPct = 100;
    unsigned pct = 0;
    if (PE_parse_boot_argn("houji_cpu_max_pct", &pct, sizeof(pct)) && pct >= 10 && pct <= 100) gMaxPct = pct;
    unsigned interactive = 0;
    if (PE_parse_boot_argn("houji_cpu_interactive", &interactive, sizeof(interactive)) && interactive) {
        gInteractive = true; gSampleMs = 40;
        unsigned value = 0;
        if (PE_parse_boot_argn("houji_cpu_sample_ms", &value, sizeof(value)) && value >= 10 && value <= 1000) gSampleMs = value;
        if (PE_parse_boot_argn("houji_cpu_frame_pct", &value, sizeof(value)) && value <= 100) gFramePct = value;
        if (PE_parse_boot_argn("houji_cpu_touch_pct", &value, sizeof(value)) && value <= 100) gTouchPct = value;
        if (PE_parse_boot_argn("houji_cpu_touch_ms", &value, sizeof(value)) && value >= 50 && value <= 1500) gTouchMs = value;
        if (gFramePct > gMaxPct) gFramePct = gMaxPct;
        if (gTouchPct > gMaxPct) gTouchPct = gMaxPct;
        IOLog("HOUJI CPU DVFS: interactive sample %u ms frame %u%% touch %u%%/%u ms (max %u%%; thermal caps override floors)\n",
              gSampleMs, gFramePct, gTouchPct, gTouchMs, gMaxPct);
    }
    unsigned first = gStock ? 70 : 0, configuredFirst = 0;
    if (PE_parse_boot_argn("houji_thermal_trigger_c", &configuredFirst, sizeof(configuredFirst)) &&
        configuredFirst >= 40 && configuredFirst <= 80) first = configuredFirst;
    if (first) for (unsigned n = 1; n < kLevels; n++) kTrigger[n] = (int)(first * 10 + 50 * (n - 1));
    if (gStock) {
        int offset = 0;
        if (PE_parse_boot_argn("houji_skin_offset_c", &offset, sizeof(offset)) && offset >= -100 && offset <= 100) gSkinOffsetC = offset;
        PE_parse_boot_argn("battery", &gBatteryMode, sizeof(gBatteryMode));
        IOLog("HOUJI CPU DVFS: policy stock, skin offset %d C, battery mode %u, TSENS emergency 75 C\n",
              gSkinOffsetC, gBatteryMode);
    }
    IOLog("HOUJI CPU DVFS: thermal triggers %d %d %d %d %d deci-C\n", kTrigger[1], kTrigger[2], kTrigger[3], kTrigger[4], kTrigger[5]);
    unsigned usable = 0;
    for (unsigned id = 0; id < kDomains; id++) {
        if (init(id)) ++usable;
        else IOLog("HOUJI CPU DVFS: domain=%u factory validation refused; no frequency writes to it\n", id);
    }
    if (!usable) return;
    gLevel = gStock ? 0 : 2;
    if (gStock) {
        const int hottest = houji_thermal_hottest();
        gEmergency = hottest >= 750;
        updateStockCaps(hottest);
    }
    apply(gLevel, false, false);
    if (gStock) IOLog("HOUJI CPU DVFS: started, policy stock, max %u%%, initial level 0\n", gMaxPct);
    else IOLog("HOUJI CPU DVFS: started, max %u%%, initial level 2\n", gMaxPct);
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
