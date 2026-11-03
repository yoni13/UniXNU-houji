/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * SM8650 TSENS v2 reader. Boot firmware configures the controllers; this only
 * reads TM_Sn_STATUS (TM base + 0xa0 + 4n: LAST_TEMP [11:0] signed deci-C,
 * VALID bit 21), as Linux drivers/thermal/qcom/tsens-v2.c does.
 * Sensor roles come from this phone's FDT thermal-zones.
 * Stock firmware shows its red chip at TsensHighTemp 85 C and stops at 90 C.
 */
namespace HoujiThermal {
constexpr unsigned controllers=3;
constexpr uint64_t tm[controllers]={0x0c228000,0x0c229000,0x0c22a000};
constexpr unsigned sensors[controllers]={15,16,13};
/* bit n set: sensor n of that controller is a CPU / GPU sensor. */
constexpr uint32_t cpuMask[controllers]={0x7ffe,0x003e,0};
constexpr uint32_t gpuMask[controllers]={0,0,0x01fe};
constexpr unsigned ddrController=1,ddrSensor=13;
static uintptr_t va[controllers];
static IOMemoryMap *maps[controllers];
static bool ready;
struct Reading { int cpu,gpu,ddr,hottest; unsigned hottestId,valid,invalid; };
static bool init(){
 for(unsigned c=0;c<controllers;c++){
  auto*m=IODeviceMemory::withRange(tm[c],0x1000);
  maps[c]=m?m->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;if(m)m->release();
  va[c]=maps[c]?maps[c]->getVirtualAddress():0;
  if(!va[c]){IOLog("HOUJI THERMAL: controller %u map failed\n",c);return false;}
 }
 ready=true;return true;
}
/* Temperatures in deci-C. A sensor that is never valid counts as invalid. */
static bool read(Reading&r){
 r={-1000,-1000,-1000,-1000,0,0,0};if(!ready)return false;
 for(unsigned c=0;c<controllers;c++)for(unsigned s=0;s<sensors[c];s++){
  uint32_t v=0;bool ok=false;
  for(unsigned tries=0;tries<3 && !ok;tries++){v=*(volatile uint32_t*)(va[c]+0xa0+4*s);ok=(v>>21)&1;}
  if(!ok){++r.invalid;continue;}
  int t=(int)(v&0xfff);if(t&0x800)t-=0x1000;
  if(t<-400 || t>1500){++r.invalid;continue;}
  ++r.valid;
  if(t>r.hottest){r.hottest=t;r.hottestId=c*16+s;}
  if(cpuMask[c]>>s&1 && t>r.cpu)r.cpu=t;
  if(gpuMask[c]>>s&1 && t>r.gpu)r.gpu=t;
  if(c==ddrController && s==ddrSensor)r.ddr=t;
 }
 return r.valid>0 && r.cpu>-1000;
}
/* Level -> percent of each domain's maximum. Rise as soon as a trigger is
 * reached; fall one level only 3 C below that level's trigger. */
constexpr int trigger[]={0,700,750,800,850,900};
constexpr unsigned bigPct[]={100,80,65,50,35,0};
constexpr unsigned littlePct[]={100,100,100,80,50,0};
constexpr unsigned levels=sizeof(trigger)/sizeof(trigger[0]);
static unsigned level(unsigned cur,int hottest){
 unsigned want=0;while(want+1<levels && hottest>=trigger[want+1])++want;
 if(want>=cur)return want;
 if(hottest<trigger[cur]-30)return cur-1;
 return cur;
}
static unsigned capIndex(const uint32_t *khz,unsigned n,unsigned pct){
 if(!n)return 0;unsigned at=0;
 while(at+1<n && (uint64_t)khz[at+1]*100<=(uint64_t)khz[n-1]*pct)++at;
 return at;
}
}
