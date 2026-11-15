// SPDX-License-Identifier: BSD-2-Clause-Patent
// The three names the kernel patch (patches/xnu/houji-debug-console.patch) calls. They carry the native_ prefix because the
// symbol-bearing reference kernel of the prelink step is linked without board archives and stands in for exactly such names.
#include <sys/types.h>

void houji_usb_debug_start(void);
void houji_debug_append(const char *, int);
void houji_reboot_reason_at_reset(void);
void native_houji_console_start(void);
void native_houji_log_append(const char *, int);
void native_houji_reset_hook(void);

void native_houji_console_start(void) { houji_usb_debug_start(); }
void native_houji_log_append(const char *data, int size) { houji_debug_append(data, size); }
void native_houji_reset_hook(void) { houji_reboot_reason_at_reset(); }
