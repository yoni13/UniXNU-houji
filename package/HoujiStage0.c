// SPDX-License-Identifier: BSD-2-Clause-Patent
// Stage 0: the debug console's commands reach into drivers that are not ported to this kernel yet. Each of these stands in for
// one of them and says so; it goes when its driver arrives.
#include <sys/types.h>
#include <sys/systm.h>
#include <pexpert/pexpert.h>
#include <machine/machine_routines.h>
/* osfmk/arm/machine_routines.h declares it only for Mach-private sources; this file has the BSD flags. */
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define ABSENT(name) printf("HOUJI DEBUG: %s is not ported to this kernel yet\n", name)
int houji_usb_phy_start(void);
void houji_usb_phy_state(const char *);
void houji_usb_phy_cold(void);
int houji_ufs_arm_recovery_bcb(void);
void sweet_hid_touch(int, int, int, int);
void sweet_hid_key(int, int);
int sweet_hid_type(const char *);
void houji_framebuffer_shot(unsigned, unsigned);
void houji_framebuffer_shot_region(unsigned, unsigned, unsigned, unsigned, unsigned, unsigned);
void houji_prof_command(const char *);
void houji_camera_command(const char *);
void houji_screen_command(const char *);
void houji_sleep_now(void);
void houji_power_command(const char *);
void houji_pm_report(void);
void houji_batt_command(const char *);
void native_adreno_client_report(void);
void houji_cpu_hotplug(unsigned, int);
void houji_bt_command(const char *);
void houji_wifi_command(const char *);
void houji_power_start(void);
void houji_ram_upload_bind_test_target(void);

/* Only "fastboot boot" is supported for now: the bootloader leaves the USB PHYs running. */
int houji_usb_phy_start(void) { return -1; }
void houji_usb_phy_state(const char *when __unused) {}
void houji_usb_phy_cold(void) {}
int houji_ufs_arm_recovery_bcb(void) { return -1; }
void sweet_hid_touch(int a __unused, int b __unused, int c __unused, int d __unused) {}
void sweet_hid_key(int a __unused, int b __unused) {}
int sweet_hid_type(const char *s __unused) { return -1; }
/* Screenshot of the firmware framebuffer the display presents into (PE_current_console): one luminance hex digit per
 * sample, in the console's SHOT format (perf-test/phone.py shot renders it). Read only; mapped once. */
void houji_framebuffer_shot_region(unsigned x0, unsigned y0, unsigned w, unsigned h, unsigned cols, unsigned rows)
{
	static vm_offset_t pixels;
	static unsigned width, height, stride;
	if (!pixels) {
		PE_Video video = {};
		if (PE_current_console(&video) || !video.v_baseAddr || video.v_depth != 32 || !video.v_width || !video.v_height ||
		    video.v_rowBytes < video.v_width * 4) {
			printf("HOUJI SHOT: unavailable\n");
			return;
		}
		pixels = ml_io_map(video.v_baseAddr, (vm_size_t)video.v_rowBytes * video.v_height);
		width = (unsigned)video.v_width; height = (unsigned)video.v_height; stride = (unsigned)video.v_rowBytes;
	}
	if (!pixels || !cols || !rows || cols > 300 || rows > 700 || !w || !h || x0 + w > width || y0 + h > height) {
		printf("HOUJI SHOT: unavailable\n");
		return;
	}
	printf("HOUJI SHOT BEGIN %u %u panel=%ux%u region=%u,%u,%u,%u\n", cols, rows, width, height, x0, y0, w, h);
	char line[320];
	for (unsigned r = 0; r < rows; r++) {
		const uint32_t y = y0 + (uint32_t)(((uint64_t)r * h + h / rows / 2) / rows);
		for (unsigned c = 0; c < cols; c++) {
			const uint32_t x = x0 + (uint32_t)(((uint64_t)c * w + w / cols / 2) / cols);
			const volatile uint8_t *p = (const volatile uint8_t *)(pixels + (uint64_t)y * stride + (uint64_t)x * 4);
			const unsigned l = (p[2] * 3u + p[1] * 6u + p[0]) / 10u;
			line[c] = "0123456789abcdef"[l >> 4];
		}
		line[cols] = 0;
		printf("SHOT %03u %s\n", r, line);
	}
	printf("HOUJI SHOT END\n");
}
void houji_framebuffer_shot(unsigned cols, unsigned rows)
{
	PE_Video video = {};
	if (PE_current_console(&video) || !video.v_width || !video.v_height) {
		printf("HOUJI SHOT: unavailable\n");
		return;
	}
	houji_framebuffer_shot_region(0, 0, (unsigned)video.v_width, (unsigned)video.v_height, cols, rows);
}
void houji_prof_command(const char *s __unused) { ABSENT("prof"); }
void houji_camera_command(const char *s __unused) { ABSENT("camera"); }
void houji_screen_command(const char *s __unused) { ABSENT("screen"); }
void houji_sleep_now(void) { ABSENT("sleep"); }
void houji_power_command(const char *s __unused) { ABSENT("pw"); }
void houji_pm_report(void) { ABSENT("pm"); }
void houji_batt_command(const char *s __unused) { ABSENT("batt"); }
void native_adreno_client_report(void) { ABSENT("gpu clients"); }
void houji_cpu_hotplug(unsigned a __unused, int b __unused) { ABSENT("cpu on/off"); }
void houji_bt_command(const char *s __unused) { ABSENT("bt"); }
void houji_wifi_command(const char *s __unused) { ABSENT("wifi"); }
void houji_power_start(void) {}
void houji_ram_upload_bind_test_target(void) {}
