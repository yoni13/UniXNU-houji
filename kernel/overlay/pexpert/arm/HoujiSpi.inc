/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * GICv3 SPI routing adapted from UniXNU/ArmCpu 9066b53b (armcpu_spi_attach).
 * Houji uses its existing timer/IPI controller. No redistributor reads, no
 * arbitrary INTIDs, no replacement of an enabled or active unowned SPI.
 */
#include "HoujiSpi.h"
struct HoujiSpiEntry {uint32_t id;void (*handler)(void *);void *context;uint64_t count;};
static struct HoujiSpiEntry houji_spis[4];
static unsigned houji_spi_used,houji_spi_writer,houji_spi_ready;
static uint64_t houji_spi_boot_affinity;
static void houji_spi_fence(void) {
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#else
    /* The host fixture checks routing policy, not hardware ordering. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}
static void houji_spi_controller_ready(void) {
    if(ml_get_cpu_number_local()!=0)return;
    houji_spi_boot_affinity=__builtin_arm_rsr64("MPIDR_EL1") & 0xff00ffffffULL;
    __atomic_store_n(&houji_spi_ready,1u,__ATOMIC_RELEASE);
}
static int houji_spi_wait(void) {
    uint64_t deadline;nanoseconds_to_deadline(10000000ULL,&deadline);
    while(gicd_read32(0)&(1u<<31))if(mach_absolute_time()>=deadline)return 0;
    return 1;
}
int houji_spi_attach(uint32_t intid,int edge,void (*handler)(void *),void *context,
                     uint32_t *readback,uint64_t *router) {
    if(readback)*readback=0;if(router)*router=0;
    if(!gicd_base || !__atomic_load_n(&houji_spi_ready,__ATOMIC_ACQUIRE))return HoujiSpiNotReady;
    /* Actual FDT: IPCC SPI 229 (INTID 261), apps RSC SPI 5 (INTID 37); houji: PCIe0 MSI group 0, SPI 141 (INTID 173, Wi-Fi). */
    if(!handler || (intid!=261 && intid!=37 && intid!=173) || edge)return HoujiSpiArgument;
    unsigned expected=0;
    if(!__atomic_compare_exchange_n(&houji_spi_writer,&expected,1u,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return HoujiSpiBusy;
    int result=HoujiSpiBusy;unsigned used=__atomic_load_n(&houji_spi_used,__ATOMIC_ACQUIRE);
    uint32_t word=(intid/32)*4,bit=1u<<(intid%32),cfg=0xc00+(intid/16)*4,shift=(intid%16)*2+1;
    if(used>=4)goto done;
    for(unsigned i=0;i<used;i++)if(houji_spis[i].id==intid)goto done;
    if(intid>=((gicd_read32(4)&31)+1)*32){result=HoujiSpiArgument;goto done;}
    if((gicd_read32(0x100+word)|gicd_read32(0x300+word))&bit)goto done;
    gicd_write32(0x180+word,bit);
    if(!houji_spi_wait()){result=HoujiSpiTimeout;goto done;}
    gicd_write32(0x80+word,gicd_read32(0x80+word)|bit);
    *(volatile uint8_t *)(gicd_base+0x400+intid)=0xa0;
    gicd_write32(cfg,gicd_read32(cfg)&~(1u<<shift));
    *(volatile uint64_t *)(gicd_base+0x6000+8*intid)=houji_spi_boot_affinity;
    houji_spi_fence();
    if(!(gicd_read32(0x80+word)&bit) || (gicd_read32(cfg)&(1u<<shift)) ||
       *(volatile uint8_t *)(gicd_base+0x400+intid)!=0xa0 ||
       _gic_read64(gicd_base+0x6000+8*intid)!=houji_spi_boot_affinity){result=HoujiSpiReadback;goto done;}
    houji_spis[used]=(struct HoujiSpiEntry){intid,handler,context,0};
    __atomic_store_n(&houji_spi_used,used+1,__ATOMIC_RELEASE);
    houji_spi_fence();
    gicd_write32(0x100+word,bit);
    if(!houji_spi_wait() || !(gicd_read32(0x100+word)&bit)) {
        gicd_write32(0x180+word,bit);(void)houji_spi_wait();
        /* Keep the handler/context registered and retained even on failure. */
        result=HoujiSpiReadback;goto done;
    }
    if(readback)*readback=3u|(0xa0u<<8);
    if(router)*router=_gic_read64(gicd_base+0x6000+8*intid);
    result=HoujiSpiOk;
done:
    __atomic_store_n(&houji_spi_writer,0u,__ATOMIC_RELEASE);return result;
}
int houji_spi_dispatch(uint32_t intid) {
    unsigned count=__atomic_load_n(&houji_spi_used,__ATOMIC_ACQUIRE);
    for(unsigned i=0;i<count;i++)if(houji_spis[i].id==intid) {
        __atomic_add_fetch(&houji_spis[i].count,1ULL,__ATOMIC_RELAXED);
        houji_spis[i].handler(houji_spis[i].context);return 1;
    }
    return 0;
}
uint64_t houji_spi_count(uint32_t intid) {
    unsigned count=__atomic_load_n(&houji_spi_used,__ATOMIC_ACQUIRE);
    for(unsigned i=0;i<count;i++)if(houji_spis[i].id==intid)return __atomic_load_n(&houji_spis[i].count,__ATOMIC_RELAXED);
    return 0;
}
