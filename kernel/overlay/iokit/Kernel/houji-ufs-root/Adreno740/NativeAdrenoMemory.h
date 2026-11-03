/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_MEMORY_H
#define NATIVE_ADRENO_MEMORY_H
#include <IOKit/IOTypes.h>
#include <stddef.h>
#include <stdint.h>
class IOBufferMemoryDescriptor;
class IOMemoryDescriptor;
struct NativeAdrenoPhysicalSpan {uint64_t offset,physical,bytes;};
struct NativeAdrenoCpuView {
    const NativeAdrenoPhysicalSpan *spans;
    size_t count,bytes;
};
struct NativeAdrenoCpuState {
    uint64_t tcr,ttbr0,ttbr1,sctlr;
    uint64_t kernel_page_bytes,kernel_map_page_bytes;
    unsigned cpu_page_shift;
};
extern "C" {
extern volatile uint32_t native_adreno_pool_mib,native_adreno_pool_status,native_adreno_pool_wc;
extern volatile uint32_t native_adreno_pool_stats[8];
}
IOReturn NativeAdrenoQueryCpuPages(NativeAdrenoCpuState *);
/* Legacy query retains its strict 4 KiB semantics. */
IOReturn NativeAdrenoQueryCpu4K(NativeAdrenoCpuState *);
enum NativeAdrenoCacheOperation {NativeAdrenoClean,NativeAdrenoCleanInvalidate,NativeAdrenoInvalidate};
IOReturn NativeAdrenoMaintainOwnedPages(void *,size_t,NativeAdrenoCacheOperation);
/* The same over whole physical pages of DRAM, through the kernel's physical aperture: data cache lines are physically tagged, so this
 * reaches lines another task's mapping left (an imported IOSurface). Apple's sys_dcache_flush is only a DSB, so user space cannot. */
IOReturn NativeAdrenoMaintainPhysicalPages(uint64_t physical,size_t bytes,NativeAdrenoCacheOperation);
/* Every CPU mapping of the spans' pages becomes write-combined (true) or takes the default cacheable attribute again (false). */
IOReturn NativeAdrenoSetPagesCacheMode(const NativeAdrenoPhysicalSpan *,size_t count,bool write_combined);
/* int-100 found a write-combined BO's page back at the default (cacheable) attribute with dirty IB lines: XNU's view machinery sets a page's
 * attribute when it populates or tears down a device-pager view. Every page of the spans no longer write-combined is made so again (the pmap
 * then rewrites every mapping it tracks and writes the page's cache lines back); *reset counts them. */
IOReturn NativeAdrenoReassertWriteCombined(const NativeAdrenoPhysicalSpan *,size_t count,uint32_t *reset);

/* Native XNU backing store, not a Metal device or user ABI. No IOMMU mapper,
 * Linux fd/ioctl, hardware address, guessed PA or implicit destructor cleanup.
 * Driver owns this object until release() succeeds; do not destroy/copy it
 * with outstanding memory. Serialize every call under the queue/resource gate.
 * Every published mapping/job must hold a device reference BEFORE publication,
 * and retire it only after actual DMA quiescence and completed native TLB unmap.
 * References are lifetime accounting, never fences or proof of quiescence. */
class NativeAdrenoMemory final {
    IOBufferMemoryDescriptor *descriptor_=nullptr;
    IOMemoryDescriptor *imported_=nullptr;
    bool read_only_=false;
    void *mapping_=nullptr;
    NativeAdrenoPhysicalSpan *spans_=nullptr;
    size_t bytes_=0,span_count_=0,span_capacity_=0;
    NativeAdrenoPhysicalSpan *owned_spans_=nullptr;
    size_t owned_bytes_=0,owned_span_count_=0,owned_span_capacity_=0,cpu_page_bytes_=0;
    uint32_t device_references_=0;
    bool prepared_=false,quarantined_=false,write_combined_=false;
    /* Imported IOSurface dirty tracking (kernel only): retained surface, its
     * seed at the last publish, and skips since the last forced publish. */
    void *surface_=nullptr;
    uint32_t surface_seed_=0;
    uint8_t surface_state_=0,seed_valid_=0;   /* state: 0 unresolved, 1 surface, 2 none */
    uint16_t seed_skips_=0;
    bool pooled_=false;
    size_t pool_offset_=0;
    IOReturn maintainImported(size_t offset,size_t bytes,NativeAdrenoCacheOperation);
public:
    NativeAdrenoMemory()=default;
    ~NativeAdrenoMemory()=default;
    NativeAdrenoMemory(const NativeAdrenoMemory &)=delete;
    NativeAdrenoMemory &operator=(const NativeAdrenoMemory &)=delete;
    /* Explicit actual backing budget; rounded exclusive backing must fit it
     * and the 256MiB per-object hard limit.
     * Error may retain quarantined allocation state if complete() fails.
     * Caller always keeps this object and checks quarantined(), even on error.
     * Exact CPU 4K/16K state and kernel page ABI are checked before allocation.
     * prepare(), raw unmapped physical segments, span/PA bounds and optional
     * physical contiguity are verified before zeroing/PoC publication. */
    /* A qualified pool owns whole CPU pages per BO, including logical padding.
     * Exhaustion falls back to an independent allocation. Imports never pool. */
    IOReturn allocate(size_t bytes,unsigned output_bits,bool contiguous,size_t byte_budget,bool pooled=false);
    /* Zero means the runtime page ABI/rounding/bounded budget is unqualified. */
    static size_t backingBudget(size_t logical_bytes);
    /* Arena capacity, not the per-BO ABI limit (which remains 256 MiB).
     * Explicit boot opt-in; the entire range is checked before use. */
    static constexpr size_t MaximumPoolBytes=512u*1024u*1024u;
    /* Before publishing any user clients: one retained, bounded, contiguous
     * IOBuffer allocation from XNU-managed RAM. No carved/guessed PA range.
     * Pool pages keep WC until reset, including across BO release/reuse.
     * Failed cache conversion retains the descriptor and disables the pool. */
    static IOReturn reservePool(unsigned output_bits,size_t bytes);
    static size_t poolBytes();
    static IOReturn writeCombinePool();
    /* Pins bytes at address in task (a user task's own pages) instead of allocating; prepare() wires them until release().
     * There is no kernel mapping: the owner keeps its CPU mapping and does its own cache maintenance around GPU use, so
     * publishForDevice/acquireDeviceWrites maintain its physical cache lines.
     * Only complete qualified CPU-page ranges and PA segments are accepted. */
    IOReturn importTask(void *task,uint64_t address,size_t bytes,unsigned output_bits,size_t byte_budget);
    bool imported() const {return imported_!=nullptr;}
    /* An import of memory the owner maps read-only (the kernel could pin it for reading only): the GPU may read it, never write it. */
    bool readOnly() const {return read_only_;}
    /* After allocate(), before any device or client mapping: cleans and invalidates the pages and makes every CPU mapping of them
     * write-combined, so publishForDevice/acquireDeviceWrites have nothing left to do. release() restores the default attribute. */
    IOReturn makeWriteCombined();
    bool writeCombined() const {return write_combined_;}
    /* Reassert over complete exclusive CPU pages, never clipped GPU leaves. */
    IOReturn reassertWriteCombined(uint32_t *reset) const
    {return write_combined_ && owned_span_count_?NativeAdrenoReassertWriteCombined(owned_spans_,owned_span_count_,reset):kIOReturnSuccess;}
    bool backed() const {return descriptor_ || imported_ || pooled_;}
    bool pooled() const {return pooled_;}
    /* Refuses device references/quarantine; only clears ownership after real
     * descriptor complete() succeeds. No automatic reset or resource recycling. */
    IOReturn release();
    IOReturn holdForDevice();
    IOReturn retireAfterQuiescence();
    void quarantine() {quarantined_=true;}
    /* Requires real CPU/device exclusion and retained ownership. Before GPU
     * writes, clean+invalidate; after genuine completion, invalidate only.
     * CPU must not dirty these pages while the device owns access. */
    /* A write-combined object is skipped unless include_write_combined: then its pages are maintained through the physical aperture,
     * for a client whose view turned out to be cacheable after all (NativeAdrenoVm::distrustWriteCombined). */
    IOReturn publishForDevice(size_t offset,size_t bytes,bool device_writes,bool include_write_combined=false);
    IOReturn acquireDeviceWrites(size_t offset,size_t bytes);
    /* publishForDevice over the whole object, except an imported IOSurface
     * whose seed has not moved since its last publish (*skipped set). */
    IOReturn publishTracked(bool device_writes,bool include_write_combined,bool *skipped);
    /* After GPU completion: acquireDeviceWrites over the whole object, except a known IOSurface, which is only marked for
     * invalidation at its next CPU lock (*deferred set). */
    IOReturn acquireTracked(bool *deferred);
    bool quarantined() const {return quarantined_;}
    uint32_t deviceReferences() const {return device_references_;}
    size_t bytes() const {return bytes_;}
    size_t ownedBytes() const {return owned_bytes_;}
    size_t cpuPageBytes() const {return cpu_page_bytes_;}
    size_t ownedSpanCount() const {return owned_span_count_;}
    const NativeAdrenoPhysicalSpan *ownedSpans() const {return owned_spans_;}
    /* Imports keep their task's existing CPU mapping. A new CPU view may
     * expose only this allocation's complete exclusive CPU backing pages. */
    bool cpuView(NativeAdrenoCpuView *out) const
    {
        if(!out)return false;*out={};
        if(!(descriptor_ || pooled_) || imported_ || !prepared_ || quarantined_ || !mapping_ ||
            !owned_spans_ || !owned_span_count_ || !owned_bytes_)return false;
        *out={owned_spans_,owned_span_count_,owned_bytes_};return true;
    }
    size_t spanCount() const {return span_count_;}
    const NativeAdrenoPhysicalSpan *spans() const {return spans_;}
    void *mapping() const {return mapping_;}
    /* Borrowed descriptor, never release independently or expose table pools
     * through clientMemoryForType. Sharing requires a separate owner/task ABI. */
    IOBufferMemoryDescriptor *descriptor() const {return descriptor_;}
};
#endif
