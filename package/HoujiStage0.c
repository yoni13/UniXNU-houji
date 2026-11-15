// SPDX-License-Identifier: BSD-2-Clause-Patent
// Stage 0: the debug console's commands reach into drivers that are not ported to this kernel yet. Each of these stands in for
// one of them and says so; it goes when its driver arrives.
#include <sys/types.h>
#include <sys/systm.h>

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
void houji_framebuffer_shot(unsigned a __unused, unsigned b __unused) { ABSENT("shot"); }
void houji_framebuffer_shot_region(unsigned a __unused, unsigned b __unused, unsigned c __unused, unsigned d __unused, unsigned e __unused,
    unsigned f __unused) { ABSENT("shotr"); }
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
