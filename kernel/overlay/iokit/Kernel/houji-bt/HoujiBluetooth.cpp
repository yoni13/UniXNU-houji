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

void readStart() {se(kSCmd0) = 1u << 27; dsb();}   // UART_START_READ, runs until cancelled
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
    // 8-bit characters, four per FIFO word, least significant first (geni_se_config_packing(8, 4, false))
    se(kTxPack0) = 0x4380e; se(kTxPack1) = 0xc3e0e;
    se(kRxPack0) = 0x4380e; se(kRxPack1) = 0xc3e0e;
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
    const uint32_t st = se(kRxFifoStatus);
    const unsigned words = st & 0x1ffffff;
    for (unsigned w = 0; w < words; ++w) {
        const uint32_t v = se(kRxFifo);
        unsigned bytes = 4;
        if (w + 1 == words && (st >> 31)) {const unsigned last = (st >> 28) & 7; bytes = last ? last : 4;}
        for (unsigned k = 0; k < bytes; ++k) {
            if (gHead - gTail >= sizeof gRing) {gDropped++; continue;}
            gRing[gHead++ % sizeof gRing] = (uint8_t)(v >> (8 * k));
        }
    }
    if (words) se(kSIrqClear) = se(kSIrqStatus);
}
unsigned ringCount() {return gHead - gTail;}
uint8_t ringAt(unsigned i) {return gRing[(gTail + i) % sizeof gRing];}

// Transmit n bytes as one GENI transfer, feeding the TX FIFO as it drains (pumping RX meanwhile).
bool uartWrite(const uint8_t *p, unsigned n) {
    if (!n) return true;
    se(kMIrqClear) = ~0u;
    se(kTxTransLen) = n;
    se(kMCmd0) = 1u << 27;             // UART_START_TX
    dsb();
    for (unsigned i = 0; i < n; i += 4) {
        unsigned spins = 0;
        while ((se(kTxFifoStatus) & 0xfffffff) >= gTxDepth - 1) {pump(); if (++spins > 200000) return false; IODelay(5);}
        uint32_t w = 0;
        for (unsigned k = 0; k < 4 && i + k < n; ++k) w |= (uint32_t)p[i + k] << (8 * k);
        se(kTxFifo) = w;
    }
    dsb();
    for (unsigned i = 0; i < 40000; ++i) {if (se(kMIrqStatus) & 1u) {se(kMIrqClear) = se(kMIrqStatus); return true;} pump(); IODelay(5);}
    return false;
}

void dump(const char *what, const uint8_t *p, unsigned n) {
    char line[3 * 48 + 1]; unsigned o = 0;
    for (unsigned i = 0; i < n && i < 48; ++i) {static const char h[] = "0123456789abcdef"; line[o++] = h[p[i] >> 4]; line[o++] = h[p[i] & 15]; line[o++] = ' ';}
    line[o] = 0;
    IOLog("HOUJI BT: %s %u bytes: %s\n", what, n, line);
}

// The next HCI event (H4 type 4) within ms milliseconds into ev (its code, length and parameters); other bytes before it are skipped and
// counted. Returns the event's byte count, 0 on timeout.
unsigned skippedBytes;
unsigned readEvent(uint8_t *ev, unsigned room, unsigned ms) {
    const uint64_t end = nowMs() + ms;
    for (;;) {
        pump();
        while (ringCount() && ringAt(0) != 0x04) {gTail++; skippedBytes++;}
        if (ringCount() >= 3) {
            const unsigned len = 2u + ringAt(2);
            if (ringCount() >= 1 + len) {
                gTail++;
                for (unsigned i = 0; i < len; ++i) {const uint8_t b = ringAt(0); gTail++; if (i < room) ev[i] = b;}
                return len < room ? len : room;
            }
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
    IOLog("HOUJI BT: command %04x: no Command Complete\n", opcode);
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
            // A wake-up: one byte that is not a packet type starts nothing on the controller (H4 resynchronises), then look again
            static const uint8_t kick[] = {0xfd};
            uartWrite(kick, 1);
            uint8_t ev[64]; const unsigned e = readEvent(ev, sizeof ev, 2000);
            if (e) dump("after a wake byte", ev, e);
            if (e >= 8 && ev[0] == 0x0e && ev[5] == 0 && ev[6] == 0x23) {b[0] = ev[7]; b[1] = ev[8]; b[2] = e > 9 ? ev[9] : 0; n = (int)e - 7;}
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
    IOLog("HOUJI BT: controller ready at 3.2 Mbaud in %llu ms (skipped %u stray bytes, dropped %u)\n", nowMs() - began, skippedBytes, gDropped);
    return true;
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
