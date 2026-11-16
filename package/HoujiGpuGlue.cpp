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
