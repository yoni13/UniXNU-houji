# Standalone USB reset review — 2026-11-05

## Device evidence

The CRC-valid durable XNU snapshot from the failed boot contains reports
through 140 seconds. It does not establish a successful desktop boot:
HFS repeatedly reports `cannot mount dirty non-journaled volumes`, followed
by mount failure 22 and root errno 19. Desktop submit and present counts
remain zero. The cause of the dirty filesystem is not established by this log.

USB reports show DWC3 ID `33313130`, initial DCTL `00f00000`, and a reset
timeout at DCTL `40000000`. The attempted USB2 SUSPHY correction reports
`0->0`. Thus USB2 suspend is not an established explanation for this failure.
The driver aborts before programming its event buffer or starting the gadget.
The previous implementation nevertheless installed a SID40 route before reset.

## Review changes awaiting device validation

- Defer installing a previously faulting USB SMMU stream until reset completes
  and HALTED/RUN readbacks confirm the controller is stopped.
- Preserve DCTL configuration when requesting reset, masking link-state change
  requests on read-modify-write, as in Linux v6.12
  `dwc3_core_soft_reset` and `dwc3_gadget_dctl_write_safe`.
- Refuse RUN if RESET has become asserted again; do not clear RESET in software
  as a side effect of starting the controller.
- Retain GCTL, DCTL, DSTS, USB2 PHY configuration and USB3 PIPE control values
  before halt, before reset and on reset timeout in the durable USB notes.

References: Linux v6.12 `drivers/usb/dwc3/core.c`, `gadget.h` and `core.h`.
These changes are review hardening and diagnostics, not verified standalone
PHY bring-up. No clock, regulator or external PHY programming is added.

## Remaining validation

Check and repair the unmounted HFS root with a suitable filesystem checker
and retained recovery evidence before expecting desktop boot. Do not merely
clear its dirty flag. Then build and test the reviewed kernel, inspect the
new reset snapshots, and establish the missing PHY/clock prerequisites from
board evidence before adding power sequencing. CI success alone does not
demonstrate USB enumeration or a clean root mount.

## Vendor bring-up reference (2026-11-05, commit 258038b and later)

The phone's own ABL stack was decoded and the USB bring-up extracted
(artifacts retained in `run-logs/abl-analysis/`):

- `abl_a.img` → FV → LZMA → LinuxLoader PE. Fastboot app only; no
  controller registers touched directly (uses protocols).
- `uefi_a.img` → DXE FV (gzip) → `UsbConfigDxe` contains the platform
  bring-up: `SSUsb1InitCommon`, the reset sequence (master-clock reset
  pulse, QUSB2PHY/USB3 PHY BCR assert-release in order), and the
  hardcoded eUSB2 HS PHY register program at 0x88e3000.
- `UsbfnDwc3Dxe` (`UsbfnDwcCoreInitImpl`) runs CSFTRST only after that
  platform bring-up, then event buffer, endpoints, DCFG/DEVTEN/DALEPENA.
- `xbl_config_a.img` DTB: `hs_phy_cfg` list verified empty on this phone
  (three 0xffffffff skip entries then stop); `ss_phy_cfg` holds 168
  register pairs for the SuperSpeed PHY (not needed for HS CDC ACM).

The kernel driver now ports that exact sequence, guarded to run only
when DCTL.CSFTRST is already stuck on the standalone path. GCC offsets
from torvalds `drivers/clk/qcom/gcc-sm8650.c` (base 0x100000):
USB30_PRIM_BCR 0x39000, master CBCR 0x39018, QUSB2PHY_PRIM_BCR
0x12000, USB3_PHY_PRIM_BCR 0x50000, USB3PHY_PHY_PRIM_BCR 0x50004,
USB3_DP_PHY_PRIM_BCR 0x50008. After the block reset, GCTL.PRTCAPDIR
must be re-asserted to device mode before core init continues.
