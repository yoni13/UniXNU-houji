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

/* For the console's thread timeline: the user registers of a thread running on another CPU now. Its saved user state is only as
 * new as its last kernel entry (a fault, a system call or an interrupt), so samples of a running thread piled up at those places
 * (root296: 807 of 3691 ms of backboardd's encoding thread "in" the tail of one memcpy, the store that had last faulted). A
 * cross-call to its CPU interrupts it; the handler reads the state that interrupt saved. One request at a time (one sampler);
 * returns 0 and pc, lr, fp when the thread was the one running there, -1 otherwise (it moved, or the CPU did not answer within
 * 200 us; a late answer then lands in the static slot, never on a stack). */
#include <arm/machine_routines.h>
static struct {
	thread_t thread;
	volatile uint32_t pending, done, found;
	uint64_t pc, lr, fp;
} houji_sample_slot;

static void
houji_sample_cb(void *arg)
{
	(void)arg;
	if (current_thread() == houji_sample_slot.thread) {
		arm_saved_state_t *ss = get_user_regs(houji_sample_slot.thread);
		if (ss) {
			houji_sample_slot.pc = get_saved_state_pc(ss);
			houji_sample_slot.lr = get_saved_state_lr(ss);
			houji_sample_slot.fp = get_saved_state_fp(ss);
			houji_sample_slot.found = 1;
		}
	}
	__atomic_store_n(&houji_sample_slot.done, 1, __ATOMIC_RELEASE);
	__atomic_store_n(&houji_sample_slot.pending, 0, __ATOMIC_RELEASE);
}

int houji_sample_running(thread_t thread, int cpu, uint64_t out[3]);
int
houji_sample_running(thread_t thread, int cpu, uint64_t out[3])
{
	if (cpu < 0 || cpu > ml_get_max_cpu_number() || __atomic_load_n(&houji_sample_slot.pending, __ATOMIC_ACQUIRE)) {
		return -1;
	}
	houji_sample_slot.thread = thread;
	houji_sample_slot.found = 0;
	houji_sample_slot.done = 0;
	__atomic_store_n(&houji_sample_slot.pending, 1, __ATOMIC_RELEASE);
	if (cpu_xcall(cpu, houji_sample_cb, &houji_sample_slot) != KERN_SUCCESS) {
		__atomic_store_n(&houji_sample_slot.pending, 0, __ATOMIC_RELEASE);
		return -1;
	}
	uint64_t deadline = 0;
	nanoseconds_to_absolutetime(200000, &deadline);
	deadline += mach_absolute_time();
	while (!__atomic_load_n(&houji_sample_slot.done, __ATOMIC_ACQUIRE)) {
		if (mach_absolute_time() > deadline) {
			return -1;
		}
	}
	if (!houji_sample_slot.found) {
		return -1;
	}
	out[0] = houji_sample_slot.pc;
	out[1] = houji_sample_slot.lr;
	out[2] = houji_sample_slot.fp;
	return 0;
}

/* For the console's thread timeline: what CPU cpu runs now. 0 idle (its idle thread or none), 1 a thread: *tid its id and *task its
 * task (an address the timeline matches to a process at its end, never dereferenced there). Read without locks, as above. */
int houji_cpu_occupant(int cpu, uint64_t *tid, uint64_t *task);
int
houji_cpu_occupant(int cpu, uint64_t *tid, uint64_t *task)
{
	processor_t processor = cpu_to_processor(cpu);
	if (processor == PROCESSOR_NULL) {
		return 0;
	}
	thread_t thread = processor->active_thread;
	if (thread == THREAD_NULL || thread == processor->idle_thread) {
		return 0;
	}
	*tid = thread->thread_id;
	*task = (uint64_t)(uintptr_t)get_threadtask(thread);
	return 1;
}

/* Timeline events (houji_tev) from the display and GPU paths while a console tl runs: the vsync tick, the swaps' stages, the GPU's
 * ring kicks, its interrupts and the driver's fence passes, each with mach time, a value and the CPU. The buffer is the timeline's
 * (HoujiUsbConsole.c); an event past its end is dropped. Callable from interrupt context. */
#include <kern/cpu_number.h>
struct houji_tev_rec {
	uint64_t t;
	uint32_t v;
	uint16_t kind, cpu;
};
struct houji_tev_rec *volatile houji_tev_buf;
volatile uint32_t houji_tev_n;
uint32_t houji_tev_cap;
void houji_tev(unsigned kind, uint32_t v);
void
houji_tev(unsigned kind, uint32_t v)
{
	struct houji_tev_rec *buf = houji_tev_buf;
	if (buf == NULL) {
		return;
	}
	const uint32_t i = __atomic_fetch_add(&houji_tev_n, 1, __ATOMIC_RELAXED);
	if (i >= houji_tev_cap) {
		return;
	}
	buf[i].t = mach_absolute_time();
	buf[i].v = v;
	buf[i].kind = (uint16_t)kind;
	buf[i].cpu = (uint16_t)cpu_number();
}
