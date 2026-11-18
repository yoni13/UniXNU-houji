// SPDX-License-Identifier: BSD-2-Clause-Patent
// houji input for NativeHidInput (UniXNU ShengHidInput): the contact table and sinks it reads, fed by the USB console's
// tap/swipe/home/key commands (sweet_hid_touch, sweet_hid_key) and, once ported, by the touch controller driver.
//
// The table is a sequence lock: native_touch_seq is odd while the contacts change and even when they are consistent; the sink
// is then called and NativeHidInput copies the table on its work loop. NativeHidInput scales a contact from its touch core's
// axis maxima (30480 x 20320) to the HID range, so panel pixels are converted to that space here.
#include <sys/types.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <kern/thread_call.h>

#define CONTACTS 12
#define CORE_X 30480
#define CORE_Y 20320

volatile uint32_t native_touch_seq, native_touch_count, native_touch_hid_reports;
volatile int native_touch_xs[CONTACTS], native_touch_ys[CONTACTS], native_touch_ids[CONTACTS];
extern volatile uint32_t native_hid_input_command;   // HidInputProbe.cpp: 1 = one Home press on the next timer tick

static void (*touch_sink)(void *);
static void *touch_context;
static void (*buttons_sink)(void *, uint32_t);
static void *buttons_context;
static uint32_t buttons_mask;
static uint64_t cpu_touch_time;

/* Read by HoujiCpuDvfs.cpp. C definitions already have the C linkage used by its extern "C" declaration. */
uint64_t houji_cpu_touch_time(void);
uint64_t houji_cpu_touch_time(void)
{
	return __atomic_load_n(&cpu_touch_time, __ATOMIC_RELAXED);
}

void native_touch_set_sink(void (*sink)(void *), void *context);
void native_touch_set_sink(void (*sink)(void *), void *context)
{
	touch_context = context;
	__asm__ volatile("dmb ish" ::: "memory");
	touch_sink = sink;
}

void native_buttons_set_sink(void (*sink)(void *, uint32_t), void *context);
void native_buttons_set_sink(void (*sink)(void *, uint32_t), void *context)
{
	buttons_context = context;
	__asm__ volatile("dmb ish" ::: "memory");
	buttons_sink = sink;
}

static unsigned panel_width(void)
{
	PE_Video video = {};
	return PE_current_console(&video) || !video.v_width ? 1200 : (unsigned)video.v_width;
}

static unsigned panel_height(void)
{
	PE_Video video = {};
	return PE_current_console(&video) || !video.v_height ? 2670 : (unsigned)video.v_height;
}

/* A lift is the last change of a gesture: if NativeHidInput's work loop lost the wakeup of that publication (its copy and the next
 * change raced), iOS kept the finger down and took every later gesture for the same touch until a new touch came (root139: two swipe
 * pairs drew 1 and 0 frames, after one tap 30 and 114). So 30 ms after a lift the table is published once more, unless
 * houji_touch_republish=0. Counted in houji_touch_republished. */
volatile uint32_t houji_touch_republished;
static thread_call_t republish_call;
static void republish(thread_call_param_t p0 __unused, thread_call_param_t p1 __unused)
{
	void (*sink)(void *) = touch_sink;
	if (sink && !(native_touch_seq & 1)) { sink(touch_context); houji_touch_republished = houji_touch_republished + 1; }
}
static void republish_after_lift(void)
{
	static int enabled = -1;
	if (enabled < 0) { unsigned v = 1; PE_parse_boot_argn("houji_touch_republish", &v, sizeof v); enabled = v != 0; }
	if (!enabled) return;
	if (!republish_call) {
		thread_call_t call = thread_call_allocate_with_options(republish, NULL, THREAD_CALL_PRIORITY_HIGH, THREAD_CALL_OPTIONS_ONCE);
		if (!call || !__atomic_compare_exchange_n(&republish_call, &(thread_call_t){NULL}, call, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			if (call) thread_call_free(call);
		}
	}
	if (republish_call) {
		uint64_t deadline = 0;
		clock_interval_to_deadline(30, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed(republish_call, deadline);
	}
}

/* One contact goes down, moves or lifts (panel pixels); the whole table is published after each change. */
void houji_touch_contact(int finger, int down, int x, int y);
void houji_touch_contact(int finger, int down, int x, int y)
{
	/* Keep the input hot path lock-free; moves extend the same interaction window as fresh contacts. */
	if (down) __atomic_store_n(&cpu_touch_time, mach_absolute_time(), __ATOMIC_RELAXED);
	const unsigned width = panel_width(), height = panel_height();
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if ((unsigned)x >= width) x = (int)width - 1;
	if ((unsigned)y >= height) y = (int)height - 1;
	const int cx = (int)((int64_t)x * CORE_X / width), cy = (int)((int64_t)y * CORE_Y / height);
	uint32_t seq = native_touch_seq;
	native_touch_seq = seq | 1;
	__asm__ volatile("dmb ish" ::: "memory");
	unsigned count = native_touch_count, at = count;
	for (unsigned i = 0; i < count; i++) {
		if (native_touch_ids[i] == finger) { at = i; break; }
	}
	if (down) {
		if (at == count && count < CONTACTS) native_touch_count = ++count;
		if (at < count) { native_touch_ids[at] = finger; native_touch_xs[at] = cx; native_touch_ys[at] = cy; }
	} else if (at < count) {
		for (unsigned i = at; i + 1 < count; i++) {
			native_touch_ids[i] = native_touch_ids[i + 1]; native_touch_xs[i] = native_touch_xs[i + 1];
			native_touch_ys[i] = native_touch_ys[i + 1];
		}
		native_touch_count = count - 1;
	}
	__asm__ volatile("dmb ish" ::: "memory");
	native_touch_seq = (seq | 1) + 1;
	void (*sink)(void *) = touch_sink;
	if (sink) sink(touch_context);
	if (!down) republish_after_lift();
}

/* The console's tap/swipe. */
void sweet_hid_touch(int finger, int down, int x, int y);
void sweet_hid_touch(int finger, int down, int x, int y) { houji_touch_contact(finger, down, x, y); }

/* The console's keys: 0 power, 1 volume up, 2 volume down, 3 home (the old SweetHID numbering). NativeHidInput takes power and
 * volume as a report 2 mask (bits 1-3) through the buttons sink; Home is its timer's mailbox. */
void sweet_hid_key(int key, int down);
void sweet_hid_key(int key, int down)
{
	if (key == 3) {
		if (down) native_hid_input_command = 1;
		return;
	}
	static const uint32_t bits[3] = { 1u << 1, 1u << 2, 1u << 3 };
	if (key < 0 || key > 2) return;
	buttons_mask = down ? (buttons_mask | bits[key]) : (buttons_mask & ~bits[key]);
	void (*sink)(void *, uint32_t) = buttons_sink;
	if (sink) sink(buttons_context, buttons_mask);
}
