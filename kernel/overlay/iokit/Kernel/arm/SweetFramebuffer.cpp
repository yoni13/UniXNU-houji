/*
 * Built-in IOMobileFramebuffer for houji on the SWEET kernel target: the
 * ABL's continuous-splash framebuffer (1200x2670 in RAM) remains scanout.
 * Completed compositor surfaces are copied into it by a separate worker.
 *
 * IOMobileFramebuffer.framework (iOS 16.7) counts /arm-io children with
 * device_type "display-subsystem", waits for that many services matching
 * IOServiceMatching("IOMobileFramebuffer"), opens each with type 0 and
 * reads canvas-width/canvas-height from the service's provider. The class
 * here (AppleCLCD, an IOMobileFramebuffer) matches the /arm-io/disp0 nub.
 *
 * User client selectors used by CoreAnimation:
 *   3  GetLayerDefaultSurface [layer, ?] -> [IOSurface port name]
 *   4  SwapBegin -> [swap id]
 *   5  SwapEnd, struct 0x46c: +0x50 swap id, +0x54 IOSurface id per layer
 *      (4 layers), +0x64 rect a / +0xc4 rect b per layer (int x,y,w,h),
 *      +0x104 enabled layer mask
 *   6  SwapWait, 7 GetID, 8 GetDisplaySize -> [w, h], 18 IsMainDisplay,
 *   28 GetDotPitch, 72 Enable/DisableNotifications [func, refcon, type, 0]
 * Everything else is accepted and logged once.
 *
 * SwapEnd retains the enabled layers for a fenced CPU copy (no scaling).
 * Vsync (notification type 5) is a configurable 60/120 Hz software timer that
 * sends (count, mach_absolute_time, 0) as 32-bit halves to clients that
 * enabled it, through the port they registered (IOConnectSetNotificationPort).
 *
 * The panel memory itself is published as an IOSurface and returned as the
 * default framebuffer surface. The kernel text console keeps drawing over
 * the GUI by default: pausing it (boot-arg sweetfb_noconsole=1, on the
 * first swap) switches the console to serial, which also hides panics.
 */
#include "SweetSurface.h"
class IOService;
extern "C" void houji_backlight_start(IOService *provider);   /* houji-buttons/HoujiScreen.cpp */
extern "C" void houji_surface_use_report(void);
#include "HoujiSurfaceAcquire.h"
#include <IOKit/IOBufferMemoryDescriptor.h>

extern "C" int houji_console_muted;

#if defined(SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/system.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include "HoujiFrameQueue.h"
#include <sys/proc.h>

enum {
	kSweetFBGetLayerDefaultSurface  = 3,
	kSweetFBSwapBegin               = 4,
	kSweetFBSwapEnd                 = 5,
	kSweetFBSwapWait                = 6,
	kSweetFBGetID                   = 7,
	kSweetFBGetDisplaySize          = 8,
	kSweetFBIsMainDisplay           = 18,
	kSweetFBGetDotPitch             = 28,
	kSweetFBNotifications           = 72,
};

#define SWEET_FB_SWAP_REC_SIZE          0x46c
#define SWEET_FB_LAYERS                 4
#define SWEET_FB_NOTIFY_TYPES           8
#define SWEET_FB_NOTIFY_VSYNC           5
#define SWEET_FB_VSYNC_US               16667
#define SWEET_FB_DOT_PITCH              458     /* iPhone X logical canvas */
#define HOUJI_LOGICAL_W 1200
#define HOUJI_LOGICAL_H 2670
static uint64_t houji_fb_copy_bytes, houji_fb_present_ticks, houji_fb_max_present_ticks;
static uint64_t houji_fb_missed_timer_ticks;
static uint64_t houji_fb_callbacks,houji_fb_due_ticks;
static uint64_t houji_fb_copy_wakes,houji_fb_copy_worker_ticks,houji_fb_copy_worker_max_ticks;
static uint64_t houji_fb_started_at,houji_fb_last_notify_at,houji_fb_max_notify_gap;
static uint32_t houji_fb_hz_arg_found,houji_fb_config_hz,houji_fb_flush_every;
static size_t houji_fb_command_line_bytes;
static uint64_t sweet_fb_swap_count,houji_fb_present_count;
static uint64_t houji_fb_sync_deferred,houji_fb_cache_bytes,houji_fb_cache_ticks,houji_fb_cache_denied;
static uint64_t houji_fb_retired,houji_fb_waits,houji_fb_wait_blocked,houji_fb_wait_timeout,houji_fb_queue_full,houji_fb_queue_peak;
struct HoujiDisplayHistory {uint32_t sequence;uint64_t values[7];};
static HoujiDisplayHistory houji_fb_history[40];
static uint32_t houji_fb_history_count;
struct HoujiRectObservation { uint32_t sequence,values[15]; };
static HoujiRectObservation houji_fb_rects[8];
static uint32_t houji_fb_rect_count;
struct HoujiColourObservation {uint32_t sequence,values[12];};
static HoujiColourObservation houji_fb_colours[12];
static uint32_t houji_fb_colour_count;
static HoujiColourObservation houji_fb_colour_peaks[SWEET_FB_LAYERS];
static uint64_t houji_fb_colour_frames,houji_fb_colour_purple,houji_fb_colour_zero_alpha;
static void houjiSaveColour(HoujiColourObservation &slot,uint32_t n,const uint32_t *values){
 __atomic_store_n(&slot.sequence,0u,__ATOMIC_RELEASE);
 __atomic_thread_fence(__ATOMIC_RELEASE);
 for(unsigned i=0;i<12;i++)__atomic_store_n(&slot.values[i],values[i],__ATOMIC_RELAXED);
 __atomic_store_n(&slot.sequence,n,__ATOMIC_RELEASE);
}
/* Read 64 points from the already acquired render surface, with the existing
 * GPU fence/issuer guard held. Store only colour counts, never pixels/text. */
static void houjiObserveColours(uint32_t frame,uint32_t layer,SweetSurface*s,uintptr_t src,
 int32_t sx,int32_t sy,int32_t w,int32_t h,int32_t dx,int32_t dy){
 if(w<8 || h<8)return;
 uint32_t opaqueBlack=0,transparentBlack=0,magenta=0,zeroAlpha=0;
 for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++){
  uint32_t ix=(uint64_t(2*x+1)*w)/16,iy=(uint64_t(2*y+1)*h)/16;
  uintptr_t at=src+s->offset+uint64_t(sy+iy)*s->bytesPerRow+uint64_t(sx+ix)*4;
  uint32_t value=*(const volatile uint32_t*)at,alpha=value>>24;
  if(!(value&0xffffffu)){if(!alpha)++transparentBlack;else if(alpha==255)++opaqueBlack;}
  if(!alpha)++zeroAlpha;
  uint32_t blue=value&255,green=(value>>8)&255,red=(value>>16)&255;
  // Broad hue observation includes the owner's pale/dark purple shadows. It
  // can also count intentional wallpaper colours, so is not a corruption test.
  uint32_t low=red<blue?red:blue,high=red>blue?red:blue;
  if(low>=24 && low>green+green/2+8 && high<=low*3)++magenta;
 }
 __atomic_add_fetch(&houji_fb_colour_frames,1ull,__ATOMIC_RELAXED);
 __atomic_add_fetch(&houji_fb_colour_purple,(uint64_t)magenta,__ATOMIC_RELAXED);
 __atomic_add_fetch(&houji_fb_colour_zero_alpha,(uint64_t)zeroAlpha,__ATOMIC_RELAXED);
 if((frame&15) && !magenta)return;
 uint32_t n=__atomic_add_fetch(&houji_fb_colour_count,1u,__ATOMIC_RELAXED);
 auto &slot=houji_fb_colours[(n-1)%12];
 uint32_t values[12]={frame,layer,s->pixelFormat,(uint32_t)w,(uint32_t)h,(uint32_t)dx,(uint32_t)dy,64,opaqueBlack,transparentBlack,magenta,zeroAlpha};
 houjiSaveColour(slot,n,values);
 // The copy workloop is the sole writer. Keep each layer's strongest hue
 // observation for the whole boot, even after the owner changes applications.
 if(layer<SWEET_FB_LAYERS && magenta>__atomic_load_n(&houji_fb_colour_peaks[layer].values[10],__ATOMIC_RELAXED))
  houjiSaveColour(houji_fb_colour_peaks[layer],n,values);
}
static void houjiReportColours(void){
 IOLog("HOUJI COLOUR LIFETIME: surfaces=%llu sampled-points=%llu purple-candidates=%llu alpha-zero=%llu; aggregate only, intentional colours included\n",__atomic_load_n(&houji_fb_colour_frames,__ATOMIC_RELAXED),__atomic_load_n(&houji_fb_colour_frames,__ATOMIC_RELAXED)*64,__atomic_load_n(&houji_fb_colour_purple,__ATOMIC_RELAXED),__atomic_load_n(&houji_fb_colour_zero_alpha,__ATOMIC_RELAXED));
 for(auto &slot:houji_fb_colour_peaks){
  uint32_t n=__atomic_load_n(&slot.sequence,__ATOMIC_ACQUIRE),v[12];if(!n)continue;
  for(unsigned i=0;i<12;i++)v[i]=__atomic_load_n(&slot.values[i],__ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  if(n!=__atomic_load_n(&slot.sequence,__ATOMIC_ACQUIRE))continue;
  IOLog("HOUJI COLOUR PEAK: n=%u frame=%u layer=%u fmt=%x rect=%ux%u dst=%u,%u samples=%u opaque-black=%u transparent-black=%u purple-candidates=%u alpha-zero=%u; retained across scene changes\n",n,v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8],v[9],v[10],v[11]);
 }
 for(auto &slot:houji_fb_colours){
  uint32_t n=__atomic_load_n(&slot.sequence,__ATOMIC_ACQUIRE),v[12];if(!n)continue;
  for(unsigned i=0;i<12;i++)v[i]=__atomic_load_n(&slot.values[i],__ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  if(n!=__atomic_load_n(&slot.sequence,__ATOMIC_ACQUIRE))continue;
  IOLog("HOUJI COLOUR SOURCE: n=%u frame=%u layer=%u fmt=%x rect=%ux%u dst=%u,%u samples=%u opaque-black=%u transparent-black=%u purple-candidates=%u alpha-zero=%u; completed source samples, packed-word counts only\n",n,v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8],v[9],v[10],v[11]);
 }
 uint64_t now=mach_absolute_time(),elapsedNs=0,gapNs=0,workerNs=0;
 absolutetime_to_nanoseconds(now-houji_fb_started_at,&elapsedNs);
 absolutetime_to_nanoseconds(__atomic_load_n(&houji_fb_max_notify_gap,__ATOMIC_RELAXED),&gapNs);
 absolutetime_to_nanoseconds(__atomic_load_n(&houji_fb_copy_worker_max_ticks,__ATOMIC_RELAXED),&workerNs);
 IOLog("HOUJI DISPLAY FINAL: configured-hz=%u callbacks=%llu elapsed-ms=%llu max-notify-gap-us=%llu copy-wakes=%llu max-worker-us=%llu missed=%llu; notification delivery, not measured panel frames\n",houji_fb_config_hz,__atomic_load_n(&houji_fb_callbacks,__ATOMIC_RELAXED),elapsedNs/1000000,gapNs/1000,__atomic_load_n(&houji_fb_copy_wakes,__ATOMIC_RELAXED),workerNs/1000,__atomic_load_n(&houji_fb_missed_timer_ticks,__ATOMIC_RELAXED));
}
static void houjiObserveRect(uint32_t frame,uint32_t layer,uint32_t id,uint32_t width,uint32_t height,uint32_t format,const uint8_t *a,const uint8_t *b) {
 uint32_t v[15]={frame,layer,id,width,height,format};memcpy(&v[6],a,16);memcpy(&v[10],b,16);
 bool scale=v[8]&&v[9]&&v[12]&&v[13]&&(v[8]!=v[12]||v[9]!=v[13]);
 bool crop=int32_t(v[6])<0||int32_t(v[7])<0||int32_t(v[10])<0||int32_t(v[11])<0;
 if(!scale&&!crop)return;v[14]=(scale?1u:0u)|(crop?2u:0u);
 uint32_t n=__atomic_add_fetch(&houji_fb_rect_count,1u,__ATOMIC_RELAXED);auto &slot=houji_fb_rects[(n-1)%8];
 __atomic_store_n(&slot.sequence,0u,__ATOMIC_RELEASE);
 __atomic_thread_fence(__ATOMIC_RELEASE);
 for(unsigned i=0;i<15;i++)__atomic_store_n(&slot.values[i],v[i],__ATOMIC_RELAXED);
 __atomic_store_n(&slot.sequence,n,__ATOMIC_RELEASE);
}
static void houjiReportRects(void) {
 for(unsigned slot=0;slot<8;slot++){
  auto &s=houji_fb_rects[slot];uint32_t n=__atomic_load_n(&s.sequence,__ATOMIC_ACQUIRE),v[15];if(!n)continue;
  for(unsigned i=0;i<15;i++)v[i]=__atomic_load_n(&s.values[i],__ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  if(n!=__atomic_load_n(&s.sequence,__ATOMIC_ACQUIRE))continue;
  IOLog("HOUJI DISPLAY RECT: n=%u frame=%u layer=%u surface=%u %ux%u fmt=%x src=%d,%d,%u,%u dst=%d,%d,%u,%u flags=%u; observed record only\n",n,v[0],v[1],v[2],v[3],v[4],v[5],int32_t(v[6]),int32_t(v[7]),v[8],v[9],int32_t(v[10]),int32_t(v[11]),v[12],v[13],v[14]);
 }
}

// Copy cached facts into the reserved log prefix. IOLog-only observations can
// disappear from the ring before recovery; this path never reads pixels/MMIO.
extern "C" unsigned houji_framebuffer_snapshot(char *out,unsigned capacity) {
 if(!out || !capacity)return 0;
 unsigned used=0;out[0]=0;
 auto append=[&](const char *format,auto... args){
  if(used>=capacity-1)return;
  int n=snprintf(out+used,capacity-used,format,args...);
  if(n>0 && (unsigned)n<capacity-used)used+=(unsigned)n;
  else out[used]=0; // retain only complete rows in the fixed log prefix
 };
 auto load=[](const uint64_t *v){return __atomic_load_n(v,__ATOMIC_RELAXED);};
 auto us=[](uint64_t ticks){uint64_t ns=0;absolutetime_to_nanoseconds(ticks,&ns);return ns/1000;};
 uint64_t now=mach_absolute_time(),began=load(&houji_fb_started_at);
 append("HOUJI DISPLAY TIMING: configured-hz=%u elapsed-ms=%llu callbacks=%llu due=%llu max-gap-us=%llu missed=%llu; software notifications, not panel vblank\n",
  houji_fb_config_hz,began && now>=began?us(now-began)/1000:0,load(&houji_fb_callbacks),load(&houji_fb_due_ticks),us(load(&houji_fb_max_notify_gap)),load(&houji_fb_missed_timer_ticks));
 append("HOUJI DISPLAY COPY: swaps=%llu completed=%llu bytes=%llu copy-us=%llu max-us=%llu worker-us=%llu max-worker-us=%llu; CPU copy and store barrier\n",
  load(&sweet_fb_swap_count),load(&houji_fb_present_count),load(&houji_fb_copy_bytes),us(load(&houji_fb_present_ticks)),us(load(&houji_fb_max_present_ticks)),us(load(&houji_fb_copy_worker_ticks)),us(load(&houji_fb_copy_worker_max_ticks)));
 append("HOUJI DISPLAY QUEUE: retired=%llu waits=%llu blocked=%llu timeouts=%llu full=%llu peak=%llu deferred=%llu cache-bytes=%llu cache-us=%llu denied=%llu\n",
  load(&houji_fb_retired),load(&houji_fb_waits),load(&houji_fb_wait_blocked),load(&houji_fb_wait_timeout),load(&houji_fb_queue_full),load(&houji_fb_queue_peak),load(&houji_fb_sync_deferred),load(&houji_fb_cache_bytes),us(load(&houji_fb_cache_ticks)),load(&houji_fb_cache_denied));
 const uint32_t end=__atomic_load_n(&houji_fb_history_count,__ATOMIC_ACQUIRE);
 // Reserve room for colour/crop facts too. Four recent windows survive even
 // when the IOLog tail has wrapped during a long interactive test.
 for(uint32_t offset=0;offset<4 && offset<end;offset++){
  uint32_t n=end-offset;auto &slot=houji_fb_history[(n-1)%40];uint64_t v[7];
  if(__atomic_load_n(&slot.sequence,__ATOMIC_ACQUIRE)!=n)continue;
  for(unsigned i=0;i<7;i++)v[i]=__atomic_load_n(&slot.values[i],__ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  if(__atomic_load_n(&slot.sequence,__ATOMIC_ACQUIRE)!=n)continue;
  append("HOUJI DISPLAY HISTORY: sample=%u up-ms=%llu window-ms=%llu swaps=%llu presents=%llu copy-us=%llu max-us=%llu missed=%llu; CPU copy, not panel FPS\n",n,v[0],v[1],v[2],v[3],v[4],v[5],v[6]);
 }
 append("HOUJI DISPLAY SOURCE: surfaces=%llu purple-candidates=%llu alpha-zero=%llu; cached samples, intentional colours included\n",
  __atomic_load_n(&houji_fb_colour_frames,__ATOMIC_RELAXED),
  __atomic_load_n(&houji_fb_colour_purple,__ATOMIC_RELAXED),
  __atomic_load_n(&houji_fb_colour_zero_alpha,__ATOMIC_RELAXED));
 for(auto &slot:houji_fb_colour_peaks){
  uint32_t n=__atomic_load_n(&slot.sequence,__ATOMIC_ACQUIRE),v[12];if(!n)continue;
  for(unsigned i=0;i<12;i++)v[i]=__atomic_load_n(&slot.values[i],__ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  if(n!=__atomic_load_n(&slot.sequence,__ATOMIC_ACQUIRE))continue;
  append("HOUJI DISPLAY SOURCE PEAK: frame=%u layer=%u fmt=%x rect=%ux%u dst=%d,%d samples=%u black=%u transparent-black=%u purple=%u alpha-zero=%u\n",
   v[0],v[1],v[2],v[3],v[4],int32_t(v[5]),int32_t(v[6]),v[7],v[8],v[9],v[10],v[11]);
 }
 for(auto &slot:houji_fb_rects){
  uint32_t n=__atomic_load_n(&slot.sequence,__ATOMIC_ACQUIRE),v[15];if(!n)continue;
  for(unsigned i=0;i<15;i++)v[i]=__atomic_load_n(&slot.values[i],__ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  if(n!=__atomic_load_n(&slot.sequence,__ATOMIC_ACQUIRE))continue;
  append("HOUJI DISPLAY SOURCE RECT: frame=%u layer=%u fmt=%x src=%d,%d,%u,%u dst=%d,%d,%u,%u flags=%u\n",
   v[0],v[1],v[5],int32_t(v[6]),int32_t(v[7]),v[8],v[9],int32_t(v[10]),int32_t(v[11]),v[12],v[13],v[14]);
 }
 return used;
}

extern "C" int native_adreno_present_begin_frame(uint32_t);
extern "C" uint32_t native_adreno_present_capture_fence(void);
static uint64_t houji_fb_last_wait_id,houji_fb_last_wait_mode,houji_fb_last_wait_ns;
static uint64_t houji_fb_wait_ticks;   /* total time SwapWait blocked */
struct HoujiDisplayFrame {
 uint8_t rec[SWEET_FB_SWAP_REC_SIZE]{};
 SweetSurface *surfaces[SWEET_FB_LAYERS]{};
 uint32_t id=0,fence=0;
};
extern "C" void native_adreno_present_end(void);
static bool houjiFramebufferAcquire(SweetSurface*s,uintptr_t src,int32_t sx,int32_t sy,int32_t w,int32_t h,unsigned line){
 if(OSDynamicCast(IOBufferMemoryDescriptor,s->memory)==nullptr)return true; /* other kernel views are already WC */
 uint64_t began=mach_absolute_time();
 for(int32_t y=0;y<h;y++){
  uintptr_t row=src+s->offset+(uint64_t)(sy+y)*s->bytesPerRow+(uint64_t)sx*4;HoujiSurfaceCacheRange range;
  if(!houji_surface_cache_range(src,(size_t)s->memory->getLength(),row,(size_t)w*4,line,&range)){
   __atomic_fetch_add(&houji_fb_cache_denied,1ull,__ATOMIC_RELAXED);return false;
  }
  for(uintptr_t address=range.begin;address<range.end;address+=line)__asm__ volatile("dc civac, %0"::"r"(address):"memory");
 }
 __asm__ volatile("dsb sy":::"memory");
 __atomic_fetch_add(&houji_fb_cache_bytes,(uint64_t)w*h*4,__ATOMIC_RELAXED);
 __atomic_fetch_add(&houji_fb_cache_ticks,mach_absolute_time()-began,__ATOMIC_RELAXED);return true;
}

#define SWEET_FB_FORMAT_BGRA            0x42475241

class IOMobileFramebuffer : public IOService {
	OSDeclareDefaultStructors(IOMobileFramebuffer);
};

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);

class IOMobileFramebufferUserClient;
class AppleCLCD;
static AppleCLCD *gHoujiPanel;

class AppleCLCD : public IOMobileFramebuffer {
	OSDeclareDefaultStructors(AppleCLCD);

	IOWorkLoop         *fWorkLoop;
	IOTimerEventSource *fVsync;
	IOWorkLoop         *fCopyWorkLoop;
	IOInterruptEventSource *fCopyEvent;
	IOTimerEventSource *fCopyRetry;   /* GPU not done yet: retry soon, not next tick */
	uint64_t            fVsyncCount;
	uint64_t            fVsyncPeriod, fNextVsync;

	static void vsyncFired(OSObject *owner, IOTimerEventSource *sender);
	static void copyFired(OSObject *owner, IOInterruptEventSource *sender, int count);
	static void copyRetryFired(OSObject *owner, IOTimerEventSource *sender);
	void copyQueued(void);

public:
	uint64_t      fPhys;
	uint32_t      fWidth, fHeight, fRowBytes;
	int32_t       fOffX, fOffY;
	uintptr_t     fPixels;          /* kernel mapping of the panel memory */
	IOMemoryMap  *fMap;
	SweetSurface *fSurface;         /* the panel memory as an IOSurface */
	IOLock       *fLock;            /* swaps and client list */
	OSArray      *fClients;
	uint32_t      fSwapID;
	bool          fConsolePaused;
	HoujiFrameQueue<HoujiDisplayFrame,8> fFrames;
	int           fGpuSync=0,fGpuCache=0,fImmediate=1;
	uint32_t      fCacheLine=64;
	uint32_t      fFlushTicks;    /* vsync ticks between panel copies */
	SweetSurface *fScanout;

	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void addClient(IOMobileFramebufferUserClient *client);
	void removeClient(IOMobileFramebufferUserClient *client);
	IOReturn swap(const uint8_t *rec);
	IOReturn waitSwap(uint32_t id,uint64_t timeoutRaw);
	bool present(const HoujiDisplayFrame &frame);
};

OSDefineMetaClassAndStructors(AppleCLCD, IOMobileFramebuffer);

class IOMobileFramebufferUserClient : public IOUserClient {
	OSDeclareDefaultStructors(IOMobileFramebufferUserClient);

public:
	AppleCLCD          *fFB;
	task_t              fTask;
	mach_port_t         fPort;
	io_user_reference_t fFunc[SWEET_FB_NOTIFY_TYPES];
	io_user_reference_t fRefcon[SWEET_FB_NOTIFY_TYPES];

	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn registerNotificationPort(mach_port_t port, UInt32 type,
	    io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
	void notify(uint32_t type, const uint64_t *values, uint32_t count);
};

OSDefineMetaClassAndStructors(IOMobileFramebufferUserClient, IOUserClient);

bool
AppleCLCD::start(IOService *provider)
{
	PE_Video video;

	if (!IOService::start(provider)) {
		return false;
	}
	PE_current_console(&video);
	fPhys = video.v_baseAddr;
	fWidth = (uint32_t)video.v_width;
	fHeight = (uint32_t)video.v_height;
	fRowBytes = (uint32_t)video.v_rowBytes;
	/* Only the independently observed Xiaomi 14 boot framebuffer. */
	if (fPhys != 0xfc400000ULL || fWidth != 1200 || fHeight != 2670 || fRowBytes != 4800) {
		IOLog("HOUJI FB: geometry guard refused\n");
		return false;
	}
	IOLog("HOUJI FB: geometry guard PASS\n");
	if (fPhys == 0 || fWidth == 0 || fHeight == 0 || fRowBytes < fWidth * 4) {
		IOLog("SWEET FB: no boot framebuffer\n");
		return false;
	}
	/* Native pixel canvas; owned render target is separate from scanout. */
	fOffX = fOffY = 0;
	fLock = IOLockAlloc();
	fClients = OSArray::withCapacity(4);
	fScanout = sweet_surface_create_physical(fPhys, fWidth, fHeight, fRowBytes, SWEET_FB_FORMAT_BGRA);
	fSurface = sweet_surface_create_buffer(HOUJI_LOGICAL_W,HOUJI_LOGICAL_H,(HOUJI_LOGICAL_W*4+63)&~63u,SWEET_FB_FORMAT_BGRA);
	fPixels = fScanout != NULL ? fScanout->kernelAddress() : 0;
	gHoujiPanel=this;
	fWorkLoop = IOWorkLoop::workLoop();
	if (fLock == NULL || fClients == NULL || fPixels == 0 || fWorkLoop == NULL || !fSurface) {
		IOLog("SWEET FB: setup failed\n");
		OSSafeReleaseNULL(fSurface);    /* stays published: also held by the table */
		OSSafeReleaseNULL(fWorkLoop);
		return false;
	}
	fVsync = IOTimerEventSource::timerEventSource(this, &AppleCLCD::vsyncFired);
	if (fVsync == NULL || fWorkLoop->addEventSource(fVsync) != kIOReturnSuccess) {
		IOLog("SWEET FB: no vsync timer\n");
		return false;
	}
	/* The notification timer must never wait for the GPU issuer mutex or the
	 * framebuffer copy. One event-driven consumer owns queue retirement; idle
	 * display periods do not wake a second polling thread. */
	fCopyWorkLoop = IOWorkLoop::workLoop();
	fCopyEvent = IOInterruptEventSource::interruptEventSource(this, &AppleCLCD::copyFired);
	if (!fCopyWorkLoop || !fCopyEvent || fCopyWorkLoop->addEventSource(fCopyEvent) != kIOReturnSuccess) {
		IOLog("HOUJI FB: asynchronous copy event setup failed\n");
		return false;
	}
	fCopyEvent->enable();
	fCopyRetry = IOTimerEventSource::timerEventSource(this, &AppleCLCD::copyRetryFired);
	if (!fCopyRetry || fCopyWorkLoop->addEventSource(fCopyRetry) != kIOReturnSuccess) {
		IOLog("HOUJI FB: copy retry timer setup failed\n");
		return false;
	}
	/* Copy each frame as soon as it is queued. The notification timer is not
	 * panel vblank, so waiting for its tick only added up to one period. */
	PE_parse_boot_argn("houji_fb_immediate",&fImmediate,sizeof(fImmediate));
	PE_parse_boot_argn("houji_fb_gpu_sync",&fGpuSync,sizeof(fGpuSync));
	PE_parse_boot_argn("houji_fb_gpu_cache",&fGpuCache,sizeof(fGpuCache));
	uint64_t ctr=0;__asm__ volatile("mrs %0, ctr_el0":"=r"(ctr));
	fCacheLine=4u<<((ctr>>16)&15);
	if(fCacheLine<16 || fCacheLine>2048)fGpuCache=0;
	uint32_t hz = 60;
	houji_fb_hz_arg_found=PE_parse_boot_argn("sweetfb_hz", &hz, sizeof(hz));
	houji_fb_config_hz=hz;
	houji_fb_command_line_bytes=PE_boot_args()?strnlen(PE_boot_args(),608):0;
	fFlushTicks = (hz == 0 || hz >= 60) ? 1 : 60 / hz;
	houji_fb_flush_every=fFlushTicks;
	const uint32_t notifyHz = hz == 120 ? 120 : 60;
	nanoseconds_to_absolutetime(NSEC_PER_SEC / notifyHz, &fVsyncPeriod);
	fNextVsync = mach_absolute_time() + fVsyncPeriod;
	houji_fb_started_at=mach_absolute_time();
	fVsyncCount = 0;
	fVsync->wakeAtTime(fNextVsync);

	setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");
	setProperty("external", false);
	setProperty("DisplayPipePlaneBaseAlignment", 64ULL, 32);
	/* Fixed software notification mode. Units match the IOMFB contracts used
	 * by the reference port: TimingElements rates are 16.16 Hz and refresh
	 * intervals are 32.32 seconds, not mach ticks. The panel remains in its
	 * inherited firmware mode; this property does not measure or switch it. */
	OSDictionary *horizontal=OSDictionary::withCapacity(1),*vertical=OSDictionary::withCapacity(3);
	OSDictionary *timing=OSDictionary::withCapacity(5),*refresh=OSDictionary::withCapacity(6);
	OSArray *timings=OSArray::withCapacity(1);
	if(!horizontal || !vertical || !timing || !refresh || !timings){
		OSSafeReleaseNULL(horizontal);OSSafeReleaseNULL(vertical);OSSafeReleaseNULL(timing);OSSafeReleaseNULL(refresh);OSSafeReleaseNULL(timings);
		return false;
	}
	struct NumberProperty {OSDictionary *dictionary;const char *key;uint64_t value;};
	uint64_t intervalMach=0;nanoseconds_to_absolutetime(NSEC_PER_SEC/notifyHz,&intervalMach);
	const NumberProperty modeNumbers[]={
		{horizontal,"Active",HOUJI_LOGICAL_W},{vertical,"Active",HOUJI_LOGICAL_H},
		{vertical,"SyncRate",(uint64_t)notifyHz<<16},{vertical,"PreciseSyncRate",(uint64_t)notifyHz<<16},
		{timing,"ID",0},{timing,"Score",100},
		{refresh,"displayMinRefreshInterval",(1ull<<32)/notifyHz},
		{refresh,"displayMaxRefreshInterval",(1ull<<32)/notifyHz},
		{refresh,"displayMinRefreshIntervalMachTime",intervalMach},
		{refresh,"displayMaxRefreshIntervalMachTime",intervalMach},
		{refresh,"displayRefreshStep",0},{refresh,"displayRefreshStepMachTime",0},
	};
	bool modeOK=true;
	for(const auto &v:modeNumbers){OSNumber*n=OSNumber::withNumber(v.value,v.dictionary==refresh?64:32);if(!n){modeOK=false;break;}modeOK=v.dictionary->setObject(v.key,n)&&modeOK;n->release();}
	modeOK=timing->setObject("IsVirtual",kOSBooleanFalse)&&modeOK;
	modeOK=timing->setObject("HorizontalAttributes",horizontal)&&modeOK;
	modeOK=timing->setObject("VerticalAttributes",vertical)&&modeOK;
	modeOK=timings->setObject(timing)&&modeOK;
	if(modeOK){modeOK=setProperty("TimingElements",timings)&&setProperty("IOMFBDisplayRefresh",refresh);}
	OSSafeReleaseNULL(horizontal);OSSafeReleaseNULL(vertical);OSSafeReleaseNULL(timing);OSSafeReleaseNULL(refresh);OSSafeReleaseNULL(timings);
	if(!modeOK)return false;
	registerService();
	houji_backlight_start(this);
	IOLog("SWEET FB: AppleCLCD %ux%u stride %u at 0x%llx, default surface %u\n",
	    fWidth, fHeight, fRowBytes, fPhys, fSurface->id);
	IOLog("HOUJI FB: native 1200x2670 canvas, owned render target, exact pixel copy; no resampling; touch stays normalized\n");
	IOLog("HOUJI FB: notification timer=%u Hz, asynchronous queued copy; inherited panel timing is not measured\n",notifyHz);
	return true;
}

/* Grayscale thumbnail of the panel for unattended UI checks over the USB debug console: one hex digit (4-bit luminance) per
 * cell, printed as SHOT rows. Reads the scanout memory only. */
extern "C" void houji_framebuffer_shot_region(unsigned x0,unsigned y0,unsigned w,unsigned h,unsigned cols,unsigned rows)
{
 AppleCLCD *fb=gHoujiPanel;
 if(!fb || !fb->fPixels || !cols || !rows || cols>300 || rows>700 || !w || !h || x0+w>fb->fWidth || y0+h>fb->fHeight){printf("HOUJI SHOT: unavailable\n");return;}
 printf("HOUJI SHOT BEGIN %u %u panel=%ux%u region=%u,%u,%u,%u\n",cols,rows,fb->fWidth,fb->fHeight,x0,y0,w,h);
 char line[320];
 for(unsigned r=0;r<rows;r++){
  const uint32_t y=y0+(uint32_t)(((uint64_t)r*h+h/rows/2)/rows);
  for(unsigned c=0;c<cols;c++){
   const uint32_t x=x0+(uint32_t)(((uint64_t)c*w+w/cols/2)/cols);
   const uint8_t *p=(const uint8_t *)(fb->fPixels+(uint64_t)y*fb->fRowBytes+(uint64_t)x*4);
   const unsigned l=(p[2]*3u+p[1]*6u+p[0])/10u;
   line[c]="0123456789abcdef"[l>>4];
  }
  line[cols]=0;printf("SHOT %03u %s\n",r,line);
 }
 printf("HOUJI SHOT END\n");
}
extern "C" void houji_framebuffer_shot(unsigned cols,unsigned rows)
{
 AppleCLCD *fb=gHoujiPanel;
 if(fb)houji_framebuffer_shot_region(0,0,fb->fWidth,fb->fHeight,cols,rows);
}

void
AppleCLCD::vsyncFired(OSObject *owner, IOTimerEventSource *sender)
{
	AppleCLCD *fb = (AppleCLCD *)owner;
	uint64_t now = mach_absolute_time();
	uint64_t last=__atomic_exchange_n(&houji_fb_last_notify_at,now,__ATOMIC_RELAXED);
	if(last){uint64_t gap=now-last,maximum=__atomic_load_n(&houji_fb_max_notify_gap,__ATOMIC_RELAXED);
	 while(gap>maximum && !__atomic_compare_exchange_n(&houji_fb_max_notify_gap,&maximum,gap,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)){}
	}
	uint64_t count = ++fb->fVsyncCount;
	__atomic_add_fetch(&houji_fb_callbacks,1ull,__ATOMIC_RELAXED);
	if(count%fb->fFlushTicks==0)__atomic_add_fetch(&houji_fb_due_ticks,1ull,__ATOMIC_RELAXED);
	uint64_t values[6] = {
		(uint32_t)count, (uint32_t)(count >> 32),
		(uint32_t)now, (uint32_t)(now >> 32),
		0, 0,
	};

	IOLockLock(fb->fLock);
	for (unsigned int i = 0; i < fb->fClients->getCount(); i++) {
		IOMobileFramebufferUserClient *client =
		    (IOMobileFramebufferUserClient *)fb->fClients->getObject(i);
		client->notify(SWEET_FB_NOTIFY_VSYNC, values, 6);
	}
 bool flush=fb->fFrames.count() && (count%fb->fFlushTicks)==0;
 IOLockUnlock(fb->fLock);
 if(flush)fb->fCopyEvent->interruptOccurred(nullptr,nullptr,0);

	// Absolute deadlines avoid adding the copy duration to every timer period.
	// Skip expired deadlines rather than emitting a burst of stale frames.
	now = mach_absolute_time();
	uint64_t next = fb->fNextVsync + fb->fVsyncPeriod;
	if (next <= now) {
		uint64_t skipped = (now - next) / fb->fVsyncPeriod + 1;
		next += skipped * fb->fVsyncPeriod;
		fb->fVsyncCount += skipped;
		__atomic_add_fetch(&houji_fb_missed_timer_ticks, skipped, __ATOMIC_RELAXED);
	}
	fb->fNextVsync = next;
	sender->wakeAtTime(next);
}

void
AppleCLCD::copyFired(OSObject *owner, __unused IOInterruptEventSource *sender, __unused int count)
{
 ((AppleCLCD *)owner)->copyQueued();
}

void
AppleCLCD::copyRetryFired(OSObject *owner, __unused IOTimerEventSource *sender)
{
 ((AppleCLCD *)owner)->copyQueued();
}

/* Runs only on fCopyWorkLoop. Drains the queue in order; a frame whose GPU
 * work is unfinished stays queued and is retried after 500 us. */
void
AppleCLCD::copyQueued(void)
{
 AppleCLCD *fb=this;
 __atomic_add_fetch(&houji_fb_copy_wakes,1ull,__ATOMIC_RELAXED);
 const uint64_t began=mach_absolute_time();
 for(unsigned n=0;n<8;n++){
  IOLockLock(fb->fLock);
  const HoujiDisplayFrame *head=fb->fFrames.front();
  if(!head){IOLockUnlock(fb->fLock);break;}
  HoujiDisplayFrame frame=*head; /* queue retains every surface until real copy completion */
  IOLockUnlock(fb->fLock);
  if(!fb->present(frame)){if(fb->fImmediate)fb->fCopyRetry->setTimeoutUS(500);break;}
  IOLockLock(fb->fLock);
  const bool retired=fb->fFrames.finish(frame.id);
  if(retired){__atomic_fetch_add(&houji_fb_retired,1ull,__ATOMIC_RELAXED);IOLockWakeup(fb->fLock,&fb->fFrames,false);}
  IOLockUnlock(fb->fLock);
  if(retired)for(auto *surface:frame.surfaces)if(surface)surface->release();
  if(!retired)break;
 }
 const uint64_t elapsed=mach_absolute_time()-began;
 __atomic_fetch_add(&houji_fb_copy_worker_ticks,elapsed,__ATOMIC_RELAXED);
 uint64_t maximum=__atomic_load_n(&houji_fb_copy_worker_max_ticks,__ATOMIC_RELAXED);
 while(elapsed>maximum && !__atomic_compare_exchange_n(&houji_fb_copy_worker_max_ticks,&maximum,elapsed,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)){}
}

void
AppleCLCD::addClient(IOMobileFramebufferUserClient *client)
{
	IOLockLock(fLock);
	fClients->setObject(client);
	IOLockUnlock(fLock);
}

void
AppleCLCD::removeClient(IOMobileFramebufferUserClient *client)
{
	IOLockLock(fLock);
	unsigned int i = fClients->getNextIndexOfObject(client, 0);
	if (i != (unsigned int)-1) {
		fClients->removeObject(i);
	}
	IOLockUnlock(fLock);
}

static uint32_t
get32(const uint8_t *p, uint32_t off)
{
	uint32_t v;
	memcpy(&v, p + off, 4);
	return v;
}

/* Intervals between consecutive SwapEnd calls: <10, 10-14, 14-20 (60 fps), 20-25, 25-36 (30 fps), 36-50, >=50 ms (idle gap).
 * Gaps under 50 ms count as animation; their total time gives the frame rate while something moves. */
static uint64_t houji_fb_interval_hist[7],houji_fb_anim_frames,houji_fb_anim_ticks,houji_fb_last_swap;
extern "C" void native_adreno_hitch_snapshot(uint64_t out[16]);
static uint32_t houji_fb_hitch_logged;
/* SwapEnd is serialized per client; backboardd is the only one, so the previous snapshot needs no lock. */
static void houjiNoteSwapInterval(void){
 const uint64_t now=mach_absolute_time(),last=__atomic_exchange_n(&houji_fb_last_swap,now,__ATOMIC_RELAXED);
 static uint64_t previous[16];uint64_t snap[16];
 native_adreno_hitch_snapshot(snap);
 if(!last){memcpy(previous,snap,sizeof(snap));return;}
 uint64_t ns=0;absolutetime_to_nanoseconds(now-last,&ns);const uint64_t ms10=ns/100000; /* 0.1 ms */
 static uint64_t previousWait;const uint64_t waitNow=__atomic_load_n(&houji_fb_wait_ticks,__ATOMIC_RELAXED);
 uint64_t waitNs=0;absolutetime_to_nanoseconds(waitNow-previousWait,&waitNs);previousWait=waitNow;
 if(ms10>=200 && ms10<500 && houji_fb_hitch_logged<40){
  IOLog("HOUJI HITCH SWAP: gap-ms=%llu.%llu swapwait-us=%llu\n",ms10/10,ms10%10,waitNs/1000);
  ++houji_fb_hitch_logged;uint64_t d[16];for(unsigned i=0;i<16;i++)d[i]=snap[i]-previous[i];
  IOLog("HOUJI HITCH: gap-ms=%llu.%llu gate n=%llu us=%llu wait=%llu audit=%llu GPU level=%llu busy-us=%llu | map n=%llu us=%llu free n=%llu us=%llu submit n=%llu us=%llu alloc=%llu fence-wait-us=%llu full=%llu depth=%llu\n",
   ms10/10,ms10%10,d[0],d[1],d[2],d[3],snap[4],d[5],d[6],d[7],d[8],d[9],d[10],d[11],d[12],d[13],d[14],d[15]);
 }
 memcpy(previous,snap,sizeof(snap));
 static const uint64_t edge[6]={100,140,200,250,360,500};
 unsigned b=0;while(b<6 && ms10>=edge[b])++b;
 __atomic_fetch_add(&houji_fb_interval_hist[b],1ull,__ATOMIC_RELAXED);
 if(b<6){__atomic_fetch_add(&houji_fb_anim_frames,1ull,__ATOMIC_RELAXED);__atomic_fetch_add(&houji_fb_anim_ticks,now-last,__ATOMIC_RELAXED);}
}
static uint32_t sweet_fb_swaps_logged;
extern "C" void houji_framebuffer_history_report(void) {
 houjiReportColours();
 const uint32_t end=__atomic_load_n(&houji_fb_history_count,__ATOMIC_ACQUIRE);
 const uint32_t first=end>40?end-39:1;
 for(uint32_t n=first;n<=end;n++){
  auto &s=houji_fb_history[(n-1)%40];uint64_t v[7];
  if(__atomic_load_n(&s.sequence,__ATOMIC_ACQUIRE)!=n)continue;
  for(unsigned i=0;i<7;i++)v[i]=__atomic_load_n(&s.values[i],__ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  if(__atomic_load_n(&s.sequence,__ATOMIC_ACQUIRE)!=n)continue;
  IOLog("HOUJI DISPLAY HISTORY: sample=%u up-ms=%llu window-ms=%llu swaps=%llu presents=%llu copy-us=%llu max-us=%llu missed=%llu; CPU copy windows, not panel FPS\n",n,v[0],v[1],v[2],v[3],v[4],v[5],v[6]);
 }
}
extern "C" void houji_framebuffer_counts(uint64_t *swaps, uint64_t *presents)
{
	if (swaps) *swaps = __atomic_load_n(&sweet_fb_swap_count, __ATOMIC_RELAXED);
	if (presents) *presents = __atomic_load_n(&houji_fb_present_count, __ATOMIC_RELAXED);
}

extern "C" void houji_framebuffer_report(void)
{
	static uint64_t previousTime, previousPresents, previousSwaps, previousTicks, previousBytes;
	uint64_t now = mach_absolute_time();
	uint64_t presents = __atomic_load_n(&houji_fb_present_count, __ATOMIC_RELAXED);
	uint64_t swaps = __atomic_load_n(&sweet_fb_swap_count, __ATOMIC_RELAXED);
	uint64_t ticks = __atomic_load_n(&houji_fb_present_ticks, __ATOMIC_RELAXED);
	uint64_t bytes = __atomic_load_n(&houji_fb_copy_bytes, __ATOMIC_RELAXED);
	uint64_t windowNs = 0, presentNs = 0, maximumNs = 0;
	absolutetime_to_nanoseconds(previousTime ? now - previousTime : 0, &windowNs);
	absolutetime_to_nanoseconds(ticks - previousTicks, &presentNs);
	absolutetime_to_nanoseconds(__atomic_load_n(&houji_fb_max_present_ticks, __ATOMIC_RELAXED), &maximumNs);
	uint64_t delta = presents - previousPresents;
	if(previousTime && windowNs){
	 uint64_t upNs=0;absolutetime_to_nanoseconds(now,&upNs);
	 const uint64_t v[7]={upNs/NSEC_PER_MSEC,windowNs/NSEC_PER_MSEC,swaps-previousSwaps,delta,delta?presentNs/NSEC_PER_USEC/delta:0,maximumNs/NSEC_PER_USEC,__atomic_load_n(&houji_fb_missed_timer_ticks,__ATOMIC_RELAXED)};
	 const uint32_t n=__atomic_add_fetch(&houji_fb_history_count,1u,__ATOMIC_RELAXED);auto &slot=houji_fb_history[(n-1)%40];
	 __atomic_store_n(&slot.sequence,0u,__ATOMIC_RELEASE);
 __atomic_thread_fence(__ATOMIC_RELEASE);
	 for(unsigned i=0;i<7;i++)__atomic_store_n(&slot.values[i],v[i],__ATOMIC_RELAXED);
	 __atomic_store_n(&slot.sequence,n,__ATOMIC_RELEASE);
	}
	IOLog("HOUJI DISPLAY CONFIG: arg-found=%u hz=%u copy-every=%u cmdline-bytes=%llu callbacks=%llu due=%llu; configured timer, not panel vblank\n",houji_fb_hz_arg_found,houji_fb_config_hz,houji_fb_flush_every,(uint64_t)houji_fb_command_line_bytes,__atomic_load_n(&houji_fb_callbacks,__ATOMIC_RELAXED),__atomic_load_n(&houji_fb_due_ticks,__ATOMIC_RELAXED));
	uint64_t workerNs=0,workerMaxNs=0;
	absolutetime_to_nanoseconds(__atomic_load_n(&houji_fb_copy_worker_ticks,__ATOMIC_RELAXED),&workerNs);
	absolutetime_to_nanoseconds(__atomic_load_n(&houji_fb_copy_worker_max_ticks,__ATOMIC_RELAXED),&workerMaxNs);
	IOLog("HOUJI DISPLAY ASYNC: copy-wakes=%llu worker-us=%llu max-worker-us=%llu; GPU gate and copy waits excluded from notification timer\n",__atomic_load_n(&houji_fb_copy_wakes,__ATOMIC_RELAXED),workerNs/1000,workerMaxNs/1000);
	IOLog("HOUJI DISPLAY PERF: window-ms=%llu swaps=%llu presents=%llu present-fps-milli=%llu bytes=%llu avg-present-us=%llu max-present-us=%llu missed-timer-ticks=%llu; CPU-copy, not GPU or measured panel refresh\n",
	    windowNs / NSEC_PER_MSEC, swaps - previousSwaps, delta,
	    windowNs ? delta * 1000000000000ULL / windowNs : 0,
	    bytes - previousBytes, delta ? presentNs / NSEC_PER_USEC / delta : 0,
	    maximumNs / NSEC_PER_USEC,
	    __atomic_load_n(&houji_fb_missed_timer_ticks, __ATOMIC_RELAXED));
	uint64_t cacheNs=0;absolutetime_to_nanoseconds(__atomic_load_n(&houji_fb_cache_ticks,__ATOMIC_RELAXED),&cacheNs);
	IOLog("HOUJI DISPLAY SYNC: deferred=%llu cache-bytes=%llu cache-us=%llu cache-denied=%llu; completed-GPU surface reads\n",__atomic_load_n(&houji_fb_sync_deferred,__ATOMIC_RELAXED),__atomic_load_n(&houji_fb_cache_bytes,__ATOMIC_RELAXED),cacheNs/1000,__atomic_load_n(&houji_fb_cache_denied,__ATOMIC_RELAXED));
	{
	 static uint64_t prevHist[7],prevFrames,prevTicks;uint64_t h[7],frames,ticks,ns=0;
	 for(unsigned i=0;i<7;i++){h[i]=__atomic_load_n(&houji_fb_interval_hist[i],__ATOMIC_RELAXED);}
	 frames=__atomic_load_n(&houji_fb_anim_frames,__ATOMIC_RELAXED);ticks=__atomic_load_n(&houji_fb_anim_ticks,__ATOMIC_RELAXED);
	 absolutetime_to_nanoseconds(ticks-prevTicks,&ns);
	 IOLog("HOUJI FRAME PACE: <10=%llu 10-14=%llu 14-20=%llu 20-25=%llu 25-36=%llu 36-50=%llu idle=%llu animating-fps-milli=%llu animating-ms=%llu; SwapEnd intervals this window\n",
	  h[0]-prevHist[0],h[1]-prevHist[1],h[2]-prevHist[2],h[3]-prevHist[3],h[4]-prevHist[4],h[5]-prevHist[5],h[6]-prevHist[6],
	  ns?(frames-prevFrames)*1000000000000ULL/ns:0,ns/1000000);
	 for(unsigned i=0;i<7;i++)prevHist[i]=h[i];prevFrames=frames;prevTicks=ticks;
	 houji_fb_hitch_logged=0;
	}
	houji_surface_use_report();
	houjiReportRects();
	{uint64_t ns=0;absolutetime_to_nanoseconds(__atomic_load_n(&houji_fb_wait_ticks,__ATOMIC_RELAXED),&ns);IOLog("HOUJI SWAP WAIT: blocked-us=%llu\n",ns/1000);}
	IOLog("HOUJI FRAME LIFE: retired=%llu waits=%llu blocked=%llu wait-timeout=%llu queue-full=%llu peak=%llu wait-id=%llu wait-mode=%llu wait-ns=%llu; real CPU copy/store completion, not panel vblank\n",__atomic_load_n(&houji_fb_retired,__ATOMIC_RELAXED),__atomic_load_n(&houji_fb_waits,__ATOMIC_RELAXED),__atomic_load_n(&houji_fb_wait_blocked,__ATOMIC_RELAXED),__atomic_load_n(&houji_fb_wait_timeout,__ATOMIC_RELAXED),__atomic_load_n(&houji_fb_queue_full,__ATOMIC_RELAXED),__atomic_load_n(&houji_fb_queue_peak,__ATOMIC_RELAXED),__atomic_load_n(&houji_fb_last_wait_id,__ATOMIC_RELAXED),__atomic_load_n(&houji_fb_last_wait_mode,__ATOMIC_RELAXED),__atomic_load_n(&houji_fb_last_wait_ns,__ATOMIC_RELAXED));
	previousTime = now; previousPresents = presents; previousSwaps = swaps;
	previousTicks = ticks; previousBytes = bytes;
}


/* Keep each accepted surface alive, and retire only after its actual CPU
 * copy/store barrier. Unlike the old latest-record slot, later SwapEnd calls
 * cannot silently overwrite an earlier request that SwapWait is waiting for. */
IOReturn
AppleCLCD::swap(const uint8_t *rec)
{
 HoujiDisplayFrame frame{};memcpy(frame.rec,rec,sizeof(frame.rec));
 frame.id=get32(rec,0x50);uint32_t mask=get32(rec,0x104);
 for(unsigned layer=0;layer<SWEET_FB_LAYERS;++layer){
  uint32_t id=get32(rec,0x54+4*layer);
  if((mask&(1u<<layer)) && id){
   frame.surfaces[layer]=sweet_surface_copy(id);
   if(!frame.surfaces[layer]){
    for(auto *surface:frame.surfaces)if(surface)surface->release();
    return kIOReturnBadArgument;
   }
  }
 }
 frame.fence=native_adreno_present_capture_fence(); /* never while holding fLock */
 uint64_t ticks=0;nanoseconds_to_absolutetime(500000000ull,&ticks);uint64_t deadline=mach_absolute_time()+ticks;
 IOLockLock(fLock);
 while(fFrames.full() && mach_absolute_time()<deadline){
  __atomic_fetch_add(&houji_fb_queue_full,1ull,__ATOMIC_RELAXED);
  if(IOLockSleepDeadline(fLock,&fFrames,deadline,THREAD_ABORTSAFE)!=THREAD_AWAKENED)break;
 }
 bool queued=fFrames.push(frame);unsigned depth=fFrames.count();
 if(depth>__atomic_load_n(&houji_fb_queue_peak,__ATOMIC_RELAXED))__atomic_store_n(&houji_fb_queue_peak,(uint64_t)depth,__ATOMIC_RELAXED);
 IOLockUnlock(fLock);
 if(!queued){for(auto *surface:frame.surfaces)if(surface)surface->release();return kIOReturnTimeout;}
 __atomic_fetch_add(&sweet_fb_swap_count,1ull,__ATOMIC_RELAXED);
 houjiNoteSwapInterval();
 if(fImmediate)fCopyEvent->interruptOccurred(nullptr,nullptr,0);
 return kIOReturnSuccess;
}

IOReturn
AppleCLCD::waitSwap(uint32_t id,uint64_t timeoutRaw)
{
 __atomic_fetch_add(&houji_fb_waits,1ull,__ATOMIC_RELAXED);
 /* 20H392 selector6 has three scalars; the timed frontend multiplies its
  * public floating timeout by 1000. Default 0 waits for retirement, bounded
  * here to 500 ms for diagnosis. Nonzero values are treated as milliseconds
  * provisionally and logged raw; no timeout ever reports false completion. */
 uint64_t ms=timeoutRaw && timeoutRaw<500?timeoutRaw:500;uint64_t ns=ms*1000000ull;
 uint64_t ticks=0;nanoseconds_to_absolutetime(ns,&ticks);uint64_t deadline=mach_absolute_time()+ticks;
 IOLockLock(fLock);
 if(!id){if(!fFrames.count()){IOLockUnlock(fLock);return kIOReturnSuccess;}id=fFrames.latest();}
 bool blocked=false;
 while(!fFrames.complete(id) && mach_absolute_time()<deadline){
  if(!blocked){blocked=true;__atomic_fetch_add(&houji_fb_wait_blocked,1ull,__ATOMIC_RELAXED);}
  if(IOLockSleepDeadline(fLock,&fFrames,deadline,THREAD_ABORTSAFE)!=THREAD_AWAKENED)break;
 }
 bool done=fFrames.complete(id);IOLockUnlock(fLock);
 if(blocked)__atomic_fetch_add(&houji_fb_wait_ticks,mach_absolute_time()-(deadline-ticks),__ATOMIC_RELAXED);
 if(!done)__atomic_fetch_add(&houji_fb_wait_timeout,1ull,__ATOMIC_RELAXED);
 return done?kIOReturnSuccess:kIOReturnTimeout;
}

bool
AppleCLCD::present(const HoujiDisplayFrame &frame)
{
 const uint8_t *rec=frame.rec;
 int token=fGpuSync?native_adreno_present_begin_frame(frame.fence):0;
 if(token<0){__atomic_fetch_add(&houji_fb_sync_deferred,1ull,__ATOMIC_RELAXED);return false;}
 struct UnlockGpu {int token;~UnlockGpu(){if(token>0)native_adreno_present_end();}} unlock{token};
	uint32_t mask = get32(rec, 0x104);
	uint64_t started = mach_absolute_time(), copied = 0;


	if (!fConsolePaused) {
		int quiet = 0;
		fConsolePaused = true;
		if (PE_parse_boot_argn("sweetfb_noconsole", &quiet, sizeof(quiet)) && quiet != 0) {
			IOLog("HOUJI FB: first present; muting panel text only\n");
			houji_console_muted = 1;
			bzero((void *)fPixels, (size_t)fRowBytes * fHeight);
		}
	}

	for (uint32_t layer = 0; layer < SWEET_FB_LAYERS; layer++) {
		uint32_t id = get32(rec, 0x54 + 4 * layer);
		const uint8_t *a = rec + 0x64 + 16 * layer;
		const uint8_t *b = rec + 0xc4 + 16 * layer;
		int32_t sx = (int32_t)get32(a, 0), sy = (int32_t)get32(a, 4);
		int32_t dx = (int32_t)get32(b, 0), dy = (int32_t)get32(b, 4);
		int32_t w = (int32_t)get32(b, 8), h = (int32_t)get32(b, 12);

		if (!(mask & (1u << layer)) || id == 0) {
			continue;
		}
		SweetSurface *s = frame.surfaces[layer];
		if(s)houjiObserveRect(frame.id,layer,id,s->width,s->height,s->pixelFormat,a,b);
		if(s)s->retain();
		if (sweet_fb_swaps_logged < 16) {
			sweet_fb_swaps_logged++;
			IOLog("SWEET FB: swap %u layer %u surface %u %ux%u fmt 0x%x bpr %u a %d,%d %ux%u b %d,%d %dx%d\n",
			    get32(rec, 0x50), layer, id, s ? s->width : 0, s ? s->height : 0,
			    s ? s->pixelFormat : 0, s ? s->bytesPerRow : 0, sx, sy, get32(a, 8),
			    get32(a, 12), dx, dy, w, h);
		}
		if (s == NULL || s == fScanout) {
			OSSafeReleaseNULL(s);
			continue;
		}
		uintptr_t src = s->kernelAddress();
		uint32_t bpe = s->bytesPerElement ? s->bytesPerElement : 4;
		if (w <= 0 || h <= 0) {
			w = (int32_t)s->width;
			h = (int32_t)s->height;
		}
		if (sx < 0 || sy < 0) {
			sx = sy = 0;
		}
		if (dx < 0 || dy < 0) {
			dx = dy = 0;
		}
		/* Bound logical coordinates before adding the physical panel origin. */
		if (sx >= (int32_t)s->width || sy >= (int32_t)s->height ||
		    dx >= HOUJI_LOGICAL_W || dy >= HOUJI_LOGICAL_H) {
			s->release();
			continue;
		}
		if (w > HOUJI_LOGICAL_W - dx) {
			w = HOUJI_LOGICAL_W - dx;
		}
		if (h > HOUJI_LOGICAL_H - dy) {
			h = HOUJI_LOGICAL_H - dy;
		}
		dx -= fOffX;
		dy -= fOffY;
		if (dx < 0) { sx -= dx; w += dx; dx = 0; }
		if (dy < 0) { sy -= dy; h += dy; dy = 0; }
		if (w > (int32_t)s->width - sx) {
			w = (int32_t)s->width - sx;
		}
		if (h > (int32_t)s->height - sy) {
			h = (int32_t)s->height - sy;
		}
		if (w > (int32_t)fWidth - dx) {
			w = (int32_t)fWidth - dx;
		}
		if (h > (int32_t)fHeight - dy) {
			h = (int32_t)fHeight - dy;
		}
		uint64_t last = (uint64_t)s->offset + (uint64_t)(sy + h - 1) * s->bytesPerRow +
		    (uint64_t)(sx + w) * 4;
		if (w <= 0 || h <= 0 || s->elementWidth != 1 || s->elementHeight != 1 ||
		    s->memory == NULL || last > s->memory->getLength()) {
			src = 0;
		}
		if (src != 0 && bpe == 4) {
			if(fGpuCache && token>0 && !houjiFramebufferAcquire(s,src,sx,sy,w,h,fCacheLine)){s->release();return false;}
			if(token>0)houjiObserveColours(frame.id,layer,s,src,sx,sy,w,h,dx,dy);
			copied += (uint64_t)w * h * 4;
			for(int32_t y=0;y<h;y++)
			 memcpy((void *)(fPixels+(uint64_t)(dy+y)*fRowBytes+(uint64_t)dx*4),
			  (const void *)(src+s->offset+(uint64_t)(sy+y)*s->bytesPerRow+(uint64_t)sx*4),(size_t)w*4);
		}
		s->release();
	}
 __asm__ volatile("dsb sy":::"memory"); /* all scanout stores before retirement */
	uint64_t elapsed = mach_absolute_time() - started;
	__atomic_add_fetch(&houji_fb_present_count,1ull,__ATOMIC_RELAXED);
	__atomic_add_fetch(&houji_fb_copy_bytes, copied, __ATOMIC_RELAXED);
	__atomic_add_fetch(&houji_fb_present_ticks, elapsed, __ATOMIC_RELAXED);
	uint64_t maximum = __atomic_load_n(&houji_fb_max_present_ticks, __ATOMIC_RELAXED);
	while (elapsed > maximum && !__atomic_compare_exchange_n(&houji_fb_max_present_ticks,
	    &maximum, elapsed, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}

 return true;
}

bool
IOMobileFramebufferUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
IOMobileFramebufferUserClient::start(IOService *provider)
{
	fFB = OSDynamicCast(AppleCLCD, provider);
	if (fFB == NULL || !IOUserClient::start(provider)) {
		return false;
	}
	fFB->addClient(this);
	return true;
}

IOReturn
IOMobileFramebufferUserClient::clientClose(void)
{
	if (fFB != NULL) {
		fFB->removeClient(this);
		IOLockLock(fFB->fLock);
		mach_port_t old = fPort;
		fPort = MACH_PORT_NULL;
		IOLockUnlock(fFB->fLock);
		if (old != MACH_PORT_NULL) {
			releaseNotificationPort(old);
		}
	}
	terminate();
	return kIOReturnSuccess;
}

IOReturn
IOMobileFramebufferUserClient::registerNotificationPort(mach_port_t port, __unused UInt32 type,
    __unused io_user_reference_t refCon)
{
	mach_port_t old;
	IOLockLock(fFB->fLock);
	old = fPort;
	fPort = port;
	IOLockUnlock(fFB->fLock);
	if (old != MACH_PORT_NULL) {
		releaseNotificationPort(old);
	}
	return kIOReturnSuccess;
}

/* Called with the framebuffer lock held. */
void
IOMobileFramebufferUserClient::notify(uint32_t type, const uint64_t *values, uint32_t count)
{
	OSAsyncReference64 ref;

	if (type >= SWEET_FB_NOTIFY_TYPES || fFunc[type] == 0 || fPort == MACH_PORT_NULL) {
		return;
	}
	bzero(ref, sizeof(ref));
	setAsyncReference64(ref, fPort, (mach_vm_address_t)fFunc[type], fRefcon[type], fTask);
	sendAsyncResult64WithOptions(ref, kIOReturnSuccess, (io_user_reference_t *)values, count,
	    kIOUserNotifyOptionCanDrop);
}

static uint64_t sweet_fb_logged;        /* selectors already logged (< 64) */
extern "C" void houji_screen_power(int on);   /* houji-buttons/HoujiScreen.cpp */

IOReturn
IOMobileFramebufferUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target,
    __unused void *reference)
{
	IOReturn ret = kIOReturnSuccess;
	uint64_t out[2] = { 0, 0 };
	uint32_t outCount = 0;

	switch (selector) {
	case kSweetFBGetLayerDefaultSurface:
		out[0] = sweet_surface_port_in_task(fFB->fSurface, fTask);
		outCount = 1;
		break;
	case kSweetFBSwapBegin:
		IOLockLock(fFB->fLock);
		out[0] = ++fFB->fSwapID;
		IOLockUnlock(fFB->fLock);
		outCount = 1;
		break;
	case kSweetFBSwapEnd: {
		uint8_t rec[SWEET_FB_SWAP_REC_SIZE];
		if (args->structureInputDescriptor != NULL ||
		    args->structureInputSize < SWEET_FB_SWAP_REC_SIZE) {
			ret = kIOReturnBadArgument;
			break;
		}
		memcpy(rec, args->structureInput, sizeof(rec));
		ret=fFB->swap(rec);
		break;
	}
	case 12:   /* display power from backboardd: scalar 0 off, 1 on (every power-key sleep/wake and auto-lock) */
		if (args->scalarInputCount >= 1) {
			houji_screen_power(args->scalarInput[0] != 0);
		}
		break;
	case kSweetFBGetID:
		out[0] = 0;
		outCount = 1;
		break;
	case kSweetFBGetDisplaySize:
		out[0] = HOUJI_LOGICAL_W;
		out[1] = HOUJI_LOGICAL_H;
		outCount = 2;
		break;
	case kSweetFBIsMainDisplay:
		out[0] = 1;
		outCount = 1;
		break;
	case kSweetFBGetDotPitch:
		out[0] = SWEET_FB_DOT_PITCH;
		outCount = 1;
		break;
	case kSweetFBNotifications:
		if (args->scalarInputCount >= 3 && args->scalarInput[2] < SWEET_FB_NOTIFY_TYPES) {
			uint32_t type = (uint32_t)args->scalarInput[2];
			IOLockLock(fFB->fLock);
			fFunc[type] = args->scalarInput[0];
			fRefcon[type] = args->scalarInput[1];
			IOLockUnlock(fFB->fLock);
			char name[32];
			proc_selfname(name, sizeof(name));
			IOLog("SWEET FB: %s notifications type %u %s\n", name, type,
			    args->scalarInput[0] ? "on" : "off");
		}
		break;
	case kSweetFBSwapWait:
		if(args->scalarInputCount!=3){ret=kIOReturnBadArgument;break;}
		__atomic_store_n(&houji_fb_last_wait_id,args->scalarInput[0],__ATOMIC_RELAXED);
		__atomic_store_n(&houji_fb_last_wait_mode,args->scalarInput[1],__ATOMIC_RELAXED);
		__atomic_store_n(&houji_fb_last_wait_ns,args->scalarInput[2],__ATOMIC_RELAXED);
		ret=fFB->waitSwap((uint32_t)args->scalarInput[0],args->scalarInput[2]);
		break;
	default:
		break;
	}

	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = i < outCount ? out[i] : 0;
	}
	if (args->structureOutputDescriptor == NULL && args->structureOutput != NULL) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	/* Display power and other state changes: a call whose scalars or struct size differ from that selector's last one is logged
	 * (the frame path, selectors 3-6, excluded). */
	if (selector < 96 && (selector < 3 || selector > 6)) {
		static uint64_t lastSig[96];
		uint64_t sig = 0x9e3779b97f4a7c15ULL * (args->scalarInputCount + 1) ^ args->structureInputSize;
		for (uint32_t i = 0; i < args->scalarInputCount && i < 4; i++) sig = (sig ^ args->scalarInput[i]) * 0x100000001b3ULL;
		if (args->structureInput && args->structureInputSize >= 4 && args->structureInputSize <= 16) {
			uint32_t w; memcpy(&w, args->structureInput, 4); sig = (sig ^ w) * 0x100000001b3ULL;
		}
		if (lastSig[selector] != sig) {
			const bool first = !lastSig[selector];
			lastSig[selector] = sig;
			if (!first || selector >= 64 || (sweet_fb_logged & (1ULL << selector))) {
				char name[32];
				proc_selfname(name, sizeof(name));
				uint32_t w = 0;
				if (args->structureInput && args->structureInputSize >= 4) memcpy(&w, args->structureInput, 4);
				IOLog("SWEET FB CHANGE: %s sel %u scalars %u [%llx %llx %llx %llx] struct %u first-word %x\n", name, selector,
				    args->scalarInputCount, args->scalarInputCount > 0 ? args->scalarInput[0] : 0,
				    args->scalarInputCount > 1 ? args->scalarInput[1] : 0, args->scalarInputCount > 2 ? args->scalarInput[2] : 0,
				    args->scalarInputCount > 3 ? args->scalarInput[3] : 0, args->structureInputSize, w);
			}
		}
	}
	if (selector >= 64 || !(sweet_fb_logged & (1ULL << selector))) {
		if (selector < 64) {
			sweet_fb_logged |= 1ULL << selector;
		}
		char name[32];
		proc_selfname(name, sizeof(name));
		IOLog("SWEET FB: %s sel %u in %u/%u out %u/%u -> 0x%x\n", name, selector,
		    args->scalarInputCount, args->structureInputSize,
		    args->scalarOutputCount, args->structureOutputSize, ret);
	}
	return ret;
}
#endif /* SWEET */
