// SPDX-License-Identifier: BSD-2-Clause-Patent
// Power and volume buttons of houji (Xiaomi 14), from UniXNU-sheng's ButtonProbe.cpp (same PMICs on SM8550) and the SteamOS houji port's
// devicetree: power is the PMK8550 PON "pwrkey" (SPMI SID 0, peripheral 0x13, RT_STS bit 7), volume down its "resin" (bit 6), volume up
// PM8550 GPIO 6 (SID 1, peripheral 0x8d, active low, RT_STS bit 0). The SPMI arbiter (spmi@c400000, EE 0, version 7) is the one sheng uses.
// Unlike sheng this reads only: no peripheral interrupt is armed and nothing is written to a PMIC; a thread reads the three levels through
// the arbiter's observer channel every 20 ms and reports changes to SweetHID (sweet_hid_key: 0 power, 1 volume up, 2 volume down).
// Boot argument houji_buttons=0 turns it off.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
extern "C" {
#include "BootResetCore.h"
}

extern "C" void sweet_hid_key(int key, int down);
// [0] stage (1 mapped, 2 polling; negative: the step that failed), [1] power APID, [2] GPIO APID, [3] pressed mask (bit 0 power, 1 up,
// 2 down), [4] reports, [5] read errors, [6] polls, [7] arbiter version
extern "C" {volatile int32_t native_buttons_stats[8];}

namespace {
constexpr uint64_t kCore = 0xc400000, kObsrvr = 0xc440000, kCnfg = 0xc42d000;
constexpr uint32_t kPonPpid = 0x013, kGpioPpid = 0x18d;
constexpr uint8_t kRtSts = 0x10, kPowerBit = 1u << 7, kResinBit = 1u << 6, kGpioBit = 1u << 0;

struct Window {IOMemoryMap *map; volatile uint32_t *va;};
bool mapWindow(Window &w, uint64_t base, uint64_t size) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}
Window gCore, gCnfg, gObs;
br_arbiter gArb;
uint32_t gApids;

// The APID serving ppid, as the mainline driver picks it: the first mapping, replaced by one this EE owns.
int findApid(uint32_t ppid) {
    int found = -1;
    for (uint32_t i = 0; i < gApids; ++i) {
        const uint32_t map = gCore.va[(BR_APID_MAP + 4 * i) / 4];
        if (!map || ((map >> 8) & 0xfff) != ppid) continue;
        if (found < 0 || (gCnfg.va[(BR_CNFG_OWNER + 4 * i) / 4] & 7u) == gArb.ee) found = (int)i;
    }
    return found;
}

void buttonsThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "houji-buttons");
    gArb.ee = 0;
    if (!mapWindow(gCore, kCore, 0x3000) || !mapWindow(gCnfg, kCnfg, 0x4000)) {native_buttons_stats[0] = -1; return;}
    gArb.core = gCore.va; gArb.cnfg = gCnfg.va;
    const uint32_t version = gCore.va[0];
    native_buttons_stats[7] = (int32_t)version;
    gApids = gCore.va[BR_FEATURES / 4] & BR_FEATURES_APIDS_MASK;
    if (version < BR_VERSION_V7_MIN || version >= BR_VERSION_V8_MIN || !gApids || gApids > 1024) {native_buttons_stats[0] = -2; return;}
    if (!mapWindow(gObs, kObsrvr, ((uint64_t)BR_OBS_APID_STRIDE * gApids + 0xfff) & ~0xfffull)) {native_buttons_stats[0] = -3; return;}
    const int pon = findApid(kPonPpid), gpio = findApid(kGpioPpid);
    native_buttons_stats[1] = pon; native_buttons_stats[2] = gpio;
    if (pon < 0) {native_buttons_stats[0] = -4; return;}
    native_buttons_stats[0] = 2;
    uint32_t last = 0;
    bool first = true;
    for (;;) {
        uint32_t mask = 0;
        uint8_t v = 0;
        if (!br_read8(gObs.va, &gArb, pon, kRtSts, &v)) {
            if (v & kPowerBit) mask |= 1;
            if (v & kResinBit) mask |= 4;
        } else native_buttons_stats[5] = native_buttons_stats[5] + 1;
        if (gpio >= 0) {
            if (!br_read8(gObs.va, &gArb, gpio, kRtSts, &v)) {if (!(v & kGpioBit)) mask |= 2;}
            else native_buttons_stats[5] = native_buttons_stats[5] + 1;
        }
        native_buttons_stats[6] = native_buttons_stats[6] + 1;
        // a key already held when polling starts is not reported until it is released once
        if (first) {last = mask; first = false; native_buttons_stats[3] = (int32_t)mask;}
        if (mask != last) {
            for (int key = 0; key < 3; ++key)
                if (((mask ^ last) >> key) & 1) sweet_hid_key(key, (mask >> key) & 1);
            IOLog("HOUJI BUTTONS: %s%s%s\n", mask & 1 ? "power " : "", mask & 2 ? "volume-up " : "", mask & 4 ? "volume-down" : mask ? "" : "released");
            last = mask;
            native_buttons_stats[3] = (int32_t)mask;
            native_buttons_stats[4] = native_buttons_stats[4] + 1;
        }
        IOSleep(20);
    }
}
}  // namespace

extern "C" void houji_buttons_start(void) {
    unsigned enabled = 1;
    PE_parse_boot_argn("houji_buttons", &enabled, sizeof enabled);
    if (!enabled || native_buttons_stats[0]) return;
    thread_t t = nullptr;
    if (kernel_thread_start(buttonsThread, nullptr, &t) == KERN_SUCCESS) thread_deallocate(t);
    else native_buttons_stats[0] = -7;
}

extern "C" void houji_buttons_report(void) {
    const volatile int32_t *s = native_buttons_stats;
    IOLog("HOUJI BUTTONS STATE: stage=%d pon-apid=%d gpio-apid=%d pressed=%x reports=%d read-errors=%d polls=%d arb=%x\n",
          s[0], s[1], s[2], s[3], s[4], s[5], s[6], (unsigned)s[7]);
}
