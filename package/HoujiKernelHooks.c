// SPDX-License-Identifier: BSD-2-Clause-Patent
// The three names the kernel patch (patches/xnu/houji-debug-console.patch) calls. They carry the native_ prefix because the
// symbol-bearing reference kernel of the prelink step is linked without board archives and stands in for exactly such names.
#include <sys/types.h>
#include <sys/systm.h>
#include <kern/debug.h>
#include <pexpert/pexpert.h>

void houji_usb_debug_start(void);
void houji_debug_append(const char *, int);
void houji_reboot_reason_at_reset(void);
void native_houji_console_start(void);
void native_houji_log_append(const char *, int);
void native_houji_reset_hook(void);

/* houji_usb_require=1: a console that did not start is a panic that shows why (the driver's own notes), since without the
 * console there is no other way to read them. */
void native_houji_console_start(void)
{
	extern int houji_usb_started(void);
	extern const char *houji_usb_notes_tail(unsigned);
	int require = 0;
	houji_usb_debug_start();
	if (PE_parse_boot_argn("houji_usb_require", &require, sizeof(require)) && require == 1 && !houji_usb_started()) {
		panic("HOUJI USB did not start: %s", houji_usb_notes_tail(600));
	}
}
void native_houji_log_append(const char *data, int size) { houji_debug_append(data, size); }
/* PSCI SYSTEM_RESET. The platform expert of this kernel (ArmDTPlatform) has no restart of its own: PEHaltRestart returns. */
void houji_psci_reset(void);
void houji_psci_reset(void)
{
	register uint64_t x0 __asm__("x0") = 0x84000009u;
	__asm__ volatile("dsb sy\n\tsmc #0" : "+r"(x0) :: "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14",
	    "x15", "x16", "x17", "cc", "memory");
}
/* Every restart of this kernel passes here (halt_all_cpus and the panic restart): the console sends what is left of the log
 * after a panic, the reboot reason is set when asked for, and the phone resets. */
void native_houji_reset_hook(void)
{
	extern void houji_usb_panic_drain(void);
	houji_usb_panic_drain();
	houji_reboot_reason_at_reset();
	houji_psci_reset();
}
