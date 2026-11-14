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
- USB console: `wifi scan | join SSID PASS | leave | status`; `type TEXT` (`\n` = Return, at most 58 characters: a console line is 64 bytes) types through a virtual
  hardware keyboard made for each run and removed after it (since 2026-11-10; before, it stayed attached and iOS hid the on-screen
  keyboard for the rest of the boot).

## Bluetooth — working: pairing and connection verified (WCN7850 on QUP2 SE6 UART)
`iokit/Kernel/houji-bt/HoujiBluetooth.cpp`, `houji_bt=1` (default off; with it the service is published at the root mount, `houji_bt_early`, default 1). USB console `bt probe`, `bt init` (without `houji_bt`),
`bt stat`. Wiring from the SteamOS houji port: uart14 = QUPv3 wrapper 2 SE6 (`0x898000`), GPIO 24-27, BT_EN GPIO 17 on the WCN7850
PMU whose rails the Wi-Fi driver turns on (the boot thread retries every 5 s until the controller answers).
- Verified on the phone: Settings shows Bluetooth "On"; its page scans (HCI Inquiry repeated, Extended Inquiry Results and LE
  advertising reports arrive); bluetoothd stays up ("Stack Did Start", "Power state changed: PoweredOn").
- Verified pairing with a Linux PC (Intel AX210, BlueZ): it appears under Other Devices, Secure Simple Pairing with numeric
  comparison (the same 6-digit code on both sides), bonded on both; after a reboot the phone reconnects with the stored key;
  Settings lists it under My Devices as Connected; AVDTP opens an A2DP stream (AAC 44.1 kHz) and BlueZ creates the audio card.
- Skywalk: the stand-in sandbox policy (`sweet_amfi.c`) now grants Skywalk privileges for their `com.apple.private.skywalk.*`
  entitlements; before, `__nexus_register` failed with EPERM and bluetoothd crashed on a NULL channel when an A2DP stream opened.
- Kernel: rampatch `image/hmtbtfw20.tlv` and NVM `image/hmtnv20.bin` from the stock `bluetooth_a` partition, 3.2 Mbaud, Qualcomm
  in-band sleep handled in the bridge; the received stream is framed by H4 lengths.
- Published for iOS: IOService `bluetooth` (also /arm-io/bluetooth: `local-mac-address`, `transport-encoding` 1 = H4,
  `transport-speed`); `/chosen mac-address-bluetooth0` set before launchd (MobileGestalt's `bluetooth` capability, which Settings'
  row requires, is computed early); `AppleOLYHAL` with vendor-id `Murata` (WifiVendor, BlueTool waits for it); the stream kernel
  control `com.apple.uart.bluetooth` (the newest client owns it: BlueTool stays connected after its script).
- Bridge emulation: vendor-group commands (OGF 0x3f) answered with success, 32 bytes of parameters echoing the first command byte
  (LE meta sub-commands are matched on it; 0xfd48 returns instance 1); Read/Write Default Erroneous Data Reporting answered (bits
  added to the supported-commands mask; bluetoothd's HCI init requires them); LE_Read_Periodic_Advertiser_List_Size answered with
  0 so the controller stays in legacy LE mode (otherwise it refuses legacy scan and advertising commands).
- Root image (installed over NBD): bluetoothd's job gets `BT_HCI_TRANSPORT=H4:POSIX:com.apple.uart.bluetooth,3200000`;
  `/usr/sbin/BlueTool` patched (`perf-test/patch-bluetool.py`: its XPC script reply always carries result 0, the ad-hoc signature's
  page hash rewritten), since its built-in scripts are Broadcom patchram sequences for a chip this phone does not have.
- Keychain: AppleKeyStore selectors 10/11 (wrap/unwrap item keys) implemented; before, bluetoothd could not store its identity keys.
- Turning Bluetooth off (2026-11-09): bluetoothd crashed (pc 0) every time. At "Terminating stack" 0x1000e6aac calls the HCI
  transport's hook at +0x60 for every transport but USB, without a null check; H4, H4DS and H5 have no such hook (H4BC, APPLEBT,
  PCIE and VIRT do). `/usr/sbin/bluetoothd` patched (`perf-test/patch-bluetoothd.py`: the hook is called only when set, page hash
  rewritten). Verified: off without a crash, then on again from Settings.
- **Open: Bluetooth takes about 30 s to come up**, at boot and on every power on: after HCI init (last command 0xfd3c)
  bluetoothd sends nothing for 27-30 s. Its unified log shows the wait: "Done starting up stack", then a synchronous CoreTelephony
  query ("Could not get the Tx state") that ends only when the connection to CommCenter breaks (it crash-loops every 15-25 s:
  no modem). Disabling CommCenter (moving `com.apple.CommCenter-noncellular.plist`, the job this hardware loads, out of
  `/System/Library/LaunchDaemons`) removed the wait (0xfd3c to the next command 50 ms), but **broke auto-lock**: SpringBoard's
  telephony manager then retried its CommCenter connection in a tight loop (about 46,000 "CTServerConnection returned error" in a
  boot), and each failure reset the backlight idle timer (`updateIdleTimerForReason:"IdleTimerReset"`), so the display never turned
  off and the phone never idle-slept. Confirmed by restoring the job: display off at 42.9 s again. **CommCenter is enabled again**
  (2026-11-10); the Bluetooth wait needs another fix (CommCenter's crash, or bluetoothd's telephony query).
- **Fixed 2026-11-09: Control Center showed Bluetooth unavailable** (dimmed, taps ignored; status bar without Bluetooth) while
  Settings showed it on. SpringBoard's BluetoothManager attaches to bluetoothd five times at its start (unified log: "failed to
  attach with error 9") and never again; bluetoothd exits until the `bluetooth` service exists, which was published only after the
  controller bring-up (about 45 s). The same race reached bluetoothd's crash-loop detection when the bring-up took longer: after
  five failed starts every later start exits 1 and Bluetooth stays off for the boot. **`houji_bt_early`** (default 1 since 2026-11-10; `houji_bt_early=0` for the old order): the service,
  AppleOLYHAL and the H4 control are published at the root mount (about 7 s), and a client connecting before the controller is up
  waits for it (up to 90 s). Verified: bluetoothd started once at 7.2 s, BlueTool waited 18.4 s, Bluetooth up at 49 s; Control
  Center's button available, a tap shows iOS's "Disconnecting Nearby Bluetooth Accessories Until Tomorrow".
- Console `bt log` traces the next 400 HCI packets; writes from a client that lost the channel are logged (first 100).
- Diagnostics used: `houji_exit_trace=NAME`, `houji_path_trace=NAME`, and the unified log parsed on the host
  (mandiant/macos-UnifiedLogs on /private/var/db/diagnostics and uuidtext copied over NBD).

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
  Cause found 2026-11-09: the root image's SpringBoard preferences `SBDisableAutoDim`, `SBDontDimOrLockOnAC` and
  `SBDisableIdleSleep` (`tools/root/add-nodim.py`). With them removed iOS dims and locks by itself (display off 42 s after
  boot, no touch); see System sleep.
  Before the first touch after a wake (the lock screen) the wait is `houji_autolock_lockscreen` (default 10 s), as on iOS: the
  lock screen's "swipe up" hint bounces the home indicator at 120 Hz, a full-screen CPU copy per frame here (about +0.5-1 W).
- Verified on the phone: power-key sleep, taps and swipes on the dark screen (stayed off), power-key wake, swipe to unlock (home
  screen), automatic lock after 60 s, slider.
- **Safari pages were black (fixed 2026-11-10 with `houji_srgb=1`, opt-in).** Pages loaded and the web processes sat idle, yet the page
  area stayed black. The D22 tree reports a P3 display (`/product artwork-display-gamut = "P3"`), so MobileGestalt's
  HasExtendedColorDisplay is true and WebKit draws its layers in two-plane RGB10A8 surfaces, which the Metal translator only
  shadows (the render server's shadowed-texture count went from 7 to 46-50 on loading a page; Turnip also refuses some surface
  layouts, `VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT`). `houji_srgb=1` sets the property to `sRGB` before launchd
  (`HoujiScreen.cpp`), and WebKit draws 8-bit BGRA surfaces like other apps. Verified: Safari shows its own error page (no Wi-Fi in
  that boot) and a `data:` page's text. Colors are then managed as sRGB. Not fixed by it: the home screen widgets stay blank.
- Not done: the panel's refresh rate (it keeps the stock idle-refresh mode from the boot firmware; no 1-120 Hz/LTPO control), and
  powering the touch controller down while the display is off.

## System sleep and resume — working (`houji_sleep=1`)
iOS's own sleep: the display turns off (Auto-Lock or the power key), powerd and the root domain idle-sleep the system, and it
resumes on the power key (full wake) or on a scheduled wake (dark wake). Not the SteamOS port's design (its sleep does not work):
XNU's own IOCPUSleepKernel path with PSCI and the GIC.
- **CPUs** (`arm/SweetCPU.cpp`, `houji_cpu_off_self` in osfmk): IOCPUSleepKernel halts the seven secondaries: `processor_exit`,
  then on each `cpu_sleep` → `quiesceCPU` → PSCI `CPU_OFF` (sleep token for `cpu_exit_wait`). On wake `processor_start` → PSCI
  `CPU_ON` → `houji_secondary_entry` → `initCPU(false)`, as at boot. The boot CPU waits in WFI with interrupts masked (without
  the masked-time check, which panics after `interrupt_masked_timeout`); only the virtual timer passes the CPU interface mask
  (priority 0x80, mask 0x90, `houji_gic_sleep_mask`), is acknowledged each tick and re-armed every 25-50 ms to read the power
  key over SPMI. 15 s asleep: 600 timer wakes (before the mask: 1.38 million, SPI 261 always pending).
- **Wake**: power key pressed after being seen released → user wake: the root domain promotes it to full wake
  (`houji_pm_next_wake_is_user`; this macOS-configured kernel does not promote wakes as iOS does) and, 300 ms later, if the
  display is still off, the buttons driver reports the press, which lights it (iOS does not light it on its own after a wake).
  Scheduled wakes: SweetCPU registers for the root domain's wake settings (`wake`, `WakeByCalendarDate`, `WakeRelativeToSleep`,
  maintenance and sleep-service calendars), which an iPhone's PMU RTC would take, and wakes at the earliest (dark wake).
- **Policy** (root domain, `HOUJI_IOS_IDLE_SLEEP` with `houji_sleep=1`): powerd sets the sleep slider to 0x7fffffff with the idle
  time in "System Idle Seconds"; only the iOS idle sleep branches honour that (the kernel is configured as macOS). powerd sets
  the slider to 0 while a PreventUserIdleSystemSleep, BGTask or SystemIsActive assertion is held (from its own type → flag table).
  `houji_sleep=1` also makes the sleep notice reply timeout 5 s (`pmtimeout`, default 30 s): locationd, bluetoothd, CommCenter
  and cloudpaird did not reply (hardware this port lacks or emulates); 30 s per round made display-off to sleep 60-110 s.
- **Root image**: `tools/root/add-nodim.py --remove` (SBDisableIdleSleep kept a SystemIsActive assertion for SpringBoard;
  SBDontDimOrLockOnAC kept the display on while charging).
- **Boot arguments**: `houji_sleep=1` (opt-in); `houji_sleep_max_s=N` wakes after N s at most (0, the default: no limit; 30 s if
  the power key cannot be read); `houji_sleep_test_key_s=N` wakes as if the key was pressed after N s (tests without a hand on
  the phone); `pmtimeout=N`; `darkwake` as upstream. USB console: `sleep` (kIOPMSleepNow), `pm` (idle sleep inputs), `cpu off N` /
  `cpu on N` (one secondary). With `io=0x20000` the root domain's trace goes to the console.
- Verified on the phone (2026-11-09, temporary boots): all 8 cores off and on 7 times in a row from the console; idle sleep with no
  input 61 s after iOS's own display-off (5 s timeout; before: 112 s), dark wakes back to sleep within 100 ms; scheduled wake at
  117.96 s for powerd's 118 s request; test-key wake to full wake with the lock screen lit, no re-sleep; `reboot` from a wake window
  left a clean volume; a real press of the power key after 20.6 s asleep: woke "(power key)", the press reached iOS (key still
  held, so the buttons thread reported it and the fallback press was not sent), display lit, no re-sleep. Not verified: power draw
  (the phone charges over USB throughout; not measured), and time: CNTVCT runs through sleep, so `mach_absolute_time` includes it
  (on an iPhone it stops). Devices are not powered down (display off; GPU, Wi-Fi, UFS keep power).
- **Measured 2026-11-11: sleep saved almost nothing.** With the USB cable in and the charger input suspended (console
  `batt hold N`, board property 54), the battery's charge counter (1 mAh steps) fell 360 mAh in 0.62 h that held one 63 s and one
  2170 s sleep (about 98 % of the window asleep): about **580 mA, 2.4 W**. Awake with the display off, same boot, 1 h: 585 mAh,
  584 mA, 2.5 W. Approximate: the end reading of the sleep window was taken a few seconds after the wake, when the input had been
  released and charging had resumed. Only the seven secondary CPUs are off in that sleep. Which parts take the 2.4 W is **not
  measured yet**: no driver powers anything down (GPU with its GMU firmware running, Wi-Fi and its PCIe link, the UFS link, MDSS/DSI
  with the panel blanked but not asleep, USB, the rails and clocks the boot firmware left on). In the idle log the CPUs sit at their
  lowest frequency with a few permille of load, and the GPU sensor is the warmest (46-47 C against 45 C).
- Also seen in that run: iOS's idle sleep at 102 s was cancelled (`PM notification cancel (pid 18, powerd)`, `idle revert`) a few
  seconds after the input was suspended, and iOS did not ask again in the following hour with the display off. That the power
  source change caused the cancel is likely but not shown.
- **2026-11-14: UFS link in HIBERN8 halves sleep power; the default with `houji_sleep=1` (`houji_sleep_off=0` turns it off).** Measured in sleep from the battery current
  that a read cycle started at the wake returns (`HOUJI BATT WAKE`, 25-45 ms after it; the fuel gauge keeps measuring while the SoC
  sleeps), 3-5 sleeps of 20 s per step, one boot per row group, charger input suspended (`perf-test/sleep-sample.py`):

  | Sleep configuration | Power |
  |---|---|
  | as before (panel blanked, nothing powered down) | 2.35 W (2.27-2.39) |
  | **panel asleep** (DCS 0x10; the default with `houji_sleep=1`) | **2.07-2.13 W** |
  | + **UFS link in HIBERN8** (the default with `houji_sleep=1`) | **1.00 W** (1.00-1.02) |
  | same, 5 sleeps of 60 s; same boot without it: 1.81, 2.11 W | **0.99 W** (all 5) |
  | panel asleep + GPU power domains collapsed (GX/CX GDSC, measurement only, not restored) | 2.09 W |
  | + display controller domains collapsed (measurement only) | 2.09 W |
  | panel asleep, CPU frequency domains lowest / L3 1555 -> 364 MHz / LLCC, MM, SNoC, PCIe NoC votes low | 2.08-2.13 W |
  | panel asleep, MX or CX rail vote lowered to SVS | 2.10 W |
  | boot CPU spinning instead of WFI (check of the method) | 2.36 W against 2.35 W |

  HIBERN8: `houji_ufs_hibernate` (HoujiUfsRead.cpp) sends DME_HIBERNATE_ENTER (0x17) from bootSleep with interrupts masked, only
  when no transfer or task is outstanding and the host lock is free (it is then held to the exit, 0x18, retried once; a failed exit
  poisons the transport). Entry completes in one 100 us poll, exit in about 6. Applied on iOS's own idle sleeps too. **Incident:** in
  a boot with 8 HIBERN8 sleeps (exits all reported success), the UFS transport failed 77 min after the last one, at a power key
  wake: every read returned EIO (backboardd's shader cache "Input/output error"), SpringBoard, backboardd and others died with
  SIGBUS in a loop, the display stayed dark. The volume headers were intact afterwards. The driver did not log why it poisoned the
  transport; it now does (`HOUJI UFS POISON`, controller and UIC error registers, time since the last HIBERN8 exit), and
  `pw ufs N` checks the link with N real reads. Whether HIBERN8 caused it is not known: afterwards 30 cycles of a 20 s
  HIBERN8 sleep each followed by 20 reads all passed (`perf-test/ufs-stress.py`); the failing boot also had long sleeps, 5-min
  `houji_sleep_max_s` wakes, and a last sleep without HIBERN8. A 90-min soak through iOS's own sleeps with HIBERN8 and 5-min wakes
  (`perf-test/ufs-soak.py`): 18 wakes, 16 HIBERN8 entries and exits (2 skipped: host lock taken, -3), a 10-read check after the
  wakes and after a final full wake, all passed, no POISON line, no SIGBUS. The failure was not reproduced. The first version never entered: it read 0x70
  (UTMRLBA, never zero) as the task doorbell instead of 0x78 (UTMRLDBR). Comparison: the Linux recovery kernel on the same phone,
  display off, USB connected, input suspended, idles awake at 212 mA, 0.85 W (no system suspend, only WFI and a cluster state).
  Remaining open: what the rest of the 1.0 W is (the SoC never enters a deep idle state; Wi-Fi stays associated), auto-hibernate
  (AHIT) for awake idle.
- **Measured 2026-11-13: where the idle power goes (first round).** The "charge counter" is not a coulomb counter: it stayed at
  2588 mAh for 9 min at about 700 mA and then fell 82 mAh at once, together with the percentage; windows under an hour cannot be
  measured with it. The fuel gauge's current reading is usable but only while awake, so parts were compared awake (no
  `houji_sleep`), display off by iOS, input suspended, one boot per configuration, 11-15 read cycles each over 2-3 min
  (`perf-test/awake-lab.sh`, median of current x voltage; about every third sample is 0.3-0.8 W higher):

  | Configuration | Median | Change |
  |---|---|---|
  | everything started (Wi-Fi, Bluetooth), panel blanked | 2.49 W | |
  | same boot, panel asleep (`screen sleepin 1`, DCS 0x10) | 2.26 W | -0.23 W |
  | same boot, panel asleep and CPUs 1-7 off (`cpu off N`) | 2.31 W | none visible (noisier: 2.07-2.75 W) |
  | `houji_wifi=0`, panel blanked | 2.22 W | -0.27 W |
  | `houji_wifi=0`, no `houji_bt`, panel blanked | 2.19 W | -0.03 W (within the spread) |

  One boot each, so boot-to-boot variation is not known. Panel sleep and Wi-Fi together account for about 0.5 W; about 2 W remains
  with neither, not attributed: GPU (GX on, GMU running), UFS link, USB controller and PHYs, MDSS/DSI, ADSP, and the SoC's shared
  rails, buses and memory, which never enter a low-power state (no CPU idle state below WFI, no RPMh sleep votes).
- **Sleep hooks and tools** (`arm/HoujiPower.inc`): `houji_sleep_off=MASK` / console `pw off MASK` (1: CPU frequency domains to their
  lowest row during sleep; 2: UFS link HIBERN8, the default with `houji_sleep=1`; 4: CPU L3 lowest; 0x80/0x100/0x200 measurement
  only); `pw rpmh` dumps the apps RSC's TCS contents and the L3 vote, `pw vote NAME HEX` sends one RPMh write, `pw l3 N`; `pw sleep N [MASK]` sleeps and wakes fully N seconds later without a key (verified: 15-120 s
  sleeps woke on time); `pw census` lists the clocks and power domains that are on in GCC and GPUCC (46 of 116 GCC branches:
  USB, UFS, PCIe0, QUP, GPU buses). Not measured: what the hooks save (needs hour-long windows).
- Open: a sleep forced while the display is on (`pw sleep`, console `sleep`) leaves the panel lit through the sleep.
- **Fixed 2026-11-09: panic minutes after a wake.** A boot reset 85 s after a power key wake; the panic log
  (`houji_panic_log=1`, see `log-capture.md`) of a later sleep test showed `wait_while_equals32 … timeout` in
  `task_restartable_ranges_synchronize` (CommCenter), with only CPU0 answering the panic's debugger IPI (all eight answer
  without sleep). A restarted CPU's `SIGPdisabled` is cleared by its first IPI, the self-IPI `initCPU(true)` sends at boot;
  `initCPU(false)` did not send it, so after a wake every IPI to a secondary failed (lost ASTs, timed-out waits). Resume now
  sends it too. Verified by hand: several sleep/power-key wake rounds with use in between, no reset.
- **Fixed 2026-11-09: the USB console died for good after a sleep** when the host sent a control request (SET_CONTROL_LINE_STATE,
  sent when a program opens the port or changes DTR) while the phone slept: the host timed it out and began a new one, and after
  wake the control TRB completed with status setup pending, which the driver took as an invalid completion and stopped (found with
  `houji_usb_fail_panic=1`, which turns a console stop into a saved panic). It now ends that control transfer and arms the next
  setup, as Linux `dwc3_ep0_complete_data`. Verified (`perf-test/usb-sleep-test.sh`, DTR toggled every second through three
  sleeps): five such transfers ended, console answered after every wake. bootSleep also prints `HOUJI USB SLEEP` (controller
  events and DSTS during sleep).
- Open: after some wakes the display turns off but the system does not idle-sleep again (cause not found). CommCenter crash-loops
  every 15-25 s (no modem); disabling it breaks auto-lock (see Bluetooth).

## Root volume (HFSX on UFS userdata through md0) — header guard
- Incident (2026-11-09): after a USB-console reboot from iOS (`reboot_kernel`, which unmounts the root) the volume did not mount.
  Its block 0 (the volume header at byte 1024) and the journal header were all zero; the rest was intact. Repaired from the
  journal without the clean image: the newest transaction's blocks (volume header, two allocation blocks, one catalog node) were
  written home and an empty journal header was written (start = end after it, next sequence), over NBD, checked by a mount and a
  full catalog walk (334,014 entries) before committing; the original blocks are kept in `private/root-repair/`.
  `perf-test/hfs-journal-scan.py` lists journal transactions without trusting the journal header.
- Second incident (2026-11-09 evening): the USB console vanished while iOS was asleep (a console command was sent during sleep);
  after the forced reset the journal header was zero (volume header intact). XNU logged `jnl: open: bad jhdr size (0)`, marked the
  volume `FSK!` and every later boot failed with "cannot mount dirty non-journaled volumes". Repaired by
  `perf-test/nbd-journal-replay.py`: the 27 surviving transactions (one contiguous chain, checksums valid) written home in sequence
  order, an empty journal header after the newest, checked by a mount and a full catalog walk (334,976 entries) before committing.
- Cause not found. Ruled out on the phone: the in-memory volume header was never zero (polled every 1 ms), journal copies did
  not change before their write, and Skywalk was not involved.
- Guard (`HoujiRootWindow.h`): a write that would leave block 0 without a valid HFSX header, or put zeros over a sector holding a
  journal header, is refused with EIO and logged (`HOUJI ROOT: REFUSED`, with a backtrace); the old header stays readable.
  Zero data is not refused elsewhere: md0 allows 32 blocks per write, so the journal splits at 16 KiB, and a 512-byte zero piece
  of an allocation block is legitimate (an earlier guard refused it and stopped the journal every boot at about 30 s).
- Verified: 220 s of iOS without refusals or journal errors; a USB reboot then left a clean volume (unmounted bit set, empty
  journal), and a catalog walk matched.

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
  doorbells) is a real level interrupt (about 20-60 per second; battery works). As GIC interrupts, the RSC line (INTID 37) froze every
  boot and PCIe0 MSI (INTID 173, Wi-Fi) froze 3 of 4 boots at the point Wi-Fi brings up its link (the 4th ran normally with about 70
  MSIs per second); the cause is not known. vote() polls its RSC TCS status itself; Wi-Fi MSI stays on the 2 ms poller.
  `houji_spi=0` restores polling for everything; `houji_spi_mask` (bit 0 IPCC, 1 RSC, 2 MSI; default 1) selects lines.
- Idle with the screen off after both: 2.4-2.5 W, CPU about 47.5 C (wfi=1 alone: 2.5-2.8 W, about 52 C).

## System apps and userland geometry — working
- All 32 original 20H392 system apps moved from the staged directory to `/Applications`; one-shot Launch Services registrar
  installed in launchd's cache. Kernel AMFI `@`-path and TCC media-library policies are active.
- `HoujiKeyboardGeometry.dylib` installed and weak-linked into 189 APPL executables, with original entitlements preserved.
- Metal bundle rebuilt with measured texture-limit publication (`NativeDeviceLimits.h`).
- Block readback passed and HFS+ fsck returns 0. Runtime verified: registrar exited 0; restored icons appear; Calculator, Clock,
  Books, Music and Photos open; Spotlight keyboard fills the 400-point canvas; status-bar sides avoid the camera region.

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

## Standalone self-boot — host prep steps moved in-image

A power-on boot from `boot_a` previously stopped at the UEFI result page with
`HOUJI LOGFS: stopped with Already started`: the PC flow (`boot-houji.sh`)
performs three preparations that the image itself did not, and the first one
gates the rest.

| Host step in `boot-houji.sh` | In-image replacement |
|---|---|
| `rm -f logfs/UniXNU.log` | `RunLogFsTest` deletes the previous UEFI transcript and writes a fresh one through the validated logfs FAT path |
| `dd XnuLog.initial.bin` into `XnuLog.bin` (16 reserved clusters) | `ResetNativeXnuLog` rebuilds the pristine 65536-byte initial image (magic `HJXNU001`, run ID, zeros; CRC32 `abbe97d5`) and restores it with size, content and CRC verified |
| `dd` of the exact 864-byte `boot-recovery` BCB into misc | `ArmStandaloneRecoveryRequest` read-modify-writes the same record at absolute LBA 1024 (bytes 864..4095 preserved), flushed and read back, before `GpuPowerRecoveryPrearm` verifies it |

The BCB write is allowed by a new bounded guard window (`UfsBcbArmWriteAllowed`):
exactly one 4096-byte block write at LBA 1024 plus a device cache flush, and
only while the single arming call holds `mBcbArmWritesArmed`; the read-only
command policy for every other LBA is unchanged. The kernel-side contract is
unchanged: native-root still refuses a non-pristine `XnuLog.bin`, and every
reset path still requires the exact no-wipe BCB readback.

With the BCB armed for the whole session, a power cycle after a failed XNU
run lands in OrangeFox automatically; after a successful run the next power
cycle passes through OrangeFox once (it clears the BCB at startup) and the
power-on after that arms it again from the image. Standalone power-on
verification is tracked below in this document's run log.

### Standalone boot verified (2026-11-04)

Two consecutive power-on boots from `boot_a` reached the iOS desktop with no
PC involvement (cable connected but no host command issued; ABL normal boot
path, no `fastboot boot`). Evidence pulled from logfs afterwards
(`run-logs/selfboot-20261104/`):

- `UniXNU.log`: production profile forced; logfs GPT/FAT validation passed;
  fresh transcript written (the old `Already started` stop is gone);
  `exact recovery request already armed` at misc LBA 1024; GPU prearm
  verified the same record; touch handoff completed; `HOUJI NATIVE UFS r222`
  reached the XNU loader.
- `XnuLog.bin`: valid header/run ID/CRC; kernel snapshot shows SpringBoard
  running, 1417 presents, live GPU commit/Metal statistics — a full desktop
  session, and proof the pristine-log restore passed the kernel's ownership
  check (it panics otherwise).
- `misc`: cleared by OrangeFox at its own startup, as documented.

Known gap from this run: the XNU USB console did not enumerate on a
standalone power-on (`HOUJI CHARGER: usb-online=0` in the snapshot). In
PC-assisted sessions ABL's fastboot had already initialized the USB
controller; on a normal power-on nothing does, and the kernel console
driver expects an inherited controller. Kernel-side USB controller bring-up
is the follow-up; it does not affect boot or the desktop.
