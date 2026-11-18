# iOS 16 → iOS 27 GPU performance gap

Offline source/log comparison; counters are not panel-FPS measurements.

## Ranked gaps

1. **GPU OPP plus DDR/interconnect vote (highest impact).** iOS 16 uses all 12 factory A750 levels (231–903 MHz), busy-time DCVS, a 680 MHz touch floor, bandwidth paired to every OPP, and TSENS caps (`kernel/overlay/iokit/Kernel/houji-ufs-root/HoujiGpuDvfs.inc:10-19,45-88`; base commit `c4d962f`, touch `6753c18`). iOS 27 has an equivalent 220–680 MHz governor and paired HFI bandwidth votes (`package/GpuPowerProbe.cpp:1831-1853,1884-2059`) **but it is opt-in and was absent from every tested boot line**. Thus `root75.log:230` proves the startup stayed `P1/1` (220 MHz/minimum BW), not that a higher clock was used.

2. **Interactive CPU DVFS and realistic thermal policy.** iOS 16 samples at 40 ms, detects render streams, holds a 45% frame floor, gives P cores a 65% touch floor, supports a 300 ms touch window and optional X4 row (`arm/HoujiDvfsPolicy.h:5-18`; `arm/HoujiCpuDvfs.inc:73-139,160-174`; commits `1757675`, `42a4ea7`). iOS 27 samples at 200 ms, has no frame/touch floor, globally caps at 60%, and starts throttling at 50 C (`package/HoujiCpuDvfs.cpp:5-7,24-28,107-176`). `fps85c.log:2239-2336` shows fully loaded big domains pinned at 0.844/0.902/0.844 GHz. Raising to 85%/60 C still reached 68.7 C uncooled (`root76.log:9798`).

3. **Frame pacing/pipeline.** iOS 16 advertises 60/120 Hz, queues copies, permits one frame in flight (`arm/SweetFramebuffer.cpp:23,400-450,770-829`; commit `9bb2dbb`), and can queue GPU work (`HoujiGpuNativeProvider.inc:130-201`; commits `2221fd9`, `b1b6d85`). iOS 27 already has early swap retirement, cached surface mappings and queued GPU submissions, but its scanout client synchronously waits for each full-screen GPU copy (`drivers/AdrenoMetal/GpuScanoutPresenter.h:54-107`; `GpuScanoutClient.h:69-109`). Its optional software refresh publication is disabled (`NativeLegacyDisplay.cpp:106-115,437-468`): all `root76` reports say `vsync hz=0`. Display swaps show ~9.6–10.2/s for sustained 10 s windows (`root76.log:7537-8009`), while short peaks reach 25.6/s (`root76.log:6125-6531`); no scanout-copy duration is exported, so CPU/GPU/copy attribution remains incomplete.

4. **Coherence/cache fast path: present.** iOS 16’s proof reduced publish cost to ~10 µs (`docs/peripherals.md:275-283`, commit `ace4b06`). iOS 27 runs the equivalent proof by default and skips maintenance only after it passes (`GpuPowerProbe.cpp:1154-1164,1235-1307`). Per-submit line audits are already default-off (`drivers/Adreno740/NativeAdrenoSubmit.cpp:24-38`), so the iOS 16 recurring-audit fixes `e21c812`/`4567d48` are not missing.

## Top-three port plan

1. In `houji-default.args`, opt in with `gpu-dcvs=1 gpu-dcvs-min=3`; first A/B also use `gpu-perf=4` (401 MHz) to isolate clock/BW impact. Add DCVS level/residency to `GpuPowerProbe.cpp` console output. Risk: higher GPU/DDR power; retain caps and require cooling for sustained tests.

2. Port iOS 16 interaction policy into `package/HoujiCpuDvfs.cpp`/`HoujiTouch.cpp` behind `houji_cpu_interactive=1`; args: `houji_cpu_frame_pct`, `houji_cpu_touch_pct`, `houji_cpu_touch_ms`, and `houji_cpu_x4_boost=1`. Keep 60%/50 C defaults; test 70%/60 C only cooled. Risk: the 85% run already reached 68.7 C.
3. Add `houji_fb_pipeline=1` in `NativeLegacyDisplay.cpp`, `GpuScanoutClient.h`, and `GpuScanoutPresenter.h`: two retained scanout submissions, completion handlers, bounded depth, and per-stage timing; publish `vsync=120` separately as `houji_fb_vsync=120`. Risks: scanout overwrite/tearing without physical vblank, IOSurface lifetime errors, and extra heat; fall back to the current synchronous path on any fence error.

## Stock A750 OPP option

`gpu-opp=stock` advertises the selected houji stock 12-level GMU table and makes DCVS/HFI30 use its paired DDR indices. HFI and
`gpu-dcvs-min` indices are ascending: `1..12` map to `231, 310, 366, 422, 500, 578, 629, 680, 720, 770, 834, 903 MHz`.
The default remains the legacy eight-level table (`1..8` = `220, 295, 348, 401, 475, 550, 615, 680 MHz`). The stock option
accepts the observed fuse selections 4 and 5, which both resolve to the 231–903 MHz houji profile; unknown bins retain the legacy
table. Touch retains the iOS 16 port's 680 MHz floor. `gpu-thermal-c` defaults to 85 C: above that GPU-zone temperature stock
DCVS is capped at 680 MHz, and above another 5 C it is capped at the configured floor.
