/* SPDX-License-Identifier: MIT
 * Same owned, already active root; immutable topology and CPU16K-owned table
 * publication. One exclusive kernel issuer, completed GPU fences + full CP/BR/BV
 * idle before each transaction. No resource retirement or new filesystem grant.
 */
#ifndef HOUJI_GPU_NATIVE_VM_INC
#define HOUJI_GPU_NATIVE_VM_INC
#include "HoujiGpuCanaryRanges.h"
static_assert(sizeof(ArmSmmu4KContextConfig)==64 && offsetof(ArmSmmu4KContextConfig,polling_faults)==56,"current exact context ABI");
static struct {
 FhrGpuLiveBinding binding;ArmSmmu4KContextConfig context;
 ArmSmmu4KRange ranges[13];ArmSmmu4KRange change;ArmSmmu4KLiveResult last;
 unsigned active,in_update,normalizing,tcr_restored,cycle,mapped,maps,unmaps,draws,code,phase,relocations,publishes,tlb_syncs;
 uint32_t crc,original_crc,last_offset,last_op;uint64_t last_value,started_us;
} gHoujiVm;
static bool houjiVmIdle(void){
 const auto&s=gHoujiSubmit;uint32_t rp=0,wp=0,status=0,busy=0;
 return !houjiSubmitGuard() && s.adopted && s.seq==s.observed &&
  !houjiFullRaw32(0x3d02018,&rp) && !houjiFullRaw32(0x3d0201c,&wp) && rp==s.end && wp==s.end &&
  !houjiFullRaw32(0x3d00840,&status) && !(status&~1u) && !houjiFullRaw32(0x3d8ec30,&busy) && !(busy&(1u<<23));
}
/* Set only while an overlapped BoMap (houji_gpu_overlap) runs under the issuer lock: its PTE publish, TLB invalidate and
 * TLB sync may then proceed while the GPU still runs earlier jobs, which cannot use the new, previously invalid entries. */
static bool houjiOverlapMapping;
static bool houjiVmIoLease(void){return ((gHoujiVm.active && gHoujiVm.in_update) || houjiApiInOperation()) && (houjiOverlapMapping || houjiVmIdle());}
static int houjiVmRead32(void*,size_t at,uint32_t*v){
 bool yes=houjiNativeSmmuRead32Offset(at);auto&g=gHoujiVm;g.last_offset=(uint32_t)at;g.last_op=1;
 if(!v || !yes || !houjiVmIoLease())return 1;int e=houjiFullRaw32(0x3da0000+(uint32_t)at,v);if(!e)g.last_value=*v;return e;
}
static int houjiVmRead64(void*,size_t at,uint64_t*v){
 auto&g=gHoujiVm;g.last_offset=(uint32_t)at;g.last_op=2;
 if(!v || (at!=0x20020 && at!=0x20028) || !houjiVmIoLease())return 1;
 int e=houjiFullRaw64(0x3da0000+(uint32_t)at,v);if(!e)g.last_value=*v;return e;
}
static int houjiVmWrite32(void*,size_t at,uint32_t value){
 auto&g=gHoujiVm;g.last_offset=(uint32_t)at;g.last_op=3;g.last_value=value;
 if(!houjiVmIoLease() || !((at==0x20610 && value==0x751) || (at==0x207f0 && value==0xffffffffu) || (at==0x20618 && !value && (g.normalizing || houjiApiInOperation())) || (at==0x20030 && value==0x80802020 && g.normalizing)))return 1;
 if(at==0x207f0)++g.tlb_syncs;return houjiCpStore(0x3da0000+(uint32_t)at,value);
}
static int houjiVmWrite64(void*,size_t at,uint64_t value){
 auto&g=gHoujiVm;g.last_offset=(uint32_t)at;g.last_op=4;g.last_value=value;
 const bool canary=g.active && g.in_update && g.change.bytes==4096 &&
  value==((UINT64_C(0x751)<<48)|(g.change.iova>>12));
 if(!houjiVmIoLease() || at!=0x20620 || (!canary && !houjiApiTlbValue(value)))return 1;
 auto*ptr=houjiFullPointer(0x3da0000+(uint32_t)at,8);if(!ptr)return 1;
 *reinterpret_cast<volatile uint64_t*>(ptr)=value;houjiStockWriteBarrier();return 0;
}
static int houjiVmBarrier(void*){if(!houjiVmIoLease())return 1;houjiStockFullBarrier();return 0;}
static int houjiVmPublish(void*,volatile uint64_t*page,size_t bytes){
 auto&g=gHoujiVm;const auto&e=gHoujiFullMemory.entry(14);uintptr_t begin=(uintptr_t)e.backing.cpu,at=(uintptr_t)page;
 if(!houjiVmIoLease() || bytes!=4096 || (at&4095) || at<begin || at-begin>FHG_LIVE_GPU_USED_PAGES*4096u-bytes ||
  gHoujiFullMemory.bo(14).deviceReferences()!=1 || e.nc || e.request.owned_bytes!=FHG_LIVE_GPU_TABLE_PAGES*4096u)return 1;
 /* The descriptor's complete owned CPU16K page; never clean an unowned neighbor. */
 uintptr_t owned=at&~uintptr_t(16383);if(owned<begin || owned-begin>e.request.owned_bytes-16384)return 1;
 if(NativeAdrenoMaintainOwnedPages((void*)owned,16384,NativeAdrenoCleanInvalidate))return 1;
 houjiStockFullBarrier();++g.publishes;return houjiVmIoLease()?0:1;
}
static int houjiVmNormalizeOwnedTcr(void){
 auto&g=gHoujiVm;if(!houjiVmIdle() || !houjiFullLease() || !houjiFullTables())return 1;
 const ArmSmmu4KIo io={nullptr,0x40000,houjiVmRead32,houjiVmRead64,houjiVmWrite32,houjiVmWrite64,houjiVmBarrier};
 ArmSmmu4KProgramResult actual{};ArmSmmu4KContextImage want{};
 if(ArmSmmu4KEncodeContext(&g.context,&want))return 1;g.in_update=g.normalizing=1;
 auto status=ArmSmmu4KObserveOwnedContext(&io,0,&g.context,&actual);
 if(status){const auto&v=actual.readback;
  if(status!=ArmSmmu4KStateMismatch || !actual.readback_valid || actual.context_offset!=0x20000 ||
   want.tcr!=0x80802020 || v.tcr!=0x80002020 || v.cbar!=want.cbar || v.cba2r!=want.cba2r ||
   ((v.tcr2^want.tcr2)&~(v.tcr2&~want.tcr2&0x60u)) || v.mair0!=want.mair0 || v.mair1!=want.mair1 ||
   ((v.sctlr^want.sctlr)&~(want.sctlr&~v.sctlr&0x1000u)) || v.ttbr0!=want.ttbr0 || v.ttbr1!=want.ttbr1 || v.reserved_zero){
   IOLog("HOUJI NATIVE TCR REFUSED: status=%u valid=%u TCR=%x SCTLR=%x roots=%llx/%llx\n",status,actual.readback_valid,v.tcr,v.sctlr,v.ttbr0,v.ttbr1);g.in_update=g.normalizing=0;return 1;
  }
  /* CP_SMMU_TABLE_UPDATE cleared EPD1. Disable the unowned upper walk again,
   * after every normal master is idle; never accept/mask the open TTBR1 image. */
  if(houjiVmWrite32(nullptr,0x20030,want.tcr) || houjiVmBarrier(nullptr) ||
   ArmSmmu4KObserveOwnedContext(&io,0,&g.context,&actual) ||
   ArmSmmu4KSyncContextTlb(&io,0x20000,0x751,1,1000,&actual) || !actual.tlb_synchronized ||
   ArmSmmu4KObserveOwnedContext(&io,0,&g.context,&actual)){g.in_update=g.normalizing=0;return 1;}
  g.tcr_restored=1;
 }
 IOLog("HOUJI NATIVE TCR: restored-EPD1=%u actual=%x expected=%x; exact context observed, owned bank0 TLBIALL/Sync, no TTBR change\n",g.tcr_restored,actual.readback.tcr,want.tcr);
 g.in_update=g.normalizing=0;return houjiVmIdle()?0:1;
}
static int houjiVmUpdate(unsigned map){
 auto&g=gHoujiVm;if(!g.active || g.mapped==map || !houjiVmIdle() || !houjiFullLease() || !houjiFullTables())return 1;
 if(map && !houjiGpuCanaryRanges(g.binding.boot_ranges,g.binding.boot_count,&g.change,g.ranges,13))return 1;
 const ArmSmmu4KIo io={nullptr,0x40000,houjiVmRead32,houjiVmRead64,houjiVmWrite32,houjiVmWrite64,houjiVmBarrier};
 const ArmSmmu4KLiveVisibility visibility={nullptr,houjiVmPublish};
 g.in_update=1;
 auto status=ArmSmmu4KLiveUpdate(g.binding.arena,g.binding.state,&io,0,&g.context,&visibility,
  map?g.binding.boot_ranges:g.ranges,map?g.binding.boot_count:13,&g.change,map,1000,&g.last);
 g.in_update=0;
 if(status || !g.last.complete || g.binding.state->poisoned || (map?g.last.translation_removed:!g.last.translation_removed)){
  ArmSmmu4KContextImage want{};unsigned encoded=ArmSmmu4KEncodeContext(&g.context,&want);const auto&got=g.last.synchronization.readback;
  IOLog("HOUJI VM CTX CONFIG: encoded=%u polling=%u sizeof=%lu ID=%x/%x/%x valid=%u offset=%llx\n",encoded,g.context.polling_faults,sizeof(g.context),g.last.synchronization.id0,g.last.synchronization.id1,g.last.synchronization.id2,g.last.synchronization.readback_valid,g.last.synchronization.context_offset);
  IOLog("HOUJI VM CTX EXPECT: CBAR=%x CBA2R=%x TCR=%x TCR2=%x MAIR=%x/%x SCTLR=%x TTBR=%llx/%llx zero=%x\n",want.cbar,want.cba2r,want.tcr,want.tcr2,want.mair0,want.mair1,want.sctlr,want.ttbr0,want.ttbr1,want.reserved_zero);
  IOLog("HOUJI VM CTX ACTUAL: CBAR=%x CBA2R=%x TCR=%x TCR2=%x MAIR=%x/%x SCTLR=%x TTBR=%llx/%llx zero=%x\n",got.cbar,got.cba2r,got.tcr,got.tcr2,got.mair0,got.mair1,got.sctlr,got.ttbr0,got.ttbr1,got.reserved_zero);
  IOLog("HOUJI NATIVE VM FAILED: map=%u status=%u complete=%u poison=%u phase=%u offset=%x op=%u value=%llx stores=%lu published=%lu\n",map,status,g.last.complete,g.binding.state->poisoned,g.last.synchronization.phase,g.last_offset,g.last_op,g.last_value,g.last.descriptor_writes,g.last.published_pages);return 1;
 }
 ArmSmmu4KLiveResult audit{};status=ArmSmmu4KLiveAudit(g.binding.arena,g.binding.state,map?g.ranges:g.binding.boot_ranges,map?13:12,&audit);
 if(status || !audit.complete || !houjiVmIdle())return 1;
 /* Only this successful exact whole-graph transaction advances the GPU-table
  * checksum. The GMU graph, immutable root/topology and all refs stay sealed. */
 const auto&e=gHoujiFullMemory.entry(14);gHoujiFull.table_crc[1]=gpp_crc32(e.backing.cpu,e.request.owned_bytes);
 if(!houjiFullTables())return 1;g.mapped=map;if(map)++g.maps;else ++g.unmaps;return 0;
}
static int houjiVmCommand(unsigned restore){
 auto&g=gHoujiVm;if(!houjiVmIdle() || g.change.iova>UINT32_MAX)return 1;
 auto*words=(volatile uint32_t*)((uintptr_t)gHoujiFullMemory.entry(12).backing.cpu+HGKI_CLASS_CURSOR+4*4096);
 unsigned n=0;for(unsigned i=0;i<HGKI_SELECTED_COMMAND_WORDS;i++){
  uint32_t original=hgki_command_word(i),want=original==HGKI_OUTPUT?(uint32_t)g.change.iova:original;
  if(words[i]!=(restore?want:original))return 1;if(original==HGKI_OUTPUT)++n;
 }
 if(n!=1)return 1;
 for(unsigned i=0;i<HGKI_SELECTED_COMMAND_WORDS;i++){uint32_t original=hgki_command_word(i);if(original==HGKI_OUTPUT)words[i]=restore?original:(uint32_t)g.change.iova;}
 houjiStockFullBarrier();if(!restore)++g.relocations;return 0;
}
static int houjiVmDynamicBatch(void){
 auto&g=gHoujiVm;auto&s=gHoujiSubmit;if(g.active || g.phase || s.completed!=32 || !houjiVmIdle())return 1;
 s.iteration=s.seq;g.active=1;g.phase=1;g.started_us=houjiSubmitNow(nullptr);g.original_crc=gHoujiFull.table_crc[1];
 if(fhr_gpu_live_binding(&gHoujiFull.storage,sizeof(gHoujiFull.storage),&g.binding,sizeof(g.binding)) ||
  !g.binding.arena || !g.binding.state || g.binding.boot_count!=12 || g.binding.state->tables.table_pages!=FHG_LIVE_GPU_USED_PAGES ||
  g.binding.state->tables.leaf_pages!=1567 || g.binding.state->generation || g.binding.state->context_offset)return g.code=1;
 g.context={&g.binding.arena->config,nullptr,&g.binding.state->tables,nullptr,0x4c017e18,0x5511,0x751,0,0,0,1};
 if(houjiVmNormalizeOwnedTcr())return g.code=14;
 /* Exercise both high L2 roots, including the final 4K leaf, before exposing
  * a user client. Every cycle restores the original table CRC and canaries. */
 const uint64_t probe_iova[]={0x40000000,HOUJI_GPU_RENDER_START,0xc0000000,HOUJI_GPU_APERTURE-4096};
 for(g.cycle=0;g.cycle<4;g.cycle++){
  const uint64_t offset=(g.cycle&1)?0x94000:0x90000;const auto&e=gHoujiFullMemory.entry(12);
  auto*out=(volatile uint8_t*)((uintptr_t)e.backing.cpu+offset);
  /* Two separate CPU16K-aligned, already owned NC spans in read/write class2.
   * Existing aliases have identical NormalNC/nonprivileged/shareable attrs. */
  if(offset<0x87000 || offset+16384>0xc0000 || !houjiFullLease())return g.code=2;
  g.change={probe_iova[g.cycle],e.backing.physical+offset,4096,ArmSmmu4KRead|ArmSmmu4KWrite,0,3};g.phase=2;
  if(houjiVmUpdate(1))return g.code=3;
  for(unsigned draw=0;draw<8;draw++){
   if(!houjiVmIdle())return g.code=4;
   for(unsigned i=0;i<16384;i++)out[i]=0x6d;houjiStockFullBarrier();
   if(houjiVmCommand(0))return g.code=5;g.phase=3;s.published=s.pending=0;++s.iteration;
   if(s.ring.submitFramed(hgki_ib,4,&s.seq) || s.seq!=s.iteration)return g.code=6;
   const NativeAdrenoRingClock clock={nullptr,houjiSubmitNow,houjiSubmitIdle};
   if(s.ring.wait(s.seq,&clock,100000,&s.observed) || s.observed!=s.seq || houjiSubmitDrainPointers())return g.code=7;
   if(houjiVmCommand(1))return g.code=8;houjiStockFullBarrier();g.phase=4;
   g.crc=hgki_crc32((const uint8_t*)out,4096);if(g.crc!=0xe843b26a)return g.code=9;
   for(unsigned i=0;i<16384;i++){const uint8_t expected=i<4096?((i&3)==0 || (i&3)==3?0xff:0):0x6d;if(out[i]!=expected)return g.code=10;}
   houjiGfxReadImage();hgki_result checked{};if(hgki_verify(gHoujiGfx.readback,HGKI_BYTES,&checked) || !checked.verified)return g.code=11;
   ++g.draws;++s.completed;
  }
  g.phase=5;if(houjiVmUpdate(0))return g.code=12;
  if(gHoujiFull.table_crc[1]!=g.original_crc)return g.code=13;
  IOLog("HOUJI NATIVE VM STEP: cycle=%u maps=%u unmaps=%u draws=%u IOVA=%llx PA=%llx seq=%u observed=%u CRC=%08x generation=%llu published=%u TLB-sync=%u; old/new retained\n",g.cycle,g.maps,g.unmaps,g.draws,g.change.iova,g.change.physical,s.seq,s.observed,g.crc,g.binding.state->generation,g.publishes,g.tlb_syncs);
 }
 g.phase=6;g.active=0;
 IOLog("HOUJI NATIVE VM RESULT: code=%u maps=%u unmaps=%u draws=%u topology=%lu leaves=%lu generation=%llu seq=%u observed=%u RPTR=%u WPTR=%u CRC=%08x published=%u TLB-sync=%u elapsed-us=%llu; no user-client/desktop claim\n",g.code,g.maps,g.unmaps,g.draws,g.binding.state->tables.table_pages,g.binding.state->tables.leaf_pages,g.binding.state->generation,s.seq,s.observed,s.rptr,s.wptr,g.crc,g.publishes,g.tlb_syncs,houjiSubmitNow(nullptr)-g.started_us);return 0;
}
#endif
