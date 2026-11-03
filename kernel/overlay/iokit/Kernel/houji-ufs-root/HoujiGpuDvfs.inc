/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Same governor shape as sheng, own full factory A750 host-table levels; hardware qualification pending.
 * Runs under NativeAdreno's existing issuer lock, including HFI ACK handling.
 * Gen7 initialization ALREADY selects XO counter0, countable5 and enables it.
 */
#include "HoujiGpuGovernorPolicy.h"
extern "C" uint64_t houji_dvfs_touch_time(void);
extern "C" {extern volatile uint32_t houji_thermal_level;}
/* GPU OPP cap by TSENS level: 903, 680, 578, 500, 366, 231 MHz. houji_gpu_maxlevel (default 12 = 903 MHz) bounds it;
 * 8 keeps 680 MHz, 5 restores the old fixed 500 MHz cap. */
static unsigned houjiGpuCap(void){
 static unsigned maximum=0;
 if(!maximum){unsigned v=12;PE_parse_boot_argn("houji_gpu_maxlevel",&v,sizeof(v));maximum=v<1?1:v>HCO_COUNT?HCO_COUNT:v;}
 static const unsigned byLevel[]={12,8,6,5,3,1};
 unsigned level=__atomic_load_n(&houji_thermal_level,__ATOMIC_RELAXED);
 unsigned cap=byLevel[level<6?level:5];
 return cap<maximum?cap:maximum;
}
static struct {
 unsigned enabled,ready,bad,level=HCO_BOOT_LEVEL,low,changes,failed,busyPermille;
 uint64_t last,lastBusy,lastTouch,idleAt,wakeUntil,lastSubmits,windowNs,busyNs,at[HCO_COUNT+1],invalid;
 unsigned wakeBoosts;bool idle;HoujiGpuGovernorPolicy policy;
 uint64_t historyAt,historyNs,historyBusy,history231,history500,maxTickUs;
} gHoujiDvfs;
struct HoujiGpuDvfsWindow {uint64_t upUs,spanUs,busyUs,at231Us,at500Us,maxTickUs;unsigned changes,wakes;};
static HoujiGpuDvfsWindow houjiGpuDvfsHistory[48];
static unsigned houjiGpuDvfsHistoryCount;
static bool houjiDvfsRead(uint32_t a,uint32_t *v){
 if(!native_adreno_board_running() || !v)return false;
 auto*p=houjiFullPointer(a,4);if(!p)return false;
 *v=*(const volatile uint32_t*)p;houjiStockReadBarrier();return true;
}
static bool houjiDvfsBusy(uint64_t *out){
 uint32_t h=0,l=0,h2=0;
 if(!houjiDvfsRead(0x3d7e114,&h) || !houjiDvfsRead(0x3d7e110,&l) || !houjiDvfsRead(0x3d7e114,&h2))return false;
 if(h2!=h && !houjiDvfsRead(0x3d7e110,&l))return false;
 *out=(uint64_t(h2)<<32)|l;return true;
}
static int houjiDvfsOwner(void*){return native_adreno_board_running() && houjiCpBusLease() && !gHoujiCp.stopping && !gHoujiCp.isolated;}
static int houjiDvfsBarrier(void*){if(!houjiDvfsOwner(nullptr))return 1;houjiStockFullBarrier();return 0;}
static int houjiDvfsDelay(void*,unsigned us){if(us!=10 || !houjiDvfsOwner(nullptr))return 1;IODelay(us);return 0;}
static int houjiDvfsDoorbell(void*){if(!houjiDvfsOwner(nullptr))return 1;auto*p=houjiFullPointer(0x3d7e650,4);if(!p)return 1;*(volatile uint32_t*)p=1;houjiStockFullBarrier();return 0;}
extern "C" void houji_gpu_dvfs_tick(void){
 auto&v=gHoujiDvfs;if(!v.enabled || v.bad || !native_adreno_board_running())return;
 uint64_t now=houjiSubmitNow(nullptr),busy=0;
 if(!houjiDvfsBusy(&busy)){v.bad=1;return;}
 if(!v.ready){
  uint32_t enable=0,select=0;
  if(!houjiDvfsRead(0x3d7e100,&enable) || !houjiDvfsRead(0x3d7e104,&select) || enable!=1 || (select&255)!=32){v.bad=1;IOLog("HOUJI GPU DVFS: existing XO counter selection invalid; no vote changes\n");return;}
  v.last=now;v.lastBusy=busy;v.idleAt=now;v.historyAt=now;v.ready=1;
  IOLog("HOUJI GPU OPP: 12 advertised factory levels 231..903 MHz; thermal cap now %u MHz (TSENS level %u), boot=500 MHz; SKU bin5/AB; firmware HFI votes, no PLL writes\n",hco_opp(houjiGpuCap())->khz/1000,houji_thermal_level);return;
 }
 uint64_t us=now-v.last;if(us<16000)return;
 if(us>v.maxTickUs)v.maxTickUs=us;
 uint64_t delta=busy-v.lastBusy;v.lastBusy=busy;v.last=now;
 if(delta>us*192/10+19200){++v.invalid;return;} // reject reset/backwards rather than fake 100% load
 uint64_t ns=us*1000,busyNs=delta*625/12;if(busyNs>ns)busyNs=ns;
 v.busyPermille=(unsigned)(busyNs*1000/ns);v.windowNs+=ns;v.busyNs+=busyNs;v.at[v.level]+=ns;
 uint64_t submits=gHoujiSubmit.ring.lastSubmitted();
 bool active=busyNs || submits!=v.lastSubmits || gHoujiApi.submit.outstanding();v.lastSubmits=submits;
 if(!active && now-v.idleAt>=80000)v.idle=true;
 if(active && v.idle){v.idle=false;v.wakeUntil=now+50000;++v.wakeBoosts;}
 if(active)v.idleAt=now;
 uint64_t touch=houji_dvfs_touch_time(),touchNs=0;
 if(touch){uint64_t abs=mach_absolute_time();if(abs>=touch)absolutetime_to_nanoseconds(abs-touch,&touchNs);}
 unsigned want=houjiGpuChoose(&v.policy,v.level,houjiGpuCap(),v.busyPermille,active,
  touch && touchNs<1000000000ULL,now<v.wakeUntil,(unsigned)gHoujiApi.submit.outstanding());
 if(!hco_opp(want)){v.bad=1;IOLog("HOUJI GPU DVFS: invalid policy output; no vote changes\n");return;}
 if(want!=v.level){
  uint32_t reply[6]={};const FhrRuntimeIo io={nullptr,houjiDvfsOwner,houjiDvfsDoorbell,houjiDvfsBarrier,houjiDvfsDelay};int code=fhr_runtime_vote(&gHoujiFull.storage,sizeof(gHoujiFull.storage),&io,want,reply);
  if(code){++v.failed;v.bad=1;IOLog("HOUJI GPU DVFS FAIL: request-index=%u bridge=%d HFI=%u sent=%u ACK=%u value=%u status=%u polls=%u; live worker isolates before log commit\n",want,code,reply[0],reply[1],reply[2],reply[3],reply[4],reply[5]);return;}
  v.level=want;++v.changes;v.low=0;
 }
 if(now-v.historyAt>=5000000){unsigned count=__atomic_load_n(&houjiGpuDvfsHistoryCount,__ATOMIC_RELAXED);
  if(count<48){houjiGpuDvfsHistory[count]={now,(v.windowNs-v.historyNs)/1000,(v.busyNs-v.historyBusy)/1000,(v.at[1]-v.history231)/1000,(v.at[HCO_BOOT_LEVEL]-v.history500)/1000,v.maxTickUs,v.changes,v.wakeBoosts};__atomic_store_n(&houjiGpuDvfsHistoryCount,count+1,__ATOMIC_RELEASE);}
  v.historyAt=now;v.historyNs=v.windowNs;v.historyBusy=v.busyNs;v.history231=v.at[1];v.history500=v.at[HCO_BOOT_LEVEL];v.maxTickUs=0;
 }
}
extern "C" void houji_gpu_dvfs_history_report(void){
 unsigned count=__atomic_load_n(&houjiGpuDvfsHistoryCount,__ATOMIC_ACQUIRE);if(count>48)count=48;
 for(unsigned i=0;i<count;i++){const auto&w=houjiGpuDvfsHistory[i];IOLog("HOUJI GPU WINDOW: up-ms=%llu span-us=%llu busy-us=%llu nominal231-us=%llu nominal500-us=%llu max-tick-us=%llu changes=%u wakes=%u; XO busy counter, ACK levels not core clock\n",w.upUs/1000,w.spanUs,w.busyUs,w.at231Us,w.at500Us,w.maxTickUs,w.changes,w.wakes);}
}
static void houjiDvfsReport(){auto&v=gHoujiDvfs;
 for(unsigned level=1;level<=HCO_COUNT;++level)IOLog("HOUJI GPU OPP TIME: index=%u MHz=%u nominal-us=%llu; HFI ACK residency\n",level,hco_opp(level)->khz/1000,v.at[level]/1000);
 IOLog("HOUJI GPU DVFS: enabled=%u ready=%u bad=%u request-MHz=%u HFI-index=%u changes=%u failed=%u busy-permille=%u counter-invalid=%llu samples-us=%llu busy-us=%llu at231-us=%llu at500-us=%llu; HFI ACK request, XO busy-time; not measured core clock\n",v.enabled,v.ready,v.bad,hco_opp(v.level)->khz/1000,v.level,v.changes,v.failed,v.busyPermille,v.invalid,v.windowNs/1000,v.busyNs/1000,v.at[1]/1000,v.at[HCO_BOOT_LEVEL]/1000);
}

extern "C" unsigned houji_gpu_dvfs_snapshot(char*out,unsigned capacity){
 if(!out || !capacity)return 0;unsigned used=0;out[0]=0;auto&v=gHoujiDvfs;
 auto append=[&](const char*f,auto...a){if(used>=capacity-1)return;int n=snprintf(out+used,capacity-used,f,a...);if(n>0)used+=(unsigned)n<capacity-used?(unsigned)n:capacity-used-1;};
 append("HOUJI OPP SNAPSHOT: ready=%u bad=%u level=%u cap-MHz=%u changes=%u failed=%u busy=%u reason=%u\n",v.ready,v.bad,v.level,hco_opp(houjiGpuCap())->khz/1000,v.changes,v.failed,v.busyPermille,v.policy.reason);
 for(unsigned i=1;i<=HCO_COUNT;i++)append("HOUJI OPP RESIDENCY: MHz=%u request-us=%llu\n",hco_opp(i)->khz/1000,__atomic_load_n(&v.at[i],__ATOMIC_RELAXED)/1000);
 return used;
}
