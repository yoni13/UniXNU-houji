/* SPDX-License-Identifier: MIT
 * Actual upstream IOKit wrapper on the qualified retained houji owner.
 * Initial trial remains kernel-driven; no userspace or Metal claim.
 */
#ifndef HOUJI_GPU_NATIVE_PROVIDER_INC
#define HOUJI_GPU_NATIVE_PROVIDER_INC
#include <IOKit/IOUserClient.h>
extern "C" IOReturn native_adreno_publish(NativeAdrenoVm*,NativeAdrenoSubmit*,NativeAdrenoRing*,const NativeAdrenoRingClock*,const NativeAdrenoTimer*,const NativeAdrenoAbiDeviceInfo*,uint32_t,uint32_t);
extern "C" IOService*native_adreno_published_service(void);
extern "C" IOReturn houji_native_accelerator_register(void);
extern "C" IOReturn native_adreno_provider_poll(void);
extern "C" IOReturn native_adreno_provider_desktop_stats(uint64_t[7]);
extern "C" void houji_framebuffer_counts(uint64_t*,uint64_t*);
extern "C" void houji_framebuffer_report(void);
extern "C" IOReturn native_adreno_provider_try_poll(void);
extern "C" unsigned native_adreno_provider_probe_result(void);
extern "C" unsigned native_adreno_provider_probe_started(void);
extern "C" uint64_t native_adreno_provider_progress_token(void);
extern "C" void native_adreno_provider_snapshot(void);
extern "C" void native_adreno_perf_report(void);
extern "C" int native_adreno_provider_stop(int(*)(void*),void*);
static struct {
 NativeAdrenoTimer timer;unsigned phase,code,published,registry,rounds,draws,views,closes,publishes;
 uint32_t first_error,crc;uint64_t started_us,audited_generation=UINT64_MAX;
} gHoujiProvider;
/* Published values: real programmed A750v2/GMEM/trap from the pinned gen7
 * profile; SM8650 UBWC4/HBB16 and version-derived swizzle6/macrotile1 from
 * Linux drivers/soc/qcom/ubwc_config.c + include/linux/soc/qcom/ubwc.h.
 * Do not advertise a general userspace coherence capability. Opt-in import
 * policy is selected only after this boot's board-owned memory proofs.
 */
static const NativeAdrenoAbiDeviceInfo gHoujiDeviceInfo={0x43051401u,3u<<20,0x1000000,0x1fffffffff000,HOUJI_GPU_RENDER_START,HOUJI_GPU_RENDER_BYTES,16,6,1,4,NativeAdrenoDeviceClientIova,0};
extern "C" const NativeAdrenoStatus*native_adreno_board_status(void){return &gHoujiApi.status;}
extern "C" bool native_adreno_board_running(void){return gHoujiLive.retained && gHoujiLive.coexist && !gHoujiLive.deny && !gHoujiCp.stopping && !gHoujiCp.isolated && gHoujiProvider.published;}
extern "C" bool native_adreno_board_va_window(uint64_t start,uint64_t bytes){
 if(!bytes || ((start|bytes)&4095) || start>=HOUJI_GPU_APERTURE || bytes>HOUJI_GPU_APERTURE-start || hzr_overlap(start,bytes,0x1000000,3u<<20))return false;
 const auto&b=gHoujiVm.binding;if(!b.state || !b.arena || !b.boot_ranges || b.boot_count!=12)return false;
 for(unsigned i=0;i<12;i++)if(hzr_overlap(start,bytes,b.boot_ranges[i].iova,b.boot_ranges[i].bytes))return false;
 return true;
}
/* Cached CPU timing only; no MMIO/FS and no change to ownership or idle checks. */
static struct {uint64_t count,total,max,phase[5],audits;} gHoujiGatePerf;
struct HoujiGateClock {
 uint64_t begin=houjiSubmitNow(nullptr),last=begin;
 void lap(unsigned i){uint64_t now=houjiSubmitNow(nullptr);__atomic_fetch_add(&gHoujiGatePerf.phase[i],now-last,__ATOMIC_RELAXED);last=now;}
 ~HoujiGateClock(){uint64_t elapsed=houjiSubmitNow(nullptr)-begin;__atomic_fetch_add(&gHoujiGatePerf.count,1ull,__ATOMIC_RELAXED);__atomic_fetch_add(&gHoujiGatePerf.total,elapsed,__ATOMIC_RELAXED);uint64_t old=__atomic_load_n(&gHoujiGatePerf.max,__ATOMIC_RELAXED);while(elapsed>old && !__atomic_compare_exchange_n(&gHoujiGatePerf.max,&old,elapsed,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)){} }
};
extern "C" {extern volatile uint64_t native_adreno_vm_phase_us[2][8];}
extern "C" {extern uint64_t native_adreno_vm_phase_max_us[2][6];}
extern "C" {extern uint64_t native_adreno_vm_alias_paths[2];}
extern "C" void native_adreno_present_sync_report(void);
extern "C" IOReturn native_adreno_dvfs_poll(void);
#include "HoujiGpuDvfs.inc"
extern "C" unsigned houji_gpu_gate_snapshot(char *out,unsigned capacity){
 if(!out || !capacity)return 0;unsigned used=0;out[0]=0;
 auto append=[&](const char *format,auto... args){
  if(used>=capacity-1)return;int n=snprintf(out+used,capacity-used,format,args...);
  if(n>0 && (unsigned)n<capacity-used)used+=(unsigned)n;else out[used]=0;
 };
 auto load=[](const uint64_t *v){return __atomic_load_n(v,__ATOMIC_RELAXED);};
 append("HOUJI GATE PERF: n=%llu total-us=%llu max-us=%llu wait-us=%llu drain-us=%llu audit-us=%llu CRC-set-us=%llu CRC-check-us=%llu audits=%llu; cached CPU costs\n",load(&gHoujiGatePerf.count),load(&gHoujiGatePerf.total),load(&gHoujiGatePerf.max),load(&gHoujiGatePerf.phase[0]),load(&gHoujiGatePerf.phase[1]),load(&gHoujiGatePerf.phase[2]),load(&gHoujiGatePerf.phase[3]),load(&gHoujiGatePerf.phase[4]),load(&gHoujiGatePerf.audits));
 for(unsigned row=0;row<2;row++){
  uint64_t v[8];for(unsigned i=0;i<8;i++)v[i]=__atomic_load_n(&native_adreno_vm_phase_us[row][i],__ATOMIC_RELAXED);
  append("HOUJI VM PERF: row=%u n=%llu p1-us=%llu p2-us=%llu transaction-us=%llu p4-us=%llu preflight-us=%llu max-us=%llu bytes=%llu\n",row,v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7]);
  uint64_t m[6];for(unsigned i=0;i<6;i++)m[i]=__atomic_load_n(&native_adreno_vm_phase_max_us[row][i],__ATOMIC_RELAXED);
  append("HOUJI VM PEAK: row=%u p1-us=%llu p2-us=%llu transaction-us=%llu p4-us=%llu preflight-us=%llu; independent phase maxima\n",row,m[1],m[2],m[3],m[4],m[5]);
 }
 append("HOUJI VM ALIAS: indexed=%llu pairwise=%llu; preflight checks, no alias validation skipped\n",load(&native_adreno_vm_alias_paths[0]),load(&native_adreno_vm_alias_paths[1]));
 return used;
}
static void houjiProviderPerfReport(void){
 IOLog("HOUJI CRC CPU: hw=%llu soft=%llu; identical whole-table IEEE CRC32 and feature-gated ISA\n",__atomic_load_n(&houji_crc_calls[1],__ATOMIC_RELAXED),__atomic_load_n(&houji_crc_calls[0],__ATOMIC_RELAXED));
 houjiDvfsReport();
 native_adreno_present_sync_report();
 auto load=[](const uint64_t *v){return __atomic_load_n(v,__ATOMIC_RELAXED);};
 IOLog("HOUJI GATE PERF: n=%llu total-us=%llu max-us=%llu wait-us=%llu drain-us=%llu audit-us=%llu CRC-set-us=%llu CRC-check-us=%llu audits=%llu; cached CPU costs, no MMIO\n",load(&gHoujiGatePerf.count),load(&gHoujiGatePerf.total),load(&gHoujiGatePerf.max),load(&gHoujiGatePerf.phase[0]),load(&gHoujiGatePerf.phase[1]),load(&gHoujiGatePerf.phase[2]),load(&gHoujiGatePerf.phase[3]),load(&gHoujiGatePerf.phase[4]),load(&gHoujiGatePerf.audits));
 for(unsigned row=0;row<2;row++){uint64_t v[8];for(unsigned i=0;i<8;i++)v[i]=__atomic_load_n(&native_adreno_vm_phase_us[row][i],__ATOMIC_RELAXED);IOLog("HOUJI VM PERF: row=%u n=%llu p1-us=%llu p2-us=%llu transaction-us=%llu p4-us=%llu preflight-us=%llu max-us=%llu bytes=%llu; cached original NativeAdreno map/unmap phases\n",row,v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7]);}
}
extern "C" IOReturn native_adreno_board_quiesce(void){
 if(!native_adreno_board_running() || !gHoujiVm.binding.state)return kIOReturnNotReady;
 HoujiGateClock gate;auto&a=gHoujiApi;auto&s=gHoujiSubmit;
 if(a.submit.waitAll(&a.clock,500000))return kIOReturnTimeout;gate.lap(0);
 s.seq=s.ring.lastSubmitted();s.observed=s.ring.completedFence();
 if(s.seq!=s.observed || houjiSubmitDrainPointers() || !houjiVmIdle())return kIOReturnIOError;gate.lap(1);
 /* The source-bound live updater audits every PTE transaction itself. Keep
  * the extra full graph/table CRC on each real generation change,
  * before any next submit; unchanged queries/waits need no repeated CRC. */
 uint64_t generation=gHoujiVm.binding.state->generation;
 if(gHoujiLive.mode==1 || gHoujiProvider.audited_generation!=generation){
  /* NativeAdrenoVm's transaction proof and periodic conflicting-route scan
   * remain authoritative. The first outer audit
   * still binds the seeded houji root. Mode1 keeps the original strict path.
   * Re-walking the entire graph after every generation was duplicate work
   * (r185:24.7s of38.6s in the gate); every-generation CRC checks remain.
   */
  if(gHoujiLive.mode==1 || gHoujiProvider.audited_generation==UINT64_MAX){
   if(a.vm.auditLive())return kIOReturnIOError;
   __atomic_fetch_add(&gHoujiGatePerf.audits,1ull,__ATOMIC_RELAXED);
  }
  gate.lap(2);
  const auto&e=gHoujiFullMemory.entry(14);gHoujiFull.table_crc[1]=houji_table_crc32(e.backing.cpu,e.request.owned_bytes);gate.lap(3);
  if(!houjiFullTables())return kIOReturnIOError;gate.lap(4);gHoujiProvider.audited_generation=generation;
 }
 a.phase=11;s.published=s.pending=0;s.iteration=s.seq+1;
 return kIOReturnSuccess;
}
/* Former pipelined-gate counters, still printed by the GPU reports (the gate was replaced by upstream's guarded dispatch). */
extern "C" {volatile uint64_t native_adreno_pipeline_stats[5];}
/* Gate and GPU governor counters for the display's hitch log; plain loads, no lock. */
extern "C" void houji_gate_hitch_snapshot(uint64_t out[6]){
 auto load=[](const uint64_t *v){return __atomic_load_n(v,__ATOMIC_RELAXED);};
 out[0]=load(&gHoujiGatePerf.count);out[1]=load(&gHoujiGatePerf.total);out[2]=load(&gHoujiGatePerf.phase[0]);
 out[3]=load(&gHoujiGatePerf.phase[2])+load(&gHoujiGatePerf.phase[3])+load(&gHoujiGatePerf.phase[4]);
 out[4]=gHoujiDvfs.level;out[5]=gHoujiDvfs.busyNs/1000;
}
/* Houji still publishes all non-coherent mapped pages, and reasserts WC
 * attributes, before a submission. These operations cannot overlap earlier
 * GPU writes merely because the VM generation is unchanged. Poll first;
 * the caller releases the issuer lock and sleeps on Timeout. Once complete,
 * the original drain/ownership/VM proof runs with the issuer lock held. */
extern "C" IOReturn native_adreno_board_prepare_submit(void){
 if(!native_adreno_board_running() || !gHoujiVm.binding.state)return kIOReturnNotReady;
 IOReturn ready=gHoujiApi.submit.waitAll(&gHoujiApi.clock,0);
 return ready?ready:native_adreno_board_quiesce();
}
static bool houjiProviderTlbValue(uint64_t v){return native_adreno_board_running() && houjiApiInOperation() && (v>>48)==0x751 && (v&UINT64_C(0x0000ffffffffffff))>=0x10000 && (v&UINT64_C(0x0000ffffffffffff))<HOUJI_GPU_APERTURE/4096;}
static int houjiProviderPublish(const volatile uint32_t*words,size_t bytes){
 auto&s=gHoujiSubmit;auto&p=gHoujiProvider;
 if(!native_adreno_board_running() || !houjiApiInOperation() || houjiSubmitGuard() || s.published || s.pending || words!=houjiCpCpu() || bytes!=HHR_BYTES || !native_adreno_ring_log_count)return 1;
 const volatile uint32_t*log=native_adreno_ring_log[(native_adreno_ring_log_count-1)&3];
 const uint32_t seq=s.ring.lastSubmitted()+1,n=log[3];
 if(!seq || log[0]!=seq || log[1]!=s.end || log[2]>=HHR_WORDS || n<46 || n>NativeAdrenoRingFrameWords+4*NativeAdrenoSubmitMaxBuffers || (n-42)%4 || log[2]!=((s.end+n)&(HHR_WORDS-1)))return 1;
 const uint32_t head[]={hhr_pkt7(0x17,1),0x80000001,hhr_pkt7(0x46,1),0x32,hhr_pkt7(0x46,1),0x33,hhr_pkt7(0x26,0),hhr_pkt7(0x17,1),3,hhr_pkt7(0x65,1),0x101};
 /* The light prologue (unchanged tables): the flush, invalidate and wait-for-idle replaced by one 4-word CP_NOP. */
 const uint32_t light[]={hhr_pkt7(0x17,1),0x80000001,hhr_pkt7(0x10,4),0,0,0,0,hhr_pkt7(0x17,1),3,hhr_pkt7(0x65,1),0x101};
 bool full=true,lite=true;
 for(unsigned i=0;i<11;i++){if(log[4+i]!=head[i])full=false;if(log[4+i]!=light[i])lite=false;}
 if(!full && !lite)return 1;
 const unsigned ibs=(n-42)/4;
 for(unsigned i=0;i<ibs;i++){unsigned at=4+11+4*i;uint64_t va=log[at+1]|((uint64_t)log[at+2]<<32);uint32_t count=log[at+3];
  if(log[at]!=0x70bf8003 || !count || count>0xfffff || (va&3) || va<0x10000000 || va>=HOUJI_GPU_APERTURE || count*4ull>HOUJI_GPU_APERTURE-va)return 1;
 }
 const uint32_t bv=HHR_SCRATCH+0x10c,fence=HHR_SCRATCH+0x104;
 const uint32_t tail[]={0x48088501,seq,hhr_pkt7(0x17,1),1,hhr_pkt7(0x46,1),0x18,hhr_pkt7(0x46,1),0x19,hhr_pkt7(0x17,1),2,
  hhr_pkt7(0x46,4),0x08000031,bv,0,seq,hhr_pkt7(0x17,1),1,hhr_pkt7(0x14,4),0,bv,0,seq,
  hhr_pkt7(0x46,4),0x88000031,fence,0,seq,hhr_pkt7(0x17,1),3,hhr_pkt7(0x65,1),0x100};
 for(unsigned i=0;i<31;i++)if(log[4+11+4*ibs+i]!=tail[i])return 1;
 for(unsigned i=0;i<n;i++)if(words[(s.end+i)&(HHR_WORDS-1)]!=log[4+i])return 1;
 s.iteration=seq;s.expected_end=log[2];s.published=1;++p.publishes;houjiStockFullBarrier();return 0;
}
static int houjiProviderTimerRead(void*,uint32_t reg,uint32_t*v){if(!v || (reg!=0x980 && reg!=0x981) || houjiSubmitGuard())return 1;return houjiFullRaw32(0x3d00000+4*reg,v);}
static IOReturn houjiWrapperCall(IOUserClient*uc,uint32_t method,const void*in,void*out){IOExternalMethodArguments a{};a.version=kIOExternalMethodArgumentsCurrentVersion;a.selector=method;a.structureInput=in;a.structureInputSize=NativeAdrenoUserClient::inputSize(method);a.structureOutput=out;a.structureOutputSize=NativeAdrenoUserClient::outputSize(method);return uc->externalMethod(method,&a,nullptr,nullptr,nullptr);}
static int houjiProviderRound(unsigned round){
 auto&p=gHoujiProvider;IOUserClient*uc=nullptr;IOService*svc=native_adreno_published_service();NativeAdrenoAbiBoAllocReply handles[2]{};
 IOMemoryDescriptor*desc[2]{};IOMemoryMap*maps[2]{};NativeAdrenoAbiBoCpuMapReply views[2]{};const uint64_t va[]={0x40000000,0x40004000};
 auto record=[&](IOReturn v){if(v && !p.first_error)p.first_error=v;return v==kIOReturnSuccess;};
 if(!svc || !record(svc->newUserClient(kernel_task,nullptr,0x4147,nullptr,&uc)) || !uc)return 1;
 p.phase=3;
 {NativeAdrenoAbiParams query{};NativeAdrenoAbiDeviceInfo info{};if(!record(houjiWrapperCall(uc,NativeAdrenoMethodQuery,nullptr,&query)) || query.abi_version!=1 || !record(houjiWrapperCall(uc,NativeAdrenoMethodGetDeviceInfo,nullptr,&info)) || info.chip_id!=0x43051401u)return 2;}
 for(unsigned i=0;i<2;i++){
  NativeAdrenoAbiBoAlloc request={16384,14,1};if(!record(houjiWrapperCall(uc,NativeAdrenoMethodBoAlloc,&request,&handles[i])))return 3;
  NativeAdrenoAbiBoMap map={handles[i].handle,va[i],i?3u:1u,0,3,0};NativeAdrenoAbiBoMapReply mapped{};
  if(!record(houjiWrapperCall(uc,NativeAdrenoMethodBoMap,&map,&mapped)) || mapped.iova!=va[i])return 4;
  NativeAdrenoAbiBoCpuMap cpu={handles[i].handle};if(!record(houjiWrapperCall(uc,NativeAdrenoMethodBoCpuMap,&cpu,&views[i])) || views[i].bytes!=16384)return 5;
  IOOptionBits options=0;if(!record(uc->clientMemoryForType(views[i].token,&options,&desc[i])) || !desc[i] || (options&kIOMapCacheMask)!=kIOMapWriteCombineCache)return 6;
  maps[i]=desc[i]->createMappingInTask(kernel_task,0,options);if(!maps[i] || maps[i]->getLength()!=16384 || (maps[i]->getVirtualAddress()&16383))return 7;
  for(unsigned off=0;off<16384;off+=4096){uint64_t read=0,write=0,addr=maps[i]->getVirtualAddress()+off+0x5a;
   if(!houjiFullHostAt(addr,false,&read) || !houjiFullHostAt(addr,true,&write) || read!=write || (read&1) || (read>>56)!=0x44)return 8;
  }++p.views;
 }
 p.phase=4;
 for(unsigned draw=0;draw<2;draw++){
  auto*cmd=(volatile uint32_t*)maps[0]->getVirtualAddress();auto*out=(volatile uint8_t*)maps[1]->getVirtualAddress();
  if(native_adreno_provider_poll())return 9;
  for(unsigned i=0;i<16384;i++)out[i]=0x6d;
  for(unsigned i=0;i<4096;i++){uint32_t word=i<HGKI_SELECTED_COMMAND_WORDS?hgki_command_word(i):0;cmd[i]=word==HGKI_OUTPUT?0x40004000:word;}
  houjiStockFullBarrier();NativeAdrenoAbiSubmit request{};NativeAdrenoAbiSubmitReply reply{};request.buffer_count=1;request.buffers[0]={handles[0].handle,va[0],HGKI_SELECTED_COMMAND_WORDS*4};
  if(!record(houjiWrapperCall(uc,NativeAdrenoMethodSubmit,&request,&reply)))return 10;
  NativeAdrenoAbiWaitFence wait={reply.fence,500000000};if(!record(houjiWrapperCall(uc,NativeAdrenoMethodWaitFence,&wait,nullptr)) || native_adreno_provider_poll())return 11;
  houjiStockFullBarrier();p.crc=hgki_crc32((const uint8_t*)out,4096);if(p.crc!=0xe843b26a)return 12;
  for(unsigned i=0;i<16384;i++){uint8_t want=i<4096?((i&3)==0 || (i&3)==3?0xff:0):0x6d;if(out[i]!=want)return 13;}
  ++p.draws;++gHoujiSubmit.completed;
  IOLog("HOUJI PROVIDER FRAME: round=%u draw=%u fence=%llu observed=%u CRC=%x; actual external method table and exclusive NC CPU views\n",round,draw,reply.fence,gHoujiSubmit.observed,p.crc);
 }
 p.phase=5;
 for(unsigned i=0;i<2;i++){maps[i]->release();maps[i]=nullptr;desc[i]->release();desc[i]=nullptr;NativeAdrenoAbiBoCpuUnmap unmap={handles[i].handle,views[i].token,0};if(!record(houjiWrapperCall(uc,NativeAdrenoMethodBoCpuUnmap,&unmap,nullptr)))return 14;}
 if(!record(uc->clientClose()))return 15;uc->release();++p.closes;++p.rounds;
 return native_adreno_provider_poll()?16:0;
}
static int houjiProviderStart(void){
 auto&p=gHoujiProvider;p.phase=1;p.started_us=houjiSubmitNow(nullptr);
 unsigned pool_mib=0;PE_parse_boot_argn("gpool",&pool_mib,sizeof(pool_mib));
 if(pool_mib){
  if(pool_mib>(NativeAdrenoMemory::MaximumPoolBytes>>20))return p.code=kIOReturnBadArgument;
  native_adreno_pool_mib=pool_mib;
  p.code=NativeAdrenoMemory::reservePool(36,(size_t)pool_mib<<20);native_adreno_pool_status=p.code;
  if(!p.code){p.code=NativeAdrenoMemory::writeCombinePool();native_adreno_pool_wc=0x80000000u|p.code;}
  IOLog("HOUJI BO POOL INIT: MiB=%u reserve=%x WC=%x code=%x; XNU-owned contiguous CPU pages retained until reset\n",pool_mib,native_adreno_pool_status,native_adreno_pool_wc,p.code);
  if(p.code)return 1;
 }
 /* No client objects are mapped and no provider is published yet. The boot
  * owner's coexistence/coherence proofs are complete; retain their order and
  * allocations. Subsequent provider rounds exercise indexed maps and draws. */
 p.code=gHoujiApi.vm.enableAliasWorkspace();
 IOLog("HOUJI VM WORKSPACE: deferred=1 code=%x; after boot qualification, before provider and known-image rounds\n",p.code);
 if(p.code)return 1;
 const NativeAdrenoTimeIo time={nullptr,houjiProviderTimerRead};p.code=p.timer.attach(&time,NativeAdrenoTimeCp);if(p.code)return 1;
 p.code=native_adreno_publish(&gHoujiApi.vm,&gHoujiApi.submit,&gHoujiSubmit.ring,&gHoujiApi.clock,&p.timer,&gHoujiDeviceInfo,32,36);if(p.code)return 1;
 p.published=1;gHoujiApi.phase=11;p.phase=2;
 /* Exercise NC CPU views, known-image GPU readback, client close and pool
  * reuse before exposing the accelerator to the desktop. */
 if(pool_mib)for(unsigned i=0;i<2;i++){p.code=houjiProviderRound(i);if(p.code)return 1;}
 p.code=houji_native_accelerator_register();if(p.code)return 1;p.registry=1;
 if(gHoujiLive.mode==1 && !pool_mib)for(unsigned i=0;i<2;i++){p.code=houjiProviderRound(i);if(p.code)break;}
 IOLog("HOUJI PROVIDER RESULT: code=%u phase=%u published=%u registry=%u rounds=%u views=%u draws=%u closes=%u first-error=%x CRC=%x fence=%u/%u elapsed-us=%llu; kernel IOKit wrapper, no Mach-trap/Metal/desktop claim\n",p.code,p.phase,p.published,p.registry,p.rounds,p.views,p.draws,p.closes,p.first_error,p.crc,gHoujiSubmit.seq,gHoujiSubmit.observed,houjiSubmitNow(nullptr)-p.started_us);
 return p.code;
}
static int houjiProviderReset(void*){return houjiCpStop(nullptr,gHoujiSubmit.end);}
extern "C" void native_adreno_board_teardown_checkpoint(const char*step,uint32_t handle,uint32_t method){
 if(gHoujiLive.mode!=2 || !gHoujiLive.retained || !houjiNativeGpuFsAllowed())return;
 native_adreno_provider_snapshot();IOLog("HOUJI VIEW TEARDOWN: step=%s handle=%u method=%u seq=%u/%u generation=%llu; before operation, no GPU gate held\n",step,handle,method,gHoujiSubmit.seq,gHoujiSubmit.observed,gHoujiVm.binding.state->generation);
 houji_ufs_checkpoint("bounded first userspace view teardown/BO free; no GPU gate held, qualified independent UFS",0);
}
#endif

static void houjiDvfsWorker(void*,wait_result_t){
 thread_set_thread_name(current_thread(),"houji-gpu-dvfs");
 while(native_adreno_board_running()){(void)native_adreno_dvfs_poll();IOSleep(16);}
 thread_terminate(current_thread());
}
static void houjiDvfsStart(){   /* this port's GPU governor (HoujiGpuDvfs.inc, our TSENS caps), not upstream's */
 unsigned flags=0;PE_parse_boot_argn("houji_dvfs",&flags,sizeof(flags));gHoujiDvfs.enabled=(flags&2)!=0;
 if(gHoujiDvfs.enabled){thread_t t;if(kernel_thread_start(houjiDvfsWorker,nullptr,&t)==KERN_SUCCESS)thread_deallocate(t);}
}
