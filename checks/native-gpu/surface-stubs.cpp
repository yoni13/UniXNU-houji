// SPDX-License-Identifier: MIT
// Host peers for this fork's IOSurface dirty tracking (SweetSurface.cpp). Tracking off and no surface found: NativeAdrenoMemory
// takes its plain maintenance path, which is what these fixtures exercise.
#include <cstdint>
extern "C" int houji_surface_seed_skip_enabled(void) {return 0;}
extern "C" void *houji_surface_retain_for_physical(uint64_t) {return nullptr;}
extern "C" uint32_t houji_surface_seed_of(void *) {return 0;}
extern "C" void houji_surface_release(void *) {}
extern "C" int houji_surface_lazy_acquire_enabled(void) {return 0;}
extern "C" void houji_surface_mark_gpu_dirty(void *) {}
