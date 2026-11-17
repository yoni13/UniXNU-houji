// SPDX-License-Identifier: BSD-2-Clause-Patent
// One explicit cumulative gate: 1 survey, 2 PAS boot, 3 GLINK channel, 4 reads, 5 IOPMPowerSource.
#include <IOKit/IOService.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
extern kmod_info_t g_kernel_kmod_info;
extern "C" void native_houji_battery_static_init(void) __asm__("__GLOBAL__sub_I_HoujiBattery.cpp");
extern "C" void houji_adsp_probe(unsigned mode);
extern "C" void houji_battery_start(unsigned mode);
extern "C" {extern volatile int32_t native_houji_battery_stats[16];}   // defined in HoujiBattery.cpp
extern "C" {extern volatile int32_t native_adsp_stats[16]; extern char native_adsp_text[96];}
#include <IOKit/IOLib.h>
#include <kern/thread.h>

// The console report (like HOUJI WIFI STATS): every 2 s for the first minute, then every 30 s, the ADSP probe's 16 words
// (HoujiAdspProbe.cpp, [0] stage or the failing step) with its last text, and the battery service's 16 words (HoujiBattery.cpp).
static void houji_battery_report(void *, wait_result_t)
{
    for (unsigned t = 0;; ++t) {
        const volatile int32_t *a = native_adsp_stats, *b = native_houji_battery_stats;
        IOLog("HOUJI ADSP STATS: t=%us %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %.60s\n", t < 30 ? 2 * t : 60 + 30 * (t - 30),
              a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15], native_adsp_text);
        IOLog("HOUJI BATT STATS: %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n",
              b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
        IOSleep(t < 30 ? 2000 : 30000);
    }
}

extern "C" void houji_battery_probe(void)
{
    unsigned mode=0;
    if(!PE_parse_boot_argn("battery",&mode,sizeof mode)||mode<1||mode>5)return;
    thread_t reporter;
    if(kernel_thread_start(houji_battery_report,nullptr,&reporter)==KERN_SUCCESS)thread_deallocate(reporter);
    houji_adsp_probe(mode);
    if(mode<3||native_houji_battery_stats[0])return;
    void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if(!handle){native_houji_battery_stats[0]=-10;return;}
    native_houji_battery_static_init();
    if(OSMetaClass::postModLoad(handle)){native_houji_battery_stats[0]=-11;return;}
    houji_battery_start(mode);
}
