// SPDX-License-Identifier: BSD-2-Clause-Patent
// Adapted from UniXNU-sheng package/GpuPowerProbe.cpp coherenceProbe at
// 08cde20edc3254ed6f7df8d0f5384fb887786e8f. Same dirty-read and clean-resident
// write proofs, adapted to exclusive CPU16K pages and houji's retained owner.
// houji (this port): mode 4 is the unbounded desktop. With houji_gpu_coherent=1 (default) it runs these proofs first and enables
// coherent=2 only when all pass this boot; when they fail with the GPU idle and its tables intact, the desktop continues with every
// mapping maintained as before (coherent=0). Only an uncertain GPU state still stops. Upstream's mode 5 (proofs, then its bounded
// desktop, mode 3) is kept; upstream's diagnostic-only mode 4 is not.
#ifndef HOUJI_GPU_COHERENCE_PROBE_INC
#define HOUJI_GPU_COHERENCE_PROBE_INC
#include "HoujiGpuCoherencePolicy.h"
#include "Adreno740/NativeAdrenoCoherencePolicy.h"
extern "C" vm_offset_t ml_io_map_unmappable(vm_offset_t,vm_size_t,uint32_t);
extern "C" volatile uint32_t native_adreno_io_coherent;
static struct {
 NativeAdrenoMemory backing,imported;
 uint64_t handle,physical[4],alias[4];
 unsigned active,phase,code,attempts,dirty,read_new,read_old,write_new,dram_new,direction;
 unsigned copies,command_mapped,unmapped,pass,done;
 unsigned draws,render_pass,render_bad_byte;uint32_t render_crc;
 unsigned view_page,view_sub;
 uint64_t cached_par,uncached_par;
 uint64_t started_us;
} gHoujiCoherence;
static bool houjiCoherenceTlbValue(uint64_t value) {
 uint64_t page=value&UINT64_C(0x0000ffffffffffff);
 return gHoujiCoherence.active && houjiApiInOperation() &&
  (value>>48)==0x751 && ((page>=0x40008 && page<0x4000c) || (page>=0x40010 && page<0x40020));
}
static int houjiCoherencePublish(const volatile uint32_t *words,size_t bytes) {
 auto &p=gHoujiCoherence;auto &s=gHoujiSubmit;
 if(!p.active || (gHoujiLive.mode!=4 && gHoujiLive.mode!=5) || gHoujiApi.phase!=12 || !houjiApiInOperation() ||
    houjiSubmitGuard() || s.published || s.pending || words!=houjiCpCpu() || bytes!=HHR_BYTES ||
    p.direction>3 || s.iteration!=s.ring.lastSubmitted()+1 || !p.command_mapped || !p.handle)return 1;
 uint32_t command[7];const unsigned count=p.direction<2?7:HGKI_SELECTED_COMMAND_WORDS;
 if(p.direction<2 && !houjiCoherenceCopyPlan(p.direction,command))return 1;
 auto *actual=(const volatile uint32_t*)gHoujiApi.bo[2]->mapping();
 for(unsigned i=0;i<count;i++){
  uint32_t expected=0;
  if(p.direction<2)expected=command[i];else if(!houjiCoherenceDrawWord(p.direction,i,&expected))return 1;
  if(actual[i]!=expected)return 1;
 }
 const uint32_t seq=s.iteration,bv=HHR_SCRATCH+0x10c,fence=HHR_SCRATCH+0x104;
 const uint32_t expected[]={
  hhr_pkt7(0x17,1),0x80000001,hhr_pkt7(0x46,1),0x32,hhr_pkt7(0x46,1),0x33,hhr_pkt7(0x26,0),hhr_pkt7(0x17,1),3,hhr_pkt7(0x65,1),0x101,
  0x70bf8003,(uint32_t)HoujiCoherenceCommandVa,0,count,
  0x48088501,seq,hhr_pkt7(0x17,1),1,hhr_pkt7(0x46,1),0x18,hhr_pkt7(0x46,1),0x19,hhr_pkt7(0x17,1),2,
  hhr_pkt7(0x46,4),0x08000031,bv,0,seq,hhr_pkt7(0x17,1),1,hhr_pkt7(0x14,4),0,bv,0,seq,
  hhr_pkt7(0x46,4),0x88000031,fence,0,seq,hhr_pkt7(0x17,1),3,hhr_pkt7(0x65,1),0x100};
 static_assert(sizeof(expected)/sizeof(expected[0])==46,"qualified framed copy");
 for(unsigned i=0;i<46;i++)if(words[(s.end+i)&(HHR_WORDS-1)]!=expected[i])return 1;
 s.expected_end=(s.end+46)&(HHR_WORDS-1);s.published=1;houjiStockFullBarrier();return 0;
}
static bool houjiCoherenceSubmit(unsigned direction) {
 auto &p=gHoujiCoherence;auto &s=gHoujiSubmit;uint32_t command[7];
 if(direction>3 || !houjiVmIdle() || gHoujiApi.submit.outstanding() ||
    (direction<2 && !houjiCoherenceCopyPlan(direction,command)))return false;
 p.direction=direction;
 const unsigned count=direction<2?7:HGKI_SELECTED_COMMAND_WORDS;
 static_assert(HGKI_SELECTED_COMMAND_WORDS*4<=HoujiCoherencePage,"owned command page");
 auto *dst=(volatile uint32_t*)gHoujiApi.bo[2]->mapping();
 for(unsigned i=0;i<count;i++){
  uint32_t word=0;
  if(direction<2)word=command[i];else if(!houjiCoherenceDrawWord(direction,i,&word))return false;
  dst[i]=word;
 }
 houjiStockFullBarrier();s.published=s.pending=0;s.iteration=s.ring.lastSubmitted()+1;
 const uint32_t ib[]={0x70bf8003,(uint32_t)HoujiCoherenceCommandVa,0,count};
 // Deliberately use the raw owned ring, as upstream cpFence does. Going
 // through Submit::submit would clean every mapped page and invalidate the
 // experiment. Both BO mappings/references stay held until proven idle.
 if(s.ring.submitFramed(ib,4,&s.seq) || s.seq!=s.iteration || !s.pending)return false;
 if(s.ring.wait(s.seq,&gHoujiApi.clock,200000,&s.observed) || s.observed!=s.seq ||
    houjiSubmitDrainPointers() || !houjiVmIdle())return false;
 if(direction<2)++p.copies;else ++p.draws;++s.completed;return true;
}
static bool houjiCoherenceOwned(void) {
 auto &p=gHoujiCoherence;auto &b=p.backing;NativeAdrenoCpuState cpu{};
 if(NativeAdrenoQueryCpuPages(&cpu) || cpu.kernel_page_bytes!=HoujiCoherencePage || cpu.cpu_page_shift!=14 ||
    !b.backed() || b.imported() || b.quarantined() || b.writeCombined() || !b.mapping() ||
    b.cpuPageBytes()!=HoujiCoherencePage || b.bytes()!=HoujiCoherenceBytes || b.ownedBytes()!=HoujiCoherenceBytes ||
    b.ownedSpanCount()!=1 || b.spanCount()!=1 || b.deviceReferences())return false;
 const auto &span=b.ownedSpans()[0];
 if(span.offset || span.bytes!=HoujiCoherenceBytes || !span.physical || (span.physical&(HoujiCoherencePage-1)) ||
    span.physical>=(UINT64_C(1)<<36) || HoujiCoherenceBytes>(UINT64_C(1)<<36)-span.physical)return false;
 auto overlaps=[&](uint64_t pa,uint64_t bytes){return hzr_overlap(span.physical,span.bytes,pa,bytes);};
 for(unsigned i=0;i<15;i++){const auto &e=gHoujiFullMemory.entry(i);if(overlaps(e.backing.physical,e.request.owned_bytes))return false;}
 for(unsigned i=0;i<3;i++){const auto &e=gHoujiGpuExternalCanary.info[i];if(overlaps(e.pa,e.owned))return false;}
 for(unsigned i=0;i<9;i++){const auto &e=gHoujiFirmwareMemory.info[i];if(overlaps(e.pa,e.owned))return false;}
 for(unsigned i=0;i<3;i++)for(size_t j=0;j<gHoujiApi.bo[i]->ownedSpanCount();j++){
  const auto &e=gHoujiApi.bo[i]->ownedSpans()[j];if(overlaps(e.physical,e.bytes))return false;
 }
 if(overlaps(gHoujiLive.dma_pa,gHoujiLive.dma_owned))return false;
 for(unsigned i=0;i<4;i++)p.physical[i]=span.physical+i*HoujiCoherencePage;
 return true;
}
static bool houjiCoherenceCpuViews(void) {
 auto &p=gHoujiCoherence;
 for(unsigned page=0;page<4;page++)for(unsigned sub=0;sub<4;sub++){
  uint64_t offset=sub*4096+0x5a,va=(uintptr_t)p.backing.mapping()+page*HoujiCoherencePage+offset;
  uint64_t cached=0,uncached=0;
  bool translated=houjiFullHostAt(va,false,&cached) && houjiFullHostAt(p.alias[page]+offset,false,&uncached);
  p.view_page=page;p.view_sub=sub;p.cached_par=cached;p.uncached_par=uncached;
  if(!translated ||
     (cached&1) || (uncached&1) || (cached>>56)!=0xff ||
     !houjiCoherenceUncachedAttribute(uncached>>56) ||
     ((cached&UINT64_C(0x0000fffffffff000))|(va&4095))!=p.physical[page]+offset ||
     ((uncached&UINT64_C(0x0000fffffffff000))|((p.alias[page]+offset)&4095))!=p.physical[page]+offset)return false;
 }
 return true;
}
static int houjiCoherenceRun(void) {
 auto &p=gHoujiCoherence;auto &a=gHoujiApi;
 if(p.active || (gHoujiLive.mode!=4 && gHoujiLive.mode!=5) || native_adreno_io_coherent ||
    a.phase!=10 || !houjiFullKeepLive(nullptr) || a.submit.outstanding())return 1;
 p.active=1;p.started_us=houjiSubmitNow(nullptr);p.phase=1;a.phase=12;
 p.code=p.backing.allocate(HoujiCoherenceBytes,36,true,HoujiCoherenceBytes,false);if(p.code)return 2;
 if(!houjiCoherenceOwned())return 3;
 // Create aliases before dirtying any line. Confirm that their creation did
 // not silently turn the original CPU mapping uncached. All aliases remain
 // pinned until reset, including any failed/precondition-inconclusive test.
 for(unsigned i=0;i<4;i++){
  p.alias[i]=ml_io_map_unmappable(p.physical[i],HoujiCoherencePage,0x7);
  if(!p.alias[i])return 4;
 }
 if(!houjiCoherenceCpuViews())return 5;
 p.code=p.imported.importTask(kernel_task,(uintptr_t)p.backing.mapping(),HoujiCoherenceBytes,36,HoujiCoherenceBytes);
 if(p.code)return 6;
 if(p.imported.readOnly() || p.imported.spanCount()!=1 || p.imported.ownedSpanCount()!=1 ||
    p.imported.spans()[0].offset || p.imported.spans()[0].bytes!=HoujiCoherenceBytes ||
    p.imported.spans()[0].physical!=p.physical[0] ||
    p.imported.ownedSpans()[0].offset || p.imported.ownedSpans()[0].bytes!=HoujiCoherenceBytes ||
    p.imported.ownedSpans()[0].physical!=p.physical[0])return 6;
 p.code=a.vm.map(&p.imported,0,HoujiCoherenceVa,3,1,3,&p.handle);
 if(p.code || !p.handle || houjiApiAuditTables())return 7;
 p.phase=2;
 NativeAdrenoAbiBoMap request={a.handle[2],HoujiCoherenceCommandVa,1,0,3,0};NativeAdrenoAbiBoMapReply reply{};
 p.code=a.client.dispatch(NativeAdrenoMethodBoMap,&request,sizeof(request),&reply,sizeof(reply),&a.clock);
 if(p.code || reply.iova!=HoujiCoherenceCommandVa || !houjiApiBo(2,1) || houjiApiAuditTables())return 8;
 p.command_mapped=1;
 volatile uint32_t *base=(volatile uint32_t*)p.backing.mapping();
 volatile uint32_t *S=base,*D=base+HoujiCoherencePage/4,*R=base+2*HoujiCoherencePage/4,*T=base+3*HoujiCoherencePage/4;
 const volatile uint32_t *dramS=(const volatile uint32_t*)p.alias[0],*dramD=(const volatile uint32_t*)p.alias[1],*dramT=(const volatile uint32_t*)p.alias[3];
 uint64_t ctr=0;__asm__ volatile("mrs %0, ctr_el0":"=r"(ctr));
 if((4u<<((ctr>>16)&15))!=64)return 9; // upstream's 48-of-64 dirty-line criterion
 p.phase=3;
 for(unsigned attempt=0;attempt<HoujiCoherenceAttempts && p.dirty<48;attempt++){
  p.attempts=attempt+1;
  for(unsigned i=0;i<HoujiCoherenceWords;i++){S[i]=0x11110000u|i;D[i]=0;R[i]=0x33330000u|i;T[i]=0x44440000u|i;}
  p.code=NativeAdrenoMaintainOwnedPages(p.backing.mapping(),HoujiCoherenceBytes,NativeAdrenoCleanInvalidate);if(p.code)return 10;
  for(unsigned i=0;i<HoujiCoherenceWords;i++)S[i]=0x22220000u|i;
  houjiStockFullBarrier();p.dirty=0;
  for(unsigned i=0;i<HoujiCoherenceWords;i+=16)p.dirty+=dramS[i]==(0x11110000u|i);
 }
 if(p.dirty<48)return 11; // inconclusive precondition, not proof of non-coherence
 p.phase=4;if(!houjiCoherenceSubmit(0))return 12;
 p.phase=5;
 for(unsigned i=0;i<HoujiCoherenceWords;i++){p.read_new+=dramD[i]==(0x22220000u|i);p.read_old+=dramD[i]==(0x11110000u|i);}
 p.code=NativeAdrenoMaintainOwnedPages((void*)(uintptr_t)T,HoujiCoherencePage,NativeAdrenoClean);if(p.code)return 13;
 uint32_t touch=0;for(unsigned i=0;i<HoujiCoherenceWords;i++)touch+=T[i];(void)touch;houjiStockFullBarrier();
 if(!houjiCoherenceSubmit(1))return 14;
 p.phase=6;
 for(unsigned i=0;i<HoujiCoherenceWords;i++)p.write_new+=T[i]==(0x33330000u|i);
 for(unsigned i=0;i<HoujiCoherenceWords;i++)p.dram_new+=dramT[i]==(0x33330000u|i);
 if(!houjiCoherenceCpuViews())return 15;
 p.pass=houjiCoherenceProved(p.phase,p.dirty,p.read_new,p.read_old,p.write_new,p.dram_new,p.code);
 // CP copies do not exercise render-target cache flushes. Reuse the already
 // qualified 32x32 red CP_BLIT, changing only its output to an owned WB page.
 // Each target has clean resident CPU lines; no invalidate follows GPU work.
 if(p.pass){
  for(unsigned round=0;round<8;round++){
   unsigned page=(round&1)?3:1;
   auto *target=(volatile uint8_t*)((uintptr_t)p.backing.mapping()+page*HoujiCoherencePage);
   const auto *dram=(const volatile uint8_t*)p.alias[page];
   for(unsigned i=0;i<HoujiCoherencePage;i++)target[i]=0x6d;
   p.code=NativeAdrenoMaintainOwnedPages((void*)(uintptr_t)target,HoujiCoherencePage,NativeAdrenoClean);if(p.code)return 19;
   unsigned touch=0;for(unsigned i=0;i<HoujiCoherencePage;i++)touch+=target[i];(void)touch;houjiStockFullBarrier();
   if(!houjiCoherenceSubmit(2+(round&1)))return 20;
   for(unsigned i=0;i<HoujiCoherencePage;i++){
    uint8_t want=i<HGKI_OUTPUT_BYTES?((i&3)==0 || (i&3)==3?0xff:0):0x6d;
    if(target[i]!=want || dram[i]!=want){p.render_bad_byte=i;return 21;}
   }
   p.render_crc=hgki_crc32((const uint8_t*)(uintptr_t)target,HGKI_OUTPUT_BYTES);
   if(p.render_crc!=0xe843b26a || !houjiCoherenceCpuViews())return 22;
  }
  p.render_pass=p.draws==8;
 }
 // Successful stop/unmap must precede any ordinary cleanup. Retain the
 // backing, imported descriptor and aliases even on success until reset.
 p.code=a.vm.unmap(p.handle);if(p.code || houjiApiAuditTables())return 16;
 p.handle=0;
 p.code=a.client.unmapRetainedForTrial(a.handle[2]);if(p.code || !houjiApiBo(2,0) || houjiApiAuditTables())return 17;
 p.command_mapped=0;p.unmapped=1;
 p.code=NativeAdrenoMaintainOwnedPages(p.backing.mapping(),HoujiCoherenceBytes,NativeAdrenoCleanInvalidate);
 return p.code?18:0;
}
static void houjiCoherenceDeadline(void*,wait_result_t) {
 uint64_t began=houjiSubmitNow(nullptr);
 while(!__atomic_load_n(&gHoujiCoherence.done,__ATOMIC_ACQUIRE) && houjiSubmitNow(nullptr)-began<30000000)IOSleep(100);
 if(!__atomic_load_n(&gHoujiCoherence.done,__ATOMIC_ACQUIRE))panic("HOUJI COHERENCE: bounded probe deadline; all memory retained, no-FS recovery");
 thread_terminate(current_thread());
}
static void houjiCoherenceTrial(void) {
 thread_t guard;if(kernel_thread_start(houjiCoherenceDeadline,nullptr,&guard)!=KERN_SUCCESS)
  panic("HOUJI COHERENCE: guard unavailable; no experiment started");
 thread_deallocate(guard);
 houji_ufs_checkpoint("upstream two-way coherence self-test; CPU16K owned scratch; no userland or coherent fast path",0);
 unsigned status=houjiCoherenceRun();auto &p=gHoujiCoherence;
 IOLog("HOUJI COHERENCE: status=%u phase=%u io=%x attempts=%u dirty=%u/64 GPU-read=%u/%u CPU-write=%u DRAM-write=%u copies=%u unmapped=%u proof=%u; scratch=%u CPU-page=%u coherent-enabled=0\n",status,p.phase,p.code,p.attempts,p.dirty,p.read_new,p.read_old,p.write_new,p.dram_new,p.copies,p.unmapped,!status && p.pass,HoujiCoherenceBytes,HoujiCoherencePage);
 IOLog("HOUJI COHERENCE VIEWS: page=%u sub=%u cached-PAR=%llx uncached-PAR=%llx; original must stay WB, alias must be uncached, both must resolve owned page\n",p.view_page,p.view_sub,p.cached_par,p.uncached_par);
 IOLog("HOUJI COHERENCE RENDER: draws=%u/8 pass=%u CRC=%x bad-byte=%u copy-proof=%u; two owned WB targets, cached and DRAM pixels plus CPU16K guard bytes; no desktop claim\n",p.draws,p.render_pass,p.render_crc,p.render_bad_byte,p.pass);
 if(houjiCpBusLease()){
  uint32_t rp=0,wp=0,rbbm=0,irq=0,protect=0,fsr=0;
  unsigned read=houjiFullRaw32(0x3d02018,&rp)|houjiFullRaw32(0x3d0201c,&wp)|
   houjiFullRaw32(0x3d00840,&rbbm)|houjiFullRaw32(0x3d00804,&irq)|
   houjiFullRaw32(0x3d02084,&protect)|houjiFullRaw32(0x3dc0058,&fsr);
  IOLog("HOUJI COHERENCE RING: direction=%u seq=%u observed=%u read=%u RPTR=%u WPTR=%u RBBM=%x IRQ=%x protect=%x FSR=%x\n",p.direction,gHoujiSubmit.seq,gHoujiSubmit.observed,read,rp,wp,rbbm,irq,protect,fsr);
 }
 const bool settled=p.unmapped && !p.handle && !p.command_mapped && houjiVmIdle() &&
    !gHoujiApi.submit.outstanding() && houjiFullLease() && houjiFullTables();
 if(!status && p.pass && p.render_pass && p.copies==2 && p.draws==8 && settled){
  // Private opt-in desktop mode: same-boot proofs completed with coherence
  // disabled. Preserve every ring/fence/issuer/presenter check. Only WB,
  // inner-shareable mappings can skip maintenance; NC imports still cannot.
  gHoujiApi.phase=10;p.active=0;if(gHoujiLive.mode==5)gHoujiLive.mode=3;
  native_adreno_io_coherent=NativeAdrenoCoherenceReadWrite;
  IOLog("HOUJI COHERENCE DESKTOP: mode=2 proofs=2 copies+8 renders; WB/inner-shareable imports only, NC maintenance and submit/present waits retained\n");
  houji_ufs_checkpoint("same-boot coherence and WB-target proofs complete; opt-in bounded coherent desktop; original owner retained",0);
  __atomic_store_n(&p.done,1u,__ATOMIC_RELEASE);return;
 }
 if(gHoujiLive.mode==4 && settled){
  // houji: no proof, but the scratch work is unmapped and the GPU idle with audited tables. Keep the owner and its scratch pages
  // (never freed after a failed experiment) and run the desktop with full maintenance.
  gHoujiApi.phase=10;p.active=0;native_adreno_io_coherent=NativeAdrenoCoherenceOff;
  IOLog("HOUJI COHERENCE DESKTOP: proofs not met (status=%u), coherent=0, every mapping maintained as before\n",status);
  __atomic_store_n(&p.done,1u,__ATOMIC_RELEASE);return;
 }
 // Use the already-qualified owner's stop sequence even when the scratch
 // experiment failed; never free a possibly referenced page on failure.
 if(houjiCpStop(nullptr,gHoujiSubmit.end)){
  gHoujiLive.deny=1;panic("HOUJI COHERENCE: stop uncertain; resources pinned, no-FS recovery");
 }
 gHoujiCp.active=0;gHoujiFull.active=0;gHoujiSubmit.active=0;gHoujiApi.active=gHoujiApi.in_op=0;
 houjiCpSaveFinal();__atomic_store_n(&p.done,1u,__ATOMIC_RELEASE);
 panic("HOUJI COHERENCE: diagnostic complete status=%u proof=%u; retained recovery, desktop fast path unchanged",status,!status && p.pass);
}
#endif
