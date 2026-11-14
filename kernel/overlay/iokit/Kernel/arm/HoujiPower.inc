/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Power work for system sleep: what is switched down while the boot CPU waits in SweetCPU::bootSleep, and tools to find where the
 * power goes. Included by SweetCPU.cpp after HoujiCpuDvfs.inc.
 *
 * Sleep hooks (a mask; boot argument houji_sleep_off=MASK, default 0 = none; console "pw off MASK"). enter() and leave() run on the
 * boot CPU with interrupts masked and every other CPU off: no logging, no blocking; results are kept and printed after the wake.
 *   1  all four CPU frequency domains to their lowest row for the sleep (the governor thread is frozen where the sleep caught it)
 *   2  the UFS link into HIBERN8 (houji_ufs_hibernate)
 *   4  the CPU L3 cache to its lowest row for the sleep
 * Measurement only (never set these in houji_sleep_off for use):
 *   80   the boot CPU spins instead of waiting in WFI: a known extra load, to check that a reading reflects the sleep
 *   100  GPU off and NOT restored: GX and CX-GMU clock branches off, then both power domains collapsed (GPUCC). The GPU is dead
 *        until the next boot: read the battery at the wake, then reboot.
 *   200  display controller off and NOT restored: both MDSS power domains collapsed (DISPCC). Reboot afterwards.
 * Console:
 *   pw census            which clocks, root generators, PLLs and power domains of GCC, GPUCC and DISPCC are on (reads only)
 *   pw rpmh              the commands held in every TCS of the apps RSC (DRV 2), with the resource names from the command database,
 *                        and the CPU L3 vote (reads only)
 *   pw l3 N              the CPU L3 cache to row N of its table (0: 364 MHz; the boot firmware leaves row 12, 1555 MHz)
 *   pw vote NAME HEX     one active-only RPMh write of HEX to resource NAME's address (+0) through active TCS 2, waited for
 *   pw off MASK          the hooks for every later sleep (hex)
 *   pw sleep N [MASK]    sleep now with these hooks (default: the current mask) and wake fully, without a key press (the display
 *                        stays off), N seconds after the command: for measuring a configuration from the battery's charge counter
 */
#include "HoujiClockCensus.h"
#include "../houji-adsp/QcomRpmh.h"
extern "C" int houji_ufs_hibernate(int enter, uint64_t *status);
extern "C" void houji_sleep_now(void);
namespace HoujiPower {
enum {kDvfsLowest = 1, kUfsHibernate = 2, kL3Lowest = 4, kSpin = 0x80, kGpuKill = 0x100, kMdssKill = 0x200};
static volatile uint32_t *gpucc, *dispcc, *l3vote;
static uint32_t l3Saved;
static bool mapL3(void) {
  if (l3vote) return true;
  IODeviceMemory *d = IODeviceMemory::withRange(0x17d90000ULL, 0x1000);
  IOMemoryMap *k = d ? d->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;   // kept for good
  if (d) d->release();
  l3vote = k ? (volatile uint32_t *)k->getVirtualAddress() : nullptr;
  return l3vote != nullptr;
}
static uint32_t killSeen[4];
static void dsbDelay(unsigned us) {__asm__ volatile("dsb sy" ::: "memory"); IODelay(us);}
// from a thread, before the sleep: the windows the kill switches write
static void mapKill(uint32_t m) {
  auto map = [](uint64_t base, uint32_t size) -> volatile uint32_t * {
    IODeviceMemory *d = IODeviceMemory::withRange(base, size);
    IOMemoryMap *k = d ? d->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (d) d->release();
    return k ? (volatile uint32_t *)k->getVirtualAddress() : nullptr;   // kept for good
  };
  if (m & kL3Lowest) mapL3();
  if ((m & kGpuKill) && !gpucc) gpucc = map(0x3d90000ULL, 0xa000);
  if ((m & kMdssKill) && !dispcc) dispcc = map(0xaf00000ULL, 0x20000);
}
static volatile uint32_t sleepOff;
static volatile uint64_t labUntil;
static bool parsed;
static uint32_t applied, savedRow[HoujiCpuDvfs::domains];
static int ufsEnter, ufsExit;
static uint64_t ufsEnterStatus, ufsExitStatus;
static uint32_t mask(void) {
  if (!parsed) {parsed = true; uint32_t v = 0; if (PE_parse_boot_argn("houji_sleep_off", &v, sizeof v)) sleepOff = v;}
  return sleepOff;
}
static void enter(void) {
  const uint32_t m = mask();
  applied = 0; ufsEnter = ufsExit = 0; ufsEnterStatus = ufsExitStatus = 0;
  if ((m & kDvfsLowest) && HoujiCpuDvfs::started && (HoujiCpuDvfs::enabled & 1)) {
    bool all = true;
    for (unsigned id = 0; id < HoujiCpuDvfs::domains; id++) {
      auto &w = HoujiCpuDvfs::d[id];
      savedRow[id] = w.cur;
      if (!w.va || w.bad || !w.n) {all = false; continue;}
      if (w.cur && !HoujiCpuDvfs::vote(id, 0)) all = false;
    }
    if (all) applied |= kDvfsLowest;
  }
  if (m & kUfsHibernate) {
    ufsEnter = houji_ufs_hibernate(1, &ufsEnterStatus);
    if (!ufsEnter) applied |= kUfsHibernate;
  }
  if (m & kSpin) applied |= kSpin;
  if ((m & kL3Lowest) && l3vote) {
    l3Saved = l3vote[0x90 / 4];
    l3vote[0x90 / 4] = 0; __asm__ volatile("dsb sy" ::: "memory");
    applied |= kL3Lowest;
  }
  if ((m & kGpuKill) && gpucc && !(applied & kGpuKill)) {
    gpucc[0x90a8 / 4] &= ~1u; gpucc[0x913c / 4] &= ~1u; dsbDelay(200);   // gx_gfx3d, cx_gmu branches
    gpucc[0x905c / 4] |= 1u; dsbDelay(500);                                 // GX: SW_COLLAPSE
    gpucc[0x9108 / 4] |= 1u; dsbDelay(2000);                                // CX
    killSeen[0] = gpucc[0x905c / 4]; killSeen[1] = gpucc[0x9108 / 4];
    applied |= kGpuKill;
  }
  if ((m & kMdssKill) && dispcc && !(applied & kMdssKill)) {
    dispcc[0x9000 / 4] |= 1u; dispcc[0xb000 / 4] |= 1u; dsbDelay(2000);
    killSeen[2] = dispcc[0x9000 / 4]; killSeen[3] = dispcc[0xb000 / 4];
    applied |= kMdssKill;
  }
}
static void leave(void) {
  if (applied & kL3Lowest) {l3vote[0x90 / 4] = l3Saved; __asm__ volatile("dsb sy" ::: "memory");}
  if (applied & kUfsHibernate) ufsExit = houji_ufs_hibernate(0, &ufsExitStatus);
  // the governor continues from the row it had chosen
  if (mask() & kDvfsLowest) {
    for (unsigned id = 0; id < HoujiCpuDvfs::domains; id++) {
      auto &w = HoujiCpuDvfs::d[id];
      if (w.va && !w.bad && w.n && savedRow[id] < w.n && savedRow[id] != w.cur) HoujiCpuDvfs::vote(id, savedRow[id]);
    }
  }
}
static void report(void) {
  IOLog("HOUJI POWER: sleep hooks asked %x applied %x; ufs enter %d (%llx) exit %d (%llx); gdsc gx=%08x cx=%08x mdss=%08x int2=%08x\n",
        mask(), applied, ufsEnter, ufsEnterStatus, ufsExit, ufsExitStatus, killSeen[0], killSeen[1], killSeen[2], killSeen[3]);
}

static void censusThread(void *, wait_result_t) {
  static uint32_t busy;
  uint32_t idle = 0;
  if (!__atomic_compare_exchange_n(&busy, &idle, 1u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {thread_terminate(current_thread()); return;}
  constexpr unsigned blocks = sizeof houjiCensusBlocks / sizeof houjiCensusBlocks[0];
  IOMemoryMap *maps[blocks] = {};
  volatile uint32_t *va[blocks] = {};
  for (unsigned b = 0; b < blocks; b++) {
    IODeviceMemory *m = IODeviceMemory::withRange(houjiCensusBlocks[b].base, houjiCensusBlocks[b].size);
    maps[b] = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly) : nullptr;
    if (m) m->release();
    va[b] = maps[b] ? (volatile uint32_t *)maps[b]->getVirtualAddress() : nullptr;
  }
  if (va[0]) {
    // GPUCC and DISPCC answer only while GCC's configuration clock to them runs (gcc_gpu_cfg_ahb 0x71004, gcc_disp_ahb 0x27004)
    const uint32_t gpuAhb = va[0][0x71004 / 4], dispAhb = va[0][0x27004 / 4];
    IOLog("HOUJI POWER CENSUS: gcc votes 52000=%08x 52008=%08x 52010=%08x 52018=%08x 52020=%08x 52028=%08x gdsc-collapse 5214c=%08x "
          "gpu-ahb=%08x disp-ahb=%08x\n", va[0][0x52000 / 4], va[0][0x52008 / 4], va[0][0x52010 / 4], va[0][0x52018 / 4],
          va[0][0x52020 / 4], va[0][0x52028 / 4], va[0][0x5214c / 4], gpuAhb, dispAhb);
    // enabled (bit 0); bit 31 also reads "off" while hardware gating idles the clock
    if (!(gpuAhb & 1)) va[1] = nullptr;
    if (!(dispAhb & 1)) va[2] = nullptr;
  }
  for (unsigned b = 0; b < blocks; b++) {
    if (!va[b]) {IOLog("HOUJI POWER CENSUS: %s not read (not mapped, or its bus clock is off)\n", houjiCensusBlocks[b].name); continue;}
    for (const char *kind = "GPRB"; *kind; kind++) {
      char line[230]; unsigned used = 0, on = 0, total = 0;
      for (const auto &e : houjiCensus) {
        if (e.kind != *kind || e.block != b) continue;
        const uint32_t v = va[b][e.offset / 4];
        total++;
        int n = 0;
        if (e.kind == 'G') {on += v >> 31; n = snprintf(line + used, sizeof line - used, " %s=%08x", e.name, v);}
        else if (e.kind == 'P') {on += v & 1; n = snprintf(line + used, sizeof line - used, " %s=%08x", e.name, v);}
        else if (v >> 31) continue;
        else if (e.kind == 'R') {on++; n = snprintf(line + used, sizeof line - used, " %s=%x", e.name, va[b][e.offset / 4 + 1] & 0xffff);}
        else {on++; n = snprintf(line + used, sizeof line - used, " %s", e.name);}
        if (n > 0) used += (unsigned)n;
        if (used > 170) {IOLog("HOUJI POWER CENSUS %s %c:%s\n", houjiCensusBlocks[b].name, e.kind, line); used = 0; IOSleep(4);}
      }
      if (used) {IOLog("HOUJI POWER CENSUS %s %c:%s\n", houjiCensusBlocks[b].name, *kind, line); IOSleep(4);}
      IOLog("HOUJI POWER CENSUS %s %c: %u of %u on\n", houjiCensusBlocks[b].name, *kind, on, total);
    }
  }
  for (unsigned b = 0; b < blocks; b++) OSSafeReleaseNULL(maps[b]);
  IOLog("HOUJI POWER CENSUS: done\n");
  __atomic_store_n(&busy, 0u, __ATOMIC_RELEASE);
  thread_terminate(current_thread());
}
static const char *number(const char *p, uint32_t *value, unsigned base) {
  while (*p == ' ') p++;
  uint32_t v = 0; bool any = false;
  for (;; p++) {
    unsigned digit;
    if (*p >= '0' && *p <= '9') digit = (unsigned)(*p - '0');
    else if (base == 16 && *p >= 'a' && *p <= 'f') digit = (unsigned)(*p - 'a') + 10;
    else break;
    v = v * base + digit; any = true;
  }
  if (any) *value = v;
  return any ? p : nullptr;
}
// ---- RPMh (apps RSC DRV 2: 0x17a20000, TCS block at +0xd00; command database 0x81c60000) ----
static constexpr uint64_t kRsc = 0x17a20000ULL, kCmdDbBase = 0x81c60000ULL;
static constexpr uint32_t kTcsBlock = 0xd00, kCmdDbBytes = 0x20000;
struct CmdDbName {char id[9]; uint32_t address;};
static CmdDbName *names;
static unsigned nameCount;
static IOLock *rpmhLock;
static volatile uint32_t *rsc;
static bool loadNames(void) {
  if (names) return true;
  IODeviceMemory *m = IODeviceMemory::withRange(kCmdDbBase, kCmdDbBytes);
  IOMemoryMap *map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly) : nullptr;
  if (m) m->release();
  if (!map) return false;
  const volatile uint8_t *db = (const volatile uint8_t *)map->getVirtualAddress();
  auto le16 = [db](uint32_t at) -> uint32_t {return db[at] | (uint32_t)db[at + 1] << 8;};
  auto le32 = [le16](uint32_t at) -> uint32_t {return le16(at) | le16(at + 2) << 16;};
  if (le32(4) != 0x0c0330db) {map->release(); return false;}   // QCOM_CMD_DB_MAGIC
  unsigned total = 0;
  for (unsigned s = 0; s < 8 && le16(8 + 16 * s); s++) total += le16(8 + 16 * s + 6);
  names = (CmdDbName *)IOMallocZeroData(sizeof(CmdDbName) * (total ? total : 1));
  if (!names) {map->release(); return false;}
  for (unsigned s = 0; s < 8 && le16(8 + 16 * s); s++) {
    const uint32_t heads = 144 + le16(8 + 16 * s + 2), count = le16(8 + 16 * s + 6);
    for (uint32_t e = 0; e < count && nameCount < total && heads + 24 * e + 24 <= kCmdDbBytes; e++) {
      CmdDbName &n = names[nameCount++];
      for (unsigned i = 0; i < 8; i++) n.id[i] = (char)db[heads + 24 * e + i];
      n.address = le32(heads + 24 * e + 16);
    }
  }
  map->release();
  return true;
}
static const char *nameOf(uint32_t address) {
  for (unsigned i = 0; i < nameCount; i++) if (names[i].address == address) return names[i].id;
  for (unsigned i = 0; i < nameCount; i++) if ((names[i].address & ~0xfu) == (address & ~0xfu)) return names[i].id;   // +4, +8: a VRM's fields
  return "?";
}
static bool openRsc(void) {
  if (rsc) return true;
  IODeviceMemory *m = IODeviceMemory::withRange(kRsc, 0x10000);
  IOMemoryMap *map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;   // kept for good
  if (m) m->release();
  rsc = map ? (volatile uint32_t *)map->getVirtualAddress() : nullptr;
  return rsc != nullptr;
}
static void rpmhThread(void *, wait_result_t) {
  if (!loadNames() || !openRsc()) {IOLog("HOUJI POWER RPMH: cannot read the RSC or the command database\n"); thread_terminate(current_thread()); return;}
  const QcomRscLayout l = qcom_rsc_layout(rsc[0]);
  IOLog("HOUJI POWER RPMH: drv id %08x, %u names; irq-status %08x\n", rsc[0], nameCount, rsc[(kTcsBlock + l.irq_status) / 4]);
  for (unsigned t = 0; t < 8; t++) {
    const uint32_t base = kTcsBlock + l.tcs_stride * t;
    const uint32_t control = rsc[(base + l.control) / 4], enable = rsc[(base + l.cmd_enable) / 4], status = rsc[(base + l.status) / 4];
    IOLog("HOUJI POWER RPMH TCS%u: control=%08x enable=%04x status=%08x wait=%04x\n", t, control, enable, status, rsc[(base + l.wait_for_cmpl) / 4]);
    for (unsigned c = 0; c < 16; c++) {
      const uint32_t cmd = base + l.cmd_stride * c;
      const uint32_t msgid = rsc[(cmd + l.cmd_msgid) / 4], address = rsc[(cmd + l.cmd_addr) / 4], data = rsc[(cmd + l.cmd_data) / 4];
      if (!address && !data) continue;
      IOLog("HOUJI POWER RPMH TCS%u.%u: %s msgid=%08x addr=%05x %-8s data=%08x\n", t, c, (enable >> c) & 1 ? "on " : "off", msgid, address,
            nameOf(address), data);
    }
    IOSleep(3);
  }
  // CPU L3 (qcom,dcvs-hw l3: l3-base 0x17d90000, table at +0x100, shared vote at +0x90)
  IODeviceMemory *m = IODeviceMemory::withRange(0x17d90000ULL, 0x1000);
  IOMemoryMap *map = m ? m->map(kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly) : nullptr;
  if (m) m->release();
  if (map) {
    const volatile uint32_t *l3 = (const volatile uint32_t *)map->getVirtualAddress();
    char line[200]; unsigned used = 0;
    for (unsigned i = 0; i < 40; i++) {
      const uint32_t raw = l3[0x100 / 4 + i];
      if (i && raw == l3[0x100 / 4 + i - 1]) break;
      const int n = snprintf(line + used, sizeof line - used, " %u", (raw & 255) * 19200 / 1000);
      if (n > 0 && used + (unsigned)n < sizeof line) used += (unsigned)n;
    }
    IOLog("HOUJI POWER RPMH L3: 0=%08x 20=%08x 90=%08x 320=%08x table MHz:%s\n", l3[0], l3[0x20 / 4], l3[0x90 / 4], l3[0x320 / 4], line);
    map->release();
  }
  IOLog("HOUJI POWER RPMH: done\n");
  thread_terminate(current_thread());
}
struct VoteArgs {char name[9]; uint32_t value;};
static void voteThread(void *arg, wait_result_t) {
  VoteArgs *heap = (VoteArgs *)arg;
  VoteArgs v = *heap; IOFreeType(heap, VoteArgs);
  uint32_t address = 0;
  if (loadNames() && openRsc())
    for (unsigned i = 0; i < nameCount && !address; i++) if (!strncmp(names[i].id, v.name, 8)) address = names[i].address;
  if (!address) {IOLog("HOUJI POWER VOTE: no resource %s\n", v.name); thread_terminate(current_thread()); return;}
  if (!rpmhLock) rpmhLock = IOLockAlloc();
  IOLockLock(rpmhLock);
  const QcomRscLayout l = qcom_rsc_layout(rsc[0]);
  QcomRpmhWrite w[16];
  const unsigned n = qcom_rpmh_active_request(&l, 2, &address, &v.value, 1, w, 16);
  for (unsigned i = 0; i < n; i++) {rsc[(kTcsBlock + w[i].offset) / 4] = w[i].value; __asm__ volatile("dsb sy" ::: "memory");}
  bool done = false;
  for (unsigned i = 0; i < 100 && !done; i++) {done = (rsc[(kTcsBlock + l.irq_status) / 4] >> 2) & 1; if (!done) IOSleep(1);}
  const unsigned m = qcom_rpmh_finish(&l, 2, w, 16);
  for (unsigned i = 0; i < m; i++) rsc[(kTcsBlock + w[i].offset) / 4] = w[i].value;
  __asm__ volatile("dsb sy" ::: "memory");
  IOLockUnlock(rpmhLock);
  IOLog("HOUJI POWER VOTE: %s (%05x) = %08x %s\n", v.name, address, v.value, done ? "done" : "NOT acknowledged in 100 ms");
  thread_terminate(current_thread());
}
}  // namespace HoujiPower
extern "C" void houji_power_command(const char *what) {
  using namespace HoujiPower;
  if (!strcmp(what, "census")) {
    thread_t t;
    if (kernel_thread_start(censusThread, nullptr, &t) == KERN_SUCCESS) thread_deallocate(t);
  } else if (!strncmp(what, "off ", 4)) {
    uint32_t m = mask();
    if (number(what + 4, &m, 16)) {mapKill(m); sleepOff = m;}
    IOLog("HOUJI POWER: sleep hooks %x\n", mask());
  } else if (!strncmp(what, "sleep ", 6)) {
    uint32_t seconds = 0, m = mask();
    const char *rest = number(what + 6, &seconds, 10);
    if (!rest || seconds < 5 || seconds > 7200) {IOLog("HOUJI POWER: pw sleep N [MASK], N 5..7200 s\n"); return;}
    if (number(rest, &m, 16)) {mapKill(m); sleepOff = m;}
    uint64_t ticks = 0;
    nanoseconds_to_absolutetime((uint64_t)seconds * NSEC_PER_SEC, &ticks);
    __atomic_store_n(&labUntil, __builtin_arm_rsr64("CNTVCT_EL0") + ticks, __ATOMIC_RELEASE);
    IOLog("HOUJI POWER: sleep until %u s from now with hooks %x\n", seconds, mask());
    houji_sleep_now();
  } else if (!strncmp(what, "l3 ", 3)) {
    uint32_t row = 0;
    if (number(what + 3, &row, 10) && row < 18 && mapL3()) {
      l3vote[0x90 / 4] = row; __asm__ volatile("dsb sy" ::: "memory");
      IODelay(200);
      IOLog("HOUJI POWER: L3 vote %u, state %u MHz\n", l3vote[0x90 / 4], (l3vote[0x20 / 4] & 255) * 192 / 10);
    } else IOLog("HOUJI POWER: pw l3 N (0..17)\n");
  } else if (!strcmp(what, "rpmh")) {
    thread_t t;
    if (kernel_thread_start(rpmhThread, nullptr, &t) == KERN_SUCCESS) thread_deallocate(t);
  } else if (!strncmp(what, "vote ", 5)) {
    VoteArgs *v = IOMallocType(VoteArgs);
    const char *p = what + 5; unsigned i = 0;
    while (*p && *p != ' ' && i < 8) v->name[i++] = *p++;
    v->name[i] = 0;
    v->value = 0;
    thread_t t;
    if (!number(p, &v->value, 16) || kernel_thread_start(voteThread, v, &t) != KERN_SUCCESS) {
      IOFreeType(v, VoteArgs); IOLog("HOUJI POWER: pw vote NAME HEX\n");
    } else thread_deallocate(t);
  } else IOLog("HOUJI POWER: pw census | pw rpmh | pw vote NAME HEX | pw off MASK | pw sleep N [MASK]; hooks %x\n", mask());
}
