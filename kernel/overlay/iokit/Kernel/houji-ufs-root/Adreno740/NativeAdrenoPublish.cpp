/* SPDX-License-Identifier: MIT */
/* IOKit surface for an already-running GPU context. The board code (kits/sheng) brings the GPU up, then hands this file
 * the live VM, submit object, ring, clock and optional timer; one IOService is published and each IOServiceOpen creates a
 * connection that mounts its own NativeAdrenoUserClient core. Up to NativeAdrenoMaxClients connections (one per Metal process) share
 * the VM, the submit object and the ring: each core has its own handles, its own queue range on the ring and its own window of the
 * one GPU address space (the render server the large one), and closing a connection waits for its own jobs only, then unmaps and
 * frees everything it created. Not yet done: per-process GPU page tables (the windows are enforced by the driver, not by the SMMU)
 * and a reset path. This is not the Metal or Turnip ABI; it carries NativeAdrenoABI.h unchanged.
 * BO CPU mappings are handed out as uncached physical-range mappings to match the GPU's non-cacheable PTEs.
 * The connection class is named AGXDeviceUserClient on purpose: the original sandbox profiles of the processes that use Metal
 * allow IOServiceOpen for that user-client class name, and nothing else in this system can create one (the Apple AGX kext is
 * never loaded without an Apple GPU), so the name only decides whether the Adreno Metal plugin may open its own driver. */
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOUserClient.h>
#include <string.h>
#include "../HoujiGpuAddressLayout.h"
#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <pexpert/pexpert.h>
#include "NativeAdrenoIssuerTrace.h"
#include "NativeAdrenoUserClient.h"

extern "C" IOReturn native_adreno_board_quiesce(void);
extern "C" IOReturn native_adreno_board_prepare_submit(void);
extern "C" void native_adreno_board_teardown_checkpoint(const char*,uint32_t,uint32_t);
static_assert(__is_same(decltype(((IOUserClient*)nullptr)->removeMappingForDescriptor(nullptr)),IOMemoryMap*),"This integrated XNU uses raw OSPtr; map references require explicit release");
extern "C" bool native_adreno_board_va_window(uint64_t,uint64_t);
extern "C" const NativeAdrenoStatus*native_adreno_board_status(void);
extern "C" bool native_adreno_board_running(void);
extern "C" void proc_selfname(char *, int);
/* The plugin's telemetry table (rows of text the Metal bridge keeps), sent as markers: 0xC000|row starts a row, 0x8000|(a<<7)|b appends two characters. */
extern "C" {char native_adreno_metal_rows[48][96];volatile uint32_t native_adreno_metal_rows_seq;}

extern "C" {
/* What the processes that reached this driver did, for the boot diagnostic page. Plain counters and short strings. */
struct NativeAdrenoMetalDiag {
    uint32_t opens, busy, bad, closes, reports, property_reads, foreign_opens, foreign_type, name_reads, calls, last_selector, last_status, ext_entries, ext_selector, ext_status, ext_in, ext_out, tunnel_calls, probes, sel_bits[8], mb_maps, mb_calls, marks_total, marks[16], chk_macf, chk_res, chk_type, type_pass_lo, type_pass_hi, attach_status, inflight, inflight_since_calls, forced_unmaps;
    char opener[3][17];      /* process names of the latest connections, newest first */
    char reader[3][17];      /* process names that read a registry property of the accelerator, newest first */
    char name_reader[17];    /* latest process that read MetalPluginName specifically */
    char starter[3][17];     /* processes that sent plugin marker 1 (the plugin's initWithAcceleratorPort: started), newest first */
    char report[64];         /* the plugin's own result line (SetMetadata text), latest */
    char report2[64];        /* the plugin's Turnip-phase line (text starting "T " or "W "), latest */
    uint32_t grants;         /* opens granted to the render server by this driver in place of the sandbox verdict (the view in GpuPowerProbe.cpp ends before this) */
} native_adreno_metal_diag;
/* BO CPU views (clientMemoryForType for a BO token): [0] calls, [1] new descriptors, [2] served from the view cache, [3] refused (not a BO / out of
 * resources), [4] last refusal's IOReturn, [5] threads inside now. int-115 showed a backboardd thread in IOConnectMapMemory64 ~650 times a second. */
extern "C" {volatile uint32_t native_adreno_view_diag[6],native_adreno_view_kept;}   /* kept: BoCpuUnmap calls that left the view for another lease */
}

/* The open type the Adreno plugin passes to IOServiceOpen ('AG'); it lets the driver tell the plugin from the original IOGPU library
 * (type 1 plus an API value) and from any other client of the accelerator. */
static volatile uint32_t gHoujiProbeResult,gHoujiProbeStarted;
static uint64_t gHoujiDesktopSubmits,gHoujiDesktopWaits,gHoujiDesktopSubmitErrors,gHoujiDesktopLastFence;
enum : UInt32 {NativeAdrenoPluginOpenType=0x4147,NativeAdrenoTunnelMagic=0x4e544e4c /* 'NTNL' */};
/* The display hand-off (boot-arg sheng-fb-gpu=1, drivers/NativeMobileDisplay) reaches the plugin through this already admitted connection: the
 * original sandbox profiles admit this class but not the presenter's own service. Methods 0x7000 + the presenter's NGP_METHOD numbers (mode,
 * wait for a frame, complete it) are forwarded unchanged, without the GPU lock (a wait sleeps); memory type NativeAdrenoScanoutType is the
 * writable scanout. Nothing here touches the GPU. */
enum : UInt32 {NativeAdrenoHandoffBase=0x7000,NativeAdrenoHandoffLast=0x7003,NativeAdrenoScanoutType=0x5343,
               NativeAdrenoHandoffOpenType=0x4148};
/* Open type 0x4148 is a connection that serves only the hand-off: it does not mount a GPU core, so it does not need (or take) a
 * GPU connection slot (the Vulkan driver holds one for as long as the Metal device lives). Same class, so the same sandbox admission. */
extern "C" IOReturn native_gop_handoff_call(uint32_t op,task_t task,const void *in,size_t inSize,void *out,size_t outSize);
/* Export (int-147): method NativeAdrenoExportMethod hands one in-order piece of a blob to the board's UFS log (kits/sheng/BootLog.c), which writes
 * the newest complete blob to its export area. The render server sends its pipeline caches this way, so a later image can be built with them.
 * Input: {generation, total, offset, bytes} then the bytes. No GPU state is touched; any connection of the render server may send it. */
enum : UInt32 {NativeAdrenoExportMethod=0x7100,NativeAdrenoExportMaxPiece=3968};
extern "C" int boot_log_export_put(uint32_t generation,uint32_t total,uint32_t offset,const void *data,uint32_t size) __attribute__((weak_import));
extern "C" {volatile uint32_t native_adreno_export_stats[4];}   /* [0] pieces taken, [1] refused, [2] last generation, [3] its total bytes */
extern "C" IOMemoryDescriptor *native_gop_handoff_memory(void);

/* Mailbox: a last-resort command channel for a sandbox that admits no external-method selector at all. It is one shared page (memory
 * type NativeAdrenoMailboxType through IOConnectMapMemory64, which the selector filter does not cover) and a kernel poller that runs
 * whatever the live connection writes into it. Layout (little endian dwords): +0 magic, +4 request seq, +8 response seq, +12 method,
 * +16 status, +20 input bytes, +24 output bytes, input at +0x40 (max 0x700), output at +0x800 (max 0x700). */
enum : UInt32 {NativeAdrenoMailboxType=0xC0DE,NativeAdrenoMailboxMagic=0x4d41494c /* 'MAIL' */};
namespace {
IOBufferMemoryDescriptor *gMailbox=nullptr;
volatile uint32_t *gMailboxWords=nullptr;
volatile bool gPollerStarted=false;
}
class AGXDeviceUserClient;
struct NativeAdrenoDispatchClock;
static IOReturn nativeAdrenoRunGuarded(AGXDeviceUserClient *,uint32_t,const void *,size_t,void *,size_t,NativeAdrenoDispatchClock &);
static IOReturn nativeAdrenoWaitFence(AGXDeviceUserClient *,const void *,void *,size_t,NativeAdrenoDispatchClock &);
/* The render server's connection serves the mailbox; it is retained while set (under g.lock), so the poller never runs on a freed object. */
static AGXDeviceUserClient *gMailboxOwner=nullptr;
void nativeAdrenoMailboxPoller(void *,wait_result_t);
static IOReturn runCore(AGXDeviceUserClient *,uint32_t method,const void *input,size_t input_size,void *output,size_t output_size);

/* The two sandbox gates IOServiceOpen applies after newUserClient (XNU IOUserClient.cpp is_io_service_open_extended): the MACF open check on
 * the client object and the sandbox's filter resolver. They are run here for the caller too, only to record which one refuses it. */
extern "C" int mac_iokit_check_open(void *cred,void *client,unsigned type);
extern "C" void *kauth_cred_get(void);
extern const struct io_filter_callbacks *gIOUCFilterCallbacks;

extern "C" volatile uint32_t native_adreno_trace_stage;
extern "C" {
/* One entry per IOServiceOpen of the accelerator (not markers): who, which type, what the two sandbox gates said. */
struct NativeAdrenoOpenRecord {char name[20]; uint32_t type, macf, res, attach, foreign;};
struct NativeAdrenoOpenRecord native_adreno_open_log[8];
volatile uint32_t native_adreno_open_log_count;
/* One row per process name that opened the plugin type (first eight names): opens granted, refused (sandbox, cap or no free slot),
 * closes, the slot of its live connection (0xff none), the highest GPU address its mappings reached (MiB) and its last refusal. */
struct NativeAdrenoClientRecord {char name[20]; uint32_t opens, refused, closes, slot, peak_mib, last_refusal;};
struct NativeAdrenoClientRecord native_adreno_client_log[8];
/* The last text line (the plugin's failure reason) each process other than the render server streamed: process name and a 64-byte line. */
struct NativeAdrenoClientText {char name[20]; char text[64];};
struct NativeAdrenoClientText native_adreno_client_text[4];
/* Why the latest mount of a connection failed: [0] 1 no free slot, 2 the core's open, 3 the window scope; [1] its IOReturn; [2] the slot; [3] failures so far. */
volatile uint32_t native_adreno_mount_fail[4];
/* Processes that read MetalPluginName (the Metal discovery), first twelve names, with counts. */
struct NativeAdrenoAskerRecord {char name[20]; uint32_t reads;};
struct NativeAdrenoAskerRecord native_adreno_metal_askers[12];
}
#include "../HoujiGpuClientLayout.h"

namespace {
void shiftName(char names[3][17]) {
    memcpy(names[2], names[1], 17);
    memcpy(names[1], names[0], 17);
    proc_selfname(names[0], 17);
}
struct Published {
    bool published=false,dying=false;
    NativeAdrenoVm *vm=nullptr;
    NativeAdrenoSubmit *submit=nullptr;
    NativeAdrenoRing *ring=nullptr;
    NativeAdrenoRingClock clock{};
    const NativeAdrenoTimer *timer=nullptr;
    const NativeAdrenoAbiDeviceInfo *info=nullptr;
    IOLock *lock=nullptr;
    IOService *service=nullptr;
    bool slots[NativeAdrenoMaxClients]{};
    bool kernel_client=false;
    uint32_t input_bits=0,output_bits=0;
    uint32_t opened=0,closed=0;
} g;
}

// gtrace=1 is an explicit CPU-only diagnostic. The wrappers preserve the
// original mutex acquisition, try-lock failure and release points exactly.
static NativeAdrenoIssuerTrace nativeAdrenoIssuerTrace;
static uint64_t nativeAdrenoIssuerNow() {
 return nativeAdrenoIssuerTrace.enabled && g.clock.now_us?g.clock.now_us(g.clock.cookie):0;
}
static void nativeAdrenoIssuerLock(unsigned site) {
 const uint64_t requested=nativeAdrenoIssuerNow();
 IOLockLock(g.lock);
 nativeAdrenoIssuerTrace.begin(requested,nativeAdrenoIssuerNow(),site);
}
static bool nativeAdrenoIssuerTryLock(unsigned site) {
 const uint64_t requested=nativeAdrenoIssuerNow();
 if(!IOLockTryLock(g.lock))return false;
 nativeAdrenoIssuerTrace.begin(requested,nativeAdrenoIssuerNow(),site);return true;
}
static void nativeAdrenoIssuerUnlock() {
 if(nativeAdrenoIssuerTrace.enabled)
  nativeAdrenoIssuerTrace.end(nativeAdrenoIssuerNow(),g.ring?g.ring->lastSubmitted():0,g.ring?g.ring->completedFence():0);
 IOLockUnlock(g.lock);
}
static void nativeAdrenoIssuerContext(unsigned method,unsigned client,const void *input,size_t bytes) {
 if(!nativeAdrenoIssuerTrace.enabled)return;
 uint64_t subject=0;
 if(input && bytes>=sizeof(subject) && (method==NativeAdrenoMethodWaitFence ||
    method==NativeAdrenoMethodBoMap || method==NativeAdrenoMethodBoFree ||
    method==NativeAdrenoMethodBoCpuMap || method==NativeAdrenoMethodBoCpuUnmap))memcpy(&subject,input,sizeof subject);
 nativeAdrenoIssuerTrace.context(method,client,subject);
}

static uint64_t gHoujiPresentSync[14];
// The event address outlives every client and display instance. Predicate and
// wait registration share g.lock; only a changed, observed completion wakes it.
static uint32_t gHoujiPresentProgress;
static void nativeAdrenoSignalProgressLocked(void) {
 if(!g.ring)return;
 const uint32_t completed=g.ring->completedFence(); // cached, no extra MMIO
 if(completed==gHoujiPresentProgress)return;
 gHoujiPresentProgress=completed;
 __atomic_fetch_add(&gHoujiPresentSync[13],1ull,__ATOMIC_RELAXED);
 thread_wakeup((event_t)&gHoujiPresentProgress);
}

class NativeAdrenoGpuPublished : public IOService {
    OSDeclareDefaultStructors(NativeAdrenoGpuPublished)
public:
    IOReturn newUserClient(task_t owningTask,void *securityID,UInt32 type,
        OSDictionary *properties,IOUserClient **handler) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(NativeAdrenoGpuPublished,IOService)

class AGXDeviceUserClient : public IOUserClient {
    OSDeclareDefaultStructors(AGXDeviceUserClient)
    IOReturn view_error_=kIOReturnSuccess;
    unsigned teardown_traces_=0;
    bool live_=false;
    bool handoffOnly_=false;
    bool kernel_=false;
    NativeAdrenoUserClient *core_=nullptr;   /* own core; deleted in free(), so a call that raced the close sees live_ false, never freed memory */
    unsigned slot_=NativeAdrenoMaxClients;
    char name_[20]{};
    /* One user-view descriptor per BO token, reused by every clientMemoryForType call. IOConnectUnmapMemory64 asks clientMemoryForType for
     * the descriptor again and unmaps only a mapping that belongs to THAT descriptor (kIOMapReference); a fresh descriptor per call made the
     * unmap a silent no-op, the user PTEs outlived BoCpuUnmap, and BoFree handed pages with a live user mapping to the VM:
     * pmap_assert_free panicked the freeing core and the panic screen parked it (the "BoFree hang", dead touch). */
    struct View {uint32_t token;IOMemoryDescriptor *descriptor;};
    /* One view per live BO token. The cache must hold every BO the client can have (NativeAdrenoClientMaxObjects): a BO whose view is not
     * cached makes IOConnectUnmapMemory64 (which asks clientMemoryForType again) a no-op, the user mapping outlives BoFree and the freed pages panic
     * pmap_assert_free. A cache of 64 held until the render server's hundreds of mapped BOs (int-087). */
    enum {MaxViews=NativeAdrenoClientMaxObjects};
    View *views_=nullptr;   /* on the heap: 1024 entries would make the client object too big for the kernel allocator's fixed-size classes */
    IOReturn dropViews(uint32_t token);
    static IOReturn action(OSObject *target,void *reference,IOExternalMethodArguments *arguments);
    IOReturn run(uint32_t method,const void *input,size_t input_size,void *output,size_t output_size);
public:
    void setHandoffOnly() {handoffOnly_=true;}
    bool mount(bool kernel);
    friend IOReturn runCore(AGXDeviceUserClient *,uint32_t,const void *,size_t,void *,size_t);
    friend IOReturn nativeAdrenoRunGuarded(AGXDeviceUserClient *,uint32_t,const void *,size_t,void *,size_t,NativeAdrenoDispatchClock &);
    friend IOReturn nativeAdrenoWaitFence(AGXDeviceUserClient *,const void *,void *,size_t,NativeAdrenoDispatchClock &);
    friend void nativeAdrenoMailboxPoller(void *,wait_result_t);
    IOReturn clientClose() APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    IOReturn clientMemoryForType(UInt32 type,IOOptionBits *options,IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
    IOReturn externalMethod(uint32_t selector,IOExternalMethodArguments *arguments,
        IOExternalMethodDispatch *dispatch,OSObject *target,void *reference) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(AGXDeviceUserClient,IOUserClient)

void nativeAdrenoMailboxPoller(void *,wait_result_t)
{
    thread_set_thread_name(current_thread(),"sheng-gpu-mailbox");
    uint32_t last=0;
    for(;;) {
        IOSleep(1);
        if(!gMailboxOwner || !gMailboxWords)continue;
        const uint32_t request=gMailboxWords[1];
        if(request==last || request==gMailboxWords[2])continue;
        last=request;
        const uint32_t method=gMailboxWords[3],in_size=gMailboxWords[5],out_size=gMailboxWords[6];
        IOReturn status=kIOReturnBadArgument;
        if(in_size<=0x700 && out_size<=0x700) {
            uint8_t input[0x700],output[0x700];
            memcpy(input,const_cast<uint32_t *>(gMailboxWords)+0x10,in_size);
            nativeAdrenoIssuerLock(__LINE__);
            AGXDeviceUserClient *owner=gMailboxOwner;
            if(owner)owner->retain();
            nativeAdrenoIssuerUnlock();
            status=owner?runCore(owner,method,input,in_size,output,out_size):kIOReturnNotReady;
            if(owner)owner->release();
            if(!status)memcpy(const_cast<uint32_t *>(gMailboxWords)+0x200,output,out_size);
        }
        native_adreno_metal_diag.mb_calls=native_adreno_metal_diag.mb_calls+1;
        gMailboxWords[4]=uint32_t(status);
        __asm__ volatile("dmb sy" ::: "memory");
        gMailboxWords[2]=request;
        __asm__ volatile("dmb sy" ::: "memory");
    }
}

/* The client log row of name (created for the first eight names); null when the table is full. Under g.lock. */
static NativeAdrenoClientRecord *clientRecord(const char *name)
{
    for(auto &record:native_adreno_client_log) {
        if(!record.name[0]) {strlcpy(record.name,name,sizeof record.name);record.slot=0xff;return &record;}
        if(!strncmp(record.name,name,sizeof record.name))return &record;
    }
    return nullptr;
}
bool AGXDeviceUserClient::mount(bool kernel)
{
    proc_selfname(name_,sizeof name_);
    NativeAdrenoUserClient *core=new NativeAdrenoUserClient;
    if(!core)return false;
    nativeAdrenoIssuerLock(__LINE__);
    bool ok=false;
    /* Keep the existing generic slots and reserve a separate bounded window
     * for Preferences; its Core Image icon path cannot use the GL fallback. */
    unsigned slot=NativeAdrenoMaxClients;
    if(kernel)slot=g.kernel_client?NativeAdrenoMaxClients:0;
    else {
        slot=houjiGpuClientSlot(!strncmp(name_,"backboardd",sizeof name_),
                               !strncmp(name_,"Preferences",sizeof name_),g.slots);
    }
    const IOReturn opened=slot<NativeAdrenoMaxClients?core->open(NativeAdrenoAbiVersion,g.vm,g.submit,g.ring,g.input_bits,g.output_bits,true):kIOReturnNoSpace;
    if(opened!=kIOReturnSuccess) {native_adreno_mount_fail[0]=slot<NativeAdrenoMaxClients?2:1;native_adreno_mount_fail[1]=uint32_t(opened);native_adreno_mount_fail[2]=slot;native_adreno_mount_fail[3]=native_adreno_mount_fail[3]+1;}
    if(opened==kIOReturnSuccess) {
        if(g.timer)core->attachTimer(g.timer);
        if(g.info)core->attachDeviceInfo(g.info);
        if(auto*status=native_adreno_board_status())core->attachStatus(status);
        /* Queue ranges: the kernel client keeps range 0 alone (no user slot is live while the board's self-test runs); users get their slot's. */
        const IOReturn scoped=kernel?core->setScope(0,0,0):core->setScope(slot*NativeAdrenoAbiMaxQueues,kClientWindows[slot].start,kClientWindows[slot].bytes);
        if(scoped==kIOReturnSuccess) {
            core_=core;core=nullptr;slot_=slot;kernel_=kernel;live_=true;ok=true;
            if(kernel)g.kernel_client=true;else g.slots[slot]=true;
            ++g.opened;
            if(NativeAdrenoClientRecord *record=clientRecord(name_))record->slot=slot;
        } else {
            native_adreno_mount_fail[0]=3;native_adreno_mount_fail[1]=uint32_t(scoped);native_adreno_mount_fail[2]=slot;native_adreno_mount_fail[3]=native_adreno_mount_fail[3]+1;
            core->releaseAll(&g.clock,0);
        }
    }
    /* The mailbox page and its poller serve the render server's connection only. */
    if(false && ok && !kernel && slot==0 && !strncmp(name_,"backboardd",sizeof name_)) {
        if(!gMailbox) {
            gMailbox=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,kIODirectionInOut,4096,4096);
            if(gMailbox) {gMailboxWords=static_cast<volatile uint32_t *>(gMailbox->getBytesNoCopy());memset(const_cast<uint32_t *>(gMailboxWords),0,4096);}
        }
        if(gMailboxWords) {
            memset(const_cast<uint32_t *>(gMailboxWords),0,4096);
            gMailboxWords[0]=NativeAdrenoMailboxMagic;
            if(!gPollerStarted) {
                thread_t thread=nullptr;
                if(kernel_thread_start(nativeAdrenoMailboxPoller,nullptr,&thread)==KERN_SUCCESS) {thread_deallocate(thread);gPollerStarted=true;}
            }
        }
        if(gMailboxOwner)gMailboxOwner->release();
        retain();gMailboxOwner=this;
    }
    nativeAdrenoIssuerUnlock();
    if(core)delete core;
    return ok;
}
/* Removes the cached view(s) of token (0 = all) and tears down every user mapping of them, so the BO's pages are never freed while a user
 * PTE still points at them, whether or not the client unmapped. */
IOReturn AGXDeviceUserClient::dropViews(uint32_t token)
{
    IOReturn previous=__atomic_load_n(&view_error_,__ATOMIC_ACQUIRE);
    if(previous)return previous;
    for(;;) {
        IOMemoryDescriptor *view=nullptr;
        nativeAdrenoIssuerLock(__LINE__);
        if(views_)for(size_t i=0;i<MaxViews;++i){View&v=views_[i];if(v.descriptor && (!token || v.token==token)){view=v.descriptor;v.descriptor=nullptr;v.token=0;break;}}
        nativeAdrenoIssuerUnlock();
        if(!view)return kIOReturnSuccess;
        /* IOUserClient returns an owned RAW IOMemoryMap* in this integrated
         * Darwin22 build. A discarded pointer leaks a live task PTE. Revoke
         * the VA explicitly, then release the returned reference exactly once.
         * No GPU gate is held across the VM's unmap path. */
        while(IOMemoryMap *mapping=removeMappingForDescriptor(view)) {
            const IOReturn unmapped=mapping->unmap();
            if(unmapped){
                __atomic_store_n(&view_error_,unmapped,__ATOMIC_RELEASE);
                native_adreno_view_diag[4]=uint32_t(unmapped);
                IOLog("HOUJI VIEW QUARANTINE: token=%u status=%x; map/view/core retained, no BO free\n",token,unmapped);
                /* Keep the owned map + view references and live core. Every
                 * later operation/close refuses to free its backing pages. */
                return unmapped;
            }
            mapping->release();
            native_adreno_metal_diag.forced_unmaps=native_adreno_metal_diag.forced_unmaps+1;
        }
        view->release();
    }
}
IOReturn AGXDeviceUserClient::clientClose()
{
    if(g.dying)return kIOReturnNotReady;
    if(!kernel_ && views_ && !strcmp(name_,"adrenometalprobe") && teardown_traces_++<4)native_adreno_board_teardown_checkpoint("before-clientClose-views",0,0);
    const IOReturn dropped=dropViews(0);if(dropped)return dropped;
    nativeAdrenoIssuerLock(__LINE__);
    IOReturn status=kIOReturnSuccess;
    AGXDeviceUserClient *mailbox=nullptr;
    if(gMailboxOwner==this) {mailbox=gMailboxOwner;gMailboxOwner=nullptr;}
    if(live_) {
        const uint64_t peak=core_->windowPeak();
        status=native_adreno_board_quiesce();
        if(!status)status=core_->releaseAll(&g.clock,2000000);
        if(!status)status=native_adreno_board_quiesce();
        /* A failed teardown leaves the objects live and the slot taken: nothing is forced or reused. */
        if(!status) {
            live_=false;
            if(kernel_)g.kernel_client=false;else if(slot_<NativeAdrenoMaxClients)g.slots[slot_]=false;
            ++g.closed;native_adreno_metal_diag.closes=native_adreno_metal_diag.closes+1;
            if(NativeAdrenoClientRecord *record=clientRecord(name_)) {
                record->closes=record->closes+1;record->slot=0xff;
                if(peak>>20>record->peak_mib)record->peak_mib=uint32_t(peak>>20);
            }
        }
    }
    nativeAdrenoIssuerUnlock();
    if(mailbox)mailbox->release();
    if(!status)terminate();
    return status;
}
void AGXDeviceUserClient::free()
{
    dropViews(0);
    if(views_) {IODelete(views_,View,MaxViews);views_=nullptr;}
    /* Still live only when a teardown failed: its objects stay referenced by the VM and the ring, so the core is kept, not freed. */
    if(core_ && !live_ && !__atomic_load_n(&view_error_,__ATOMIC_ACQUIRE)) {delete core_;core_=nullptr;}
    IOUserClient::free();
}
IOReturn AGXDeviceUserClient::clientMemoryForType(UInt32 type,IOOptionBits *options,IOMemoryDescriptor **memory)
{
    if(memory)*memory=nullptr;
    if(g.dying)return kIOReturnNotReady;
    if(__atomic_load_n(&view_error_,__ATOMIC_ACQUIRE))return kIOReturnNotReady;
    if(!memory || !options)return kIOReturnBadArgument;
    IOReturn status=kIOReturnNotFound;
    if(type==NativeAdrenoScanoutType) {
        if(!live_ && !handoffOnly_)return kIOReturnNotReady;
        IOMemoryDescriptor *scanout=native_gop_handoff_memory();
        if(!scanout)return kIOReturnNotReady;
        *memory=scanout;                        /* already retained for the caller */
        *options=kIOMapAnywhere|kIOMapInhibitCache;   /* firmware framebuffer: device memory, never cached by the CPU */
        return kIOReturnSuccess;
    }
    if(type==NativeAdrenoMailboxType) {
        if(live_ && gMailbox && gMailboxOwner==this) {
            gMailbox->retain();
            *memory=gMailbox;
            *options=kIOMapAnywhere;   /* ordinary cached memory: both sides are CPUs, so it is coherent */
            native_adreno_metal_diag.mb_maps=native_adreno_metal_diag.mb_maps+1;
            return kIOReturnSuccess;
        }
        return kIOReturnNotReady;
    }
    native_adreno_view_diag[0]=native_adreno_view_diag[0]+1;
    /* [5]: threads inside this function right now (a thread stuck here, on the gate or in a mapping, shows as a non-zero count that stays). */
    struct Inside {Inside() {__atomic_fetch_add(&native_adreno_view_diag[5],1,__ATOMIC_RELAXED);} ~Inside() {__atomic_fetch_sub(&native_adreno_view_diag[5],1,__ATOMIC_RELAXED);}} inside;
    nativeAdrenoIssuerLock(__LINE__);
    if(!views_)views_=IONewZero(View,MaxViews);
    if(!views_) {nativeAdrenoIssuerUnlock();return kIOReturnNoMemory;}
    NativeAdrenoMemory *backing=live_?core_->memoryForToken(type):nullptr;
    NativeAdrenoCpuView view{};
    enum {MaxSpans=65536};
    /* Recheck even a cached view: quarantine or retired ownership must not
     * grant another descriptor lease. Already mapped views remain retained. */
    if(!backing || !backing->cpuView(&view) || !view.count || view.count>MaxSpans) {
        nativeAdrenoIssuerUnlock();return kIOReturnNotReady;
    }
    for(size_t i=0;i<MaxViews;++i)if(views_[i].descriptor && views_[i].token==type) {
        views_[i].descriptor->retain();*memory=views_[i].descriptor;*options=kIOMapAnywhere|kIOMapWriteCombineCache;
        native_adreno_view_diag[2]=native_adreno_view_diag[2]+1;
        nativeAdrenoIssuerUnlock();
        return kIOReturnSuccess;
    }
    if(backing) {
        /* Candidate CPU16K/GPU4K separation: expose only this BO's complete
         * exclusive CPU backing. A clipped GPU logical span must never become
         * a rounded CPU mapping over a neighbour's storage.
         * The BO's pages are cached in the kernel and the GPU's PTEs are non-cacheable, so the client needs an uncached
         * view. A mapping of the buffer descriptor with a different cache mode is not reliable, so describe the same raw
         * physical spans (the ones the SMMU maps) and ask for kIOMapWriteCombineCache, exactly how the board code maps the
         * ring and the GMU windows. The descriptor does not retain the pages: the client must unmap before BoCpuUnmap/BoFree. */
        /* A BO's pages can come scattered, one span per page (a 64 MiB BO is 16384 of them): the descriptor takes them all, as many as the GPU's
         * own table accepts. A fixed 64-entry array made every scattered BO above 256 KiB fail to map (VK_ERROR_MEMORY_MAP_FAILED, int-084),
         * only when memory happened to be fragmented. The descriptor copies the ranges. */
        const size_t count=view.count;
        if(!count || count>MaxSpans)status=kIOReturnNoResources;
        else {
            IOAddressRange *ranges=IONewData(IOAddressRange,count);
            if(!ranges)status=kIOReturnNoMemory;
            else {
                for(size_t i=0;i<count;++i)ranges[i]={view.spans[i].physical,view.spans[i].bytes};
                IOMemoryDescriptor *descriptor=IOMemoryDescriptor::withAddressRanges(ranges,(UInt32)count,
                    static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryTypePhysical64),nullptr);
                IODeleteData(ranges,IOAddressRange,count);
                if(!descriptor)status=kIOReturnNoMemory;
                else {
                    bool cached=false;
                    for(size_t i=0;i<MaxViews;++i)if(!views_[i].descriptor) {views_[i].token=type;views_[i].descriptor=descriptor;descriptor->retain();cached=true;break;}
                    if(!cached) {
                        /* Never hand out a view that cannot be torn down again. */
                        descriptor->release();
                        status=kIOReturnNoResources;
                    } else {
                        *memory=descriptor;
                        *options=kIOMapAnywhere|kIOMapWriteCombineCache;
                        native_adreno_view_diag[1]=native_adreno_view_diag[1]+1;
                        status=kIOReturnSuccess;
                    }
                }
            }
        }
    }
    nativeAdrenoIssuerUnlock();
    if(status){native_adreno_view_diag[3]=native_adreno_view_diag[3]+1;native_adreno_view_diag[4]=(uint32_t)status;}
    return status;
}
/* Fence waits as the callers see them (int-147; method 6's own statistics count every poll): [0] waits, [1] us waited in total, [2] pauses
 * taken outside the connection lock, [3] the longest wait in us,
 * [4] pending observations that skipped the issuer mutex. */
extern "C" {volatile uint64_t native_adreno_wait_outer[5];}
extern "C" bool native_adreno_board_fence_pending(uint32_t fence);
// A negative hint never retires a job. These are published under the issuer
// mutex; lock-free readers only skip a still-pending poll until its deadline.
static uint32_t nativeAdrenoHintIssued=0,nativeAdrenoHintEnabled=0;
static bool nativeAdrenoFencePending(uint64_t fence) {
 if(!fence || fence>UINT32_MAX || !__atomic_load_n(&nativeAdrenoHintEnabled,__ATOMIC_ACQUIRE))return false;
 const uint32_t issued=__atomic_load_n(&nativeAdrenoHintIssued,__ATOMIC_ACQUIRE);
 return fence<=issued && native_adreno_board_fence_pending((uint32_t)fence);
}
/* Query/status/info and metadata use only host bookkeeping under g.lock.
 * Timestamp uses the original guarded, rollover-checked counter reader: it
 * reads but never mutates GPU state. Neither path needs to drain unrelated
 * rendering. VM/BO mutations and submissions poll, then sleep outside the lock
 * while pending GPU writes finish; both retain the original drain/proof. */
static bool nativeAdrenoNonMutatingGpuMethod(uint32_t method) {
    switch(method) {
    case NativeAdrenoMethodQuery:
    case NativeAdrenoMethodGetTimestamp:
    case NativeAdrenoMethodGetDeviceInfo:
    case NativeAdrenoMethodGetStatus:
    case NativeAdrenoMethodSetMetadata:
    case NativeAdrenoMethodGetMetadata:return true;
    default:return false;
    }
}
extern "C" void houji_gate_hitch_snapshot(uint64_t out[6]);
extern "C" void houji_surface_lazy_report(void);
extern "C" void houji_power_report(void);
extern "C" void houji_cp_lease_report(void);
extern "C" {extern volatile uint64_t native_adreno_prologue_stats[2];}
extern "C" {extern volatile uint64_t native_adreno_wptr_stats[4];}
extern "C" {extern volatile uint64_t native_adreno_dvfs_skips[2];}
extern "C" {extern volatile uint64_t native_adreno_submit_parts[12][2];}
extern "C" {extern volatile uint64_t native_adreno_pipeline_stats[5];extern volatile uint64_t native_adreno_tlb_stats[2];}
static uint64_t nativeAdrenoDispatchPerf[20][4]; // calls, time us, max us, cached path
/* Issuer-lock protected. A draining mutation keeps its place while asleep,
 * so new submissions cannot refill the GPU ahead of it. Queries, fence polls
 * and the display observer still take the free issuer lock normally. */
static const void *nativeAdrenoMutationOwner;
static uint64_t nativeAdrenoGateRetries[2]; // BO/queue operations, submissions
#include "NativeAdrenoLatency.h"
#include "NativeAdrenoAllocationTrace.h"
static NativeAdrenoLatency nativeAdrenoLatency;
struct NativeAdrenoDispatchClock {
 uint64_t began=g.clock.now_us?g.clock.now_us(g.clock.cookie):0,last=began,phases[4]{};
 void lap(unsigned phase){uint64_t now=g.clock.now_us?g.clock.now_us(g.clock.cookie):last;phases[phase]+=now-last;last=now;}
 void finish(unsigned method,IOReturn status){lap(3);nativeAdrenoLatency.record(method,began,last-began,phases,(uint32_t)status);}
};
static bool nativeAdrenoSubmitMethod(uint32_t method) {
 return method==NativeAdrenoMethodSubmit || method==NativeAdrenoMethodSubmitBatch;
}
extern "C" volatile uint64_t native_adreno_submit_cost[12];
extern "C" volatile uint64_t native_adreno_submit_batches[4];
extern "C" volatile uint32_t native_adreno_io_coherent,native_adreno_coherent_skips[4],native_adreno_wc_audit[6],native_adreno_wc_distrusted;
extern "C" volatile uint32_t native_adreno_coherent_import_fallbacks;
extern "C" uint64_t houji_gpu_submit_activity(void) {
 return __atomic_load_n(&nativeAdrenoDispatchPerf[NativeAdrenoMethodSubmit][0],__ATOMIC_RELAXED)+
        __atomic_load_n(&nativeAdrenoDispatchPerf[NativeAdrenoMethodSubmitBatch][0],__ATOMIC_RELAXED);
}
extern "C" void houji_sandbox_abi_report(void);
extern "C" void houji_cpu_scheduler_report(void);

/* Called with no issuer lock held. Keeping this path separate also lets host
 * checks exercise the actual lock/retry/dispatch ordering with scheduler peers. */
static IOReturn nativeAdrenoRunGuarded(AGXDeviceUserClient *connection,uint32_t method,const void *input,size_t input_size,void *output,size_t output_size,NativeAdrenoDispatchClock &timing)
{
    // Host imports must use the same pending-work gate as other BO mutations.
    // The former special case drained the GPU while holding the issuer lock,
    // blocking completion/present observers and bypassing this timing record.
    if(method==NativeAdrenoMethodBoImport &&
       (!input || input_size!=sizeof(NativeAdrenoAbiBoImport) || !output || output_size!=sizeof(NativeAdrenoAbiBoAllocReply)))
        return kIOReturnBadArgument;
    IOReturn status;
    const uint64_t began=g.clock.now_us?g.clock.now_us(g.clock.cookie):0;
    for(;;) {
        timing.lap(3);
        nativeAdrenoIssuerLock(__LINE__);
        timing.lap(0);
        nativeAdrenoIssuerContext(method,connection->slot_,input,input_size);
        const bool cached=nativeAdrenoNonMutatingGpuMethod(method);
        if(!connection->live_ || g.dying || !native_adreno_board_running())status=kIOReturnNotReady;
        else if(cached)status=kIOReturnSuccess;
        else if(nativeAdrenoMutationOwner && nativeAdrenoMutationOwner!=&timing)status=kIOReturnTimeout;
        else {
            if(!nativeAdrenoSubmitMethod(method))nativeAdrenoMutationOwner=&timing;
            status=native_adreno_board_prepare_submit();
        }
        timing.lap(1);
        // BO mutations use the same zero-timeout readiness poll as submits.
        // Keep the original quiescence, cache and VM proof before dispatch;
        // only its pending-GPU wait moves outside the issuer lock. Never
        // retry a dispatched operation, even if it returns Timeout.
        if(!cached && status==kIOReturnTimeout && g.clock.now_us) {
            uint64_t elapsed=g.clock.now_us(g.clock.cookie)-began;
            if(elapsed<500000) {
                __atomic_fetch_add(&nativeAdrenoGateRetries[nativeAdrenoSubmitMethod(method)?1:0],1ull,__ATOMIC_RELAXED);
                nativeAdrenoSignalProgressLocked();
                nativeAdrenoIssuerUnlock();
                uint32_t pause=(uint32_t)((500000-elapsed)<100?(500000-elapsed):100);
                assert_wait_timeout((event_t)&native_adreno_wait_outer,THREAD_UNINT,pause,NSEC_PER_USEC);
                thread_block(THREAD_CONTINUE_NULL);
                continue;
            }
        }
        if(!status) {
            if(!connection->live_ || g.dying || !native_adreno_board_running())status=kIOReturnNotReady;
            else if(method==NativeAdrenoMethodBoImport) {
                // An external method runs in its caller's task, even after a
                // bounded wait. Keep that task, aligned copies and the original
                // pinning/ownership checks; never retry a dispatched import.
                NativeAdrenoAbiBoImport request{};
                NativeAdrenoAbiBoAllocReply reply{};
                memcpy(&request,input,sizeof(request));
                status=connection->core_->importBo(&request,current_task(),&reply);
                if(!status)memcpy(output,&reply,sizeof(reply));
            } else status=connection->core_->dispatch(method,input,input_size,output,output_size,&g.clock);
        }
        timing.lap(2);
        if(!status && nativeAdrenoSubmitMethod(method) && output && output_size==sizeof(NativeAdrenoAbiSubmitReply)) {
            const uint64_t fence=static_cast<const NativeAdrenoAbiSubmitReply*>(output)->fence;
            __atomic_store_n(&nativeAdrenoHintIssued,fence<=UINT32_MAX?(uint32_t)fence:0u,__ATOMIC_RELEASE);
            nativeAdrenoIssuerTrace.subject(fence);
        }
        if(method<20) {
            const uint64_t elapsed=g.clock.now_us?g.clock.now_us(g.clock.cookie)-began:0;
            __atomic_fetch_add(&nativeAdrenoDispatchPerf[method][0],1ull,__ATOMIC_RELAXED);__atomic_fetch_add(&nativeAdrenoDispatchPerf[method][1],elapsed,__ATOMIC_RELAXED);
            uint64_t maximum=__atomic_load_n(&nativeAdrenoDispatchPerf[method][2],__ATOMIC_RELAXED);while(elapsed>maximum && !__atomic_compare_exchange_n(&nativeAdrenoDispatchPerf[method][2],&maximum,elapsed,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)){}
            if(cached)__atomic_fetch_add(&nativeAdrenoDispatchPerf[method][3],1ull,__ATOMIC_RELAXED);
        }
        if(method==NativeAdrenoMethodBoMap && !status && connection->live_ && !connection->kernel_)
            if(NativeAdrenoClientRecord *record=clientRecord(connection->name_))
                if(connection->core_->windowPeak()>>20>record->peak_mib)record->peak_mib=uint32_t(connection->core_->windowPeak()>>20);
        nativeAdrenoSignalProgressLocked();
        if(nativeAdrenoMutationOwner==&timing)nativeAdrenoMutationOwner=nullptr;
        nativeAdrenoIssuerUnlock();
        return status;
    }
}

static IOReturn nativeAdrenoWaitFence(AGXDeviceUserClient *connection,const void *input,void *output,size_t output_size,NativeAdrenoDispatchClock &timing)
{
    // Adapted from Adreno740 dd43abd: pending polls do not queue on the
    // issuer mutex. The qualified NC hint cannot authorize success, resource
    // release, or a cache operation. Completion, errors, invalid/unknown fences
    // and the deadline ALWAYS go through the original locked dispatch.
    NativeAdrenoAbiWaitFence poll{};
    memcpy(&poll,input,sizeof(poll));
    const uint64_t timeout_us=poll.timeout_ns/1000,began=g.clock.now_us(g.clock.cookie);
    poll.timeout_ns=0;
    uint64_t waited=0;
    IOReturn status=kIOReturnTimeout;
    for(;;) {
        waited=g.clock.now_us(g.clock.cookie)-began;
        if(waited>=timeout_us || !nativeAdrenoFencePending(poll.fence)) {
            timing.lap(3);
            nativeAdrenoIssuerLock(__LINE__);
            timing.lap(0);
            nativeAdrenoIssuerContext(NativeAdrenoMethodWaitFence,connection->slot_,input,sizeof(poll));
            status=connection->live_ && !g.dying && native_adreno_board_running()?
                connection->core_->dispatch(NativeAdrenoMethodWaitFence,&poll,sizeof(poll),output,output_size,&g.clock):kIOReturnNotReady;
            timing.lap(2);
            nativeAdrenoSignalProgressLocked();
            nativeAdrenoIssuerUnlock();
            waited=g.clock.now_us(g.clock.cookie)-began;
            if(status!=kIOReturnTimeout || waited>=timeout_us)break;
        } else __atomic_fetch_add(&native_adreno_wait_outer[4],1ull,__ATOMIC_RELAXED);
        __atomic_fetch_add(&native_adreno_wait_outer[2],1ull,__ATOMIC_RELAXED);
        // Houji has no GPU IRQ wake source. Retain the bounded scheduler
        // sleep outside the lock; never exceed the caller's remaining slice.
        const uint64_t remaining=timeout_us-waited;
        const uint32_t pause=(uint32_t)(remaining<100?remaining:100);
        assert_wait_timeout((event_t)&native_adreno_wait_outer,THREAD_UNINT,pause,NSEC_PER_USEC);
        thread_block(THREAD_CONTINUE_NULL);
    }
    __atomic_fetch_add(&native_adreno_wait_outer[0],1ull,__ATOMIC_RELAXED);
    __atomic_fetch_add(&native_adreno_wait_outer[1],waited,__ATOMIC_RELAXED);
    uint64_t old=__atomic_load_n(&native_adreno_wait_outer[3],__ATOMIC_RELAXED);
    while(waited>old && !__atomic_compare_exchange_n(&native_adreno_wait_outer[3],&old,waited,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)){}
    return status;
}

static IOReturn runCore(AGXDeviceUserClient *connection,uint32_t method,const void *input,size_t input_size,void *output,size_t output_size)
{
    if(g.dying || !native_adreno_board_running())return kIOReturnNotReady;
    if(method==NativeAdrenoMethodSetMetadata && input && input_size==sizeof(NativeAdrenoAbiMetadata)) {
        const NativeAdrenoAbiMetadata *meta=static_cast<const NativeAdrenoAbiMetadata *>(input);
        /* The raw test's line and the Turnip phase's line ("T ..." result or "W ..." watchdog sample) are kept apart, so neither hides the other. */
        const bool turnip=meta->bytes>=2 && (meta->text[0]=='T' || meta->text[0]=='W') && meta->text[1]==' ';
        char *line=turnip?native_adreno_metal_diag.report2:native_adreno_metal_diag.report;
        uint32_t n=meta->bytes<sizeof(native_adreno_metal_diag.report)-1?meta->bytes:uint32_t(sizeof(native_adreno_metal_diag.report)-1);
        memset(line,0,sizeof(native_adreno_metal_diag.report));
        memcpy(line,meta->text,n);
        native_adreno_metal_diag.reports=native_adreno_metal_diag.reports+1;
    }
    /* A call that is still running when the boot page is photographed shows as inflight = its method number + 1 (0 = none), so a hang in the
     * kernel is attributable to one ABI method. */
    native_adreno_metal_diag.inflight=method+1;
    native_adreno_metal_diag.inflight_since_calls=native_adreno_metal_diag.calls;
    IOReturn status;
    NativeAdrenoDispatchClock timing;
    if(method==NativeAdrenoMethodWaitFence && connection->live_ && input && input_size==sizeof(NativeAdrenoAbiWaitFence) && g.clock.now_us) {
        status=nativeAdrenoWaitFence(connection,input,output,output_size,timing);
    } else {
        status=nativeAdrenoRunGuarded(connection,method,input,input_size,output,output_size,timing);
    }
    timing.finish(method,status);
    native_adreno_metal_diag.inflight=0;
    native_adreno_metal_diag.calls=native_adreno_metal_diag.calls+1;
    native_adreno_metal_diag.last_selector=method;
    native_adreno_metal_diag.last_status=uint32_t(status);
    if(!strcmp(connection->name_,"backboardd")){
        if(nativeAdrenoSubmitMethod(method)){
            if(!status && output && output_size==sizeof(NativeAdrenoAbiSubmitReply)){
                __atomic_fetch_add(&gHoujiDesktopSubmits,1ull,__ATOMIC_RELAXED);
                __atomic_store_n(&gHoujiDesktopLastFence,static_cast<const NativeAdrenoAbiSubmitReply*>(output)->fence,__ATOMIC_RELEASE);
            }else if(status)__atomic_fetch_add(&gHoujiDesktopSubmitErrors,1ull,__ATOMIC_RELAXED);
        }
        if(method==NativeAdrenoMethodWaitFence && !status)__atomic_fetch_add(&gHoujiDesktopWaits,1ull,__ATOMIC_RELAXED);
    }
    return status;
}
IOReturn AGXDeviceUserClient::run(uint32_t method,const void *input,size_t input_size,void *output,size_t output_size)
{
    if(g.dying)return kIOReturnNotReady;
    if(__atomic_load_n(&view_error_,__ATOMIC_ACQUIRE))return kIOReturnNotReady;
    if(!kernel_ && !strcmp(name_,"adrenometalprobe") && (method==NativeAdrenoMethodBoFree || method==NativeAdrenoMethodBoCpuUnmap) && input && input_size==NativeAdrenoUserClient::inputSize(method) && teardown_traces_<4){
        ++teardown_traces_;uint32_t handle=0;memcpy(&handle,input,sizeof(handle));
        native_adreno_board_teardown_checkpoint(method==NativeAdrenoMethodBoFree?"before-BoFree":"before-BoCpuUnmap",handle,method);
    }
    if(method>=NativeAdrenoHandoffBase && method<=NativeAdrenoHandoffLast)
        return native_gop_handoff_call(method-NativeAdrenoHandoffBase,current_task(),input,input_size,output,output_size);
    if(method==NativeAdrenoExportMethod) {
        struct Piece {uint32_t generation,total,offset,bytes;};
        char caller[20]={};proc_selfname(caller,sizeof caller);
        Piece piece{};
        if(input && input_size>=sizeof(piece))memcpy(&piece,input,sizeof(piece));
        const bool valid=!output_size && input && input_size>=sizeof(piece) && piece.bytes && piece.bytes<=NativeAdrenoExportMaxPiece &&
            input_size==sizeof(piece)+piece.bytes && !strncmp(caller,"backboardd",sizeof caller);
        const int stored=-1; /* optional upstream blob export is unsupported on houji */
        if(stored) {native_adreno_export_stats[1]=native_adreno_export_stats[1]+1;return kIOReturnBadArgument;}
        native_adreno_export_stats[0]=native_adreno_export_stats[0]+1;
        native_adreno_export_stats[2]=piece.generation;native_adreno_export_stats[3]=piece.total;
        return kIOReturnSuccess;
    }
    if(method==NativeAdrenoMethodBoCpuUnmap && input && input_size==sizeof(NativeAdrenoAbiBoCpuUnmap)) {
        /* The view is shared by every CPU mapping of the BO, and dropViews removes all of them. A BO mapped twice (the view cache served 207
         * second requests in int-177) lost both mappings at the first unmap, and the other holder's next store hit an unmapped page: the
         * render server's crash in the command stream writes of vkCmdCopyBuffer. Only the last lease takes the view down; the earlier
         * ones leave their mapping until it, or the client unmaps it itself (IOConnectUnmapMemory64 works on the cached view). */
        const uint32_t token=static_cast<const NativeAdrenoAbiBoCpuUnmap *>(input)->token;
        nativeAdrenoIssuerLock(__LINE__);
        const uint32_t leases=live_?core_->mappingLeases(token):0;
        nativeAdrenoIssuerUnlock();
        if(leases>1)native_adreno_view_kept=native_adreno_view_kept+1;
        else {const IOReturn dropped=dropViews(token);if(dropped)return dropped;}
    }
    return runCore(this,method,input,input_size,output,output_size);
}
IOReturn AGXDeviceUserClient::action(OSObject *target,void *,IOExternalMethodArguments *arguments)
{
    AGXDeviceUserClient *connection=OSDynamicCast(AGXDeviceUserClient,target);
    if(!connection || !arguments)return kIOReturnBadArgument;
    return connection->run(arguments->selector,arguments->structureInput,arguments->structureInputSize,
        arguments->structureOutput,arguments->structureOutputSize);
}
IOReturn AGXDeviceUserClient::externalMethod(uint32_t selector,IOExternalMethodArguments *arguments,
    IOExternalMethodDispatch *,OSObject *target,void *reference)
{
    if(g.dying)return kIOReturnNotReady;
    if(selector==NativeAdrenoTunnelMagic)return kIOReturnUnsupported;
    /* One entry per ABI method; sizes are the exact wire records. */
    static const IOExternalMethodDispatch table[]={
        {&action,0,0,0,sizeof(NativeAdrenoAbiParams)},
        {&action,0,sizeof(NativeAdrenoAbiBoAlloc),0,sizeof(NativeAdrenoAbiBoAllocReply)},
        {&action,0,sizeof(NativeAdrenoAbiBoMap),0,sizeof(NativeAdrenoAbiBoMapReply)},
        {&action,0,sizeof(NativeAdrenoAbiBoFree),0,0},
        {&action,0,sizeof(NativeAdrenoAbiSubmit),0,sizeof(NativeAdrenoAbiSubmitReply)},
        {&action,0,sizeof(NativeAdrenoAbiWaitFence),0,0},
        {&action,0,0,0,sizeof(NativeAdrenoAbiTimestamp)},
        {&action,0,0,0,sizeof(NativeAdrenoAbiStatus)},
        {&action,0,sizeof(NativeAdrenoAbiMetadata),0,0},
        {&action,0,sizeof(NativeAdrenoAbiMetadata),0,sizeof(NativeAdrenoAbiMetadata)},
        {&action,0,sizeof(NativeAdrenoAbiQueue),0,sizeof(NativeAdrenoAbiQueueReply)},
        {&action,0,sizeof(NativeAdrenoAbiQueue),0,0},
        {&action,0,sizeof(NativeAdrenoAbiBoCpuMap),0,sizeof(NativeAdrenoAbiBoCpuMapReply)},
        {&action,0,sizeof(NativeAdrenoAbiBoCpuUnmap),0,0},
        {&action,0,0,0,sizeof(NativeAdrenoAbiDeviceInfo)},
        {&action,0,sizeof(NativeAdrenoAbiBoImport),0,sizeof(NativeAdrenoAbiBoAllocReply)},
        {&action,0,sizeof(NativeAdrenoAbiSubmitBatch),0,sizeof(NativeAdrenoAbiSubmitReply)}};
    native_adreno_metal_diag.ext_entries=native_adreno_metal_diag.ext_entries+1;
    if(selector<256)native_adreno_metal_diag.sel_bits[selector>>5]|=1u<<(selector&31);
    /* Tunnel. The sandbox resolves a filter policy for this class at open time and then admits only the selectors its profile lists,
     * so which selector numbers reach this object is decided outside the driver. A tunnel call carries the real ABI method in its
     * first eight bytes and may use ANY admitted selector: {magic, method, payload...} in, the method's exact output out. Method
     * 0xffffffff is a probe that only proves the selector was admitted. */
    if(arguments && arguments->structureInput && arguments->structureInputSize>=8 &&
        static_cast<const uint32_t *>(arguments->structureInput)[0]==NativeAdrenoTunnelMagic) {
        const uint32_t *header=static_cast<const uint32_t *>(arguments->structureInput);
        if(header[1]==0xffffffffu) {
            native_adreno_metal_diag.probes=native_adreno_metal_diag.probes+1;
            return kIOReturnSuccess;
        }
        native_adreno_metal_diag.tunnel_calls=native_adreno_metal_diag.tunnel_calls+1;
        const IOReturn tunneled=run(header[1],reinterpret_cast<const uint8_t *>(arguments->structureInput)+8,
            arguments->structureInputSize-8,arguments->structureOutput,arguments->structureOutputSize);
        native_adreno_metal_diag.ext_status=uint32_t(tunneled);
        return tunneled;
    }
    native_adreno_metal_diag.ext_selector=selector;
    native_adreno_metal_diag.ext_in=arguments?arguments->structureInputSize:0xffff;
    native_adreno_metal_diag.ext_out=arguments?arguments->structureOutputSize:0xffff;
    if(selector<1 || selector>sizeof(table)/sizeof(table[0])) {
        native_adreno_metal_diag.ext_status=uint32_t(kIOReturnUnsupported);
        return kIOReturnUnsupported;
    }
    const IOReturn result=IOUserClient::externalMethod(selector,arguments,const_cast<IOExternalMethodDispatch *>(&table[selector-1]),
        target?target:this,reference);
    native_adreno_metal_diag.ext_status=uint32_t(result);
    return result;
}
/* limit > 0 caps the connections ever granted: a plugin that crashes its host process on every launch (the process respawns and
 * loads it again) must not be able to keep a system process in a crash loop, so after the cap further opens are refused. */
extern "C" IOReturn native_adreno_new_user_client(IOService *provider,task_t owningTask,void *securityID,UInt32 type,
    OSDictionary *properties,IOUserClient **handler,unsigned limit)
{
    if(handler)*handler=nullptr;
    if(!handler || !g.published)return kIOReturnNotReady;
    /* Progress markers: open types 0x4147xxxx (xxxx nonzero) are not connections. The plugin sends one before and after each step so
     * that a sandbox that admits neither selectors nor mapping, or a crash, still leaves a trace of how far it got. */
    if((type>>16)==NativeAdrenoPluginOpenType && (type&0xffff)) {
        const uint32_t value=type&0xffff,slot=native_adreno_metal_diag.marks_total&15;
        /* Text stream into report2, one character per marker: 0x7fff starts a line, 0x70cc appends character cc. It needs no connection,
         * so the plugin's Turnip watchdog can report while Turnip itself holds its connection slot. Not counted as markers. */
        if(value>=0x8000) {
            static unsigned row=48,length;
            if(value>=0xC000) {
                row=value&0x3f;length=0;
                if(row<48)memset(native_adreno_metal_rows[row],0,sizeof(native_adreno_metal_rows[row]));
                native_adreno_metal_rows_seq=native_adreno_metal_rows_seq+1;
            } else if(row<48) {
                const char pair[2]={char((value>>7)&0x7f),char(value&0x7f)};
                for(unsigned i=0;i<2;++i)if(pair[i] && length<sizeof(native_adreno_metal_rows[row])-1)native_adreno_metal_rows[row][length++]=pair[i];
            }
            return kIOReturnUnsupported;
        }
        if(value>=0x7000 && value<0x8000) {
            /* 0x7ffe starts a line in report (GPU7) instead: the Metal bridge's result, after the raw test's line has been read. */
            static uint32_t length;
            static char *line=native_adreno_metal_diag.report2;
            static_assert(sizeof(native_adreno_metal_diag.report)==sizeof(native_adreno_metal_diag.report2),"stream lines");
            if(value==0x7fff || value==0x7ffe) {
                line=value==0x7fff?native_adreno_metal_diag.report2:native_adreno_metal_diag.report;
                /* A line from a process other than the render server goes to that process's own row (native_adreno_client_text): the render server
                 * rewrites report2 every ten seconds, so the reason another process's device failed would be gone from it. */
                char who[20]={};
                proc_selfname(who,sizeof who);
                if(value==0x7fff && strncmp(who,"backboardd",sizeof who)) {
                    for(auto &entry:native_adreno_client_text) {
                        if(!entry.name[0]) strlcpy(entry.name,who,sizeof entry.name);
                        if(!strncmp(entry.name,who,sizeof entry.name)) {line=entry.text;break;}
                    }
                }
                memset(line,0,sizeof(native_adreno_metal_diag.report2));length=0;
            }
            else if(value<0x7100 && length<sizeof(native_adreno_metal_diag.report2)-1) {line[length++]=char(value&0xff);line[length]=0;}
            return kIOReturnUnsupported;
        }
        if(value>=0x6601 && value<=0x66ff) {
            char who[20]={};proc_selfname(who,sizeof who);
            if(!strcmp(who,"adrenometalprobe")) {
                /* The current probe already truncated its own stdout before
                 * this RPC. Only now may the worker parse that file. */
                __atomic_store_n(&gHoujiProbeStarted,1u,__ATOMIC_RELEASE);
                if(value==0x66fe || value==0x66ff)__atomic_store_n(&gHoujiProbeResult,value==0x66fe?1u:2u,__ATOMIC_RELEASE);
            }
        }
        native_adreno_metal_diag.marks[slot]=value;
        native_adreno_metal_diag.marks_total=native_adreno_metal_diag.marks_total+1;
        if(value==1) {
            memcpy(native_adreno_metal_diag.starter[2],native_adreno_metal_diag.starter[1],17);
            memcpy(native_adreno_metal_diag.starter[1],native_adreno_metal_diag.starter[0],17);
            proc_selfname(native_adreno_metal_diag.starter[0],17);
        }
        return kIOReturnUnsupported;
    }
    shiftName(native_adreno_metal_diag.opener);
    AGXDeviceUserClient *connection=new AGXDeviceUserClient;
    if(!connection)return kIOReturnNoMemory;
    if(!connection->initWithTask(owningTask,securityID,type,properties)) {
        connection->release();
        native_adreno_metal_diag.bad=native_adreno_metal_diag.bad+1;
        return kIOReturnNoResources;
    }
    /* IOService::newUserClient attaches and starts what it creates (a client without a provider chain is also a reason for the sandbox to
     * refuse). Then the two sandbox gates IOServiceOpen would apply afterwards are run here for every opener and logged with the
     * process name and open type, so the boot page shows which processes the sandbox lets reach this class and with which type. */
    unsigned attach_state=0;
    if(provider) {
        const bool attached=connection->attach(provider);
        attach_state=attached?1:2;
        if(attached && !connection->start(provider)) {
            connection->detach(provider);
            attach_state=3;
        }
    }
    native_adreno_metal_diag.attach_status=attach_state;
    const uint32_t macf=uint32_t(mac_iokit_check_open(kauth_cred_get(),connection,type));
    uint32_t resolver=0xfeedfeed;
    if(gIOUCFilterCallbacks && gIOUCFilterCallbacks->io_filter_resolver) {
        io_filter_policy_t policy=0;
        const IOReturn resolved=gIOUCFilterCallbacks->io_filter_resolver(owningTask,connection,type,&policy);
        resolver=uint32_t(resolved);
        if(resolved==kIOReturnSuccess && policy && gIOUCFilterCallbacks->io_filter_release)gIOUCFilterCallbacks->io_filter_release(policy);
    }
    native_adreno_metal_diag.chk_type=type;
    native_adreno_metal_diag.chk_macf=macf;
    native_adreno_metal_diag.chk_res=resolver;
    {
        NativeAdrenoOpenRecord &record=native_adreno_open_log[native_adreno_open_log_count&7];
        proc_selfname(record.name,sizeof(record.name));
        record.type=type;record.macf=macf;record.res=resolver;record.attach=attach_state;record.foreign=type!=NativeAdrenoPluginOpenType;
        native_adreno_open_log_count=native_adreno_open_log_count+1;
    }
    if(type==NativeAdrenoHandoffOpenType) {
        connection->setHandoffOnly();
        *handler=connection;
        return kIOReturnSuccess;
    }
    /* Only the Adreno plugin's open (type NativeAdrenoPluginOpenType) is served. The original IOGPU library opens the accelerator with type 1
     * (and an API value in the upper bits) for an IOGPUDeviceUserClient, which this driver is not; refuse it cleanly. */
    if(type!=NativeAdrenoPluginOpenType) {
        connection->release();
        native_adreno_metal_diag.foreign_opens=native_adreno_metal_diag.foreign_opens+1;
        native_adreno_metal_diag.foreign_type=type;
        return kIOReturnUnsupported;
    }
    /* Every process that runs the Adreno plugin opens with its type: on Apple hardware every Metal process opens the AGX user client, and the
     * original profiles here name no class of this driver for the render server (MACF 1 for the plugin's types at every boot since int-038).
     * The plugin's two open types are granted here, by this driver: the grant is a property of the client object that the kernel's open path
     * (patches/xnu/native-adreno-sandbox-grant.patch) honours in place of the sandbox verdict. The verdict is still taken above and logged.
     * Which processes run the plugin at all is the plugin's allow-list (NativeAdrenoMetal.clients). */
    const bool kernel=owningTask==kernel_task;
    char opener[20]={};
    proc_selfname(opener,sizeof opener);
    if(!kernel && (macf || resolver!=uint32_t(kIOReturnSuccess))) {
        /* Original kernel sandbox verdict remains authoritative; no grant patch. */
        native_adreno_metal_diag.grants=native_adreno_metal_diag.grants+1;
    }
    /* limit caps the connections granted to each process name, so one process that crashes on every launch cannot loop forever, and cannot use
     * up the others' opens; a process other than the render server gets at most eight per boot. */
    const unsigned cap=strncmp(opener,"backboardd",sizeof opener)?(limit && limit<8?limit:8):limit;
    nativeAdrenoIssuerLock(__LINE__);
    NativeAdrenoClientRecord *record=kernel?nullptr:clientRecord(opener);
    const bool capped=cap && record && record->opens>=cap;
    nativeAdrenoIssuerUnlock();
    IOReturn refusal=kIOReturnSuccess;
    if(capped)refusal=kIOReturnBusy;
    else if(!connection->mount(kernel))refusal=kIOReturnBusy;   /* every slot taken */
    if(refusal) {
        connection->release();
        native_adreno_metal_diag.busy=native_adreno_metal_diag.busy+1;
        nativeAdrenoIssuerLock(__LINE__);
        if(record) {record->refused=record->refused+1;record->last_refusal=uint32_t(refusal);}
        nativeAdrenoIssuerUnlock();
        return refusal;
    }
    nativeAdrenoIssuerLock(__LINE__);
    if(record)record->opens=record->opens+1;
    nativeAdrenoIssuerUnlock();
    native_adreno_metal_diag.opens=native_adreno_metal_diag.opens+1;
    *handler=connection;
    return kIOReturnSuccess;
}
extern "C" void native_adreno_note_property_read(const char *key)
{
    if(key && !strcmp(key,"MetalPluginName")) {
        proc_selfname(native_adreno_metal_diag.name_reader,17);
        native_adreno_metal_diag.name_reads=native_adreno_metal_diag.name_reads+1;
        char name[20]={};
        proc_selfname(name,sizeof name);
        /* Unlocked: the discovery runs at process start, a lost count is harmless and a row is claimed by its first byte. */
        for(auto &asker:native_adreno_metal_askers) {
            if(!asker.name[0]) {strlcpy(asker.name,name,sizeof asker.name);asker.reads=1;break;}
            if(!strncmp(asker.name,name,sizeof asker.name)) {asker.reads=asker.reads+1;break;}
        }
    }
    shiftName(native_adreno_metal_diag.reader);
    native_adreno_metal_diag.property_reads=native_adreno_metal_diag.property_reads+1;
}
IOReturn NativeAdrenoGpuPublished::newUserClient(task_t owningTask,void *securityID,UInt32 type,
    OSDictionary *properties,IOUserClient **handler)
{
    return native_adreno_new_user_client(this,owningTask,securityID,type,properties,handler,0);
}

/* Publishes the service once. Everything passed in must outlive the system (the board probe keeps them in statics) and
 * the VM, submit object and ring must already be attached to the live GPU context. The core must not be open. */
extern "C" IOReturn native_adreno_publish(NativeAdrenoVm *vm,NativeAdrenoSubmit *submit,NativeAdrenoRing *ring,
    const NativeAdrenoRingClock *clock,const NativeAdrenoTimer *timer,const NativeAdrenoAbiDeviceInfo *info,
    uint32_t input_bits,uint32_t output_bits)
{
    if(g.published || !vm || !submit || !ring || !clock || !clock->now_us)return kIOReturnBadArgument;
    for(auto w:kClientWindows)if(!native_adreno_board_va_window(w.start,w.bytes))return kIOReturnBadArgument;
    g.lock=IOLockAlloc();
    NativeAdrenoGpuPublished *service=new NativeAdrenoGpuPublished;
    if(!g.lock || !service || !service->init() || !service->attach(IOService::getPlatform())) {
        if(service)service->release();
        return kIOReturnNoMemory;
    }
    g.vm=vm;g.submit=submit;g.ring=ring;g.clock=*clock;
    unsigned trace=0;
    nativeAdrenoIssuerTrace.enabled=PE_parse_boot_argn("gtrace",&trace,sizeof trace) && trace==1;
    __atomic_store_n(&native_adreno_memory_now_us,+[]()->uint64_t{return g.clock.now_us(g.clock.cookie);},__ATOMIC_RELEASE);
    g.timer=timer;g.info=info;g.input_bits=input_bits;g.output_bits=output_bits;
    __atomic_store_n(&nativeAdrenoHintIssued,g.ring->lastSubmitted(),__ATOMIC_RELEASE);
    __atomic_store_n(&nativeAdrenoHintEnabled,1u,__ATOMIC_RELEASE);
    g.service=service;g.published=true;
    service->setName("NativeAdrenoGpu");
    service->registerService();
    return kIOReturnSuccess;
}
extern "C" uint32_t native_adreno_publish_stats(uint32_t *closed)
{
    if(closed)*closed=g.closed;
    return g.opened;
}
extern "C" IOService *native_adreno_published_service()
{
    return g.service;
}

/* Same owner gate as each external method. No filesystem takes this lock. */
extern "C" IOReturn native_adreno_provider_poll(void){if(!g.published || !g.lock || g.dying)return kIOReturnNotReady;nativeAdrenoIssuerLock(__LINE__);IOReturn s=native_adreno_board_quiesce();nativeAdrenoIssuerUnlock();return s;}
extern "C" void houji_touch_driver_report(void);
extern "C" void houji_framebuffer_history_report(void);
extern "C" void houji_cpu_dvfs_history_report(void);
extern "C" void houji_usb_report(void);
extern "C" void houji_gpu_dvfs_history_report(void);
extern "C" volatile uint32_t native_adreno_client_stats[NativeAdrenoMaxClients+1][6];
extern "C" volatile uint32_t native_adreno_window_overflow[NativeAdrenoMaxClients][3];
// Bounded cached observations for the periodic log snapshot, before teardown.
// Try the existing lock only for copying text; never hold it while printing.
extern "C" unsigned houji_gpu_dvfs_snapshot(char *,unsigned);
extern "C" unsigned houji_gpu_gate_snapshot(char *,unsigned);
extern "C" unsigned native_adreno_present_sync_snapshot(char *,unsigned);
extern "C" unsigned houji_gpu_cached_timing(char *out,unsigned capacity) {
 if(!out || !capacity)return 0;
 static char rows[48][96];static NativeAdrenoClientRecord clients[8];static uint32_t scratch_busy;
 if(__atomic_exchange_n(&scratch_busy,1u,__ATOMIC_ACQUIRE)){
  int n=snprintf(out,capacity,"HOUJI CACHED TIMING: scratch busy; counters omitted\n");
  return n>0?((unsigned)n<capacity?(unsigned)n:capacity-1):0;
 }
 unsigned used=0;out[0]=0;bzero(rows,sizeof rows);bzero(clients,sizeof clients);bool copied=false;
 if(g.lock && nativeAdrenoIssuerTryLock(__LINE__)) {
  memcpy(rows,native_adreno_metal_rows,sizeof rows);
  memcpy(clients,native_adreno_client_log,sizeof clients);
  nativeAdrenoIssuerUnlock();copied=true;
 }
 auto append=[&](const char *format,auto... args){
  if(used>=capacity-1)return;int n=snprintf(out+used,capacity-used,format,args...);
  if(n>0)used+=(unsigned)n<capacity-used?(unsigned)n:capacity-used-1;
 };
 append("HOUJI CACHED TIMING: bridge-copied=%u; RAM only; snapshot header before kernel tail\n",copied);
 {uint64_t v[4];for(unsigned i=0;i<4;++i)v[i]=__atomic_load_n(&native_adreno_submit_batches[i],__ATOMIC_RELAXED);
  append("HOUJI CACHED BATCH: jobs=%llu IBs=%llu max=%llu over16=%llu\n",v[0],v[1],v[2],v[3]);}
 const char *phases[]={"all","wc-reassert","publish","audit"};
 for(unsigned phase=0;phase<4;++phase){
  uint64_t v[3];for(unsigned i=0;i<3;++i)v[i]=__atomic_load_n(&native_adreno_submit_cost[phase*3+i],__ATOMIC_RELAXED);
  append("HOUJI CACHED SUBMIT: %s n=%llu us=%llu max=%llu\n",phases[phase],v[0],v[1],v[2]);
 }
 append("HOUJI CACHED WC: coherent=%u audit=%u dirty=%u stale=%u failures=%u resets=%u\n",
  native_adreno_io_coherent,native_adreno_wc_audit[0],native_adreno_wc_audit[2],native_adreno_wc_audit[3],native_adreno_wc_audit[4],native_adreno_wc_audit[5]);
 append("HOUJI CACHED IMPORTS: NC-fallbacks=%u; failed WB alias preflight only\n",native_adreno_coherent_import_fallbacks);
 append("HOUJI CACHED TLB: per-page=%u; unmap retains ASID scope and TLBSYNC\n",g.vm?g.vm->perPageTlbInvalidation():1);
 append("HOUJI BO POOL: requested-MiB=%u reserve=%x WC=%x MiB=%u hits=%u fallback=%u used-MiB=%u peak-MiB=%u standalone=%u spans=%u import-spans=%u; retained CPU-page ownership\n",
  native_adreno_pool_mib,native_adreno_pool_status,native_adreno_pool_wc,native_adreno_pool_stats[0],native_adreno_pool_stats[1],native_adreno_pool_stats[2],
  native_adreno_pool_stats[3],native_adreno_pool_stats[4],native_adreno_pool_stats[5],native_adreno_pool_stats[6],native_adreno_pool_stats[7]);
 nativeAdrenoLatency.snapshot(append);
 nativeAdrenoIssuerTrace.snapshot(append);
 append("HOUJI FENCE POLL: calls=%llu pauses=%llu unlocked-pending=%llu max-us=%llu; hints never authorize completion\n",__atomic_load_n(&native_adreno_wait_outer[0],__ATOMIC_RELAXED),__atomic_load_n(&native_adreno_wait_outer[2],__ATOMIC_RELAXED),__atomic_load_n(&native_adreno_wait_outer[4],__ATOMIC_RELAXED),__atomic_load_n(&native_adreno_wait_outer[3],__ATOMIC_RELAXED));
 append("HOUJI DISPATCH WAIT: BO-retries=%llu submit-retries=%llu mutation-reservation=1; pending waits outside issuer lock, original drain/proof retained\n",
  __atomic_load_n(&nativeAdrenoGateRetries[0],__ATOMIC_RELAXED),__atomic_load_n(&nativeAdrenoGateRetries[1],__ATOMIC_RELAXED));
 nativeAdrenoAllocationTrace.snapshot(append);
 used+=houji_gpu_gate_snapshot(out+used,capacity-used);
 used+=native_adreno_present_sync_snapshot(out+used,capacity-used);
 used+=houji_gpu_dvfs_snapshot(out+used,capacity-used);
 for(const auto &client:clients)if(copied && client.name[0]) {
  unsigned window=client.slot<NativeAdrenoMaxClients?(unsigned)(kClientWindows[client.slot].bytes>>20):0;
  unsigned base=client.slot<NativeAdrenoMaxClients?(unsigned)(kClientWindows[client.slot].start>>20):0;
  append("HOUJI CLIENT WINDOW: name=%.20s slot=%u highest-address-MiB=%u base-MiB=%u window-MiB=%u refused=%u last=%x; address high-water mark, not allocated bytes\n",
   client.name,client.slot,client.peak_mib,base,window,client.refused,client.last_refusal);
 }
 for(unsigned row=0;row<48;++row)if(copied && rows[row][0])append("HOUJI CACHED METAL %u: %.95s\n",row,rows[row]);
 __atomic_store_n(&scratch_busy,0u,__ATOMIC_RELEASE);
 return used;
}

/* One snapshot per displayed frame for the hitch log: [0..5] gate count/total/wait/audit-us, GPU level, GPU busy-us;
 * [6..13] map/free/submit/alloc calls and us (map n,us, free n,us, submit n,us, alloc n, fence-wait us);
 * [14] full fallbacks, [15] depth waits. */
extern "C" void native_adreno_hitch_snapshot(uint64_t out[16]){
 houji_gate_hitch_snapshot(out);
 auto load=[](unsigned m,unsigned i){return __atomic_load_n(&nativeAdrenoDispatchPerf[m][i],__ATOMIC_RELAXED);};
 out[6]=load(NativeAdrenoMethodBoMap,0);out[7]=load(NativeAdrenoMethodBoMap,1);
 out[8]=load(NativeAdrenoMethodBoFree,0);out[9]=load(NativeAdrenoMethodBoFree,1);
 out[10]=load(NativeAdrenoMethodSubmit,0);out[11]=load(NativeAdrenoMethodSubmit,1);
 out[12]=load(NativeAdrenoMethodBoAlloc,0);out[13]=native_adreno_wait_outer[1];
 out[14]=native_adreno_pipeline_stats[2];out[15]=native_adreno_pipeline_stats[3];
}
/* Live profile for the USB log: cumulative per-method cost and the Metal
 * plugin's own telemetry rows. Reads counters only; no lock, no GPU access. */
extern "C" void native_adreno_perf_report(void){
 for(unsigned method=0;method<20;++method){
  uint64_t v[4];for(unsigned i=0;i<4;++i)v[i]=__atomic_load_n(&nativeAdrenoDispatchPerf[method][i],__ATOMIC_RELAXED);
  if(v[0])IOLog("HOUJI ABI METHOD: id=%u calls=%llu total-us=%llu max-us=%llu no-drain=%llu; live\n",method,v[0],v[1],v[2],v[3]);
 }
 {
  const char *phases[]={"all","wc-reassert","publish","audit"};
  for(unsigned phase=0;phase<4;++phase){
   uint64_t v[3];for(unsigned i=0;i<3;++i)v[i]=__atomic_load_n(&native_adreno_submit_cost[phase*3+i],__ATOMIC_RELAXED);
   IOLog("HOUJI SUBMIT COST: phase=%s calls=%llu total-us=%llu max-us=%llu; live\n",phases[phase],v[0],v[1],v[2]);
  }
  IOLog("HOUJI COHERENT SKIPS: %u/%u/%u/%u wc-trusted-audit=%u/%u/%u/%u reset=%u distrusted=%u; live\n",native_adreno_coherent_skips[0],native_adreno_coherent_skips[1],native_adreno_coherent_skips[2],native_adreno_coherent_skips[3],native_adreno_wc_audit[0],native_adreno_wc_audit[1],native_adreno_wc_audit[2],native_adreno_wc_audit[3],native_adreno_wc_audit[5],native_adreno_wc_distrusted);
 }
 {
  IOLog("HOUJI PIPELINE: light-submits=%llu light-other=%llu full-fallbacks=%llu depth-waits=%llu fault-refusals=%llu TLB-reused=%llu TLB-invalidated=%llu jobs=%lu; live\n",
   native_adreno_pipeline_stats[0],native_adreno_pipeline_stats[1],native_adreno_pipeline_stats[2],native_adreno_pipeline_stats[3],native_adreno_pipeline_stats[4],
   native_adreno_tlb_stats[0],native_adreno_tlb_stats[1],g.submit?g.submit->outstanding():0ul);
 }
 {
  const char *names[12]={"retire","acquire-imported","hold","sample","ring","tlb","depth-wait","light-gate","ring-rptr","ring-emit","ring-publish","ring-wptr"};
  for(unsigned i=0;i<12;i++)IOLog("HOUJI SUBMIT PART: %s n=%llu us=%llu; live\n",names[i],native_adreno_submit_parts[i][0],native_adreno_submit_parts[i][1]);
 }
 houji_surface_lazy_report();
 houji_power_report();
 houji_cp_lease_report();
 IOLog("HOUJI PROLOGUE: light=%llu full=%llu; live\n",native_adreno_prologue_stats[0],native_adreno_prologue_stats[1]);
 IOLog("HOUJI DVFS LOCK: skipped-ticks=%llu retries=%llu; live\n",native_adreno_dvfs_skips[0],native_adreno_dvfs_skips[1]);
 IOLog("HOUJI WPTR: writes=%llu retried=%llu retries=%llu loop-us=%llu; live\n",native_adreno_wptr_stats[0],native_adreno_wptr_stats[1],native_adreno_wptr_stats[2],native_adreno_wptr_stats[3]);
 IOLog("HOUJI WAIT OUTER: waits=%llu total-us=%llu pauses=%llu max-us=%llu; live\n",native_adreno_wait_outer[0],native_adreno_wait_outer[1],native_adreno_wait_outer[2],native_adreno_wait_outer[3]);
 for(unsigned row=0;row<48;row++)if(native_adreno_metal_rows[row][0])IOLog("HOUJI METAL ROW %u: %.95s\n",row,native_adreno_metal_rows[row]);
}
static void nativeAdrenoStopDiagnostics(void){
 const char *phases[]={"all","wc-reassert","publish","audit"};
 IOLog("HOUJI CACHE POLICY: coherent=%u skipped=%u/%u/%u/%u wc-reset=%u distrusted=%u dirty=%u stale=%u NC-import-fallbacks=%u; cached observations only\n",native_adreno_io_coherent,native_adreno_coherent_skips[0],native_adreno_coherent_skips[1],native_adreno_coherent_skips[2],native_adreno_coherent_skips[3],native_adreno_wc_audit[5],native_adreno_wc_distrusted,native_adreno_wc_audit[2],native_adreno_wc_audit[3],native_adreno_coherent_import_fallbacks);
 for(unsigned phase=0;phase<4;++phase){
  uint64_t v[3];for(unsigned i=0;i<3;++i)v[i]=__atomic_load_n(&native_adreno_submit_cost[phase*3+i],__ATOMIC_RELAXED);
  IOLog("HOUJI SUBMIT COST: phase=%s calls=%llu total-us=%llu max-us=%llu; CPU wall time, original validation and cache policy\n",phases[phase],v[0],v[1],v[2]);
 }
 houji_cpu_scheduler_report();
 houji_cpu_dvfs_history_report();
 houji_usb_report();
 houji_gpu_dvfs_history_report();
 houji_sandbox_abi_report();
 for(unsigned method=0;method<20;++method) {
  uint64_t v[4];for(unsigned i=0;i<4;++i)v[i]=__atomic_load_n(&nativeAdrenoDispatchPerf[method][i],__ATOMIC_RELAXED);
  if(v[0])IOLog("HOUJI ABI METHOD: id=%u calls=%llu total-us=%llu max-us=%llu no-drain=%llu; lock held, original core validation\n",method,v[0],v[1],v[2],v[3]);
 }

 houji_touch_driver_report();
 houji_framebuffer_history_report();
 for(unsigned slot=0;slot<NativeAdrenoMaxClients;slot++){
  const auto&s=native_adreno_client_stats[slot];const auto&w=native_adreno_window_overflow[slot];
  IOLog("HOUJI GPU ALLOC: slot=%u live=%u peak-objects=%u failed=%u status=%x last-bytes=%u window-denied=%u last-map-KiB=%u last-end-MiB=%u; cached counters, not RAM usage\n",slot,s[0],s[1],s[2],s[3],s[4],w[0],w[1],w[2]);
 }
 const auto&d=native_adreno_metal_diag;
 IOLog("HOUJI USERLAND ABI: opens=%u busy=%u bad=%u closes=%u calls=%u selector=%u status=%x inflight=%u ext=%u/%u/%x bytes=%u/%u marks=%u mount=%u/%x/%u; RAM diagnostic before real reset\n",d.opens,d.busy,d.bad,d.closes,d.calls,d.last_selector,d.last_status,d.inflight,d.ext_entries,d.ext_selector,d.ext_status,d.ext_in,d.ext_out,d.marks_total,native_adreno_mount_fail[0],native_adreno_mount_fail[1],native_adreno_mount_fail[2]);
 IOLog("HOUJI USERLAND METAL: report=%.63s report2=%.63s views=%u/%u/%u refused=%u last=%x openers=%.16s/%.16s/%.16s\n",d.report,d.report2,native_adreno_view_diag[0],native_adreno_view_diag[1],native_adreno_view_diag[2],native_adreno_view_diag[3],native_adreno_view_diag[4],d.opener[0],d.opener[1],d.opener[2]);
 for(unsigned i=0;i<16;i++)if(d.marks[i])IOLog("HOUJI USERLAND MARK: slot=%u value=%x\n",i,d.marks[i]);
 for(unsigned row=0;row<48;row++)if(native_adreno_metal_rows[row][0])IOLog("HOUJI METAL ROW %u: %.95s\n",row,native_adreno_metal_rows[row]);
 for(const auto&text:native_adreno_client_text)if(text.name[0])IOLog("HOUJI USERLAND CLIENT TEXT: name=%.19s text=%.63s\n",text.name,text.text);
 for(const auto&rec:native_adreno_client_log)if(rec.name[0]){
  IOLog("HOUJI USERLAND CLIENT: name=%.19s opens=%u refused=%u closes=%u slot=%u peak-MiB=%u last=%x\n",rec.name,rec.opens,rec.refused,rec.closes,rec.slot,rec.peak_mib,rec.last_refusal);
  if(rec.slot<NativeAdrenoMaxClients){const auto&w=kClientWindows[rec.slot];IOLog("HOUJI GPU VA WINDOW: name=%.19s slot=%u base=%llx end=%llx bytes=%llu high-water-end-MiB=%u; GPU virtual addresses, not physical RAM usage\n",rec.name,rec.slot,w.start,w.start+w.bytes,w.bytes,rec.peak_mib);}
 }
}

extern "C" int native_adreno_provider_stop(int(*stop)(void*),void*c){if(!g.lock || !stop)return 1;nativeAdrenoIssuerLock(__LINE__);__atomic_store_n(&nativeAdrenoHintEnabled,0u,__ATOMIC_RELEASE);g.dying=true;g.published=false;(void)native_adreno_board_quiesce();nativeAdrenoStopDiagnostics();int s=stop(c);nativeAdrenoIssuerUnlock();return s;}
extern "C" IOMemoryDescriptor *native_gop_handoff_memory(void){return nullptr;}
extern "C" IOReturn native_gop_handoff_call(uint32_t,task_t,const void*,size_t,void*,size_t){return kIOReturnUnsupported;}

extern "C" IOReturn native_adreno_provider_try_poll(void){
 if(!g.published || !g.lock || g.dying)return kIOReturnNotReady;
 if(!nativeAdrenoIssuerTryLock(__LINE__))return kIOReturnBusy;
 /* No work can be added while holding this gate. VM/BO mutations and
  * submissions still enter the strict board quiesce/VM path. Avoid full
  * idle audits in a diagnostic
  * timer when the real submit object has no outstanding jobs. */
 if(g.submit && !g.submit->outstanding()){nativeAdrenoIssuerUnlock();return kIOReturnSuccess;}
 IOReturn result=native_adreno_board_quiesce();nativeAdrenoIssuerUnlock();return result;
}
extern "C" unsigned native_adreno_provider_probe_result(void){return __atomic_load_n(&gHoujiProbeResult,__ATOMIC_ACQUIRE);}
extern "C" void native_adreno_provider_snapshot(void){
 const auto&d=native_adreno_metal_diag;
 IOLog("HOUJI USERLAND PROGRESS: calls=%u selector=%u status=%x inflight=%u ext=%u/%x marker=%x result=%u opens=%u closes=%u; RAM observation, no GPU access or gate\n",d.calls,d.last_selector,d.last_status,d.inflight,d.ext_selector,d.ext_status,d.marks_total?d.marks[(d.marks_total-1)&15]:0,native_adreno_provider_probe_result(),d.opens,d.closes);
}

extern "C" unsigned native_adreno_provider_probe_started(void){return __atomic_load_n(&gHoujiProbeStarted,__ATOMIC_ACQUIRE);}
extern "C" uint64_t native_adreno_provider_progress_token(void){const auto&d=native_adreno_metal_diag;return ((uint64_t)d.marks_total<<32)|((uint64_t)d.inflight<<16)|d.last_selector;}

extern "C" IOReturn native_adreno_provider_desktop_stats(uint64_t out[7]){
 if(!out || !g.published || !g.lock || g.dying)return kIOReturnNotReady;
 if(!nativeAdrenoIssuerTryLock(__LINE__))return kIOReturnBusy;
 IOReturn result=native_adreno_board_quiesce();
 if(!result){
  out[0]=__atomic_load_n(&gHoujiDesktopSubmits,__ATOMIC_ACQUIRE);out[1]=__atomic_load_n(&gHoujiDesktopWaits,__ATOMIC_ACQUIRE);out[2]=__atomic_load_n(&gHoujiDesktopSubmitErrors,__ATOMIC_ACQUIRE);out[3]=__atomic_load_n(&gHoujiDesktopLastFence,__ATOMIC_ACQUIRE);
  out[4]=g.ring->completedFence();out[5]=g.submit->outstanding();out[6]=g.ring->lastSubmitted();
 }
 nativeAdrenoIssuerUnlock();return result;
}

/* Retain the qualified producer AND current ring-tail fence before a CPU read.
 * A pending copy sleeps outside the issuer mutex until an existing ABI
 * observer sees progress, with a bounded timer fallback. Observed completion
 * can resume the accepted frame without waiting for the next display tick.
 * Success keeps the mutex through cache maintenance/copy, excluding later
 * GPU writes. No reference is retired here; this is not a GPU IRQ or vblank.
 */
extern "C" unsigned native_adreno_present_sync_snapshot(char *out,unsigned capacity){
 if(!out || !capacity)return 0;
 unsigned long long v[14];for(unsigned i=0;i<14;i++)v[i]=__atomic_load_n(&gHoujiPresentSync[i],__ATOMIC_RELAXED);
 int n=snprintf(out,capacity,"HOUJI PRESENT SYNC: attempts=%llu granted=%llu gate-busy=%llu GPU-pending=%llu observed-fence=%llu max-hold-us=%llu lock-wait-us=%llu max-lock-wait-us=%llu; cached CPU observations\nHOUJI PRESENT WAKE: sleeps=%llu event-wakes=%llu expired=%llu sleep-us=%llu signals=%llu; pending checks may repeat; no GPU IRQ or panel vblank\n",v[0],v[1],v[2],v[3],v[4],v[6],v[7],v[8],v[9],v[10],v[11],v[12],v[13]);
 if(n<=0 || (unsigned)n>=capacity){out[0]=0;return 0;}return (unsigned)n;
}
extern "C" int native_adreno_present_begin_frame(uint32_t desired){
 __atomic_fetch_add(&gHoujiPresentSync[0],1ull,__ATOMIC_RELAXED);
 if(!g.published || !g.lock || !g.ring)return 0;
 const uint64_t began=g.clock.now_us(g.clock.cookie);
 for(;;){
  if(!nativeAdrenoIssuerTryLock(__LINE__)){
   __atomic_fetch_add(&gHoujiPresentSync[2],1ull,__ATOMIC_RELAXED);
   uint64_t lockBegan=g.clock.now_us(g.clock.cookie);
   nativeAdrenoIssuerLock(__LINE__); /* normal priority-inheriting CPU mutex, no GPU wait */
   uint64_t elapsed=g.clock.now_us(g.clock.cookie)-lockBegan;
   __atomic_fetch_add(&gHoujiPresentSync[7],elapsed,__ATOMIC_RELAXED);
   uint64_t old=__atomic_load_n(&gHoujiPresentSync[8],__ATOMIC_RELAXED);
   while(elapsed>old && !__atomic_compare_exchange_n(&gHoujiPresentSync[8],&old,elapsed,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)){}
  }
  nativeAdrenoIssuerTrace.context(UINT32_MAX,UINT32_MAX,desired);
  if(g.dying || !native_adreno_board_running()){nativeAdrenoIssuerUnlock();return -1;}
  uint32_t fence=desired?desired:g.ring->lastSubmitted(),seen=0;
  auto result=fence?g.ring->observeFence(fence,nullptr,0,&seen):NativeAdrenoRingFenceComplete;
  // A completed producer does not cover newer writes to an alias. Re-read
  // the current tail on EVERY wake with the issuer mutex held.
  uint32_t tail=g.ring->lastSubmitted();
  if(result==NativeAdrenoRingFenceComplete && tail!=fence && tail)
   result=g.ring->observeFence(tail,nullptr,0,&seen);
  if(result==NativeAdrenoRingFenceComplete){
   __atomic_store_n(&gHoujiPresentSync[4],seen,__ATOMIC_RELAXED);
   __atomic_store_n(&gHoujiPresentSync[5],g.clock.now_us(g.clock.cookie),__ATOMIC_RELAXED);
   __atomic_fetch_add(&gHoujiPresentSync[1],1ull,__ATOMIC_RELAXED);return 1;
  }
  if(result!=NativeAdrenoRingFenceTimeout){nativeAdrenoIssuerUnlock();return -1;}
  __atomic_fetch_add(&gHoujiPresentSync[3],1ull,__ATOMIC_RELAXED);
  const uint64_t now=g.clock.now_us(g.clock.cookie),elapsed=now-began;
  if(elapsed>=8333){nativeAdrenoIssuerUnlock();return -1;}
  // Register under the notifier's mutex so a completion cannot be lost
  // between checking and sleeping. Release it BEFORE blocking. Clients
  // without a fence observer still get a bounded timeout, never a fake fence.
  assert_wait_timeout((event_t)&gHoujiPresentProgress,THREAD_UNINT,(uint32_t)(8333-elapsed),NSEC_PER_USEC);
  __atomic_fetch_add(&gHoujiPresentSync[9],1ull,__ATOMIC_RELAXED);
  nativeAdrenoIssuerUnlock();
  wait_result_t woke=thread_block(THREAD_CONTINUE_NULL);
  __atomic_fetch_add(&gHoujiPresentSync[woke==THREAD_AWAKENED?10:11],1ull,__ATOMIC_RELAXED);
  __atomic_fetch_add(&gHoujiPresentSync[12],g.clock.now_us(g.clock.cookie)-now,__ATOMIC_RELAXED);
 }
}
extern "C" void native_adreno_present_end(void){
 uint64_t elapsed=g.clock.now_us(g.clock.cookie)-__atomic_load_n(&gHoujiPresentSync[5],__ATOMIC_RELAXED);
 uint64_t old=__atomic_load_n(&gHoujiPresentSync[6],__ATOMIC_RELAXED);
 while(elapsed>old && !__atomic_compare_exchange_n(&gHoujiPresentSync[6],&old,elapsed,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)){}
 nativeAdrenoIssuerUnlock();
}
extern "C" void native_adreno_present_sync_report(void){
 uint64_t v[9];for(unsigned i=0;i<9;i++)v[i]=__atomic_load_n(&gHoujiPresentSync[i],__ATOMIC_RELAXED);
 IOLog("HOUJI PRESENT SYNC: attempts=%llu granted=%llu gate-busy=%llu GPU-pending=%llu observed-fence=%llu max-hold-us=%llu lock-wait-us=%llu max-lock-wait-us=%llu; queued CPU mutex, fenced copy\n",v[0],v[1],v[2],v[3],v[4],v[6],v[7],v[8]);
}

extern "C" int native_adreno_present_begin(void){return native_adreno_present_begin_frame(0);}
extern "C" uint32_t native_adreno_present_capture_fence(void){
 if(!g.published || !g.lock || !g.ring)return 0;
 nativeAdrenoIssuerLock(__LINE__);
 uint32_t fence=(!g.dying && native_adreno_board_running())?g.ring->lastSubmitted():0;
 nativeAdrenoIssuerUnlock();return fence;
}

extern "C" void houji_gpu_dvfs_tick(void);
/* Governor ticks skipped because submits held the lock for a whole retry window; live counter. */
extern "C" {volatile uint64_t native_adreno_dvfs_skips[2];}
extern "C" IOReturn native_adreno_dvfs_poll(void){
 if(!g.published || !g.lock || g.dying)return kIOReturnNotReady;
 // This dedicated worker may sleep. Queue behind the issuer rather than
 // losing every sample to a busy compositor and retaining an idle OPP.
 nativeAdrenoIssuerLock(__LINE__);
 if(!g.published || g.dying || !native_adreno_board_running()){nativeAdrenoIssuerUnlock();return kIOReturnNotReady;}
 houji_gpu_dvfs_tick();nativeAdrenoIssuerUnlock();return kIOReturnSuccess;
}
