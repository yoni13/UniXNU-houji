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
// The PMK8550 real-time clock (PMIC SID 0, peripheral 0x61, as Linux rtc-pm8xxx's pmk8350 layout: control 0x46 bit 7 enabled, count
// 0x48..0x4b little-endian seconds). Read only, twice so a carry between the byte reads shows. 0 on success.
extern "C" int houji_rtc_read(uint32_t *seconds, uint8_t *control);
extern "C" int houji_rtc_read(uint32_t *seconds, uint8_t *control) {
  int rc = houji_reboot_reason_prepare(0);
  if (rc) return 100 + rc;
  const uint32_t apids = spmiCore[BR_FEATURES / 4] & BR_FEATURES_APIDS_MASK;
  int apid = -1;
  for (uint32_t i = 0; i < apids; ++i) {
    const uint32_t map = spmiCore[(BR_APID_MAP + 4 * i) / 4];
    if (map && ((map >> 8) & 0xfff) == 0x061) {apid = (int)i; break;}
  }
  if (apid < 0) return 200;
  if (br_read8(spmiObs, &arb, apid, 0x46, control)) return 201;
  uint32_t value[2] = {0, 0};
  for (int pass = 0; pass < 2; ++pass) {
    for (int b = 0; b < 4; ++b) {
      uint8_t v = 0;
      if (br_read8(spmiObs, &arb, apid, (uint8_t)(0x48 + b), &v)) return 202;
      value[pass] |= (uint32_t)v << (8 * b);
    }
  }
  // A carry between bytes: the second read is consistent unless it also straddled one, which the difference shows.
  *seconds = value[1] - value[0] <= 1 ? value[1] : 0;
  return *seconds ? 0 : 203;
}
// The SDAM cell Linux keeps the RTC's offset in (sm8650/x1e devicetrees: pmk8550 sdam_2 at 0x7100, rtc-offset@bc, 4 bytes): if the stock
// firmware fills it, raw + offset is the time. Read only, through the SDAM channel the reboot-reason write already uses.
extern "C" int houji_rtc_offset_read(uint32_t *offset);
extern "C" int houji_rtc_offset_read(uint32_t *offset) {
  int rc = houji_reboot_reason_prepare(0);
  if (rc) return 100 + rc;
  if (reasonApid < 0) return 200;
  uint32_t v = 0;
  for (int b = 0; b < 4; ++b) {
    uint8_t x = 0;
    if (br_read8(spmiObs, &arb, reasonApid, (uint8_t)(0xbc + b), &x)) return 201;
    v |= (uint32_t)x << (8 * b);
  }
  *offset = v;
  return 0;
}
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
