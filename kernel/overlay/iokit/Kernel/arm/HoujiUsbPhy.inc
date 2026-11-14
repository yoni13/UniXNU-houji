/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/*
 * USB 2 PHY bring-up for the debug console after a power-on boot.
 *
 * After "fastboot boot" the bootloader's fastboot has left the USB PHYs running and the console driver only restarts the controller.
 * After a normal power-on nobody has touched USB: the controller's soft reset (DCTL.CSFTRST) never completes, because it needs the
 * PHY clocks. The console driver calls houji_usb_phy_start() when that reset times out. The sequence is the one Linux v6.12 runs
 * for this SoC (sm8650.dtsi usb_1_hsphy, pm8550b eusb2 repeater, dwc3 "qcom,select-utmi-as-pipe-clk"):
 *   1. regulators: repeater vdd18 (ldob15, 1.8 V) and vdd3 (ldob5, 3.104 V) with their voltage, PHY vdd (ldoi1) and vdda12 (ldoi3)
 *      enable only (shared rails, their voltage is left to the other users); values from the stock kernel's regulator summary,
 *   2. PM8550B eUSB2 repeater (SPMI SID 7, 0xfd00): enable, tuning, device mode    (phy-qcom-eusb2-repeater.c),
 *   3. the 38.4 MHz reference (TCSR_USB2_CLKREF_EN) and a GCC_QUSB2PHY_PRIM_BCR pulse,
 *   4. the eUSB2 PHY at 0x88e3000                                                  (phy-qcom-snps-eusb2.c),
 *   5. UTMI clock as the controller's PIPE clock, as the USB 3 PHY stays off       (dwc3-qcom.c dwc3_qcom_select_utmi_clk).
 * houji_usb_phy_state() prints the registers involved; houji_usb_phy_cold() (boot argument houji_usb_cold=1, a test) puts a
 * fastboot-booted phone into the power-on state so the bring-up can be exercised without installing an image.
 */
#include <stdarg.h>
#include "../houji-buttons/BootResetCore.h"

extern "C" void houji_usb_note(const char *line);
extern "C" void houji_usb_phy_state(const char *when);
extern "C" int houji_usb_phy_start(void);
extern "C" void houji_usb_phy_cold(void);

namespace HoujiUsbPhy {
static volatile uint32_t *phy, *gccQusb, *gccUsb, *gccPhy, *tcsr, *qscratch, *spmiCore, *spmiCnfg, *spmiObs, *spmiChn;
static br_arbiter arb;
static int repeater = -1, repeaterOwner = -1;
static constexpr uint32_t kRepeaterPpid = 0x7fd;
static constexpr unsigned kTcsrUsb2 = 0x118 / 4, kTcsrUsb3 = 0x108 / 4;   // TCSR 0x1fc0000 + 0x31118 / 0x31108
static constexpr unsigned kQsGeneral = 0x808 / 4, kQsHs = 0x810 / 4, kQsSs = 0x830 / 4;   // QSCRATCH 0xa6f8800

static volatile uint32_t *mapIo(uint64_t pa, uint64_t bytes) {
  IODeviceMemory *m = IODeviceMemory::withRange(pa, bytes);
  IOMemoryMap *map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;   // kept for good
  if (m) m->release();
  return map ? (volatile uint32_t *)map->getVirtualAddress() : nullptr;
}
static void note(const char *format, ...) __printflike(1, 2);
static void note(const char *format, ...) {
  char line[200];
  va_list args; va_start(args, format); vsnprintf(line, sizeof line, format, args); va_end(args);
  houji_usb_note(line);
}
static bool openAll(void) {
  if (phy && repeater >= 0) return true;
  if (!phy) {
    phy = mapIo(0x88e3000, 0x1000); gccQusb = mapIo(0x112000, 0x1000); gccUsb = mapIo(0x139000, 0x1000); gccPhy = mapIo(0x150000, 0x1000);
    tcsr = mapIo(0x1ff1000, 0x1000); qscratch = mapIo(0xa6f8000, 0x1000);
    spmiCore = mapIo(0xc400000, 0x3000); spmiCnfg = mapIo(0xc42d000, 0x4000);
  }
  if (!phy || !gccQusb || !gccUsb || !gccPhy || !tcsr || !qscratch || !spmiCore || !spmiCnfg) return false;
  arb.core = spmiCore; arb.cnfg = spmiCnfg; arb.ee = 0;
  const uint32_t version = spmiCore[0], apids = spmiCore[BR_FEATURES / 4] & BR_FEATURES_APIDS_MASK;
  if (version < BR_VERSION_V7_MIN || version >= BR_VERSION_V8_MIN || !apids || apids > 1024) return true;   // PHY only
  for (uint32_t i = 0; i < apids; i++) {
    const uint32_t map = spmiCore[(BR_APID_MAP + 4 * i) / 4];
    if (!map || ((map >> 8) & 0xfff) != kRepeaterPpid) continue;
    const int owner = (int)(spmiCnfg[(BR_CNFG_OWNER + 4 * i) / 4] & 7u);
    if (repeater < 0 || owner == arb.ee) {repeater = (int)i; repeaterOwner = owner;}
  }
  if (repeater < 0) return true;
  if (!spmiObs) spmiObs = mapIo(0xc440000, ((uint64_t)BR_OBS_APID_STRIDE * apids + 0xfff) & ~0xfffull);
  if (!spmiChn) spmiChn = mapIo(0xc500000 + (uint64_t)BR_CHNL_APID_STRIDE * (uint32_t)repeater, 0x1000);
  if (!spmiObs || !spmiChn) repeater = -1;
  return true;
}
static int rptRead(uint8_t reg) {
  uint8_t v = 0;
  return repeater >= 0 && !br_read8(spmiObs, &arb, repeater, reg, &v) ? v : -1;
}
static int rptWrite(uint8_t reg, uint8_t value) {return repeater >= 0 ? br_write8(spmiChn, 0, reg, value) : -1;}   // spmiChn is this APID's channel
static uint32_t hs(unsigned offset) {return phy[offset / 4];}
static void hsMask(unsigned offset, uint32_t mask, uint32_t value) {
  phy[offset / 4] = (phy[offset / 4] & ~mask) | (value & mask);
  __asm__ volatile("dsb sy" ::: "memory");
  (void)phy[offset / 4];
}
static void setBits(volatile uint32_t *reg, uint32_t clear, uint32_t bits) {
  *reg = (*reg & ~clear) | bits;
  __asm__ volatile("dsb sy" ::: "memory");
  (void)*reg;
}
// One active-only RPMh request (HoujiPower's RSC mapping and lock, TCS 2). count <= 4.
static bool vote(const char *name, const uint32_t *offsets, const uint32_t *values, unsigned count) {
  using namespace HoujiPower;
  uint32_t base = 0, addresses[4];
  if (count > 4 || !loadNames() || !openRsc()) return false;
  for (unsigned i = 0; i < nameCount && !base; i++) if (!strncmp(names[i].id, name, 8)) base = names[i].address;
  if (!base) {note("HOUJI USB PHY: no RPMh resource %s\n", name); return false;}
  for (unsigned i = 0; i < count; i++) addresses[i] = base + offsets[i];
  if (!rpmhLock) rpmhLock = IOLockAlloc();
  IOLockLock(rpmhLock);
  const QcomRscLayout l = qcom_rsc_layout(rsc[0]);
  QcomRpmhWrite w[16];
  // The done flag only appears for a TCS whose bit is set in IRQ_ENABLE (first register of the TCS block); nobody takes the interrupt.
  rsc[kTcsBlock / 4] |= 1u << 2;
  const unsigned n = qcom_rpmh_active_request(&l, 2, addresses, values, count, w, 16);
  for (unsigned i = 0; i < n; i++) {rsc[(kTcsBlock + w[i].offset) / 4] = w[i].value; __asm__ volatile("dsb sy" ::: "memory");}
  bool done = false;
  for (unsigned i = 0; n && i < 100 && !done; i++) {done = (rsc[(kTcsBlock + l.irq_status) / 4] >> 2) & 1; if (!done) IOSleep(1);}
  const unsigned m = qcom_rpmh_finish(&l, 2, w, 16);
  for (unsigned i = 0; i < m; i++) rsc[(kTcsBlock + w[i].offset) / 4] = w[i].value;
  __asm__ volatile("dsb sy" ::: "memory");
  IOLockUnlock(rpmhLock);
  if (!done) note("HOUJI USB PHY: RPMh vote for %s not acknowledged\n", name);
  return done;
}
}  // namespace HoujiUsbPhy

void houji_usb_phy_state(const char *when) {
  using namespace HoujiUsbPhy;
  if (!openAll()) {note("HOUJI USB PHY %s: cannot map the registers\n", when); return;}
  note("HOUJI USB PHY %s: clkref usb2=%x usb3=%x bcr qusb2=%x usb30=%x phy=%x/%x/%x pipe=%08x qs gen=%08x hs=%08x ss=%08x\n", when,
       tcsr[kTcsrUsb2], tcsr[kTcsrUsb3], gccQusb[0], gccUsb[0], gccPhy[0], gccPhy[1], gccPhy[2], gccUsb[0x68 / 4], qscratch[kQsGeneral],
       qscratch[kQsHs], qscratch[kQsSs]);
  note("HOUJI USB PHY %s: hs 3c=%02x 50=%02x 54=%02x 58=%02x 5c=%02x 60=%02x 64=%02x 68=%02x 6c=%02x 70=%02x 74=%02x 78=%02x 7c=%02x "
       "80=%02x 94=%02x 98=%02x b8=%02x\n", when, hs(0x3c), hs(0x50), hs(0x54), hs(0x58), hs(0x5c), hs(0x60), hs(0x64), hs(0x68), hs(0x6c),
       hs(0x70), hs(0x74), hs(0x78), hs(0x7c), hs(0x80), hs(0x94), hs(0x98), hs(0xb8));
  if (repeater < 0) {note("HOUJI USB PHY %s: no eUSB2 repeater APID (arbiter %08x)\n", when, spmiCore[0]); return;}
  note("HOUJI USB PHY %s: repeater apid=%d owner=%d status=%02x en=%02x tune 50..5b=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x "
       "%02x %02x force e8=%02x ed=%02x\n", when, repeater, repeaterOwner, rptRead(0x08), rptRead(0x46), rptRead(0x50), rptRead(0x51),
       rptRead(0x52), rptRead(0x53), rptRead(0x54), rptRead(0x55), rptRead(0x56), rptRead(0x57), rptRead(0x58), rptRead(0x59),
       rptRead(0x5a), rptRead(0x5b), rptRead(0xe8), rptRead(0xed));
}

int houji_usb_phy_start(void) {
  using namespace HoujiUsbPhy;
  if (!openAll()) return 1;
  // 1. Regulators. VRM fields: +0 voltage (mV), +4 enable, +8 mode (7 = HPM).
  {
    const uint32_t full[3] = {0, 8, 4}, enable[1] = {4}, on[1] = {1};
    const uint32_t vdd18[3] = {1800, 7, 1}, vdd3[3] = {3104, 7, 1};
    const bool a = vote("ldoi1", enable, on, 1), b = vote("ldoi3", enable, on, 1), c = vote("ldob15", full, vdd18, 3),
               d = vote("ldob5", full, vdd3, 3);
    note("HOUJI USB PHY: regulator votes ldoi1=%d ldoi3=%d ldob15=%d ldob5=%d\n", a, b, c, d);
    IOSleep(5);
  }
  // 2. Repeater: enable, the stock devicetree's device-mode tuning (qcom,param-override-seq), device mode (no forced 19.2 MHz clock).
  int repeaterOk = -1;
  if (repeater >= 0) {
    static const uint8_t tune[12] = {0x00, 0x0f, 0x04, 0x07, 0x03, 0x01, 0x03, 0x01, 0x02, 0x01, 0x03, 0x02};   // registers 0x50..0x5b
    int rc = rptWrite(0x46, 0x80);
    for (unsigned i = 0; i < 12; i++) rc |= rptWrite((uint8_t)(0x50 + i), tune[i]);
    rc |= rptWrite(0xe8, 0); rc |= rptWrite(0xed, 0);
    int status = -1;
    for (unsigned i = 0; i < 20; i++) {status = rptRead(0x08); if (status >= 0 && (status & 0x80)) break; IOSleep(1);}
    repeaterOk = !rc && status >= 0 && (status & 0x80);
    note("HOUJI USB PHY: repeater write rc=%d status=%02x %s\n", rc, status, repeaterOk ? "ok" : "NOT ready");
  }
  // 3. Reference clock, then the PHY reset.
  setBits(&tcsr[kTcsrUsb2], 0, 1);
  IODelay(200);
  setBits(&gccQusb[0], 0, 1); IODelay(150); setBits(&gccQusb[0], 1, 0); IODelay(150);
  // 4. eUSB2 PHY (qcom_snps_eusb2_hsphy_init, 38.4 MHz reference, default tuning).
  hsMask(0x94, 2, 2);                 // CFG0: CMN_CTRL_OVERRIDE_EN
  hsMask(0x50, 2, 2);                 // UTMI_CTRL5: POR
  hsMask(0x54, 0x09, 0x09);           // HS_PHY_CTRL_COMMON0: PHY_ENABLE | RETENABLEN
  hsMask(0x130, 4, 4);                // APB_ACCESS_CMD: APB_LOGIC_RESET
  hsMask(0x98, 0x40, 0);              // UTMI_PHY_CMN_CTRL0: TESTBURNIN off
  hsMask(0xb8, 1, 1);                 // FSEL_SEL
  hsMask(0x54, 0x70, 0x40);           // FSEL 38.4 MHz
  hsMask(0x5c, 0xff, 0xc8);           // CFG_CTRL_2: PLL feedback divider 7:0
  hsMask(0x60, 0x0f, 0x00);           // CFG_CTRL_3: divider 11:8
  hsMask(0x60, 0xf0, 0x00);           // reference divider
  hsMask(0x58, 0xfe, 0x1 << 1);       // CFG_CTRL_1: PLL CP bias
  hsMask(0x68, 0xfc, 0x8 << 2);       // CFG_CTRL_4: PLL integral
  hsMask(0x68, 0x03, 0x1);            // PLL gmp
  hsMask(0x6c, 0x1f, 0x10);           // CFG_CTRL_5: PLL proportional
  hsMask(0x70, 0x07, 0x0);            // CFG_CTRL_6: PLL VCO
  hsMask(0x6c, 0xc0, 0x1 << 6);       // PLL vref tune
  hsMask(0x64, 0x10, 0x10);           // HS_PHY_CTRL2: VBUS_DET_EXT_SEL
  hsMask(0x7c, 0x07, 0x0);            // CFG_CTRL_9: TX pre-emphasis
  hsMask(0x7c, 0x60, 0x2 << 5);       // TX rise/fall
  hsMask(0x7c, 0x18, 0x1 << 3);       // source impedance
  hsMask(0x78, 0x38, 0x3 << 3);       // CFG_CTRL_8: HS DC level
  hsMask(0x78, 0xc0, 0x0);            // HS crossover
  hsMask(0x64, 0x0c, 0x0c);           // USB2_SUSPEND_N_SEL | USB2_SUSPEND_N
  hsMask(0x3c, 1, 1);                 // UTMI_CTRL0: SLEEPM
  hsMask(0x54, 2, 2);                 // SIDDQ_SEL
  hsMask(0x54, 4, 0);                 // SIDDQ off
  hsMask(0x50, 2, 0);                 // POR released
  hsMask(0x64, 0x08, 0);              // USB2_SUSPEND_N_SEL off
  IOSleep(2);
  // 5. The controller takes the UTMI clock as its PIPE clock.
  setBits(&qscratch[kQsGeneral], 0, 1u << 8);
  IODelay(500);
  setBits(&qscratch[kQsGeneral], 0, (1u << 0) | (1u << 3));
  IODelay(500);
  setBits(&qscratch[kQsGeneral], 1u << 8, 0);
  IODelay(500);
  return repeaterOk == 0 ? 2 : 0;
}

void houji_usb_phy_cold(void) {
  using namespace HoujiUsbPhy;
  if (!openAll()) return;
  houji_usb_phy_state("handoff");
  if (repeater >= 0) (void)rptWrite(0x46, 0);
  setBits(&gccPhy[0], 0, 1); setBits(&gccPhy[1], 0, 1); setBits(&gccPhy[2], 0, 1); setBits(&gccQusb[0], 0, 1);
  setBits(&gccUsb[0], 0, 1);                 // GCC_USB30_PRIM_BCR: controller and QSCRATCH back to their reset values
  IODelay(1000);
  setBits(&gccUsb[0], 1, 0); setBits(&gccQusb[0], 1, 0); setBits(&gccPhy[1], 1, 0); setBits(&gccPhy[2], 1, 0); setBits(&gccPhy[0], 1, 0);
  setBits(&tcsr[kTcsrUsb2], 1, 0);
  IOSleep(20);
  note("HOUJI USB PHY: TEST houji_usb_cold=1: repeater off, PHYs and controller reset, reference off\n");
}
