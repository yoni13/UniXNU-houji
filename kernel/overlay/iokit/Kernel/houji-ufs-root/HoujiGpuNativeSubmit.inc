/* SPDX-License-Identifier: MIT
 * NativeAdreno's actual runtime submit/fence implementation, adopting the
 * initialized same-owner ring without resetting it. Only fixed source-bound
 * CP_BLIT batches are admitted here; all resources remain retained to reset.
 */
#ifndef HOUJI_GPU_NATIVE_SUBMIT_INC
#define HOUJI_GPU_NATIVE_SUBMIT_INC
#include "NativeAdrenoRing.h"
static struct {
 NativeAdrenoRing ring;unsigned once,active,adopted,iteration,completed,published,pending,code;
 uint32_t end=51,expected_end,seq,observed,irq,protect,status,busy,rptr,wptr,crc;
 uint64_t start_us,elapsed_us,submit_us,wait_us;unsigned last_phase;
} gHoujiSubmit;
#ifdef HOUJI_FULL_HOST_NATIVE_HOST_TEST
static uint64_t houjiSubmitHostNow(void);
#endif
static uint64_t houjiSubmitNow(void*){
#ifdef HOUJI_FULL_HOST_NATIVE_HOST_TEST
 return houjiSubmitHostNow();
#else
 uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns/1000;
#endif
}
static bool houjiApiInOperation(void);
static bool houjiApiActive(void);
static bool houjiApiTlbValue(uint64_t);
static int houjiApiPublish(const volatile uint32_t*,size_t);
static int houjiSubmitGuard(void){return !gHoujiSubmit.active || !gHoujiGfx.complete || !gHoujiGfx.zap.authenticated || houjiCpGuard(nullptr);}
static int houjiSubmitRead(void*,uint32_t reg,uint32_t*v){
 const uint32_t allowed[]={0x800,0x801,0x802,0x804,0x805,0xa98,0xa99,0x806,0x807,0x808};
 bool yes=false;for(auto r:allowed)if(reg==r)yes=true;
 return !v || !yes || houjiSubmitGuard()?1:houjiFullRaw32(0x3d00000+4*reg,v);
}
static int houjiSubmitBarrier(void*){if(houjiSubmitGuard())return 1;houjiStockFullBarrier();return 0;}
/* WPTR writes: [0] writes, [1] writes that needed a retry (fence status bit 0 set), [2] retries, [3] us spent in the write loop. */
extern "C" {volatile uint64_t native_adreno_wptr_stats[4];}
static int houjiSubmitWrite(void*,uint32_t reg,uint32_t v){
 auto&s=gHoujiSubmit;if(houjiSubmitGuard() || reg!=0x807 || !s.adopted || !s.published || s.pending || v!=s.expected_end || v>=HHR_WORDS)return 1;
 s.pending=1;
 const uint64_t began=houjiSubmitNow(nullptr);
 for(unsigned i=0;i<200;i++){uint32_t fence=0;if(houjiSubmitGuard() || houjiCpStore(0x3d0201c,v) || houjiFullRaw32(0x3d8ec4c,&fence))return 1;
  if(!(fence&1)){s.end=v;native_adreno_wptr_stats[0]=native_adreno_wptr_stats[0]+1;if(i){native_adreno_wptr_stats[1]=native_adreno_wptr_stats[1]+1;native_adreno_wptr_stats[2]=native_adreno_wptr_stats[2]+i;}
   native_adreno_wptr_stats[3]=native_adreno_wptr_stats[3]+(houjiSubmitNow(nullptr)-began);return 0;}
  IODelay(10);}
 return 1;
}
// The hint reads only this boot's retained, Normal-NC CP completion word.
// The pointer is set once after adoptLive and an AT read verifies the exact
// backing PA/cache attribute; the owner never releases it before reset.
// No MMIO, cache invalidation, ring accounting or error state is changed here.
static const volatile uint32_t *houjiSubmitFenceHint=nullptr;
extern "C" bool native_adreno_board_fence_pending(uint32_t fence) {
 const volatile uint32_t *word=houjiSubmitFenceHint;
 if(!word || !fence)return false;
 const uint32_t seen=*word;houjiStockReadBarrier();
 return seen<fence;
}
static int houjiSubmitAcquire(void*,volatile uint32_t*words,size_t bytes){
 if(houjiSubmitGuard() || words!=houjiCpCpu()+(HHR_SCRATCH+0x100-HHR_RING)/4 || bytes!=64)return 1;houjiStockFullBarrier();return 0;
}
static int houjiSubmitPublish(void*,const volatile uint32_t*words,size_t bytes){
 if(houjiApiActive())return houjiApiPublish(words,bytes);
 auto&s=gHoujiSubmit;if(houjiSubmitGuard() || !s.adopted || !s.iteration || s.iteration>64 || s.published || words!=houjiCpCpu() || bytes!=HHR_BYTES)return 1;
 const uint32_t fence=HHR_SCRATCH+0x104,bv=HHR_SCRATCH+0x10c,seq=s.iteration;
 const uint32_t expected[]={
  hhr_pkt7(0x17,1),0x80000001,hhr_pkt7(0x46,1),0x32,hhr_pkt7(0x46,1),0x33,hhr_pkt7(0x26,0),hhr_pkt7(0x17,1),3,hhr_pkt7(0x65,1),0x101,
  0x70bf8003,HGKI_COMMAND,0,HGKI_SELECTED_COMMAND_WORDS,
  0x48088501,seq,hhr_pkt7(0x17,1),1,hhr_pkt7(0x46,1),0x18,hhr_pkt7(0x46,1),0x19,hhr_pkt7(0x17,1),2,
  hhr_pkt7(0x46,4),0x08000031,bv,0,seq,hhr_pkt7(0x17,1),1,hhr_pkt7(0x14,4),0,bv,0,seq,
  hhr_pkt7(0x46,4),0x88000031,fence,0,seq,hhr_pkt7(0x17,1),3,hhr_pkt7(0x65,1),0x100};
 static_assert(sizeof(expected)/sizeof(expected[0])==46,"canonical fixed framed batch");
 for(unsigned i=0;i<46;i++)if(words[(s.end+i)&(HHR_WORDS-1)]!=expected[i])return 1;
 s.expected_end=(s.end+46)&(HHR_WORDS-1);s.published=1;houjiStockFullBarrier();return 0;
}
static int houjiSubmitIdle(void*){if(houjiSubmitGuard())return 1;IODelay(50);return 0;}
static int houjiSubmitDrainPointers(void){
 auto&s=gHoujiSubmit;
 for(unsigned i=0;i<1000;i++){
  if(houjiSubmitGuard() || houjiFullRaw32(0x3d02018,&s.rptr) || houjiFullRaw32(0x3d0201c,&s.wptr) ||
    houjiFullRaw32(0x3d00840,&s.status) || houjiFullRaw32(0x3d8ec30,&s.busy) ||
    houjiFullRaw32(0x3d00804,&s.irq) || houjiFullRaw32(0x3d02084,&s.protect))return 1;
  if(s.protect || (s.irq&~((1u<<0)|(1u<<17)|(1u<<18)|(1u<<20))))return 1;
  if(s.rptr==s.end && s.wptr==s.end && !(s.status&~1u) && !(s.busy&(1u<<23)))return 0;
  IODelay(50);
 }return 1;
}
#include "HoujiGpuNativeVm.inc"
#include "HoujiGpuNativeClient.inc"
static int houjiVmDynamicBatch(void);
static void houjiRuntimeAtStop(void){
 auto&s=gHoujiSubmit;auto&r=gHoujiCp.ring;
 if(s.once || r.phase!=7 || r.code || !gHoujiGfx.complete)return;s.once=1;s.active=1;s.start_us=houjiSubmitNow(nullptr);s.last_phase=1;
 if(r.rptr!=51 || r.wptr!=51 || houjiSubmitGuard() || !houjiFullLease() || !houjiFullTables()){s.code=1;goto done;}
 {const NativeAdrenoRingIo io={nullptr,houjiSubmitWrite,houjiSubmitRead,houjiSubmitBarrier};const NativeAdrenoRingCache cache={nullptr,houjiSubmitPublish,houjiSubmitAcquire};
  s.code=gHoujiSubmit.ring.adoptLive(&io,&cache,(void*)houjiCpCpu(),HHR_BYTES,HHR_RING,(void*)(houjiCpCpu()+(HHR_SCRATCH+0x100-HHR_RING)/4),64,HHR_SCRATCH+0x100,51);if(s.code)goto done;}
 s.adopted=1;s.last_phase=2;
 {const auto&e=gHoujiFullMemory.entry(12);
  const uint64_t offset=0xc0000+(HHR_SCRATCH+0x104-HHR_RING);
  const uintptr_t address=(uintptr_t)e.backing.cpu+offset;uint64_t translation=0;
  if(offset+sizeof(uint32_t)<=e.request.owned_bytes &&
     houjiFullHostAt(address,false,&translation) && !(translation&1) &&
     (translation>>56)==0x44 &&
     ((translation&UINT64_C(0x0000fffffffff000))|(address&4095))==e.backing.physical+offset)
   houjiSubmitFenceHint=(const volatile uint32_t*)address;
  IOLog("HOUJI FENCE HINT: enabled=%u; retained exact-PA Normal-NC word; locked completion still mandatory\n",houjiSubmitFenceHint!=nullptr);
 }
 for(s.iteration=1;s.iteration<=32;s.iteration++){
  if(houjiSubmitGuard() || (s.completed && houjiSubmitDrainPointers())){s.code=2;goto done;}
  {auto*output=(volatile uint8_t*)((uintptr_t)gHoujiFullMemory.entry(12).backing.cpu+HGKI_CLASS_CURSOR+5*4096);for(unsigned i=0;i<4096;i++)output[i]=0x6d;houjiStockFullBarrier();}
  s.published=s.pending=0;s.last_phase=3;
  {uint64_t before=houjiSubmitNow(nullptr);s.code=s.ring.submitFramed(hgki_ib,4,&s.seq);s.submit_us+=houjiSubmitNow(nullptr)-before;if(s.code || s.seq!=s.iteration || !s.pending){if(!s.code)s.code=3;goto done;}}
  s.last_phase=4;
  {const NativeAdrenoRingClock clock={nullptr,houjiSubmitNow,houjiSubmitIdle};uint64_t before=houjiSubmitNow(nullptr);s.code=s.ring.wait(s.seq,&clock,100000,&s.observed);s.wait_us+=houjiSubmitNow(nullptr)-before;if(s.code || s.observed!=s.iteration){if(!s.code)s.code=4;goto done;}}
  s.last_phase=5;if(houjiSubmitDrainPointers()){s.code=5;goto done;}
  houjiGfxReadImage();{hgki_result checked{};s.code=hgki_verify(gHoujiGfx.readback,HGKI_BYTES,&checked);s.crc=checked.output_crc;if(s.code || !checked.verified){if(!s.code)s.code=6;goto done;}}
  ++s.completed;
  if(s.iteration==1 || s.iteration==8 || s.iteration==16 || s.iteration==32)IOLog("HOUJI NATIVE SUBMIT STEP: frames=%u sequence=%u observed=%u RPTR=%u WPTR=%u CRC=%08x IRQ=%08x; actual NativeAdrenoRing\n",s.completed,s.seq,s.observed,s.rptr,s.wptr,s.crc,s.irq);
 }
 s.last_phase=6;
 if(houjiVmDynamicBatch()){IOLog("HOUJI NATIVE VM STOP: code=%u phase=%u maps=%u unmaps=%u draws=%u offset=%x op=%u value=%llx\n",gHoujiVm.code,gHoujiVm.phase,gHoujiVm.maps,gHoujiVm.unmaps,gHoujiVm.draws,gHoujiVm.last_offset,gHoujiVm.last_op,gHoujiVm.last_value);s.code=7;goto done;}
 if(houjiNativeClientBatch()){s.code=8;goto done;}
done:
 s.elapsed_us=houjiSubmitNow(nullptr)-s.start_us;
 IOLog("HOUJI NATIVE SUBMIT RESULT: code=%u phase=%u adopted=%u completed=%u/80 seq=%u observed=%u pending=%u RPTR=%u WPTR=%u CRC=%08x IRQ=%08x CP-fault=%08x elapsed-us=%llu submit-us=%llu wait-us=%llu; no user-client or desktop claim\n",s.code,s.last_phase,s.adopted,s.completed,s.seq,s.observed,s.pending,s.rptr,s.wptr,s.crc,s.irq,s.protect,s.elapsed_us,s.submit_us,s.wait_us);
 if(s.rptr==s.wptr && s.rptr==s.end){r.rptr=s.rptr;r.wptr=s.wptr;}
 if(s.code || s.completed!=80)r.code=HHR_POST_INIT;s.active=0;
}
#endif
