# houji peripherals: state and next steps

Device facts come from the handset's own devicetree, the SteamOS houji port and mainline Linux; code borrowed from UniXNU-sheng
(SM8550, same PMICs and Wi-Fi chip) is credited in each file.

## Battery and charging — working
`iokit/Kernel/houji-adsp/`: the stock ADSP firmware (read in place from `modem_a`) runs the battery manager; PMIC GLINK feeds an
`IOPMPowerSource`. On a USB-PD supply the host ends Xiaomi's adapter verification (board property 20 = 0), which lifts the 100 mA input
limit; the battery current vote is `houji_fcc_ma` (default 1000, 500 mA outside 10–40 °C, 0 = leave the firmware alone).

## Wi-Fi — working (WCN7850 on PCIe0)
`iokit/Kernel/houji-wifi/`, module built by `tools/build-ath12k.py` from UniXNU/Ath12kWifi plus `kernel/ath12k-module/*.patch`.
On by default (`houji_wifi=0` off). Verified: scan, WPA2 join on 5 GHz (80 MHz) and 2.4 GHz from Settings, DHCP, default route, DNS,
Safari loading a page by name.
- Firmware: stock Kiwi v2 files from `modem_a:/image/kiwi` (amss20, phy_ucode20, bd_n3gl, regdb_xiaomi); MAC from the chip ID.
- DMA: a 48 MiB buffer behind a stage 1 SMMU table on stream 0x1401 when no memory below 4 GiB is free.
- `/arm-io/wlan` is added so launchd starts wifid.
- Root image: `WiFiManager/LinkUpEventAfter4way` must be off (`unixnu-root/add-wifi-flags.py`); with it wifid holds the link-up
  waiting for a supplicant event this driver does not send.
- Packets are tapped into BPF in both directions; without it IPConfiguration's router ARP check never succeeds and Wi-Fi is never
  made primary (no default route, no DNS).
- Known gaps: the network password is not kept across reboots (keychain); the firmware dozes unless power save is turned off (done
  after each join); a heartbeat HTT stats request crashed this firmware (removed).
- USB console: `wifi scan | join SSID PASS | leave | status`; `type TEXT` (`\n` = Return) types through a virtual keyboard.

## Display power and brightness — working
`houji-buttons/HoujiScreen.cpp` owns the panel's DSI0 command FIFO (`0x0ae94000`, lonzzi/UniXNU-houji's HoujiBacklightProtocol.h).
- **Display power follows iOS.** backboardd calls IOMobileFramebuffer selector 12 with 0 on sleep and 1 on wake (seen on every
  power-key press and lock); `SweetFramebuffer.cpp` forwards it. Off: brightness 0, DCS `0x28`; on: DCS `0x29`, then the last
  non-zero level. `houji_panel_sleep=1` adds sleep in/out (`0x10`/`0x11`, 120 ms), untested on the panel. A touch on a dark screen
  does nothing, as on iOS (which also ignores it); only the power key wakes.
- **Brightness.** `HoujiBacklight` answers `io_object_conforms_to(..., "AppleARMBacklight")` and accepts backboardd's
  `IODisplayParameters` writes (upstream's IOUserClient.cpp hooks in `patches/local.patch`); levels go to the same thread. The
  Settings slider drove 0..2866 with no DSI failures. The panel's boot level is read back with DCS `0x52` (307 here).
- **Auto-lock.** iOS's idle timer never fires on this port (Auto-Lock 1 minute: no dim or display-off request arrives). Fallback:
  after `houji_autolock` seconds without a touch (default 60, 0 off) the kernel presses the power key, and iOS locks itself.
  Open: why the idle timer does not run.
  Before the first touch after a wake (the lock screen) the wait is `houji_autolock_lockscreen` (default 10 s), as on iOS: the
  lock screen's "swipe up" hint bounces the home indicator at 120 Hz, a full-screen CPU copy per frame here (about +0.5-1 W).
- Verified on the phone: power-key sleep, taps and swipes on the dark screen (stayed off), power-key wake, swipe to unlock (home
  screen), automatic lock after 60 s, slider.
- Not done: the panel's refresh rate (it keeps the stock idle-refresh mode from the boot firmware; no 1-120 Hz/LTPO control), and
  powering the touch controller down while the display is off.

## Idle power
Measured as USB input minus battery charge power (charger readings), screen off, nothing drawing:

| Boot line | Phone draw | CPU temperature | single-sample peaks |
|---|---|---|---|
| `wfi=0` (the inherited base boot line) | 3.2-3.5 W | about 60 C | 82-85 C |
| `cpus=2 wfi=0` (six cores never started) | 2.6-2.9 W | | |
| `wfi=1` (now the default) | 2.5-2.8 W | about 52 C | 54-62 C |

`wfi=0` made XNU's `cpu_idle` skip the WFI instruction, so every idle core spun in the idle loop. Upstream r244 had already run
`wfi=1` on all eight cores without missed wakeups; here wake, unlock, touch and Settings all worked with it.
The remaining floor includes charger conversion loss. Not yet done: CPU power collapse in idle (PSCI CPU_SUSPEND; only WFI now),
GPU power collapse, DDR/CX votes left by the boot firmware, copying only the changed area of a frame, and system sleep.

## GPU cache coherence and interrupts (ported from lonzzi/UniXNU-houji 99beb10, 2026-11-03)
- **Coherence.** Each boot (runtime mode 4, `houji_gpu_coherent=1` default) runs upstream's two-way CPU/GPU coherence proofs (two
  copies, 8 write-back renders, CRC e843b26a) and then enables `coherent=2`: write-back inner-shareable GPU mappings skip CPU cache
  maintenance; NC imports are still maintained. A failed proof with an idle GPU continues with coherent=0 (upstream panics).
  Measured: per-submit cache publish 10 us average / 36 us max (before 200-600 us / 2-5 ms). Desktop, Settings and scrolling render
  correctly; no SMMU/GPU faults. Also taken: guarded dispatch (GPU waits outside the issuer lock, replacing our pipelined gate,
  which upstream considers unsafe with non-coherent maintenance), completion wakeups, buffer pool, 128-entry internal submit and
  the batch ABI (our root's userland keeps the 32-buffer submit), no per-submit TLBIALL, 4 GiB GPU address layout.
  Kept ours: GPU frequency governor and TSENS caps, the frame copy worker, IOSurface dirty tracking.
- **Interrupts.** `pexpert/arm/HoujiSpi` (GIC SPI routing) with a dispatch hook in `sleh_irq`. IPCC (INTID 261, ADSP/modem
  doorbells) and PCIe0 MSI (INTID 173, Wi-Fi) are real level interrupts: about 60 and 70 per second while Wi-Fi scans, battery and
  Wi-Fi work, and the old 2 ms poller thread no longer starts. The RSC line (INTID 37) froze the boot as a GIC interrupt, so vote()
  polls its TCS status itself. `houji_spi=0` restores polling; `houji_spi_mask` (bit 0 IPCC, 1 RSC, 2 MSI; default 5) selects lines.
- Idle with the screen off after both: 2.4-2.5 W, CPU about 47.5 C (wfi=1 alone: 2.5-2.8 W, about 52 C).

## Buttons — implemented, not yet pressed on hardware
`iokit/Kernel/houji-buttons/HoujiButtons.cpp`: power (PMK8550 PON pwrkey), volume down (resin), volume up (PM8550 GPIO 6) read
every 20 ms through the SPMI observer (read only) and sent to SweetHID. Verified: both peripherals found, no read errors, idle state
reads released. `houji_buttons=0` turns it off.

## GPS — started
GNSS runs in the modem (MPSS). Done: the stock modem image is read from `modemfirmware_a` and placed against the devicetree
carveouts without starting anything (`HOUJI MODEM: stage=3`, 162 MB, 36 segments, relocatable, span = the 0xf400000 mpss region).
The modem runs (`houji_modem=2`, opt-in; `HoujiAdsp.cpp` "the modem (MPSS)" and "GNSS"):
- start: SMP2P 435/428 and the MPSS doorbells, AOSS `load_state modem`, CX/MSS votes and a CE0 bus vote, SCM MP assign of the DSM and
  qlink regions to MSS_MSA and of the rmtfs buffer (0xd7c00000) to HLOS + MSA, the dtb (PAS 0x26) then the modem (PAS 4). Verified:
  ready about 1.05 s after the start, GLINK and QRTR up, 52 services announced (LOC 0x10 among them).
- The PAS metadata must lie below 4 GiB (Xiaomi's driver: dma_phys_below_32b) in normal non-cacheable memory and must not be in an SHM
  bridge of our own. This kernel's RAM is above 4 GiB, so the metadata goes to fixed pages at 0xd7a00000 / 0xd7a10000 (unused RAM
  outside the UEFI map). Above 4 GiB the secure world answered 0xffcfffbb; with our own bridge 0xffcffff6.
- rmtfs (QMI service 14): `/boot/modem_fs1`, `fs2`, `fsg`, `fsc` served from RAM copies of modemst1/2, fsg, fsc; iovec buffer
  fields are physical addresses. Verified: 4 MiB read, 8 MiB written (kept in RAM, never written to UFS); no modem crash.
- LOC client: register (stock fields HAL/1/0), 0x3b (the engine answers lock ALL), 0xbf, 0x36, SET_ENGINE_LOCK none (0x3a), START
  (0x22) — all accepted. No position or satellite reports follow yet.
Next: the QRTR TFTP file service for the modem's MCFG requests before MPSS starts (SteamOS: needed before satellites were reported),
then time injection once the iOS clock is network-synced, then the iOS side.
Remaining steps once the modem starts (the SteamOS port shows each is required):
1. rmtfs: a QMI server (QRTR) giving the modem its EFS partitions `modemst1`, `modemst2`, `fsg`, `fsc`; writes kept in RAM first.
2. A QRTR file server for the modem's MCFG requests (SteamOS: `tqftpserv-mbnconfig.patch`).
3. Start MPSS: PAS 4 (+ dtb), the DSM regions 0x86b00000 + 0x4900000 and 0x8b400000 + 0x800000 (not in the UEFI memory map, so not
   OS RAM), CX/MX votes, AOSS `modem` load state, SMP2P stop state, GLINK edge remote pid 1 (IPCC client 2), QRTR on `IPCRTR`.
   SteamOS saw a phone reset when MPSS was restarted within one boot: start it once per boot.
4. QMI LOC client (SteamOS `gps/houji-loc-test.c`: hold one QRTR client for the session, XTRA/UTC assistance).
5. iOS: feed positions to locationd (no Qualcomm GNSS path exists in iOS).

## Cameras — all four sensors powered and identified
No reference implementation: SteamOS lists cameras as not implemented, UniXNU-sheng has none, mainline has only camcc and CCI for
SM8650 (no ISP driver). `houji-camera/HoujiCamera.cpp`, boot argument `houji_camera=1` (off by default; runs 70 s into the boot and
powers each camera down after its scan). USB console: `camera probe` (all) or `camera probe N` (cell N).

Layout from the stock dtbo (fragments 67-70) and the base dtb in `vendor_boot` (pin groups):

| Cell | Camera | Bus (SDA/SCL GPIO) | MCLK GPIO, camcc RCG/branch | Reset | vio / vana / vdig |
|---|---|---|---|---|---|
| 0 | wide | CCI0 master 1 (115/116) | 100, `0x15000`/`0x15018` | 108 | L10B 1.8 V / GPIO 127 switch / L2M 1.25 V |
| 1 | front | QUP2 SE1 I2C (4/5) | 104, `0x15070`/`0x15088` | 7 | L5M 1.8 V / L3M 2.8 V / L1M 1.1 V |
| 2 | ultra-wide | CCI1 master 0 (117/118) | 101, `0x1501c`/`0x15034` | 109 | L10B / GPIO 126 switch / L2C 1.104 V |
| 3 | tele | CCI0 master 0 (113/114) | 103, `0x15054`/`0x1506c` | 110 | L10B / GPIO 125 switch / L1E 1.104 V |

Verified on the phone (read-only probe, 19.2 MHz MCLK from XO):

| Cell | Sensor answers | ID read | Other devices |
|---|---|---|---|
| 0 wide | `0x10`, `0x21` | OmniVision `56 50 46` at `0x300a-0x300c` | `0x51`, `0x59` (EEPROM-like) |
| 1 front | `0x10`, `0x21` | OmniVision `56 32 42` (OV32B) | `0x51`, `0x59` (EEPROM-like) |
| 2 ultra-wide | `0x3d` | `38 e1` at `0x0000-0x0001` (Samsung S5KJN1) | `0x50` (EEPROM-like) |
| 3 tele | `0x2d` | `38 e1` (Samsung S5KJN1) | `0x54` |

Power sequence: BOB2 (3.296 V auto; PM8010 L3/L4 input, left on), GCC camera AHB/XO, Titan top GDSC (camcc `0x132bc`), then per
camera vio, vana, vdig, MCLK, reset released. L10B is shared by the rear cameras and left on. GPIOs 4, 5 and 7 have an island (egpio)
function and boot island-owned (config `0x801`): writes only reach them with EGPIO_ENABLE (bit 12) set. I2C is bit-banged at about
100 kHz.

Next: streaming, which needs sensor mode tables (the stock vendor sensor libraries), CSIPHY, CSID and IFE programming, and no public
driver covers this chip; a first target is one raw frame from the front OV32B into memory.
