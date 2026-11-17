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
extern "C" {volatile int32_t native_houji_battery_stats[16];}

extern "C" void houji_battery_probe(void)
{
    unsigned mode=0;
    if(!PE_parse_boot_argn("battery",&mode,sizeof mode)||mode<1||mode>5)return;
    houji_adsp_probe(mode);
    if(mode<3||native_houji_battery_stats[0])return;
    void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if(!handle){native_houji_battery_stats[0]=-10;return;}
    native_houji_battery_static_init();
    if(OSMetaClass::postModLoad(handle)){native_houji_battery_stats[0]=-11;return;}
    houji_battery_start(mode);
}
