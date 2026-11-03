// SPDX-License-Identifier: BSD-2-Clause-Patent
// Panel power for iOS's display state. backboardd switches the display with IOMobileFramebuffer selector 12 (scalar 0 off, 1 on: seen
// on every power-key sleep and wake and on auto-lock); SweetFramebuffer forwards it here. Off: brightness 0, then DCS display off (0x28)
// and, with houji_panel_sleep=1, sleep in (0x10). On: sleep out (0x11, 120 ms) when it slept, display on (0x29), then the brightness
// read back from the panel at start (DCS 0x52). Nothing else changes the panel's state: a touch on a dark screen does not light it,
// matching iOS, which ignores touches while its display is off.
// iOS's own idle timer never fires on this port (Auto-Lock set, no dim or sleep request ever arrives), so with the display on and no
// touch for houji_autolock seconds (default 60, 0 never) this presses the power key for it: iOS then locks and turns the display off
// exactly as for a real press. Until the first touch after the display comes on the screen is the lock screen, whose "swipe up" hint
// redraws at 120 Hz (a full-screen copy each frame here): like iOS's own lock-screen timeout, that wait is houji_autolock_lockscreen
// seconds (default 10).
// Commands go through DSI0's command DMA FIFO (0x0ae94000) with lonzzi/UniXNU-houji's HoujiBacklightProtocol.h; short DCS writes use the
// MDSS header (last packet bit 31, data type 0x05, the command in bits 7:0).
// Brightness: HoujiBacklight below answers iOS's AppleARMBacklight queries (lonzzi/UniXNU-houji's approach: the registry object stays
// HoujiBacklight, only io_object_conforms_to and backboardd's property writes are extended, see patches/local.patch IOUserClient.cpp)
// and hands CoreBrightness's levels (0..65536, FadePeriod) to the same thread, so one owner drives the command FIFO.
// USB console: "screen off", "screen on" (the panel only; iOS keeps its own state), "screen LEVEL" (25..2866).
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>
#include <sys/proc.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include "HoujiBacklightProtocol.h"

// [0] stage (1 running; negative: failed), [1] panel on, [2] level, [3] commands sent, [4] failed, [5] last change cause (1 iOS,
// 4 console), [6] iOS power requests, [7] level read from the panel (0: not read), [8] iOS brightness requests, [9] last iOS level
extern "C" {volatile int32_t native_screen_stats[10];}
extern "C" uint64_t houji_dvfs_touch_time(void);   // arm/HoujiCpuDvfs.inc: the last touch down
extern "C" void sweet_hid_key(int key, int down);   // arm/hid/SweetHID.cpp: key 0 is the power key

namespace {
IOMemoryMap *gMap;
volatile uint8_t *gDsi;
uint32_t rd(void *, unsigned off) {
    const uint32_t v = *reinterpret_cast<volatile uint32_t *>(gDsi + off);
    __asm__ volatile("dmb oshld" ::: "memory");
    return v;
}
void wr(void *, unsigned off, uint32_t v) {
    __asm__ volatile("dmb oshst" ::: "memory");
    *reinterpret_cast<volatile uint32_t *>(gDsi + off) = v;
    __asm__ volatile("dsb osh" ::: "memory");
}
uint64_t nowUs() {uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000;}
HoujiDsiCommand gCmd;
unsigned gLevel = 1600;
bool gOn = true, gSlept = false, gSleepIn = false;
// separate slots so a brightness change never replaces a pending power change; in each the latest request wins
uint32_t gPowerReq;   // 0 none; 1 off, 2 on; | 0x100 from iOS
uint32_t gLevelReq;   // 0 none; 0x80000000 | level (0..2866) | 0x40000000 from iOS
IOLock *gLock;
volatile int32_t *const st = native_screen_stats;

constexpr uint32_t dcs(uint8_t command) {return 0x80050000u | command;}

// one transfer, waited for with 1 ms sleeps (50 ms command limit in the protocol)
bool sendWords(const uint32_t *words, unsigned count) {
    if (gCmd.failed) return false;
    if (!gCmd.begin(words, count, nowUs())) {st[4] = st[4] + 1; return false;}
    for (unsigned i = 0; i < 80; ++i) {
        const int r = gCmd.poll(nowUs());
        if (r) {if (r < 0) {st[4] = st[4] + 1; gCmd.failed = false;} else st[3] = st[3] + 1; return r > 0;}
        IOSleep(1);
    }
    st[4] = st[4] + 1;
    return false;
}
bool sendDcs(uint8_t command) {const uint32_t w = dcs(command); return sendWords(&w, 1);}
bool sendLevel(unsigned level) {
    const uint32_t words[2] = {0xc0390003u, 0xff000051u | ((level & 255) << 16) | ((level >> 8) << 8)};
    return sendWords(words, 2);
}
// the panel's current brightness (DCS 0x52), as upstream HoujiBacklight reads it at start
bool readLevel(unsigned *level) {
    if (!gCmd.readBrightness(nowUs())) return false;
    int r = 0;
    for (unsigned i = 0; i < 80 && !(r = gCmd.poll(nowUs())); ++i) IOSleep(1);
    if (r <= 0) {gCmd.failed = false; return false;}
    for (unsigned i = 0; i < 50; ++i) {
        const uint32_t control = rd(nullptr, 0x1d4);
        if (rd(nullptr, 0x110) & (1u << 24)) return false;
        if ((control >> 16) >= 4) return houjiBacklightReadback(control, rd(nullptr, 0x6c), level);
        IOSleep(1);
    }
    return false;
}

void panelOff(int cause) {
    if (!gOn) return;
    sendLevel(0);
    sendDcs(0x28);
    if (gSleepIn) {sendDcs(0x10); IOSleep(5); gSlept = true;}
    gOn = false; st[1] = 0; st[5] = cause;
    IOLog("HOUJI SCREEN: off (%s)%s\n", cause == 1 ? "iOS" : "console", gSlept ? ", panel asleep" : "");
}
void panelOn(int cause) {
    if (gOn) return;
    if (gSlept) {sendDcs(0x11); IOSleep(120); gSlept = false;}
    sendDcs(0x29);
    IOSleep(1);
    sendLevel(gLevel);
    gOn = true; st[1] = 1; st[5] = cause;
    IOLog("HOUJI SCREEN: on (%s), level %u\n", cause == 1 ? "iOS" : "console", gLevel);
}

void screenThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "houji-screen");
    IODeviceMemory *m = IODeviceMemory::withRange(0x0ae94000ULL, 0x1000);
    gMap = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (m) m->release();
    if (!gMap) {st[0] = -1; return;}
    gDsi = reinterpret_cast<volatile uint8_t *>(gMap->getVirtualAddress());
    gCmd.io = {nullptr, rd, wr};
    unsigned sleepIn = 0; PE_parse_boot_argn("houji_panel_sleep", &sleepIn, sizeof sleepIn);
    gSleepIn = sleepIn != 0;
    unsigned level = 0;
    if (PE_parse_boot_argn("houji_screen_level", &level, sizeof level) && level >= HBL_MIN && level <= HBL_MAX) gLevel = level;
    else if (readLevel(&level) && level >= HBL_MIN && level <= HBL_MAX) {gLevel = level; st[7] = (int32_t)level;}
    st[0] = 1; st[1] = 1; st[2] = (int32_t)gLevel;
    IOLog("HOUJI SCREEN: ready, level %u (%s), panel sleep %s\n", gLevel, st[7] ? "read from the panel" : "default", gSleepIn ? "on" : "off");
    unsigned autolock = 60; PE_parse_boot_argn("houji_autolock", &autolock, sizeof autolock);
    unsigned lockscreen = 10; PE_parse_boot_argn("houji_autolock_lockscreen", &lockscreen, sizeof lockscreen);
    if (lockscreen > autolock) lockscreen = autolock;
    uint64_t activeSince = mach_absolute_time(), lockPressed = 0;
    for (;;) {
        IOLockLock(gLock);
        if (!gPowerReq && !gLevelReq) {
            uint64_t deadline = 0; clock_interval_to_deadline(1, kSecondScale, &deadline);
            IOLockSleepDeadline(gLock, (void *)&gPowerReq, deadline, THREAD_UNINT);
        }
        const uint32_t power = gPowerReq, level = gLevelReq; gPowerReq = gLevelReq = 0;
        IOLockUnlock(gLock);
        if (autolock && gOn) {
            const uint64_t touch = houji_dvfs_touch_time(), since = touch > activeSince ? touch : activeSince;
            uint64_t idleNs = 0; absolutetime_to_nanoseconds(mach_absolute_time() - since, &idleNs);
            uint64_t sincePress = UINT64_MAX;
            if (lockPressed) absolutetime_to_nanoseconds(mach_absolute_time() - lockPressed, &sincePress);
            // one press per idle period; again only if iOS did not act on it within 10 s
            const unsigned limit = touch > activeSince ? autolock : lockscreen;   // no touch since the display came on: the lock screen
            if (limit && idleNs > (uint64_t)limit * 1000000000ull && sincePress > 10000000000ull) {
                IOLog("HOUJI SCREEN: %u s without a touch%s, pressing power for iOS's lock\n", limit, limit == autolock ? "" : " on the lock screen");
                sweet_hid_key(0, 1); IOSleep(120); sweet_hid_key(0, 0);
                lockPressed = mach_absolute_time();
            }
        }
        // a level that arrives with "off" is kept for the wake; one with "on" is applied by it
        if (level) {
            const unsigned v = level & 0xffff;
            if (v) {gLevel = v; st[2] = (int32_t)v;}
            if (gOn && !(power & 3)) sendLevel(v);   // 0 from iOS: dark but on, as it asked
        }
        if ((power & 3) == 1) panelOff((power & 0x100) ? 1 : 4);
        else if ((power & 3) == 2) {panelOn((power & 0x100) ? 1 : 4); activeSince = mach_absolute_time(); lockPressed = 0;}
    }
}
void postPower(uint32_t req) {IOLockLock(gLock); gPowerReq = req; IOLockWakeup(gLock, (void *)&gPowerReq, false); IOLockUnlock(gLock);}
void postLevel(uint32_t level, bool ios) {
    IOLockLock(gLock); gLevelReq = 0x80000000u | (ios ? 0x40000000u : 0u) | level; IOLockWakeup(gLock, (void *)&gPowerReq, false);
    IOLockUnlock(gLock);
}
}  // namespace

// ---- the brightness service iOS looks for ----
class HoujiBacklight : public IOService {
    OSDeclareDefaultStructors(HoujiBacklight)
public:
    void publish(unsigned fraction) {
        OSDictionary *b = OSDictionary::withCapacity(3), *p = OSDictionary::withCapacity(1);
        OSNumber *min = OSNumber::withNumber(0ULL, 32), *max = OSNumber::withNumber((uint64_t)HBL_SCALE, 32), *v = OSNumber::withNumber(fraction, 32);
        if (b && p && min && max && v) {
            b->setObject("min", min); b->setObject("max", max); b->setObject("value", v); p->setObject("brightness", b);
            setProperty("IODisplayParameters", p);
        }
        OSSafeReleaseNULL(b); OSSafeReleaseNULL(p); OSSafeReleaseNULL(min); OSSafeReleaseNULL(max); OSSafeReleaseNULL(v);
    }
    static OSNumber *number(OSObject *o) {
        if (auto *n = OSDynamicCast(OSNumber, o)) return n;
        if (auto *d = OSDynamicCast(OSDictionary, o)) return OSDynamicCast(OSNumber, d->getObject("value"));
        return nullptr;
    }
    // CoreBrightness: {brightness: 0..65536 (a number or {value}), FadePeriod: 16.16 seconds, other display keys ignored}
    static bool values(OSObject *object, unsigned *fraction) {
        auto *d = OSDynamicCast(OSDictionary, object);
        OSNumber *b = d ? number(d->getObject("brightness")) : nullptr;
        if (!d || d->getCount() > 16 || !b || b->unsigned64BitValue() > HBL_SCALE) return false;
        *fraction = b->unsigned32BitValue();
        return true;
    }
    IOReturn setProperties(OSObject *object) APPLE_KEXT_OVERRIDE {
        unsigned fraction = 0;
        if (!values(object, &fraction)) return kIOReturnBadArgument;
        const unsigned level = houjiBacklightLevel(fraction);
        st[8] = st[8] + 1; st[9] = (int32_t)level;
        if (st[0] == 1) postLevel(level, true);
        publish(fraction);
        return kIOReturnSuccess;
    }
};
OSDefineMetaClassAndStructors(HoujiBacklight, IOService)
static HoujiBacklight *gBacklight;
// user space asks io_object_conforms_to(..., "AppleARMBacklight"); only this object answers yes, the kernel keeps its real class
extern "C" bool houji_backlight_conforms(const OSObject *object, const char *name) {
    return object && object == gBacklight && !strcmp(name, "AppleARMBacklight");
}
extern "C" bool houji_backlight_allows_properties(IORegistryEntry *entry, OSObject *object) {
    if (!entry || entry != gBacklight) return false;
    char process[32] = {}; proc_selfname(process, sizeof process);
    unsigned fraction = 0;
    return !strcmp(process, "backboardd") && HoujiBacklight::values(object, &fraction);
}
// from SweetFramebuffer's start, with the framebuffer as provider (houji_bl=0: none)
extern "C" void houji_backlight_start(IOService *provider) {
    unsigned enabled = 1; PE_parse_boot_argn("houji_bl", &enabled, sizeof enabled);
    if (!enabled || gBacklight || !provider) return;
    auto *s = new HoujiBacklight;
    if (!s || !s->init() || !s->attach(provider) || !s->start(provider)) {
        if (s) {s->detach(provider); s->release();}
        IOLog("HOUJI BACKLIGHT: not registered\n");
        return;
    }
    gBacklight = s;
    const unsigned level = st[7] ? (unsigned)st[7] : 307;
    s->publish(houjiBacklightFraction(level));
    s->setProperty("backlight-control", true);
    s->registerService();
    s->release();
    IOLog("HOUJI BACKLIGHT: registered for CoreBrightness, initial level %u\n", level);
}

extern "C" void houji_screen_start(void) {
    unsigned enabled = 1; PE_parse_boot_argn("houji_screen", &enabled, sizeof enabled);
    if (!enabled || native_screen_stats[0] || !(gLock = IOLockAlloc())) return;
    thread_t t = nullptr;
    if (kernel_thread_start(screenThread, nullptr, &t) == KERN_SUCCESS) thread_deallocate(t);
}
// iOS's display power (IOMobileFramebuffer selector 12)
extern "C" void houji_screen_power(int on) {
    st[6] = st[6] + 1;
    if (st[0] == 1) postPower((on ? 2u : 1u) | 0x100u);
}
// "off", "on", or a level 25..2866
extern "C" void houji_screen_command(const char *what) {
    if (st[0] != 1) return;
    if (!strcmp(what, "off")) postPower(1);
    else if (!strcmp(what, "on")) postPower(2);
    else {unsigned v = 0; for (const char *c = what; *c >= '0' && *c <= '9'; ++c) v = v * 10 + (unsigned)(*c - '0');
          if (v >= HBL_MIN && v <= HBL_MAX) postLevel(v, false);}
}
extern "C" void houji_screen_report(void) {
    const volatile int32_t *s = native_screen_stats;
    IOLog("HOUJI SCREEN STATE: stage=%d on=%d level=%d sent=%d failed=%d cause=%d ios-requests=%d panel-level=%d brightness-requests=%d last=%d\n",
          s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9]);
}
