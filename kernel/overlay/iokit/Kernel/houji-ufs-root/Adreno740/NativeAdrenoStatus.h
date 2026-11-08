/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_STATUS_H
#define NATIVE_ADRENO_STATUS_H
#include <IOKit/IOTypes.h>
#include <stdint.h>
/* Monotonic fault and suspend counters for Turnip's device_check_status and
 * device_get_suspend_count. Turnip compares the fault count against the value
 * it saw last; a change means the context is lost. This object only counts
 * what the driver tells it: the IRQ path records a fatal decode, the reset or
 * power path records a suspend, and submission poisoning must be recorded by
 * the caller that observed it. It performs no detection, reset or recovery and
 * never clears a count, so a client cannot hide a fault by re-reading. */
struct NativeAdrenoStatusCounts {
    uint64_t faults;
    uint64_t suspends;
};
class NativeAdrenoStatus final {
    NativeAdrenoStatusCounts counts_{0,0};
public:
    /* For lock-free readers: the counters only grow, and aligned 64-bit loads are single-copy atomic. */
    const NativeAdrenoStatusCounts &counts_ref() const {return counts_;}
    void noteFault() {if(counts_.faults!=UINT64_MAX)__atomic_store_n(&counts_.faults,counts_.faults+1,__ATOMIC_RELEASE);}
    void noteSuspend() {if(counts_.suspends!=UINT64_MAX)__atomic_store_n(&counts_.suspends,counts_.suspends+1,__ATOMIC_RELEASE);}
    uint64_t faults() const {return __atomic_load_n(&counts_.faults,__ATOMIC_ACQUIRE);}
    uint64_t suspends() const {return __atomic_load_n(&counts_.suspends,__ATOMIC_ACQUIRE);}
    NativeAdrenoStatusCounts counts() const {return {faults(),suspends()};}
};
#endif
