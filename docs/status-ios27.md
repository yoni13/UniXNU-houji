# houji iOS 27.2.1 (24B156): verified state

Every line is a hardware result on this phone, with the run that showed it. "Not verified" means exactly that.

## GPU-rendered UI

- The lock screen and home screen are drawn by the A750 and handed to the panel by the GPU (root75: 1000 of 1000 frames
  `gpu 1000/0 cpu 0/0`; the user saw and used the UI). Needs all of:
  - `HOUJI_RAM_GIB=4` (pack.sh): the kernel owns 4 GiB at 0x8C0000000 instead of 2 GiB. With 2 GiB, SpringBoard's code was paged
    out and in (76 600 page-ins in a stackshot, root68) and the lock screen never came through the GPU path.
  - `MAXVNODES=66560`: the iOS 27 profile sets no `kern.maxvnodes`; xnu's early sizing then gives 1024 (sane_size still 0), and the
    console logged "vnode jetsam : 1024 desired" thousands of times (18 065 in root72, 0 in root73).
  - `fb-gpu=1`: the GPU copies each frame to the scanout; the CPU copy read GPU-written frames as black and stalled (root73).
  - The 24B156 QuartzCore branches in `scripts/adrenometal-24B156.patch` (0x18434d078 and the IsVirtualDevice test 0x18434d0e4).
  - Shader priming is opt-in (`gpu-prime=1`); its filesystem walk competed with SpringBoard's page-ins (root68).
- 8 GiB did not boot (root71: no console; not diagnosed). 4 GiB boots (root72-root85).
- Frame rate: not measured yet. The console harness (t-fps.sh) could not wake the display reliably; the console key is now held 500 ms.
- With the default DVFS limits (60 %, throttle from 50 C) the big clusters ran at 0.84-0.9 GHz at full load; without the cooler the CPU
  DVFS sensor reached 68.7 C at 85 % (root76). DVFS is not tuned.

## Battery and charging (battery=1..6, opt-in)

- The ADSP boots from this phone's own `modem_a` partition (read-only GPT/FAT access): stage 13, GLINK up, PMIC_RTR_ADSP_APPS channel
  (root82, root83). The ADSP region is mapped as Device-GRE: plain memcpy/bzero there reset the SoC (root78-root80); aligned stores fixed
  it (root81).
- Battery readings from the ADSP: percent, voltage, temperature (0.01 C), present/external/charging (root82).
- `battery=5` publishes an IOPMPowerSource (root83).
- `battery=6` sets the fast-charge current as the iOS 16 kernel did (property 12, 1000 mA, 500 mA outside 10-40 C): the battery charged
  under the GPU UI (root84: 39 % -> 40 %, 3803 -> 3839 mV; the 40 C step-down to 500 mA was seen). Without it the battery drained
  (root83: 44 % -> 40 % in 7 minutes).

## Wi-Fi (wifi=1..5, opt-in)

- WCN7850 on PCIe, firmware running, Ath12kWifi core and `en0` up, the network stack opens it (root63; 64 MiB DMA window).
- `wifi-userspace=1` installs the supplicant and its launchd job. The merged launchd cache must start from this build's own
  `/System/Library/xpc/launchd.plist` (24B156: 732 jobs, 2 539 659 bytes, copied off the phone with `hex PATH START LENGTH`); a cache
  from another build made backboardd abort and launchd reboot (root86, root87). AMFI refuses the supplicant with a launch-constraint
  violation unless `no-lc=1` (root86); with both, the supplicant runs and opens the driver's control channel (root90).
- Settings shows the Wi-Fi row only when the device tree has `/chosen unique-chip-id` (pack `UNIQUE_CHIP_ID=1`, a local random seed,
  not the phone's ECID): without it the driver has no stable address, `mac-address-wifi0` is not published and MobileGestalt reports
  no Wi-Fi (root90: no Wi-Fi, Bluetooth or Cellular rows). With it (root91): the Wi-Fi row, the switch on, and opening the page sends
  a scan (Apple80211 type 10) that returns the nearby networks within 1.5 s, listed in Settings. Join: not verified (needs a
  password typed on the phone).

## Boot images

- This bootloader loads a fastboot image of 77.2 MB and refuses one of 91.0 MB; after one refusal it refuses every image until it
  restarts (`fastboot reboot bootloader`).
