/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Fixed r131 CP_INIT/two-MEM_WRITE profile. No PAS, secure-mode transition,
 * arbitrary IB, shaders, user submissions or resource reclamation.
 */
#ifndef HOUJI_GPU_NATIVE_CP_INC
#define HOUJI_GPU_NATIVE_CP_INC
#include "GpuHostRing.h"
static void houjiRuntimeAtStop(void);
static int houjiGfxPrepare(void);
static int houjiGfxCapabilities(void);
static unsigned houjiGfxCanaryBase(void);
static bool houjiGfxRingWrite(uint32_t,uint32_t);
static struct {
 hhr_result ring;ggs_state aperture;
 unsigned active,once,prepared,aperture_kind,aperture_ok,enabled,stopping,isolated,stop_stage;
 unsigned published,peeked,reads,writes,smc_calls,record_verified,save_attempted,save_returned;
 uint32_t ack[3];uint64_t roots[4];
} gHoujiCp;
#ifdef HOUJI_FULL_HOST_NATIVE_HOST_TEST
static int houjiCpHostSmc(ggs_regs*);
#endif
static bool houjiCpReadAllowed(uint32_t a){
 const uint32_t addresses[]={0x3d02600,0x3d02604,0x3d00058,0x3d0005c,0x3d0f114,0x3d0f118,0x3d02800,0x3d3e00c,0x3d3e000,0x3d3e004,0x3d3e008,0x3d02000,0x3d02004,0x3d02008,0x3d02010,0x3d02014,0x3d02018,0x3d0201c,0x3d02020,0x3d02a60,0x3d02a64,0x3d00840,0x3d00804,0x3d02084,0x3d8ec30,0x3d8ec4c};
 for(auto x:addresses)if(a==x)return true;return false;
}
/* The register half of the lease (power gates, configuration, GMU state, stream routes, table roots: 20+ slow MMIO reads, about
 * 90 us) is static while the owner runs; it was repeated five or more times per submission and on every fence poll. A passed
 * check is reused for 2 ms. The software state above it, and houjiCpGuard's fault registers, are still checked on every call. */
static uint64_t gHoujiLeaseValidUntil;
static volatile uint64_t gHoujiLeaseStats[2];   /* full register checks, reuses */
static bool houjiCpBusLease(void){
 auto&c=gHoujiCp;auto&p=gHoujiFull;if(!c.active || !p.active || p.lost || p.stage!=FHR_PRE_CP_BOUNDARY || p.held!=FHG_ALL || !p.zero_held || !p.record_published){gHoujiLeaseValidUntil=0;return false;}
 const uint64_t now=mach_absolute_time();
 /* Never reused while the CP is being stopped or is isolated: that sequence changes exactly this state. */
 if(c.stopping || c.isolated)gHoujiLeaseValidUntil=0;
 else if(now<__atomic_load_n(&gHoujiLeaseValidUntil,__ATOMIC_ACQUIRE)){gHoujiLeaseStats[1]=gHoujiLeaseStats[1]+1;return true;}
 gHoujiLeaseStats[0]=gHoujiLeaseStats[0]+1;
 if(!houjiFullConfig() || !houjiFullPower()){gHoujiLeaseValidUntil=0;return false;}
 uint32_t v=0;uint64_t q=0;
 if(houjiFullRaw32(0x3d7e340,&v) || (v&3) || houjiFullRaw32(0x3d8ec40,&v) || v){gHoujiLeaseValidUntil=0;return false;}
 for(unsigned i=0;i<3;i++){const uint32_t smr=i?0x80000000u+(i-1):0x80000005u;if(houjiFullRaw32(0x3da0800+4*i,&v) || v!=smr || houjiFullRaw32(0x3da0c00+4*i,&v) || v!=(i?0u:2u)){gHoujiLeaseValidUntil=0;return false;}}
 const uint32_t roots[]={0x3dc0020,0x3dc0028,0x3dc2020,0x3dc2028};const uint64_t wanted[]={p.gpu_fields[5],p.gpu_fields[6],p.stock.encoded[5],p.stock.encoded[6]};
 for(unsigned i=0;i<4;i++)if(houjiFullRaw64(roots[i],&q) || q!=wanted[i]){gHoujiLeaseValidUntil=0;return false;}
 uint64_t window=0;nanoseconds_to_absolutetime(2000000,&window);
 __atomic_store_n(&gHoujiLeaseValidUntil,now+window,__ATOMIC_RELEASE);
 return true;
}
extern "C" void houji_cp_lease_report(void){IOLog("HOUJI CP LEASE: full-checks=%llu reused=%llu; live\n",gHoujiLeaseStats[0],gHoujiLeaseStats[1]);}
static int houjiCpGuard(void*){
 auto&c=gHoujiCp;uint32_t v=0;if(!houjiCpBusLease() || c.stopping || c.isolated)return 1;
 if(!c.enabled && (houjiFullRaw32(0x3d8ec30,&v) || (v&(1u<<23)))){gHoujiLeaseValidUntil=0;return 1;}
 const uint32_t faults[]={0x3da0048,0x3dc0058,0x3dc2058};for(unsigned i=0;i<3;i++)if(houjiFullRaw32(faults[i],&v) || (v&(i?0xc00001feu:0xffffffffu))){gHoujiLeaseValidUntil=0;return 1;}
 return 0;
}
static int houjiCpRead(void*,uint32_t a,uint32_t*v){if(!v || !houjiCpReadAllowed(a) || houjiCpGuard(nullptr))return 1;++gHoujiCp.reads;return houjiFullRaw32(a,v);}
static int houjiCpStore(uint32_t a,uint32_t v){
 auto*ptr=houjiFullPointer(a,4);if(!ptr)return 1;auto&p=gHoujiFull;p.last_address=a;p.last_op=2;p.last_width=4;p.last_value=v;p.last_attempted=1;p.last_returned=0;
 *reinterpret_cast<volatile uint32_t*>(ptr)=v;houjiStockWriteBarrier();p.last_returned=1;++p.writes;++gHoujiCp.writes;return 0;
}
static int houjiCpWrite(void*,uint32_t a,uint32_t v){
 auto&c=gHoujiCp;auto&r=c.ring;if(!c.prepared || houjiCpGuard(nullptr))return 1;
 const uint32_t addresses[]={0x3d02000,0x3d02004,0x3d02008,0x3d02010,0x3d02014,0x3d02a60,0x3d02a64};
 const uint32_t values[]={HHR_RING,0,0x20c,HHR_SCRATCH+0x100,0,HHR_SCRATCH+0x108,0};bool allowed=false;
 for(unsigned i=0;i<7;i++)if(r.phase==2 && !c.enabled && a==addresses[i] && v==values[i])allowed=true;
 if(a==0x3d02020 && v==1 && r.phase==3 && r.enabled && !c.enabled && c.published==1 && c.aperture_ok){allowed=true;c.enabled=1;}
 if(a==0x3d0201c && c.enabled && v==r.wptr && ((r.phase==4 && (v==10 || houjiGfxRingWrite(a,v))) || (r.phase==5 && v==houjiGfxCanaryBase()+5) || (r.phase==6 && v==houjiGfxCanaryBase()+10)))allowed=true;
 return allowed?houjiCpStore(a,v):1;
}
static volatile uint32_t*houjiCpCpu(void){return (volatile uint32_t*)((uintptr_t)gHoujiFullMemory.entry(12).backing.cpu+0xc0000);}
static int houjiCpPrepare(void*){
 auto&c=gHoujiCp;if(c.prepared || houjiCpGuard(nullptr) || !houjiFullLease() || !houjiFullTables())return 1;
 auto*cpu=houjiCpCpu();for(unsigned i=0;i<0x40000/4;i++)if(cpu[i])return 1;
 const uint32_t a[]={0x3d0f114,0x3d00058,0x3d02800,0x3d3e00c,0x3d3e000,0x3d3e004,0x3d3e008};
 for(unsigned i=0;i<7;i++){uint32_t before=0,after=0;if(houjiCpGuard(nullptr) || houjiFullRaw32(a[i],&before) || houjiCpStore(a[i],0) || houjiFullRaw32(a[i],&after) || after)return 1;}
 c.prepared=1;return houjiGfxPrepare() || houjiCpGuard(nullptr);
}
static int houjiCpSmc(void*,ggs_regs*r){
 auto&c=gHoujiCp;if(!r || houjiCpGuard(nullptr) || r->x[7])return 1;
 const uint64_t fn=c.aperture_kind==1?0x42000601:0x42000c1b;
 if(!((r->x[0]==fn && !r->x[6]) || (r->x[0]==1 && c.aperture.last_output.x[0]==1 && r->x[6]==c.aperture.last_output.x[6])))return 1;
 if(c.aperture_kind==1){if(r->x[1]!=1 || r->x[2]!=0x02000c1b || r->x[3] || r->x[4] || r->x[5])return 1;}
 else if(c.aperture_kind!=2 || r->x[1]!=4 || r->x[2]!=0xffff0000u || r->x[3]!=0xffffffffu || r->x[4]!=0xffffffffu || r->x[5]!=0xffffffffu)return 1;
 ++c.smc_calls;
#ifdef HOUJI_FULL_HOST_NATIVE_HOST_TEST
 return houjiCpHostSmc(r);
#else
 return ggs_aarch64_call(nullptr,r);
#endif
}
static int houjiCpBusyDelay(void*,unsigned ms){if(ms!=GGS_BUSY_DELAY_MS || houjiCpGuard(nullptr))return 1;IOSleep(ms);return houjiCpGuard(nullptr);}
static int houjiCpAperture(void*){
 auto&c=gHoujiCp;if(!c.prepared || c.aperture_kind || houjiCpGuard(nullptr))return 1;
 c.aperture.convention_verified=gHoujiGpuSecureCx.scm.convention_verified;c.aperture.init_succeeded=gHoujiGpuSecureCx.scm.init_succeeded;
 const ggs_ops ops={nullptr,houjiCpSmc,houjiCpBusyDelay,nullptr};c.aperture_kind=1;
 if(ggs_exchange(&ops,&c.aperture,0x42000601,0x02000c1b) || !c.aperture.last_output.x[1])return 1;c.aperture_kind=2;
 const ggs_regs request={{0x42000c1b,4,0xffff0000u,0xffffffffu,0xffffffffu,0xffffffffu,0,0}};
 if(ggs_exchange_frame(&ops,&c.aperture,&request) || c.aperture.last_output.x[0] || houjiCpGuard(nullptr))return 1;c.aperture_ok=1;return houjiGfxCapabilities();
}
static int houjiCpPublish(void*,unsigned offset,const uint32_t*words,unsigned count){
 auto&c=gHoujiCp;auto&r=c.ring;if(!words || houjiCpGuard(nullptr))return 1;
 const uint32_t init[]={0x70970001,0x08000000,0x70c80007,0x94b,3,0x20000000,2,HHR_RECORD,0,0x80000000};
 const uint32_t packet[]={0x703d8003,(uint32_t)HHR_SCRATCH+(r.phase==6?4u:0u),0,0xc0ffee00u+(r.phase==6?1:0),0x70928000};
 const uint32_t*expected=nullptr;
 if(r.phase==2 && offset==0 && count==10 && !c.published)expected=init;
 else if(r.phase==5 && offset==houjiGfxCanaryBase() && count==5 && c.published==1)expected=packet;
 else if(r.phase==6 && offset==houjiGfxCanaryBase()+5 && count==5 && c.published==2)expected=packet;
 if(!expected)return 1;auto*cpu=houjiCpCpu();for(unsigned i=0;i<count;i++)if(words[i]!=expected[i] || cpu[offset+i])return 1;
 for(unsigned i=0;i<count;i++)cpu[offset+i]=words[i];houjiStockFullBarrier();
 for(unsigned i=0;i<count;i++)if(cpu[offset+i]!=words[i])return 1;++c.published;return houjiCpGuard(nullptr);
}
static int houjiCpPeek(void*,unsigned i,uint32_t*v){if(!v || i>1 || houjiCpGuard(nullptr))return 1;houjiStockFullBarrier();*v=houjiCpCpu()[HHR_WORDS+i];++gHoujiCp.peeked;return 0;}
static int houjiCpDelay(void*,unsigned us){if((us!=10 && us!=100) || !houjiCpBusLease())return 1;IODelay(us);return 0;}
static int houjiCpCheckpoint(void*){return gHoujiCp.enabled || !gHoujiCp.aperture_ok || !gHoujiCp.prepared || !houjiFullLease() || houjiCpGuard(nullptr);}
static int houjiCpEngine(void*){return !gHoujiCp.enabled || houjiCpGuard(nullptr);}
static void houjiCpSnapshot(void*,unsigned stage){if(stage==4)houjiRuntimeAtStop();const auto&r=gHoujiCp.ring;IOLog("HOUJI NATIVE CP STEP: %u phase=%u code=%u kicks=%u RPTR=%u WPTR=%u idle=%u writebacks=%u words=%08x/%08x OP=%u A=%08x V=%08x valid=%u; RAM only\n",stage,r.phase,r.code,r.kicks,r.rptr,r.wptr,r.init_idle,r.writebacks,r.observed[0],r.observed[1],r.last_operation,r.last_address,r.last_value,r.last_valid);}
static int houjiCpStop(void*,uint32_t){
 auto&c=gHoujiCp;if(c.stopping || !c.enabled || !houjiCpBusLease())return 1;c.stopping=1;c.stop_stage=1;
 gHoujiFull.reset_attempted=1;if(houjiCpStore(0x3d7e000,1))return 1;uint32_t reset=0;if(houjiFullRaw32(0x3d7e000,&reset) || reset!=1)return 1;gHoujiFull.reset_returned=1;
 const uint32_t a[]={0x3d00058,0x3d0f114,0x3d0f114},ack[]={0x3d0005c,0x3d0f118,0x3d0f118},value[]={1,1,2};
 for(unsigned i=0;i<3;i++){c.stop_stage=2+i;if(!houjiCpBusLease() || houjiCpStore(a[i],value[i]))return 1;bool ok=false;for(unsigned j=0;j<1000;j++){if(!houjiCpBusLease() || houjiFullRaw32(ack[i],&c.ack[i]))return 1;if((c.ack[i]&value[i])==value[i]){ok=true;break;}IODelay(10);}if(!ok)return 1;}
 if(!houjiCpBusLease() || !houjiFullLease() || !houjiFullTables())return 1;c.stop_stage=5;c.isolated=1;return 0;
}
#include "HoujiGpuNativeGfx.inc"
#include "HoujiGpuNativeSubmit.inc"
static void houjiFullReport(void);
static void houjiCpSaveFinal(void);
#include "HoujiGpuNativeLive.inc"
static int houjiCpAfterPrepare(void*){
 auto&c=gHoujiCp;if(c.once)return 1;c.once=1;c.active=1;
 const hhr_io ops={nullptr,houjiCpGuard,houjiCpPrepare,houjiCpAperture,houjiCpRead,houjiCpWrite,houjiCpPublish,houjiCpPeek,houjiCpDelay,houjiCpCheckpoint,houjiCpSnapshot,houjiCpStop,houjiCpEngine,houjiGfxPostInit,houjiCpKeepLive};
 unsigned code=hhr_run(&ops,&c.ring);
 IOLog("HOUJI NATIVE CP RESULT: code=%u phase=%u enabled=%u kicks=%u init-idle=%u writebacks=%u RPTR=%u WPTR=%u words=%08x/%08x stop=%u/%u ACK=%x/%x/%x SMC=%u; PAS/ZAP/2D in GFX RESULT, no Metal\n",code,c.ring.phase,c.enabled,c.ring.kicks,c.ring.init_idle,c.ring.writebacks,c.ring.rptr,c.ring.wptr,c.ring.observed[0],c.ring.observed[1],c.stop_stage,c.isolated,c.ack[0],c.ack[1],c.ack[2],c.smc_calls);
 if(c.ring.retained){IOLog("HOUJI NATIVE CP LIVE: retained=1 stopped=0 reset=0;84 native GPU jobs and independent UFS coexistence verified\n");return code;}
 houjiGfxAfterStop();c.active=0;return code || !c.isolated;
}
static bool houjiCpFinalSaveGate(void){
 const auto&c=gHoujiCp;const auto&p=gHoujiFull;
 if(c.active || c.once!=1 || !c.enabled || !c.isolated || c.stop_stage!=5 || !(c.ack[0]&1) || !(c.ack[1]&1) || !(c.ack[2]&2) ||
  p.active || p.lost || !p.reset_attempted || !p.reset_returned || p.held!=FHG_ALL || !p.zero_held || !houjiFullLease() || !houjiFullTables())return false;
 for(unsigned i=0;i<7;i++){uint32_t v=0;if(houjiFullRaw32(hsnp_gate_address(i),&v) || (v&hsnp_gate_mask(i))!=hsnp_gate_value(i))return false;}
 uint32_t reset=0,gx=0,arb=0;
 return !houjiFullRaw32(0x3d7e000,&reset) && reset==1 && !houjiFullRaw32(0x3d0005c,&gx) && (gx&1) && !houjiFullRaw32(0x3d0f118,&arb) && (arb&3)==3;
}
static void houjiCpSaveFinal(void){
 auto&c=gHoujiCp;
 if(!houjiCpFinalSaveGate()){IOLog("HOUJI NATIVE CP LOG: no qualified bus isolation; retain/noFS\n");return;}
 c.save_attempted=1;IOLog("HOUJI NATIVE CP LOG: CM3 reset1 plus fresh GX/client/arb halt ACK1/3/3; all BO retained, no route/unmap/free; saving terminal records\n");
 houji_ufs_checkpoint("CP terminal records after real CM3-reset and three GPU/GBIF halt acknowledgements; no resources released",0);
 c.save_returned=1;IOLog("HOUJI NATIVE CP LOG: save returned; retained PSCI recovery next\n");
}
#endif
