/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * SM8650 board EPSS governor. Register facts: MiCode qcom-cpufreq-hw;
 * policy: UniXNU-sheng CpuFreqProbe (08cde20e). No raw voltage/PLL writes.
 * Four domains and caps are THIS phone's FDT + recovery cpufreq inventory.
 */
#include <IOKit/IOMemoryDescriptor.h>
#include "HoujiDvfsPolicy.h"
#include "HoujiThermal.inc"
extern "C" uint64_t houji_cpu_idle_mach(int);
extern "C" uint64_t houji_gpu_submit_activity(void);
static uint64_t houjiTouchTime;
/* Current TSENS level (HoujiThermal) for the GPU governor's cap. Starts at 2 (500 MHz GPU cap) until a real reading arrives. */
extern "C" {volatile uint32_t houji_thermal_level=2;}
extern "C" void houji_dvfs_touch(void) {
 __atomic_store_n(&houjiTouchTime,mach_absolute_time(),__ATOMIC_RELAXED);
}
extern "C" uint64_t houji_dvfs_touch_time(void) {
 return __atomic_load_n(&houjiTouchTime,__ATOMIC_RELAXED);
}
namespace HoujiCpuDvfs {
constexpr unsigned domains=4,rows=40;
constexpr unsigned cpuDomain[8]={0,0,3,3,3,1,1,2},counts[4]={2,2,1,3};
constexpr uint32_t caps[4]={2265600,2956800,3052800,3148800};
constexpr uint32_t minima[4]={364800,499200,480000,499200};
struct Domain {
 IOMemoryMap *map;uintptr_t va;uint32_t khz[rows];uint8_t index[rows];
 unsigned n,cur,low,load,changes,readback,bad,initial;uint32_t state;
};
static Domain d[domains];
static bool started;
static unsigned enabled;
struct History {
 uint64_t upMs,us,boostUs,frameUs,maxTickUs;
 uint32_t core[8],request[4],stateMin[4],stateMax[4],throttled[4],changes[4];
 int cpuMax,gpuMax,hottest;unsigned levelMax;
};
static History history[48];
static unsigned historyCount;
static uint32_t read(Domain&w,unsigned off){return *(volatile uint32_t*)(w.va+off);}
static bool init(unsigned id){
 auto&w=d[id];auto*m=IODeviceMemory::withRange(0x17d91000ULL+id*0x1000,0x1000);
 w.map=m?m->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;if(m)m->release();
 w.va=w.map?w.map->getVirtualAddress():0;
 if(!w.va || !(read(w,0)&1))return false;
 uint32_t prev=0;
 for(unsigned i=0;i<rows;i++){
  uint32_t raw=read(w,0x100+i*4),src=raw>>30,khz=(raw&255)*19200;
  if(!src || khz<minima[id] || khz>4000000)return false;
  if(i && khz==prev)break;
  if(i && khz<prev)return false;prev=khz;
  if(khz>caps[id])continue;
  w.khz[w.n]=khz;w.index[w.n++]=i;
 }
 if(w.n<2 || w.khz[0]!=minima[id] || w.khz[w.n-1]!=caps[id])return false;
 w.initial=read(w,0x320);if(w.initial>=rows)return false;w.cur=w.n-1;
 for(unsigned i=0;i<w.n;i++)if(w.index[i]==w.initial)w.cur=i;
 IOLog("HOUJI CPU DVFS INIT: domain=%u cpus=%u rows=%u cap-kHz=%u initial-index=%u initial-request-kHz=%u control=%x; factory LUT, EPSS voltage control preserved\n",id,counts[id],w.n,caps[id],w.initial,(read(w,0x100+w.initial*4)&255)*19200,read(w,0xb0));
 return true;
}
static bool vote(unsigned id,unsigned at){
 auto&w=d[id];if(at>=w.n || !(read(w,0)&1))return false;
 unsigned n=(read(w,0xb0)&1)?counts[id]:1;
 for(unsigned c=0;c<n;c++)*(volatile uint32_t*)(w.va+0x320+c*4)=w.index[at];
 __asm__ volatile("dsb sy" ::: "memory");
 for(unsigned c=0;c<n;c++)if(read(w,0x320+c*4)!=w.index[at])return false;
 w.cur=at;++w.changes;return true;
}
static void worker(void*,wait_result_t){
 thread_set_thread_name(current_thread(),"houji-cpu-dvfs");
 uint64_t last=mach_absolute_time(),idle[8]={},report=last,boost=0,frameTicks=0,frameUntil=0;
 uint64_t submits=houji_gpu_submit_activity(),sumUs=0,boostUs=0,frameUs=0,maxTickUs=0;
 uint64_t coreUs[8]={},requestUs[4]={};
 HoujiThermal::Reading heat{};bool sensing=HoujiThermal::init();unsigned heatLevel=sensing?0:2,heatTick=0,levelMax=0,spikesIgnored=0,spikesActed=0;
 int cpuMax=-1000,gpuMax=-1000,hotMax=-1000;
 if(!sensing)IOLog("HOUJI THERMAL: sensors unavailable; fixed level 2 CPU cap\n");
 unsigned recent=0,quiet[4]={},stateMin[4]={UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX},stateMax[4]={},throttled[4]={},changes[4]={};
 nanoseconds_to_absolutetime(300000000,&boost);
 nanoseconds_to_absolutetime(300000000,&frameTicks);
 for(unsigned c=0;c<8;c++)idle[c]=houji_cpu_idle_mach(c);
 for(;;){
  IOSleep(40);uint64_t now=mach_absolute_time(),dt=now-last;last=now;
  if(!dt)continue;unsigned busy[4]={};uint64_t dtNs=0;absolutetime_to_nanoseconds(dt,&dtNs);uint64_t us=dtNs/1000;
  sumUs+=us;if(us>maxTickUs)maxTickUs=us;
  uint64_t nextSubmits=houji_gpu_submit_activity();recent=((recent<<1)|(nextSubmits!=submits))&15;submits=nextSubmits;
  if(__builtin_popcount(recent)>=2)frameUntil=now+frameTicks;
  bool framing=now<frameUntil;
  for(unsigned c=0;c<8;c++){
   uint64_t v=houji_cpu_idle_mach(c),old=idle[c];idle[c]=v;
   if(v==UINT64_MAX || old==UINT64_MAX || v<old)continue;
   uint64_t di=v-old;unsigned b=di>=dt?0:(unsigned)((dt-di)*1000/dt);
   coreUs[c]+=uint64_t(b)*us;
   if(b>busy[cpuDomain[c]])busy[cpuDomain[c]]=b;
  }
  uint64_t touch=houji_dvfs_touch_time();bool boosted=touch && now>=touch && now-touch<boost;
  if(boosted)boostUs+=us;if(framing)frameUs+=us;
  if(sensing && ++heatTick>=6){heatTick=0;
   if(HoujiThermal::read(heat)){
    /* A single 240 ms sample on one sensor reached 87-93 C with ~61 C on the next (app launch bursts), dropping CPU and GPU
     * to minimum for about a second. Rises follow the lower of two consecutive readings; >=95 C still acts at once. */
    static int previous=-1000;
    int confirmed=heat.hottest>=950?heat.hottest:(heat.hottest<previous?heat.hottest:previous);
    /* Spikes that keep coming are heat, not noise: a third single-sample spike within 10 s (42 samples) counts at its own value
     * (a phone that reset itself at 09:17 had 181 ignored spikes of 72-87 C before its last 80.7 C reading). */
    static uint32_t sample,spikeAt[3]={0,0,0};++sample;
    if(heatLevel+1<HoujiThermal::levels && heat.hottest>=HoujiThermal::trigger[heatLevel+1] && confirmed<HoujiThermal::trigger[heatLevel+1]){
     spikeAt[0]=spikeAt[1];spikeAt[1]=spikeAt[2];spikeAt[2]=sample;
     if(spikeAt[0] && sample-spikeAt[0]<=42){confirmed=heat.hottest;++spikesActed;}
     else ++spikesIgnored;
    }
    previous=heat.hottest;
    unsigned next=HoujiThermal::level(heatLevel,confirmed>-1000?confirmed:heat.hottest);
    if(next!=heatLevel)IOLog("HOUJI THERMAL LEVEL: %u->%u hottest=%d.%dC sensor=%u/%u cpu=%d.%dC gpu=%d.%dC\n",heatLevel,next,heat.hottest/10,heat.hottest%10,heat.hottestId>>4,heat.hottestId&15,heat.cpu/10,heat.cpu%10,heat.gpu/10,heat.gpu%10);
    heatLevel=next;__atomic_store_n(&houji_thermal_level,heatLevel,__ATOMIC_RELAXED);
    if(heat.cpu>cpuMax)cpuMax=heat.cpu;if(heat.gpu>gpuMax)gpuMax=heat.gpu;if(heat.hottest>hotMax)hotMax=heat.hottest;
   }else if(heatLevel<2){heatLevel=2;IOLog("HOUJI THERMAL: read failed valid=%u invalid=%u; level 2 CPU cap\n",heat.valid,heat.invalid);}
  }
  if(heatLevel>levelMax)levelMax=heatLevel;
  for(unsigned id=0;id<domains;id++){
   auto&w=d[id];if(w.bad)continue;
   w.load=busy[id];w.state=read(w,0x20);uint32_t state=(w.state&255)*19200;
   requestUs[id]+=uint64_t(w.khz[w.cur])*us;
   if(state<stateMin[id])stateMin[id]=state;if(state>stateMax[id])stateMax[id]=state;
   if(w.load>=500 && state && uint64_t(state)*5<uint64_t(w.khz[w.cur])*4)++throttled[id];
   unsigned floor=houjiDvfsInteractiveFloor(w.khz,w.n,w.load,id!=0,boosted,framing,&quiet[id]);
   unsigned cap=HoujiThermal::capIndex(w.khz,w.n,id?HoujiThermal::bigPct[heatLevel]:HoujiThermal::littlePct[heatLevel]);
   if(floor>cap)floor=cap;
   unsigned at=houjiDvfsChoose(w.khz,w.n,w.cur,w.load,floor,&w.low);
   if(at>cap)at=cap;
   if(at!=w.cur && !vote(id,at)){w.bad=1;IOLog("HOUJI CPU DVFS FAIL: domain=%u request/readback mismatch; domain writes disabled\n",id);}
   w.readback=read(w,0x320);w.state=read(w,0x20);
  }
  uint64_t ns=0;absolutetime_to_nanoseconds(now-report,&ns);
  if(ns>=5000000000ULL){report=now;
   unsigned count=__atomic_load_n(&historyCount,__ATOMIC_RELAXED);
   if(count<48 && sumUs){auto&h=history[count];uint64_t upNs=0;absolutetime_to_nanoseconds(now,&upNs);
    h.upMs=upNs/1000000;h.us=sumUs;h.boostUs=boostUs;h.frameUs=frameUs;h.maxTickUs=maxTickUs;
    for(unsigned c=0;c<8;c++)h.core[c]=(uint32_t)(coreUs[c]/sumUs);
    h.cpuMax=cpuMax;h.gpuMax=gpuMax;h.hottest=hotMax;h.levelMax=levelMax;
    for(unsigned id=0;id<domains;id++){auto&w=d[id];h.request[id]=(uint32_t)(requestUs[id]/sumUs);h.stateMin[id]=stateMin[id];h.stateMax[id]=stateMax[id];h.throttled[id]=throttled[id];h.changes[id]=w.changes-changes[id];}
    __atomic_store_n(&historyCount,count+1,__ATOMIC_RELEASE);
   }
   IOLog("HOUJI THERMAL: hottest=%d.%dC sensor=%u/%u cpu=%d.%dC gpu=%d.%dC ddr=%d.%dC level=%u window-max=%d.%dC valid=%u invalid=%u spikes-ignored=%u spikes-acted=%u\n",heat.hottest/10,heat.hottest%10,heat.hottestId>>4,heat.hottestId&15,heat.cpu/10,heat.cpu%10,heat.gpu/10,heat.gpu%10,heat.ddr/10,heat.ddr%10,heatLevel,hotMax/10,hotMax%10,heat.valid,heat.invalid,spikesIgnored,spikesActed);
   cpuMax=gpuMax=hotMax=-1000;levelMax=heatLevel;
   sumUs=boostUs=frameUs=maxTickUs=0;for(auto&c:coreUs)c=0;
   for(unsigned id=0;id<domains;id++){requestUs[id]=0;stateMin[id]=UINT32_MAX;stateMax[id]=throttled[id]=0;changes[id]=d[id].changes;}
   for(unsigned id=0;id<domains;id++){auto&w=d[id];IOLog("HOUJI CPU DVFS: d=%u req-kHz=%u read-index=%u state-kHz=%u load-permille=%u changes=%u bad=%u touch-boost=%u; domain-state hardware sample, not cycle measurement\n",id,w.khz[w.cur],w.readback,(w.state&255)*19200,w.load,w.changes,w.bad,boosted);}
  }
 }
}
static void start(){
 if(started)return;started=true;PE_parse_boot_argn("houji_dvfs",&enabled,sizeof(enabled));
 if(!(enabled&1))return;
 {char text[32]={};
  if(PE_parse_boot_arg_str("houji_cpuf",text,sizeof text)){
   unsigned v[3]={houjiDvfsFloors[0],houjiDvfsFloors[1],houjiDvfsFloors[2]},i=0;const char *p=text;
   while(*p && i<3){unsigned x=0;bool any=false;while(*p>='0'&&*p<='9'){x=x*10+unsigned(*p-'0');p++;any=true;}if(any&&x<=100)v[i]=x;i++;if(*p==',')p++;else break;}
   for(unsigned k=0;k<3;k++)houjiDvfsFloors[k]=v[k];
  }
  IOLog("HOUJI CPU DVFS FLOORS: frame=%u%% touch=%u%% recent=%u%%\n",houjiDvfsFloors[0],houjiDvfsFloors[1],houjiDvfsFloors[2]);}
 for(unsigned id=0;id<domains;id++)if(!init(id)){IOLog("HOUJI CPU DVFS: domain=%u factory validation refused; no frequency writes\n",id);return;}
 for(unsigned id=0;id<domains;id++)if(!vote(id,d[id].cur)){IOLog("HOUJI CPU DVFS: initial readback refused domain=%u\n",id);return;}
 thread_t t;if(kernel_thread_start(worker,nullptr,&t)==KERN_SUCCESS)thread_deallocate(t);
}
}
extern "C" void houji_cpu_dvfs_history_report(void){
 using namespace HoujiCpuDvfs;
 unsigned count=__atomic_load_n(&historyCount,__ATOMIC_ACQUIRE);if(count>48)count=48;
 for(unsigned i=0;i<count;i++){const auto&h=history[i];
  IOLog("HOUJI CPU WINDOW: up-ms=%llu span-ms=%llu core-permille=%u,%u,%u,%u,%u,%u,%u,%u mean-req-kHz=%u,%u,%u,%u state-min=%u,%u,%u,%u state-max=%u,%u,%u,%u throttle=%u,%u,%u,%u changes=%u,%u,%u,%u touch-ms=%llu frame-ms=%llu max-tick-us=%llu heat-max-dC=%d,%d,%d level-max=%u\n",h.upMs,h.us/1000,h.core[0],h.core[1],h.core[2],h.core[3],h.core[4],h.core[5],h.core[6],h.core[7],h.request[0],h.request[1],h.request[2],h.request[3],h.stateMin[0],h.stateMin[1],h.stateMin[2],h.stateMin[3],h.stateMax[0],h.stateMax[1],h.stateMax[2],h.stateMax[3],h.throttled[0],h.throttled[1],h.throttled[2],h.throttled[3],h.changes[0],h.changes[1],h.changes[2],h.changes[3],h.boostUs/1000,h.frameUs/1000,h.maxTickUs,h.cpuMax,h.gpuMax,h.hottest,h.levelMax);
 }
}
