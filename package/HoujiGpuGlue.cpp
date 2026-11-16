// SPDX-License-Identifier: BSD-2-Clause-Patent
// Definitions GpuPowerProbe.cpp (from sheng) refers to whose sheng implementations houji does not build: the boot latency tracer
// (BootLatencyTrace.c), the GPU add-on loader's stage words and the wide boot-progress rows. All weak, so a real definition wins.
#include <stdint.h>
#include "BootLatencyTrace.h"

extern "C" {
__attribute__((weak)) int boot_latency_enabled(void) {return 0;}
__attribute__((weak)) void boot_latency_record(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {}
// No GPU add-on payload on houji yet: the stages stay 0 (not started).
__attribute__((weak)) volatile int native_gpu_addon_stage, native_gpu_addon_status, native_gpu_addon2_stage, native_gpu_addon2_status;
__attribute__((weak)) char boot_progress_ext_wide[2][112];
}

// Kernel hooks of sheng's XNU patches that Adreno740 and GpuMemoryPool.cpp test for before use. houji's kernel has none of them;
// each stub answers as the missing hook does (no close notification, no log export, no memory outside the kernel's map).
#include <IOKit/IOMemoryDescriptor.h>
extern "C" {
__attribute__((weak)) kern_return_t iokit_physical_descriptor_close_notify(IOMemoryDescriptor *, void (*)(void *), void *) {return KERN_FAILURE;}
__attribute__((weak)) int boot_log_export_put(uint32_t, uint32_t, uint32_t, const void *, uint32_t) {return -1;}
__attribute__((weak)) uint32_t ml_outside_memory_pages;
}
