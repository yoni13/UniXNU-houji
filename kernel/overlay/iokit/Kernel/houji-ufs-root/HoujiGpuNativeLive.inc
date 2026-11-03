/* SPDX-License-Identifier: MIT
 * Bounded same-owner live handoff and independent UFS DMA qualification.
 * All GPU storage remains pinned to platform reset. No userspace provider yet.
 */
#ifndef HOUJI_GPU_NATIVE_LIVE_INC
#define HOUJI_GPU_NATIVE_LIVE_INC
extern "C" void houji_touch_driver_report(void);
extern "C" void houji_managed_physical_map_debug_report(void);
static struct {
 unsigned mode,requested,retained,ufs_proven,fs_trial,coexist,deny,phase,code,draws,pending_read,pending_write,worker;
 uint64_t dma_pa,dma_owned;uint32_t before_fence,after_fence,crc;
} gHoujiLive;
static bool houjiNativeGpuFsAllowed(void){
 if(!gHoujiFull.possible)return true;
 if(__atomic_load_n(&gHoujiLive.deny,__ATOMIC_ACQUIRE))return false;
 if(gHoujiCp.isolated)return true;
 return __atomic_load_n(&gHoujiLive.coexist,__ATOMIC_ACQUIRE) || (gHoujiLive.fs_trial && gHoujiLive.ufs_proven);
}
static int houjiLiveCoexistPublish(const volatile uint32_t*words,size_t bytes){
 auto&s=gHoujiSubmit;if(!gHoujiLive.fs_trial || !houjiApiInOperation() || houjiSubmitGuard() || s.published || s.pending ||
  words!=houjiCpCpu() || bytes!=HHR_BYTES || s.iteration<81 || s.iteration>84)return 1;
 uint32_t expected[128];unsigned n=0;
 const uint32_t prologue[]={hhr_pkt7(0x17,1),0x80000001,hhr_pkt7(0x46,1),0x32,hhr_pkt7(0x46,1),0x33,hhr_pkt7(0x26,0),hhr_pkt7(0x17,1),3,hhr_pkt7(0x65,1),0x101};
 for(auto w:prologue)expected[n++]=w;
 for(unsigned i=0;i<16;i++){expected[n++]=0x70bf8003;expected[n++]=0x40008000;expected[n++]=0;expected[n++]=HGKI_SELECTED_COMMAND_WORDS*14;}
 const uint32_t fence=HHR_SCRATCH+0x104,bv=HHR_SCRATCH+0x10c,seq=s.iteration;
 const uint32_t tail[]={0x48088501,seq,hhr_pkt7(0x17,1),1,hhr_pkt7(0x46,1),0x18,hhr_pkt7(0x46,1),0x19,hhr_pkt7(0x17,1),2,
  hhr_pkt7(0x46,4),0x08000031,bv,0,seq,hhr_pkt7(0x17,1),1,hhr_pkt7(0x14,4),0,bv,0,seq,
  hhr_pkt7(0x46,4),0x88000031,fence,0,seq,hhr_pkt7(0x17,1),3,hhr_pkt7(0x65,1),0x100};
 for(auto w:tail)expected[n++]=w;
 if(n!=106)return 1;for(unsigned i=0;i<n;i++)if(words[(s.end+i)&(HHR_WORDS-1)]!=expected[i])return 1;
 s.expected_end=(s.end+n)&(HHR_WORDS-1);s.published=1;houjiStockFullBarrier();return 0;
}
static bool houjiLiveDmaOwner(void){
 auto&l=gHoujiLive;
 if(!gHost.ready || gHost.poisoned || !gHost.dma || !gHost.virt || !gHost.phys ||
  (gHost.phys&16383) || ((uintptr_t)gHost.virt&16383) || gHost.dma->getBytesNoCopy()!=gHost.virt || gHost.dma->getLength()!=kDmaBytes)return false;
 IOByteCount length=0;uint64_t pa=gHost.dma->getPhysicalSegment(0,&length,kIOMemoryMapperNone);
 uint64_t owned=(kDmaBytes+16383u)&~uint64_t(16383);
 if(pa!=gHost.phys || length<kDmaBytes || !owned || pa>=(UINT64_C(1)<<36) || owned>(UINT64_C(1)<<36)-pa)return false;
 for(unsigned i=0;i<15;i++){const auto&e=gHoujiFullMemory.entry(i);if(hzr_overlap(pa,owned,e.backing.physical,e.request.owned_bytes))return false;}
 for(unsigned i=0;i<3;i++){const auto&e=gHoujiGpuExternalCanary.info[i];if(hzr_overlap(pa,owned,e.pa,e.owned))return false;}
 for(unsigned i=0;i<9;i++){const auto&e=gHoujiFirmwareMemory.info[i];if(hzr_overlap(pa,owned,e.pa,e.owned))return false;}
 for(unsigned i=0;i<3;i++){auto*b=gHoujiApi.bo[i];if(!b || !houjiApiBo(i,0))return false;for(size_t j=0;j<b->ownedSpanCount();j++)if(hzr_overlap(pa,owned,b->ownedSpans()[j].physical,b->ownedSpans()[j].bytes))return false;}
 for(unsigned i=0;i<12;i++){const auto&e=gHoujiVm.binding.boot_ranges[i];if(hzr_overlap(pa,owned,e.physical,e.bytes))return false;}
 for(uint64_t off=0;off<kDmaBytes;off+=4096){uint64_t r=0,w=0,va=(uintptr_t)gHost.virt+off+0x5a;
  if(!houjiFullHostAt(va,false,&r) || !houjiFullHostAt(va,true,&w) || r!=w || (r&1) ||
   ((r>>56)!=0xff && (r>>56)!=0x44) || ((r&UINT64_C(0x0000fffffffff000))|(va&4095))!=pa+off+0x5a)return false;
 }
 l.dma_pa=pa;l.dma_owned=owned;return true;
}
static int houjiLiveQualifyUfs(void){
 auto&l=gHoujiLive;auto&a=gHoujiApi;auto&s=gHoujiSubmit;
 l.phase=1;if(a.code || a.phase!=8 || a.draws!=16 || a.unmaps!=3 || s.completed!=80 || !houjiVmIdle() ||
  !houjiFullLease() || !houjiFullTables() || !houjiLiveDmaOwner())return l.code=1;
 l.ufs_proven=l.fs_trial=1;a.active=a.in_op=1;
 IOLog("HOUJI LIVE DMA: UFS=%llx owned=%llu descriptor=%u; disjoint from all27 retained GPU/GMU BOs,3 new BOs and both actual roots; new bounded coexistence trial\n",l.dma_pa,l.dma_owned,kDmaBytes);
 static uint8_t header[4096];
 if(hostRead4k(0,kHoujiRootFirst,1,header)!=kIOReturnSuccess || !houjiHfsRootHeader(header,sizeof(header)))return l.code=2;
 houji_ufs_checkpoint("live GPU idle plus independent UFS read and preallocated-log commit qualification",0);
 l.phase=2;
 for(unsigned i=0;i<3;i++){
  NativeAdrenoAbiBoMap request={a.handle[i],a.va[i],i==2?1u:3u,0,3,0};NativeAdrenoAbiBoMapReply reply{};
  if(a.client.dispatch(NativeAdrenoMethodBoMap,&request,sizeof(request),&reply,sizeof(reply),&a.clock) || reply.iova!=a.va[i] || houjiApiAuditTables())return l.code=3;
 }
 for(unsigned frame=0;frame<4;frame++){
  l.phase=3;if(!houjiVmIdle() || a.submit.outstanding())return l.code=4;
  auto*out=(volatile uint8_t*)a.bo[0]->mapping();auto*cmd=(volatile uint32_t*)a.bo[2]->mapping();
  for(unsigned i=0;i<16384;i++)out[i]=0x6d;
  for(unsigned i=0;i<4096;i++){uint32_t word=i<HGKI_SELECTED_COMMAND_WORDS*14?hgki_command_word(i%HGKI_SELECTED_COMMAND_WORDS):0;cmd[i]=word==HGKI_OUTPUT?0x40000000:word;}
  houjiStockFullBarrier();a.phase=9;s.published=s.pending=0;s.iteration=s.seq+1;
  NativeAdrenoAbiSubmit request{};NativeAdrenoAbiSubmitReply reply{};request.buffer_count=16;
  for(unsigned i=0;i<16;i++)request.buffers[i]={a.handle[2],a.va[2],HGKI_SELECTED_COMMAND_WORDS*14*4};
  if(a.client.dispatch(NativeAdrenoMethodSubmit,&request,sizeof(request),&reply,sizeof(reply),&a.clock) || reply.fence!=s.iteration)return l.code=5;s.seq=reply.fence;
  houjiStockFullBarrier();l.before_fence=houjiCpCpu()[(HHR_SCRATCH+0x104-HHR_RING)/4];
  if(frame&1){if(l.before_fence!=s.seq)++l.pending_write;houji_ufs_checkpoint("new bounded live-GPU/UFS concurrent fixed-log-write trial; all DMA buffers disjoint and retained",0);}
  else {if(l.before_fence!=s.seq)++l.pending_read;if(hostRead4k(0,kHoujiRootFirst,1,header)!=kIOReturnSuccess || !houjiHfsRootHeader(header,sizeof(header)))return l.code=6;}
  houjiStockFullBarrier();l.after_fence=houjiCpCpu()[(HHR_SCRATCH+0x104-HHR_RING)/4];
  NativeAdrenoAbiWaitFence wait={reply.fence,500000000};if(a.client.dispatch(NativeAdrenoMethodWaitFence,&wait,sizeof(wait),nullptr,0,&a.clock))return l.code=7;
  s.observed=s.ring.completedFence();if(s.observed!=s.seq || houjiSubmitDrainPointers() || a.submit.outstanding() || houjiApiAuditTables())return l.code=8;
  l.crc=hgki_crc32((const uint8_t*)out,4096);if(l.crc!=0xe843b26a)return l.code=9;
  for(unsigned i=0;i<16384;i++){uint8_t want=i<4096?((i&3)==0 || (i&3)==3?0xff:0):0x6d;if(out[i]!=want)return l.code=10;}
  ++l.draws;++s.completed;IOLog("HOUJI LIVE UFS STEP: frame=%u operation=%s before=%u after=%u final=%u CRC=%x RPTR=%u WPTR=%u;224 fixed blits per GPU job\n",frame,frame&1?"write-checkpoint":"read-root",l.before_fence,l.after_fence,s.observed,l.crc,s.rptr,s.wptr);
 }
 l.phase=4;
 for(unsigned i=0;i<3;i++)if(a.client.unmapRetainedForTrial(a.handle[i]) || !houjiApiBo(i,0) || houjiApiAuditTables())return l.code=11;
 if(!l.pending_read || !l.pending_write)return l.code=12;
 __atomic_store_n(&l.coexist,1u,__ATOMIC_RELEASE);l.fs_trial=0;a.phase=10;l.phase=5;
 IOLog("HOUJI LIVE UFS RESULT: code=0 frames=%u read-pending=%u write-pending=%u fence=%u/%u CRC=%x generation=%llu; independent UFS read/write and GPU execution qualified for this retained owner only\n",l.draws,l.pending_read,l.pending_write,s.seq,s.observed,l.crc,gHoujiVm.binding.state->generation);
 return 0;
}
static int houjiCpKeepLive(void*,hhr_result*r){
 auto&l=gHoujiLive;if(!l.requested || !r || r->code || r->phase!=7)return 0;
 gHoujiSubmit.active=1;
 if(houjiLiveQualifyUfs()){IOLog("HOUJI LIVE REFUSED: phase=%u code=%u seq=%u/%u; stop and retain all resources\n",l.phase,l.code,gHoujiSubmit.seq,gHoujiSubmit.observed);gHoujiApi.active=gHoujiApi.in_op=0;gHoujiSubmit.active=0;r->code=HHR_POST_INIT;return 0;}
 if(!houjiVmIdle() || !houjiFullLease() || !houjiFullTables())return 0;
 r->rptr=gHoujiSubmit.rptr;r->wptr=gHoujiSubmit.wptr;l.retained=1;return 1;
}
static int houjiFullKeepLive(void*){return gHoujiLive.requested && gHoujiLive.retained && gHoujiLive.coexist && !gHoujiLive.code && !houjiSubmitGuard() && houjiVmIdle() && houjiFullLease() && houjiFullTables()?1:0;}
#include "HoujiGpuNativeProvider.inc"
#include "HoujiGpuCoherenceProbe.inc"
#include "HoujiGpuProbeLog.inc"
static void houjiLiveDeadline(void*,wait_result_t){
 thread_set_thread_name(current_thread(),"houji-gpu-guard");uint64_t began=houjiSubmitNow(nullptr);
 while(houjiSubmitNow(nullptr)-began<(gHoujiLive.mode==3?210000000ull:360000000ull))IOSleep(1000);
 /* Independent of provider/VFS locks. All device resources remain pinned;
  * the host prearmed recovery BCB. Panic recovery does no filesystem I/O. */
 native_adreno_provider_snapshot();panic("HOUJI LIVE: independent runtime deadline; retained resources, PSCI-only recovery, no FS");
}
static void houjiLiveWorker(void*,wait_result_t){
 thread_set_thread_name(current_thread(),"houji-gpu-owner");auto&l=gHoujiLive;uint64_t started=houjiSubmitNow(nullptr),busy_since=0,last_saved=UINT64_MAX,last_report=0,last_desktop_save=0,first_draw=0,first_fence=0,base_swaps=0,base_presents=0;unsigned checks=0,probe_state=0;
 if(l.mode>=3)houji_framebuffer_counts(&base_swaps,&base_presents);
 for(;;){
  IOSleep(l.mode>=3?500:l.mode==2?250:100);++checks;
  if(l.mode==2 && !(checks&3)){
   uint64_t progress=native_adreno_provider_progress_token();
   if(progress!=last_saved){
    native_adreno_provider_snapshot();IOLog("HOUJI LIVE PROGRESS: checks=%u elapsed-us=%llu fence=%u/%u; changed progress before provider gate\n",checks,houjiSubmitNow(nullptr)-started,gHoujiSubmit.seq,gHoujiSubmit.observed);
    if(houjiNativeGpuFsAllowed())houji_ufs_checkpoint("qualified same-owner userland progress, before provider gate; no GPU lock held",0);
    last_saved=progress;
   }
  }
  IOReturn polled=l.mode>=2?native_adreno_provider_try_poll():native_adreno_provider_poll();
  if(polled==kIOReturnBusy && l.mode>=2){
   if(!busy_since)busy_since=houjiSubmitNow(nullptr);
   if(houjiSubmitNow(nullptr)-busy_since>10000000)panic("HOUJI LIVE: provider call held gate over10s; all resources pinned, last progress saved, no-FS PSCI");
  }else {busy_since=0;if(polled){l.code=13;break;}}
  if(l.mode>=3){
   if(gHoujiDvfs.failed){l.code=16;break;}
   uint64_t now=houjiSubmitNow(nullptr);
   if(now-last_report>=5000000){
    uint64_t stats[7]={},swaps=0,presents=0;IOReturn observed=native_adreno_provider_desktop_stats(stats);houji_framebuffer_counts(&swaps,&presents);last_report=now;
    IOLog("HOUJI DESKTOP: observed=%x backboard-submits=%llu waits=%llu submit-errors=%llu client-last-fence=%llu real-completed=%llu jobs=%llu ring-last=%llu swaps=%llu presents=%llu elapsed-us=%llu; actual IOKit caller and GPU fence\n",observed,stats[0],stats[1],stats[2],stats[3],stats[4],stats[5],stats[6],swaps-base_swaps,presents-base_presents,now-started);
    houji_framebuffer_report();
    if(!observed && stats[0] && !first_draw){first_draw=now;first_fence=stats[4];}
    if(!observed && stats[2]){l.code=15;break;}
    if(now-last_desktop_save>=20000000){houji_touch_driver_report();houji_managed_physical_map_debug_report();houjiProviderPerfReport();native_adreno_perf_report();native_adreno_provider_snapshot();if(houjiNativeGpuFsAllowed())houji_ufs_checkpoint("desktop actual-submit/fence/display sampling; no GPU gate held",0);last_desktop_save=now;}
    if(l.mode==3 && first_draw && now-first_draw>=120000000 && !observed && stats[0]>=64 && stats[4]>=first_fence+32 && presents-base_presents>=30){IOLog("HOUJI DESKTOP: GPU and display activity sampling complete; physical UI confirmation still required\n");break;}
   }
   if(l.mode==3 && now-started>=360000000)break;
  }else if(l.mode==2){
   probe_state=native_adreno_provider_probe_result();
   /* The per-boot started flag proves this process cleared the previous log.
    * One small cached-file read per second catches its userspace watchdog. */
   if(!probe_state && !(checks&3) && native_adreno_provider_probe_started())probe_state=houjiProbeLogState();
   if(probe_state && polled!=kIOReturnBusy)break;
   if(houjiSubmitNow(nullptr)-started>=300000000)break;
  }else if(checks>=100)break;
 }
 houjiProviderPerfReport();native_adreno_provider_snapshot();IOLog("HOUJI LIVE LIFETIME: worker=%u mode=%u checks=%u elapsed-us=%llu probe-state=%u code=%u fence=%u/%u; original roots and all active BO retained\n",l.worker,l.mode,checks,houjiSubmitNow(nullptr)-started,probe_state,l.code,gHoujiSubmit.seq,gHoujiSubmit.observed);
 gHoujiApi.in_op=1;
 if(native_adreno_provider_stop(houjiProviderReset,nullptr)){__atomic_store_n(&l.deny,1u,__ATOMIC_RELEASE);panic("HOUJI LIVE: stop/isolation uncertain; no FS, every BO retained");}
 gHoujiApi.status.noteSuspend();gHoujiCp.active=0;gHoujiFull.active=0;gHoujiSubmit.active=0;gHoujiApi.in_op=gHoujiApi.active=0;
 /* Commit terminal GPU/call state before any diagnostic vnode lookup can block. */
 houjiFullReport();houjiCpSaveFinal();
 if(l.mode==2){houjiProbeLogDump();houjiCpSaveFinal();}
 if(l.mode>=3){houjiWallpaperLogDump();houjiCpSaveFinal();houjiKeyboardLogDump();houjiCpSaveFinal();}
 panic("HOUJI LIVE: runtime ended mode=%u code=%u; retained PSCI recovery",l.mode,l.code);
}
static bool houjiLiveStart(void){
 if(!houjiFullKeepLive(nullptr))return false;
 {unsigned coherent=1;PE_parse_boot_argn("houji_gpu_coherent",&coherent,sizeof(coherent));
  if((gHoujiLive.mode==4 && coherent) || gHoujiLive.mode==5)houjiCoherenceTrial();}
 if(houjiProviderStart())return false;
 houjiDvfsStart();
 IOLog("HOUJI LIVE COMMIT: exact retained handoff and coexistence before userland; completed=%u real jobs\n",gHoujiSubmit.completed);
 houji_ufs_checkpoint("new live handoff and UFS coexistence qualification completed; original GPU owner/roots remain active",0);
 thread_t t;
 if(kernel_thread_start(houjiLiveWorker,nullptr,&t)!=KERN_SUCCESS)return false;
 thread_deallocate(t);gHoujiLive.worker=1;
 if(gHoujiLive.mode==2 || gHoujiLive.mode==3){thread_t guard;if(kernel_thread_start(houjiLiveDeadline,nullptr,&guard)!=KERN_SUCCESS)panic("HOUJI LIVE: independent guard creation failed; all resources pinned, no-FS PSCI");thread_deallocate(guard);}
 return true;
}
#endif
