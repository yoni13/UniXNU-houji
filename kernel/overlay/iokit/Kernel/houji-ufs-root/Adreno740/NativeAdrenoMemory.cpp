/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoMemory.h"
#include "NativeAdrenoCpuPagePolicy.h"
#include "NativeAdrenoPool.h"
#include "NativeAdrenoAllocationTrace.h"
NativeAdrenoAllocationTrace nativeAdrenoAllocationTrace;
extern "C" {uint64_t (*native_adreno_memory_now_us)(void)=nullptr;}
/* Where a call that never returns is stuck: the free/unmap/release path stores a stage number here, the board page shows it. */
extern "C" {volatile uint32_t native_adreno_trace_stage;}
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <kern/task.h>
extern "C" int houji_surface_seed_skip_enabled(void);
extern "C" void *houji_surface_retain_for_physical(uint64_t physical);
extern "C" uint32_t houji_surface_seed_of(void *surface);
extern "C" void houji_surface_release(void *surface);
extern "C" int houji_surface_lazy_acquire_enabled(void);
extern "C" void houji_surface_mark_gpu_dirty(void *surface);

static bool outputWidth(unsigned bits)
{return bits==32 || bits==36 || bits==40 || bits==42 || bits==44 || bits==48;}
/* Where the last import stopped (10..19, 19 = done), and for a refused page the physical address and offset: the board log shows them. */
enum : IOReturn {kNativeAdrenoProtectionFailure=2};   /* KERN_PROTECTION_FAILURE, as prepare() reports a range that cannot be wired for writing */
extern "C" {volatile uint32_t native_adreno_pool_mib=0,native_adreno_pool_status=0;volatile uint32_t native_adreno_import_stage;volatile uint64_t native_adreno_import_fail_pa,native_adreno_import_fail_offset;}
extern "C" {volatile uint32_t native_adreno_pool_stats[8];}
/* The pool's write-combine (writeCombinePool): bit 31 once tried, the low bits its status. */
extern "C" {volatile uint32_t native_adreno_pool_wc;}   /* [0] pool MiB, [1] BOs from the pool, [2] fell back, [3] MiB in use, [4] peak MiB, [5] scattered BOs, [6] their spans, [7] import spans */
namespace {
struct NativePool {
    IOBufferMemoryDescriptor *descriptor=nullptr;
    unsigned char *mapping=nullptr;
    uint64_t physical=0;
    size_t cpu_bytes=0;
    bool ready=false,write_combined=false,quarantined=false;
    NativeAdrenoPoolExtents extents;
} pool;
void poolUsage() {
    native_adreno_pool_stats[3]=(uint32_t)(pool.extents.used>>20);
    native_adreno_pool_stats[4]=(uint32_t)(pool.extents.peak>>20);
}
}
size_t NativeAdrenoMemory::poolBytes() {return pool.ready && !pool.quarantined?pool.extents.bytes:0;}
IOReturn NativeAdrenoMemory::reservePool(unsigned bits,size_t bytes)
{
    if(pool.descriptor)return kIOReturnBusy;
    if(!outputWidth(bits) || !bytes || bytes>MaximumPoolBytes)return kIOReturnBadArgument;
    NativeAdrenoCpuState cpu{};
    IOReturn status=NativeAdrenoQueryCpuPages(&cpu);if(status)return status;
    const size_t page=(size_t)cpu.kernel_page_bytes;
    if((page!=4096 && page!=16384) || (bytes&(page-1)))return kIOReturnBadArgument;
    const uint64_t limit=UINT64_C(1)<<bits;
    auto *descriptor=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryMapperNone)|kIOMemoryPhysicallyContiguous,
        bytes,(limit-1)&~uint64_t(page-1));
    if(!descriptor)return kIOReturnNoMemory;
    status=descriptor->prepare();
    if(status) {descriptor->release();return status;}
    pool.descriptor=descriptor;pool.cpu_bytes=page;
    pool.mapping=static_cast<unsigned char *>(descriptor->getBytesNoCopy());
    status=kIOReturnUnsupported;
    if(!pool.mapping || ((uintptr_t)pool.mapping&(page-1)) || bytes>UINTPTR_MAX-(uintptr_t)pool.mapping ||
        descriptor->getLength()!=bytes)goto failed;
    /* Check every CPU page, not just the first and last physical segment. */
    for(size_t offset=0;offset<bytes;offset+=page) {
        IOByteCount length=0;const addr64_t pa=descriptor->getPhysicalSegment(offset,&length,kIOMemoryMapperNone);
        if(!pa || (pa&(page-1)) || length<page || pa>=limit || page>limit-pa)goto failed;
        if(!offset)pool.physical=pa;
        if(offset>limit-pool.physical || pa!=pool.physical+offset)goto failed;
    }
    if(!pool.extents.initialize(bytes,page))goto failed;
    pool.ready=true;native_adreno_pool_stats[0]=(uint32_t)(bytes>>20);return kIOReturnSuccess;
failed:
    if(descriptor->complete()!=kIOReturnSuccess)pool.quarantined=true;
    else {descriptor->release();bzero(&pool,sizeof(pool));}
    return status;
}
IOReturn NativeAdrenoMemory::writeCombinePool()
{
    if(!pool.ready || pool.quarantined || pool.extents.used)return kIOReturnNotReady;
    if(pool.write_combined)return kIOReturnSuccess;
    IOReturn status=NativeAdrenoMaintainOwnedPages(pool.mapping,pool.extents.bytes,NativeAdrenoCleanInvalidate);
    if(status)return status;
    const NativeAdrenoPhysicalSpan span={0,pool.physical,pool.extents.bytes};
    /* No release after an uncertain partial pmap conversion. */
    pool.quarantined=true;
    status=NativeAdrenoSetPagesCacheMode(&span,1,true);
    if(status)return status;
    pool.write_combined=true;pool.quarantined=false;return kIOReturnSuccess;
}
size_t NativeAdrenoMemory::backingBudget(size_t logical)
{
    NativeAdrenoCpuState cpu{};size_t owned=0;
    if(NativeAdrenoQueryCpuPages(&cpu)!=kIOReturnSuccess ||
        !NativeAdrenoRoundCpuBacking(logical,(size_t)cpu.kernel_page_bytes,&owned) ||
        owned>256u*1024u*1024u)return 0;
    return owned;
}
IOReturn NativeAdrenoMemory::allocate(size_t bytes,unsigned bits,bool contiguous,size_t budget,bool pooled)
{
    if(descriptor_ || imported_ || pooled_ || spans_ || owned_spans_ || quarantined_)return kIOReturnBusy;
    if(!bytes || (bytes&4095) || !outputWidth(bits) || !budget || budget>256u*1024u*1024u)
        return kIOReturnBadArgument;
    if(bytes>budget)return kIOReturnNoResources;
    NativeAdrenoCpuState cpu{};
    IOReturn status=NativeAdrenoQueryCpuPages(&cpu);if(status!=kIOReturnSuccess)return status;
    size_t owned=0;
    const size_t cpu_bytes=(size_t)cpu.kernel_page_bytes;
    if(!NativeAdrenoRoundCpuBacking(bytes,cpu_bytes,&owned) || owned>256u*1024u*1024u)return kIOReturnBadArgument;
    if(owned>budget)return kIOReturnNoResources;
    const uint64_t limit=UINT64_C(1)<<bits;
    NativeAdrenoAllocationTimer timing(NativeAdrenoAllocationTrace::Standalone,owned,status);
    IOOptionBits options=static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryMapperNone);
    if(pooled && pool.ready && pool.write_combined && !pool.quarantined && pool.cpu_bytes==cpu_bytes) {
        size_t offset=0;
        if(pool.physical<limit && pool.extents.bytes<=limit-pool.physical && pool.extents.take(owned,&offset)) {
            timing.route=NativeAdrenoAllocationTrace::Pool;
            pooled_=true;pool_offset_=offset;prepared_=true;write_combined_=true;
            bytes_=bytes;owned_bytes_=owned;cpu_page_bytes_=cpu_bytes;mapping_=pool.mapping+offset;
            span_capacity_=owned_span_capacity_=1;
            spans_=IONewData(NativeAdrenoPhysicalSpan,1);owned_spans_=IONewData(NativeAdrenoPhysicalSpan,1);
            poolUsage();
            if(!spans_ || !owned_spans_) {status=kIOReturnNoMemory;goto failed;}
            spans_[0]={0,pool.physical+offset,bytes};owned_spans_[0]={0,pool.physical+offset,owned};
            span_count_=owned_span_count_=1;
            /* Device-pager teardown may have reset the attribute. Recheck before
             * zeroing/reuse; never leave stale data in the CPU-page padding. */
            timing.lap(0);
            {uint32_t reset=0;status=NativeAdrenoReassertWriteCombined(owned_spans_,1,&reset);}
            timing.lap(2);
            if(status) {quarantined_=true;return status;}
            bzero(mapping_,owned);
            __asm__ volatile("dsb sy":::"memory");
            timing.lap(3);
            native_adreno_pool_stats[1]=native_adreno_pool_stats[1]+1;
            return kIOReturnSuccess;
        }
        native_adreno_pool_stats[2]=native_adreno_pool_stats[2]+1;
    }
    if(contiguous)options|=kIOMemoryPhysicallyContiguous;
    descriptor_=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,options,owned,(limit-1)&~uint64_t(cpu_bytes-1));
    if(!descriptor_) {status=kIOReturnNoMemory;return status;}
    status=descriptor_->prepare();
    if(status!=kIOReturnSuccess) {descriptor_->release();descriptor_=nullptr;return status;}
    prepared_=true;bytes_=bytes;owned_bytes_=owned;cpu_page_bytes_=cpu_bytes;mapping_=descriptor_->getBytesNoCopy();
    status=kIOReturnUnsupported;
    if(!mapping_ || ((uintptr_t)mapping_&(cpu_bytes-1)) || owned>UINTPTR_MAX-(uintptr_t)mapping_ ||
        descriptor_->getLength()!=owned)goto failed;
    owned_span_capacity_=owned/cpu_bytes;span_capacity_=bytes/4096;
    owned_spans_=IONewData(NativeAdrenoPhysicalSpan,owned_span_capacity_);
    spans_=IONewData(NativeAdrenoPhysicalSpan,span_capacity_);
    if(!owned_spans_ || !spans_) {status=kIOReturnNoMemory;goto failed;}
    timing.lap(0);
    for(size_t offset=0;offset<owned;offset+=cpu_bytes) {
        IOByteCount length=0;
        const addr64_t physical=descriptor_->getPhysicalSegment(offset,&length,kIOMemoryMapperNone);
        if(!physical || (physical&(cpu_bytes-1)) || length<cpu_bytes || physical>=limit || cpu_bytes>limit-physical)
            {status=kIOReturnUnsupported;goto failed;}
        if(contiguous && offset && physical!=owned_spans_[0].physical+offset)
            {status=kIOReturnUnsupported;goto failed;}
        if(owned_span_count_ && physical==owned_spans_[owned_span_count_-1].physical+owned_spans_[owned_span_count_-1].bytes)
            owned_spans_[owned_span_count_-1].bytes+=cpu_bytes;
        else owned_spans_[owned_span_count_++]={offset,physical,cpu_bytes};
    }
    /* Clip only GPU-visible spans. Rounded tail is CPU-owned, zeroed and never
     * implicitly mapped into the GPU aperture or another BO's CPU view. */
    for(size_t i=0;i<owned_span_count_ && owned_spans_[i].offset<bytes;++i) {
        const auto &o=owned_spans_[i];
        const uint64_t length=o.bytes<bytes-o.offset?o.bytes:bytes-o.offset;
        spans_[span_count_++]={o.offset,o.physical,length};
    }
    timing.lap(1);
    bzero(mapping_,owned);
    status=NativeAdrenoMaintainOwnedPages(mapping_,owned,NativeAdrenoClean);
    timing.lap(3);
    if(status!=kIOReturnSuccess)goto failed;
    native_adreno_pool_stats[5]=native_adreno_pool_stats[5]+1;native_adreno_pool_stats[6]=native_adreno_pool_stats[6]+(uint32_t)span_count_;
    return kIOReturnSuccess;
failed:
    /* Metadata, full backing and descriptor remain owned on failed complete. */
    (void)release();return status;
}
/* An import holds the pages through a memory entry of its own (kIOMemoryPersistent), which references the VM object: the owner may unmap
 * its range while the GPU still uses it (Core Animation frees an image's Shmem once its no-copy buffer is released, before the runtime frees
 * the import), and the object then lives on until the import completes instead of being torn down under a wired UPL (int-233..235: the render
 * thread stuck in vm_deallocate). A range no memory entry can be made of is imported as before; [0] counts the persistent imports, [1] those. */
extern "C" {volatile uint32_t native_adreno_import_persistent[2];}
static IOMemoryDescriptor *importRange(uint64_t address,size_t bytes,IODirection direction,void *task)
{
    const IOOptionBits options=static_cast<IOOptionBits>(direction)|static_cast<IOOptionBits>(kIOMemoryMapperNone);
    IOMemoryDescriptor *range=IOMemoryDescriptor::withAddressRange(address,bytes,options|kIOMemoryPersistent,static_cast<task_t>(task));
    if(range) {native_adreno_import_persistent[0]=native_adreno_import_persistent[0]+1;return range;}
    range=IOMemoryDescriptor::withAddressRange(address,bytes,options,static_cast<task_t>(task));
    if(range)native_adreno_import_persistent[1]=native_adreno_import_persistent[1]+1;
    return range;
}
IOReturn NativeAdrenoMemory::importTask(void *task,uint64_t address,size_t bytes,unsigned bits,size_t budget)
{
    native_adreno_import_stage=10;
    if(descriptor_ || imported_ || pooled_ || spans_ || owned_spans_ || quarantined_)return kIOReturnBusy;
    native_adreno_import_stage=11;
    if(!task || !address || (address&4095) || !bytes || (bytes&4095) || address>UINT64_MAX-bytes || !outputWidth(bits) ||
        !budget || budget>256u*1024u*1024u)return kIOReturnBadArgument;
    native_adreno_import_stage=12;
    if(bytes>budget)return kIOReturnNoResources;
    NativeAdrenoCpuState cpu{};
    native_adreno_import_stage=13;
    IOReturn status=NativeAdrenoQueryCpuPages(&cpu);if(status!=kIOReturnSuccess)return status;
    const size_t cpu_bytes=(size_t)cpu.kernel_page_bytes;
    /* Partial CPU pages cannot establish the complete pinned range boundary. */
    if((address|bytes)&(cpu_bytes-1))return kIOReturnUnsupported;
    const uint64_t limit=UINT64_C(1)<<bits;
    imported_=importRange(address,bytes,kIODirectionInOut,task);
    native_adreno_import_stage=14;
    if(!imported_)return kIOReturnNoMemory;
    native_adreno_import_stage=15;
    status=imported_->prepare();
    if(status==kNativeAdrenoProtectionFailure) {
        /* Read-only memory of the owner (a mapped file, constant data): cannot be pinned for writing. Pin it for reading only; the GPU then maps it
         * read-only. Metal's no-copy buffers over such memory are only ever read by shaders. */
        imported_->release();imported_=nullptr;
        imported_=importRange(address,bytes,kIODirectionOut,task);
        if(!imported_)return kIOReturnNoMemory;
        status=imported_->prepare();
        if(status==kIOReturnSuccess)read_only_=true;
    }
    if(status!=kIOReturnSuccess) {imported_->release();imported_=nullptr;return status;}
    prepared_=true;bytes_=bytes;owned_bytes_=bytes;cpu_page_bytes_=cpu_bytes;
    status=kIOReturnUnsupported;
    native_adreno_import_stage=16;
    if(imported_->getLength()!=bytes)goto failed;
    native_adreno_import_stage=17;
    span_capacity_=bytes/4096;owned_span_capacity_=bytes/cpu_bytes;
    spans_=IONewData(NativeAdrenoPhysicalSpan,span_capacity_);
    owned_spans_=IONewData(NativeAdrenoPhysicalSpan,owned_span_capacity_);
    if(!spans_ || !owned_spans_) {status=kIOReturnNoMemory;goto failed;}
    for(size_t offset=0;offset<bytes;offset+=cpu_bytes) {
        IOByteCount length=0;
        const addr64_t physical=imported_->getPhysicalSegment(offset,&length,kIOMemoryMapperNone);
        if(!physical || (physical&(cpu_bytes-1)) || length<cpu_bytes || physical>=limit || cpu_bytes>limit-physical)
            {native_adreno_import_stage=18;native_adreno_import_fail_pa=physical;native_adreno_import_fail_offset=offset;
             status=kIOReturnUnsupported;goto failed;}
        if(span_count_ && physical==spans_[span_count_-1].physical+spans_[span_count_-1].bytes) {
            spans_[span_count_-1].bytes+=cpu_bytes;owned_spans_[owned_span_count_-1].bytes+=cpu_bytes;
        } else {
            spans_[span_count_++]={offset,physical,cpu_bytes};
            owned_spans_[owned_span_count_++]={offset,physical,cpu_bytes};
        }
    }
    native_adreno_import_stage=19;
    native_adreno_pool_stats[7]=native_adreno_pool_stats[7]+(uint32_t)span_count_;
    return kIOReturnSuccess;
failed:
    (void)release();return status;
}
IOReturn NativeAdrenoMemory::makeWriteCombined()
{
    if(pooled_ && prepared_ && !quarantined_ && !device_references_ && write_combined_ && pool.write_combined)
        return kIOReturnSuccess;
    if(!descriptor_ || imported_ || !prepared_ || quarantined_ || device_references_ || write_combined_ || !mapping_ || !span_count_)
        return kIOReturnNotReady;
    IOReturn status=kIOReturnSuccess;
    NativeAdrenoAllocationTimer timing(NativeAdrenoAllocationTrace::WriteCombine,owned_bytes_,status);
    status=NativeAdrenoMaintainOwnedPages(mapping_,owned_bytes_,NativeAdrenoCleanInvalidate);
    timing.lap(3);
    if(status)return status;
    /* Mark before mutation: a void pmap write plus failed readback has unknown
     * partial state and may never release these CPU pages back to the VM. */
    write_combined_=true;
    status=NativeAdrenoSetPagesCacheMode(owned_spans_,owned_span_count_,true);
    timing.lap(2);
    if(status) {quarantined_=true;return status;}
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::release()
{
    native_adreno_trace_stage=30;
    if(device_references_ || quarantined_)return kIOReturnBusy;
    if(write_combined_ && !pooled_) {
        /* The pages go back to the VM cacheable, as they came. */
        const IOReturn status=NativeAdrenoSetPagesCacheMode(owned_spans_,owned_span_count_,false);
        if(status) {quarantined_=true;return status;}
        write_combined_=false;
    }
    if(surface_) {houji_surface_release(surface_);surface_=nullptr;}
    surface_state_=seed_valid_=0;seed_skips_=0;
    if(imported_) {
        if(prepared_) {
            IOReturn status=imported_->complete();
            if(status!=kIOReturnSuccess) {quarantined_=true;return status;}
            prepared_=false;
        }
        imported_->release();imported_=nullptr;
    }
    if(descriptor_) {
        if(prepared_) {
            native_adreno_trace_stage=31;
            IOReturn status=descriptor_->complete();
            native_adreno_trace_stage=32;
            if(status!=kIOReturnSuccess) {quarantined_=true;return status;}
            prepared_=false;
        }
        native_adreno_trace_stage=33;
        descriptor_->release();descriptor_=nullptr;
        native_adreno_trace_stage=34;
    }
    if(pooled_) {
        if(!pool.extents.give(pool_offset_,owned_bytes_)) {quarantined_=true;return kIOReturnNoResources;}
        poolUsage();pooled_=false;pool_offset_=0;prepared_=false;write_combined_=false;
    }
    if(spans_)IODeleteData(spans_,NativeAdrenoPhysicalSpan,span_capacity_);
    if(owned_spans_)IODeleteData(owned_spans_,NativeAdrenoPhysicalSpan,owned_span_capacity_);
    mapping_=nullptr;spans_=nullptr;owned_spans_=nullptr;bytes_=span_count_=span_capacity_=0;
    owned_bytes_=owned_span_count_=owned_span_capacity_=cpu_page_bytes_=0;read_only_=false;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::holdForDevice()
{
    if(!backed() || !prepared_ || !span_count_ || quarantined_)return kIOReturnNotReady;
    if(device_references_==UINT32_MAX)return kIOReturnNoResources;
    ++device_references_;return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::retireAfterQuiescence()
{
    if(!device_references_)return kIOReturnBadArgument;
    if(quarantined_)return kIOReturnNotReady;
    --device_references_;return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::publishForDevice(size_t offset,size_t bytes,bool writes,bool include_write_combined)
{
    if(!backed() || !prepared_ || quarantined_)return kIOReturnNotReady;
    if(!bytes || ((offset|bytes)&4095) || offset>bytes_ || bytes>bytes_-offset)return kIOReturnBadArgument;
    if(imported_)return maintainImported(offset,bytes,writes?NativeAdrenoCleanInvalidate:NativeAdrenoClean);
    if(write_combined_)   /* no CPU mapping should cache these pages; if one does, reach its lines through the physical pages */
        return include_write_combined?maintainImported(offset,bytes,writes?NativeAdrenoCleanInvalidate:NativeAdrenoClean):kIOReturnSuccess;
    if(!cpu_page_bytes_)return kIOReturnBadArgument;
    const size_t begin=offset&~(cpu_page_bytes_-1),end=(offset+bytes+cpu_page_bytes_-1)&~(cpu_page_bytes_-1);
    if(!cpu_page_bytes_ || end>owned_bytes_ || end<=begin)return kIOReturnBadArgument;
    return NativeAdrenoMaintainOwnedPages(static_cast<unsigned char *>(mapping_)+begin,end-begin,
        writes?NativeAdrenoCleanInvalidate:NativeAdrenoClean);
}
IOReturn NativeAdrenoMemory::publishTracked(bool writes,bool include_write_combined,bool *skipped)
{
    *skipped=false;
    if(imported_ && !write_combined_ && houji_surface_seed_skip_enabled()) {
        if(!surface_state_ && owned_spans_ && owned_span_count_) {
            surface_=houji_surface_retain_for_physical(owned_spans_[0].physical);
            surface_state_=surface_?1:2;
        }
        if(surface_state_==1) {
            /* Read the seed before cleaning: a write unlock after this read
             * changes it, so the next publish cleans again. */
            const uint32_t seed=houji_surface_seed_of(surface_);
            if(seed_valid_ && seed==surface_seed_ && ++seed_skips_<32) {*skipped=true;return kIOReturnSuccess;}
            seed_skips_=0;
            const IOReturn status=publishForDevice(0,bytes_,writes,include_write_combined);
            if(!status) {surface_seed_=seed;seed_valid_=1;}
            return status;
        }
    }
    return publishForDevice(0,bytes_,writes,include_write_combined);
}
IOReturn NativeAdrenoMemory::acquireTracked(bool *deferred)
{
    *deferred=false;
    if(imported_ && houji_surface_lazy_acquire_enabled()) {
        if(!surface_state_ && owned_spans_ && owned_span_count_) {
            surface_=houji_surface_retain_for_physical(owned_spans_[0].physical);
            surface_state_=surface_?1:2;
        }
        if(surface_state_==1) {houji_surface_mark_gpu_dirty(surface_);*deferred=true;return kIOReturnSuccess;}
    }
    return acquireDeviceWrites(0,bytes_);
}
IOReturn NativeAdrenoMemory::acquireDeviceWrites(size_t offset,size_t bytes)
{
    if(!backed() || !prepared_ || quarantined_)return kIOReturnNotReady;
    if(!bytes || ((offset|bytes)&4095) || offset>bytes_ || bytes>bytes_-offset)return kIOReturnBadArgument;
    /* Imported pages may hold the owner's unrelated dirty lines next to the GPU's writes: clean as well as invalidate. */
    if(imported_)return maintainImported(offset,bytes,NativeAdrenoCleanInvalidate);
    if(write_combined_)return kIOReturnSuccess;
    if(!cpu_page_bytes_)return kIOReturnBadArgument;
    const size_t begin=offset&~(cpu_page_bytes_-1),end=(offset+bytes+cpu_page_bytes_-1)&~(cpu_page_bytes_-1);
    if(!cpu_page_bytes_ || end>owned_bytes_ || end<=begin)return kIOReturnBadArgument;
    return NativeAdrenoMaintainOwnedPages(static_cast<unsigned char *>(mapping_)+begin,end-begin,NativeAdrenoInvalidate);
}
/* Imported pages have no kernel mapping here (and write-combined ones only a non-cacheable one); maintain each span's part of
 * [offset, offset+bytes) by physical address. */
IOReturn NativeAdrenoMemory::maintainImported(size_t offset,size_t bytes,NativeAdrenoCacheOperation op)
{
    if(!cpu_page_bytes_ || !owned_spans_ || !owned_span_count_ || !bytes || offset>bytes_ || bytes>bytes_-offset)return kIOReturnBadArgument;
    const size_t owned_begin=offset&~(cpu_page_bytes_-1),owned_end=(offset+bytes+cpu_page_bytes_-1)&~(cpu_page_bytes_-1);
    if(owned_end>owned_bytes_ || owned_end<=owned_begin)return kIOReturnBadArgument;
    size_t covered=0;
    for(size_t i=0;i<owned_span_count_;++i) {
        const NativeAdrenoPhysicalSpan &span=owned_spans_[i];
        const size_t begin=owned_begin>span.offset?owned_begin:span.offset;
        const size_t end=owned_end<span.offset+span.bytes?owned_end:span.offset+span.bytes;
        if(begin>=end)continue;
        const IOReturn status=NativeAdrenoMaintainPhysicalPages(span.physical+(begin-span.offset),end-begin,op);
        if(status)return status;covered+=end-begin;
    }
    if(covered!=owned_end-owned_begin)return kIOReturnBadArgument;
    return kIOReturnSuccess;
}
