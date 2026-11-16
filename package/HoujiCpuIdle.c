// SPDX-License-Identifier: BSD-2-Clause-Patent
// Per-CPU idle time for the houji CPU frequency governor (HoujiCpuDvfs.cpp), the load input the iOS 16 governor took from
// houji_cpu_idle_mach(): the processor's accumulated idle time in mach ticks, read as XNU's own processor CPU-load info does
// (osfmk/kern/processor.c, recount_processor_usage on pr_recount). 0 for a CPU that is not started.
/* The Mach-private view osfmk's own files compile with: struct processor and the recount interface. */
#ifndef MACH_KERNEL_PRIVATE
#define MACH_KERNEL_PRIVATE 1
#endif
#include <kern/processor.h>
#include <kern/recount.h>

uint64_t houji_cpu_idle_mach(int cpu);
uint64_t houji_cpu_idle_mach(int cpu)
{
	processor_t processor = cpu_to_processor(cpu);
	if (processor == PROCESSOR_NULL) {
		return 0;
	}
	struct recount_usage usage = { 0 };
	uint64_t idle_time = 0;
	recount_processor_usage(&processor->pr_recount, &usage, &idle_time);
	return idle_time;
}
