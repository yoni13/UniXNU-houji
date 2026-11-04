/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Candidate: independent XNU bank2/SID5 fixed external RAM canary, no stock
 * firmware/HFI/GPU stream/CP. Reset1 alone never authorizes CPU data access.
 * The scalar /chosen record conveys no UEFI pointer or ownership. Private
 * Device maps and original values remain retained until platform reset.
 * Include after normal-context houji_ufs_checkpoint and its logging flags.
 * False MUST select the armed no-FS panic/reset path before another checkpoint.
 * A bounded software poll cannot recover a CPU-stalled MMIO instruction.
 */
#ifndef HOUJI_GPU_EXTERNAL_CANARY_PROBE_INC
#define HOUJI_GPU_EXTERNAL_CANARY_PROBE_INC
#include "GmuExternalCanary.h"
#include "Adreno740/NativeAdrenoMemory.h"
#ifndef HOUJI_GPU_EXTERNAL_CANARY_PROBE_HOST_TEST
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/cpu_data.h>
#include <mach/vm_param.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSharedPtr.h>
#include <pexpert/pexpert.h>
#endif
extern "C" vm_offset_t phystokv(uint64_t);
enum {GECN_WINDOWS=6,GECN_FRESH_SLOTS=32,GECN_FRESH_LIMIT=2*GEC_POLLS+64,
 GECN_BOS=3,GECN_TABLE=0,GECN_DATA=1,GECN_ZERO=2,GECN_CPU_BYTES=16384};
enum {GECN_OK,GECN_FLAGS,GECN_PROPERTY,GECN_RECORD,GECN_OWNER,GECN_ORDER,
 GECN_ADDRESS,GECN_CAPACITY,GECN_ALLOC,GECN_MAP,GECN_MAPPING,GECN_SENTINEL,
 GECN_VALUE,GECN_PHASE,GECN_COMMIT,GECN_DELAY,GECN_GEOMETRY,GECN_MEMORY,GECN_AT,GECN_TABLES,GECN_CACHE};
static const uint64_t kHoujiExternalBase[GECN_WINDOWS]={0x170000,0x3d90000,0x3d7c000,0x3da0000,0x3d6c000,0x3d8c000};
static const uint64_t kHoujiExternalBytes[GECN_WINDOWS]={0x4000,0x10000,0x4000,0x40000,0x10000,0x4000};
static unsigned gHoujiGpuExternalCanaryOnce;
static struct {
 OSSharedPtr<IOMemoryMap> maps[GECN_WINDOWS];
 struct hph_record record;struct gec_result result;struct gec_memory memory;
 NativeAdrenoCpuState cpu;NativeAdrenoMemory bo[GECN_BOS];
 struct Bo {uint64_t va,pa,logical,owned,read_par[4],write_par[4],alias_read_par[4],alias_write_par[4];
  uint64_t initial_read_par[4],initial_write_par[4],initial_alias_read_par[4],initial_alias_write_par[4];
  unsigned allocated,rows,held,nc_attempted,nc,identity_ready,alias_rows,alias_wb,alias_nc;IOReturn status;} info[GECN_BOS];
 ArmSmmu4KTableAuditResult table_audit;unsigned char table_visited[4];
 size_t physical_reads,physical_refusals;unsigned memory_ready,held_mask,published_mask,memory_checks,cpu_access_allowed,cpu_finish_called,cpu_invalidated,cpu_restored;
 struct Fresh {
  struct gec_snapshot snapshot;
  unsigned sequence,mode,attempted,returned,valid,raw_code,code,recheck,reads,valid_reads,gate_valid,failed_address;
 } fresh[GECN_FRESH_SLOTS];
 struct EarlyCold {struct hph_cold cold;struct hph_clock clock;unsigned attempted,returned,code,recheck,reads,gates,failed_address;} early_cold[2];
 unsigned early_attempted,early_reason,early_saved,early_returned,early_in_fresh,early_current,early_owner_lost,memory_phase,memory_index;
 struct NoSubmission {struct gec_snapshot snapshot;unsigned attempted,returned,raw_code,code,reads,gates,failed_address,memory_valid,table_crc,fresh_sequence;} no_submission[4];
 unsigned no_submission_attempted[2],no_submission_saved[2],no_submission_returned[2],no_submission_post[2],no_submission_in_fresh,no_submission_current;
 uint32_t table_crc;unsigned table_crc_valid;
 unsigned wb_invalidated_mask,memory_profile,nc_published,nc_acquired,nc_restored,nc_publish_barriers,nc_acquire_barriers;
 unsigned enabled,active,decoded,record_ready,in_fresh,in_commit,fresh_count,current_fresh,fresh_ready,fresh_mode;
 unsigned phase,code,core_called,core_code,cxo_valid,cxo_raw,property_bytes;
 unsigned read_calls,tcm_reads,control_reads,tcm_writes,control_writes,reset_writes,read_barriers,write_barriers,barriers,delay_calls;
 unsigned map_attempted,map_valid,commit_attempted,commit_returned,last_commit_stage,pending_barrier;
 unsigned first_io_code,first_io_phase,first_io_address,first_io_expected,first_io_observed;
 uint64_t last_value;unsigned last_address,last_wide,event_claims,event_seen[GEC_MAX_EVENTS],release_seen,reset_seen;
} gHoujiGpuExternalCanary;
#ifdef HOUJI_GPU_EXTERNAL_CANARY_PROBE_HOST_TEST
static void houjiGpuExternalReadBarrier(void);
static void houjiGpuExternalWriteBarrier(void);
static void houjiGpuExternalFullBarrier(void);
static void houjiGpuExternalNcPublishBarrier(void);
static void houjiGpuExternalNcAcquireBarrier(void);
#else
static void houjiGpuExternalReadBarrier(void){__asm__ volatile("dmb oshld":::"memory");}
static void houjiGpuExternalWriteBarrier(void){__asm__ volatile("dsb sy":::"memory");}
static void houjiGpuExternalFullBarrier(void){__asm__ volatile("dsb sy":::"memory");}
static void houjiGpuExternalNcPublishBarrier(void){__asm__ volatile("dsb sy":::"memory");}
static void houjiGpuExternalNcAcquireBarrier(void){__asm__ volatile("dmb oshld\n\tdsb sy":::"memory");}
#endif
static unsigned houjiGpuExternalFail(unsigned code,uint32_t address,uint32_t expected=0,uint32_t observed=0) {
 auto&p=gHoujiGpuExternalCanary;
 if(!p.first_io_code){p.first_io_code=code;p.first_io_phase=p.result.phase;p.first_io_address=address;p.first_io_expected=expected;p.first_io_observed=observed;}
 return code;
}
/* Software single owner/configuration, not a hardware lease. Actual fresh
 * power/clock/reset/FAULT-all stream/bank observations separately authorize I/O. */
static int houjiGpuExternalExclusive(void*cookie) {
 auto&p=gHoujiGpuExternalCanary;
 const bool lost=cookie!=&gHoujiGpuExternalCanary || !p.active || !p.decoded || !p.record_ready || p.in_commit || p.result.qualification_lost ||
  !gRootEnabled || !gLogEnabled || !gReturnRecovery ||
  __atomic_load_n(&gHoujiGpuExternalCanaryOnce,__ATOMIC_ACQUIRE)!=1;
 if(lost)p.early_owner_lost=1;return lost?1:0;
}
static int houjiGpuExternalWindow(uint32_t address) {
 for(unsigned i=0;i<GECN_WINDOWS;i++)if(address>=kHoujiExternalBase[i] && address-kHoujiExternalBase[i]<=kHoujiExternalBytes[i]-4)return (int)i;
 return -1;
}
static volatile uint32_t *houjiGpuExternalPointer(uint32_t address,unsigned window,unsigned *status) {
 auto&p=gHoujiGpuExternalCanary;*status=0;
 if(window>=GECN_WINDOWS || (address&3) || address<kHoujiExternalBase[window] || address-kHoujiExternalBase[window]>kHoujiExternalBytes[window]-4) {
  *status=houjiGpuExternalFail(GECN_ADDRESS,address);return nullptr;
 }
 if(!p.maps[window]) {
  p.map_attempted|=1u<<window;
  IODeviceMemory *memory=IODeviceMemory::withRange(kHoujiExternalBase[window],kHoujiExternalBytes[window]);
  if(!memory){*status=houjiGpuExternalFail(GECN_ALLOC,address);return nullptr;}
  const unsigned options=kIOMapAnywhere|kIOMapInhibitCache|((window==2 || window==3 || window==4 || window==5)?0u:(unsigned)kIOMapReadOnly);
  p.maps[window]=OSSharedPtr<IOMemoryMap>(memory->map(options),OSNoRetain);memory->release();
  if(!p.maps[window]){*status=houjiGpuExternalFail(GECN_MAP,address);return nullptr;}
 }
 const uint64_t va=p.maps[window]->getVirtualAddress(),length=p.maps[window]->getLength(),offset=address-kHoujiExternalBase[window];
 if(!va || (va&3) || length<kHoujiExternalBytes[window] || va>UINT64_MAX-offset-4) {
  *status=houjiGpuExternalFail(GECN_MAPPING,address);return nullptr;
 }
 p.map_valid|=1u<<window;return reinterpret_cast<volatile uint32_t *>((uintptr_t)(va+offset));
}
static unsigned houjiGpuExternalRequired(uint32_t address) {
 for(unsigned i=0;i<8;i++)if(address==hgb_address(i))return (1u<<i)-1;
 return 0xff;
}
static void houjiGpuExternalGate(uint32_t address,uint32_t value) {
 auto&p=gHoujiGpuExternalCanary;auto&s=p.fresh[p.current_fresh];
 const uint32_t masks[]={0x80000003u,0x80000003u,0x80000001u,1,0x80000000u,0x80000000u,0xffffffffu,0x80000001u};
 const uint32_t want[]={1,1,1,0,0x80000000u,0,s.mode,1};
 for(unsigned i=0;i<8;i++)if(address==hgb_address(i)) {
  bool good=i==HGB_RESET && s.mode==2?value<=1:(value&masks[i])==want[i];
  if(good)s.gate_valid|=1u<<i;else s.gate_valid&=~(1u<<i);return;
 }
}
static bool houjiGpuExternalRawAllowed(uint32_t a) {
 if(hph_read_allowed(a) || a==0x3d99144u)return true;
 for(unsigned i=0;i<9;i++)if(a==gec_field_address(i) || ((i==5 || i==6) && a==gec_field_address(i)+4))return true;
 return a==HGP_CB(4) || a==0x3da6008u || a==0x3da600cu || a==HGP_CB(0x58) || a==HGP_CB(0x60) || a==HGP_CB(0x64) || a==HGP_CB(0x68);
}
static GPP_U64 houjiGpuExternalFreshRead(void*cookie,GPP_U32 address,GPP_U32*value) {
 auto&p=gHoujiGpuExternalCanary;
 if(!value || !p.in_fresh || !p.fresh_count || !houjiGpuExternalRawAllowed(address) || (address&3))return houjiGpuExternalFail(GECN_ADDRESS,address);
 if(houjiGpuExternalExclusive(cookie))return houjiGpuExternalFail(GECN_OWNER,address);
 auto&s=p.fresh[p.current_fresh];unsigned required=houjiGpuExternalRequired(address);
 if((s.gate_valid&required)!=required)return houjiGpuExternalFail(GECN_ORDER,address);
 int window=houjiGpuExternalWindow(address);if(window<0 || window==4 || window==5)return houjiGpuExternalFail(GECN_ADDRESS,address);
 unsigned status=0;auto*pointer=houjiGpuExternalPointer(address,(unsigned)window,&status);if(!pointer)return status;
 ++p.read_calls;++s.reads;p.last_address=address;p.last_wide=0;*value=*pointer;p.last_value=*value;
 houjiGpuExternalReadBarrier();++p.read_barriers;
 /* Initial context fields may contain all ones; clock/state sentinel policy
  * is enforced in the real observer, not generalized to raw original data. */
 ++s.valid_reads;houjiGpuExternalGate(address,*value);return 0;
}
static unsigned houjiGpuExternalNoSubmissionSave(unsigned kind,struct gec_snapshot *after,uint32_t *failed);
static unsigned houjiGpuExternalFresh(void*cookie,unsigned mode,struct gec_snapshot*snapshot,GPP_U32*failed) {
 auto&p=gHoujiGpuExternalCanary;auto&r=p.result;if(failed)*failed=0;
 if(!snapshot || !failed || mode>2 || p.in_fresh || p.pending_barrier || houjiGpuExternalExclusive(cookie))return HPH_OWNER;
 if(mode==0 && (!r.possible_execution || !p.release_seen || r.phase!=GEC_PHASE_POLL))return HPH_OWNER;
 if(mode==2 && (!r.possible_execution || !p.release_seen || (r.phase!=GEC_PHASE_RESET && r.phase!=GEC_PHASE_RESET_POLL)))return HPH_OWNER;
 p.fresh_ready=0;p.cxo_valid=0;if(p.fresh_count>=GECN_FRESH_LIMIT)return HPH_ARGUMENT;
 p.current_fresh=p.fresh_count%GECN_FRESH_SLOTS;++p.fresh_count;auto&s=p.fresh[p.current_fresh];hph_zero(&s,sizeof(s));
 s.sequence=p.fresh_count;s.mode=mode;s.attempted=1;p.in_fresh=1;
 const struct hph_ro_io io={&p,houjiGpuExternalFreshRead,houjiGpuExternalExclusive};
 s.raw_code=gec_observe_ro(&io,mode,&s.snapshot,&s.failed_address);s.code=s.raw_code;p.in_fresh=0;s.returned=1;
 if(!s.code && houjiGpuExternalExclusive(cookie))s.code=HPH_OWNER;
 if(!s.code && p.core_called){s.recheck=gec_state_qualified(&p.record,&p.memory,&r,&s.snapshot,mode)?0u:HPH_CHANGED;if(s.recheck)s.code=s.recheck;}
 if(!s.code && !p.core_called){
  s.recheck=hph_recheck(&p.record,&s.snapshot.cold,&s.snapshot.clock);
  if(s.recheck || s.snapshot.valid!=0x1fff || s.snapshot.context[0]!=s.snapshot.cold.sctlr[2] || (s.snapshot.context[0]&0x41u) || s.snapshot.actlr!=3 || s.snapshot.prr_lo || s.snapshot.prr_hi || (s.snapshot.fsr&0xc00001feu) || (s.snapshot.cxo&0x80000001u)!=1)s.code=HPH_COLD;
 }
 s.valid=s.code==0;p.fresh_ready=s.valid;p.fresh_mode=mode;p.cxo_raw=s.snapshot.cxo;p.cxo_valid=s.valid;
 /* This is the last real active-context guard before the core attempts CM3
  * release. Independent diagnostic proofs cannot recursively enter Fresh. */
 if(s.valid && mode==1 && r.phase==GEC_PHASE_ROUTE && r.enabled && r.route_prepared && r.stream_attached &&
    !r.possible_execution && !p.no_submission_attempted[0]) {
  s.code=houjiGpuExternalNoSubmissionSave(0,&s.snapshot,&s.failed_address);
  s.valid=s.code==0;p.fresh_ready=s.valid;p.cxo_raw=s.snapshot.cxo;p.cxo_valid=s.valid;
 }
 *snapshot=s.snapshot;*failed=s.failed_address;return s.code;
}
#ifdef HOUJI_GPU_EXTERNAL_CANARY_PROBE_HOST_TEST
static bool houjiGpuExternalAt(uint64_t,bool,uint64_t*);
#else
static bool houjiGpuExternalAt(uint64_t va,bool write,uint64_t*par) {
 if(!par)return false;uint64_t daif,saved;disable_preemption();const int before=cpu_number();
 __asm__ volatile("mrs %0,daif\n\tmsr daifset,#3\n\tmrs %1,par_el1":"=r"(daif),"=r"(saved)::"memory");
 if(write)__asm__ volatile("at s1e1w,%0\n\tisb sy"::"r"(va):"memory");else __asm__ volatile("at s1e1r,%0\n\tisb sy"::"r"(va):"memory");
 __asm__ volatile("mrs %0,par_el1\n\tmsr par_el1,%1\n\tmsr daif,%2":"=&r"(*par):"r"(saved),"r"(daif):"memory");
 const int after=cpu_number();enable_preemption();return before>=0 && before==after;
}
#endif
/* No phystokv, expected-table copy or foreign descriptor fallback. */
static int houjiGpuExternalTableRead(void*cookie,uint64_t pa,uint64_t*value) {
 auto&p=gHoujiGpuExternalCanary;auto&b=p.info[GECN_TABLE];const auto&bo=p.bo[GECN_TABLE];
 if(cookie!=&p || !value || !bo.backed() || bo.quarantined() || !b.allocated || !b.va || (pa&7) || pa<b.pa || pa-b.pa>GECN_CPU_BYTES-8){++p.physical_refusals;return 1;}
 *value=*reinterpret_cast<const volatile uint64_t*>((uintptr_t)b.va+(uintptr_t)(pa-b.pa));++p.physical_reads;return 0;
}
static bool houjiGpuExternalTables(void) {
 auto&p=gHoujiGpuExternalCanary;auto&m=p.memory;hph_zero(&p.table_audit,sizeof(p.table_audit));hph_zero(p.table_visited,sizeof(p.table_visited));
 const ArmSmmu4KTableIo io={&p,houjiGpuExternalTableRead};const ArmSmmu4KAuditRegion region={&m.owned.range,1,4,2048,p.table_visited,sizeof(p.table_visited)};
 if(ArmSmmu4KAuditTables(&io,&m.owned.config,&m.owned.built,&region,m.owned.table_physical,&p.table_audit) || !p.table_audit.complete || p.table_audit.table_pages!=3 || p.table_audit.leaf_pages!=1 || p.table_audit.descriptor_reads!=1536)return false;
 const auto*t=static_cast<const volatile uint64_t*>(m.owned.tables);for(unsigned i=1536;i<2048;i++)if(t[i])return false;
 /* Independent bounded byte-PA walk; adjacent/negative pages stay invalid. */
 const uint64_t iovas[]={0x60000000,0x60000fff,0x5fffffff,0x60001000,0x60002000};
 for(unsigned n=0;n<5;n++) {
  uint64_t root=m.owned.table_physical,leaf=0;bool mapped=false;
  for(unsigned level=1;level<=3;level++){
   uint64_t d=0;const unsigned shift=39-9*level;if(houjiGpuExternalTableRead(&p,root+8*((iovas[n]>>shift)&511),&d))return false;
   if(!d)break;if((d&3)!=3)return false;if(level==3){leaf=d;mapped=true;break;}
   if(d!=((d&UINT64_C(0x0000fffffffff000))|3))return false;root=d&UINT64_C(0x0000fffffffff000);
  }
  if(n<2){if(!mapped || ((leaf&UINT64_C(0x0000fffffffff000))|(iovas[n]&4095))!=m.owned.data_physical+(iovas[n]&4095))return false;}
  else if(mapped)return false;
 }
 return true;
}
/* NC uses only the descriptor mapping for data. The physical aperture is
 * observed by AT only: a WB alias is retained inactive, never dereferenced.
 * EffectiveShareability for Normal iNCoNC is OSH even when PAR.SH is zero. */
static bool houjiGpuExternalOwn(unsigned i,unsigned expected_refs) {
 auto&p=gHoujiGpuExternalCanary;if(i>=GECN_BOS)return false;auto&b=p.info[i];auto&bo=p.bo[i];const size_t logical=i==GECN_TABLE?16384u:4096u;
 const bool nc=p.memory_profile==GEC_MEMORY_NORMAL_NC && i!=GECN_TABLE && b.nc;
 if(!b.allocated || !bo.backed() || bo.quarantined() || bo.imported() || bo.readOnly() || bo.deviceReferences()!=expected_refs || bo.writeCombined()!=nc || bo.cpuPageBytes()!=16384 || bo.bytes()!=logical || b.logical!=bo.bytes() || bo.ownedBytes()!=16384 || b.owned!=bo.ownedBytes() || bo.spanCount()!=1 || bo.ownedSpanCount()!=1 || !bo.mapping() || !bo.descriptor())return false;
 const auto&s=bo.spans()[0];const auto&o=bo.ownedSpans()[0];const uint64_t va=(uint64_t)(uintptr_t)bo.mapping();
 if(va!=b.va || (va&16383) || va>UINT64_MAX-16384 || bo.descriptor()->getLength()!=16384 || s.offset || o.offset || s.bytes!=logical || o.bytes!=16384 || s.physical!=b.pa || o.physical!=b.pa || !b.pa || (b.pa&16383) || b.pa>=(UINT64_C(1)<<36)-16384)return false;
 if(p.memory_profile==GEC_MEMORY_NORMAL_NC) {
  if(!gPhysSize || gPhysBase>UINT64_MAX-gPhysSize || b.pa<gPhysBase || b.owned>gPhysSize || b.pa-gPhysBase>gPhysSize-b.owned)return false;
  for(unsigned j=0;j<GECN_BOS;j++)if(j!=i && p.info[j].allocated){const auto&a=p.info[j];if((b.pa<a.pa+a.owned && a.pa<b.pa+b.owned) || (b.va<a.va+a.owned && a.va<b.va+b.owned))return false;}
 }
 NativeAdrenoCpuView view{};if(!bo.cpuView(&view) || view.spans!=bo.ownedSpans() || view.count!=1 || view.bytes!=16384)return false;
 b.alias_rows=b.alias_wb=b.alias_nc=0;
 for(unsigned row=0;row<4;row++) {
  IOByteCount length=0;uint64_t off=(uint64_t)row*4096,physical=bo.descriptor()->getPhysicalSegment(off,&length,kIOMemoryMapperNone),r=0,w=0;
  if(physical!=b.pa+off || length<4096 || !houjiGpuExternalAt(va+off+0x5a,false,&r) || !houjiGpuExternalAt(va+off+0x5a,true,&w))return false;
  b.read_par[row]=r;b.write_par[row]=w;b.rows=row+1;const unsigned attr=nc?0x44u:0xffu;
  if((r&1) || (w&1) || (r>>56)!=attr || (w>>56)!=attr || ((r&UINT64_C(0x0000fffffffff000))|((va+off+0x5a)&4095))!=b.pa+off+0x5a || ((w&UINT64_C(0x0000fffffffff000))|((va+off+0x5a)&4095))!=b.pa+off+0x5a)return false;
  if(p.memory_profile==GEC_MEMORY_NORMAL_NC) {
   if(r!=w || ((r>>7)&3)==1)return false;
   const uint64_t alias=(uint64_t)phystokv(b.pa+off+0x5a);uint64_t ar=0,aw=0;
   if(!alias || !houjiGpuExternalAt(alias,false,&ar) || !houjiGpuExternalAt(alias,true,&aw))return false;
   b.alias_read_par[row]=ar;b.alias_write_par[row]=aw;b.alias_rows=row+1;const unsigned aa=(unsigned)(ar>>56);
   if((ar&1) || (aw&1) || ar!=aw || (aa!=0xff && aa!=0x44) || ((ar>>7)&3)==1 || ((ar&UINT64_C(0x0000fffffffff000))|(alias&4095))!=b.pa+off+0x5a || ((aw&UINT64_C(0x0000fffffffff000))|(alias&4095))!=b.pa+off+0x5a)return false;
   if(b.identity_ready && (r!=b.initial_read_par[row] || w!=b.initial_write_par[row] || ar!=b.initial_alias_read_par[row] || aw!=b.initial_alias_write_par[row]))return false;
   if(!b.identity_ready){b.initial_read_par[row]=r;b.initial_write_par[row]=w;b.initial_alias_read_par[row]=ar;b.initial_alias_write_par[row]=aw;}
   if(aa==0xff)++b.alias_wb;else ++b.alias_nc;
  }
 }
 return true;
}
static bool houjiGpuExternalMemoryIdentity(unsigned refs) {
 auto&p=gHoujiGpuExternalCanary;NativeAdrenoCpuState now{};
 if(NativeAdrenoQueryCpuPages(&now) || PAGE_SIZE!=16384 || now.kernel_page_bytes!=16384 || now.cpu_page_shift!=14 || (now.tcr&(UINT64_C(1)<<23)))return false;
 if(p.memory_profile>GEC_MEMORY_NORMAL_NC || p.memory.memory_profile!=p.memory_profile)return false;
 for(unsigned i=0;i<3;i++)if(p.info[i].nc!=(p.memory_profile==GEC_MEMORY_NORMAL_NC && i!=GECN_TABLE) || !houjiGpuExternalOwn(i,refs))return false;
 for(unsigned i=0;i<3;i++)for(unsigned j=0;j<i;j++)if((p.info[i].pa<p.info[j].pa+16384 && p.info[j].pa<p.info[i].pa+16384) || (p.info[i].va<p.info[j].va+16384 && p.info[j].va<p.info[i].va+16384))return false;
 const auto&m=p.memory;
 return m.owned.tables==p.bo[0].mapping() && m.owned.data==p.bo[1].mapping() && m.zero_cpu==p.bo[2].mapping() && m.owned.table_physical==p.info[0].pa && m.owned.data_physical==p.info[1].pa && m.zero_physical==p.info[2].pa && gec_memory_valid(&m);
}
static bool houjiGpuExternalIsolated(void) {
 const auto&p=gHoujiGpuExternalCanary;const auto&r=p.result;
 if(!p.fresh_ready || p.fresh_mode!=1 || !p.release_seen || !p.reset_seen || !r.possible_execution || !r.reset_held || !r.detached || !r.isolated || !r.context_retired || r.enabled || r.stream_attached || r.route_prepared || r.platform_prepared || !r.positive || !r.negative || r.code || r.qualification_lost || r.barrier_uncertain)return false;
 if(!gec_terminal_mailbox_valid(&r) || !gec_terminal_stack_valid(&r))return false;
 const auto&s=p.fresh[p.current_fresh].snapshot;
 return s.cold.values[HGB_RESET]==1 && !s.context[0] && !s.context[5] && !s.context[6] && s.actlr==3 && !s.prr_lo && !s.prr_hi && !(s.fsr&0xc00001feu) && gec_state_qualified(&p.record,&p.memory,&r,&s,1);
}
static GPP_U64 houjiGpuExternalMemoryCheck(void*cookie,unsigned stage,const struct gec_memory*m) {
 auto&p=gHoujiGpuExternalCanary;const auto&r=p.result;
 if(houjiGpuExternalExclusive(cookie) || m!=&p.memory || stage>2 || !p.memory_ready || p.pending_barrier || r.memory_check_calls<1 || r.memory_check_calls>4)return GECN_MEMORY;
 const auto&a=r.memory_access[r.memory_check_calls-1];if(a.value!=stage || !a.attempted || a.returned)return GECN_PHASE;
 if(stage==0 && (r.phase!=GEC_PHASE_INPUT || p.held_mask || p.release_seen))return GECN_PHASE;
 if(stage==1 && (r.phase!=GEC_PHASE_PREPARED_COMMIT || p.held_mask!=7 || p.release_seen || r.enabled || r.stream_attached))return GECN_PHASE;
 if(stage==2 && !houjiGpuExternalIsolated())return GECN_PHASE;
 ++p.memory_checks;if(!houjiGpuExternalMemoryIdentity(stage?1u:0u))return GECN_MEMORY;
 return houjiGpuExternalTables()?0:GECN_TABLES;
}
static GPP_U64 houjiGpuExternalPublish(void*cookie,const struct gec_memory*m) {
 auto&p=gHoujiGpuExternalCanary;const auto&r=p.result;
 if(houjiGpuExternalExclusive(cookie) || m!=&p.memory || !p.memory_ready || !p.fresh_ready || p.fresh_mode!=1 || p.pending_barrier || r.phase!=GEC_PHASE_BACKUP_COMMIT || r.event_count || p.held_mask || p.published_mask || !r.publication.attempted || r.publication.returned || !gec_backup_intact(&p.result) || !houjiGpuExternalMemoryIdentity(0))return GECN_PHASE;
 for(unsigned i=0;i<3;i++) {
  /* Tables stay WB; NC data/PRR zero have no cacheable descriptor access.
   * DSB completes descriptor stores before any master can be released. */
  if(p.memory_profile==GEC_MEMORY_NORMAL_NC && i!=GECN_TABLE){houjiGpuExternalNcPublishBarrier();++p.nc_publish_barriers;p.nc_published|=1u<<i;p.published_mask|=1u<<i;continue;}
  p.info[i].status=NativeAdrenoMaintainOwnedPages(p.bo[i].mapping(),p.bo[i].ownedBytes(),i==GECN_DATA?NativeAdrenoCleanInvalidate:NativeAdrenoClean);
  if(p.info[i].status)return GECN_CACHE;p.published_mask|=1u<<i;
 }
 for(unsigned i=0;i<3;i++){p.info[i].status=p.bo[i].holdForDevice();if(p.info[i].status)return GECN_MEMORY;p.info[i].held=1;p.held_mask|=1u<<i;}
 return houjiGpuExternalMemoryIdentity(1)?0:GECN_MEMORY;
}
static const struct gec_load *houjiGpuExternalExpectedRead(uint32_t address,unsigned wide) {
 const auto&r=gHoujiGpuExternalCanary.result;if(!r.load_count || r.load_count>GEC_MAX_LOADS)return nullptr;
 const auto&l=r.loads[r.load_count-1];return l.phase==r.phase && l.address==address && l.wide==wide && l.access.attempted==1 && !l.access.returned?&l:nullptr;
}
static GPP_U64 houjiGpuExternalReadCommon(void*cookie,GPP_U32 address,GPP_U64*value,unsigned wide) {
 auto&p=gHoujiGpuExternalCanary;
 if(!value || p.in_fresh || !p.fresh_ready || p.pending_barrier || houjiGpuExternalExclusive(cookie) || !gec_read_allowed(address,wide) || !houjiGpuExternalExpectedRead(address,wide))return houjiGpuExternalFail(GECN_PHASE,address);
 if(wide && (address&7))return houjiGpuExternalFail(GECN_ADDRESS,address);
 int window=houjiGpuExternalWindow(address);if(window<0)return houjiGpuExternalFail(GECN_ADDRESS,address);
 unsigned status=0;auto*pointer=houjiGpuExternalPointer(address,(unsigned)window,&status);if(!pointer)return status;
 if(wide && (address-kHoujiExternalBase[window]>kHoujiExternalBytes[window]-8 || ((uintptr_t)pointer&7)))return houjiGpuExternalFail(GECN_ADDRESS,address);
 p.last_address=address;p.last_wide=wide;++p.read_calls;
 *value=wide?*reinterpret_cast<const volatile uint64_t*>(pointer):*pointer;p.last_value=*value;houjiGpuExternalReadBarrier();++p.read_barriers;
 if(gec_tcm_index(address)>=0)++p.tcm_reads;else ++p.control_reads;return 0;
}
static GPP_U64 houjiGpuExternalRead32(void*cookie,GPP_U32 address,GPP_U32*value) {if(!value)return GECN_ADDRESS;GPP_U64 v=0;GPP_U64 status=houjiGpuExternalReadCommon(cookie,address,&v,0);*value=(GPP_U32)v;return status;}
static GPP_U64 houjiGpuExternalRead64(void*cookie,GPP_U32 address,GPP_U64*value) {return houjiGpuExternalReadCommon(cookie,address,value,1);}
static GPP_U64 houjiGpuExternalWriteCommon(void*cookie,GPP_U32 address,GPP_U64 value,unsigned wide) {
 auto&p=gHoujiGpuExternalCanary;auto&r=p.result;
 if(p.in_fresh || !p.fresh_ready || p.pending_barrier || p.held_mask!=7 || houjiGpuExternalExclusive(cookie) || !r.event_count || r.event_count>GEC_MAX_EVENTS || !gec_image_valid(&p.memory,&r) || !r.backup_committed || !gec_backup_intact(&r) || !gec_store_allowed(&p.memory,&r,address,value,wide))return houjiGpuExternalFail(GECN_PHASE,address);
 const unsigned index=r.event_count-1;const auto&e=r.events[index];
 if(index!=p.event_claims || p.event_seen[index] || e.phase!=r.phase || e.address!=address || e.wide!=wide || e.wanted!=value || e.store.value!=value || e.store.attempted!=1 || e.store.returned)return houjiGpuExternalFail(GECN_PHASE,address);
 if(wide && (address&7))return houjiGpuExternalFail(GECN_ADDRESS,address);
 p.event_seen[index]=1;++p.event_claims;
 int window=houjiGpuExternalWindow(address);if(window<0)return houjiGpuExternalFail(GECN_ADDRESS,address);
 unsigned status=0;auto*pointer=houjiGpuExternalPointer(address,(unsigned)window,&status);if(!pointer)return status;
 if(wide && (address-kHoujiExternalBase[window]>kHoujiExternalBytes[window]-8 || ((uintptr_t)pointer&7)))return houjiGpuExternalFail(GECN_ADDRESS,address);
 p.pending_barrier=1;p.last_address=address;p.last_value=value;p.last_wide=wide;
 if(gec_tcm_index(address)>=0)++p.tcm_writes;else if(address==0x3d7e000u){++p.reset_writes;if(value==0)p.release_seen=1;else p.reset_seen=1;}else ++p.control_writes;
 if(wide)*reinterpret_cast<volatile uint64_t*>(pointer)=value;else *pointer=(uint32_t)value;
 houjiGpuExternalWriteBarrier();++p.write_barriers;return 0;
}
static GPP_U64 houjiGpuExternalWrite32(void*cookie,GPP_U32 address,GPP_U32 value){return houjiGpuExternalWriteCommon(cookie,address,value,0);}
static GPP_U64 houjiGpuExternalWrite64(void*cookie,GPP_U32 address,GPP_U64 value){return houjiGpuExternalWriteCommon(cookie,address,value,1);}
static GPP_U64 houjiGpuExternalBarrier(void*cookie) {
 if(cookie!=&gHoujiGpuExternalCanary)return GECN_OWNER;
 houjiGpuExternalFullBarrier();++gHoujiGpuExternalCanary.barriers;gHoujiGpuExternalCanary.pending_barrier=0;return 0;
}
static GPP_U64 houjiGpuExternalDelay(void*cookie,unsigned us) {
 auto&p=gHoujiGpuExternalCanary;const auto&r=p.result;
 const bool phase=r.phase==GEC_PHASE_POLL || r.phase==GEC_PHASE_RESET_POLL || r.phase==GEC_PHASE_CONTEXT || r.phase==GEC_PHASE_PLATFORM || r.phase==GEC_PHASE_DETACH || r.phase==GEC_PHASE_PLATFORM_RETIRE || r.phase==GEC_PHASE_CONTEXT_RETIRE;
 if(us!=10 || !phase || p.in_fresh || p.pending_barrier || !p.fresh_ready || houjiGpuExternalExclusive(cookie) || p.delay_calls>=2*GEC_POLLS+5*100)return GECN_DELAY;
 ++p.delay_calls;IODelay(10);return 0;
}
static GPP_U64 houjiGpuExternalCpuFinish(void*cookie,const struct gec_memory*m,struct gec_cpu_result*out) {
 auto&p=gHoujiGpuExternalCanary;const auto&r=p.result;
 if(houjiGpuExternalExclusive(cookie) || m!=&p.memory || out!=&p.result.cpu || r.phase!=GEC_PHASE_CPU_FINISH || !r.cpu_finish_access.attempted || r.cpu_finish_access.returned || p.cpu_finish_called || p.pending_barrier || !houjiGpuExternalIsolated() || !houjiGpuExternalMemoryIdentity(1))return GECN_PHASE;
 p.cpu_finish_called=1;p.cpu_access_allowed=1;
 for(unsigned i=0;i<3;i++){
  if(p.memory_profile==GEC_MEMORY_NORMAL_NC && i!=GECN_TABLE){houjiGpuExternalNcAcquireBarrier();++p.nc_acquire_barriers;p.nc_acquired|=1u<<i;p.result.nc_acquired=p.nc_acquired;continue;}
  if(NativeAdrenoMaintainOwnedPages(p.bo[i].mapping(),p.bo[i].ownedBytes(),NativeAdrenoInvalidate))return GECN_CACHE;p.wb_invalidated_mask|=1u<<i;if(p.memory_profile==GEC_MEMORY_NORMAL_NC && i==GECN_TABLE)p.result.nc_table_invalidated=1;
 }
 /* The legacy invalidated bit is WB-only and stays zero for NC. Separate
  * core evidence records actual table maintenance, NC DMB/DSB acquisition
  * and the later restore DSB, never invalidation of a WB data copy. */
 if(p.memory_profile==GEC_MEMORY_NORMAL_NC && !houjiGpuExternalMemoryIdentity(1))return GECN_MEMORY;
 if(p.memory_profile==GEC_MEMORY_NORMAL_NC) {
  if(out->invalidated || p.cpu_invalidated || p.wb_invalidated_mask!=1 || p.nc_acquired!=6 || p.result.nc_table_invalidated!=1)return GECN_CACHE;
 }else {p.cpu_invalidated=out->invalidated=p.wb_invalidated_mask==7;if(!out->invalidated)return GECN_CACHE;}
 const auto*data=static_cast<const volatile uint32_t*>(m->owned.data);out->observed=data[2];
 for(unsigned i=0;i<1024;i++){uint32_t wanted=i==0?0x7504cafeu:i==1?0x86500071u:i==2?0xa750cafeu:0u;if(data[i]!=wanted)return GECN_VALUE;++out->checked_words;}
 const auto*pad=static_cast<const volatile unsigned char*>(m->owned.data);for(size_t i=4096;i<m->data_owned_bytes;i++)if(pad[i])return GECN_VALUE;
 const auto*zero=static_cast<const volatile unsigned char*>(m->zero_cpu);for(size_t i=0;i<m->zero_owned_bytes;i++)if(zero[i])return GECN_VALUE;
 if(!houjiGpuExternalTables())return GECN_TABLES;out->padding_checked=out->verified=1;
 static_cast<volatile uint32_t*>(m->owned.data)[2]=0;
 if(p.memory_profile==GEC_MEMORY_NORMAL_NC){houjiGpuExternalNcPublishBarrier();++p.nc_publish_barriers;p.nc_restored=1;p.result.nc_restore_published=1;}
 else if(NativeAdrenoMaintainOwnedPages(m->owned.data,m->data_owned_bytes,NativeAdrenoClean))return GECN_CACHE;
 if(p.memory_profile==GEC_MEMORY_NORMAL_NC && !houjiGpuExternalMemoryIdentity(1))return GECN_MEMORY;
 if(!gec_cpu_initial(m))return GECN_VALUE;p.cpu_restored=out->restored=1;return 0;
}
static void houjiGpuExternalReport(unsigned stage) {
 const auto&p=gHoujiGpuExternalCanary;const auto&r=p.result;
 IOLog("HOUJI GMU DMA MEMORY: profile=%u NC-publish=%x NC-acquire=%x NC-restore=%u WB-invalidated=%x DSB/DMB=%u/%u; table WB, descriptor-only data, known alias AT observation only; inactive WB alias never data-access permission\n",p.memory_profile,p.nc_published,p.nc_acquired,p.nc_restored,p.wb_invalidated_mask,p.nc_publish_barriers,p.nc_acquire_barriers);
 IOLog("HOUJI GMU DMA EARLY: attempted=%u reason=%u saved=%u returned=%u owner-lost=%u memory-phase=%u index=%u; diagnostic-only never changes core eligibility or safe-to-save\n",p.early_attempted,p.early_reason,p.early_saved,p.early_returned,p.early_owner_lost,p.memory_phase,p.memory_index);
 for(unsigned i=0;i<2;i++){const auto&e=p.early_cold[i];if(e.attempted)IOLog("HOUJI GMU DMA EARLY COLD: i=%u returned=%u code=%u recheck=%u reads=%u gates=%x failed=%x cold=%x reset=%x FW=%x HFI=%x GFSR=%x clock=%u/%x/%x; independent raw cold proof, extra snapshot unmodified\n",i,e.returned,e.code,e.recheck,e.reads,e.gates,e.failed_address,e.cold.valid,e.cold.values[HGB_RESET],e.cold.values[HGB_FWINIT],e.cold.hfi,e.cold.values[HGB_GFSR],e.clock.stable,e.clock.valid[0],e.clock.valid[1]);}
 IOLog("HOUJI GMU DMA RESULT: stage=%u phase=%u code=%u native=%u complete=%u safe-to-save=%u retained=%u lost=%u barrier-uncertain=%u exec=%u reset=%u ctx=%u/%u platform=%u route=%u/%u enable=%u isolated=%u positive=%u negative=%u TCM=%u/68 restore=%u/68 final=%u/68 control=%u/9 restore=%u/7 IRQ=%x events=%u loads=%u polls=%u/%u commits=%u/%u core=%u/%u/%u fresh=%u IO=%u/%u DSB=%u/%u delay10=%u maps=%x/%x cpu-access=%u cpu-finish=%u/%u/%u; BOs retained until platform reset, GPU/CP/HFI=0\n",
  stage,r.phase,r.code,p.code,r.complete,r.safe_to_save,r.retained,r.qualification_lost,r.barrier_uncertain,r.possible_execution,r.reset_held,r.context_prepared,r.context_retired,r.platform_prepared,r.route_prepared,r.stream_attached,r.enabled,r.isolated,r.positive,r.negative,r.backup_words,r.restored_words,r.final_verified_words,r.control_programmed,r.control_restored,r.irq_retained,r.event_count,r.load_count,r.polls,r.reset_polls,p.commit_attempted,p.commit_returned,r.backup_committed,r.prepared_committed,r.restore_committed,p.fresh_count,p.read_calls,p.tcm_writes+p.control_writes+p.reset_writes,p.write_barriers,p.barriers,p.delay_calls,p.map_attempted,p.map_valid,p.cpu_access_allowed,p.cpu_finish_called,p.cpu_invalidated,p.cpu_restored);
 IOLog("HOUJI GMU DMA POSTCOMMIT: native=%u/%u GEC=%u/%u/%u no-submission pre=%u/%u/%u/%u failure=%u/%u/%u/%u; returned flags are CPU observations after their checkpoint\n",p.commit_attempted,p.commit_returned,r.commit_access[0].returned,r.commit_access[1].returned,r.commit_access[2].returned,p.no_submission_attempted[0],p.no_submission_saved[0],p.no_submission_returned[0],p.no_submission_post[0],p.no_submission_attempted[1],p.no_submission_saved[1],p.no_submission_returned[1],p.no_submission_post[1]);
 if(r.event_count && r.event_count<=GEC_MAX_EVENTS){const auto&e=r.events[r.event_count-1];IOLog("HOUJI GMU DMA LAST EVENT: i=%u phase=%u address=%x wide=%u wanted=%llx mask=%llx store=%u/%u/%u/%llx barrier=%u/%u/%u/%llx verify=%u/%u/%u/%llx/%llx release-seen=%u reset-seen=%u\n",r.event_count-1,e.phase,e.address,e.wide,(unsigned long long)e.wanted,(unsigned long long)e.mask,e.store.attempted,e.store.returned,e.store.valid,(unsigned long long)e.store.status,e.barrier.attempted,e.barrier.returned,e.barrier.valid,(unsigned long long)e.barrier.status,e.verify.attempted,e.verify.returned,e.verify.valid,(unsigned long long)e.verify.status,(unsigned long long)e.verify.value,p.release_seen,p.reset_seen);}
 if(r.load_count && r.load_count<=GEC_MAX_LOADS){const auto&l=r.loads[r.load_count-1];IOLog("HOUJI GMU DMA LAST LOAD: i=%u phase=%u address=%x wide=%u attempted=%u returned=%u valid=%u status=%llx value=%llx\n",r.load_count-1,l.phase,l.address,l.wide,l.access.attempted,l.access.returned,l.access.valid,(unsigned long long)l.access.status,(unsigned long long)l.access.value);}
 for(unsigned i=0;i<4;i++){const auto&d=p.no_submission[i];if(d.attempted)IOLog("HOUJI GMU DMA NO SUBMISSION: i=%u returned=%u raw=%u code=%u reads=%u gates=%x failed=%x memory=%u tableCRC=%08x/%08x rawvalid=%x reset=%x SCTLR=%llx route=%x/%x TTBR=%llx/%llx ACTLR=%x PRR=%x/%x FSR=%x clock=%u/%x/%x\n",i,d.returned,d.raw_code,d.code,d.reads,d.gates,d.failed_address,d.memory_valid,d.table_crc,p.table_crc,d.snapshot.valid,d.snapshot.cold.values[HGB_RESET],(unsigned long long)d.snapshot.context[0],d.snapshot.cold.smr[0],d.snapshot.cold.s2cr[0],(unsigned long long)d.snapshot.context[5],(unsigned long long)d.snapshot.context[6],d.snapshot.actlr,d.snapshot.prr_hi,d.snapshot.prr_lo,d.snapshot.fsr,d.snapshot.clock.stable,d.snapshot.clock.valid[0],d.snapshot.clock.valid[1]);}
 IOLog("HOUJI GMU DMA FIRST: code=%u phase=%u op=%u index=%u address=%x expected=%llx observed=%llx status=%llx native=%u phase=%u address=%x last=%x/%llx/%u FSR=%x FAR=%llx FSYNR=%x stack=%x CPU-word=%x checked=%u padding=%u record=%08x backupCRC=%08x/%08x\n",
  r.first.code,r.first.phase,r.first.operation,r.first.index,r.first.address,(unsigned long long)r.first.expected,(unsigned long long)r.first.observed,(unsigned long long)r.first.status,p.first_io_code,p.first_io_phase,p.first_io_address,p.last_address,(unsigned long long)p.last_value,p.last_wide,r.fsr,(unsigned long long)r.far,r.fsynr,r.stack_changed_mask,r.cpu.observed,r.cpu.checked_words,r.cpu.padding_checked,p.record.crc32,r.backup_crc,r.control_crc);
 for(unsigned i=0;i<3;i++){const auto&b=p.info[i];if(p.memory_profile==GEC_MEMORY_NORMAL_NC)for(unsigned row=0;row<b.rows;row++)IOLog("HOUJI GMU DMA PAR: i=%u row=%u NC=%u descriptor=%llx/%llx alias=%llx/%llx WB-inactive=%u NC=%u locked=%u\n",i,row,b.nc,(unsigned long long)b.read_par[row],(unsigned long long)b.write_par[row],(unsigned long long)b.alias_read_par[row],(unsigned long long)b.alias_write_par[row],b.alias_wb,b.alias_nc,b.identity_ready);}
 for(unsigned i=0;i<3;i++){const auto&b=p.info[i];IOLog("HOUJI GMU DMA BO: i=%u alloc=%u status=%x VA=%llx PA=%llx logical=%llu owned=%llu rows=%u refs=%u quarantine=%u PAR0=%llx/%llx PAR3=%llx/%llx; never released\n",i,b.allocated,(unsigned)b.status,(unsigned long long)b.va,(unsigned long long)b.pa,(unsigned long long)b.logical,(unsigned long long)b.owned,b.rows,p.bo[i].deviceReferences(),p.bo[i].quarantined()?1u:0u,(unsigned long long)b.read_par[0],(unsigned long long)b.write_par[0],(unsigned long long)b.read_par[3],(unsigned long long)b.write_par[3]);}
 IOLog("HOUJI GMU DMA TABLE: root=%llx pages=%llu leaves=%llu actual-audit=%u/%llu physical-reads=%llu refusals=%llu held=%x published=%x memory-checks=%u; IOVA60000000 one4K, negative60002000 invalid\n",(unsigned long long)p.memory.owned.built.root_physical,(unsigned long long)p.memory.owned.built.table_pages,(unsigned long long)p.memory.owned.built.leaf_pages,p.table_audit.complete,(unsigned long long)p.table_audit.descriptor_reads,(unsigned long long)p.physical_reads,(unsigned long long)p.physical_refusals,p.held_mask,p.published_mask,p.memory_checks);
 unsigned show=stage?1u:4u,start=p.fresh_count>show?p.fresh_count-show:0;
 for(unsigned i=start;i<p.fresh_count;i++){const auto&s=p.fresh[i%GECN_FRESH_SLOTS];const auto&o=s.snapshot;IOLog("HOUJI GMU DMA FRESH: seq=%u mode=%u ret=%u valid=%u raw-code=%u code=%u recheck=%u failed=%x reads=%u gates=%x rawvalid=%x reset=%x SCTLR=%llx TTBR=%llx/%llx ACTLR=%x PRR=%x/%x FSR=%x FAR=%llx FSYNR=%x CXO=%x clock=%u/%x/%x\n",s.sequence,s.mode,s.returned,s.valid,s.raw_code,s.code,s.recheck,s.failed_address,s.reads,s.gate_valid,o.valid,o.cold.values[HGB_RESET],(unsigned long long)o.context[0],(unsigned long long)o.context[5],(unsigned long long)o.context[6],o.actlr,o.prr_hi,o.prr_lo,o.fsr,(unsigned long long)o.far,o.fsynr,o.cxo,o.clock.stable,o.clock.valid[0],o.clock.valid[1]);}
 IOLog("HOUJI GMU DMA MAIL: %08x/%08x/%08x/%08x/%08x/%08x/%08x/%08x; DSB-ordered external accesses plus full mailbox prove completion; reset alone never DMA proof\n",(unsigned)r.mailbox[0].value,(unsigned)r.mailbox[1].value,(unsigned)r.mailbox[2].value,(unsigned)r.mailbox[3].value,(unsigned)r.mailbox[4].value,(unsigned)r.mailbox[5].value,(unsigned)r.mailbox[6].value,(unsigned)r.mailbox[7].value);
 if(r.stack_after[0].attempted)for(unsigned i=0;i<16;i+=8){uint32_t w[8]={0};unsigned valid=0;for(unsigned j=0;j<8;j++){const auto&a=r.stack_after[i+j];w[j]=(uint32_t)a.value;valid|=(gec_access_ok(&a)?1u:0u)<<j;}IOLog("HOUJI GMU DMA STACK: i=%u %08x/%08x/%08x/%08x/%08x/%08x/%08x/%08x valid=%02x changed=%04x; exception R12/LR are captured data only, never control inputs\n",i,w[0],w[1],w[2],w[3],w[4],w[5],w[6],w[7],valid,r.stack_changed_mask);}
 if(stage==1 && r.backup_words==68)for(unsigned i=0;i<68;i+=8){uint32_t w[8]={0};unsigned valid=0;for(unsigned j=0;j<8 && i+j<68;j++){w[j]=(uint32_t)r.backup[i+j].value;valid|=(r.backup[i+j].valid?1u:0u)<<j;}IOLog("HOUJI GMU DMA ORIGINAL: i=%u %08x/%08x/%08x/%08x/%08x/%08x/%08x/%08x valid=%02x crc=%08x\n",i,w[0],w[1],w[2],w[3],w[4],w[5],w[6],w[7],valid,r.backup_crc);}
 if(stage==1 && r.control_backup_words==9)for(unsigned i=0;i<9;i+=8){uint32_t w[8]={0};unsigned valid=0;for(unsigned j=0;j<8 && i+j<9;j++){w[j]=(uint32_t)r.control_backup[i+j].value;valid|=(r.control_backup[i+j].valid?1u:0u)<<j;}IOLog("HOUJI GMU DMA CTRL ORIGINAL: i=%u %08x/%08x/%08x/%08x/%08x/%08x/%08x/%08x valid=%02x crc=%08x\n",i,w[0],w[1],w[2],w[3],w[4],w[5],w[6],w[7],valid,r.control_crc);}
 if(stage==1 && r.context_backup_words==9)for(unsigned i=0;i<9;i+=3)IOLog("HOUJI GMU DMA CONTEXT ORIGINAL: i=%u raw=%llx/%llx/%llx valid=%u/%u/%u; captured only, never arbitrary replay\n",i,(unsigned long long)r.context_backup[i].value,(unsigned long long)r.context_backup[i+1].value,(unsigned long long)r.context_backup[i+2].value,r.context_backup[i].valid,r.context_backup[i+1].valid,r.context_backup[i+2].valid);
}
static GPP_U64 houjiGpuExternalCommit(void*cookie,unsigned stage,const struct gec_result*r) {
 auto&p=gHoujiGpuExternalCanary;
 if(houjiGpuExternalExclusive(cookie) || r!=&p.result || stage<1 || stage>3 || stage!=p.commit_attempted+1 || !p.fresh_ready || p.fresh_mode!=1 || p.pending_barrier || p.held_mask!=7 || !gec_backup_intact(&p.result))return GECN_COMMIT;
 const auto&s=p.fresh[p.current_fresh].snapshot;
 if(s.cold.values[HGB_RESET]!=1 || s.cold.smr[0] || s.cold.s2cr[0]!=0x20000 || (stage==1?(s.context[0]&0x41u):s.context[0]) || !gec_state_qualified(&p.record,&p.memory,r,&s,1))return GECN_COMMIT;
 if(stage==1 && (r->phase!=GEC_PHASE_BACKUP_COMMIT || r->event_count || p.release_seen))return GECN_COMMIT;
 if(stage==2 && (r->phase!=GEC_PHASE_PREPARED_COMMIT || r->programmed_words!=52 || r->control_programmed!=9 || !r->platform_prepared || !r->context_prepared || r->possible_execution || r->enabled || r->stream_attached))return GECN_COMMIT;
 if(stage==3 && (r->phase!=GEC_PHASE_FINAL_COMMIT || !houjiGpuExternalIsolated() || r->restored_words!=68 || r->control_restored!=7 || r->final_verified_words!=68 || !r->cpu.verified || !r->cpu.restored || !r->cpu.padding_checked || !p.cpu_restored))return GECN_COMMIT;
 ++p.commit_attempted;houjiGpuExternalReport(stage);p.fresh_ready=0;p.in_commit=1;
 houji_ufs_checkpoint(stage==1?"external GMU originals68+controls9+context9 and owned BOs published; no MMIO store yet":stage==2?"external GMU code52+controls9 and disabled bank2 verified; no master released": "external GMU DONE and exact fault proven; stream/bank/TLB/fault retired; originals68+controls7 restored and CPU canary verified",0);
 p.in_commit=0;++p.commit_returned;return gRootEnabled && gLogEnabled && gReturnRecovery?0:GECN_FLAGS;
}
/* Only a terminal diagnostic failure before ANY GPU publication/store may
 * use this independent cold proof. ACTLR/PRR/FAR extras are deliberately not
 * prerequisites for this filesystem-only decision: all masters remain reset,
 * all24 routes FAULT and all22 banks M=0. Original extra failure stays intact. */
static bool houjiGpuExternalEarlyNoStores(void) {
 auto&p=gHoujiGpuExternalCanary;const auto&r=p.result;
 if(houjiGpuExternalExclusive(&p) || p.early_owner_lost || p.core_called || r.started || r.event_count || r.possible_execution || r.qualification_lost ||
  p.event_claims || p.tcm_writes || p.control_writes || p.reset_writes || p.write_barriers || p.release_seen || p.reset_seen || p.pending_barrier ||
  p.held_mask || p.published_mask || p.commit_attempted || p.in_fresh || p.in_commit || p.cpu_finish_called)return false;
 for(unsigned i=0;i<3;i++)if(p.bo[i].deviceReferences() || p.info[i].held)return false;return true;
}
static GPP_U64 houjiGpuExternalEarlyRead(void*cookie,GPP_U32 address,GPP_U32*value) {
 auto&p=gHoujiGpuExternalCanary;
 if(cookie!=&p || !value || !p.early_in_fresh || p.early_current>1 || !hph_read_allowed(address) || (address&3) || !houjiGpuExternalEarlyNoStores())return 1;
 auto&e=p.early_cold[p.early_current];const unsigned required=houjiGpuExternalRequired(address);
 if((e.gates&required)!=required){e.failed_address=address;return 1;}
 const int window=houjiGpuExternalWindow(address);if(window<0 || window==4 || window==5)return 1;
 unsigned status=0;auto*pointer=houjiGpuExternalPointer(address,(unsigned)window,&status);if(!pointer){e.failed_address=address;return status?status:1;}
 *value=*pointer;houjiGpuExternalReadBarrier();++e.reads;
 const uint32_t masks[]={0x80000003u,0x80000003u,0x80000001u,1,0x80000000u,0x80000000u,0xffffffffu,0x80000001u};
 const uint32_t want[]={1,1,1,0,0x80000000u,0,1,1};
 for(unsigned i=0;i<8;i++)if(address==hgb_address(i)){if((*value&masks[i])==want[i])e.gates|=1u<<i;else e.gates&=~(1u<<i);break;}
 return 0;
}
static int houjiGpuExternalEarlyOwner(void*cookie) {
 return cookie==&gHoujiGpuExternalCanary && houjiGpuExternalEarlyNoStores()?0:1;
}
static bool houjiGpuExternalEarlyCold(unsigned index) {
 auto&p=gHoujiGpuExternalCanary;if(index>1 || !houjiGpuExternalEarlyNoStores())return false;
 p.early_current=index;auto&e=p.early_cold[index];e.attempted=1;p.early_in_fresh=1;
 const struct hph_ro_io io={&p,houjiGpuExternalEarlyRead,houjiGpuExternalEarlyOwner};
 e.code=hph_observe_ro(&io,&e.cold,&e.clock,&e.failed_address);p.early_in_fresh=0;e.returned=1;
 e.recheck=hph_recheck(&p.record,&e.cold,&e.clock);
 return !e.code && !e.recheck && e.gates==0xff && houjiGpuExternalEarlyNoStores();
}
static void houjiGpuExternalEarlyFailureSave(unsigned reason) {
 auto&p=gHoujiGpuExternalCanary;
 if(p.early_attempted || (reason!=1 && reason!=2) || !houjiGpuExternalEarlyNoStores())return;
 p.early_attempted=1;p.early_reason=reason;p.fresh_ready=0;p.cxo_valid=0;
 if(!houjiGpuExternalEarlyCold(0))return;
 houjiGpuExternalReport(0);
 /* Normal thread, no adapter lock. The independent cold proof authorizes
  * only this failure report. No result eligibility/ready flag is granted. */
 p.in_commit=1;
 houji_ufs_checkpoint(reason==1?"external GMU early initialFresh refused; NO GPU stores/refs/publish and independent cold proof verified":
  "external GMU early PrepareMemory refused; NO GPU stores/refs/publish and independent cold proof verified",0);
 p.in_commit=0;p.early_returned=p.early_saved=1;
 (void)houjiGpuExternalEarlyCold(1);p.fresh_ready=0;p.cxo_valid=0;
 /* This function never continues the core, even when both proofs pass. */
}
/* Filesystem-only NoSubmission authorization. It never edits the core
 * flags or converts active hardware into an all-FAULT/M0 cold snapshot.
 * Only complete source-bound flag prefixes are accepted; partial/unknown
 * writes, barriers, ownership loss or ANY reset/release event forbid FS. */
static bool houjiGpuExternalNoSubmissionState(void) {
 auto&p=gHoujiGpuExternalCanary;const auto&r=p.result;
 if(houjiGpuExternalExclusive(&p) || p.early_owner_lost || !p.core_called || !r.started || r.safe_to_save || r.complete ||
  r.qualification_lost || r.barrier_uncertain || r.possible_execution || p.release_seen || p.reset_seen || p.reset_writes ||
  p.pending_barrier || p.in_fresh || p.early_in_fresh || p.in_commit || !p.memory_ready || p.held_mask!=7 || p.published_mask!=7 ||
  !p.table_crc_valid || !r.context_prepared || !r.platform_prepared || r.context_retired || !r.backup_committed ||
  !r.reset_held || r.programmed_words!=52 || r.control_programmed!=9 || p.commit_returned!=2 ||
  !gec_access_ok(&r.commit_access[1]) || !r.event_count || r.event_count>GEC_MAX_EVENTS ||
  p.event_claims!=r.event_count || p.write_barriers!=r.event_count || p.barriers!=r.event_count || r.writes!=r.event_count || r.barriers!=r.event_count)return false;
 if(r.phase!=GEC_PHASE_PREPARED_COMMIT && r.phase!=GEC_PHASE_ENABLE && r.phase!=GEC_PHASE_ROUTE)return false;
 if((r.route_prepared && !r.enabled) || (r.stream_attached && !r.route_prepared) ||
  (r.phase==GEC_PHASE_PREPARED_COMMIT && (r.enabled || r.route_prepared || r.stream_attached)) ||
  (r.phase!=GEC_PHASE_PREPARED_COMMIT && !r.prepared_committed))return false;
 for(unsigned i=0;i<r.event_count;i++){const auto&e=r.events[i];
  if(e.address==0x3d7e000u || !p.event_seen[i] || !gec_access_ok(&e.store) || !gec_access_ok(&e.barrier) ||
   (e.mask && !gec_access_ok(&e.verify)))return false;
 }
 for(unsigned i=0;i<3;i++)if(!p.info[i].held || p.bo[i].deviceReferences()!=1 || p.bo[i].quarantined())return false;
 return true;
}
static GPP_U64 houjiGpuExternalNoSubmissionRead(void*cookie,GPP_U32 address,GPP_U32*value) {
 auto&p=gHoujiGpuExternalCanary;
 if(cookie!=&p || !value || !p.no_submission_in_fresh || p.no_submission_current>=4 ||
  !houjiGpuExternalRawAllowed(address) || (address&3) || houjiGpuExternalExclusive(cookie))return 1;
 auto&d=p.no_submission[p.no_submission_current];const unsigned required=houjiGpuExternalRequired(address);
 if((d.gates&required)!=required){d.failed_address=address;return 1;}
 const int window=houjiGpuExternalWindow(address);if(window<0 || window==4 || window==5)return 1;
 unsigned status=0;auto*pointer=houjiGpuExternalPointer(address,(unsigned)window,&status);if(!pointer){d.failed_address=address;return status?status:1;}
 *value=*pointer;houjiGpuExternalReadBarrier();++d.reads;
 const uint32_t masks[]={0x80000003u,0x80000003u,0x80000001u,1,0x80000000u,0x80000000u,0xffffffffu,0x80000001u};
 const uint32_t want[]={1,1,1,0,0x80000000u,0,1,1};
 for(unsigned i=0;i<8;i++)if(address==hgb_address(i)){if((*value&masks[i])==want[i])d.gates|=1u<<i;else d.gates&=~(1u<<i);break;}
 return 0;
}
static bool houjiGpuExternalNoSubmissionProof(unsigned index) {
 auto&p=gHoujiGpuExternalCanary;if(index>=4 || p.no_submission_in_fresh || !houjiGpuExternalNoSubmissionState())return false;
 p.no_submission_current=index;auto&d=p.no_submission[index];d.attempted=1;d.fresh_sequence=p.fresh_count;p.no_submission_in_fresh=1;
 const struct hph_ro_io io={&p,houjiGpuExternalNoSubmissionRead,houjiGpuExternalExclusive};
 d.raw_code=gec_observe_ro(&io,1,&d.snapshot,&d.failed_address);p.no_submission_in_fresh=0;d.returned=1;d.code=d.raw_code;
 if(!d.code && (d.gates!=0xff || !gec_state_qualified(&p.record,&p.memory,&p.result,&d.snapshot,1)))d.code=HPH_CHANGED;
 if(!d.code && (!houjiGpuExternalNoSubmissionState() || !houjiGpuExternalMemoryIdentity(1) || !houjiGpuExternalTables()))d.code=GECN_MEMORY;
 if(!d.code){d.table_crc=gpp_crc32(p.bo[GECN_TABLE].mapping(),GECN_CPU_BYTES);if(d.table_crc!=p.table_crc)d.code=GECN_TABLES;}
 d.memory_valid=d.code==0;
 return !d.code && houjiGpuExternalNoSubmissionState();
}
static unsigned houjiGpuExternalNoSubmissionSave(unsigned kind,struct gec_snapshot *after,uint32_t *failed) {
 auto&p=gHoujiGpuExternalCanary;const auto&r=p.result;
 if(kind>1 || p.no_submission_attempted[kind] || !houjiGpuExternalNoSubmissionState())return GECN_ORDER;
 if(kind==0 && (r.phase!=GEC_PHASE_ROUTE || !r.enabled || !r.route_prepared || !r.stream_attached || !r.prepared_committed || r.code))return GECN_ORDER;
 if(kind==1 && r.safe_to_save)return GECN_ORDER;
 p.no_submission_attempted[kind]=1;p.fresh_ready=0;p.cxo_valid=0;
 const unsigned before=2*kind,post=before+1;
 if(!houjiGpuExternalNoSubmissionProof(before))return GECN_ORDER;
 houjiGpuExternalReport(kind?5u:4u);p.in_commit=1;
 houji_ufs_checkpoint(kind?"external GMU failure retained; independently verified NO SUBMISSION, exact owned context/clock/BO graph; no reset or release event":
  "external GMU pre-release NO SUBMISSION; actual enabled bank2/SID5 and immutable owned graph verified; CM3 remains reset1",0);
 p.in_commit=0;p.no_submission_saved[kind]=p.no_submission_returned[kind]=1;
 const bool good=houjiGpuExternalNoSubmissionProof(post);p.no_submission_post[kind]=good?1u:0u;
 p.fresh_ready=0;p.cxo_valid=0;
 if(!good)return GECN_ORDER;
 if(after)*after=p.no_submission[post].snapshot;if(failed)*failed=p.no_submission[post].failed_address;
 /* Only the normal Fresh caller may publish this newly observed snapshot.
  * Terminal failure reporting never resumes the core or changes result flags. */
 return 0;
}
static bool houjiGpuExternalPrepareMemory(void) {
 auto&p=gHoujiGpuExternalCanary;p.memory_phase=1;
 if(NativeAdrenoQueryCpuPages(&p.cpu) || PAGE_SIZE!=16384 || p.cpu.kernel_page_bytes!=16384 || p.cpu.cpu_page_shift!=14 || (p.cpu.tcr&(UINT64_C(1)<<23)))return false;
 for(unsigned i=0;i<3;i++) {
  p.memory_index=i;p.memory_phase=2;auto&b=p.info[i];auto&bo=p.bo[i];const size_t logical=i==GECN_TABLE?16384u:4096u;
  b.status=bo.allocate(logical,36,true,NativeAdrenoMemory::backingBudget(logical),false);if(b.status)return false;
  b.allocated=1;b.va=(uint64_t)(uintptr_t)bo.mapping();b.pa=bo.ownedSpanCount()?bo.ownedSpans()[0].physical:0;b.logical=bo.bytes();b.owned=bo.ownedBytes();
  p.memory_phase=3;if(!houjiGpuExternalOwn(i,0))return false;
  if(p.memory_profile==GEC_MEMORY_NORMAL_NC && i!=GECN_TABLE){b.nc_attempted=1;b.status=bo.makeWriteCombined();if(b.status)return false;b.nc=1;if(!houjiGpuExternalOwn(i,0))return false;}
  p.memory_phase=4;const auto*bytes=static_cast<const volatile unsigned char*>(bo.mapping());for(size_t j=0;j<bo.ownedBytes();j++)if(bytes[j])return false;
 }
 p.memory_phase=5;auto&m=p.memory;m.owned.tables=p.bo[0].mapping();m.owned.data=p.bo[1].mapping();m.owned.table_physical=p.info[0].pa;m.owned.data_physical=p.info[1].pa;
 m.zero_physical=p.info[2].pa;m.zero_cpu=p.bo[2].mapping();m.cpu_page_bytes=16384;m.memory_profile=p.memory_profile;m.table_owned_bytes=m.data_owned_bytes=m.zero_owned_bytes=16384;
 m.owned.config={32,36,m.owned.table_physical,1};m.owned.range={0x60000000,m.owned.data_physical,4096,ArmSmmu4KRead|ArmSmmu4KWrite,p.memory_profile==GEC_MEMORY_NORMAL_NC?0u:1u,3};
 if(ArmSmmu4KBuild(&m.owned.config,&m.owned.range,1,static_cast<uint64_t*>(m.owned.tables),4,&m.owned.built) || m.owned.built.table_pages!=3 || m.owned.built.leaf_pages!=1 || m.owned.built.root_physical!=m.owned.table_physical || m.owned.built.mair!=0xff44)return false;
 auto*data=static_cast<volatile uint32_t*>(m.owned.data);data[0]=0x7504cafeu;data[1]=0x86500071u;
 p.memory_phase=6;if(!houjiGpuExternalMemoryIdentity(0) || !houjiGpuExternalTables())return false;p.table_crc=gpp_crc32(p.bo[GECN_TABLE].mapping(),GECN_CPU_BYTES);p.table_crc_valid=1;for(auto&b:p.info)b.identity_ready=1;p.memory_phase=7;p.memory_ready=1;return true;
}
/* Failure-only screen capture window. Reads ONLY completed CPU records;
 * no MMIO/FS/cache operation, resource release or eligibility change here.
 * Caller still selects its existing no-FS panic/PSCI return after 8 seconds. */
static void houjiGpuExternalFailureHold(void) {
 const auto&p=gHoujiGpuExternalCanary;const auto&r=p.result;
 if(r.safe_to_save==1 || !r.started)return;
 const struct gec_snapshot *snapshot=nullptr;unsigned raw=0,code=0,sequence=0,source=4;
 if(p.fresh_count){const auto&f=p.fresh[(p.fresh_count-1)%GECN_FRESH_SLOTS];snapshot=&f.snapshot;raw=f.raw_code;code=f.code;sequence=f.sequence;}
 /* An independent pre/post-save proof may be newer than the normal Fresh
  * that invoked it. Preserve that actual partial raw snapshot on failure. */
 for(unsigned i=0;i<4;i++){const auto&d=p.no_submission[i];if(d.attempted && d.returned && d.fresh_sequence>=sequence){snapshot=&d.snapshot;raw=d.raw_code;code=d.code;sequence=d.fresh_sequence;source=i;}}
 const struct gec_event *event=r.event_count && r.event_count<=GEC_MAX_EVENTS?&r.events[r.event_count-1]:nullptr;
 IOLog("HOUJI GMU STOP1 P=%u C=%u N=%u/%u OP=%u A=%x S=%llx\n",r.phase,r.code,p.code,p.first_io_code,r.first.operation,r.first.address,(unsigned long long)r.first.status);
 IOLog("HOUJI GMU STOP2 SRC=%u RAW=%u C=%u R=%x SCT=%llx FSR=%x FAR=%llx\n",source,raw,code,snapshot?snapshot->cold.values[HGB_RESET]:0,(unsigned long long)(snapshot?snapshot->context[0]:0),snapshot?snapshot->fsr:0,(unsigned long long)(snapshot?snapshot->far:0));
 IOLog("HOUJI GMU STOP3 A=%x TRY/RET=%u/%u S=%llx REL/EX/RST=%u/%u/%u\n",event?event->address:0,event?event->store.attempted:0,event?event->store.returned:0,(unsigned long long)(event?event->store.status:0),p.release_seen,r.possible_execution,p.reset_seen);
 IOLog("HOUJI GMU STOP4 MAIL=%08x/%08x/%08x/%08x HOLD=8s PSCI next\n",(unsigned)r.mailbox[0].value,(unsigned)r.mailbox[1].value,(unsigned)r.mailbox[2].value,(unsigned)r.mailbox[3].value);
 IOSleep(8000);
}
static bool houjiGpuExternalCanaryProbeOnce(void) {
 int enabled=1;PE_parse_boot_argn("houji_gpu_external_canary_probe",&enabled,sizeof(enabled));if(enabled!=1)return true;unsigned expected=0;
 if(!__atomic_compare_exchange_n(&gHoujiGpuExternalCanaryOnce,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return gHoujiGpuExternalCanary.result.safe_to_save==1 && !gHoujiGpuExternalCanary.active;
 auto&p=gHoujiGpuExternalCanary;p.enabled=1;p.phase=1;
 int nc=1;PE_parse_boot_argn("houji_gpu_external_nc_probe",&nc,sizeof(nc));if(nc!=0 && nc!=1){p.code=GECN_FLAGS;houjiGpuExternalReport(0);return false;}p.memory_profile=(unsigned)nc;
 if(!gRootEnabled || !gLogEnabled || !gReturnRecovery){p.code=GECN_FLAGS;houjiGpuExternalReport(0);return false;}
 IORegistryEntry*chosen=IORegistryEntry::fromPath("/chosen",gIODTPlane);if(!chosen){p.code=GECN_PROPERTY;houjiGpuExternalReport(0);return false;}
 OSData*data=OSDynamicCast(OSData,chosen->getProperty("houji,gpu-power-handoff"));p.property_bytes=data?data->getLength():0;
 if(!data || p.property_bytes!=HPH_RECORD_BYTES || !data->getBytesNoCopy()){chosen->release();p.code=GECN_PROPERTY;houjiGpuExternalReport(0);return false;}
 p.decoded=hph_decode(data->getBytesNoCopy(),p.property_bytes,&p.record)?1u:0u;chosen->release();p.record_ready=p.decoded && p.record.complete==1 && p.record.code==0;
 if(!p.record_ready){p.code=GECN_RECORD;houjiGpuExternalReport(0);return false;}
 p.active=1;p.phase=2;struct gec_snapshot initial;uint32_t failed=0;
 IOLog("HOUJI GMU DMA ENTRY: initialFresh begins; core=0 GPU-stores=0 refs=0\n");
 if(houjiGpuExternalFresh(&p,1,&initial,&failed)){p.code=GECN_ORDER;houjiGpuExternalEarlyFailureSave(1);p.active=0;houjiGpuExternalReport(0);return false;}
 IOLog("HOUJI GMU DMA ENTRY: PrepareMemory begins; core=0 GPU-stores=0 refs=0\n");
 if(!houjiGpuExternalPrepareMemory()){p.code=GECN_MEMORY;houjiGpuExternalEarlyFailureSave(2);p.active=0;for(auto&bo:p.bo)if(bo.backed())bo.quarantine();houjiGpuExternalReport(0);return false;}
 p.core_called=1;p.phase=3;
 const struct gec_io io={&p,houjiGpuExternalRead32,houjiGpuExternalRead64,houjiGpuExternalWrite32,houjiGpuExternalWrite64,houjiGpuExternalBarrier,houjiGpuExternalDelay,houjiGpuExternalFresh,houjiGpuExternalCommit,houjiGpuExternalExclusive,houjiGpuExternalMemoryCheck,houjiGpuExternalPublish,houjiGpuExternalCpuFinish};
 p.core_code=gec_run(&io,&p.record,&initial.cold,&initial.clock,&p.memory,&p.result);
 if(p.result.safe_to_save!=1)(void)houjiGpuExternalNoSubmissionSave(1,nullptr,nullptr);p.active=0;p.phase=4;
 if(p.result.safe_to_save!=1)for(auto&bo:p.bo)if(bo.backed())bo.quarantine();houjiGpuExternalReport(0);
 if(p.result.safe_to_save!=1 && p.result.started)houjiGpuExternalFailureHold();return p.result.safe_to_save==1;
}
#endif
