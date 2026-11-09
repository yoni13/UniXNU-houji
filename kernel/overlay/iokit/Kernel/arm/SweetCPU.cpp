// SPDX-License-Identifier: BSD-2-Clause-Patent
// Houji opt-in 1..8-core SMP bring-up with captured affinities and per-core proof.
// Two scheduler classes (A520 E; A720/X4 P), with original per-domain DVFS.
// Hardware scheduling/performance qualification is separate from CPU online proof.
#include <pexpert/arm64/board_config.h>
#if defined(SWEET)
#include <IOKit/IOCPU.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <arm64/proc_reg.h>
#include <machine/atomic.h>
#include <kern/clock.h>
#include <kern/processor.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <mach/mach_time.h>
#include "HoujiCpuPolicy.h"
#include "HoujiSmpGic.h"
#include "HoujiCpuDvfs.inc"

extern "C" void houji_secondary_entry(void);
// sched_prim.h only exports this internal primitive to osfmk.
extern "C" processor_t thread_bind(processor_t processor);
extern "C" uint64_t houji_cpu_context_pa(unsigned int logical_cpu);
extern "C" void houji_cpu_off_self(void) __attribute__((noreturn));
extern "C" uint32_t houji_cpu_off_reached[8];
extern "C" int houji_cpu_off_psci;
extern "C" kern_return_t processor_exit(processor_t processor);
static uint32_t cpuResumes[8], cpuHalts[8];
extern "C" int houji_buttons_power_level(void);
extern "C" void houji_buttons_wake_press(void);
extern "C" void timer_resync_deadlines(void);
static uint32_t sleepCount, sleepLastReason;
static uint64_t sleepLastUs, sleepLastPolls;
extern "C" uint32_t houji_publish_secondary_state(uint64_t *tcr, uint64_t *root);
extern "C" char LowResetVectorBase[], const_boot_args[];
extern "C" uint64_t houji_smp_timer_irqs[8];
extern "C" uint64_t houji_smp_ipi_irqs[8];
uint64_t houji_smp_timer_irqs[8];
uint64_t houji_smp_ipi_irqs[8];
extern "C" {extern char sched_string[];}
extern "C" void houji_sched_edge_preferences(uint64_t*);
extern "C" void houji_cpu_scheduler_report(void) {
 const ml_topology_info_t *t=ml_get_topology_info();
 IOLog("HOUJI SCHED TOPOLOGY: scheduler=%s cpus=%u clusters=%u types=%x AMP=%u; real kernel topology, not utilization\n",sched_string,t->num_cpus,t->num_clusters,t->cluster_types,(unsigned)__ARM_AMP__);
 uint64_t preferences[3]={};houji_sched_edge_preferences(preferences);
 IOLog("HOUJI SCHED DEFAULT: P-groups=%llu E-groups=%llu early-fallback=%llu; fixed/foreground/initiated/default prefer P, utility/background prefer E\n",preferences[0],preferences[1],preferences[2]);
 for(unsigned i=0;i<t->num_cpus;i++){
  const auto &c=t->cpus[i];
  IOLog("HOUJI SCHED CORE: logical=%u phys=%x cluster=%u type=%u; cached registration\n",c.cpu_id,c.phys_id,c.cluster_id,(unsigned)c.cluster_type);
 }
}
static uint32_t completedCPUs;
static uint32_t bootRegistered;
static uint32_t workerBeats[8];
static uint32_t workerMask;
static uint32_t startupLock;
static uint64_t cpuMIDR[8];
static uint64_t cpuMPIDR[8];
// Saved on each actual CPU before it enters normal scheduling; read-only MRS.
static uint64_t cpuTCR[8],cpuTTBR0[8],cpuTTBR1[8],cpuSCTLR[8];
static uint32_t cpuInitialIRQs[8];
static void reportCore(unsigned id) {
  IOLog("HOUJI CORE%u: MIDR=%llx MPIDR=%llx init-irq=%u timer=%llu ipi=%llu\n", id,
      __atomic_load_n(&cpuMIDR[id],__ATOMIC_ACQUIRE),
      __atomic_load_n(&cpuMPIDR[id],__ATOMIC_ACQUIRE),
      __atomic_load_n(&cpuInitialIRQs[id],__ATOMIC_ACQUIRE),
      __atomic_load_n(&houji_smp_timer_irqs[id],__ATOMIC_RELAXED),
      __atomic_load_n(&houji_smp_ipi_irqs[id],__ATOMIC_RELAXED));
  IOLog("HOUJI GPU CPU PAGE CORE%u: TCR=%llx TTBR0=%llx TTBR1=%llx SCTLR=%llx TG0=%u TG1=%u; actual per-core init snapshot, no control writes\n",id,
      __atomic_load_n(&cpuTCR[id],__ATOMIC_ACQUIRE),
      __atomic_load_n(&cpuTTBR0[id],__ATOMIC_ACQUIRE),
      __atomic_load_n(&cpuTTBR1[id],__ATOMIC_ACQUIRE),
      __atomic_load_n(&cpuSCTLR[id],__ATOMIC_ACQUIRE),
      unsigned((__atomic_load_n(&cpuTCR[id],__ATOMIC_ACQUIRE)>>14)&3),
      unsigned((__atomic_load_n(&cpuTCR[id],__ATOMIC_ACQUIRE)>>30)&3));
  if (!__atomic_load_n(&houji_gic_snapshot_ready[id],__ATOMIC_ACQUIRE)) return;
  const HoujiGicSnapshot &s = houji_gic_snapshot[id];
  IOLog("HOUJI GIC%u: grp1=%llx pmr=%llx sre=%llx daif=%llx vbar=%llx timer-ctl=%llx ctl=%llx hppir=%llx\n",
      id,s.group1,s.pmr,s.sre,s.daif,s.vbar,s.timer_control,s.control,s.highest_pending);
}

class SweetCPU : public IOCPU {
  OSDeclareDefaultStructors(SweetCPU);
  const OSSymbol *cpuName = NULL;
  uint32_t physicalID = 0;
  bool initialized = false;
  static SweetCPU *instances[8];
  static SweetCPU *pending[8];
  static void drainPending(void);
  static void startThread(void *, wait_result_t);
  bool registerProcessor(void);
  void finishStartup(void);
  static void probeWorker(void *, wait_result_t);
public:
  bool start(IOService *) APPLE_KEXT_OVERRIDE;
  void free(void) APPLE_KEXT_OVERRIDE;
  void initCPU(bool) APPLE_KEXT_OVERRIDE;
  void quiesceCPU(void) APPLE_KEXT_OVERRIDE;
  kern_return_t startCPU(vm_offset_t, vm_offset_t) APPLE_KEXT_OVERRIDE;
  void haltCPU(void) APPLE_KEXT_OVERRIDE;
  void signalCPU(IOCPU *) APPLE_KEXT_OVERRIDE;
  const OSSymbol *getCPUName(void) APPLE_KEXT_OVERRIDE;
  static SweetCPU *instance(unsigned id) { return id < 8 ? instances[id] : NULL; }
  static void bootSleep(void);
};
OSDefineMetaClassAndStructors(SweetCPU, IOCPU);
SweetCPU *SweetCPU::instances[8];
SweetCPU *SweetCPU::pending[8];

bool SweetCPU::start(IOService *provider) {
  const ml_topology_info_t *topology = ml_get_topology_info();
  OSNumber *logical = OSDynamicCast(OSNumber, provider->getProperty("logical-cpu-id"));
  OSData *reg = OSDynamicCast(OSData, provider->getProperty("reg"));
  int enabled = 0;
  PE_parse_boot_argn("houji_smp", &enabled, sizeof(enabled));
  if (!logical || !reg || reg->getLength() != sizeof(uint32_t)) return false;
  unsigned id = logical->unsigned32BitValue();
  physicalID = *(const uint32_t *)reg->getBytesNoCopy();
  if (!houjiCpuNodeAllowed(id, physicalID, topology->num_cpus, enabled == 1) ||
      id >= topology->num_cpus || topology->cpus[id].phys_id != physicalID)
    return false;
  if (!IOCPU::start(provider)) return false;
  char name[40];
  snprintf(name, sizeof(name), "SM8650 houji CPU%u", id);
  cpuName = OSSymbol::withCString(name);
  if (!cpuName) return false;
  setCPUNumber(id);
  instances[id] = this;
  if (id) {
    retain();
    __atomic_store_n(&pending[id], this, __ATOMIC_RELEASE);
    if (__atomic_load_n(&bootRegistered, __ATOMIC_ACQUIRE)) drainPending();
    return true;
  }
  if (!registerProcessor()) return false;
  finishStartup();
  __atomic_store_n(&bootRegistered, 1U, __ATOMIC_RELEASE);
  drainPending();
  return true;
}

bool SweetCPU::registerProcessor(void) {
  const ml_topology_info_t *topology = ml_get_topology_info();
  unsigned id = getCPUNumber();
  const ml_topology_cpu_t *cpu = &topology->cpus[id];
  ml_processor_info_t info = {};
  info.cpu_id = reinterpret_cast<cpu_id_t>(this);
  info.phys_id = cpu->phys_id; info.log_id = cpu->cpu_id;
  info.cluster_id = cpu->cluster_id; info.cluster_type = cpu->cluster_type;
  info.l2_cache_id = cpu->l2_cache_id; info.l2_cache_size = cpu->l2_cache_size;
  info.l3_cache_id = cpu->l3_cache_id; info.l3_cache_size = cpu->l3_cache_size;
  perfmon_interrupt_handler_func pmi = NULL;
  kern_return_t result = ml_processor_register(&info, &machProcessor, &ipi_handler, &pmi);
  if (result != KERN_SUCCESS || pmi) panic("HOUJI SMP: processor registration failed %d", result);
  return true;
}

void SweetCPU::finishStartup(void) {
  const ml_topology_info_t *topology = ml_get_topology_info();
  unsigned id = getCPUNumber();
  IOLog("HOUJI SMP: starting CPU%u affinity=%x\n", id, physicalID);
  kern_return_t result = processor_start(machProcessor);
  if (result != KERN_SUCCESS) panic("HOUJI SMP: processor_start failed %d", result);
  IOLog("HOUJI SMP: CPU%u processor_start returned; IOCPU state=%u\n", id, getCPUState());
  reportCore(id);
  if (id) {
    // A bounded remote request also tests CPU0 -> CPU1 routing after the
    // secondary's initCPU has returned. No new interrupt source is enabled.
    instances[0]->signalCPU(this);
    IOLog("HOUJI SMP: CPU0 sent one post-start SGI1 to CPU%u\n", id);
  }
  uint64_t deadline;
  nanoseconds_to_deadline(5 * NSEC_PER_SEC, &deadline);
  while (getCPUState() != kIOCPUStateRunning ||
      __atomic_load_n(&houji_smp_ipi_irqs[id], __ATOMIC_RELAXED) == 0) {
    if (mach_absolute_time() > deadline) {
      reportCore(0); reportCore(id);
      panic("HOUJI SMP: CPU%u startup/SGI timeout state=%u timer=%llu ipi=%llu MPIDR=%llx", id,
          getCPUState(),__atomic_load_n(&houji_smp_timer_irqs[id],__ATOMIC_RELAXED),
          __atomic_load_n(&houji_smp_ipi_irqs[id],__ATOMIC_RELAXED),
          __atomic_load_n(&cpuMPIDR[id],__ATOMIC_ACQUIRE));
    }
    IOSleep(1);
  }
  // initCPU runs with interrupts masked on a secondary. Print its saved
  // diagnostics here, after processor_start returns to a schedulable thread.
  assert(ml_get_interrupts_enabled());
  IOLog("HOUJI SMP: CPU%u init MIDR=%llx MPIDR=%llx interrupts-at-init=%u (deferred)\n", id,
      __atomic_load_n(&cpuMIDR[id],__ATOMIC_ACQUIRE),
      __atomic_load_n(&cpuMPIDR[id],__ATOMIC_ACQUIRE),
      __atomic_load_n(&cpuInitialIRQs[id],__ATOMIC_ACQUIRE));
  registerService();
  // CPU0 makes matching workers available; completion is published after both.
  if (!id) cpusRunning();
  unsigned done = __atomic_add_fetch(&completedCPUs, 1U, __ATOMIC_ACQ_REL);
  IOLog("HOUJI SMP: CPU%u online; completed=%u/%u\n", id, done, topology->num_cpus);
  if (done == topology->num_cpus) {
    ml_cpu_init_completed();
    IOService::publishResource(gIOAllCPUInitializedKey, kOSBooleanTrue);
    ml_set_max_cpus(topology->max_cpu_id + 1);
    int probe = 1;
    PE_parse_boot_argn("houji_smp_probe", &probe, sizeof(probe));
    if (probe == 1 && done >= 2) {
      thread_t thread;
      if (kernel_thread_start(probeWorker, NULL, &thread) != KERN_SUCCESS)
        panic("HOUJI SMP: worker creation failed");
      thread_deallocate(thread);
    }
  }
}

void SweetCPU::drainPending(void) {
  for (unsigned id = 1; id < ml_get_topology_info()->num_cpus; id++) {
    SweetCPU *cpu = __atomic_exchange_n(&pending[id], (SweetCPU *)NULL, __ATOMIC_ACQ_REL);
    if (!cpu) continue;
    thread_t thread;
    if (kernel_thread_start(startThread, cpu, &thread) != KERN_SUCCESS)
      panic("HOUJI SMP: secondary start helper creation failed");
    thread_deallocate(thread);
  }
}

void SweetCPU::startThread(void *argument, wait_result_t) {
  SweetCPU *cpu = (SweetCPU *)argument;
  thread_bind(instances[0]->machProcessor);
  if (cpu_number() != 0) thread_block(THREAD_CONTINUE_NULL);
  // Register and start one secondary at a time. Shared reset publications and
  // IOCPU registration must not race between seven new matching nubs.
  while (__atomic_exchange_n(&startupLock, 1U, __ATOMIC_ACQUIRE)) IOSleep(1);
  if (!cpu->registerProcessor()) panic("HOUJI SMP: secondary registration failed");
  cpu->finishStartup();
  __atomic_store_n(&startupLock, 0U, __ATOMIC_RELEASE);
  cpu->release();
  thread_bind(PROCESSOR_NULL);
  thread_terminate(current_thread());
}

void SweetCPU::initCPU(bool boot) {
  if (!boot) {
    // A secondary back from CPU_OFF (houji_secondary_entry, arm_init_cpu; PE_init_cpu has set up its GIC interface again).
    // Interrupts are masked here: no logging.
    if (!initialized || !getCPUNumber()) panic("HOUJI SMP: resume of a CPU that never started");
    __atomic_add_fetch(&cpuResumes[getCPUNumber()], 1U, __ATOMIC_RELAXED);
    ml_broadcast_cpu_event(CPU_BOOTED, getCPUNumber());
    setCPUState(kIOCPUStateRunning);
    return;
  }
  if (initialized) panic("HOUJI SMP: CPU initialized twice");
  initialized = true;
  unsigned id = getCPUNumber();
  assert(cpu_number() == (int)id);
  uint64_t midr = __builtin_arm_rsr64("MIDR_EL1");
  uint64_t mpidr = __builtin_arm_rsr64("MPIDR_EL1");
  if ((mpidr & 0xff00ffffffULL) != physicalID)
    panic("HOUJI SMP: CPU%u affinity mismatch %llx", id, mpidr);
  __atomic_store_n(&cpuMIDR[id], midr, __ATOMIC_RELEASE);
  __atomic_store_n(&cpuMPIDR[id], mpidr, __ATOMIC_RELEASE);
  __atomic_store_n(&cpuTCR[id], __builtin_arm_rsr64("TCR_EL1"), __ATOMIC_RELEASE);
  __atomic_store_n(&cpuTTBR0[id], __builtin_arm_rsr64("TTBR0_EL1"), __ATOMIC_RELEASE);
  __atomic_store_n(&cpuTTBR1[id], __builtin_arm_rsr64("TTBR1_EL1"), __ATOMIC_RELEASE);
  __atomic_store_n(&cpuSCTLR[id], __builtin_arm_rsr64("SCTLR_EL1"), __ATOMIC_RELEASE);
  __atomic_store_n(&cpuInitialIRQs[id], ml_get_interrupts_enabled(), __ATOMIC_RELEASE);
  if (((midr >> 24) & 0xff) != 0x41 ||
      ((midr >> 4) & 0xfff) != houjiCpuPart(id))
    panic("HOUJI SMP: unexpected CPU%u MIDR %llx", id, midr);
  ml_broadcast_cpu_event(CPU_BOOTED, id);
  // Secondary startup may mask interrupts. Its caller enables them after init;
  // wait on CPU0, never spin here waiting for a masked self-IPI.
  signalCPU(this);
  houji_smp_capture_gic(); // memory snapshot only; no output while IRQs are masked
  setCPUState(kIOCPUStateRunning);
}

void SweetCPU::signalCPU(IOCPU *target) {
  SweetCPU *cpu = OSDynamicCast(SweetCPU, target);
  uint64_t sgi;
  if (!cpu || !houjiSgiValue(cpu->physicalID, &sgi))
    panic("HOUJI SMP: unsupported SGI target");
  __builtin_arm_dsb(DSB_ISHST);
  __builtin_arm_wsr64("ICC_SGI1R_EL1", sgi);
  __builtin_arm_isb(ISB_SY);
}

kern_return_t SweetCPU::startCPU(vm_offset_t, vm_offset_t) {
  if (!getCPUNumber() || !houjiCpuNodeAllowed(getCPUNumber(), physicalID,
      ml_get_topology_info()->num_cpus, true) || !instances[0])
    return KERN_NOT_SUPPORTED;
  IORegistryEntry *psci = IORegistryEntry::fromPath("/psci", gIODTPlane);
  OSData *method = psci ? OSDynamicCast(OSData, psci->getProperty("method")) : NULL;
  bool smc = method && method->getLength() == 4 &&
      memcmp(method->getBytesNoCopy(), "smc", 4) == 0;
  OSSafeReleaseNULL(psci);
  if (!smc) panic("HOUJI SMP: captured SMC PSCI conduit absent");
  vm_offset_t entry = ml_static_vtop((vm_offset_t)&houji_secondary_entry);
  uint64_t context = houji_cpu_context_pa(getCPUNumber());
  if (!entry || !context || (entry & 3) || (context & 7))
    panic("HOUJI SMP: invalid physical entry/context");
  // The new core starts with caches off. Publish code/boot arguments to PoC;
  // XNU cpu_start() already flushes the per-CPU context and entry array.
  flush_dcache((vm_offset_t)LowResetVectorBase, 0x10000, FALSE);
  flush_dcache((vm_offset_t)const_boot_args, 0x1000, FALSE);
  // common_start reads these real osfmk entities with caches still disabled.
  // The typed helper snapshots them, then publishes their exact sizeof to PoC.
  uint64_t publishedTcr = 0, publishedRoot = 0;
  const uint32_t publishedBytes = houji_publish_secondary_state(&publishedTcr, &publishedRoot);
  IOLog("HOUJI SMP: secondary PoC published TCR=%llx root=%llx bytes=%u; CPU0 snapshots, not secondary execution proof\n",
      publishedTcr, publishedRoot, publishedBytes);
  IOLog("HOUJI SMP: PSCI CPU_ON affinity=%x entry=%llx context=%llx\n",
      physicalID, (uint64_t)entry, context);
  register uint64_t x0 __asm__("x0") = 0xc4000003ULL;
  register uint64_t x1 __asm__("x1") = physicalID;
  register uint64_t x2 __asm__("x2") = entry;
  register uint64_t x3 __asm__("x3") = context;
  // Darwin's assembler treats ';' as a comment marker, not an instruction
  // separator. Keep both instructions on distinct assembly lines.
  __asm__ volatile("dsb sy\n\tsmc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
      : : "x4","x5","x6","x7","x8","x9","x10","x11","x12","x13",
          "x14","x15","x16","x17","memory","cc");
  IOLog("HOUJI SMP: PSCI CPU_ON returned %lld\n", (int64_t)x0);
  if ((int64_t)x0 != 0) panic("HOUJI SMP: CPU_ON failed %lld", (int64_t)x0);
  return KERN_SUCCESS;
}

void SweetCPU::probeWorker(void *, wait_result_t) {
  unsigned total = ml_get_topology_info()->num_cpus;
  unsigned beats = total == 2 ? 30 : 10;
  for (unsigned id = 1; id < total; id++) {
    SweetCPU *cpu = instances[id];
    thread_bind(cpu->machProcessor);
    thread_block(THREAD_CONTINUE_NULL);
    for (unsigned i = 0; i < beats; i++) {
      if (cpu_number() != (int)id) panic("HOUJI SMP: bound worker ran on wrong CPU");
      cpu->signalCPU(instances[0]);
      __atomic_add_fetch(&workerBeats[id], 1U, __ATOMIC_RELEASE);
      IOSleep(total == 2 ? 100 : 20);
    }
    __atomic_fetch_or(&workerMask, 1U << id, __ATOMIC_RELEASE);
  }
  IOLog("HOUJI SMP: all requested secondary bound workers completed\n");
  thread_bind(PROCESSOR_NULL);
  thread_terminate(current_thread());
}

extern "C" void houji_smp_report(void) {
  unsigned total = ml_get_topology_info()->num_cpus;
  IOLog("HOUJI SMP STATE: initialized=%u target=%u worker-mask=%x; SMP scheduling, no DVFS\n",
      __atomic_load_n(&completedCPUs,__ATOMIC_ACQUIRE),total,
      __atomic_load_n(&workerMask,__ATOMIC_ACQUIRE));
  for (unsigned id = 0; id < total; id++) {
    reportCore(id);
    IOLog("HOUJI CORE%u WORKER: beats=%u expected-part=%x\n", id,
        __atomic_load_n(&workerBeats[id],__ATOMIC_ACQUIRE),houjiCpuPart(id));
  }
}
extern "C" bool houji_smp_preflight(void) {
  unsigned total = ml_get_topology_info()->num_cpus;
  if (total == 1) return true;
  if (total < 2 || total > 8) return false;
  int probe = 1;
  PE_parse_boot_argn("houji_smp_probe", &probe, sizeof(probe));
  if (probe != 1) return false;
  uint32_t mask = ((1U << total) - 1U) & ~1U;
  uint64_t deadline;
  nanoseconds_to_deadline(8 * NSEC_PER_SEC, &deadline);
  while (__atomic_load_n(&workerMask,__ATOMIC_ACQUIRE) != mask &&
      mach_absolute_time() < deadline) IOSleep(10);
  // Eight successful processor_start completions are the required HFS preflight. The bound-worker probe remains diagnostic:
  // under wfi=1 it can still be queued when setconf runs, although the cores are online and taking interrupts.
  bool pass = __atomic_load_n(&completedCPUs,__ATOMIC_ACQUIRE) == total;
  for (unsigned id = 0; id < total; id++) {
    // The mount safety proof is all CPUs online plus the exact bound-worker mask/count. On some wfi=1 boots a core has not yet
    // taken both diagnostic timer/IPI classes by this instant; report that separately, but do not make HFS depend on timing noise.
    pass = pass && (id == 0 || __atomic_load_n(&workerBeats[id],__ATOMIC_ACQUIRE) == (total == 2 ? 30U : 10U));
  }
  houji_smp_report();
  IOLog("HOUJI SMP PREFLIGHT: %s; %u cores and bound workers; timer/IPI counters are diagnostic; before HFS\n", pass ? "PASS" : "FAIL",total);
  return pass;
}

void SweetCPU::free(void) { OSSafeReleaseNULL(cpuName); IOCPU::free(); }
// On the CPU going down, at the end of cpu_sleep (interrupts masked, caches cleaned): power it off.
void SweetCPU::quiesceCPU(void) {
  if (!getCPUNumber()) panic("HOUJI SMP: quiesce of the boot CPU");
  setCPUState(kIOCPUStateStopped);
  houji_cpu_off_self();
}
// Called on another CPU (IOCPUSleepKernel, or houji_cpu_hotplug): take this one out of the scheduler; it ends in quiesceCPU.
void SweetCPU::haltCPU(void) {
  const unsigned id = getCPUNumber();
  if (!id) {bootSleep(); return;}
  static bool parsed;
  if (!parsed) {parsed = true; int v = 1; PE_parse_boot_argn("houji_cpu_off", &v, sizeof v); houji_cpu_off_psci = v != 0;}
  IOLog("HOUJI SMP: CPU%u processor_exit (%s)\n", id, houji_cpu_off_psci ? "PSCI CPU_OFF" : "WFI park");
  IOSleep(200);
  // XNU lets a CPU exit only while quiescing for system sleep (ml_cpu_can_exit); IOCPUSleepKernel sets that flag.
  const kern_return_t r = processor_exit(machProcessor);
  IOLog("HOUJI SMP: CPU%u processor_exit returned %d; reached CPU_OFF %u times\n", id, r, houji_cpu_off_reached[id]);
  if (r == KERN_SUCCESS) __atomic_add_fetch(&cpuHalts[id], 1U, __ATOMIC_RELAXED);
}
// System sleep on the boot CPU, the last one (IOCPUSleepKernel: secondaries off, console suspended). Interrupts masked, WFI with
// the virtual timer re-armed every 50 ms to look at the power key (read directly over SPMI). Wakes on a press after the key was
// seen released, or after houji_sleep_max_s seconds (0, the default: no limit; 30 s if the key cannot be read). Returns on wake;
// IOCPUSleepKernel then restarts the secondaries.
void SweetCPU::bootSleep(void) {
  unsigned maxS = 0;
  PE_parse_boot_argn("houji_sleep_max_s", &maxS, sizeof maxS);
  if (!maxS && houji_buttons_power_level() < 0) maxS = 30;
  const boolean_t enabled = ml_set_interrupts_enabled(FALSE);
  const uint64_t ctl = __builtin_arm_rsr64("CNTV_CTL_EL0"), cval = __builtin_arm_rsr64("CNTV_CVAL_EL0");
  uint64_t tick, limit = 0;
  nanoseconds_to_absolutetime(50 * NSEC_PER_MSEC, &tick);
  if (maxS) nanoseconds_to_absolutetime((uint64_t)maxS * NSEC_PER_SEC, &limit);
  const uint64_t t0 = __builtin_arm_rsr64("CNTVCT_EL0");
  bool released = false;
  uint32_t reason = 0;
  uint64_t polls = 0;
  for (;;) {
    const int key = houji_buttons_power_level();
    if (key == 0) released = true;
    else if (key == 1 && released) {reason = 1; break;}
    const uint64_t now = __builtin_arm_rsr64("CNTVCT_EL0");
    if (limit && now - t0 >= limit) {reason = 2; break;}
    __builtin_arm_wsr64("CNTV_CVAL_EL0", now + tick);
    __builtin_arm_wsr64("CNTV_CTL_EL0", 1);   // enabled, not masked: its interrupt (pending, masked here) ends WFI
    __builtin_arm_isb(ISB_SY);
    __builtin_arm_dsb(DSB_SY);
    __builtin_arm_wfi();
    polls++;
  }
  const uint64_t slept = __builtin_arm_rsr64("CNTVCT_EL0") - t0;
  __builtin_arm_wsr64("CNTV_CVAL_EL0", cval);
  __builtin_arm_wsr64("CNTV_CTL_EL0", ctl);
  __builtin_arm_isb(ISB_SY);
  ml_set_interrupts_enabled(enabled);
  timer_resync_deadlines();
  uint64_t ns; absolutetime_to_nanoseconds(slept, &ns);
  sleepCount++; sleepLastReason = reason; sleepLastUs = ns / 1000; sleepLastPolls = polls;
  if (reason == 1) houji_buttons_wake_press();
  IOLog("HOUJI SLEEP: woke after %llu ms (%s), %llu polls; sleep %u\n", ns / 1000000, reason == 1 ? "power key" : "time limit", polls,
        sleepCount);
}
// USB console "sleep": ask the root power domain to sleep now, as a sleep request from iOS would
extern "C" void houji_sleep_now(void) {
  IOPMrootDomain *root = IOService::getPMRootDomain();
  IOLog("HOUJI SLEEP: requesting system sleep (kIOPMSleepNow)\n");
  if (root) root->receivePowerNotification(kIOPMSleepNow);
}
// USB console "cpu off N" / "cpu on N": one secondary down or up, outside system sleep (bring-up test of the sleep path).
static void hotplugThread(void *arg, wait_result_t) {
  const uintptr_t v = (uintptr_t)arg; const unsigned id = (unsigned)(v & 0xff); const bool on = v >> 8;
  SweetCPU *cpu = id < 8 ? SweetCPU::instance(id) : NULL;
  thread_bind(SweetCPU::instance(0)->getMachProcessor());
  if (cpu_number() != 0) thread_block(THREAD_CONTINUE_NULL);
  if (!cpu || !id) IOLog("HOUJI SMP: cpu %s %u: no such secondary\n", on ? "on" : "off", id);
  else if (!on && cpu->getCPUState() == kIOCPUStateRunning) {
    const uint64_t t0 = mach_absolute_time();
    ml_set_is_quiescing(true);   // as IOCPUSleepKernel does around the halts
    cpu->haltCPU();
    ml_set_is_quiescing(false);
    uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
    IOLog("HOUJI SMP: CPU%u off in %llu us; state=%u halts=%u\n", id, ns / 1000, cpu->getCPUState(), cpuHalts[id]);
  } else if (on && cpu->getCPUState() == kIOCPUStateStopped) {
    const uint64_t t0 = mach_absolute_time();
    const kern_return_t r = processor_start(cpu->getMachProcessor());
    uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
    IOLog("HOUJI SMP: CPU%u on: processor_start %d in %llu us; state=%u resumes=%u\n", id, r, ns / 1000, cpu->getCPUState(),
          cpuResumes[id]);
  } else IOLog("HOUJI SMP: cpu %s %u: state %u, nothing to do\n", on ? "on" : "off", id, cpu->getCPUState());
  thread_bind(PROCESSOR_NULL);
  thread_terminate(current_thread());
}
extern "C" void houji_cpu_hotplug(unsigned id, int on) {
  thread_t t;
  if (kernel_thread_start(hotplugThread, (void *)(uintptr_t)((id & 0xff) | (on ? 0x100 : 0)), &t) == KERN_SUCCESS) thread_deallocate(t);
}
const OSSymbol *SweetCPU::getCPUName(void) { return cpuName; }
extern "C" void houji_cpu_dvfs_start(void) { HoujiCpuDvfs::start(); }
#endif
