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
- `wifi-userspace=1` installs the supplicant and its launchd job; launchd starts it, AMFI refused it with a launch-constraint violation
  (root86). With `no-lc=1` the kernel stopped responding (root87; no log captured). Scan and join: not verified.

## Boot images

- This bootloader loads a fastboot image of 77.2 MB and refuses one of 91.0 MB; after one refusal it refuses every image until it
  restarts (`fastboot reboot bootloader`).
