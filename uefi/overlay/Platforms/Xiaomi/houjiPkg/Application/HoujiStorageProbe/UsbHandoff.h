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

STATIC
VOID
UsbHsWrite32 (UINT64 Base, UINT32 Off, UINT32 Value)
{
  *(volatile UINT32 *)(UINTN)(Base + Off) = Value;
  __asm__ volatile ("dsb sy" ::: "memory");
}

STATIC
VOID
UsbHsMask32 (UINT64 Base, UINT32 Off, UINT32 Mask, UINT32 Value)
{
  UINT32 Reg = UsbHsRead32 (Base, Off);
  Reg &= ~Mask;
  Reg |= Value & Mask;
  UsbHsWrite32 (Base, Off, Reg);
  (VOID)UsbHsRead32 (Base, Off);
}

/* SPMI v7 arbiter: find the APID for a PPID, then byte reads through the
 * observer channels (EE 0, 0x20 stride) and writes through the command
 * channels (0x1000 stride). Command: op<<27 | byte_addr<<4; status done
 * bit 0, error bits 1..3. Same layout as BootResetCore.h. */
STATIC
BOOLEAN
UsbSpmiFindApid (INT32 *Apid)
{
  volatile UINT32 *Core = (volatile UINT32 *)(UINTN)HOUJI_SPMI_CORE;
  UINT32 Version = Core[0];
  UINT32 Apids = Core[1] & 0x7ffu;
  *Apid = -1;
  if (Version < 0x70000000u || Version >= 0x80000000u || !Apids || Apids > 1024) {
    return FALSE;
  }
  for (UINT32 I = 0; I < Apids; I++) {
    UINT32 Map = Core[0x2000 / 4 + I];
    if (Map && ((Map >> 8) & 0xfff) == HOUJI_RPTR_PPID) {
      *Apid = (INT32)I;
      /* As the buttons driver picks it: first mapping, replaced by one
       * this EE (0) owns, so writes are never denied. */
      volatile UINT32 *Cnfg = (volatile UINT32 *)(UINTN)HOUJI_SPMI_CNFG;
      if ((Cnfg[I] & 7u) == 0) { return TRUE; }
    }
  }
  return *Apid >= 0;
}

STATIC
BOOLEAN
UsbSpmiRead8 (volatile UINT32 *Obs, INT32 Apid, UINT8 Address, UINT8 *Value)
{
  volatile UINT32 *Ch = Obs + (0x20 * (UINT32)Apid) / 4;
  Ch[0x00 / 4] = (1u << 27) | ((UINT32)Address << 4); /* EXT_READ: command at 0x00, status at 0x08 */
  for (UINTN N = 0; N < 100000; N++) {
    UINT32 Status = Ch[0x08 / 4];
    if (Status & 1u) {
      if (Status & 0xeu) { return FALSE; }
      *Value = (UINT8)Ch[0x18 / 4];
      return TRUE;
    }
  }
  return FALSE;
}

STATIC
BOOLEAN
UsbSpmiWrite8 (volatile UINT32 *Chn, INT32 Apid, UINT8 Address, UINT8 Value)
{
  volatile UINT32 *Ch = Chn + (0x1000 * (UINT32)Apid) / 4;
  Ch[0x10 / 4] = Value;
  Ch[0x00 / 4] = (UINT32)Address << 4; /* EXT_WRITE */
  for (UINTN N = 0; N < 100000; N++) {
    UINT32 Status = Ch[0x08 / 4];
    if (Status & 1u) { return (Status & 0xeu) == 0; }
  }
  return FALSE;
}

/* One rail through RPMh/CmdDb (same interface and validation as the touch
 * handoff): pmic5-ldo commands +0 voltage (uV), +8 mode (7=HPM), +4 enable. */
STATIC EFI_STATUS
UsbEnableRail (CONST CHAR8 *Name, UINT32 Microvolts)
{
  HJ_RPMH *Rpmh = NULL;
  HJ_CMDDB *Db = NULL;
  EFI_STATUS Status = gBS->LocateProtocol (&mProvidesRpmhDxe, NULL, (VOID **)&Rpmh);
  if (EFI_ERROR (Status)) { return Status; }
  Status = gBS->LocateProtocol (&mCmdDb, NULL, (VOID **)&Db);
  if (EFI_ERROR (Status)) { return Status; }
  if (Rpmh->Version != 0x10001 || Db->Version != 0x10000 || !Rpmh->Methods[1] ||
      !Rpmh->Methods[5] || !Rpmh->Methods[18] || !Rpmh->Methods[11] ||
      !Db->Methods[1] || !Db->Methods[7]) { return EFI_UNSUPPORTED; }
  /* Reuse the touch handoff's proven client when present; the rails are
   * additional votes through the same apps-RSC client. */
  if (!mUsbPowerClient && mTouchPowerClient) { mUsbPowerClient = mTouchPowerClient; }
  if (!mUsbPowerClient) {
    Status = ((HJ_CREATE)Rpmh->Methods[5])(2, "houji-usb-vdd", &mUsbPowerClient);
    if (EFI_ERROR (Status) || !mUsbPowerClient) { return EFI_NOT_READY; }
  }
  UINT32 Address = 0, Slave = 0;
  Status = ((HJ_QUERY)Db->Methods[1])(Db, Name, &Address);
  if (!EFI_ERROR (Status)) { Status = ((HJ_QUERY)Db->Methods[7])(Db, Name, &Slave); }
  Print (L"HOUJI USB RAIL: %a address=%x slave=%u query=%r\n", Name, Address, Slave, Status);
  if (EFI_ERROR (Status) || Slave != 4 || (Address >> 16) != 4 || (Address & 15) || !Address) {
    return EFI_UNSUPPORTED;
  }
  STATIC CONST UINT32 Offsets[3] = {0, 8, 4};
  CONST UINT32 Values[3] = {Microvolts, 7, 1};
  for (UINTN I = 0; I < 3; I++) {
    UINT32 Request = 0;
    Status = ((HJ_ISSUE)Rpmh->Methods[18])(mUsbPowerClient, 0, TRUE, Address + Offsets[I], Values[I], &Request);
    if (!EFI_ERROR (Status) && Request) { Status = ((HJ_WAIT)Rpmh->Methods[11])(mUsbPowerClient, Request); }
    if (EFI_ERROR (Status)) { return Status; }
  }
  Print (L"HOUJI USB RAIL: %a at %u uV ok\n", Name, Microvolts);
  return EFI_SUCCESS;
}

/* Program an RCG: CFG = src<<8 | (prediv*2-1) at cmd_rcgr+4, then UPDATE. */
STATIC
VOID
UsbRcgProgram (UINT32 CmdRcgr, UINT32 Src, UINT32 PreDiv)
{
  UsbHsWrite32 (HOUJI_GCC_BASE, CmdRcgr + 4, (Src << 8) | (PreDiv * 2 - 1));
  UsbHsWrite32 (HOUJI_GCC_BASE, CmdRcgr, BIT0);
  for (UINTN N = 0; N < 500; N++) {
    if (!(UsbHsRead32 (HOUJI_GCC_BASE, CmdRcgr) & BIT0)) { return; }
    gBS->Stall (10);
  }
}

/* Enable a CBCR (bit 0) and confirm it is running (bit 31 clear). */
STATIC
BOOLEAN
UsbBranchOn (UINT32 Off)
{
  UsbHsMask32 (HOUJI_GCC_BASE, Off, BIT0, BIT0);
  for (UINTN N = 0; N < 500; N++) {
    if (!(UsbHsRead32 (HOUJI_GCC_BASE, Off) & BIT31)) { return TRUE; }
    gBS->Stall (10);
  }
  return FALSE;
}

STATIC EFI_STATUS
PrepareUsbHandoff (VOID)
{
  EFI_STATUS Status;
  if (mUsbHandoffAttempted) { return EFI_ALREADY_STARTED; }
  mUsbHandoffAttempted = TRUE;
  /* PC-assisted boots (fastboot) already have the full ABL USB state;
   * leave it untouched. The kernel readback on this phone for that state
   * is gdscr bit31 set with master/cfg/aggre bit31 clear and GCTL
   * PRTCAPDIR=2 (0x00102001). */
  if ((UsbHsRead32 (HOUJI_GCC_BASE, HOUJI_GCC_USB_GDSCR) & BIT31) &&
      !(UsbHsRead32 (HOUJI_GCC_BASE, HOUJI_GCC_USB_MASTER) & BIT31) &&
      !(UsbHsRead32 (HOUJI_GCC_BASE, HOUJI_GCC_USB_CFG_NOC) & BIT31) &&
      !(UsbHsRead32 (HOUJI_GCC_BASE, HOUJI_GCC_USB_AGGRE) & BIT31) &&
      ((UsbHsRead32 (HOUJI_DWC3_BASE, DWC3_GCTL) >> DWC3_GCTL_PRTCL_SHIFT) & 3u) ==
        DWC3_GCTL_PRTCL_DEVICE) {
    Print (L"HOUJI USB UEFI: ABL state already present; handoff skipped\n");
    return EFI_SUCCESS;
  }

  /* 1. Rails (live-DT CmdDb names and init voltages). */
  STATIC CONST CHAR8 *CONST Rails[4] = {"ldoi1", "ldoi3", "ldob5", "ldob15"};
  STATIC CONST UINT32 RailMicrovolts[4] = {880000, 1200000, 3104000, 1800000};
  for (UINTN I = 0; I < 4; I++) {
    Status = UsbEnableRail (Rails[I], RailMicrovolts[I]);
    if (EFI_ERROR (Status)) { return Status; }
  }

  /* 2. USB30 PRIM GDSC: clear SW collapse, wait for power (bit 31). */
  UsbHsMask32 (HOUJI_GCC_BASE, HOUJI_GCC_USB_GDSCR, BIT0, 0);
  BOOLEAN Powered = FALSE;
  for (UINTN N = 0; N < 500; N++) {
    if (UsbHsRead32 (HOUJI_GCC_BASE, HOUJI_GCC_USB_GDSCR) & BIT31) { Powered = TRUE; break; }
    gBS->Stall (1000);
  }
  Print (L"HOUJI USB GDSC: gdscr=%08x powered=%u\n",
    UsbHsRead32 (HOUJI_GCC_BASE, HOUJI_GCC_USB_GDSCR), Powered);
  if (!Powered) { return EFI_DEVICE_ERROR; }

  /* 3. USB30 PRIM block reset pulse. */
  UsbHsWrite32 (HOUJI_GCC_BASE, HOUJI_GCC_USB_BCR, BIT0);
  gBS->Stall (20);
  UsbHsMask32 (HOUJI_GCC_BASE, HOUJI_GCC_USB_BCR, BIT0, 0);
  gBS->Stall (100);

  /* 4. TCSR usb2 reference clock gate (19.2 MHz bi_tcxo). */
  UsbHsMask32 (HOUJI_TCSR_BASE, HOUJI_TCSR_USB2_CLKREF, BIT0, BIT0);

  /* 5. RCGs: master = GPLL0/3 (src 1) for 200 MHz; mock_utmi = XO (src 0)
   * for 19.2 MHz; postdiv is read-only hardware. */
  UsbRcgProgram (HOUJI_GCC_USB_MASTER_RCG, 1, 3);
  UsbRcgProgram (HOUJI_GCC_USB_UTMI_RCG, 0, 1);

  /* 6. Branches: the kernel driver checks master/cfg/aggre explicitly. */
  if (!UsbBranchOn (HOUJI_GCC_USB_MASTER) ||
      !UsbBranchOn (HOUJI_GCC_USB_CFG_NOC) ||
      !UsbBranchOn (HOUJI_GCC_USB_AGGRE) ||
      !UsbBranchOn (HOUJI_GCC_USB_UTMI) ||
      !UsbBranchOn (HOUJI_GCC_USB_SLEEP)) {
    Print (L"HOUJI USB GCC: branch enable failed (master=%08x cfg=%08x aggre=%08x utmi=%08x)\n",
      UsbHsRead32 (HOUJI_GCC_BASE, HOUJI_GCC_USB_MASTER),
      UsbHsRead32 (HOUJI_GCC_BASE, HOUJI_GCC_USB_CFG_NOC),
      UsbHsRead32 (HOUJI_GCC_BASE, HOUJI_GCC_USB_AGGRE),
      UsbHsRead32 (HOUJI_GCC_BASE, HOUJI_GCC_USB_UTMI));
    return EFI_DEVICE_ERROR;
  }
  Print (L"HOUJI USB GCC: master=%08x cfg=%08x aggre=%08x utmi=%08x\n",
    UsbHsRead32 (HOUJI_GCC_BASE, HOUJI_GCC_USB_MASTER),
    UsbHsRead32 (HOUJI_GCC_BASE, HOUJI_GCC_USB_CFG_NOC),
    UsbHsRead32 (HOUJI_GCC_BASE, HOUJI_GCC_USB_AGGRE),
    UsbHsRead32 (HOUJI_GCC_BASE, HOUJI_GCC_USB_UTMI));

  /* 7. QUSB2PHY_PRIM reset pulse. */
  UsbHsWrite32 (HOUJI_GCC_BASE, HOUJI_GCC_QUSB2PHY_BCR, BIT0);
  gBS->Stall (20);
  UsbHsMask32 (HOUJI_GCC_BASE, HOUJI_GCC_QUSB2PHY_BCR, BIT0, 0);
  gBS->Stall (100);

  /* 8. PM8550B repeater: enable, this phone's tune table, device mode. */
  INT32 Apid = -1;
  if (!UsbSpmiFindApid (&Apid)) {
    Print (L"HOUJI USB RPTR: no APID for PPID %x\n", HOUJI_RPTR_PPID);
    return EFI_NOT_FOUND;
  }
  volatile UINT32 *Chn = (volatile UINT32 *)(UINTN)HOUJI_SPMI_CHNLS;
  volatile UINT32 *Obs = (volatile UINT32 *)(UINTN)HOUJI_SPMI_OBSRVR;
  /* device mode: clear the host 19.2 MHz force workaround */
  if (!UsbSpmiWrite8 (Chn, Apid, 0xe8, 0) || !UsbSpmiWrite8 (Chn, Apid, 0xed, 0)) {
    return EFI_DEVICE_ERROR;
  }
  if (!UsbSpmiWrite8 (Chn, Apid, HOUJI_RPTR_EN_CTL1, 0x80)) { return EFI_DEVICE_ERROR; }
  /* This phone's qcom,param-override-seq (live DT, device mode): reg, val. */
  STATIC CONST UINT8 Tune[][2] = {
    {0x50, 0x00}, {0x51, 0x0f}, {0x52, 0x04}, {0x53, 0x07},
    {0x54, 0x03}, {0x55, 0x01}, {0x56, 0x03}, {0x57, 0x01},
    {0x58, 0x02}, {0x59, 0x01}, {0x5a, 0x03}, {0x5b, 0x02},
  };
  for (UINTN I = 0; I < ARRAY_SIZE (Tune); I++) {
    if (!UsbSpmiWrite8 (Chn, Apid, Tune[I][0], Tune[I][1])) { return EFI_DEVICE_ERROR; }
  }
  UINT8 RptrStatus = 0;
  BOOLEAN RptrOk = FALSE;
  for (UINTN N = 0; N < 500; N++) {
    if (UsbSpmiRead8 (Obs, Apid, HOUJI_RPTR_STATUS, &RptrStatus) && (RptrStatus & 0x80)) {
      RptrOk = TRUE; break;
    }
    gBS->Stall (10);
  }
  Print (L"HOUJI USB RPTR: ok=%u status=%02x\n", RptrOk, RptrStatus);
  if (!RptrOk) {
    /* Non-fatal: finish the PHY and GCTL so the kernel driver always sees
     * a complete, consistent controller state; it re-checks everything. */
    Print (L"HOUJI USB RPTR: status not OK; continuing with PHY init\n");
  }

  /* 9. eUSB2 HS PHY init, qcom_snps_eusb2_hsphy_init() order, 19.2 MHz. */
  UsbHsMask32 (HOUJI_HSPHY_BASE, HSPHY_CFG0, BIT1, BIT1);                        /* CMN_CTRL_OVERRIDE_EN */
  UsbHsMask32 (HOUJI_HSPHY_BASE, HSPHY_UTMI_CTRL5, BIT1, BIT1);                  /* POR */
  UsbHsMask32 (HOUJI_HSPHY_BASE, HSPHY_CTRL_COMMON0, BIT0 | BIT3, BIT0 | BIT3);  /* PHY_ENABLE|RETENABLEN */
  UsbHsMask32 (HOUJI_HSPHY_BASE, HSPHY_APB_ACCESS_CMD, BIT2, BIT2);              /* APB_LOGIC_RESET */
  UsbHsMask32 (HOUJI_HSPHY_BASE, HSPHY_CMN_CTRL0, BIT6, 0);                      /* TESTBURNIN off */
  UsbHsMask32 (HOUJI_HSPHY_BASE, HSPHY_FSEL_SEL, BIT0, BIT0);                    /* FSEL_SEL */
  UsbHsMask32 (HOUJI_HSPHY_BASE, HSPHY_CTRL_COMMON0, 0x70, 0);                    /* FSEL 19.2 MHz */
  UsbHsMask32 (HOUJI_HSPHY_BASE, HSPHY_CFG_CTRL_2, 0xff, 0x90);                  /* fb div 7:0 */
  UsbHsMask32 (HOUJI_HSPHY_BASE, HSPHY_CFG_CTRL_3, 0x0f, 0x1);                   /* fb div 11:8 */
  UsbHsMask32 (HOUJI_HSPHY_BASE, HSPHY_CFG_CTRL_3, 0xf0, 0);                     /* PLL_REF_DIV */
  UsbHsMask32 (HOUJI_HSPHY_BASE, HSPHY_CFG_CTRL_1, 0xfe, 0);                     /* CPBIAS */
  UsbHsMask32 (HOUJI_HSPHY_BASE, HSPHY_CFG_CTRL_4, 0xfc, 0x8 << 2);              /* PLL_INT 0x8 */
  UsbHsMask32 (HOUJI_HSPHY_BASE, HSPHY_CFG_CTRL_4, 0x03, 0x1);                   /* PLL_GMP 0x1 */
  UsbHsMask32 (HOUJI_HSPHY_BASE, HSPHY_CFG_CTRL_5, 0x1f, 0x10);                  /* PLL_PROP 0x10 */
  UsbHsMask32 (HOUJI_HSPHY_BASE, HSPHY_CFG_CTRL_6, 0x07, 0);                     /* PLL_VCO */
  UsbHsMask32 (HOUJI_HSPHY_BASE, HSPHY_CFG_CTRL_5, 0xc0, 0x1 << 6);              /* PLL_VREF 0x1 */
  UsbHsMask32 (HOUJI_HSPHY_BASE, HSPHY_CTRL_2, BIT4, BIT4);                      /* VBUS_DET_EXT_SEL */
  UsbHsMask32 (HOUJI_HSPHY_BASE, HSPHY_CFG_CTRL_9, 0x07, 0);                     /* TX_PREEMP 0 */
  UsbHsMask32 (HOUJI_HSPHY_BASE, HSPHY_CFG_CTRL_9, 0x60, 0x2 << 5);              /* TX_RISE 2 */
  UsbHsMask32 (HOUJI_HSPHY_BASE, HSPHY_CFG_CTRL_9, 0x18, 0x1 << 3);             /* TX_RES 1 */
  UsbHsMask32 (HOUJI_HSPHY_BASE, HSPHY_CFG_CTRL_8, 0x38, 0x3 << 3);              /* TX_HS_VREF 3 */
  UsbHsMask32 (HOUJI_HSPHY_BASE, HSPHY_CFG_CTRL_8, 0xc0, 0);                     /* TX_HS_XV 0 */
  UsbHsMask32 (HOUJI_HSPHY_BASE, HSPHY_CTRL_2, BIT3 | BIT2, BIT3 | BIT2);        /* SUSPEND_N_SEL|SUSPEND_N */
  UsbHsMask32 (HOUJI_HSPHY_BASE, HSPHY_UTMI_CTRL0, BIT0, BIT0);                  /* SLEEPM */
  UsbHsMask32 (HOUJI_HSPHY_BASE, HSPHY_CTRL_COMMON0, BIT1, BIT1);                /* SIDDQ_SEL */
  UsbHsMask32 (HOUJI_HSPHY_BASE, HSPHY_CTRL_COMMON0, BIT2, 0);                    /* SIDDQ off */
  UsbHsMask32 (HOUJI_HSPHY_BASE, HSPHY_UTMI_CTRL5, BIT1, 0);                     /* POR release */
  UsbHsMask32 (HOUJI_HSPHY_BASE, HSPHY_CTRL_2, BIT3, 0);                         /* SUSPEND_N_SEL off */
  UsbHsMask32 (HOUJI_HSPHY_BASE, HSPHY_CFG0, BIT1, 0);                           /* OVERRIDE_EN off */
  Print (L"HOUJI USB HSPHY: init complete (19.2 MHz)\n");

  /* 10. DWC3 in device mode: the fastboot-left state (kernel readback on
   * this phone: gctl=0x00102001, PRTCAPDIR=2). RUN stays off; the kernel
   * driver performs its own halt/reset/run sequence. */
  UsbHsMask32 (HOUJI_DWC3_BASE, DWC3_GCTL, 3u << DWC3_GCTL_PRTCL_SHIFT,
    DWC3_GCTL_PRTCL_DEVICE << DWC3_GCTL_PRTCL_SHIFT);
  Print (L"HOUJI USB: standalone handoff complete (gctl=%08x)\n",
    UsbHsRead32 (HOUJI_DWC3_BASE, DWC3_GCTL));
  return EFI_SUCCESS;
}
#endif
