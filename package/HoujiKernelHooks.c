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
void native_houji_reset_hook(void) { houji_reboot_reason_at_reset(); }
