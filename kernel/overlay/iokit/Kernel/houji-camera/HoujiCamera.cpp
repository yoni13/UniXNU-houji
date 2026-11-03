// SPDX-License-Identifier: BSD-2-Clause-Patent
// Camera bring-up on houji (Xiaomi 14), stage 1: power each camera in turn and find what answers on its bus. Boot argument houji_camera=1
// (off by default); runs once, 70 s into the boot, powering every camera down again after its scan. USB console: "camera probe" (all),
// "camera probe N" (cell N only).
// Facts from the handset's stock dtbo (fragments 67-70, qcom,cam-sensorN): the front sensor (cell 1) and its EEPROM sit on QUP2 SE1's
// I2C (SDA GPIO 4, SCL GPIO 5); the rear ones on CCI: CCI0 master 0 (GPIO 113/114) tele, CCI0 master 1 (115/116) wide, CCI1 master 0
// (117/118, fragment@69's cci_i2c_*2 pins) ultra-wide. Each
// has an MCLK pin (function 1) fed by a camcc MCLK RCG/branch pair (camcc-sm8650.c), an active-low reset GPIO, and supplies (vio, vana,
// vdig); the rear vana supplies are fixed regulators switched by a GPIO. camcc needs the MMCX rail (on for the display), GCC's camera
// AHB (0x26004) and XO (0x26028) branches and, for the MCLKs, the Titan top GDSC. I2C is driven by toggling the two pins (open drain:
// low = output 0, high = input), so no QUP or CCI programming is involved; the probe only reads.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include "../houji-adsp/QcomRpmh.h"

// [0] stage (negative: the step that failed), [1] rails found (bit 0 BOB2, then 3 bits per cell), [2] last mclk branch after enable,
// [3] devices answering, [4..7] the first addresses (cell << 8 | address), [8] Titan top GDSC (high bits of GDSCR; bit 0: power-up
// complete), [9] cells with a device
extern "C" {volatile int32_t native_camera_stats[10];}

namespace {
volatile int32_t *const cs = native_camera_stats;
struct Window {IOMemoryMap *map; volatile uint32_t *va;};
bool mapWindow(Window &w, uint64_t base, uint64_t size) {
    IODeviceMemory *m = IODeviceMemory::withRange(base, size);
    w.map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (m) m->release();
    w.va = w.map ? reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}
void release(Window &w) {if (w.map) w.map->release(); w = {};}
void dsb() {__asm__ volatile("dsb sy" ::: "memory");}

constexpr uint64_t kTlmm = 0xf100000, kGcc = 0x100000, kCamcc = 0x0ade0000;
unsigned kSda = 4, kScl = 5;   // the bus being probed
constexpr uint64_t kRscDrv = 0x17a20000, kRscTcs = 0xd00, kCmdDb = 0x81c60000, kCmdDbSize = 0x20000;
Window gTlmm, gGcc, gCamcc;

// ---- TLMM: config at pin * 0x1000 (pull 1:0, function 5:2, drive 8:6, output enable 9), io at +4 (bit 0 in, bit 1 out) ----
volatile uint32_t &pinCfg(unsigned pin) {return gTlmm.va[pin * 0x1000 / 4];}
volatile uint32_t &pinIo(unsigned pin) {return gTlmm.va[(pin * 0x1000 + 4) / 4];}
// Pins with an island (egpio) function belong to the application processor only while EGPIO_ENABLE (bit 12) is set; EGPIO_PRESENT is
// bit 11 (pinctrl-sm8650.c, msm_pinmux_set_mux). Without it writes here do not reach the pin.
void setCfg(unsigned pin, uint32_t v) {pinCfg(pin) = v | ((pinCfg(pin) & (1u << 11)) ? 1u << 12 : 0u);}
void pinGpio(unsigned pin, bool output, int level) {
    if (level >= 0) pinIo(pin) = level ? 2u : 0u;
    setCfg(pin, (output ? 1u << 9 : 0u) | (1u << 6));   // function 0 (GPIO), 4 mA, no pull
    dsb();
}

// ---- I2C by hand, about 100 kHz ----
void half() {IODelay(5);}
void sdaLow() {pinIo(kSda) = 0; setCfg(kSda, (1u << 9) | (1u << 6)); dsb();}
void sdaRelease() {setCfg(kSda, 1u << 6); dsb();}
void sclLow() {pinIo(kScl) = 0; setCfg(kScl, (1u << 9) | (1u << 6)); dsb();}
bool sclRelease() {setCfg(kScl, 1u << 6); dsb(); for (unsigned i = 0; i < 1000; ++i) {if (pinIo(kScl) & 1) return true; IODelay(1);} return false;}
bool sdaRead() {return pinIo(kSda) & 1;}
void start() {sdaRelease(); sclRelease(); half(); sdaLow(); half(); sclLow(); half();}
void stop() {sdaLow(); half(); sclRelease(); half(); sdaRelease(); half();}
bool writeByte(uint8_t b) {   // true: acknowledged
    for (int i = 7; i >= 0; --i) {if ((b >> i) & 1) sdaRelease(); else sdaLow(); half(); sclRelease(); half(); sclLow();}
    sdaRelease(); half(); sclRelease(); half();
    const bool ack = !sdaRead();
    sclLow(); half();
    return ack;
}
uint8_t readByte(bool ack) {
    uint8_t b = 0;
    sdaRelease();
    for (int i = 7; i >= 0; --i) {half(); sclRelease(); half(); b = (uint8_t)(b << 1 | (sdaRead() ? 1 : 0)); sclLow();}
    if (ack) sdaLow(); else sdaRelease();
    half(); sclRelease(); half(); sclLow(); sdaRelease();
    return b;
}
bool probe(uint8_t addr) {start(); const bool a = writeByte((uint8_t)(addr << 1)); stop(); return a;}
// a 16-bit register address, one byte back (the sensors' convention); -1 when not acknowledged
int read16(uint8_t addr, uint16_t reg) {
    start();
    if (!writeByte((uint8_t)(addr << 1)) || !writeByte((uint8_t)(reg >> 8)) || !writeByte((uint8_t)reg)) {stop(); return -1;}
    start();
    if (!writeByte((uint8_t)(addr << 1 | 1))) {stop(); return -1;}
    const uint8_t v = readByte(false);
    stop();
    return v;
}

// ---- RPMh: the camera rails through active TCS 2 (the ADSP uses 1, Wi-Fi 0) ----
struct Rpmh {
    uint8_t *db; Window rsc; QcomRscLayout layout;
    bool open() {
        db = static_cast<uint8_t *>(IOMallocData(kCmdDbSize));
        Window dbw = {};
        if (!db || !mapWindow(dbw, kCmdDb, kCmdDbSize)) return false;
        for (uint64_t i = 0; i < kCmdDbSize; i += 4) {const uint32_t w = dbw.va[i / 4]; memcpy(db + i, &w, 4);}
        release(dbw);
        if (!mapWindow(rsc, kRscDrv, 0x10000)) return false;
        layout = qcom_rsc_layout(rsc.va[0]);
        return true;
    }
    void close() {if (db) IOFreeData(db, kCmdDbSize); db = nullptr; release(rsc);}
    void send(uint32_t address, uint32_t value) {
        QcomRpmhWrite writes[16];
        const unsigned n = qcom_rpmh_active_request(&layout, 2, &address, &value, 1, writes, 16);
        for (unsigned i = 0; i < n; ++i) rsc.va[(kRscTcs + writes[i].offset) / 4] = writes[i].value;
        dsb(); IOSleep(2);
        const unsigned m = qcom_rpmh_finish(&layout, 2, writes, 16);
        for (unsigned i = 0; i < m; ++i) rsc.va[(kRscTcs + writes[i].offset) / 4] = writes[i].value;
    }
    bool ldo(const char *name, uint32_t mv, bool on, uint32_t mode = 7) {   // mode 7: LDO HPM; 6: BOB auto
        QcomRpmhResource r;
        if (qcom_cmd_db_find(db, kCmdDbSize, name, &r)) return false;
        if (on) {send(r.address + 8, mode); send(r.address, mv); send(r.address + 4, 1);}
        else send(r.address + 4, 0);
        IOSleep(3);
        return true;
    }
};

bool waitBits(volatile uint32_t *reg, uint32_t mask, bool set, unsigned us) {
    for (unsigned i = 0; i < us; i += 10) {if (((*reg & mask) != 0) == set) return true; IODelay(10);}
    return ((*reg & mask) != 0) == set;
}

struct Rail {const char *name; uint32_t mv; bool keep;};   // keep: shared, left on afterwards
struct Camera {
    const char *name; unsigned sda, scl, mclkPin, reset; int vanaGpio; uint32_t rcg, cbcr; Rail vio, vana, vdig;
};
// cell index = position; vana with a GPIO has no rail
const Camera kCameras[4] = {
    {"wide", 115, 116, 100, 108, 127, 0x15000, 0x15018, {"ldob10", 1800, true}, {nullptr, 0, false}, {"ldom2", 1250, false}},
    {"front", 4, 5, 104, 7, -1, 0x15070, 0x15088, {"ldom5", 1800, false}, {"ldom3", 2800, false}, {"ldom1", 1100, false}},
    {"ultra-wide", 117, 118, 101, 109, 126, 0x1501c, 0x15034, {"ldob10", 1800, true}, {nullptr, 0, false}, {"ldoc2", 1104, false}},
    {"tele", 113, 114, 103, 110, 125, 0x15054, 0x1506c, {"ldob10", 1800, true}, {nullptr, 0, false}, {"ldoe1", 1104, false}},
};
volatile int32_t gOnly = -1;   // a console request for one cell

unsigned runCamera(Rpmh &rpmh, unsigned cell, unsigned &total) {
    const Camera &c = kCameras[cell];
    kSda = c.sda; kScl = c.scl;
    const unsigned pins[5] = {c.sda, c.scl, c.reset, c.mclkPin, c.vanaGpio >= 0 ? (unsigned)c.vanaGpio : c.reset};
    uint32_t saved[5];
    for (unsigned i = 0; i < 5; ++i) saved[i] = pinCfg(pins[i]);
    IOLog("HOUJI CAMERA: cell %u %s, pins before: sda %08x scl %08x reset %08x mclk %08x vana %08x\n", cell, c.name, saved[0], saved[1],
          saved[2], saved[3], saved[4]);
    // supplies in the sensor's order: vio, vana, vdig
    uint32_t found = 0;
    if (rpmh.ldo(c.vio.name, c.vio.mv, true)) found |= 1;
    if (c.vanaGpio >= 0) {pinGpio((unsigned)c.vanaGpio, true, 1); found |= 2;}
    else if (rpmh.ldo(c.vana.name, c.vana.mv, true)) found |= 2;
    if (rpmh.ldo(c.vdig.name, c.vdig.mv, true)) found |= 4;
    cs[1] = cs[1] | (int32_t)(found << (1 + 3 * cell));
    IOSleep(5);
    // MCLK: RCG from the crystal, divider 1 (CFG source 0, SRC_DIV 2 * 1 - 1), update, then the branch; then the pin to the clock
    volatile uint32_t *rcg = gCamcc.va + c.rcg / 4, *cbcr = gCamcc.va + c.cbcr / 4;
    rcg[1] = 1u; dsb();
    rcg[0] |= 1u; dsb();
    waitBits(&rcg[0], 1u, false, 2000);
    *cbcr |= 1u; dsb();
    const bool running = waitBits(cbcr, 1u << 31, false, 2000);
    cs[2] = (int32_t)*cbcr;
    setCfg(c.mclkPin, (1u << 2) | (1u << 6)); dsb();   // function 1, 4 mA
    // reset: low, then released after the clock runs
    pinGpio(c.reset, true, 0); IOSleep(5);
    pinGpio(c.reset, true, 1); IOSleep(20);
    // the bus: both lines released, a scan, then the ID registers of whatever answered
    pinGpio(kSda, false, -1); pinGpio(kScl, false, -1);
    IOSleep(1);
    IOLog("HOUJI CAMERA: cell %u rails %x, mclk %s (rcg cmd %08x cbcr %08x), bus idle sda %u scl %u\n", cell, found,
          running ? "running" : "NOT running", (unsigned)rcg[0], (unsigned)*cbcr, (unsigned)(pinIo(kSda) & 1), (unsigned)(pinIo(kScl) & 1));
    unsigned count = 0;
    if ((pinIo(kSda) & 1) && (pinIo(kScl) & 1)) for (uint8_t a = 0x08; a < 0x78; ++a) {
        if (!probe(a)) continue;
        if (total < 4) cs[4 + total] = (int32_t)(cell << 8 | a);
        ++count; ++total;
        const int o0 = read16(a, 0x300a), o1 = read16(a, 0x300b), o2 = read16(a, 0x300c), s0 = read16(a, 0x0016), s1 = read16(a, 0x0017),
                  m0 = read16(a, 0x0000), m1 = read16(a, 0x0001);
        IOLog("HOUJI CAMERA: cell %u device 0x%02x answers: 300a-300c %02x %02x %02x, 0016-0017 %02x %02x, 0000-0001 %02x %02x\n", cell, a,
              o0 & 0xff, o1 & 0xff, o2 & 0xff, s0 & 0xff, s1 & 0xff, m0 & 0xff, m1 & 0xff);
    }
    IOLog("HOUJI CAMERA: cell %u %s: %u devices\n", cell, c.name, count);
    // power down: reset, pins back, clock, supplies in reverse (shared ones stay)
    pinGpio(c.reset, true, 0);
    if (c.vanaGpio >= 0) pinGpio((unsigned)c.vanaGpio, true, 0);
    IOSleep(1);
    for (unsigned i = 0; i < 5; ++i) pinCfg(pins[i]) = saved[i];
    dsb();
    *cbcr &= ~1u; dsb();
    if (!c.vdig.keep) rpmh.ldo(c.vdig.name, 0, false);
    if (c.vana.name && !c.vana.keep) rpmh.ldo(c.vana.name, 0, false);
    if (!c.vio.keep) rpmh.ldo(c.vio.name, 0, false);
    return count;
}

void cameraThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "houji-camera");
    unsigned delay = 70; PE_parse_boot_argn("houji_camera_delay", &delay, sizeof delay);
    if (!cs[0]) IOSleep(delay * 1000);   // the first run waits for the boot; console reruns start at once
    for (unsigned i = 1; i < 10; ++i) cs[i] = 0;
    Rpmh rpmh = {};
    if ((!gTlmm.va && !mapWindow(gTlmm, kTlmm, 0x100000)) || (!gGcc.va && !mapWindow(gGcc, kGcc, 0x80000)) || !rpmh.open()) {cs[0] = -1; return;}
    cs[0] = 1;
    // camcc's rail MMCX is already up for the display (dispcc shares it); a vote from this DRV would replace the firmware's, so none here.
    static const uint32_t branches[] = {0x26004u, 0x26028u};
    for (uint32_t off : branches) {gGcc.va[off / 4] |= 1u; dsb(); waitBits(&gGcc.va[off / 4], 1u << 31, false, 1000);}
    if (!gCamcc.va && !mapWindow(gCamcc, kCamcc, 0x20000)) {cs[0] = -2; rpmh.close(); return;}
    cs[0] = 2;
    // BOB2: PM8010's L3/L4 input (sm8650 boards: vdd-l3-l4-supply); every stock sensor lists cam_bob2, 2.704-3.296 V. Left on afterwards:
    // other consumers may share it and this DRV's vote would replace the firmware's.
    if (rpmh.ldo("bobb2", 3296, true, 6)) cs[1] = 1;
    IOSleep(2);
    // Titan top GDSC (camcc 0x132bc, the stock cam_clk supply): clear SW_COLLAPSE (bit 0), keep flip-flop retention (bit 11), then poll
    // POWER_UP_COMPLETE (bit 16 of the CFG_GDSCR at +4) as Linux does for POLL_CFG_GDSCR
    volatile uint32_t *gdsc = gCamcc.va + 0x132bc / 4;
    *gdsc = (*gdsc & ~1u) | (1u << 11); dsb();
    const bool gdscOn = waitBits(&gdsc[1], 1u << 16, true, 5000);
    cs[8] = (int32_t)((gdsc[0] & 0xfffff000u) | (gdscOn ? 1u : 0u));
    cs[0] = 3;
    unsigned total = 0;
    const int only = gOnly;
    for (unsigned cell = 0; cell < 4; ++cell) {
        if (only >= 0 && (unsigned)only != cell) continue;
        if (runCamera(rpmh, cell, total)) cs[9] = cs[9] | (1 << cell);
    }
    cs[3] = (int32_t)total;
    cs[0] = 6;
    *gdsc |= 1u; dsb();
    rpmh.close();
    cs[0] = 7;
}
}  // namespace

extern "C" void houji_camera_start(void) {
    unsigned stage = 0; PE_parse_boot_argn("houji_camera", &stage, sizeof stage);
    if (!stage || native_camera_stats[0]) return;
    thread_t t = nullptr;
    if (kernel_thread_start(cameraThread, nullptr, &t) == KERN_SUCCESS) thread_deallocate(t);
}
extern "C" void houji_camera_report(void) {
    const volatile int32_t *s = native_camera_stats;
    if (s[0]) IOLog("HOUJI CAMERA STATE: stage=%d rails=%x mclk=%08x devices=%d %03x %03x %03x %03x gdsc=%08x cells=%x\n", s[0], s[1],
                    (unsigned)s[2], s[3], s[4], s[5], s[6], s[7], (unsigned)s[8], s[9]);
}
// USB console "camera probe [CELL]": run the probe again once the previous run has finished (stage 7) or failed
extern "C" void houji_camera_command(const char *what) {
    if (strncmp(what, "probe", 5) || (native_camera_stats[0] > 0 && native_camera_stats[0] != 7)) return;
    gOnly = (what[5] == ' ' && what[6] >= '0' && what[6] <= '3') ? what[6] - '0' : -1;
    if (!native_camera_stats[0]) native_camera_stats[0] = 7;
    thread_t t = nullptr;
    if (kernel_thread_start(cameraThread, nullptr, &t) == KERN_SUCCESS) thread_deallocate(t);
}
