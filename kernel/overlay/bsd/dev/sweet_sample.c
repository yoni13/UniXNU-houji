/*
 * SWEET process sampler: where is a daemon stuck?
 *
 * The phone has no debugger and daemons log nowhere readable. Every
 * interval a kernel thread prints, for each process named in boot-arg
 * sweetsample (comma separated, default "SpringBoard,backboardd"; "-" = off)
 * and for each of its threads, the user pc/lr/sp and a frame-pointer
 * backtrace as "SMP name[pid] tN pc ... lr ... bt ..." (header line with the
 * shared region slide and the main binary's mach header, to symbolicate
 * with tools/sweet-crash.py). A thread blocked in a syscall shows the
 * syscall stub in pc and its caller in lr/bt.
 *
 * First sample 90 s after start, then every 120 s, 40 samples at most.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/proc_internal.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <mach/thread_act.h>
#include <pexpert/pexpert.h>
/* BSD uses these osfmk counters via declarations, as vm/vm_unix.c does. */
extern unsigned int vm_page_free_count, vm_page_active_count;
extern unsigned int vm_page_inactive_count, vm_page_wire_count;

extern task_t proc_task(proc_t);
extern int hz;
extern void sweet_sample_start(void);

static char sweet_sample_names[128];

static bool
sweet_sample_wanted(const char *name)
{
	size_t n = strlen(name);
	for (const char *t = sweet_sample_names; *t;) {
		const char *end = strchr(t, ',');
		size_t tl = end ? (size_t)(end - t) : strlen(t);
		if (tl == n && n > 0 && strncmp(t, name, n) == 0) {
			return true;
		}
		if (!end) {
			break;
		}
		t = end + 1;
	}
	return false;
}

extern void sweet_sample_task(const char *name, int pid, task_t task);  /* osfmk/kern/exception.c */

static int
sweet_sample_proc(proc_t p, __unused void *arg)
{
	char name[32];
	proc_name(proc_pid(p), name, sizeof(name));
	if (sweet_sample_wanted(name)) {
		task_t task = proc_task(p);
		if (task != TASK_NULL) {
			task_reference(task);
			printf("SMP BEGIN %s[%d]\n", name, proc_pid(p));
			sweet_sample_task(name, proc_pid(p), task);
			task_deallocate(task);
		}
	}
	return PROC_RETURNED;
}

#if defined(ARM64_BOARD_CONFIG_SWEET)
extern void houji_ufs_checkpoint(const char *, unsigned);
extern void houji_framebuffer_counts(uint64_t *, uint64_t *);
extern void houji_framebuffer_report(void);
extern void houji_location_ipc_report(void);
extern void houji_location_exit_report(void);
extern void houji_smp_report(void);
extern void houji_touch_report(void);
extern void houji_touch_driver_report(void);
extern bool gIOPlatformUUIDAndSerialDone;
static int
houji_list_proc(proc_t p, void *arg)
{
	char name[32];
	proc_name(proc_pid(p), name, sizeof(name));
	if (arg != NULL || sweet_sample_wanted(name) || proc_pid(p) == 1)
		printf("HOUJI PROC: %s[%d]\n", name, proc_pid(p));
	return PROC_RETURNED;
}
#endif

static void
sweet_sample_thread(__unused void *arg, __unused wait_result_t wr)
{
	static int chan;
#if defined(ARM64_BOARD_CONFIG_SWEET)
	int trial = 0;
	PE_parse_boot_argn("houji_userland_trial", &trial, sizeof(trial));
	if (trial == 1) {
		int requested_samples = 180;
		PE_parse_boot_argn("houji_trial_samples", &requested_samples, sizeof(requested_samples));
		if (requested_samples < 12 || requested_samples > 720)
			requested_samples = 180;
		unsigned total_samples = (unsigned)requested_samples;
		if (!PE_parse_boot_argn("sweetsample", sweet_sample_names, sizeof(sweet_sample_names)))
			strlcpy(sweet_sample_names, "SpringBoard,backboardd", sizeof(sweet_sample_names));
		for (unsigned i = 1; i <= total_samples; i++) {
			(void)tsleep(&chan, PZERO, "houjiusr", 5 * hz);
			printf("HOUJI USERLAND: process inventory sample %u/%u\n", i, total_samples);
			proc_iterate(PROC_ALLPROCLIST, houji_list_proc, i == 1 ? (void *)1 : NULL, NULL, NULL);
			printf("HOUJI VM: free=%u active=%u inactive=%u wired=%u page_bytes=%lu\n",
			    vm_page_free_count, vm_page_active_count, vm_page_inactive_count,
			    vm_page_wire_count, (unsigned long)PAGE_SIZE);
			uint64_t swaps = 0, presents = 0;
			houji_framebuffer_counts(&swaps, &presents);
			printf("HOUJI DISPLAY: swaps=%llu presents=%llu\n", swaps, presents);
			houji_framebuffer_report();
			printf("HOUJI ID: platform UUID published=%u\n", gIOPlatformUUIDAndSerialDone);
			if ((i % 4) == 0) {
				houji_location_ipc_report();
				houji_location_exit_report();
				houji_smp_report();
				houji_touch_report();houji_touch_driver_report();
				houji_ufs_checkpoint("before userland stack sampling", 0);
				proc_iterate(PROC_ALLPROCLIST, sweet_sample_proc, NULL, NULL, NULL);
			}
			char stage[96];
			snprintf(stage, sizeof(stage), "userland process inventory sample %u/%u", i, total_samples);
			houji_ufs_checkpoint(stage, i == total_samples ? 7 : 0);
		}
		thread_terminate(current_thread());
		return;
	}
#endif
	if (!PE_parse_boot_argn("sweetsample", sweet_sample_names, sizeof(sweet_sample_names))) {
		strlcpy(sweet_sample_names, "SpringBoard,backboardd", sizeof(sweet_sample_names));
	}
	if (sweet_sample_names[0] == '-') {
		thread_terminate(current_thread());
		return;
	}
	(void)tsleep(&chan, PZERO, "sweetsmp", 90 * hz);
	for (int i = 0; i < 40; i++) {
		printf("SMP --- sample %d\n", i);
		proc_iterate(PROC_ALLPROCLIST, sweet_sample_proc, NULL, NULL, NULL);
		(void)tsleep(&chan, PZERO, "sweetsmp", 120 * hz);
	}
	thread_terminate(current_thread());
}

void
sweet_sample_start(void)
{
	thread_t thread;
	if (kernel_thread_start(sweet_sample_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
