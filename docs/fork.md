# yoni13 fork of UniXNU-houji

This fork runs iOS 16 (20H392 userland) on the Xiaomi 14 (`houji`, SM8650) with its own kernel work: Wi-Fi, battery
charging, buttons, display power and brightness, idle power, GPU coherence, real interrupts, and modem/GPS and camera bring-up.
Per-peripheral status, measurements and boot arguments are in [peripherals.md](peripherals.md).

## Upstream and sources

- **Upstream: [lonzzi/UniXNU-houji](https://github.com/lonzzi/UniXNU-houji).** The fork starts at upstream `99beb10`
  (2026-11-02). UEFI, DarwinBoot, the userland tooling, the graphics userland, the archive and the Chinese documentation
  (README.md and the other files in docs/) are upstream's; read them for everything not described here.
- The kernel tree under `kernel/` comes from upstream snapshot `c4d962f`, with this fork's drivers and fixes. Parts of later
  upstream work were merged back on 2026-11-03: GPU cache coherence with its boot-time self-test, guarded GPU dispatch,
  completion wakeups, the buffer pool, the batch submission ABI, the 4 GiB GPU address layout, GIC SPI routing (`HoujiSpi`),
  the brightness service design (`HoujiBacklightProtocol.h`), and the system-app and TCC policies in `sweet_amfi.c`.
- Other sources, credited in the files that use them:
  - [UniXNU](https://unixnu.com/) and UniXNU-sheng: XNU port structure, ADSP/QRTR/GLINK clients, the battery manager.
  - [UniXNU/Ath12kWifi](https://github.com/UniXNU/Ath12kWifi): the Wi-Fi driver module, built by `tools/build-ath12k.py` with the
    patches in `kernel/ath12k-module/`.
  - The SteamOS (Armada) port for houji: modem start sequence, LOC client order, TFTP/mbnconfig mapping.
  - Linux mainline drivers (pinctrl, camcc, gcc, rpmh, ath12k) and linux-msm tqftpserv: register layouts and protocols.

## What differs from upstream

| Area | This fork |
|---|---|
| Wi-Fi | WCN7850 over PCIe through the Ath12kWifi module: scan, WPA2 join from Settings, DHCP, DNS, Safari. Root step `tools/root/add-wifi-flags.py`. |
| Battery and charging | Own ADSP battery-manager client with USB-PD charge control, thermal current limits, and no charging when full and above 40 C. |
| Buttons | Power and volume keys by read-only SPMI polling, delivered as HID. |
| Display | iOS's display power (IOMobileFramebuffer selector 12) drives the panel (DCS 0x28/0x29); iOS brightness through an AppleARMBacklight-compatible service; a touch on a dark screen no longer lights it. |
| Idle power | `wfi=1` (cores stop in WFI instead of spinning), a real interrupt for IPCC (ADSP/modem doorbells) instead of a 2 ms poller, a 10 s lock-screen timeout. Screen-off draw went from 3.3 W to about 2.5 W. |
| GPU | Upstream's coherence merged into this kernel; per-submit cache maintenance 10 us instead of 200-600 us. |
| Modem and GPS | Opt-in modem start (`houji_modem=2`), rmtfs and TFTP servers, QMI LOC client. No position fix yet. |
| Cameras | Opt-in probe (`houji_camera=1`) powers all four sensors and reads their chip IDs. No streaming yet. |
| Thermal | Repeated sensor spikes count as heat; CPU and GPU caps by TSENS level. |
| USB console | Commands for taps, swipes, typing, Wi-Fi, screen, power key, camera probe and logs (`kernel/overlay/bsd/dev/houji_usb_debug.c`). |
| Standalone boot | The installed image prepares its own run state: a fresh UEFI transcript, a pristine `XnuLog.bin` (CRC-verified against the kernel constant), and the exact no-wipe recovery BCB armed in-image at LBA 1024. A power-on boot from `boot_a` needs no host-side `dd`/`rm`. |

Not taken from upstream: its battery/ADSP stack (this fork has its own), its CPU/GPU governors (including X4 boost
frequencies), its thermal policy, display-timing diagnostics, network-stack stub, sample cache and newer `MASTER.arm64.sweet`
(`no_kextd`). Their host checks are removed from `tools/run-host-checks.py`; `checks/display-queue.py` is adapted to this
kernel's copy worker and `checks/battery-spi.c` also covers the Wi-Fi MSI line.

## Building

Continuous kernel builds run in [yoni13/houji-xnu-build](https://github.com/yoni13/houji-xnu-build) (macOS runner, pinned
blacktop/darwin-xnu-build and XNU 8796.141.3). Building by hand follows the same steps:

1. `python3 tools/prepare-sources.py kernel --upstream <xnu-8796.141.3> --output <tree>`.
2. Generate this phone's private inputs into `<tree>/iokit/Kernel/houji-ufs-root/` (never commit them):
   - GPU firmware headers with `tools/make-firmware-headers.py` from your own phone's partitions (`kernel/firmware-inputs.json`
     lists the expected hashes);
   - `HoujiLogAllocation.h` and `HoujiZapTrial.h`, the log file allocation on your phone's log filesystem and the matching
     GPU handoff identity (both carry the same 16-byte run ID; examples in `archive/native/`);
   - if your phone's GPT differs from the pinned one (`HoujiRootPolicy.h`, `HoujiGptCheck.h`, UEFI `LogFsTest.h`), the
     values read from your phone.
3. Build the Wi-Fi module: `python3 tools/build-ath12k.py <Ath12kWifi checkout> out/ath12k`, then set
   `HOUJI_EXTRA_OBJS=out/ath12k/houji_ath12k.o` and add those objects to the iokit link list (see the build repository's workflow).
4. `tools/build-kernel.sh` with `HOUJI_XNU`, `HOUJI_KDK`, `HOUJI_FAKEROOT` and `HOUJI_BUILD_ROOT` set.

The UEFI boot line in `uefi/overlay/.../HoujiStorageProbe.c` is the one this kernel runs with (`houji_gpu_runtime=4`, the
unbounded desktop; mode 3 is a bounded test that stops after a few minutes).

## Root image steps

Applied to upstream's root image with libguestfs (`tools/root/`): `install-metal.py` (Metal bundle), `add-nodeep.py` (8-bit
framebuffer), `add-wifi-flags.py` (WiFiManager `LinkUpEventAfter4way` off). `add-nodim.py` disables SpringBoard's auto-dim and
lock; it predates working display power and is being removed (with it set, iOS never locks the screen by itself).

## Device-specific data

Nothing here identifies a particular phone: GPT identities, log allocations, firmware and partition contents stay local.
Hardware claims in peripherals.md come from tests on one Xiaomi 14 (OS3.0.303.0.WNCTWXM firmware).

## Root-image changes installed on the test phone (2026-11-04)

- Restored all 32 original Apple apps from `/private/var/staged_system_apps` to `/Applications`; installed the one-shot
  `houji-system-apps` Launch Services registrar and its launchd cache entry. The kernel AMFI/TCC policy is in this repository.
- Built `/usr/lib/HoujiKeyboardGeometry.dylib`; added its absolute weak load command to 189 APPL main executables and ad-hoc
  re-signed them with their original entitlements. This implements the upstream keyboard and status-bar geometry rules.
- Rebuilt `NativeAdrenoMetal.bundle` with `NativeDeviceLimits.h`, which publishes the measured 2D texture width/height in
  `_MTLDevice`'s limits record for Photos and other direct readers.
- Every write was a 4 KiB block patch to the offline-pulled HFSX image, immediately read back from UFS; the resulting HFS+
  volume passes `fsck.hfsplus` (0). Runtime verification is pending a complete power-off: repeated warm resets leave UEFI's GPU
  power handoff not ready before userland.

## Feature defaults

Stable hardware support is enabled in code, not by a growing UEFI argument list. The normal boot line selects the root/diagnostic
profile; individual features use explicit `=0` opt-outs:

- GPU runtime desktop mode 4, external WB/NC canary qualification, same-boot coherence qualification: on by default
  (`houji_gpu_runtime=0`, `houji_gpu_external_canary_probe=0`, `houji_gpu_external_nc_probe=0`, or `houji_gpu_coherent=0` to disable).
- Eight-core SMP bound-worker probe: on (`houji_smp_probe=0` disables only its diagnostic worker).
- IPCC GIC interrupt: on; RSC and Wi-Fi MSI stay polled (`houji_spi=0` restores all polling; `houji_spi_mask` overrides the line mask).
- Wi-Fi, buttons, display power/brightness and the 60 s/10 s screen locks: on (`houji_wifi=0`, `houji_buttons=0`,
  `houji_screen=0`, `houji_autolock=0` opt out). Camera streaming probes and modem start remain opt-in.
