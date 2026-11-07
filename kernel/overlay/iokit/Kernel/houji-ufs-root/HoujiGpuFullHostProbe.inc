/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * New independent runtime owner for the full original host graph. Always
 * terminal, no filesystem access after first possible CM3 execution. No CP,
 * PAS, ZAP or graphics permission is implied by reaching PRE_CP_BOUNDARY.
 */
#ifndef HOUJI_GPU_FULL_HOST_PROBE_INC
#define HOUJI_GPU_FULL_HOST_PROBE_INC
#include "houji_full_host_runtime.h"
#include "HoujiTableCrc32.h"
#include "HoujiFullHostNativeMemory.h"
#include "HoujiCpFirmwareImages.h"
#include "GpuGen7Table.h"
#include "HoujiNativeSmmuReadPolicy.h"
static FhgStorage gHoujiFullGraph;
static HoujiFullHostNativeMemory gHoujiFullMemory;
static unsigned gHoujiFullOnce;
static struct {
 FhrStorage storage;FhrSummary summary;FhgSummary graph_summary;FhgGraph graph[2];
 OSSharedPtr<IOMemoryMap> maps[10];HsnpBindings stock;uint64_t gpu_fields[9];
 unsigned active,lost,code,complete,bridge,core,stage,held,zero_held,possible,reset_attempted,reset_returned;
 unsigned in_cold,in_save,cold_gates,clock_gates,cold_count,publishes,checkpoints,reads,writes,at_calls,lease_calls;
 unsigned gen_cursor,gen_pending,gen_before_valid,record_published;uint32_t gen_before,gen_expected,power_values[H7C_PAIRS];
 uint32_t table_crc[2];unsigned table_crc_ready,first_address,first_op,first_width,first_attempted,first_returned;
 uint64_t first_value;unsigned last_address,last_op,last_width,last_attempted,last_returned;uint64_t last_value;
} gHoujiFull;
static int houjiFullFail(unsigned code,uint32_t address=0) {
 auto&p=gHoujiFull;if(!p.code){p.code=code;p.first_address=address;p.first_op=p.last_op;p.first_width=p.last_width;p.first_value=p.last_value;p.first_attempted=p.last_attempted;p.first_returned=p.last_returned;}p.lost=1;return 1;
}
static bool houjiFullConfig(void){return houjiStockConfiguration() && gHoujiFullMemory.prepared();}
static bool houjiFullOwner(void*cookie){const auto&p=gHoujiFull;return cookie==&p && p.active && !p.lost && !p.in_save && !p.complete && houjiFullConfig();}
static bool houjiFullBo(const NativeAdrenoMemory&bo,uint64_t va,uint64_t pa,uint64_t logical,uint64_t owned,bool nc,unsigned refs,const uint64_t*read,const uint64_t*alias) {
 auto&p=gHoujiFull;
 if(!bo.backed() || bo.imported() || bo.quarantined() || bo.readOnly() || bo.deviceReferences()!=refs || bo.writeCombined()!=nc ||
  bo.cpuPageBytes()!=16384 || bo.bytes()!=logical || bo.ownedBytes()!=owned || bo.mapping()!=(void*)(uintptr_t)va || !bo.descriptor() ||
  bo.descriptor()->getLength()!=owned || bo.ownedSpanCount()!=1 || bo.spanCount()!=1 || !owned || !logical || ((va|pa|owned)&16383) ||
  owned!=NativeAdrenoMemory::backingBudget(logical) || (owned>4*1024*1024 && !(logical==FHG_LIVE_GPU_TABLE_PAGES*4096u && owned==logical && va==(uintptr_t)gHoujiFullMemory.entry(14).backing.cpu && pa==gHoujiFullMemory.entry(14).backing.physical)) || va>UINT64_MAX-owned || pa>=(UINT64_C(1)<<36) || owned>(UINT64_C(1)<<36)-pa ||
  !gPhysSize || pa<gPhysBase || owned>gPhysSize || pa-gPhysBase>gPhysSize-owned)return false;
 const auto&o=bo.ownedSpans()[0];const auto&s=bo.spans()[0];NativeAdrenoCpuView view{};
 if(o.offset || s.offset || o.physical!=pa || s.physical!=pa || o.bytes!=owned || s.bytes!=logical || !bo.cpuView(&view) || view.spans!=bo.ownedSpans() || view.count!=1 || view.bytes!=owned)return false;
 for(unsigned row=0;row<owned/4096;row++){
  const uint64_t off=(uint64_t)row*4096,probe=va+off+0x5a,ap=(uint64_t)phystokv(pa+off+0x5a);uint64_t r=0,w=0,ar=0,aw=0;IOByteCount length=0;
  if(bo.descriptor()->getPhysicalSegment(off,&length,kIOMemoryMapperNone)!=pa+off || length<4096 || !ap ||
   !houjiFullHostAt(probe,false,&r) || !houjiFullHostAt(probe,true,&w) || !houjiFullHostAt(ap,false,&ar) || !houjiFullHostAt(ap,true,&aw))return false;
  p.at_calls+=4;
  if(r!=w || r!=read[row] || ar!=aw || ar!=alias[row] || (r&1) || (ar&1) || (r>>56)!=(nc?0x44u:0xffu) ||
   ((ar>>56)!=0xff && (ar>>56)!=0x44) || ((r>>7)&3)==1 || ((ar>>7)&3)==1 ||
   ((r&UINT64_C(0x0000fffffffff000))|(probe&4095))!=pa+off+0x5a || ((ar&UINT64_C(0x0000fffffffff000))|(ap&4095))!=pa+off+0x5a)return false;
 }return true;
}
static bool houjiGfxForeign(void);
static bool houjiFullLease(void) {
 auto&p=gHoujiFull;++p.lease_calls;if(!houjiFullConfig() || !houjiGfxForeign())return false;NativeAdrenoCpuState cpu{};
 if(NativeAdrenoQueryCpuPages(&cpu) || PAGE_SIZE!=16384 || cpu.kernel_page_bytes!=16384 || cpu.cpu_page_shift!=14)return false;
 const auto&e=gHoujiGpuExternalCanary;const auto&f=gHoujiFirmwareMemory;
 for(unsigned i=0;i<3;i++){const auto&b=e.info[i];if(!houjiFullBo(e.bo[i],b.va,b.pa,b.logical,b.owned,i!=0,1u+(i==2?p.zero_held:0),b.initial_read_par,b.initial_alias_read_par))return false;}
 for(unsigned i=0;i<9;i++){const auto&b=f.info[i];if(!houjiFullBo(f.bo[i],b.va,b.pa,b.logical,b.owned,i>=5 && i<=7,0,b.read_par,b.alias_read_par))return false;}
 uint64_t va[27],pa[27],bytes[27];
 for(unsigned i=0;i<27;i++){
  if(i<3){va[i]=e.info[i].va;pa[i]=e.info[i].pa;bytes[i]=e.info[i].owned;}
  else if(i<12){va[i]=f.info[i-3].va;pa[i]=f.info[i-3].pa;bytes[i]=f.info[i-3].owned;}
  else {unsigned j=i-12;const auto&b=gHoujiFullMemory.entry(j);va[i]=(uintptr_t)b.backing.cpu;pa[i]=b.backing.physical;bytes[i]=b.request.owned_bytes;
   if(!houjiFullBo(gHoujiFullMemory.bo(j),va[i],pa[i],b.request.logical_bytes,bytes[i],b.nc,(p.held>>j)&1u,b.read_par,b.alias_par))return false;}
  for(unsigned j=0;j<i;j++)if((pa[i]<pa[j]+bytes[j] && pa[j]<pa[i]+bytes[i]) || (va[i]<va[j]+bytes[j] && va[j]<va[i]+bytes[i]))return false;
 }return true;
}
static volatile uint8_t*houjiFullPointer(uint32_t a,unsigned width) {
 auto&p=gHoujiFull;const uint64_t bases[10]={0x170000,0x3d90000,0x3d7c000,0x3da0000,0x3d6c000,0x3d8c000,0x3d50000,0xb284000,0x3d0c000,0x3d00000};
 const uint64_t bytes[10]={0x4000,0x10000,0x4000,0x40000,0x10000,0x4000,0x4000,0x4000,0x4000,0x40000};
 if((width!=4 && width!=8) || (a&(width-1)))return nullptr;
 for(unsigned i=0;i<10;i++)if(a>=bases[i] && a-bases[i]<=bytes[i]-width){
  if(!p.maps[i])return nullptr;const uint64_t va=p.maps[i]->getVirtualAddress();if(!va || p.maps[i]->getLength()!=bytes[i])return nullptr;
  return reinterpret_cast<volatile uint8_t*>((uintptr_t)(va+a-bases[i]));
 }
 return nullptr;
}
static bool houjiFullMap(void) {
 auto&p=gHoujiFull;const uint64_t bases[10]={0x170000,0x3d90000,0x3d7c000,0x3da0000,0x3d6c000,0x3d8c000,0x3d50000,0xb284000,0x3d0c000,0x3d00000};
 const uint64_t bytes[10]={0x4000,0x10000,0x4000,0x40000,0x10000,0x4000,0x4000,0x4000,0x4000,0x40000};
 for(unsigned i=0;i<10;i++){
  IODeviceMemory*m=IODeviceMemory::withRange(bases[i],bytes[i]);if(!m)return false;
  p.maps[i]=OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere|kIOMapInhibitCache|(i<2?kIOMapReadOnly:0)),OSNoRetain);m->release();
  if(!p.maps[i] || (p.maps[i]->getVirtualAddress()&16383) || p.maps[i]->getLength()!=bytes[i])return false;
  for(uint64_t off=0;off<bytes[i];off+=4096){const uint64_t va=p.maps[i]->getVirtualAddress()+off+0x5a;uint64_t r=0,w=0;
   if(!houjiFullHostAt(va,false,&r) || (r&1) || ((r>>56)!=0 && (r>>56)!=4) || ((r&UINT64_C(0x0000fffffffff000))|(va&4095))!=bases[i]+off+0x5a)return false;
   if(i>=2 && (!houjiFullHostAt(va,true,&w) || w!=r))return false;
  }
 }return true;
}
static bool houjiCpReadAllowed(uint32_t);
static bool houjiFullReadAllowed(uint32_t a,unsigned w){
 if(w==4 && a>=0x3da0000 && a<0x3de0000 && houjiNativeSmmuRead32Offset(a-0x3da0000))return true;
 if(w==4 && houjiCpReadAllowed(a))return true;
 if(w==4 && gHoujiFull.in_cold && houjiSecureCxRawAllowed(a))return true;
 if(hsnp_allowed_read(a,w*8))return true;
 if(w==8)return a==0x3dc0020 || a==0x3dc0028;
 if(w!=4 || (a&3))return false;
 const uint32_t fields[]={0x3da0020,0x3da0024,0x3da0028,0x3da1800,0x3da1000,0x3dc0004,0x3dc0010,0x3dc0030,0x3dc0038,0x3dc003c,0x3dc0058,0x3dc07f4,0x3d7e340,0x3d7e658,0x3d8ec40,0x3d8ec30,0x3d00840,0x3d0084c,0x3d02020,0x3d020c0,0x3d020c4,0x3d02dc0,0x3d02dc4};
 for(auto x:fields)if(a==x)return true;
 for(unsigned i=0;i<H7C_OPS;i++)if(h7c_ops[i].kind!=2 && a==h7c_ops[i].address)return true;
 for(unsigned i=0;i<H7C_PAIRS;i++)if(a==h7c_power_regs[i].address)return true;return false;
}
static int houjiFullRaw32(uint32_t a,uint32_t*v) {if(!v || !houjiFullReadAllowed(a,4))return 1;auto*ptr=houjiFullPointer(a,4);if(!ptr)return 1;*v=*reinterpret_cast<const volatile uint32_t*>(ptr);houjiStockReadBarrier();return 0;}
static int houjiFullRaw64(uint32_t a,uint64_t*v) {if(!v || !houjiFullReadAllowed(a,8))return 1;auto*ptr=houjiFullPointer(a,8);if(!ptr)return 1;*v=*reinterpret_cast<const volatile uint64_t*>(ptr);houjiStockReadBarrier();return 0;}
static bool houjiFullPower(void){auto&p=gHoujiFull;for(unsigned i=0;i<7;i++){uint32_t v=0;if(houjiFullRaw32(hsnp_gate_address(i),&v) || (v&hsnp_gate_mask(i))!=hsnp_gate_value(i))return false;}p.clock_gates=127;return true;}
static unsigned houjiGfxZapAttempted(void);
static unsigned houjiGfxImageAttempted(void);
static void houjiFullRefresh(void){auto&p=gHoujiFull;FhrSummary s{};if(!fhr_summary(&p.storage,sizeof(p.storage),&s,sizeof(s))){s.zap_attempted=houjiGfxZapAttempted();s.graphics_attempted=houjiGfxImageAttempted();p.summary=s;}}
static bool houjiFullGpuLive(void){uint32_t a=0,b=0,c=0,d=0;return !houjiFullRaw32(0x3d7e340,&a) && !(a&3) && !houjiFullRaw32(0x3d8ec40,&b) && !b && !houjiFullRaw32(0x3d8ec30,&c) && !(c&(1u<<23)) && !houjiFullRaw32(0x3dc0058,&d) && !(d&0xc00001feu);}
static int houjiFullRead32(void*cookie,uint32_t a,uint32_t*v){
 auto&p=gHoujiFull;if(!houjiFullOwner(cookie) || !v || !houjiFullReadAllowed(a,4))return houjiFullFail(2,a);
 bool gate=false;for(unsigned i=0;i<7;i++)if(a==hsnp_gate_address(i)){gate=true;if(i==0)p.clock_gates=0;if((p.clock_gates&((1u<<i)-1))!=((1u<<i)-1))return houjiFullFail(3,a);}
 if(!gate && p.clock_gates!=127)return houjiFullFail(3,a);
 if(a>=0x3d00000 && a<0x3d40000 && a!=0x3d0f114){houjiFullRefresh();if(!p.summary.gx_ready || !p.summary.gx_acked || !p.summary.gx_clear_confirmed || !houjiFullGpuLive())return houjiFullFail(4,a);}
 p.last_address=a;p.last_op=1;p.last_width=4;p.last_attempted=1;p.last_returned=0;if(houjiFullRaw32(a,v))return houjiFullFail(5,a);p.last_returned=1;p.last_value=*v;++p.reads;
 if(gate)for(unsigned i=0;i<7;i++)if(a==hsnp_gate_address(i) && (*v&hsnp_gate_mask(i))==hsnp_gate_value(i))p.clock_gates|=1u<<i;
 if(p.stage==FHR_GEN7 && p.gen_cursor<H7C_OPS){const auto&o=h7c_ops[p.gen_cursor];if(a==o.address){
  if(o.kind==1 && !p.gen_pending){p.gen_before=*v;p.gen_before_valid=1;}
  if((o.kind==3 && (*v&o.verify)==o.value) || (p.gen_pending && ((*v^p.gen_expected)&o.verify)==0)){++p.gen_cursor;p.gen_pending=p.gen_before_valid=0;}
 }}
 if(p.stage==FHR_GEN7 && p.gen_cursor==H7C_OPS)for(unsigned i=0;i<H7C_PAIRS;i++)if(a==h7c_power_regs[i].address)p.power_values[i]=*v;return 0;
}
static int houjiFullRead64(void*cookie,uint32_t a,uint64_t*v){auto&p=gHoujiFull;if(!houjiFullOwner(cookie) || p.clock_gates!=127 || !houjiFullReadAllowed(a,8))return houjiFullFail(2,a);p.last_address=a;p.last_op=1;p.last_width=8;p.last_attempted=1;p.last_returned=0;if(houjiFullRaw64(a,v))return houjiFullFail(5,a);p.last_returned=1;p.last_value=*v;++p.reads;return 0;}
static bool houjiFullWriteAllowed(uint32_t a,unsigned width,uint64_t v){
 auto&p=gHoujiFull;
 if(hsnp_allowed_write(&p.stock,a,width*8,v))return !p.possible || (width==4 && ((a==0x3d7e000 && v==1) || (a==0x3d7e618 && v==1) || (a==0x3d7e650 && v==1)));
 if(p.stage==FHR_BANK0){const uint32_t a0[]={0x3dc0000,0x3da1800,0x3da1000,0x3dc0010,0x3dc0030,0x3dc0020,0x3dc0028,0x3dc0038,0x3dc003c};
  for(unsigned i=0;i<9;i++)if(a==a0[i] && width==(i==5 || i==6?8u:4u) && v==p.gpu_fields[i])return true;
  if(width==4 && ((a==0x3dc0000 && v==0x1027) || (a==0x3dc0004 && v==0x32b) || (a==0x3dc0618 && !v) || (a==0x3dc07f0 && v==0xffffffffu) || (a==0x3da0c04 && !v) || (a==0x3da0804 && v==0x80000000u) || (a==0x3da0c08 && !v) || (a==0x3da0808 && v==0x80000001u)))return true;
 }
 if(p.stage==FHR_GX && width==4 && ((a==0x3d7e650 && v==0x40000000u) || (a==0x3d7e644 && v==0x80000000u)))return true;
 if(p.stage==FHR_CODE && width==4){const uint32_t ca[]={0x3d020c0,0x3d020c4,0x3d02dc0,0x3d02dc4},cv[]={0x63001000,0,0x63021000,0};for(unsigned i=0;i<4;i++)if(a==ca[i] && v==cv[i])return true;}
 if(p.stage==FHR_GEN7 && width==4 && p.gen_cursor<H7C_OPS && !p.gen_pending){const auto&o=h7c_ops[p.gen_cursor];if(o.kind==3 || a!=o.address || (o.kind==1 && !p.gen_before_valid))return false;return v==(o.kind==1?(p.gen_before&~o.mask)|o.value:o.value);}
 return false;
}
static int houjiFullWrite(void*cookie,uint32_t a,uint64_t v,unsigned width){
 auto&p=gHoujiFull;if(!houjiFullOwner(cookie) || p.clock_gates!=127 || p.held!=FHG_ALL || !p.zero_held || !houjiFullWriteAllowed(a,width,v))return houjiFullFail(6,a);
 if((p.stage==FHR_CODE || p.stage==FHR_GEN7) && !houjiFullGpuLive())return houjiFullFail(4,a);
 if(a==0x3d7e000 && !v){if(p.possible || p.checkpoints!=2 || p.publishes!=2 || !houjiFullLease())return houjiFullFail(7,a);p.possible=1;IOLog("HOUJI FULL RELEASE: no later FS/cache/free; all15 BO and PRRzero retained\n");}
 p.last_address=a;p.last_op=2;p.last_width=width;p.last_value=v;p.last_attempted=p.last_returned=0;auto*ptr=houjiFullPointer(a,width);if(!ptr)return houjiFullFail(5,a);
 if(a==0x3d7e000 && v==1)p.reset_attempted=1;p.last_attempted=1;
 if(width==8)*reinterpret_cast<volatile uint64_t*>(ptr)=v;else *reinterpret_cast<volatile uint32_t*>(ptr)=(uint32_t)v;
 houjiStockWriteBarrier();p.last_returned=1;++p.writes;if(a==0x3d7e000 && v==1)p.reset_returned=1;
 if(p.stage==FHR_GEN7 && p.gen_cursor<H7C_OPS && a==h7c_ops[p.gen_cursor].address){const auto&o=h7c_ops[p.gen_cursor];p.gen_expected=(uint32_t)v;if(o.kind==2){++p.gen_cursor;p.gen_before_valid=0;}else p.gen_pending=1;}
 return 0;
}
static int houjiFullWrite32(void*c,uint32_t a,uint32_t v){return houjiFullWrite(c,a,v,4);}
static int houjiFullWrite64(void*c,uint32_t a,uint64_t v){return houjiFullWrite(c,a,v,8);}
static int houjiFullBarrier(void*c){if(!houjiFullOwner(c))return houjiFullFail(1);houjiStockFullBarrier();return 0;}
static int houjiFullDelay(void*c,unsigned us){if(!houjiFullOwner(c) || (us!=1 && us!=10))return houjiFullFail(1);IODelay(us);return houjiFullOwner(c)?0:1;}
static int houjiFullHash(void*c,const uint8_t*b,size_t n,uint8_t*out){if(!houjiFullOwner(c) || !b || !out || !n || (uintptr_t)b>UINTPTR_MAX-n)return 1;NativeAdrenoSha256::hash(b,n,out);return 0;}
static GPP_U64 houjiFullColdRead(void*c,GPP_U32 a,GPP_U32*v){auto&p=gHoujiFull;if(!p.in_cold || !houjiFullOwner(c) || !houjiSecureCxRawAllowed(a))return 1;unsigned required=255;for(unsigned i=0;i<8;i++)if(a==hgb_address(i))required=(1u<<i)-1;if((p.cold_gates&required)!=required || houjiFullRaw32(a,v))return 1;const uint32_t masks[]={0x80000003u,0x80000003u,0x80000001u,1u,0x80000000u,0x80000000u,0xffffffffu,0x80000001u},want[]={1,1,1,0,0x80000000u,0,1,1};for(unsigned i=0;i<8;i++)if(a==hgb_address(i) && (*v&masks[i])==want[i])p.cold_gates|=1u<<i;return 0;}
static int houjiFullColdOwner(void*c){return houjiFullOwner(c)?0:1;}
static bool houjiFullCold(void){auto&p=gHoujiFull;if(p.possible || p.in_cold || !houjiFullLease())return false;struct gec_snapshot snap{};unsigned failed=0;p.in_cold=1;p.cold_gates=0;const hph_ro_io io={&p,houjiFullColdRead,houjiFullColdOwner};unsigned code=gec_observe_ro(&io,1,&snap,&failed);p.in_cold=0;++p.cold_count;return !code && !failed && p.cold_gates==255 && !hph_recheck(&gHoujiGpuExternalCanary.record,&snap.cold,&snap.clock) && gec_state_qualified(&gHoujiGpuExternalCanary.record,&gHoujiGpuExternalCanary.memory,&gHoujiGpuExternalCanary.result,&snap,1);}
static int houjiFullClaim(void*c){auto&p=gHoujiFull;return houjiFullOwner(c) && !p.held && !p.zero_held && !p.possible && houjiFullLease() && houjiFullCold()?0:houjiFullFail(8);}
static int houjiFullPrerequisite(void*c){auto&p=gHoujiFull;if(!houjiFullOwner(c) || !houjiFullCold())return 0;if(!p.held){for(unsigned i=0;i<15;i++){if(const_cast<NativeAdrenoMemory&>(gHoujiFullMemory.bo(i)).holdForDevice())return houjiFullFail(8),0;p.held|=1u<<i;}if(gHoujiGpuExternalCanary.bo[2].holdForDevice())return houjiFullFail(8),0;p.zero_held=1;}if(p.held!=FHG_ALL || !p.zero_held || !houjiFullLease())return houjiFullFail(8),0;p.clock_gates=127;return 1;}
/* Set when the provider gate accepted a GPU-table change: the next check takes a new GPU-table baseline instead of comparing. */
static bool houjiFullGpuTableStale;
static bool houjiFullTables(void){auto&p=gHoujiFull;for(unsigned i=0;i<2;i++){const auto&e=gHoujiFullMemory.entry(i?14:11);uint32_t crc=houji_table_crc32(e.backing.cpu,e.request.owned_bytes);if(p.table_crc_ready && crc!=p.table_crc[i] && !(i && houjiFullGpuTableStale))return false;p.table_crc[i]=crc;}p.table_crc_ready=1;houjiFullGpuTableStale=false;return true;}
/* After a legitimate GPU-table change (provider gate, new VM generation): check the GMU table against its baseline and
 * mark the GPU-table baseline stale. Recomputing that baseline here verified nothing, it only re-read the whole table,
 * and app launches change the generation on most of their ~40 gates per frame. The next houjiFullTables() takes it. */
static bool houjiFullTablesRebaseGpu(void){auto&p=gHoujiFull;if(!p.table_crc_ready){houjiFullGpuTableStale=true;return houjiFullTables();}const auto&g=gHoujiFullMemory.entry(11);if(houji_table_crc32(g.backing.cpu,g.request.owned_bytes)!=p.table_crc[0])return false;houjiFullGpuTableStale=true;return true;}
static int houjiFullPublish(void*c){auto&p=gHoujiFull;if(!houjiFullOwner(c) || p.possible || p.publishes>=2 || p.held!=FHG_ALL || !houjiFullLease() || !houjiFullTables())return houjiFullFail(9);for(unsigned i=0;i<15;i++){const auto&e=gHoujiFullMemory.entry(i);if(!e.nc && NativeAdrenoMaintainOwnedPages(e.backing.cpu,e.request.owned_bytes,NativeAdrenoCleanInvalidate))return houjiFullFail(9);}houjiStockFullBarrier();++p.publishes;return houjiFullLease() && houjiFullTables()?0:houjiFullFail(9);}
static int houjiFullCheckpoint(void*c,unsigned stage){auto&p=gHoujiFull;if(!houjiFullOwner(c) || p.possible || stage!=p.checkpoints+1 || stage>2 || !houjiFullLease() || !houjiFullTables())return houjiFullFail(10);++p.checkpoints;if(stage==1){if(p.writes || !houjiFullCold())return houjiFullFail(8);p.in_save=1;houji_ufs_checkpoint("full host graph15BO CPU16K/highPA retained; original stock/host/bank0/GX/code/gen7 batch armed before first device write",0);p.in_save=0;if(!houjiFullCold())return houjiFullFail(8);}return 0;}
static int houjiFullStage(void*c,unsigned stage){auto&p=gHoujiFull;if(!houjiFullOwner(c) || stage<FHR_HOST || stage>FHR_PRE_CP_BOUNDARY || !p.possible || !houjiFullPower())return houjiFullFail(11);if(stage!=p.stage){if(stage!=p.stage+1 || !houjiFullLease() || !houjiFullTables())return houjiFullFail(11);p.stage=stage;IOLog("HOUJI FULL STAGE: %u; same15BO, no FS/retirement, CP not enabled\n",stage);}
 uint32_t v=0;if(houjiFullRaw32(0x3da0048,&v) || v || houjiFullRaw32(0x3dc2058,&v) || (v&0xc00001feu))return houjiFullFail(12);
 if(stage>=FHR_GX){uint64_t q=0;for(unsigned i=0;i<24;i++){const uint32_t smr=i==0?0x80000005u:i==1?0x80000000u:i==2?0x80000001u:0,route=i==0?2:i<3?0:0x20000u;if(houjiFullRaw32(0x3da0800+4*i,&v) || v!=smr || houjiFullRaw32(0x3da0c00+4*i,&v) || v!=route)return houjiFullFail(12);}
  if(houjiFullRaw32(0x3dc0000,&v) || (v&~0x1000u)!=0x27 || houjiFullRaw32(0x3dc0004,&v) || v!=0x32b || houjiFullRaw64(0x3dc0020,&q) || q!=p.gpu_fields[5] || houjiFullRaw64(0x3dc0028,&q) || q!=p.gpu_fields[6] || houjiFullRaw32(0x3dc0058,&v) || (v&0xc00001feu))return houjiFullFail(12);
 }
 if(stage>=FHR_CODE && !houjiFullGpuLive())return houjiFullFail(4);return 0;
}
static int houjiFullPublishRecord(void*c){auto&p=gHoujiFull;if(!houjiFullOwner(c) || p.stage!=FHR_GEN7 || p.gen_cursor!=H7C_OPS || p.gen_pending || p.record_published || !houjiFullLease() || houjiFullStage(c,FHR_GEN7))return houjiFullFail(13);
 const auto&e=gHoujiFullMemory.entry(13);auto*words=(const volatile uint32_t*)((uintptr_t)e.backing.cpu+0x3f000);for(unsigned i=0;i<1024;i++){uint32_t expected=0;if(i==3)expected=H7C_IFPC|(H7C_PREEMPT<<8);else if(i>=4 && i<154)expected=(i&1)?p.power_values[(i-4)/2]:(h7c_power_regs[(i-4)/2].address-0x3d00000)/4;if(words[i]!=expected)return houjiFullFail(13);}
 houjiStockFullBarrier();p.record_published=1;return 0;
}
static int houjiFullStopped(void*c){auto&p=gHoujiFull;if(!houjiFullOwner(c) || !p.possible || !p.reset_attempted || !p.reset_returned || !houjiFullPower() || !houjiFullLease())return houjiFullFail(14);uint32_t v=0;return houjiFullRaw32(0x3d7e000,&v) || v!=1?houjiFullFail(14):0;}
static void houjiFullReport(void){auto&p=gHoujiFull;houjiFullRefresh();const auto&s=p.summary;
 IOLog("HOUJI FULL STOP1 stage=%u completed=%u code=%u native=%u bridge=%u stock=%u FW=%u HFI=%u host=%u/%u qualified=%u refs=%04x reset=%u/%u\n",s.stage,s.completed_stage,s.code,p.code,p.bridge,s.stock_code,s.fw_ready,s.hfi_ready,s.host_completed,s.host_code,s.host_qualified,p.held,p.reset_attempted,p.reset_returned);
 IOLog("HOUJI FULL STOP2 BANK0=%u/%u attached=%u SID0=%u SID1=%u GX=%u/%u ready=%u ACK=%u clear=%u CODE=%u/%u verified=%x\n",s.bank0_code,s.bank0_phase,s.bank0_attached,s.sid0_attached,s.sid1_attached,s.gx_code,s.gx_phase,s.gx_ready,s.gx_acked,s.gx_clear_confirmed,s.code_bind_code,s.code_bind_phase,s.code_bind_verified);
 IOLog("HOUJI FULL STOP3 GEN7=%u/%u step=%u completed=%u/102 record=%u pairs=%u native-cursor=%u record-pub=%u PRECP=%u CP=%u ZAP=%u 2D=%u\n",s.gen7_code,s.gen7_phase,s.gen7_step,s.gen7_completed,s.gen7_record_ready,s.gen7_pairs,p.gen_cursor,p.record_published,s.pre_cp_boundary,s.cp_attempted,s.zap_attempted,s.graphics_attempted);
 IOLog("HOUJI FULL STOP4 native=%u OP=%u A=%08x W=%u V=%llx attempted=%u returned=%u C=%u/%u A=%08x HGF=%u/%u/%08x FSR=%x FAR=%llx RX=%u/%u dropped=%u IO=%u/%u lease=%u AT=%u; RAM-only\n",p.code,p.first_op,p.first_address,p.first_width,(unsigned long long)p.first_value,p.first_attempted,p.first_returned,s.first_callback.kind,s.first_callback.stage,s.first_callback.address,s.failed_phase,s.failed_operation,s.failed_address,s.fsr,(unsigned long long)s.far,s.rx_records,s.rx_calls,s.rx_overflow,p.reads,p.writes,p.lease_calls,p.at_calls);
}

#include "HoujiGpuNativeCp.inc"
static void houjiGpuFullHostProbeOnce(void){int mode=0;PE_parse_boot_argn("houji_gpu_stock_probe",&mode,sizeof(mode));if(mode!=8)return;unsigned expected=0;if(!__atomic_compare_exchange_n(&gHoujiFullOnce,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))panic("HOUJI FULL: repeated terminal attempt");auto&p=gHoujiFull;
 int liveMode=4;
 if(!houjiStockConfiguration()){p.code=15;goto terminal;}
 PE_parse_boot_argn("houji_gpu_runtime",&liveMode,sizeof(liveMode));gHoujiLive.mode=liveMode;gHoujiLive.requested=liveMode>=1 && liveMode<=5;
 if(houjiGfxImport()){p.code=20;IOLog("HOUJI NATIVE GFX IMPORT REFUSED: stage=%u which=%u page=%u imported=%u; no new GPU execution or target store\n",gHoujiGfx.import_stage,gHoujiGfx.import_which,gHoujiGfx.import_page,gHoujiGfx.imported);if(!p.possible && !p.writes && !gHoujiCp.once && !gHoujiGfx.target_started && houjiStockConfiguration())houji_ufs_checkpoint("signed graphics import refused before any full-host GPU write; prior secureCX cold/report completed",0);goto terminal;}
 {HoujiFullHostNativeMemory::Span foreign[12];for(unsigned i=0;i<12;i++){if(i<3){const auto&b=gHoujiGpuExternalCanary.info[i];foreign[i]={b.va,b.pa,b.owned};}else{const auto&b=gHoujiFirmwareMemory.info[i-3];foreign[i]={b.va,b.pa,b.owned};}}
  if(!gHoujiFullMemory.begin(foreign,12)){p.code=16;goto terminal;}const FhgConfig config={FHG_ABI_VERSION,16384,{0,0}};const auto ops=gHoujiFullMemory.callbacks();uint32_t code=UINT32_MAX;
  if(fhg_init(&gHoujiFullGraph,sizeof(gHoujiFullGraph),&config,sizeof(config),&ops,sizeof(ops)) || fhg_prepare(&gHoujiFullGraph,sizeof(gHoujiFullGraph),gHoujiGmuFirmwareImage,sizeof(gHoujiGmuFirmwareImage),gHoujiSqeFirmwareImage,sizeof(gHoujiSqeFirmwareImage),gHoujiAqeFirmwareImage,sizeof(gHoujiAqeFirmwareImage),&code) || code || !gHoujiFullMemory.sealPrepared(gHoujiFullGraph)){p.code=17;gHoujiFullMemory.retainFailure();goto terminal;}}
 if(fhg_summary(&gHoujiFullGraph,sizeof(gHoujiFullGraph),&p.graph_summary,sizeof(p.graph_summary)) || fhg_graph(&gHoujiFullGraph,sizeof(gHoujiFullGraph),0,&p.graph[0],sizeof(FhgGraph)) || fhg_graph(&gHoujiFullGraph,sizeof(gHoujiFullGraph),1,&p.graph[1],sizeof(FhgGraph))){p.code=18;goto terminal;}
 IOLog("HOUJI GPU PERF: fixed request=500000000Hz HFI-index=5 ARC=0x00500305 BW-index=5; full 12-level factory houji profile, no measured-clock or governor claim\n");
 IOLog("HOUJI FULL MEMORY: 15BO prepared actual CPU16K highPA GMU=%llu/%llu GPU=%llu/%llu; about to run original lower pipeline\n",p.graph[0].table_pages,p.graph[0].leaf_pages,p.graph[1].table_pages,p.graph[1].leaf_pages);
 for(unsigned i=0;i<15;i++){const auto&e=gHoujiFullMemory.entry(i);IOLog("HOUJI FULL BO: i=%u PA=%llx logical=%llu owned=%llu NC=%u refs=%u\n",i,e.backing.physical,e.request.logical_bytes,e.request.owned_bytes,e.nc,gHoujiFullMemory.bo(i).deviceReferences());}
 p.active=1;p.stage=FHR_STOCK;
 {const uint64_t common[]={0,1,0x1f300,0x38011,0x80802020,0,0,0xff44,0};for(unsigned i=0;i<9;i++){p.stock.encoded[i]=common[i];p.gpu_fields[i]=common[i];}p.stock.encoded[5]=(UINT64_C(0x750)<<48)|p.graph[0].root_physical;p.stock.encoded[6]=UINT64_C(0x750)<<48;p.gpu_fields[5]=(UINT64_C(0x751)<<48)|p.graph[1].root_physical;p.gpu_fields[6]=UINT64_C(0x751)<<48;p.stock.zero_physical=gHoujiGpuExternalCanary.info[2].pa;}
 if(!houjiFullLease() || !houjiFullTables() || !houjiFullMap()){houjiFullFail(19);goto terminal;}
 {const FhrOps ops={FHR_ABI_VERSION,0,&p,houjiFullHash,houjiFullClaim,houjiFullRead32,houjiFullRead64,houjiFullWrite32,houjiFullWrite64,houjiFullBarrier,houjiFullDelay,houjiFullPrerequisite,houjiFullPublish,houjiFullCheckpoint,houjiFullStopped,houjiFullStage,houjiFullPublishRecord,houjiCpAfterPrepare,houjiFullKeepLive};p.bridge=fhr_init(&p.storage,sizeof(p.storage),&ops,sizeof(ops));if(!p.bridge)p.bridge=fhr_run(&p.storage,sizeof(p.storage),&gHoujiFullGraph,sizeof(gHoujiFullGraph),p.stock.zero_physical,&p.core);houjiFullRefresh();p.complete=!p.lost && !p.bridge && !p.code && !p.core && !p.summary.code && p.summary.pre_cp_boundary && p.summary.gen7_record_ready && p.record_published;}
 if(p.complete && p.summary.retained_live && gHoujiLive.retained){if(houjiLiveStart()){IOLog("HOUJI LIVE HANDOFF: retained owner/root/ring; provider=1 mode=%u; worker controls recovery\n",gHoujiLive.mode);
   if(gHoujiLive.mode==1){/* Mode 1 holds forever, but IOFindBSDRoot calls this path on the boot
    * thread; sleeping inline would block root mount and launchd forever. Hold on a fresh thread
    * and return so userspace starts; the hold thread keeps the GPU owner alive indefinitely. */
    thread_t hold;if(kernel_thread_start([](void*,wait_result_t){for(;;)IOSleep(1000);},nullptr,&hold)!=KERN_SUCCESS)panic("HOUJI LIVE: mode 1 hold thread failed");thread_deallocate(hold);}
   return;}if(!houjiCpStop(nullptr,gHoujiSubmit.end)){gHoujiCp.active=0;p.active=0;gHoujiSubmit.active=0;gHoujiApi.active=gHoujiApi.in_op=0;houjiCpSaveFinal();}gHoujiLive.deny=1;panic("HOUJI LIVE: provider/owner start refused; all BO retained");}
 terminal:p.active=0;houjiFullReport();houjiCpSaveFinal();if(!gHoujiCp.save_returned)IOSleep(8000);panic("HOUJI FULL: terminal complete=%u stage=%u; retained noFS PSCI recovery",p.complete,p.stage);
}
#endif
