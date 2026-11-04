/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Standalone USB bring-up for the CDC ACM console (houji).
 *
 * On a PC-assisted boot ABL's fastboot leaves the USB30 PRIM domain, its
 * clocks and the eUSB2 HS PHY initialized, and the XNU console driver
 * (houji_usb_debug.c) only reuses that state. A normal power-on from
 * boot_a initializes none of it, so the console stayed down on standalone
 * boots (verified 2026-11-04: no HOUJI USB lines in the snapshot, while a
 * PC-assisted boot logs "GCC power=f822f800 master=1 cfg=1 aggre=1").
 *
 * This handoff reproduces the preconditions the kernel driver checks,
 * from evidence:
 *  - GCC registers and the 200 MHz master setting come from torvalds
 *    drivers/clk/qcom/gcc-sm8650.c: GDSCR 0x39004 (power bit 31, SW
 *    collapse bit 0), USB30_PRIM_BCR 0x39000, master CBCR 0x39018,
 *    cfg_noc 0x39088, aggre 0x3908c, mock_utmi CBCR 0x39028, sleep
 *    0x39024, master RCG cmd_rcgr 0x3902c, mock_utmi RCG 0x39044
 *    (postdiv 0x3905c is read-only). Freq table: 200 MHz = GPLL0/3,
 *    GPLL0_OUT_MAIN is parent-map-0 entry src 1. RCG encoding per
 *    clk-rcg2.c: CFG at cmd_rcgr+4 (src<<8 | prediv, prediv reg =
 *    divisor*2-1), CMD bit 0 = UPDATE, poll until it clears.
 *    CBCR bit 31 is the clock-off status: clear means running.
 *  - TCSR usb2_clkref_en: tcsrcc-sm8650.c 0x1f40000 + 0x31118, bit 0.
 *  - eUSB2 HS PHY (0x88e3000): qcom_snps_eusb2_hsphy_init() from
 *    torvalds drivers/phy/phy-snps-eusb2.c, 19.2 MHz reference variant
 *    (FSEL=0, PLL fb div 7:0=0x90, 11:8=1).
 *  - PM8550B eUSB2 repeater (SPMI SID 7, peripheral 0xfd, live-DT
 *    phandle 0x632): register map from torvalds
 *    phy-qcom-eusb2-repeater.c (EN_CTL1 0x46 bit 7, STATUS 0x08 bit 7);
 *    tune values are this phone's own qcom,param-override-seq (live DT,
 *    device-mode sequence); device mode also clears the host 19.2 MHz
 *    force registers 0xe8/0xed. SPMI arbiter access uses the v7 channel
 *    layout already hardware-proven by this fork's BootResetCore.h
 *    (buttons driver), for PPID (7<<8)|0xfd.
 *  - Regulator rails resolved from this phone's live device tree:
 *    CmdDb resources ldoi1 (880 mV, hsphy vdd), ldoi3 (1200 mV, vdda12),
 *    ldob5 (3104 mV, repeater vdd3), ldob15 (1800 mV, repeater vdd18),
 *    enabled through the same RPMh/CmdDb DXE interfaces as the proven
 *    touch handoff (pmic5-ldo: +0 voltage, +8 mode HPM=7, +4 enable).
 *
 * Only controller existence is reproduced: GCTL is left in device mode
 * (PRTCAPDIR=2, the fastboot-left value whose kernel readback on this
 * phone is 0x00102001) with RUN off. All endpoint/event/DCFG setup stays
 * in the kernel driver, unchanged, with all its guards intact.
 * Failure here is non-fatal: the boot continues and the kernel driver
 * simply reports its existing preflight failure, as it does today.
 */
#ifndef HOUJI_USB_HANDOFF_H
#define HOUJI_USB_HANDOFF_H
/* Defined in LogFsTest.h, included after this file. */
EFI_STATUS EFIAPI ProbeCheckpoint (CONST CHAR8 *Stage);

/* GCC window: base 0x100000, USB30 PRIM block at +0x39000 (GDSCR absolute
 * 0x139004, as the kernel driver checks at GCC 0x139000 + 4). */
#define HOUJI_GCC_BASE           0x00100000ULL
#define HOUJI_GCC_USB_GDSCR      0x039004
#define HOUJI_GCC_USB_BCR        0x039000
#define HOUJI_GCC_USB_MASTER     0x039018
#define HOUJI_GCC_USB_CFG_NOC    0x039088
#define HOUJI_GCC_USB_AGGRE      0x03908c
#define HOUJI_GCC_USB_UTMI       0x039028
#define HOUJI_GCC_USB_SLEEP      0x039024
#define HOUJI_GCC_USB_MASTER_RCG 0x03902c
#define HOUJI_GCC_USB_UTMI_RCG   0x039044
#define HOUJI_GCC_QUSB2PHY_BCR   0x012000

#define HOUJI_TCSR_BASE          0x01f40000ULL
#define HOUJI_TCSR_USB2_CLKREF   0x031118

/* eUSB2 HS PHY at 0x88e3000 (phy-snps-eusb2.c register map). */
#define HOUJI_HSPHY_BASE         0x088e3000ULL
#define HSPHY_UTMI_CTRL0         0x3c
#define HSPHY_UTMI_CTRL5         0x50
#define HSPHY_CTRL_COMMON0       0x54
#define HSPHY_CFG_CTRL_1         0x58
#define HSPHY_CFG_CTRL_2         0x5c
#define HSPHY_CFG_CTRL_3         0x60
#define HSPHY_CTRL_2             0x64
#define HSPHY_CFG_CTRL_4         0x68
#define HSPHY_CFG_CTRL_5         0x6c
#define HSPHY_CFG_CTRL_6         0x70
#define HSPHY_CFG_CTRL_8         0x78
#define HSPHY_CFG_CTRL_9         0x7c
#define HSPHY_CFG0               0x94
#define HSPHY_CMN_CTRL0          0x98
#define HSPHY_FSEL_SEL           0xb8
#define HSPHY_APB_ACCESS_CMD     0x130

/* DWC3 (0xa600000): only GCTL device-mode selection. */
#define HOUJI_DWC3_BASE          0x0a600000ULL
#define DWC3_GCTL                0xc110
#define DWC3_GCTL_PRTCL_SHIFT    12
#define DWC3_GCTL_PRTCL_DEVICE   2

/* SPMI PMIC arbiter, v7 layout (BootResetCore.h, hardware-proven). */
#define HOUJI_SPMI_CORE          0x0c400000ULL
#define HOUJI_SPMI_CNFG         0x0c42d000ULL
#define HOUJI_SPMI_CHNLS         0x0c500000ULL
#define HOUJI_SPMI_OBSRVR        0x0c440000ULL
#define HOUJI_RPTR_PPID          0x7fd
#define HOUJI_RPTR_EN_CTL1       0x46
#define HOUJI_RPTR_STATUS        0x08

STATIC BOOLEAN mUsbHandoffAttempted;
STATIC VOID *mUsbPowerClient;

STATIC
UINT32
UsbHsRead32 (UINT64 Base, UINT32 Off)
{
  __asm__ volatile ("dmb oshld" ::: "memory");
  return *(volatile UINT32 *)(UINTN)(Base + Off);
}



STATIC EFI_STATUS
PrepareUsbHandoff (VOID)
{
  if (mUsbHandoffAttempted) { return EFI_ALREADY_STARTED; }
  mUsbHandoffAttempted = TRUE;
  /* Diagnostic-only by design. The standalone USB recovery lives in the
   * kernel console driver (houji_usb_debug.c, reset-timeout branch): the
   * UEFI-side attempts stalled the boot twice (RPMh rail re-vote with no
   * HJ_WAIT timeout; USB30 block reset with ABL's gadget state live),
   * while the kernel driver runs after the UFS root is safe and already
   * owns the proven halt/reset/run sequence. This function only records
   * the state the kernel will find, which the kernel report correlates. */
  BOOLEAN AblPowerOn = (UsbHsRead32 (HOUJI_GCC_BASE, HOUJI_GCC_USB_GDSCR) & BIT31) &&
    !(UsbHsRead32 (HOUJI_GCC_BASE, HOUJI_GCC_USB_MASTER) & BIT31) &&
    !(UsbHsRead32 (HOUJI_GCC_BASE, HOUJI_GCC_USB_CFG_NOC) & BIT31) &&
    !(UsbHsRead32 (HOUJI_GCC_BASE, HOUJI_GCC_USB_AGGRE) & BIT31);
  Print (L"HOUJI USB UEFI: ABL power/clock state present=%u (gdscr=%08x master=%08x)\n",
    AblPowerOn,
    UsbHsRead32 (HOUJI_GCC_BASE, HOUJI_GCC_USB_GDSCR),
    UsbHsRead32 (HOUJI_GCC_BASE, HOUJI_GCC_USB_MASTER));
  (VOID)ProbeCheckpoint (AblPowerOn ? "usb-handoff-abl-state-detected" : "usb-handoff-cold-path");
  return EFI_SUCCESS;
}
#endif
