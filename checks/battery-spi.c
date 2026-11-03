/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint64_t registers[0x10000/8],tick;
static uintptr_t gicd_base;
static unsigned cpu,writes,callback_count;
static int bad_readback;
static uint32_t gicd_read32(unsigned at) {
    if(bad_readback && at>=0xc00 && at<0xd00)return 0xffffffffu;
    return *(uint32_t *)(gicd_base+at);
}
static uint64_t _gic_read64(uintptr_t at) {return *(uint64_t *)at;}
static void gicd_write32(unsigned at,uint32_t value) {
    ++writes;
    if(at>=0x180 && at<0x200)*(uint32_t *)(gicd_base+at-0x80)&=~value;
    else if(at>=0x100 && at<0x180)*(uint32_t *)(gicd_base+at)|=value;
    else *(uint32_t *)(gicd_base+at)=value;
}
static unsigned ml_get_cpu_number_local(void) {return cpu;}
static uint64_t mpidr(void) {return 0x100000100ULL;}
#define __builtin_arm_rsr64(x) mpidr()
static void nanoseconds_to_deadline(uint64_t ns,uint64_t *out) {(void)ns;*out=tick+100;}
static uint64_t mach_absolute_time(void) {return ++tick;}
#include "../kernel/overlay/pexpert/arm/HoujiSpi.inc"
static void callback(void *p) {assert(p==&callback_count);++callback_count;}
static void reset(void) {
    memset(registers,0,sizeof(registers));memset(houji_spis,0,sizeof houji_spis);
    gicd_base=(uintptr_t)registers;*(uint32_t *)(gicd_base+4)=31;
    houji_spi_used=houji_spi_writer=houji_spi_ready=0;cpu=writes=callback_count=0;tick=0;bad_readback=0;
}
int main(void) {
    reset();assert(houji_spi_attach(261,0,callback,&callback_count,0,0)==HoujiSpiNotReady && !writes);
    cpu=1;houji_spi_controller_ready();assert(!houji_spi_ready);cpu=0;houji_spi_controller_ready();
    assert(houji_spi_attach(33,0,callback,0,0,0)==HoujiSpiArgument && !writes);
    assert(houji_spi_attach(261,1,callback,0,0,0)==HoujiSpiArgument && !writes);
    *(uint32_t *)(gicd_base+0x120)=1u<<5;
    assert(houji_spi_attach(261,0,callback,0,0,0)==HoujiSpiBusy && !writes);
    *(uint32_t *)(gicd_base+0x120)=0;*(uint32_t *)(gicd_base+0x320)=1u<<5;
    assert(houji_spi_attach(261,0,callback,0,0,0)==HoujiSpiBusy && !writes);
    *(uint32_t *)(gicd_base+0x320)=0;
    uint32_t readback;uint64_t router;
    assert(!houji_spi_attach(261,0,callback,&callback_count,&readback,&router));
    assert(router==mpidr() && readback==(3u|(0xa0u<<8)));
    assert(houji_spi_dispatch(261)==1 && callback_count==1 && houji_spi_count(261)==1);
    assert(!houji_spi_dispatch(27) && !houji_spi_dispatch(1));
    unsigned saved=writes;assert(houji_spi_attach(261,0,callback,0,0,0)==HoujiSpiBusy && writes==saved);
    assert(!houji_spi_attach(37,0,callback,&callback_count,0,0));assert(houji_spi_dispatch(37));
    /* yoni13: the PCIe0 MSI group 0 line (Wi-Fi) is a third accepted SPI */
    assert(!houji_spi_attach(173,0,callback,&callback_count,0,0));assert(houji_spi_dispatch(173));
    reset();houji_spi_controller_ready();*(uint32_t *)gicd_base=1u<<31;
    assert(houji_spi_attach(261,0,callback,0,0,0)==HoujiSpiTimeout && !houji_spi_used && !houji_spi_writer);
    reset();houji_spi_controller_ready();bad_readback=1;
    assert(houji_spi_attach(261,0,callback,0,0,0)==HoujiSpiReadback && !houji_spi_used);
    assert(!(gicd_read32(0x120)&(1u<<5)));
    puts("battery SPI policy: ownership, restricted IDs, route, dispatch and bounded failures PASS");
}
