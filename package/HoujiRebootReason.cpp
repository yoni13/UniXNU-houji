// SPDX-License-Identifier: BSD-2-Clause-Patent
// The PMIC reboot-reason cell (SDAM, SID 0 0x7100 + 0x48) set to "bootloader", so the restart that follows stops in fastboot:
// console "reboot bootloader", houji_reset_after_s, and with houji_reboot_bootloader=1 every restart including a panic's.
// From the iOS 16 port of this phone (iokit/Kernel/arm/HoujiUsbPhy.inc), where 00 -> 04 and the stop in fastboot were verified.
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include "BootResetCore.h"

extern "C" int houji_reboot_reason_bootloader(void);
extern "C" int houji_reboot_reason_prepare(int always);
extern "C" void houji_reboot_reason_at_reset(void);

namespace {
volatile uint32_t *spmiCore, *spmiCnfg, *spmiObs, *reasonChn;
br_arbiter arb;
int reasonApid = -1, reasonOwner = -1;
bool reasonAlways;

volatile uint32_t *mapIo(uint64_t pa, uint64_t bytes) {
  IODeviceMemory *m = IODeviceMemory::withRange(pa, bytes);
  IOMemoryMap *map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;   // kept for good
  if (m) m->release();
  return map ? (volatile uint32_t *)map->getVirtualAddress() : nullptr;
}
}

// Maps what the write needs (thread context). always: every restart goes to the bootloader.
int houji_reboot_reason_prepare(int always) {
  if (always) reasonAlways = true;
  if (reasonApid >= 0) return 0;
  if (!spmiCore) {spmiCore = mapIo(0xc400000, 0x3000); spmiCnfg = mapIo(0xc42d000, 0x4000);}
  if (!spmiCore || !spmiCnfg) return 10;
  arb.core = spmiCore; arb.cnfg = spmiCnfg; arb.ee = 0;
  br_found f;
  const int rc = br_find(&arb, &f);
  if (rc) return rc;
  if (!spmiObs) spmiObs = mapIo(0xc440000, ((uint64_t)BR_OBS_APID_STRIDE * f.apids + 0xfff) & ~0xfffull);
  volatile uint32_t *chn = mapIo(0xc500000 + (uint64_t)BR_CHNL_APID_STRIDE * (uint32_t)f.apid, 0x1000);
  if (!spmiObs || !chn) return 11;
  reasonChn = chn - (BR_CHNL_APID_STRIDE * (uint32_t)f.apid) / 4; reasonOwner = f.owner; reasonApid = f.apid;
  return 0;
}
int houji_reboot_reason_bootloader(void) {
  int rc = houji_reboot_reason_prepare(0);
  if (rc) return rc;
  uint8_t before = 0, after = 0;
  rc = br_set_bootloader(spmiObs, reasonChn, &arb, reasonApid, &before, &after);
  IOLog("HOUJI REBOOT: reason cell apid=%d owner=%d %02x -> %02x rc=%d\n", reasonApid, reasonOwner, before, after, rc);
  return rc;
}
// From halt_all_cpus, also in a panic: register access only.
void houji_reboot_reason_at_reset(void) {
  uint8_t before = 0, after = 0;
  if (reasonAlways && reasonApid >= 0) (void)br_set_bootloader(spmiObs, reasonChn, &arb, reasonApid, &before, &after);
}

// Why the PMIC last powered the SoC on or reset it (console "pon", and once at boot): the first 0x40 bytes of the PMK8550 PON
// peripherals PON_PBS (SID 0 0x0800; on PMIC5 Gen3 the PON, warm-reset, power-off and fault reason registers) and PON_HLOS (0x1300),
// read through this EE's observer channel. Read only. Compared between a known restart (console "reboot bootloader") and an unexplained
// one, it tells a PS_HOLD/software reset from a watchdog bite or a PMIC fault.
extern "C" void houji_pon_report(void);
extern "C" void houji_pon_report(void) {
  int rc = houji_reboot_reason_prepare(0);
  if (rc) {IOLog("HOUJI PON: SPMI not ready rc=%d\n", rc); return;}
  const uint32_t apids = spmiCore[BR_FEATURES / 4] & BR_FEATURES_APIDS_MASK;
  static const struct {uint16_t ppid; const char *name;} blocks[] = {{0x008, "PON_PBS 0x0800"}, {0x013, "PON_HLOS 0x1300"}};
  for (const auto &b : blocks) {
    int apid = -1;
    for (uint32_t i = 0; i < apids; ++i) {
      const uint32_t map = spmiCore[(BR_APID_MAP + 4 * i) / 4];
      if (map && ((map >> 8) & 0xfff) == b.ppid) {apid = (int)i; break;}
    }
    if (apid < 0) {IOLog("HOUJI PON: %s not mapped\n", b.name); continue;}
    for (unsigned row = 0; row < 0x40; row += 16) {
      char text[64]; unsigned used = 0; int bad = 0;
      for (unsigned j = row; j < row + 16; ++j) {
        uint8_t v = 0;
        if (br_read8(spmiObs, &arb, apid, (uint8_t)j, &v)) {++bad; v = 0xee;}
        used += (unsigned)snprintf(text + used, sizeof(text) - used, "%02x%s", v, (j & 3) == 3 ? " " : "");
      }
      IOLog("HOUJI PON: %s apid=%d +%02x: %s%s\n", b.name, apid, row, text, bad ? " (read errors)" : "");
    }
  }
}
