/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoSubmit.h"
#include "AdrenoPM4.h"
#include <IOKit/IOLib.h>
// Optional board clock: host checks without a published driver collect no
// timings. Observations never change submission, references or cache policy.
extern "C" uint64_t (*native_adreno_vm_now_us)(void);
extern "C" {volatile uint64_t native_adreno_submit_cost[12];}
/* Accepted jobs, total IBs, largest batch, jobs with more than 16 IBs. */
extern "C" {volatile uint64_t native_adreno_submit_batches[4];}
static uint64_t submitDiagNow(void) {return native_adreno_vm_now_us?native_adreno_vm_now_us():0;}
static void submitDiagRecord(unsigned slot,uint64_t began) {
    if(!began)return;
    const uint64_t end=submitDiagNow();if(end<began)return;
    const uint64_t us=end-began;
    __atomic_fetch_add(&native_adreno_submit_cost[slot],1ull,__ATOMIC_RELAXED);
    __atomic_fetch_add(&native_adreno_submit_cost[slot+1],us,__ATOMIC_RELAXED);
    uint64_t peak=__atomic_load_n(&native_adreno_submit_cost[slot+2],__ATOMIC_RELAXED);
    while(us>peak && !__atomic_compare_exchange_n(&native_adreno_submit_cost[slot+2],&peak,us,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)){}
}
/* Finer parts, [part][0]=count [1]=us: 0 retireCompleted, 1 acquireImported (job release), 2 holdJob, 3 sample/peek words,
 * 4 ring write, 5 TLB step; 6 depth wait and 7 light gate are recorded by the board. */
extern "C" {volatile uint64_t native_adreno_submit_parts[12][2];volatile uint32_t native_adreno_light_prologue;}
extern "C" void native_adreno_submit_part(unsigned part,uint64_t began)
{
    if(!began || part>=12)return;
    const uint64_t end=submitDiagNow();if(end<began)return;
    __atomic_fetch_add(&native_adreno_submit_parts[part][0],1ull,__ATOMIC_RELAXED);
    __atomic_fetch_add(&native_adreno_submit_parts[part][1],end-began,__ATOMIC_RELAXED);
}
extern "C" uint64_t native_adreno_submit_part_now(void) {return submitDiagNow();}
struct PartScope {unsigned part;uint64_t began;
    explicit PartScope(unsigned p):part(p),began(submitDiagNow()){}
    ~PartScope(){native_adreno_submit_part(part,began);}};
struct SubmitDiagScope {unsigned slot;uint64_t began;
    explicit SubmitDiagScope(unsigned s):slot(s),began(submitDiagNow()){}
    ~SubmitDiagScope(){submitDiagRecord(slot,began);}};

/* CP_INDIRECT_BUFFER is opcode 0x3f with a 3-word payload: 64-bit GPU address
 * and IB_SIZE in dwords (bits 0..19), exactly as the pinned adreno_pm4.xml
 * domain states and Linux msm_gem_submit.c converts the userspace byte size. */
enum : uint32_t {SubmitIndirectBuffer=0x3f,SubmitIbWords=3,SubmitMaxJobs=8,
    SubmitMaxBuffers=NativeAdrenoSubmitMaxBuffers,SubmitMaxIbBytes=0xfffffu*4u,
    SubmitAuditBytes=256u*1024u};   /* IB bytes the write-combine audit compares per submission */
extern "C" {volatile uint32_t native_adreno_coherence[8];}
/* The submission (1-based count) at which the write-combine audit found a dirty IB line or failed; 0 = never. */
extern "C" {volatile uint32_t native_adreno_wc_distrusted;}
/* The write-combine audit before each submission while trusted: [0] submissions audited, [1] IB lines compared, [2] dirty lines,
 * [3] stale clean lines, [4] the last audit failure (any failure also ends the trust), [5] pages found back at the default attribute
 * and write-combined again. */
extern "C" {volatile uint32_t native_adreno_wc_audit[6];}
/* Fence waits: [0] calls, [1] timeouts, [2] the fence of the last timed-out wait, [3] the fence value the GPU had written then, [4] the newest fence handed out,
 * [5] the newest completed, [6] waits for a fence no job holds (observeFence path), [7] the last fence refused as never submitted. */
extern "C" {volatile uint32_t native_adreno_fence_wait[8];}
/* The IBs of the last submissions: [slot] fence, iova low/high, bytes, then the first 28 dwords of the first IB (read as the kernel maps it). */
extern "C" {volatile uint32_t native_adreno_ib_log[4][4+28];volatile uint32_t native_adreno_ib_log_count;}
IOReturn NativeAdrenoSubmit::poison()
{
    poisoned_=true;return kIOReturnIOError;
}
NativeAdrenoSubmit::Job *NativeAdrenoSubmit::find(uint32_t fence)
{
    if(fence)for(auto &job:jobs_)if(job.active && job.fence==fence)return &job;
    return nullptr;
}
NativeAdrenoSubmit::Job *NativeAdrenoSubmit::free()
{
    for(auto &job:jobs_)if(!job.active)return &job;
    return nullptr;
}
IOReturn NativeAdrenoSubmit::attach(NativeAdrenoVm *vm,NativeAdrenoRing *ring)
{
    if(attached_ || poisoned_ || vm_ || ring_)return kIOReturnBusy;
    if(!vm || !ring || vm->poisoned() || ring->poisoned())return kIOReturnBadArgument;
    vm_=vm;ring_=ring;attached_=true;stats_={};
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::release(Job &job)
{
    IOReturn status=kIOReturnSuccess;
    for(size_t i=0;i<job.count;++i) {
        IOReturn retired=vm_->retireJobAfterCompletion(job.handles[i]);
        if(retired)status=retired;else ++stats_.references_retired;
    }
    if(status)return poison();
    IOReturn acquired;
    {PartScope part(1);acquired=vm_->acquireImported();}
    if(acquired)return poison();
    job={};return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::detach()
{
    if(poisoned_ || outstanding())return kIOReturnBusy;
    if(!attached_)return kIOReturnNotReady;
    attached_=false;vm_=nullptr;ring_=nullptr;return kIOReturnSuccess;
}
bool NativeAdrenoSubmit::queueOutstanding(uint32_t queue) const
{
    return rangeOutstanding(queue,1);
}
bool NativeAdrenoSubmit::rangeOutstanding(uint32_t first,uint32_t count) const
{
    for(const auto &job:jobs_)if(job.active && job.queue-first<count)return true;
    return false;
}
size_t NativeAdrenoSubmit::outstanding() const
{
    size_t count=0;for(const auto &job:jobs_)if(job.active)++count;return count;
}
IOReturn NativeAdrenoSubmit::submit(const NativeAdrenoSubmitBuffer *buffers,size_t count,
    uint32_t queue,uint32_t *fence)
{
    SubmitDiagScope measured(0);
    if(!fence)return kIOReturnBadArgument;*fence=0;
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    if(!buffers || !count || count>SubmitMaxBuffers)return kIOReturnBadArgument;
    /* Every buffer is validated against the live VM before any reference. */
    uint64_t *handles=scratch_handles_;size_t handle_count=0;
    uint32_t *words=scratch_words_;size_t word_count=0;
    for(size_t i=0;i<count;++i) {
        const NativeAdrenoSubmitBuffer &buffer=buffers[i];
        if(!buffer.handle || !buffer.bytes || (buffer.bytes&3) || buffer.bytes>SubmitMaxIbBytes)
            return kIOReturnBadArgument;
        uint64_t iova=0;size_t bytes=0;unsigned permissions=0;
        IOReturn status=vm_->describe(buffer.handle,&iova,&bytes,&permissions);
        if(status)return status;
        if(!(permissions&ArmSmmu4KRead) || buffer.iova<iova ||
            buffer.iova-iova>bytes || buffer.bytes>bytes-(buffer.iova-iova))return kIOReturnBadArgument;
        uint32_t header=0;
        if(AdrenoPM4Type7(SubmitIndirectBuffer,SubmitIbWords,&header)!=AdrenoPM4Success)
            return poison();
        words[word_count++]=header;
        words[word_count++]=(uint32_t)buffer.iova;
        words[word_count++]=(uint32_t)(buffer.iova>>32);
        words[word_count++]=(uint32_t)(buffer.bytes/4);
        bool known=false;
        for(size_t j=0;j<handle_count;++j)if(handles[j]==buffer.handle) {known=true;break;}
        if(!known)handles[handle_count++]=buffer.handle;
    }
    IOReturn swept;
    {PartScope part(0);swept=retireCompleted();}
    if(swept)return swept;
    Job *job=free();
    if(!job)return kIOReturnBusy;
    const uint64_t hold_began=submitDiagNow();
    for(size_t i=0;i<handle_count;++i) {
        IOReturn status=vm_->holdJob(handles[i]);
        if(status) {
            while(i) {vm_->retireJobAfterCompletion(handles[--i]);++stats_.references_retired;}
            return status;
        }
        ++stats_.references_held;
    }
    native_adreno_submit_part(2,hold_began);
    uint32_t value=0;
    /* int-070: the first IB word as the kernel's cacheable alias and DRAM see it before any maintenance, then DRAM after the CPU
     * caches over every mapped BO are cleaned and invalidated. The board shows the last submission's record on a stall. */
    uint32_t cached=~0u,before=~0u,ignored=0,after=~0u;
    /* Write-combined stores this CPU made before the call (the client's command words) complete before anything below. */
    __asm__ volatile("dsb sy":::"memory");
    /* int-098/099: with write-combined user BOs and no maintenance the CP stopped at the end of the first IB, as in int-065..069 when an
     * IB sat in a CPU cache. While write-combined BOs are trusted, every line of this submission's IBs is compared as a CPU cache and
     * DRAM hold it (bounded). A dirty line means a client view still caches its BOs: maintain every BO from now on, this submission
     * included (the audit already wrote the IB lines back; the rest of the BOs may hold more). */
    if(vm_->writeCombinedTrusted()) {
        /* int-100: the dirty IB page was back at XNU's default (cacheable) attribute. Write-combine every such page again first; the
         * pmap writes its lines back on the way, so the audit below should then find none. */
        uint32_t reset=0;
        IOReturn reasserted;
        {SubmitDiagScope measured(3);reasserted=vm_->reassertWriteCombined(&reset);}
        native_adreno_wc_audit[5]=native_adreno_wc_audit[5]+reset;
        if(reasserted) {
            /* Mandatory page-attribute correction failed. Its error is not
             * the optional line-audit hook's Unsupported result. The VM has
             * quarantined the failing BO; preserve every held IB reference
             * and do not publish, invalidate, prepare or kick any GPU work. */
            native_adreno_wc_audit[4]=reasserted;
            vm_->distrustWriteCombined();
            native_adreno_wc_distrusted=native_adreno_coherence[0]+1;
            *job=Job{};++stats_.poisoned_jobs;
            for(size_t i=0;i<handle_count;++i)job->handles[i]=handles[i];
            job->count=handle_count;job->queue=queue;job->active=true;
            (void)poison();return reasserted;
        }
        IOReturn audited=kIOReturnSuccess;
        uint32_t lines=0,dirty=0,stale=0;size_t budget=SubmitAuditBytes;
        /* The re-assertion above is the correction and runs every time; the audit only detects, and found no dirty line in int-141's
         * 133798, so after the first 16 submissions it compares every eighth one's IBs (it cost about 2 ms each). */
        ++audit_tick_;
        const bool audit_now=audit_tick_<=16 || (audit_tick_&7)==1;
        if(!audit_now)budget=0;
        const uint64_t audit_began=audit_now?submitDiagNow():0;
        for(size_t i=0;i<count && budget && !audited;++i) {
            const size_t bytes=buffers[i].bytes<budget?(size_t)buffers[i].bytes:budget;
            audited=vm_->auditRange(buffers[i].iova,bytes,&lines,&dirty,&stale);budget-=bytes;
        }
        submitDiagRecord(9,audit_began);
        if(audit_now && audited!=kIOReturnUnsupported) {   /* no board hook (host): nothing to compare */
            native_adreno_wc_audit[0]=native_adreno_wc_audit[0]+1;
            native_adreno_wc_audit[1]=native_adreno_wc_audit[1]+lines;
            native_adreno_wc_audit[2]=native_adreno_wc_audit[2]+dirty;
            native_adreno_wc_audit[3]=native_adreno_wc_audit[3]+stale;
            if(audited)native_adreno_wc_audit[4]=audited;
        }
        if(dirty || (audited && audited!=kIOReturnUnsupported)) {
            vm_->distrustWriteCombined();
            native_adreno_wc_distrusted=native_adreno_coherence[0]+1;
        }
    }
    const bool sample=(stats_.submissions&63)==0;   /* upstream: diagnostic word on 1 submission in 64 */
    if(sample){PartScope part(3);(void)vm_->sampleWord(buffers[0].iova,&cached,&before);}
    IOReturn status;
    {SubmitDiagScope measured(6);status=vm_->publishMapped();}
    /* Before invalidateTlb records the current tables: a change since the previous submission keeps the full prologue. */
    ring_->requestLightPrologue(native_adreno_light_prologue && vm_->tlbCurrent());
    if(!status) {
        if(sample){PartScope part(3);(void)vm_->sampleWord(buffers[0].iova,&ignored,&after);}
        native_adreno_coherence[0]=native_adreno_coherence[0]+1;
        if(sample){native_adreno_coherence[1]=cached;native_adreno_coherence[2]=before;native_adreno_coherence[3]=after;}
        if(sample && (cached!=before || before!=after)) {
            if(!native_adreno_coherence[4]) {
                native_adreno_coherence[5]=cached;native_adreno_coherence[6]=before;native_adreno_coherence[7]=after;
            }
            native_adreno_coherence[4]=native_adreno_coherence[4]+1;
        }
    }
    /* The active ArmSmmu4KLiveUpdateContextBatch fills invalid leaves only,
     * publishes their table pages, and synchronously invalidates each unmap
     * before releasing its memory. Upper tables stay fixed. A successful
     * unchanged VM therefore needs no TLBIALL at each submission; that would
     * discard useful translations and serialize every frame on device MMIO.
     * Failed table updates still poison the VM and retain their resources. */
    if(status) {
        for(size_t i=0;i<handle_count;++i) {vm_->retireJobAfterCompletion(handles[i]);++stats_.references_retired;}
        return status;
    }
    {
        const uint32_t slot=native_adreno_ib_log_count&3;uint32_t head[28];
        {PartScope part(3);(void)vm_->peekWords(buffers[0].iova,head,28);}
        native_adreno_ib_log[slot][0]=ring_->lastSubmitted()+1;native_adreno_ib_log[slot][1]=(uint32_t)buffers[0].iova;
        native_adreno_ib_log[slot][2]=(uint32_t)(buffers[0].iova>>32);native_adreno_ib_log[slot][3]=(uint32_t)buffers[0].bytes;
        for(unsigned i=0;i<28;++i)native_adreno_ib_log[slot][4+i]=head[i];
        native_adreno_ib_log_count=native_adreno_ib_log_count+1;
    }
    {PartScope part(4);status=ring_->submitFramed(words,word_count,&value);}
    if(status) {
        /* A poisoned ring may already have consumed the WPTR write, so the
         * referenced buffers cannot be retired here. */
        if(ring_->poisoned()) {
            *job=Job{};++stats_.poisoned_jobs;
            for(size_t i=0;i<handle_count;++i)job->handles[i]=handles[i];
            job->count=handle_count;job->queue=queue;job->active=true;
            return poison();
        }
        while(handle_count) {vm_->retireJobAfterCompletion(handles[--handle_count]);++stats_.references_retired;}
        return status;
    }
    for(size_t i=0;i<handle_count;++i)job->handles[i]=handles[i];
    job->count=handle_count;job->fence=value;job->queue=queue;job->active=true;
    ++stats_.submissions;
    __atomic_fetch_add(&native_adreno_submit_batches[0],1ull,__ATOMIC_RELAXED);
    __atomic_fetch_add(&native_adreno_submit_batches[1],(uint64_t)count,__ATOMIC_RELAXED);
    uint64_t peak=__atomic_load_n(&native_adreno_submit_batches[2],__ATOMIC_RELAXED);
    while(count>peak && !__atomic_compare_exchange_n(&native_adreno_submit_batches[2],&peak,(uint64_t)count,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)){}
    if(count>16)__atomic_fetch_add(&native_adreno_submit_batches[3],1ull,__ATOMIC_RELAXED);
    if(outstanding()>stats_.peak_jobs)stats_.peak_jobs=outstanding();
    *fence=value;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::observeFence(uint32_t fence,const NativeAdrenoRingClock *clock,
    uint32_t timeout_us)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    if(!fence)return kIOReturnBadArgument;
    const NativeAdrenoRingFence result=ring_->observeFence(fence,clock,timeout_us,nullptr);
    if(result==NativeAdrenoRingFencePoisoned)return poison();
    if(result==NativeAdrenoRingFenceArgument)return kIOReturnBadArgument;
    if(result==NativeAdrenoRingFenceTimeout)return kIOReturnTimeout;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::wait(uint32_t fence,const NativeAdrenoRingClock *clock,
    uint32_t timeout_us)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    Job *job=find(fence);
    uint32_t observed=0;
    native_adreno_fence_wait[0]=native_adreno_fence_wait[0]+1;
    if(!job) {
        native_adreno_fence_wait[6]=native_adreno_fence_wait[6]+1;
        const NativeAdrenoRingFence passed=ring_->observeFence(fence,clock,timeout_us,&observed);
        if(passed==NativeAdrenoRingFencePoisoned)return poison();
        if(passed==NativeAdrenoRingFenceArgument) {native_adreno_fence_wait[7]=fence;return kIOReturnBadArgument;}
        if(passed==NativeAdrenoRingFenceTimeout) {
            ++stats_.timeouts;
            native_adreno_fence_wait[1]=native_adreno_fence_wait[1]+1;native_adreno_fence_wait[2]=fence;native_adreno_fence_wait[3]=observed;
            native_adreno_fence_wait[4]=ring_->lastSubmitted();native_adreno_fence_wait[5]=ring_->completedFence();
            return kIOReturnTimeout;
        }
        return kIOReturnSuccess;
    }
    const NativeAdrenoRingFence result=ring_->wait(fence,clock,timeout_us,&observed);
    if(result==NativeAdrenoRingFencePoisoned)return poison();
    if(result==NativeAdrenoRingFenceArgument) {native_adreno_fence_wait[7]=fence;return kIOReturnBadArgument;}
    if(result==NativeAdrenoRingFenceTimeout) {
        ++stats_.timeouts;
        native_adreno_fence_wait[1]=native_adreno_fence_wait[1]+1;native_adreno_fence_wait[2]=fence;native_adreno_fence_wait[3]=observed;
        native_adreno_fence_wait[4]=ring_->lastSubmitted();native_adreno_fence_wait[5]=ring_->completedFence();
        return kIOReturnTimeout;
    }
    IOReturn status=release(*job);
    if(status)return status;
    ++stats_.completed;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::waitAll(const NativeAdrenoRingClock *clock,uint32_t timeout_us)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    while(outstanding()) {
        Job *oldest=nullptr;
        for(auto &job:jobs_)if(job.active && (!oldest || job.fence-oldest->fence>0x80000000u))oldest=&job;
        const IOReturn status=wait(oldest->fence,clock,timeout_us);
        if(status)return status;
    }
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::waitDepth(size_t depth,const NativeAdrenoRingClock *clock,uint32_t timeout_us)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    if(!depth)return kIOReturnBadArgument;
    while(outstanding()>=depth) {
        Job *oldest=nullptr;
        for(auto &job:jobs_)if(job.active && (!oldest || job.fence-oldest->fence>0x80000000u))oldest=&job;
        const IOReturn status=wait(oldest->fence,clock,timeout_us);
        if(status)return status;
    }
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::waitRange(uint32_t first,uint32_t count,const NativeAdrenoRingClock *clock,uint32_t timeout_us)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    while(rangeOutstanding(first,count)) {
        Job *oldest=nullptr;
        for(auto &job:jobs_)if(job.active && job.queue-first<count && (!oldest || job.fence-oldest->fence>0x80000000u))oldest=&job;
        const IOReturn status=wait(oldest->fence,clock,timeout_us);
        if(status)return status;
    }
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoSubmit::retireCompleted()
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    for(auto &job:jobs_) {
        if(!job.active)continue;
        const NativeAdrenoRingFence state=ring_->observeFence(job.fence,nullptr,0,nullptr);
        if(state==NativeAdrenoRingFencePoisoned)return poison();
        if(state!=NativeAdrenoRingFenceComplete)continue;
        const IOReturn status=release(job);
        if(status)return status;
        ++stats_.completed;
    }
    return kIOReturnSuccess;
}
