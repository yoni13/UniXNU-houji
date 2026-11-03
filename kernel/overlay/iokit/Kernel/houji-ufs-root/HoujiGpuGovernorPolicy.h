/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef HOUJI_GPU_GOVERNOR_POLICY_H
#define HOUJI_GPU_GOVERNOR_POLICY_H
#include "HoujiGpuFactoryOpp.h"
struct HoujiGpuGovernorPolicy {unsigned filtered,ready,low,quiet,reason;};
/* Policy only: no clocks, voltages, hardware access or scheduler calls here.
 * About 55% utilisation leaves room for the next interactive frame. Rising
 * load acts immediately; four low samples precede a fall of at most two OPPs.
 * The caller updates its current level only after the HFI ACK succeeds. */
static inline unsigned houjiGpuChoose(struct HoujiGpuGovernorPolicy *p,
 unsigned cur,unsigned cap,unsigned busy,unsigned active,unsigned touch,
 unsigned wake,unsigned queued) {
 if(!p || !hco_opp(cur) || !cap || cap>HCO_COUNT)return 0;
 if(busy>1000)busy=1000;
 if(!p->ready){p->ready=1;p->filtered=busy;}
 else p->filtered=(p->filtered*3+busy)/4;
 if(cur>cap){p->low=0;p->reason=6;return cap;}
 uint64_t need=(uint64_t)hco_opp(cur)->khz*p->filtered/550;
 if(busy>=900 && need<(uint64_t)hco_opp(cur)->khz*3/2)
  need=(uint64_t)hco_opp(cur)->khz*3/2;
 /* Two or more queued jobs: the GPU is behind the render server; go straight to the thermal cap. */
 if(queued>=2)need=(uint64_t)houjiGpuOpps[cap-1].khz;
 unsigned want=hco_at_least(need,cap),floor=1;p->reason=1;
 if(touch){floor=hco_at_least(680000,cap);p->reason=2;}
 if(wake){unsigned f=hco_at_least(578000,cap);if(f>floor)floor=f;p->reason=3;}
 if(queued){unsigned f=hco_at_least(500000,cap);if(f>floor)floor=f;}
 if(want<floor)want=floor;
 if(!active && !queued && !touch && !wake){
  if(++p->quiet>=5){want=1;p->reason=4;}
 }else p->quiet=0;
 if(want>cur){p->low=0;return want;}
 if(want==cur){p->low=0;return cur;}
 if(++p->low<4)return cur;
 p->low=0;return want+2<cur?cur-2:want;
}
#endif
