// SPDX-License-Identifier: BSD-2-Clause-Patent
// Included by HoujiUfsRead.cpp after its normal logging/checkpoint facilities.
#include "HoujiTouchProbe.h"
static struct ht_probe gTouchProbe;
static bool gTouchProbeEnabled;
static uintptr_t gTouchGcc,gTouchSe,gTouchGpio[6];
static OSSharedPtr<IOMemoryMap> gTouchGccMap,gTouchSeMap,gTouchGpioMap[6];
static int touchProbeRead32(void *,uint32_t physical,uint32_t *value) {
  if(!ht_probe_address_allowed(physical) || !value)return 0;
  uintptr_t base=0;uint32_t offset=0;
  if(physical>=0x100000 && physical<0x154000) {
    if(!gTouchGcc)gTouchGcc=mapPhys(0x100000,0x54000,gTouchGccMap);
    base=gTouchGcc;offset=physical-0x100000;
  } else if(physical>=0xa90000 && physical<0xa94000) {
    if(!gTouchSe)gTouchSe=mapPhys(0xa90000,0x4000,gTouchSeMap);
    base=gTouchSe;offset=physical-0xa90000;
  } else for(unsigned i=0;i<6;i++) {
    uint32_t page=0xf100000+ht_gpio[i]*0x1000;
    if(physical<page || physical>page+4)continue;
    if(!gTouchGpio[i])gTouchGpio[i]=mapPhys(page,0x1000,gTouchGpioMap[i]);
    base=gTouchGpio[i];offset=physical-page;break;
  }
  if(!base)return 0;
  *value=*(volatile uint32_t *)(base+offset);
  __asm__ volatile("dmb oshld" ::: "memory");
  return 1;
}
extern "C" void houji_touch_report(void) {
  if(!gTouchProbeEnabled)return;
  const struct ht_probe &s=gTouchProbe;
  IOLog("HOUJI TOUCH RO: result=%u reads=%u GCC branch=%x vote=%x root=%x/%x; cached, no re-read\n",
    s.result,s.completed_reads,s.branch,s.vote,s.root_command,s.root_config);
  IOLog("HOUJI TOUCH RO: SE firmware=%x interface=%x status=%x dma=%x tx=%x rx=%x dma-if=%x\n",
    s.firmware,s.interface,s.status,s.dma_mode,s.tx_fifo,s.rx_fifo,s.dma_interface);
  for(unsigned i=0;i<6;i++)IOLog("HOUJI TOUCH RO: gpio%u cfg=%x io=%x\n",ht_gpio[i],s.gpio_config[i],s.gpio_io[i]);
}
static void touchProbeOnce(void) {
  /* On by default: the touch driver needs this handoff capture (result 20 without it). houji_touch_probe=0 turns it off. */
  int enabled=1;PE_parse_boot_argn("houji_touch_probe",&enabled,sizeof(enabled));
  if(enabled!=1)return;
  houji_ufs_checkpoint("before read-only touch handoff capture",0);
  struct ht_probe_ops ops={nullptr,touchProbeRead32};
  ht_probe_capture(&ops,&gTouchProbe);gTouchProbeEnabled=true;
  houji_touch_report();
  houji_ufs_checkpoint("read-only touch handoff captured; GUI continues",0);
}
