// SPDX-License-Identifier: BSD-2-Clause-Patent
// Bluetooth on houji (Xiaomi 14): the WCN7850's Bluetooth controller over its UART, brought up to a running HCI controller.
// USB console: "bt probe [nocts]" (ROM version and HCI_Reset at 115200), "bt init" (the whole bring-up below). Nothing runs at boot.
// Board facts (the SteamOS houji port, armada/ports/houji/bluetooth-haptics.dtsi and sm8650-xiaomi-houji.dts, where this controller
// pairs): uart14 = QUPv3 wrapper 2 serial engine 6 (0x00898000, wrapper 0x008c0000), pins GPIO 24 CTS, 25 RTS, 26 TX, 27 RX
// (function qup2_se6 = 1, pinctrl-sm8650.c); BT_EN is GPIO 17, beside WLAN_EN 16 on the shared WCN7850 PMU whose input rails and RF
// clock HoujiWifi turns on (the controller answers only after that). Clocks (gcc-sm8650.c): wrapper 2 core (vote bit 0), core 2x
// (bit 3), m-AHB (bit 2), s-AHB (bit 1) and SE6 (bit 10) in the vote register 0x52010; SE6's RCG at 0x1e760 (parent map: XO 0, GPLL0
// main 1, GPLL7 2, GPLL0 even 6). GENI programming follows Linux qcom-geni-se.c / qcom_geni_serial.c, polled FIFO mode.
// Bring-up (Linux btqca.c / hci_qca.c, WCN7850): at 115200 the ROM answers the EDL version request (opcode 0xfc00, sub-command 0x19);
// vendor command 0xfc48 {0x11} moves it to 3.2 Mbaud; the rampatch image/hmtbtfw20.tlv and the NVM image/hmtnv20.b<board id> (else
// .bin) of the stock bluetooth partition go down in 243-byte EDL segments (sub-command 0x1e); then logging off (0xfc17 {0x14, 0}),
// HCI_Reset and the build label (sub-command 0x20). NVM changes: tag 2 = this phone's address; tag 27 deep sleep off (this host does
// not run Qualcomm's in-band sleep protocol, and tag 17 leaves it off); tag 17's baud byte 0x11 = 3.2 Mbaud (the stock value).
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLocks.h>
#include <pexpert/pexpert.h>
extern "C" {
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
}
#include <libkern/c++/OSData.h>
#include <kern/thread.h>
#include <string.h>
#include <sys/errno.h>

extern "C" int houji_fw_file(unsigned which, const char *path, uint8_t **data, size_t *size, size_t max);

// [0] stage (negative: the step that failed), [1] QUP hardware version, [2] GENI firmware revision, [3] bytes received last,
// [4] controller ready (1), [5] operating baud, [6] board id, [7] milliseconds the bring-up took
extern "C" {volatile int32_t native_bt_stats[8];}

namespace {
volatile int32_t *const bs = native_bt_stats;
struct Window {IOMemoryMap *map; volatile uint32_t *va;};
bool mapWindow(Window &w, uint64_t base, uint64_t size) {
    if (w.va) return true;
    IODeviceMemory *m = IODeviceMemory::withRange(base, size);
    w.map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (m) m->release();
    w.va = w.map ? reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}
void dsb() {__asm__ volatile("dsb sy" ::: "memory");}
uint64_t nowMs() {uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000000;}

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
// SE6 RCG from GPLL0 even (300 MHz) * m / n, dual-edge M/N (clk-rcg2.c __clk_rcg2_configure_mnd): 384/15625 = 7.3728 MHz (115200 x 16
// x 4), 64/375 = 51.2 MHz (3.2 Mbaud x 16)
bool seClock(uint32_t m, uint32_t n) {
    constexpr uint32_t rcg = 0x1e760;
    gcc(rcg + 8) = m;
    gcc(rcg + 12) = ~(n - m) & 0xffff;
    gcc(rcg + 16) = ~n & 0xffff;          // 2d = n, within [m, 2(n - m)]
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
unsigned gTxDepth, gRxDepth, gSampling = 16;
bool gCts = true;

// UART_START_READ, runs until cancelled; with the RX watermark and RX-last (stale) status enabled, as qcom_geni_serial_start_rx_fifo does
void readStart() {se(kSCmd0) = 1u << 27; se(kSIrqEn) = se(kSIrqEn) | (1u << 26) | (1u << 27); dsb();}
void readStop() {
    if (!(se(kStatus) & (1u << 12))) return;    // S_GENI_CMD_ACTIVE
    se(kSCmdCtrl) = 1u << 2;                    // cancel
    for (unsigned i = 0; i < 200 && (se(kStatus) & (1u << 12)); ++i) IODelay(10);
    if (se(kStatus) & (1u << 12)) {se(kSCmdCtrl) = 1u << 1; for (unsigned i = 0; i < 200 && (se(kStatus) & (1u << 12)); ++i) IODelay(10);}
    se(kSIrqClear) = ~0u;
}
void setDivider(unsigned baudClockDiv) {se(kMClkCfg) = (baudClockDiv << 4) | 1u; se(kSClkCfg) = (baudClockDiv << 4) | 1u; dsb();}
bool uartSetup() {
    gTxDepth = (se(kHwParam0) >> 16) & 0x3f;
    gRxDepth = (se(kHwParam1) >> 16) & 0x3f;
    if (gTxDepth < 4 || gRxDepth < 4) return false;
    readStop();
    se(kMIrqClear) = ~0u; se(kSIrqClear) = ~0u;
    se(kCgcCtrl) = se(kCgcCtrl) | 0x7fu;          // geni_se_io_init
    se(kOutputCtrl) = 0x7f;
    se(kForceDefault) = 1;
    se(kDmaModeEn) = se(kDmaModeEn) & ~1u;      // FIFO mode
    se(kSeIrqEn) = (se(kSeIrqEn) | 0xcu) & ~3u;  // GENI M/S on, DMA off
    // 8-bit characters, least significant first: four per FIFO word to transmit (geni_se_config_packing(8, 4, false)), one per word
    // received (geni_se_config_packing(8, 1, false), as qcom_geni_serial's console does), so that no received byte waits in a partial
    // word for the stale timeout (a reply's last bytes did, until the next packet arrived)
    se(kTxPack0) = 0x4380e; se(kTxPack1) = 0xc3e0e;
    se(kRxPack0) = 0xf; se(kRxPack1) = 0;
    se(kByteGran) = 0;
    se(kRxWatermark) = 4;
    se(kRxRfrWatermark) = gRxDepth - 2;
    se(kClkSel) = 0;
    setDivider(7372800u / (115200u * gSampling));
    se(kLoopback) = 0;
    se(kTxTransCfg) = gCts ? 0u : 2u;   // bit 1: ignore CTS
    se(kTxParity) = 0; se(kRxTransCfg) = 0; se(kRxParity) = 0;
    se(kTxWordLen) = 8; se(kRxWordLen) = 8; se(kTxStopLen) = 0;
    se(kRxStale) = 16 * 10;
    se(kManualRfr) = 0;                // RFR follows the receive FIFO
    se(kMIrqEn) = se(kMIrqEn) | 1u;    // command done
    dsb();
    readStart();
    return true;
}

// ---- receive ring: everything the controller sends, pumped from the RX FIFO ----
uint8_t gRing[16384];
uint32_t gHead, gTail;   // free-running
unsigned gDropped;
void pump() {
    // Clear the receive status first, every time (qcom_geni_serial handle_rx): a stale-timeout status left pending after a transfer
    // that ended on a word boundary kept the next reply's partial last word from being released.
    const uint32_t irq = se(kSIrqStatus);
    if (irq) se(kSIrqClear) = irq;
    const uint32_t st = se(kRxFifoStatus);
    const unsigned words = st & 0x1ffffff;
    for (unsigned w = 0; w < words; ++w) {   // one byte per word
        const uint32_t v = se(kRxFifo);
        if (gHead - gTail >= sizeof gRing) {gDropped++; continue;}
        gRing[gHead++ % sizeof gRing] = (uint8_t)v;
    }
}
unsigned ringCount() {return gHead - gTail;}
uint8_t ringAt(unsigned i) {return gRing[(gTail + i) % sizeof gRing];}

IOLock *gTxLock, *gRxLock, *gUartLock;   // client packets in order; the receive ring; one UART transfer at a time
// Qualcomm in-band sleep (IBS, hci_qca.c): at a packet boundary 0xfd = wake indication (answered 0xfc), 0xfc = wake acknowledgement,
// 0xfe = sleep indication. The controller sends them although the NVM leaves software IBS off.
volatile bool gCtlAsleep;
volatile uint32_t gWakeAcks;
unsigned gIbsIn[3];
bool uartWrite(const uint8_t *p, unsigned n);
bool uartSend(const uint8_t *p, unsigned n) {
    if (gUartLock) IOLockLock(gUartLock);
    const bool ok = uartWrite(p, n);
    if (gUartLock) IOLockUnlock(gUartLock);
    return ok;
}
// Receive while transmitting; at run time the receive thread pumps too, so under its lock
void pumpSafe() {if (gRxLock) {IOLockLock(gRxLock); pump(); IOLockUnlock(gRxLock);} else pump();}
// Transmit n bytes as one GENI transfer, feeding the TX FIFO as it drains (pumping RX meanwhile).
bool uartWrite(const uint8_t *p, unsigned n) {
    if (!n) return true;
    se(kMIrqClear) = ~0u;
    se(kTxTransLen) = n;
    se(kMCmd0) = 1u << 27;             // UART_START_TX
    dsb();
    for (unsigned i = 0; i < n; i += 4) {
        unsigned spins = 0;
        while ((se(kTxFifoStatus) & 0xfffffff) >= gTxDepth - 1) {pumpSafe(); if (++spins > 200000) return false; IODelay(5);}
        uint32_t w = 0;
        for (unsigned k = 0; k < 4 && i + k < n; ++k) w |= (uint32_t)p[i + k] << (8 * k);
        se(kTxFifo) = w;
    }
    dsb();
    for (unsigned i = 0; i < 40000; ++i) {if (se(kMIrqStatus) & 1u) {se(kMIrqClear) = se(kMIrqStatus); return true;} pumpSafe(); IODelay(5);}
    return false;
}

void dump(const char *what, const uint8_t *p, unsigned n) {
    char line[3 * 48 + 1]; unsigned o = 0;
    for (unsigned i = 0; i < n && i < 48; ++i) {static const char h[] = "0123456789abcdef"; line[o++] = h[p[i] >> 4]; line[o++] = h[p[i] & 15]; line[o++] = ' ';}
    line[o] = 0;
    IOLog("HOUJI BT: %s %u bytes: %s\n", what, n, line);
}

// The next HCI event (H4 type 4) within ms milliseconds into ev (its code, length and parameters). Other packets (the controller's debug
// log comes as ACL data on handle 0x2edc until logging is turned off) are skipped whole, by their own lengths; a byte that starts no
// packet type is skipped alone. Returns the event's byte count, 0 on timeout.
unsigned skippedBytes, skippedPackets;
unsigned readEvent(uint8_t *ev, unsigned room, unsigned ms) {
    const uint64_t end = nowMs() + ms;
    for (;;) {
        pump();
        for (;;) {
            const unsigned have = ringCount();
            if (!have) break;
            const uint8_t type = ringAt(0);
            unsigned need;
            if (type == 2 || type == 5) {if (have < 5) break; need = 5 + ((ringAt(3) | ringAt(4) << 8) & (type == 5 ? 0x3fff : 0xffff));}
            else if (type == 3) {if (have < 4) break; need = 4 + ringAt(3);}
            else if (type == 4) {if (have < 3) break; need = 3 + ringAt(2);}
            else {gTail++; skippedBytes++; continue;}
            if (have < need) break;
            if (type != 4) {gTail += need; skippedPackets++; continue;}
            const unsigned len = need - 1;
            gTail++;
            for (unsigned i = 0; i < len; ++i) {const uint8_t b = ringAt(0); gTail++; if (i < room) ev[i] = b;}
            return len < room ? len : room;
        }
        if (nowMs() > end) return 0;
        IODelay(50);
    }
}
// Whatever arrives within ms milliseconds, raw (then discarded)
void rawDump(const char *what, unsigned ms) {
    const uint64_t end = nowMs() + ms;
    while (nowMs() < end) {pump(); IODelay(100);}
    uint8_t b[48]; const unsigned n = ringCount() < sizeof b ? ringCount() : (unsigned)sizeof b;
    for (unsigned i = 0; i < n; ++i) b[i] = ringAt(i);
    IOLog("HOUJI BT: raw %s: %u bytes\n", what, ringCount());
    if (n) dump("raw", b, n);
    gTail = gHead;
}
bool sendCommand(uint16_t opcode, const uint8_t *params, uint8_t plen) {
    uint8_t pkt[4 + 255];
    pkt[0] = 1; pkt[1] = (uint8_t)opcode; pkt[2] = (uint8_t)(opcode >> 8); pkt[3] = plen;
    if (plen) memcpy(pkt + 4, params, plen);
    return uartWrite(pkt, 4u + plen);
}
// A command and its Command Complete (event 0x0e for this opcode); returns the return parameters' length (status first) or -1.
int command(uint16_t opcode, const uint8_t *params, uint8_t plen, uint8_t *ret, unsigned room, unsigned ms = 1000) {
    if (!sendCommand(opcode, params, plen)) {IOLog("HOUJI BT: command %04x not sent (IOS=%x)\n", opcode, se(kIos)); return -1;}
    const uint64_t end = nowMs() + ms;
    uint8_t ev[260];
    while (nowMs() < end) {
        const unsigned n = readEvent(ev, sizeof ev, (unsigned)(end - nowMs()) + 1);
        if (!n) break;
        if (ev[0] == 0x0e && n >= 5 && (ev[3] | ev[4] << 8) == opcode) {
            const unsigned r = n - 5 < room ? n - 5 : room;
            memcpy(ret, ev + 5, r);
            return (int)(n - 5);
        }
        dump("other event", ev, n);
    }
    {
        uint8_t b[24]; const unsigned c = ringCount() < sizeof b ? ringCount() : (unsigned)sizeof b;
        for (unsigned i = 0; i < c; ++i) b[i] = ringAt(i);
        IOLog("HOUJI BT: command %04x: no Command Complete; RX FIFO %x S-IRQ %x GENI %x IOS %x RFR %x, %u bytes buffered\n", opcode,
              se(kRxFifoStatus), se(kSIrqStatus), se(kStatus), se(kIos), se(kManualRfr), ringCount());
        if (c) dump("buffered", b, c);
    }
    return -1;
}
// EDL (opcode 0xfc00) request with one sub-command byte: the reply's EDL data after {status, sub-command} into out
int edl(uint8_t sub, uint8_t *out, unsigned room, unsigned ms = 1000) {
    uint8_t r[260];
    const int n = command(0xfc00, &sub, 1, r, sizeof r, ms);
    if (n < 2 || r[0] != 0 || r[1] != sub) {if (n > 0) dump("EDL reply", r, (unsigned)n); return -1;}
    const unsigned d = (unsigned)n - 2 < room ? (unsigned)n - 2 : room;
    memcpy(out, r + 2, d);
    return n - 2;
}

// ---- bring-up ----
uint8_t gAddr[6];   // public address, most significant byte first
void makeAddress() {
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    OSData *id = chosen ? OSDynamicCast(OSData, chosen->getProperty("unique-chip-id")) : nullptr;
    uint64_t seed = 0;
    if (id && id->getLength() >= sizeof seed) memcpy(&seed, id->getBytesNoCopy(), sizeof seed);
    OSSafeReleaseNULL(chosen);
    if (!seed) seed = 0x686f756a69ull;
    uint64_t h = 0xcbf29ce484222325ull ^ 0x6274;   // "bt": a different address from the Wi-Fi one
    for (unsigned i = 0; i < 8; ++i) h = (h ^ ((seed >> (8 * i)) & 0xff)) * 0x100000001b3ull;
    for (unsigned i = 0; i < 6; ++i) gAddr[i] = (uint8_t)(h >> (8 * i));
    gAddr[0] = (uint8_t)((gAddr[0] & 0xfe) | 0x02);   // locally administered
}

bool hwUp() {
    if (!mapWindow(gTlmm, kTlmm, 0x100000) || !mapWindow(gGcc, kGcc, 0x80000) || !mapWindow(gSe, kSe, 0x4000) ||
        !mapWindow(gWrap, kWrap, 0x2000)) {bs[0] = -1; return false;}
    bs[0] = 2;
    gcc(0x52010) = gcc(0x52010) | (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 10);
    dsb();
    if (!branchOn(0x2328c) || !branchOn(0x23290) || !branchOn(0x23294)) {bs[0] = -2; IOLog("HOUJI BT: wrapper 2 AHB/core clocks stay off\n"); return false;}
    bs[0] = 3;
    if (!seClock(384, 15625) || !branchOn(0x1e754)) {bs[0] = -3; IOLog("HOUJI BT: SE6 clock did not start (cfg %x)\n", gcc(0x1e764)); return false;}
    bs[0] = 4;
    const uint32_t hw = gWrap.va[1], fw = se(kFwRev);
    bs[1] = (int32_t)hw; bs[2] = (int32_t)fw;
    const unsigned major = hw >> 28, minor = (hw >> 16) & 0xfff, proto = (fw >> 8) & 0xff;
    gSampling = major >= 2 && minor >= 5 ? 16 : 32;
    IOLog("HOUJI BT: QUP %u.%u, SE6 firmware %x (protocol %u, 2 = UART), TX/RX FIFO depth %u/%u words\n", major, minor, fw, proto,
          (se(kHwParam0) >> 16) & 0x3f, (se(kHwParam1) >> 16) & 0x3f);
    if (proto != 2) {bs[0] = -4; IOLog("HOUJI BT: SE6 is not loaded as a UART\n"); return false;}
    bs[0] = 5;
    pinFunc(kCts, 1, 0); pinFunc(kRts, 1, 0); pinFunc(kTx, 1, 0); pinFunc(kRx, 1, 3);
    // The Bluetooth function of the WCN7850: BT_EN low 50 ms, high, 100 ms to settle (pwrseq-qcom-wcn.c: >= 50 ms)
    pinOut(kBtEn, false); IOSleep(50); pinOut(kBtEn, true); IOSleep(100);
    bs[0] = 6;
    if (!uartSetup()) {bs[0] = -6; IOLog("HOUJI BT: bad FIFO depth\n"); return false;}
    gHead = gTail = 0;
    return true;
}

// Vendor 0xfc48 {0x11} at 115200, then this side to 3.2 Mbaud. While it changes, RFR is held off so that its answer at the old rate is
// not received half-switched; the controller's first event at the new rate is dropped (hci_qca.c qca_set_speed).
bool toFast() {
    se(kManualRfr) = (1u << 31) | (1u << 1);   // manual, not ready
    dsb();
    static const uint8_t baud[] = {0x01, 0x48, 0xfc, 0x01, 0x11};
    if (!uartWrite(baud, sizeof baud)) {IOLog("HOUJI BT: baud command not sent\n"); return false;}
    IOSleep(10);
    readStop();
    if (!seClock(64, 375) || !branchOn(0x1e754)) {IOLog("HOUJI BT: 51.2 MHz SE clock failed\n"); return false;}
    setDivider(51200000u / (3200000u * gSampling));
    gHead = gTail = 0;
    readStart();
    se(kManualRfr) = 0;
    dsb();
    uint8_t ev[64];
    const unsigned n = readEvent(ev, sizeof ev, 200);
    if (n) dump("at 3.2M, dropped", ev, n);
    return true;
}

// One TLV file in 243-byte EDL segments. mode: the patch header's download mode (3 = no event for any segment but the last).
bool download(const char *name, const uint8_t *data, size_t size, unsigned mode) {
    uint8_t seg[2 + 243];
    size_t done = 0;
    unsigned segments = 0;
    const uint64_t began = nowMs();
    while (done < size) {
        const unsigned n = size - done > 243 ? 243 : (unsigned)(size - done);
        const bool last = done + n == size || n < 243;
        seg[0] = 0x1e; seg[1] = (uint8_t)n;
        memcpy(seg + 2, data + done, n);
        if (mode == 0 || last) {
            uint8_t r[16];
            const int rn = command(0xfc00, seg, (uint8_t)(n + 2), r, sizeof r, 2000);
            if (rn < 2 || r[0] != 0 || r[1] != 0x1e) {IOLog("HOUJI BT: %s segment %u at %zu: reply %d %02x %02x\n", name, segments, done, rn, rn > 0 ? r[0] : 0, rn > 1 ? r[1] : 0); return false;}
        } else if (!sendCommand(0xfc00, seg, (uint8_t)(n + 2))) {IOLog("HOUJI BT: %s segment %u not sent\n", name, segments); return false;}
        done += n; segments++;
    }
    IOLog("HOUJI BT: %s: %zu bytes in %u segments, %llu ms\n", name, size, segments, nowMs() - began);
    return true;
}

bool patchNvm(uint8_t *d, size_t size) {
    if (size < 4) return false;
    uint32_t tl; memcpy(&tl, d, 4);
    size_t off = 4, len = tl >> 8;
    if ((tl & 0xff) == 4) {if (size < 8) return false; memcpy(&tl, d + 4, 4); off = 8; len = tl >> 8;}
    if ((tl & 0xff) != 2 || off + len > size) return false;
    unsigned changed = 0;
    for (size_t i = off; i + 12 <= off + len;) {
        const unsigned tag = d[i] | d[i + 1] << 8, tlen = d[i + 2] | d[i + 3] << 8;
        uint8_t *v = d + i + 12;
        if (i + 12 + tlen > off + len) return false;
        if (tag == 2 && tlen == 6) {for (unsigned k = 0; k < 6; ++k) v[k] = gAddr[5 - k]; changed++;}   // little-endian address
        if (tag == 17 && tlen >= 2) {v[0] &= (uint8_t)~0x80; v[1] = 0x11; changed++;}                   // no in-band sleep, 3.2 Mbaud
        if (tag == 27 && tlen >= 1) {v[0] &= (uint8_t)~0x01; changed++;}                                // no deep sleep
        i += 12 + tlen;
    }
    return changed == 3;
}

volatile uint32_t gBusy;
volatile bool gReady;
bool bringUp() {
    gReady = false;
    const uint64_t began = nowMs();
    if (!hwUp()) return false;
    bs[0] = 7;
    uint8_t v[64];
    int n = edl(0x19, v, sizeof v);   // {length?, product le32, patch le16, rom le16, soc le32}
    if (n < 13) {bs[0] = -7; IOLog("HOUJI BT: no ROM version (IOS=%x); are the Wi-Fi rails up?\n", se(kIos)); return false;}
    uint32_t product, soc; uint16_t patch, rom;
    memcpy(&product, v + 1, 4); memcpy(&patch, v + 5, 2); memcpy(&rom, v + 7, 2); memcpy(&soc, v + 9, 4);
    IOLog("HOUJI BT: ROM product %x patch %x rom %x soc %08x\n", product, patch, rom, soc);
    bs[0] = 8;
    if (!toFast()) {bs[0] = -8; return false;}
    n = edl(0x19, v, sizeof v);
    if (n < 13) {bs[0] = -8; IOLog("HOUJI BT: no answer at 3.2 Mbaud\n"); return false;}
    bs[5] = 3200000;
    bs[0] = 9;
    uint8_t *fw = nullptr; size_t fwSize = 0;
    int rc = houji_fw_file(2, "image/hmtbtfw20.tlv", &fw, &fwSize, 1u << 20);
    if (rc || fwSize < 24) {bs[0] = -9; IOLog("HOUJI BT: image/hmtbtfw20.tlv of the bluetooth partition: error %d\n", rc); return false;}
    const unsigned mode = fw[14];   // tlv_type_patch.download_mode
    const bool ok = download("rampatch hmtbtfw20.tlv", fw, fwSize, mode);
    IOFreeData(fw, fwSize);
    if (!ok) {bs[0] = -9; return false;}
    IOSleep(10);
    bs[0] = 10;
    uint8_t b[8] = {};
    {
        const uint64_t asked = nowMs();
        n = edl(0x23, b, sizeof b, 10000);   // {length, id high, id low}; Linux allows HCI_INIT_TIMEOUT
        IOLog("HOUJI BT: board id reply after %llu ms (%d)\n", nowMs() - asked, n);
        if (n < 0) {
            static const uint8_t version[] = {0x01, 0x01, 0x10, 0x00};   // HCI_Read_Local_Version_Information
            const unsigned before = ringCount();
            uartWrite(version, sizeof version);
            const uint64_t end = nowMs() + 300;
            while (nowMs() < end) {pump(); IODelay(100);}
            uint8_t raw[48]; const unsigned c = ringCount() < sizeof raw ? ringCount() : (unsigned)sizeof raw;
            for (unsigned i = 0; i < c; ++i) raw[i] = ringAt(i);
            IOLog("HOUJI BT: after Read_Local_Version: %u bytes (%u before)\n", ringCount(), before);
            if (c) dump("raw after the nudge", raw, c);
            uint8_t ev[64];
            if (readEvent(ev, sizeof ev, 10) >= 9 && ev[0] == 0x0e && ev[5] == 0 && ev[6] == 0x23) {b[0] = ev[7]; b[1] = ev[8]; b[2] = ev[9]; n = 3;}
            gTail = gHead;
        }
    }
    const unsigned board = n >= 3 ? (unsigned)(b[1] << 8 | b[2]) : 0;
    bs[6] = (int32_t)board;
    char path[40];
    snprintf(path, sizeof path, board ? "image/hmtnv20.b%x" : "image/hmtnv20.bin", board);
    rc = houji_fw_file(2, path, &fw, &fwSize, 1u << 20);
    if (rc == ENOENT && board) {IOLog("HOUJI BT: %s missing, using hmtnv20.bin\n", path); strlcpy(path, "image/hmtnv20.bin", sizeof path); rc = houji_fw_file(2, path, &fw, &fwSize, 1u << 20);}
    if (rc) {bs[0] = -10; IOLog("HOUJI BT: %s: error %d\n", path, rc); return false;}
    IOLog("HOUJI BT: board id %x, NVM %s\n", board, path);
    const bool patched = patchNvm(fw, fwSize);
    const bool nvmOk = patched && download(path, fw, fwSize, 0);
    IOFreeData(fw, fwSize);
    if (!nvmOk) {bs[0] = -10; IOLog("HOUJI BT: NVM %s\n", patched ? "download failed" : "tags 2/17/27 not found"); return false;}
    bs[0] = 11;
    rawDump("after the NVM", 300);
    uint8_t r[260];
    if (command(0x0c03, nullptr, 0, r, sizeof r) < 1) {
        IOLog("HOUJI BT: no HCI_Reset reply at 3.2 Mbaud; trying 115200\n");
        readStop(); seClock(384, 15625); setDivider(7372800u / (115200u * gSampling)); gHead = gTail = 0; readStart();
        if (command(0x0c03, nullptr, 0, r, sizeof r) >= 1) {bs[5] = 115200; IOLog("HOUJI BT: the controller is at 115200 after the NVM\n");}
        else rawDump("at 115200", 300);
    }
    static const uint8_t noLogs[] = {0x14, 0x00};
    if (command(0xfc17, noLogs, sizeof noLogs, r, sizeof r) < 1) IOLog("HOUJI BT: logging off: no reply\n");
    n = command(0x0c03, nullptr, 0, r, sizeof r);
    if (n < 1 || r[0]) {bs[0] = -11; IOLog("HOUJI BT: HCI_Reset after the patch failed\n"); return false;}
    n = edl(0x20, r, sizeof r);   // {length, label}
    if (n > 1) {char label[128]; const unsigned l = r[0] < sizeof label - 1 ? r[0] : sizeof label - 1; memcpy(label, r + 1, l); label[l] = 0;
                for (unsigned i = 0; i < l; ++i) if (label[i] < 32) label[i] = ' ';
                IOLog("HOUJI BT: firmware %s\n", label);}
    n = command(0x1009, nullptr, 0, r, sizeof r);   // HCI_Read_BD_ADDR
    if (n >= 7) IOLog("HOUJI BT: address %02x:%02x:%02x:%02x:%02x:%02x (wanted %02x:%02x:%02x:%02x:%02x:%02x)\n", r[6], r[5], r[4], r[3], r[2], r[1],
                      gAddr[0], gAddr[1], gAddr[2], gAddr[3], gAddr[4], gAddr[5]);
    n = command(0x1001, nullptr, 0, r, sizeof r);   // HCI_Read_Local_Version_Information
    if (n >= 9) IOLog("HOUJI BT: HCI version %u revision %04x LMP %u manufacturer %04x subversion %04x\n", r[1], r[2] | r[3] << 8, r[4],
                      r[5] | r[6] << 8, r[7] | r[8] << 8);
    bs[7] = (int32_t)(nowMs() - began);
    bs[0] = 12; bs[4] = 1;
    gReady = true;
    IOLog("HOUJI BT: controller ready at 3.2 Mbaud in %llu ms (skipped %u packets and %u stray bytes, dropped %u)\n", nowMs() - began,
          skippedPackets, skippedBytes, gDropped);
    return true;
}

// ---- iOS side ----
// bluetoothd (iOS 16.7) leaves at once ("No bluetooth on this device.") unless IOServiceNameMatching("bluetooth") finds a service, from
// which it also takes local-mac-address. With BT_HCI_TRANSPORT=H4:POSIX:com.apple.uart.bluetooth,3200000 in its launchd job its POSIX
// driver connects a stream kernel control of that name (the default, com.apple.uart.sk.bluetooth, wants a Skywalk channel), reads and
// writes termios through getsockopt TIOCGETA / setsockopt TIOCSETA at SYSPROTO_CONTROL, and then sends and receives H4 packets.
kern_ctl_ref gCtl;
uint32_t gUnit;            // connected unit, 0: none
volatile bool gCtlUp;
unsigned gLogged;          // packets logged so far (the first 200 in each direction)
uint8_t gTxAsm[1100]; unsigned gTxHave;   // H4 packet being assembled from the client
unsigned gVendorLogged;
void deliver(const uint8_t *p, unsigned n);
volatile uint64_t gRxLoops, gRxBytes, gTxBytes;

// Length of a complete H4 packet at p (n bytes available), 0 if more bytes are needed, -1 if the type is unknown
int h4Length(const uint8_t *p, unsigned n) {
    if (!n) return 0;
    switch (p[0]) {
    case 1: return n < 4 ? 0 : 4 + p[3];
    case 2: return n < 5 ? 0 : 5 + (p[3] | p[4] << 8);
    case 3: return n < 4 ? 0 : 4 + p[3];
    case 4: return n < 3 ? 0 : 3 + p[2];
    case 5: return n < 5 ? 0 : 5 + ((p[3] | p[4] << 8) & 0x3fff);
    default: return -1;
    }
}
void logPacket(const char *dir, const uint8_t *p, unsigned n) {
    if (gLogged >= 400) return;
    gLogged++;
    if (p[0] == 1 && n >= 4) IOLog("HOUJI BT: %s command %04x len %u\n", dir, p[1] | p[2] << 8, p[3]);
    else if (p[0] == 4 && n >= 3 && p[1] == 0x0e && n >= 7) IOLog("HOUJI BT: %s complete %04x status %02x\n", dir, p[4] | p[5] << 8, p[6]);
    else if (p[0] == 4 && n >= 3 && p[1] == 0x0f && n >= 7) IOLog("HOUJI BT: %s status %04x status %02x\n", dir, p[5] | p[6] << 8, p[3]);
    else if (p[0] == 4 && n >= 3) IOLog("HOUJI BT: %s event %02x len %u\n", dir, p[1], p[2]);
    else IOLog("HOUJI BT: %s packet type %u len %u\n", dir, p[0], n);
}
// Client bytes to the controller, a whole H4 packet at a time
void clientBytes(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (gTxHave < sizeof gTxAsm) gTxAsm[gTxHave++] = p[i];
        const int len = h4Length(gTxAsm, gTxHave);
        if (len < 0) {IOLog("HOUJI BT: client sent packet type %u; resynchronising\n", gTxAsm[0]); gTxHave = 0; continue;}
        if (len && gTxHave >= (unsigned)len) {
            logPacket("host->ctl", gTxAsm, (unsigned)len);
            // Vendor-group commands (OGF 0x3f) are Broadcom's and Apple's: BlueTool's boot script (patchram, baud, sleep and audio
            // set-up) and bluetoothd's own. This controller was booted by the kernel and would refuse them, so they are answered here
            // with success and no return parameters, never sent.
            const uint16_t opcode = (uint16_t)(gTxAsm[1] | gTxAsm[2] << 8);
            // Read/Write Default Erroneous Data Reporting (0x0c5a/0x0c5b): bluetoothd's HCI initialisation requires them (status
            // 628, "HCI initialization failed"); this controller does not list them. Answered here: reporting off, the default.
            // LE_Read_Periodic_Advertiser_List_Size (0x204a) is one of the extended advertising commands; after one, the WCN7850
            // refuses legacy advertising and scanning commands (Command Disallowed, as the specification allows), which bluetoothd
            // uses with extended advertising off. Answered here: list size 0.
            if (gTxAsm[0] == 1 && (opcode == 0x0c5a || opcode == 0x0c5b || opcode == 0x204a)) {
                const bool value = opcode != 0x0c5b;
                const uint8_t done[] = {0x04, 0x0e, (uint8_t)(value ? 5 : 4), 0x01, gTxAsm[1], gTxAsm[2], 0x00, 0x00};
                IOLog("HOUJI BT: command %04x answered here\n", opcode);
                deliver(done, value ? 8u : 7u);
                gTxHave = 0;
                continue;
            }
            if (gTxAsm[0] == 1 && (opcode >> 10) == 0x3f) {
                // Success and 32 bytes of return parameters: bluetoothd's Broadcom parsers read a few fields and assert that they are
                // there (bcm_vsc.c). The first echoes the command's first parameter: multiplexed commands (the LE meta VSC) carry their
                // sub-command there and the reply is matched on it. 0xfd48 creates a multi-advertising instance: its number, 1.
                uint8_t done[7 + 32] = {0x04, 0x0e, 4 + 32, 0x01, gTxAsm[1], gTxAsm[2], 0x00};
                if (gTxAsm[3]) done[7] = gTxAsm[4];
                if (opcode == 0xfd48) done[7] = 1;
                if (gVendorLogged++ < 2000) IOLog("HOUJI BT: vendor command %04x (%u bytes) answered here\n", opcode, gTxAsm[3]);
                deliver(done, sizeof done);
                gTxHave = 0;
                continue;
            }
            IOLockLock(gTxLock);
            gTxBytes += (unsigned)len;
            if (gCtlAsleep) {   // wake it: 0xfd, then wait for its 0xfc (the receive thread sees it), up to three tries
                bool awake = false;
                for (unsigned tries = 0; tries < 3 && !awake; ++tries) {
                    const uint32_t before = gWakeAcks;
                    static const uint8_t wake[] = {0xfd};
                    uartSend(wake, 1);
                    for (unsigned ms = 0; ms < 100 && !awake; ++ms) {IOSleep(1); awake = gWakeAcks != before;}
                }
                if (!awake) IOLog("HOUJI BT: controller did not acknowledge the wake indication\n");
                gCtlAsleep = false;
            }
            if (!uartSend(gTxAsm, (unsigned)len)) IOLog("HOUJI BT: UART write of %d bytes timed out\n", len);
            IOLockUnlock(gTxLock);
            gTxHave = 0;
        }
    }
}
// termios as the client sees it (struct termios, LP64): raw, 8 data bits, RTS/CTS, 3.2 Mbaud
struct Termios {uint64_t iflag, oflag, cflag, lflag; uint8_t cc[20]; uint32_t pad; uint64_t ispeed, ospeed;};
static_assert(sizeof(Termios) == 72, "struct termios");
Termios gTermios = {0, 0, 0x300 | 0x800 | 0x8000 | 0x10000 | 0x20000, 0, {}, 0, 3200000, 3200000};
constexpr int kTiocGeta = 0x40487413, kTiocSeta = 0x80487414;
errno_t ctlConnect(kern_ctl_ref, struct sockaddr_ctl *sac, void **) {
    if (!gReady) return ENXIO;
    // The newest client owns the channel: BlueTool stays connected after its boot script, and bluetoothd connects next (refusing it
    // made its transport open fail, fatal error 1201). The previous client's later sends are dropped.
    if (gUnit) IOLog("HOUJI BT: unit %u takes over from unit %u\n", sac->sc_unit, gUnit);
    gUnit = sac->sc_unit; gTxHave = 0;
    IOLog("HOUJI BT: client connected (unit %u)\n", gUnit);
    return 0;
}
errno_t ctlDisconnect(kern_ctl_ref, u_int32_t unit, void *) {
    if (unit == gUnit) {gUnit = 0; IOLog("HOUJI BT: client disconnected\n");}
    return 0;
}
errno_t ctlSend(kern_ctl_ref, u_int32_t unit, void *, mbuf_t m, int) {
    if (unit != gUnit) {mbuf_freem(m); return ENOTCONN;}
    for (mbuf_t q = m; q; q = mbuf_next(q)) clientBytes(static_cast<const uint8_t *>(mbuf_data(q)), mbuf_len(q));
    mbuf_freem(m);
    return 0;
}
errno_t ctlSetopt(kern_ctl_ref, u_int32_t, void *, int opt, void *data, size_t len) {
    if (opt == kTiocSeta && len >= sizeof(Termios)) {
        const Termios *t = static_cast<const Termios *>(data);
        IOLog("HOUJI BT: client TIOCSETA cflag %llx speed %llu\n", t->cflag, t->ospeed);
        gTermios.cflag = t->cflag;
        return 0;
    }
    IOLog("HOUJI BT: client setsockopt %x (%zu bytes) accepted\n", opt, len);
    return 0;
}
errno_t ctlGetopt(kern_ctl_ref, u_int32_t, void *, int opt, void *data, size_t *len) {
    if (opt == kTiocGeta && data && *len >= sizeof(Termios)) {memcpy(data, &gTermios, sizeof gTermios); *len = sizeof gTermios; return 0;}
    IOLog("HOUJI BT: client getsockopt %x (%zu bytes) unsupported\n", opt, len ? *len : 0);
    return ENOPROTOOPT;
}
// Controller bytes to the client
void deliver(const uint8_t *p, unsigned n) {
    if (!gUnit || !n) return;   // no client: dropped
    for (unsigned tries = 0; tries < 200; ++tries) {
        const errno_t e = ctl_enqueuedata(gCtl, gUnit, const_cast<uint8_t *>(p), n, 0);
        if (e != ENOBUFS) {if (e) IOLog("HOUJI BT: delivering %u bytes: error %d\n", n, e); break;}
        IOSleep(1);
    }
}
void rxThread(void *, wait_result_t) {
    uint8_t chunk[512], pkt[1100]; unsigned have = 0;
    for (;;) {
        unsigned n = 0;
        gRxLoops++;
        IOLockLock(gRxLock);
        pump();
        while (ringCount() && n < sizeof chunk) {chunk[n++] = ringAt(0); gTail++;}
        IOLockUnlock(gRxLock);
        if (!n) {IOSleep(1); continue;}
        gRxBytes += n;
        for (unsigned i = 0; i < n; ++i) {
            const uint8_t b = chunk[i];
            if (!have && (b == 0xfd || b == 0xfc || b == 0xfe)) {   // IBS, between packets only
                if (b == 0xfd) {static const uint8_t ack[] = {0xfc}; uartSend(ack, 1); gCtlAsleep = false; gIbsIn[0]++;}
                else if (b == 0xfc) {gCtlAsleep = false; gWakeAcks++; gIbsIn[1]++;}
                else {gCtlAsleep = true; gIbsIn[2]++;}
                continue;
            }
            if (have < sizeof pkt) pkt[have++] = b;
            const int len = h4Length(pkt, have);
            if (len < 0) {IOLog("HOUJI BT: stray byte %02x from the controller\n", pkt[0]); have = 0; continue;}
            if (len && have >= (unsigned)len) {
                // Read_Local_Supported_Commands: add the commands the bridge answers itself (octet 18 bits 2, 3: 0x0c5a, 0x0c5b)
                if (pkt[0] == 4 && pkt[1] == 0x0e && len >= 7 + 64 && pkt[4] == 0x02 && pkt[5] == 0x10 && pkt[6] == 0) pkt[7 + 18] |= 0x0c;
                logPacket("ctl->host", pkt, (unsigned)len); deliver(pkt, (unsigned)len); have = 0;
            }
        }
    }
}
// MobileGestalt's WifiVendor (which BlueTool asks for before anything else, and waits for) is the vendor-id string of the IOService of
// class AppleOLYHAL, Apple's wireless module HAL; BlueTool compares it with module makers' names (Murata among them).
void publishWifiHal();
bool publishIos() {
    publishWifiHal();
    // The service bluetoothd matches, also placed in the device tree under /arm-io as on Apple hardware
    IOService *bt = OSTypeAlloc(IOService);
    if (!bt || !bt->init()) {OSSafeReleaseNULL(bt); return false;}
    static const char name[] = "bluetooth", compatible[] = "bluetooth,wcn7850", type[] = "bluetooth";
    const uint32_t encoding = 1, speed = 3200000;   // H4
    bt->setName(name);
    bt->setProperty("name", (void *)name, sizeof name);
    bt->setProperty("compatible", (void *)compatible, sizeof compatible);
    bt->setProperty("device_type", (void *)type, sizeof type);
    bt->setProperty("local-mac-address", gAddr, sizeof gAddr);
    bt->setProperty("transport-encoding", (void *)&encoding, sizeof encoding);
    bt->setProperty("transport-speed", (void *)&speed, sizeof speed);
    if (IORegistryEntry *armio = IORegistryEntry::fromPath("/arm-io", gIODTPlane)) {bt->attachToParent(armio, gIODTPlane); armio->release();}
    // MobileGestalt's BluetoothAddress comes from /chosen, as on Apple hardware (beside mac-address-wifi0)
    if (IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane)) {
        if (!chosen->getProperty("mac-address-bluetooth0")) chosen->setProperty("mac-address-bluetooth0", gAddr, sizeof gAddr);
        chosen->release();
    }
    const bool attached = bt->attach(IOService::getPlatform());
    if (attached) bt->registerService();
    bt->release();
    // The H4 channel
    kern_ctl_reg reg = {};
    strlcpy(reg.ctl_name, "com.apple.uart.bluetooth", sizeof reg.ctl_name);
    reg.ctl_flags = CTL_FLAG_REG_SOCK_STREAM;
    reg.ctl_sendsize = 64 * 1024; reg.ctl_recvsize = 64 * 1024;
    reg.ctl_connect = ctlConnect; reg.ctl_disconnect = ctlDisconnect; reg.ctl_send = ctlSend;
    reg.ctl_setopt = ctlSetopt; reg.ctl_getopt = ctlGetopt;
    const errno_t e = ctl_register(&reg, &gCtl);
    IOLog("HOUJI BT: service bluetooth %s, address %02x:%02x:%02x:%02x:%02x:%02x; control com.apple.uart.bluetooth: %d\n",
          attached ? "registered" : "not attached", gAddr[0], gAddr[1], gAddr[2], gAddr[3], gAddr[4], gAddr[5], e);
    return attached && !e;
}
void startIos() {
    if (gCtlUp) return;
    gTxLock = IOLockAlloc(); gRxLock = IOLockAlloc(); gUartLock = IOLockAlloc();
    if (!gTxLock || !gRxLock || !gUartLock || !publishIos()) return;
    gCtlUp = true;
    thread_t t = nullptr;
    if (kernel_thread_start(rxThread, nullptr, &t) == KERN_SUCCESS) thread_deallocate(t);
}
// houji_bt=1 at boot: bring the controller up once the Wi-Fi driver has powered the shared rails (it answers only then), then publish
void bootThread(void *, wait_result_t) {
    makeAddress();
    IOSleep(20000);
    for (unsigned attempt = 0; attempt < 32 && !gReady; ++attempt) {
        if (bringUp()) break;
        IOSleep(5000);
    }
    if (gReady) startIos();
    else IOLog("HOUJI BT: controller did not come up; Bluetooth stays off\n");
    __atomic_store_n(&gBusy, 0u, __ATOMIC_RELEASE);
    thread_terminate(current_thread());
}

bool gInit;
void btThread(void *, wait_result_t) {
    makeAddress();
    if (gInit) bringUp();
    else if (hwUp()) {
        uint8_t v[64];
        const int n = edl(0x19, v, sizeof v);
        if (n > 0) dump("ROM version", v, (unsigned)n);
        uint8_t r[16];
        const int rn = command(0x0c03, nullptr, 0, r, sizeof r);
        IOLog("HOUJI BT: probe %s\n", n > 0 && rn > 0 ? "answered" : "got no answer");
    }
    __atomic_store_n(&gBusy, 0u, __ATOMIC_RELEASE);
}
}

// USB console "bt probe [nocts]" | "bt init"
extern "C" void houji_bt_command(const char *what) {
    if (gCtlUp) {
        if (!strncmp(what, "stat", 4))
            IOLog("HOUJI BT: stat rx-loops %llu rx-bytes %llu tx-bytes %llu ring %u/%u dropped %u; RX FIFO %x S-IRQ %x M-IRQ %x GENI %x IOS %x RFR %x"
                  " TX FIFO %x client %u; IBS in wake/ack/sleep %u/%u/%u asleep %d\n", gRxLoops, gRxBytes, gTxBytes, gHead, gTail, gDropped,
                  se(kRxFifoStatus), se(kSIrqStatus), se(kMIrqStatus), se(kStatus), se(kIos), se(kManualRfr), se(kTxFifoStatus), gUnit,
                  gIbsIn[0], gIbsIn[1], gIbsIn[2], (int)gCtlAsleep);
        if (!strncmp(what, "stat", 4)) {
            uint8_t last[48]; const unsigned n = gHead < sizeof last ? gHead : (unsigned)sizeof last;
            for (unsigned i = 0; i < n; ++i) last[i] = gRing[(gHead - n + i) % sizeof gRing];
            dump("last received", last, n);
        }
        else IOLog("HOUJI BT: running for iOS; only \"bt stat\"\n");
        return;
    }
    const bool probe = !strncmp(what, "probe", 5), init = !strncmp(what, "init", 4);
    if (!probe && !init) {IOLog("HOUJI BT: commands: bt probe [nocts] | bt init\n"); return;}
    uint32_t idle = 0;
    if (!__atomic_compare_exchange_n(&gBusy, &idle, 1u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {IOLog("HOUJI BT: busy\n"); return;}
    gInit = init;
    gCts = !(probe && !strncmp(what + 5, " nocts", 6));
    thread_t t = nullptr;
    if (kernel_thread_start(btThread, nullptr, &t) == KERN_SUCCESS) thread_deallocate(t);
    else __atomic_store_n(&gBusy, 0u, __ATOMIC_RELEASE);
}

// Boot: houji_bt=1 runs the bring-up and publishes Bluetooth to iOS (off by default)
extern "C" void houji_bt_start(void) {
    unsigned on = 0;
    PE_parse_boot_argn("houji_bt", &on, sizeof on);
    if (!on) return;
    uint32_t idle = 0;
    if (!__atomic_compare_exchange_n(&gBusy, &idle, 1u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    thread_t t = nullptr;
    if (kernel_thread_start(bootThread, nullptr, &t) == KERN_SUCCESS) thread_deallocate(t);
    else __atomic_store_n(&gBusy, 0u, __ATOMIC_RELEASE);
}

// The class MobileGestalt matches by name (IOServiceMatching("AppleOLYHAL")); nothing else here.
class AppleOLYHAL : public IOService {
    OSDeclareDefaultStructors(AppleOLYHAL);
};
OSDefineMetaClassAndStructors(AppleOLYHAL, IOService);
namespace {
void publishWifiHal() {
    AppleOLYHAL *hal = OSTypeAlloc(AppleOLYHAL);
    if (!hal || !hal->init()) {OSSafeReleaseNULL(hal); return;}
    hal->setProperty("vendor-id", "Murata");
    const bool attached = hal->attach(IOService::getPlatform());
    if (attached) hal->registerService();
    hal->release();
    IOLog("HOUJI BT: AppleOLYHAL %s (vendor-id Murata, for MobileGestalt WifiVendor)\n", attached ? "registered" : "not attached");
}
}
