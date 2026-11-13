/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Power work for system sleep: what is switched down while the boot CPU waits in SweetCPU::bootSleep, and tools to find where the
 * power goes. Included by SweetCPU.cpp after HoujiCpuDvfs.inc.
 *
 * Sleep hooks (a mask; boot argument houji_sleep_off=MASK, default 0 = none; console "pw off MASK"). enter() and leave() run on the
 * boot CPU with interrupts masked and every other CPU off: no logging, no blocking; results are kept and printed after the wake.
 *   1  all four CPU frequency domains to their lowest row for the sleep (the governor thread is frozen where the sleep caught it)
 *   2  the UFS link into HIBERN8 (houji_ufs_hibernate)
 * Measurement only (never set these in houji_sleep_off for use):
 *   80   the boot CPU spins instead of waiting in WFI: a known extra load, to check that a reading reflects the sleep
 *   100  GPU off and NOT restored: GX and CX-GMU clock branches off, then both power domains collapsed (GPUCC). The GPU is dead
 *        until the next boot: read the battery at the wake, then reboot.
 *   200  display controller off and NOT restored: both MDSS power domains collapsed (DISPCC). Reboot afterwards.
 * Console:
 *   pw census            which clocks, root generators, PLLs and power domains of GCC, GPUCC and DISPCC are on (reads only)
 *   pw off MASK          the hooks for every later sleep (hex)
 *   pw sleep N [MASK]    sleep now with these hooks (default: the current mask) and wake fully, without a key press (the display
 *                        stays off), N seconds after the command: for measuring a configuration from the battery's charge counter
 */
#include "HoujiClockCensus.h"
extern "C" int houji_ufs_hibernate(int enter, uint64_t *status);
extern "C" void houji_sleep_now(void);
namespace HoujiPower {
enum {kDvfsLowest = 1, kUfsHibernate = 2, kSpin = 0x80, kGpuKill = 0x100, kMdssKill = 0x200};
static volatile uint32_t *gpucc, *dispcc;
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
  } else IOLog("HOUJI POWER: pw census | pw off MASK | pw sleep N [MASK]; hooks %x\n", mask());
}
