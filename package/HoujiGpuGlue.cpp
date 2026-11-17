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
// The two functions are strong: a weak definition reached through a signed pointer becomes a weak-lookup import, which the
// kernel collection cannot carry (XnuPrelink: chained fixup imports are not supported).
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
extern "C" {
kern_return_t iokit_physical_descriptor_close_notify(IOMemoryDescriptor *, void (*)(void *), void *) {return KERN_FAILURE;}
// The render server's diagnostic export (the plugin's telemetry and watchdog text with diag=1), in pieces: printed as text lines on the
// console, at most 64 KiB per boot so the USB log cannot be flooded, and acknowledged as stored.
int boot_log_export_put(uint32_t generation, uint32_t, uint32_t offset, const void *data, uint32_t size) {
    static uint32_t printed;
    if (!data || printed > 65536) return 0;
    const char *text = static_cast<const char *>(data);
    char line[160];
    uint32_t n = 0;
    for (uint32_t i = 0; i < size; ++i) {
        const char c = text[i];
        if (c >= 32 && c < 127 && n < sizeof line - 1) line[n++] = c;
        if (c == '\n' || n == sizeof line - 1 || i + 1 == size) {
            if (n) {line[n] = 0; IOLog("HOUJI EXPORT g%u o%u: %s\n", generation, offset, line); printed += n;}
            n = 0;
        }
    }
    return 0;
}
__attribute__((weak)) uint32_t ml_outside_memory_pages;
}
