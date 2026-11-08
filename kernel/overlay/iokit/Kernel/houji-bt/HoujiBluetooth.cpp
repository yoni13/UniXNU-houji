// SPDX-License-Identifier: BSD-2-Clause-Patent
// Bluetooth bring-up on houji (Xiaomi 14), stage 1: talk to the WCN7850's Bluetooth ROM over its UART. USB console only:
// "bt probe" (RTS/CTS flow control), "bt probe nocts" (transmit without waiting for CTS). Nothing runs at boot.
// Board facts (the SteamOS houji port, armada/ports/houji/bluetooth-haptics.dtsi and sm8650-xiaomi-houji.dts, where this controller
// pairs): the controller sits on uart14 = QUPv3 wrapper 2 serial engine 6 (0x00898000, wrapper 0x008c0000), pins GPIO 24 CTS,
// 25 RTS, 26 TX, 27 RX (function qup2_se6 = 1, pinctrl-sm8650.c); BT_EN is GPIO 17, beside WLAN_EN 16 on the shared WCN7850 PMU
// whose input rails and RF clock HoujiWifi already turns on. Clocks (gcc-sm8650.c): wrapper 2 core (vote bit 0), core 2x (bit 3),
// m-AHB (bit 2), s-AHB (bit 1) and SE6 (bit 10) in the vote register 0x52010; SE6's RCG at 0x1e760 (parent map: XO 0, GPLL0 main 1,
// GPLL7 2, GPLL0 even 6). GENI programming follows Linux qcom-geni-se.c / qcom_geni_serial.c, polled FIFO mode. The protocol
// (Linux btqca.c): at 115200, EDL version request = H4 command 0xfc00 with the one byte 0x19; the ROM answers with an event.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <string.h>

// [0] stage (negative: the step that failed), [1] QUP hardware version, [2] GENI firmware revision, [3] bytes received last
extern "C" {volatile int32_t native_bt_stats[4];}

namespace {
volatile int32_t *const bs = native_bt_stats;
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

constexpr uint64_t kTlmm = 0xf100000, kGcc = 0x100000, kSe = 0x898000, kWrap = 0x8c0000;
constexpr unsigned kBtEn = 17, kCts = 24, kRts = 25, kTx = 26, kRx = 27;
Window gTlmm, gGcc, gSe, gWrap;
volatile uint32_t &gcc(uint32_t off) {return gGcc.va[off / 4];}
volatile uint32_t &se(uint32_t off) {return gSe.va[off / 4];}

// ---- TLMM: config at pin * 0x1000 (pull 1:0, function 5:2, drive 8:6, output enable 9), io at +4 (bit 0 in, bit 1 out) ----
volatile uint32_t &pinCfg(unsigned pin) {return gTlmm.va[pin * 0x1000 / 4];}
volatile uint32_t &pinIo(unsigned pin) {return gTlmm.va[(pin * 0x1000 + 4) / 4];}
void pinFunc(unsigned pin, unsigned func, unsigned pull) {pinCfg(pin) = (func << 2) | pull; dsb();}   // 2 mA
void pinOut(unsigned pin, bool high) {pinIo(pin) = high ? 2u : 0u; pinCfg(pin) = (1u << 9) | (7u << 6); dsb();}   // GPIO, 16 mA

// ---- GCC ----
bool branchOn(uint32_t halt) {for (unsigned i = 0; i < 200; ++i) {if (!(gcc(halt) >> 31)) return true; IODelay(10);} return false;}
// SE6 RCG to 7.3728 MHz: GPLL0 even (300 MHz) * 384 / 15625, dual-edge M/N (clk-rcg2.c __clk_rcg2_configure_mnd)
bool seClock() {
    constexpr uint32_t rcg = 0x1e760, m = 384, n = 15625;
    gcc(rcg + 8) = m;
    gcc(rcg + 12) = ~(n - m) & 0xffff;
    gcc(rcg + 16) = ~n & 0xffff;          // 2d = n, clamped to [m, 2(n - m)]
    gcc(rcg + 4) = (2u << 12) | (6u << 8) | 1u;   // dual edge, GPLL0 even, divider 1 (hid 2 * 1 - 1)
    dsb();
    gcc(rcg) = gcc(rcg) | 1u;   // update
    dsb();
    for (unsigned i = 0; i < 500; ++i) {if (!(gcc(rcg) & 1u)) return !(gcc(rcg) >> 31); IODelay(10);}
    return false;
}

// ---- GENI (qcom-geni-se.h / qcom_geni_serial.c offsets) ----
enum : uint32_t {
    kForceDefault = 0x20, kOutputCtrl = 0x24, kCgcCtrl = 0x28, kStatus = 0x40, kMClkCfg = 0x48, kSClkCfg = 0x4c, kFwRev = 0x68,
    kClkSel = 0x7c, kLoopback = 0x22c, kByteGran = 0x254, kDmaModeEn = 0x258, kTxTransCfg = 0x25c, kTxPack0 = 0x260, kTxPack1 = 0x264,
    kTxWordLen = 0x268, kTxStopLen = 0x26c, kTxTransLen = 0x270, kRxTransCfg = 0x280, kRxPack0 = 0x284, kRxPack1 = 0x288,
    kRxWordLen = 0x28c, kRxStale = 0x294, kTxParity = 0x2a4, kRxParity = 0x2a8, kManualRfr = 0x2ac,
    kMCmd0 = 0x600, kMCmdCtrl = 0x604, kMIrqStatus = 0x610, kMIrqEn = 0x614, kMIrqClear = 0x618,
    kSCmd0 = 0x630, kSCmdCtrl = 0x634, kSIrqStatus = 0x640, kSIrqEn = 0x644, kSIrqClear = 0x648,
    kTxFifo = 0x700, kRxFifo = 0x780, kTxFifoStatus = 0x800, kRxFifoStatus = 0x804, kTxWatermark = 0x80c, kRxWatermark = 0x810,
    kRxRfrWatermark = 0x814, kIos = 0x908, kSeIrqEn = 0xe1c, kHwParam0 = 0xe24, kHwParam1 = 0xe28,
};
unsigned gTxDepth, gRxDepth;

bool uartSetup(bool cts, unsigned sampling) {
    gTxDepth = (se(kHwParam0) >> 16) & 0x3f;
    gRxDepth = (se(kHwParam1) >> 16) & 0x3f;
    if (gTxDepth < 4 || gRxDepth < 4) return false;
    se(kMIrqClear) = ~0u; se(kSIrqClear) = ~0u;
    se(kCgcCtrl) = se(kCgcCtrl) | 0x7fu;          // geni_se_io_init
    se(kOutputCtrl) = 0x7f;
    se(kForceDefault) = 1;
    se(kDmaModeEn) = se(kDmaModeEn) & ~1u;      // FIFO mode
    se(kSeIrqEn) = (se(kSeIrqEn) | 0xcu) & ~3u;  // GENI M/S on, DMA off
    // 8-bit characters, four per FIFO word, least significant first (geni_se_config_packing(8, 4, false))
    se(kTxPack0) = 0x4380e; se(kTxPack1) = 0xc3e0e;
    se(kRxPack0) = 0x4380e; se(kRxPack1) = 0xc3e0e;
    se(kByteGran) = 0;
    se(kRxWatermark) = 4;
    se(kRxRfrWatermark) = gRxDepth - 2;
    se(kClkSel) = 0;
    const uint32_t div = 7372800u / (115200u * sampling);
    se(kMClkCfg) = (div << 4) | 1u; se(kSClkCfg) = (div << 4) | 1u;
    se(kLoopback) = 0;
    se(kTxTransCfg) = cts ? 0u : 2u;   // bit 1: ignore CTS
    se(kTxParity) = 0; se(kRxTransCfg) = 0; se(kRxParity) = 0;
    se(kTxWordLen) = 8; se(kRxWordLen) = 8; se(kTxStopLen) = 0;
    se(kRxStale) = 16 * 10;
    se(kManualRfr) = 0;                // RFR follows the receive FIFO
    se(kMIrqEn) = se(kMIrqEn) | 1u;    // command done
    dsb();
    se(kSCmd0) = 1u << 27;             // UART_START_READ, runs until cancelled
    dsb();
    return true;
}
bool uartWrite(const uint8_t *p, unsigned n) {
    if (!n || n > gTxDepth * 4) return false;
    se(kMIrqClear) = ~0u;
    se(kTxTransLen) = n;
    se(kMCmd0) = 1u << 27;             // UART_START_TX
    dsb();
    for (unsigned i = 0; i < n; i += 4) {
        uint32_t w = 0;
        for (unsigned k = 0; k < 4 && i + k < n; ++k) w |= (uint32_t)p[i + k] << (8 * k);
        se(kTxFifo) = w;
    }
    dsb();
    for (unsigned i = 0; i < 2000; ++i) {if (se(kMIrqStatus) & 1u) {se(kMIrqClear) = se(kMIrqStatus); return true;} IODelay(50);}
    return false;
}
unsigned uartRead(uint8_t *out, unsigned room, unsigned ms) {
    unsigned got = 0;
    for (unsigned t = 0; t < ms * 10 && got < room; ++t) {
        const uint32_t st = se(kRxFifoStatus);
        const unsigned words = st & 0x1ffffff;
        if (!words) {IODelay(100); continue;}
        for (unsigned w = 0; w < words && got < room; ++w) {
            const uint32_t v = se(kRxFifo);
            unsigned bytes = 4;
            if (w + 1 == words && (st >> 31)) {const unsigned last = (st >> 28) & 7; bytes = last ? last : 4;}
            for (unsigned k = 0; k < bytes && got < room; ++k) out[got++] = (uint8_t)(v >> (8 * k));
        }
        se(kSIrqClear) = se(kSIrqStatus);
    }
    return got;
}
void dump(const char *what, const uint8_t *p, unsigned n) {
    char line[3 * 64 + 1]; unsigned o = 0;
    for (unsigned i = 0; i < n && i < 64; ++i) {static const char h[] = "0123456789abcdef"; line[o++] = h[p[i] >> 4]; line[o++] = h[p[i] & 15]; line[o++] = ' ';}
    line[o] = 0;
    IOLog("HOUJI BT: %s %u bytes: %s\n", what, n, line);
}
bool exchange(const char *what, const uint8_t *cmd, unsigned n, unsigned ms) {
    if (!uartWrite(cmd, n)) {IOLog("HOUJI BT: %s not sent (no TX done; IOS=%x)\n", what, se(kIos)); return false;}
    uint8_t rx[256];
    const unsigned got = uartRead(rx, sizeof rx, ms);
    bs[3] = (int32_t)got;
    dump(what, rx, got);
    return got != 0;
}

volatile uint32_t gBusy;
bool gCts = true;
void btProbe(void *, wait_result_t) {
    bs[0] = 1;
    if (!mapWindow(gTlmm, kTlmm, 0x100000) || !mapWindow(gGcc, kGcc, 0x80000) || !mapWindow(gSe, kSe, 0x4000) ||
        !mapWindow(gWrap, kWrap, 0x2000)) {bs[0] = -1; goto out;}
    // Clocks before any SE access: a gated SE's registers would not answer.
    bs[0] = 2;
    gcc(0x52010) = gcc(0x52010) | (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 10);
    dsb();
    if (!branchOn(0x2328c) || !branchOn(0x23290) || !branchOn(0x23294)) {bs[0] = -2; IOLog("HOUJI BT: wrapper 2 AHB/core clocks stay off\n"); goto out;}
    bs[0] = 3;
    if (!seClock()) {bs[0] = -3; IOLog("HOUJI BT: SE6 RCG update did not complete (cfg %x)\n", gcc(0x1e764)); goto out;}
    if (!branchOn(0x1e754)) {bs[0] = -3; IOLog("HOUJI BT: SE6 branch stays off\n"); goto out;}
    {
        bs[0] = 4;
        const uint32_t hw = gWrap.va[1], fw = se(kFwRev);
        bs[1] = (int32_t)hw; bs[2] = (int32_t)fw;
        const unsigned major = hw >> 28, minor = (hw >> 16) & 0xfff, proto = (fw >> 8) & 0xff;
        IOLog("HOUJI BT: QUP %u.%u, SE6 firmware %x (protocol %u, 2 = UART), status %x, TX/RX FIFO depth %u/%u words\n", major, minor,
              fw, proto, se(kStatus), (se(kHwParam0) >> 16) & 0x3f, (se(kHwParam1) >> 16) & 0x3f);
        if (proto != 2) {bs[0] = -4; IOLog("HOUJI BT: SE6 is not loaded as a UART; stop\n"); goto out;}
        // Pins, then the Bluetooth function of the WCN7850: BT_EN low 50 ms, high, 100 ms to settle (pwrseq-qcom-wcn.c: >= 50 ms)
        bs[0] = 5;
        pinFunc(kCts, 1, 0); pinFunc(kRts, 1, 0); pinFunc(kTx, 1, 0); pinFunc(kRx, 1, 3);
        pinOut(kBtEn, false); IOSleep(50); pinOut(kBtEn, true); IOSleep(100);
        bs[0] = 6;
        if (!uartSetup(gCts, major >= 2 && minor >= 5 ? 16 : 32)) {bs[0] = -6; IOLog("HOUJI BT: bad FIFO depth\n"); goto out;}
        IOLog("HOUJI BT: BT_EN high, UART 115200 8N1 %s, IOS=%x\n", gCts ? "RTS/CTS" : "ignoring CTS", se(kIos));
        uint8_t early[64];
        const unsigned e = uartRead(early, sizeof early, 100);
        if (e) dump("unsolicited", early, e);
        bs[0] = 7;
        static const uint8_t version[] = {0x01, 0x00, 0xfc, 0x01, 0x19};   // EDL_PATCH_CMD_OPCODE, EDL_PATCH_VER_REQ_CMD
        static const uint8_t reset[] = {0x01, 0x03, 0x0c, 0x00};            // HCI_Reset
        const bool v = exchange("version reply", version, sizeof version, 500);
        const bool r = exchange("reset reply", reset, sizeof reset, 500);
        bs[0] = v || r ? 8 : -7;
        IOLog("HOUJI BT: probe %s\n", v || r ? "answered" : "got no answer");
    }
    se(kSCmdCtrl) = 1u << 2;   // cancel the read
out:
    release(gTlmm); release(gGcc); release(gSe); release(gWrap);
    __atomic_store_n(&gBusy, 0u, __ATOMIC_RELEASE);
}
}

// USB console "bt probe [nocts]"
extern "C" void houji_bt_command(const char *what) {
    if (strncmp(what, "probe", 5)) {IOLog("HOUJI BT: commands: bt probe [nocts]\n"); return;}
    uint32_t idle = 0;
    if (!__atomic_compare_exchange_n(&gBusy, &idle, 1u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {IOLog("HOUJI BT: busy\n"); return;}
    gCts = strncmp(what + 5, " nocts", 6) != 0;
    thread_t t = nullptr;
    if (kernel_thread_start(btProbe, nullptr, &t) == KERN_SUCCESS) thread_deallocate(t);
    else __atomic_store_n(&gBusy, 0u, __ATOMIC_RELEASE);
}
