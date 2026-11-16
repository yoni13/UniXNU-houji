// SPDX-License-Identifier: BSD-2-Clause-Patent
// houji SoC temperatures for iOS 27: the iOS 16 kernel's SM8650 TSENS v2 reader (fork/kernel/overlay/iokit/Kernel/arm/HoujiThermal.inc),
// read only. Boot firmware configures the three controllers; this reads TM_Sn_STATUS (TM base + 0xa0 + 4n: LAST_TEMP [11:0] signed
// deci-C, VALID bit 21) as Linux drivers/thermal/qcom/tsens-v2.c does. Sensor roles come from this phone's FDT thermal-zones. Stock
// firmware shows its red chip at 85 C and stops at 90 C. Console "temp" prints one reading; houji_thermal_log_s=N logs one every N s.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>

namespace {
constexpr unsigned kControllers = 3;
constexpr uint64_t kTm[kControllers] = {0x0c228000, 0x0c229000, 0x0c22a000};
constexpr unsigned kSensors[kControllers] = {15, 16, 13};
// bit n set: sensor n of that controller is a CPU / GPU sensor
constexpr uint32_t kCpuMask[kControllers] = {0x7ffe, 0x003e, 0};
constexpr uint32_t kGpuMask[kControllers] = {0, 0, 0x01fe};
constexpr unsigned kDdrController = 1, kDdrSensor = 13;
uintptr_t gVa[kControllers];
IOMemoryMap *gMaps[kControllers];
bool gReady;
struct Reading {int cpu, gpu, ddr, hottest; unsigned hottestId, valid, invalid;};

bool init() {
    if (gReady) return true;
    for (unsigned c = 0; c < kControllers; c++) {
        IODeviceMemory *m = IODeviceMemory::withRange(kTm[c], 0x1000);
        gMaps[c] = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        if (m) m->release();
        gVa[c] = gMaps[c] ? gMaps[c]->getVirtualAddress() : 0;
        if (!gVa[c]) {IOLog("HOUJI THERMAL: controller %u map failed\n", c); return false;}
    }
    gReady = true;
    return true;
}

// Temperatures in deci-C. A sensor that is never valid counts as invalid.
bool read(Reading &r) {
    r = {-1000, -1000, -1000, -1000, 0, 0, 0};
    if (!init()) return false;
    for (unsigned c = 0; c < kControllers; c++) for (unsigned s = 0; s < kSensors[c]; s++) {
        uint32_t v = 0; bool ok = false;
        for (unsigned tries = 0; tries < 3 && !ok; tries++) {v = *(volatile uint32_t *)(gVa[c] + 0xa0 + 4 * s); ok = (v >> 21) & 1;}
        if (!ok) {++r.invalid; continue;}
        int t = (int)(v & 0xfff); if (t & 0x800) t -= 0x1000;
        if (t < -400 || t > 1500) {++r.invalid; continue;}
        ++r.valid;
        if (t > r.hottest) {r.hottest = t; r.hottestId = c * 16 + s;}
        if ((kCpuMask[c] >> s & 1) && t > r.cpu) r.cpu = t;
        if ((kGpuMask[c] >> s & 1) && t > r.gpu) r.gpu = t;
        if (c == kDdrController && s == kDdrSensor) r.ddr = t;
    }
    return r.valid > 0 && r.cpu > -1000;
}

void report() {
    Reading r;
    if (!read(r)) {IOLog("HOUJI THERMAL: no valid reading (valid %u invalid %u)\n", r.valid, r.invalid); return;}
    IOLog("HOUJI THERMAL: cpu %d.%d C gpu %d.%d C ddr %d.%d C hottest %d.%d C (sensor %u:%u) valid %u invalid %u\n",
          r.cpu / 10, r.cpu % 10 < 0 ? -(r.cpu % 10) : r.cpu % 10, r.gpu / 10, r.gpu % 10 < 0 ? -(r.gpu % 10) : r.gpu % 10,
          r.ddr / 10, r.ddr % 10 < 0 ? -(r.ddr % 10) : r.ddr % 10, r.hottest / 10, r.hottest % 10 < 0 ? -(r.hottest % 10) : r.hottest % 10,
          r.hottestId / 16, r.hottestId % 16, r.valid, r.invalid);
}

void logThread(void *arg, wait_result_t) {
    const unsigned seconds = (unsigned)(uintptr_t)arg;
    for (;;) {report(); IOSleep(seconds * 1000);}
}
}  // namespace

extern "C" void houji_thermal_report(void) {report();}

// Hottest sensor in deci-C, or -1000 without a reading: for the sleep/DVFS policy and the console.
extern "C" int houji_thermal_hottest(void) {Reading r; return read(r) ? r.hottest : -1000;}

extern "C" void houji_thermal_start(void) {
    static bool started;
    unsigned seconds = 0;
    if (started || !PE_parse_boot_argn("houji_thermal_log_s", &seconds, sizeof(seconds)) || seconds < 1 || seconds > 3600) return;
    started = true;
    thread_t t = nullptr;
    if (kernel_thread_start(logThread, (void *)(uintptr_t)seconds, &t) == KERN_SUCCESS) thread_deallocate(t);
}
