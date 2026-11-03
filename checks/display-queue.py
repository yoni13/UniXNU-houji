#!/usr/bin/env python3
"""Exercise actual SwapEnd/copy callbacks with host surface and event peers."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ARM = ROOT / 'kernel/overlay/iokit/Kernel/arm'
source = (ARM / 'SweetFramebuffer.cpp').read_text()
swap_start = source.index('IOReturn\nAppleCLCD::swap(')
copy_start = source.index('void\nAppleCLCD::copyFired(')
functions = source[swap_start:source.index('IOReturn\nAppleCLCD::waitSwap(', swap_start)]
functions += source[copy_start:source.index('void\nAppleCLCD::addClient(', copy_start)]
unit = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include "HoujiFrameQueue.h"
using IOReturn=int;
constexpr int kIOReturnSuccess=0,kIOReturnBadArgument=1,kIOReturnTimeout=2;
constexpr unsigned SWEET_FB_SWAP_REC_SIZE=0x46c,SWEET_FB_LAYERS=4;
constexpr int THREAD_ABORTSAFE=0,THREAD_AWAKENED=0;
struct OSObject {};
struct IOInterruptEventSource {
 unsigned requests=0;
 void interruptOccurred(void*,void*,int){++requests;}
};
struct SweetSurface {unsigned refs=0;void release(){assert(refs);--refs;}};
static SweetSurface surfaces[2];
static SweetSurface *sweet_surface_copy(uint32_t id){
 if(!id || id>2)return nullptr;
 ++surfaces[id-1].refs;return &surfaces[id-1];
}
struct HoujiDisplayFrame {
 uint8_t rec[SWEET_FB_SWAP_REC_SIZE]{};
 SweetSurface *surfaces[SWEET_FB_LAYERS]{};
 uint32_t id=0,fence=0;
};
static uint64_t houji_fb_copy_wakes,houji_fb_retired,houji_fb_copy_worker_ticks,houji_fb_copy_worker_max_ticks;
static uint64_t houji_fb_queue_full,houji_fb_queue_peak,sweet_fb_swap_count;
static void IOLockLock(void*){}
static void IOLockUnlock(void*){}
static void IOLockWakeup(void*,void*,bool){}
static int IOLockSleepDeadline(void*,void*,uint64_t,int){return 1;}
static uint64_t mach_absolute_time(){static uint64_t now=0;return ++now;}
static void nanoseconds_to_absolutetime(uint64_t ns,uint64_t *out){*out=ns;}
static uint32_t native_adreno_present_capture_fence(){return 7;}
static void houjiNoteSwapInterval(){}
struct IOTimerEventSource {unsigned armed=0;void setTimeoutUS(unsigned){++armed;}};
static uint32_t get32(const uint8_t *p,uint32_t offset){uint32_t n;memcpy(&n,p+offset,4);return n;}
class AppleCLCD : public OSObject {
public:
 void *fLock=nullptr;
 HoujiFrameQueue<HoujiDisplayFrame,8> fFrames;
 unsigned fFlushTicks=1,copies=0;
 int fImmediate=1;
 bool gpuReady=false;
 IOInterruptEventSource event;
 IOInterruptEventSource *fCopyEvent=&event;
 IOTimerEventSource retry;
 IOTimerEventSource *fCopyRetry=&retry;
 IOReturn swap(const uint8_t *);
 static void copyFired(OSObject *,IOInterruptEventSource *,int);
 static void copyRetryFired(OSObject *,IOTimerEventSource *);
 void copyQueued(void);
 bool present(const HoujiDisplayFrame &f){assert(f.fence==7);if(!gpuReady)return false;++copies;return true;}
};
static int enqueue(AppleCLCD &fb,uint32_t id,uint32_t surface){
 uint8_t rec[SWEET_FB_SWAP_REC_SIZE]{};
 memcpy(rec+0x50,&id,4);memcpy(rec+0x54,&surface,4);uint32_t mask=1;memcpy(rec+0x104,&mask,4);
 return fb.swap(rec);
}
'''
unit += functions
unit += r'''
int main(){
 // yoni13 port: SwapEnd wakes the copy worker at once (houji_fb_immediate=1), one wake drains every ready frame in order,
 // a frame whose GPU work is pending stays queued with its surface retained and the 500 us retry timer armed.
 AppleCLCD fb;
 assert(enqueue(fb,1,1)==kIOReturnSuccess && fb.event.requests==1);
 assert(enqueue(fb,2,2)==kIOReturnSuccess && fb.event.requests==2);
 AppleCLCD::copyFired(&fb,&fb.event,2);
 assert(fb.copies==0 && fb.fFrames.count()==2 && surfaces[0].refs==1 && surfaces[1].refs==1 && fb.retry.armed==1);
 fb.gpuReady=true;AppleCLCD::copyRetryFired(&fb,&fb.retry);
 assert(fb.copies==2 && fb.fFrames.complete(1) && fb.fFrames.complete(2) && fb.fFrames.count()==0);
 assert(!surfaces[0].refs && !surfaces[1].refs);
 // Without immediate wakes, swaps only queue; the notification timer paces copies.
 fb.fImmediate=0;assert(!enqueue(fb,3,1) && !enqueue(fb,4,2));
 assert(fb.event.requests==2 && fb.fFrames.count()==2);
 AppleCLCD::copyFired(&fb,&fb.event,1);assert(fb.fFrames.count()==0 && fb.copies==4);
 assert(enqueue(fb,5,99)==kIOReturnBadArgument && fb.fFrames.count()==0);
 assert(!surfaces[0].refs && !surfaces[1].refs);
 puts("PASS: actual display callbacks wake immediately, drain ready frames in one wake, retain pending surfaces with a retry, and obey timer pacing");
}
'''
with tempfile.TemporaryDirectory(prefix='houji-display-') as temporary:
    tmp = Path(temporary)
    (tmp / 'display.cpp').write_text(unit)
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-D__unused=__attribute__((unused))', '-I' + str(ARM), str(tmp / 'display.cpp'),
                    '-o', str(tmp / 'display')], check=True)
    subprocess.run([str(tmp / 'display')], check=True, timeout=10)
