// SPDX-License-Identifier: BSD-2-Clause-Patent
// A pass-through SMMU context for a stream whose controller is halted (the debug USB controller, stream 0x40): the route the
// iOS 16 port of this phone uses. Never panics; returns null or the refusal.
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include "SmmuTakeover.h"

extern "C" const char *houji_smmu_route_stream(uint16_t stream, uint64_t dma, uint64_t bytes, uint32_t report[3]);

static uint32_t read32(uintptr_t base, unsigned offset) { return *reinterpret_cast<volatile uint32_t *>(base + offset); }
static void write32(uintptr_t base, unsigned offset, uint32_t value) { *reinterpret_cast<volatile uint32_t *>(base + offset) = value; }

extern "C" const char *houji_smmu_route_stream(uint16_t stream, uint64_t dma, uint64_t bytes, uint32_t report[3])
{
  static IOMemoryMap *keep;
  report[0] = report[1] = report[2] = 0;
  if (!keep) {
    IODeviceMemory *memory = IODeviceMemory::withRange(0x15000000ULL, 0xd0000);
    if (memory) {
      keep = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
      memory->release();
    }
  }
  if (!keep) return "SMMU map failed";
  const uintptr_t smmu = static_cast<uintptr_t>(keep->getVirtualAddress());
  auto rd = [smmu](unsigned offset) { return read32(smmu, offset); };
  UfsSmmuPlan plan = planUfsSmmu(stream, dma, bytes, rd);
  if (plan.error) { report[0] = plan.slot; report[1] = plan.smr; report[2] = plan.s2cr; return plan.error; }
  const char *failure = configureUfsSmmu(plan, stream, rd, [smmu](unsigned offset, uint32_t value) {
    write32(smmu, offset, value);
    __asm__ volatile ("dsb sy" ::: "memory");
  });
  report[0] = plan.slot | (plan.bank << 16);
  report[1] = read32(smmu, 0x1000 + 4 * plan.bank);
  report[2] = read32(smmu, (plan.pages + plan.bank) * 4096);
  return failure;
}
