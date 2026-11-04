/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Read-only USB state capture before XNU.
 *
 * UEFI-side USB recovery was removed: regulator votes and USB30 block
 * resets both destabilized standalone boots. The kernel console driver owns
 * the bounded recovery and records its result in the durable XNU snapshot.
 * This file only captures the ABL-left GCC state and checkpoints it for
 * correlation with that kernel report. It performs no USB, clock, PHY,
 * regulator, SPMI, reset, endpoint, event-buffer, or DMA writes.
 */
#ifndef HOUJI_USB_HANDOFF_H
#define HOUJI_USB_HANDOFF_H
/* Defined in LogFsTest.h, included after this file. */
EFI_STATUS EFIAPI ProbeCheckpoint (CONST CHAR8 *Stage);

/* GCC window: base 0x100000; the kernel checks this same USB30 PRIM block. */
#define HOUJI_GCC_BASE           0x00100000ULL
#define HOUJI_GCC_USB_GDSCR      0x039004
#define HOUJI_GCC_USB_MASTER     0x039018
#define HOUJI_GCC_USB_CFG_NOC    0x039088
#define HOUJI_GCC_USB_AGGRE      0x03908c

STATIC BOOLEAN mUsbHandoffAttempted;

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
  BOOLEAN AblPowerOn = (UsbHsRead32 (HOUJI_GCC_BASE, HOUJI_GCC_USB_GDSCR) & BIT31) &&
    !(UsbHsRead32 (HOUJI_GCC_BASE, HOUJI_GCC_USB_MASTER) & BIT31) &&
    !(UsbHsRead32 (HOUJI_GCC_BASE, HOUJI_GCC_USB_CFG_NOC) & BIT31) &&
    !(UsbHsRead32 (HOUJI_GCC_BASE, HOUJI_GCC_USB_AGGRE) & BIT31);
  Print (L"HOUJI USB UEFI: read-only ABL state present=%u (gdscr=%08x master=%08x)\n",
    AblPowerOn,
    UsbHsRead32 (HOUJI_GCC_BASE, HOUJI_GCC_USB_GDSCR),
    UsbHsRead32 (HOUJI_GCC_BASE, HOUJI_GCC_USB_MASTER));
  (VOID)ProbeCheckpoint (AblPowerOn ? "usb-handoff-abl-state-detected" : "usb-handoff-cold-path");
  return EFI_SUCCESS;
}
#endif
