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
extern "C" void houji_cpu_l3_follow(bool busy, uint32_t cpuKhz);   // below: the L3 clock with the CPU (houji_l3)
unsigned houji_l3_mode_arg(void);   // below: boot argument houji_l3
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
// Opt-in (boot argument p7c=MHz or console "dvfs set p7cap MHz", 0 off): the prime core's (cpu7, EPSS domain kPrime = 2) cap while
// the stock policy is at its first thermal step (kStockCpu7 {250, 2630400}: from 25.0 C skin, so at room temperature always), up to
// the domain's own cap (kCaps, 3052.8 MHz). The hotter steps and the emergency cut still apply. (Until 2026-11 this and p7pin acted on
// domain 3, cpu2-4: root366 and root367's prime-core cap and pin results are cpu2-4's.)
unsigned gP7CapKhz;
constexpr unsigned kPrime = kCpuDomain[7];
// Test only (console "dvfs set p7pin MHz", 0 off): the prime core voted at the table's row at or under MHz, whatever its load and
// floors, still under its cap: for timing a core against its vote (console cpub).
unsigned gP7PinKhz;
// houji_cpu_load_sum=1: a domain's load is the sum of its cores' busy shares (at most 100 %), not the busiest core's. A single thread
// moving among the cores of a cluster (SpringBoard's main thread over CPUs 2-6 while Control Center opens, root273) left every core
// 20-40 % busy, and the governor asked 1.4-1.5 GHz for a thread that ran all the time.
bool gLoadSum;

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
                const uint32_t stepKhz = table.step[gStockState[id] - 1].khz;
                const unsigned stockCap = khzCapIndex(w, id == kPrime && gStockState[id] == 1 && gP7CapKhz > stepKhz ? gP7CapKhz : stepKhz);
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
        if (id == kPrime && gP7PinKhz) {
            at = khzCapIndex(w, gP7PinKhz);
            if (at > cap) at = cap;
        } else if (gInteractive) {
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
                        if (gLoadSum) {
                            load[kCpuDomain[cpu]] += busy;
                            if (load[kCpuDomain[cpu]] > 1000) load[kCpuDomain[cpu]] = 1000;
                        } else if (busy > load[kCpuDomain[cpu]]) load[kCpuDomain[cpu]] = busy;
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
        {
            uint32_t top = 0;
            for (unsigned id = 1; id < kDomains; id++)
                if (d[id].ok && d[id].khz[d[id].cur] > top) top = d[id].khz[d[id].cur];
            houji_cpu_l3_follow(framing || touching, top);
        }
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
    houji_l3_mode_arg();
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
        if (PE_parse_boot_argn("p7c", &value, sizeof(value)) && value <= 3149) gP7CapKhz = value * 1000;
        if (PE_parse_boot_argn("houji_cpu_touch_ms", &value, sizeof(value)) && value >= 50 && value <= 1500) gTouchMs = value;
        if (PE_parse_boot_argn("houji_cpu_load_sum", &value, sizeof(value)) && value) gLoadSum = true;
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
        // sko=N: the same in 7 bytes of the 1000-byte command line. Under 25.0 C of skin the stock tables lift every cap (root370-371:
        // 22.7-23.7 C on a cool night, boot load at 3.1 GHz, 76 C), so a test run holds them with sko=4 (to 25-32 C: past 32 C the second step caps cpu7 at 2169.6 MHz).
        if (PE_parse_boot_argn("sko", &offset, sizeof(offset)) && offset >= -100 && offset <= 100) gSkinOffsetC = offset;
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

// The current clock of domain `id` in kHz (its performance state, read from the hardware), 0 for one not in use: the console's tl
// samples it every 10 ms beside the threads (HoujiUsbConsole.c, "HOUJI TL CLK").
extern "C" uint32_t houji_cpu_state_khz(unsigned id) {
    if (!gStarted || id >= kDomains || !d[id].ok) return 0;
    return (rd(d[id], 0x20) & 255) * 19200;
}

extern "C" void houji_cpu_dvfs_report(void) {
    if (!gStarted) {IOLog("HOUJI CPU DVFS: not started (houji_dvfs=1)\n"); return;}
    for (unsigned id = 0; id < kDomains; id++) {
        if (!d[id].ok) {IOLog("HOUJI CPU DVFS: d=%u not in use (validation refused)\n", id); continue;}
        IOLog("HOUJI CPU DVFS: d=%u req-kHz=%u state-kHz=%u changes=%u bad=%u\n", id, d[id].khz[d[id].cur],
              (rd(d[id], 0x20) & 255) * 19200, d[id].changes, d[id].bad);
    }
}

// The EPSS domain of a CPU (kCpuDomain), for the console's cpub and tl.
extern "C" unsigned houji_cpu_domain_of(unsigned cpu) {return cpu < 8 ? kCpuDomain[cpu] : 0;}

// Console "dvfs lut": each domain's registers (enable 0x0, domain state 0x20, control 0xb0, the perf-state votes from 0x320) and its
// whole factory table, every row raw with its frequency and bits 18:16 (LUT_CORE_COUNT in Linux qcom-cpufreq-hw: 1 marks a boost row),
// up to the repeated row that ends it, rows over kCaps included.
extern "C" void houji_cpu_dvfs_lut(void) {
    for (unsigned id = 0; id < kDomains; id++) {
        Domain &w = d[id];
        if (!w.va) {IOLog("HOUJI CPU LUT: d=%u not mapped\n", id); continue;}
        // Only the vote words the domain has (one per core with per-core voting, control bit 0, else one): root369 reset the phone
        // reading 0x324 and 0x328 of every domain.
        const unsigned votes = (rd(w, 0xb0) & 1) ? kCounts[id] : 1;
        uint32_t v[3] = {0, 0, 0};
        for (unsigned c = 0; c < votes && c < 3; c++) v[c] = rd(w, 0x320 + c * 4);
        IOLog("HOUJI CPU LUT: d=%u cpus %u enable %08x state %08x (%u kHz) control %08x votes(%u) %08x %08x %08x cur-row %u\n", id,
              kCounts[id], rd(w, 0), rd(w, 0x20), (rd(w, 0x20) & 255) * 19200, rd(w, 0xb0), votes, v[0], v[1], v[2],
              w.ok ? w.index[w.cur] : 255);
        uint32_t prev = 0;
        for (unsigned i = 0; i < kRows; i++) {
            const uint32_t raw = rd(w, 0x100 + i * 4), khz = (raw & 255) * 19200;
            IOLog("HOUJI CPU LUT: d=%u row %2u raw %08x %7u kHz src %u cores %u volt-bits %03x\n", id, i, raw, khz, raw >> 30, (raw >> 16) & 7,
                  (raw >> 8) & 0xff);
            if (i && raw == prev) break;
            prev = raw;
        }
    }
}

// Console "dvfs set NAME VALUE": the interaction settings changed while running, so one boot can compare them (each a boot argument
// otherwise): framepct, touchpct (0-100, under the max), loadsum (0/1), skin (the skin offset, -100 to 100 C), p7cap and p7pin (MHz, gP7CapKhz, gP7PinKhz).
extern "C" void houji_cpu_dvfs_set(const char *name, int value) {
    bool ok = true;
    if (!strcmp(name, "framepct") && value >= 0 && value <= 100) gFramePct = (unsigned)value > gMaxPct ? gMaxPct : (unsigned)value;
    else if (!strcmp(name, "touchpct") && value >= 0 && value <= 100) gTouchPct = (unsigned)value > gMaxPct ? gMaxPct : (unsigned)value;
    else if (!strcmp(name, "loadsum") && (value == 0 || value == 1)) gLoadSum = value;
    else if (!strcmp(name, "skin") && value >= -100 && value <= 100) gSkinOffsetC = value;
    else if (!strcmp(name, "p7cap") && value >= 0 && value <= 3149) gP7CapKhz = (unsigned)value * 1000;
    else if (!strcmp(name, "p7pin") && value >= 0 && value <= 3149) gP7PinKhz = (unsigned)value * 1000;
    else ok = false;
    IOLog("HOUJI CPU DVFS SET: %s %d %s (frame %u%% touch %u%% loadsum %u skin offset %d C p7cap %u MHz)\n", name, value, ok ? "ok" : "refused",
          gFramePct, gTouchPct, gLoadSum ? 1u : 0u, gSkinOffsetC, gP7CapKhz / 1000);
}

// The EPSS L3 (CPU shared cache) clock at 0x17d90000 (Linux sm8650.dtsi epss_l3, drivers/interconnect/qcom/osm-l3.c: LUT rows at
// 0x100, 4 bytes each, L value bits 7:0 times 19.2 MHz when the source bits 31:30 are set, else the 300 MHz alternate; the vote at
// 0x90, the perf state at 0x320). The bootloader leaves the vote at row 12 (1555 MHz); nothing changed it before. root283, one boot,
// Control Center opens and closes alternating the vote: at row 0 (365 MHz) opens 24-44 fps (mean 36), closes 60-86 (69); at the
// top row (2035 MHz) opens 48-85 (65), closes 93-98 (95).
static IOMemoryMap *gL3Map;
static uintptr_t gL3Va;
static unsigned gL3Rows, gL3Vote = ~0u;
static uint32_t gL3Khz[40];
static unsigned gL3Mode;   // houji_l3: 0 leave it, 1 follow the CPU (top while touched or drawing), 2 always the top row

static bool l3Map(void) {
    if (gL3Va) return true;
    IODeviceMemory *m = IODeviceMemory::withRange(0x17d90000ULL, 0x1000);
    gL3Map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (m) m->release();
    gL3Va = gL3Map ? gL3Map->getVirtualAddress() : 0;
    if (!gL3Va) {IOLog("HOUJI L3: not mapped\n"); return false;}
    volatile uint32_t *r = (volatile uint32_t *)gL3Va;
    for (unsigned i = 0; i < 40; i++) {
        const uint32_t info = r[(0x100 + i * 4) / 4];
        gL3Khz[i] = (info >> 30) ? (info & 255) * 19200 : 300000;
        if (i && gL3Khz[i] == gL3Khz[i - 1]) break;
        gL3Rows = i + 1;
    }
    return gL3Rows != 0;
}

// Called by the governor after each sample (houji_l3=1 or 2): the lowest row at or above the fastest CPU domain's request, the top
// row while touched or drawing frames (1), or always the top row (2).
extern "C" void houji_cpu_l3_follow(bool busy, uint32_t cpuKhz) {
    if (!gL3Mode || !l3Map()) return;
    unsigned row = gL3Rows - 1;
    if (gL3Mode == 1 && !busy) {
        row = 0;
        while (row + 1 < gL3Rows && gL3Khz[row] < cpuKhz) ++row;
    }
    if (row == gL3Vote) return;
    ((volatile uint32_t *)gL3Va)[0x90 / 4] = row;
    gL3Vote = row;
}

// Console "l3" / "l3 vote N" / "l3 perf N" / "l3 mode N": the table and state; write row N (past the table: its top row); the mode.
extern "C" void houji_cpu_l3(int op, unsigned value) {
    if (op == 3) {gL3Mode = value <= 2 ? value : gL3Mode; gL3Vote = ~0u;}
    if (!l3Map()) return;
    volatile uint32_t *r = (volatile uint32_t *)gL3Va;
    const unsigned rows = gL3Rows;
    uint32_t *khz = gL3Khz;
    if (value >= rows) value = rows - 1;
    if (op == 1) r[0x90 / 4] = value;
    if (op == 2) r[0x320 / 4] = value;
    __asm__ volatile("dsb sy" ::: "memory");
    IOLog("HOUJI L3: mode %u enable %#x state %#x (%u kHz) vote %u perf %u; %u rows kHz %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u\n",
          gL3Mode, r[0], r[0x20 / 4], (r[0x20 / 4] & 255) * 19200, r[0x90 / 4], r[0x320 / 4], rows, khz[0], khz[1], khz[2], khz[3], khz[4],
          khz[5], khz[6], khz[7], khz[8], khz[9], khz[10], khz[11], khz[12], khz[13], khz[14], khz[15], khz[16], khz[17], khz[18], khz[19]);
}

// Boot argument houji_l3=1|2 (houji_cpu_l3_follow).
unsigned houji_l3_mode_arg(void) {
    unsigned mode = 0;
    if (PE_parse_boot_argn("houji_l3", &mode, sizeof(mode)) && mode <= 2) gL3Mode = mode;
    if (gL3Mode) IOLog("HOUJI L3: mode %u\n", gL3Mode);
    return gL3Mode;
}

// Console "ddr Y": an active-only RPMh vote for the DDR (BCM MC0) and the LLCC/system hub (BCM SH0) through TCS 0 of the application
// processor's RSC DRV 2 (as houji_touch_power), Y the BCM's peak vote (0-16383; 16383 asks for its top level, 0 withdraws the vote).
// Linux bcm-voter: BCM_TCS_CMD(commit 1 << 30, valid 1 << 29, vote_x << 14, vote_y). Nothing in this kernel votes the CPU's memory path
// otherwise; the GPU's GMU votes DDR only for the GPU. For measuring whether the CPU waits on memory; not a governor.
#include "QcomRpmh.h"
extern "C" void houji_ddr_vote(unsigned y) {
    static uint32_t mc0, sh0;
    static IOMemoryMap *rscMap;
    static volatile uint32_t *rsc;
    if (!rsc) {
        uint8_t *db = static_cast<uint8_t *>(IOMallocData(0x20000));
        IODeviceMemory *m = IODeviceMemory::withRange(0x81c60000ULL, 0x20000);
        IOMemoryMap *map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        if (m) m->release();
        if (db && map) {
            for (uint64_t i = 0; i < 0x20000; i += 4) {
                const uint32_t w = *(volatile uint32_t *)(map->getVirtualAddress() + i);
                memcpy(db + i, &w, 4);
            }
            QcomRpmhResource r;
            if (!qcom_cmd_db_find(db, 0x20000, "MC0", &r)) mc0 = r.address;
            if (!qcom_cmd_db_find(db, 0x20000, "SH0", &r)) sh0 = r.address;
        }
        if (map) map->release();
        if (db) IOFreeData(db, 0x20000);
        IODeviceMemory *rm = IODeviceMemory::withRange(0x17a20000ULL, 0x10000);
        rscMap = rm ? rm->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        if (rm) rm->release();
        rsc = rscMap ? (volatile uint32_t *)rscMap->getVirtualAddress() : nullptr;
        if (!rsc || !mc0 || !sh0) {IOLog("HOUJI DDR: refused (rsc %p mc0 %x sh0 %x)\n", (void *)rsc, mc0, sh0); rsc = nullptr; return;}
    }
    if (y > 16383) y = 16383;
    const QcomRscLayout layout = qcom_rsc_layout(rsc[0]);
    const uint32_t value = (1u << 30) | ((y ? 1u : 0u) << 29) | y, addresses[2] = {mc0, sh0}, values[2] = {value, value};
    QcomRpmhWrite writes[16];
    const unsigned n = qcom_rpmh_active_request(&layout, 0, addresses, values, 2, writes, 16);
    for (unsigned i = 0; i < n; ++i) rsc[(0xd00 + writes[i].offset) / 4] = writes[i].value;
    __asm__ volatile("dsb sy" ::: "memory");
    IOSleep(2);
    const unsigned m2 = qcom_rpmh_finish(&layout, 0, writes, 16);
    for (unsigned i = 0; i < m2; ++i) rsc[(0xd00 + writes[i].offset) / 4] = writes[i].value;
    IOLog("HOUJI DDR: vote %u (cmd %#x) to MC0 %#x and SH0 %#x\n", y, value, mc0, sh0);
}
