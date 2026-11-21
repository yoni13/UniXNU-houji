// SPDX-License-Identifier: BSD-2-Clause-Patent
// Per-CPU idle time for the houji CPU frequency governor (HoujiCpuDvfs.cpp), the load input the iOS 16 governor took from
// houji_cpu_idle_mach(): the processor's accumulated idle time in mach ticks, read as XNU's own processor CPU-load info does
// (osfmk/kern/processor.c, recount_processor_usage on pr_recount). 0 for a CPU that is not started.
/* Compiled with osfmk/kern/processor.c's command (build-houji-kit.py RECORD_OVERRIDES): the Mach-private view of struct processor. */
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

/* For the console's thread timeline (HoujiUsbConsole.c "tl"): where a thread is now. 1 on a CPU (the processor it last ran on is
 * running it), 2 runnable but waiting for a CPU, 0 waiting; *cpu is the processor it last ran on (-1 for none). Read without locks: a
 * sample may be off by a switch. */
#include <kern/thread.h>
int houji_thread_cpu_state(thread_t thread, int *cpu);
int houji_thread_cpu_state(thread_t thread, int *cpu)
{
	processor_t processor = thread->last_processor;
	*cpu = processor != PROCESSOR_NULL ? processor->cpu_id : -1;
	if (processor != PROCESSOR_NULL && processor->active_thread == thread) {
		return 1;
	}
	return (thread->state & TH_RUN) ? 2 : 0;
}
