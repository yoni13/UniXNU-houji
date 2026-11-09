// SPDX-License-Identifier: BSD-2-Clause-Patent
// CPU-interface system registers only. Do not read redistributor SGI MMIO:
// houji r40 aborted on GICR_IGROUPR0 despite earlier configuration writes.
#if defined(SWEET)
#include "HoujiSmpGic.h"
struct HoujiGicSnapshot houji_gic_snapshot[8];
uint32_t houji_gic_snapshot_ready[8];
void houji_smp_capture_gic(void) {
  unsigned int id = ml_get_cpu_number_local();
  if (id >= 8) return;
  struct HoujiGicSnapshot *s = &houji_gic_snapshot[id];
  s->group1 = __builtin_arm_rsr64("ICC_IGRPEN1_EL1");
  s->pmr = __builtin_arm_rsr64("ICC_PMR_EL1");
  s->sre = __builtin_arm_rsr64("ICC_SRE_EL1");
  s->daif = __builtin_arm_rsr64("DAIF");
  s->vbar = __builtin_arm_rsr64("VBAR_EL1");
  s->timer_control = __builtin_arm_rsr64("CNTV_CTL_EL0");
  s->control = __builtin_arm_rsr64("ICC_CTLR_EL1");
  // HPPIR observes the highest pending interrupt without acknowledging it.
  s->highest_pending = __builtin_arm_rsr64("ICC_HPPIR1_EL1");
  __atomic_store_n(&houji_gic_snapshot_ready[id], 1U, __ATOMIC_RELEASE);
}
/*
 * System sleep on the boot CPU (SweetCPU): only the virtual timer may end WFI. It gets priority 0x80 and the mask 0x90, so
 * device SPIs and the other PPIs (all 0xA0, as pe_init_fiq sets them) stay pending until wake instead of ending every WFI.
 * Known values are written, nothing is read back (a redistributor SGI-frame read aborted on houji).
 */
void houji_gic_sleep_mask(int enter) {
  vm_offset_t gicr_pe_base = find_gicr_pe_base();
  const uint32_t reg = GICR_IPRIORITYR0 + (sweet_vtimer_intid & ~3U);
  const uint32_t shift = (sweet_vtimer_intid & 3U) * 8;
  if (enter) {
    gicr_write32(reg, (0xA0A0A0A0U & ~(0xFFU << shift)) | (0x80U << shift));
    __builtin_arm_dsb(DSB_SY);
    __builtin_arm_wsr64("ICC_PMR_EL1", 0x90);
  } else {
    __builtin_arm_wsr64("ICC_PMR_EL1", 0xFF);
    gicr_write32(reg, 0xA0A0A0A0U);
  }
  __builtin_arm_isb(ISB_SY);
}
#endif
