# iOS 27 frame rate on houji (24B156): measurements and changes

Measured with `ios27/t-actions.sh` on 8 GiB + persistent Data. Each action is timed from its gesture's start (the console "gesture"
line, mapped to the swap clock) to its first frame, then over the 0.8 s after that frame, from the kernel's ring of the last 128
swaps (console `swaps`). These are frames the render server delivered to the scanout copy, not a measured panel refresh: the panel
keeps the boot firmware's mode and has no TE/vblank sync here.

## Morning summary, 2026-11-23 (root365-root376)

**Best measured configuration: about 53 fps untraced Control Center opens, up from about 42.** All opt-in:
- boot `tt=1` (Turnip's unpadded linear images: no shadowed textures);
- the four frame threads bound to cpu7 from the console (SpringBoard's main thread, backboardd's compositing thread
  `com.apple.coreanimation.display.` by id, its render-server thread, the Metal encoding thread: `t-bal.sh` side P4);
- frame and touch floors at 100 % (`dvfs set framepct/touchpct 100`), stock caps held with `sko=4`.

| Config (P4, tt=0 unless said) | GPU ms/frame (CP stamps) | Traced fps | Untraced fps | CC GPU hangs |
|---|---|---|---|---|
| P4 (root369, root372) | 20.4-23.9 | 40.8-41.1 | 42.6-43.3 | 0 in 39 opens |
| PC: SpringBoard on cpu3 (root369) | 20.4-22.4 | 41.8 | 41.3 | 0 |
| P7: P4 with cpu7 at 3052.8 MHz (root372) | 20.5-23.4 | 43.3 | 39.1 (1 open, 75 C stop) | 0 |
| **P4 + tt=1** (root374, root375) | **16.3-18.3** | **52.2-54.3** | **52.6-53.4** | **0 in 25 opens, 2 boots** |
| P4 + tt=1 + `tu-cb=1`, concurrent binning (root376) | 17.5-20.7 | 47.9 | 43.4 (5 opens) | 0 in 13 opens (0 at boot too); slower, stays off |

**What each phase found:**
1. *Audits (no boot).*
   - dada's in-place rendering passes 1 of 457 shadowed textures (0.0017 % of texels): every full-screen 1200x2670 layer fails its
     height % 4 rule, and dada's panel is 1200x2670 too. Not ported.
   - Our metal2vulkan already keeps render passes per device (`KeptRenderPasses`, 256): no churn, so Branch C(1) is moot.
   - cpu7 is EPSS domain 2, not 3: `p7cap`/`p7pin`, `cpub` and `framebud.py` acted on or read cpu2-4. Fixed (`kPrime`).
2. *Measurement.*
   - Console `gtl` adds the CP's always-on stamps around every GPU job at run time, and `tl` now also records GPU kicks,
     interrupts, fence passes, swaps and every CPU (its sampler on cpu0-1).
   - The 7-9 ms "idle gap" was the untracked compositing thread (2.5-11 ms a frame) and the GPU.
   - In the open the GPU is busy 82-98 % at 903 MHz, 20-24 ms a frame. The GPU DCVS's 8-22 % over 10 s windows agrees: one open's
     saturated 0.7-0.8 s.
   - P4 beats PC: SpringBoard runs 10-14 ms a frame on cpu3, against 2.4-3.4 on cpu7.
3. *Branch A (cpu7 clocks).*
   - On its real domain cpu7 scales: 1474 / 2627 / 3047 MHz timed at 1500 / 2630 / 3052.8 MHz votes.
   - Its table's rows past 3052.8 MHz are boost rows (bits 18:16 = 1), so `kCaps` stops at 3052.8.
   - 3.05 GHz trims about 1 ms each from the compositing and encoding threads, but while the GPU is saturated that is about 6 % traced
     and hot. `p7c` stays opt-in and off.
4. *Branch B (GPU >= 10 ms).*
   - Most GPU time is outside render passes (root373, root194).
   - `tt=1` removes about 5 ms of GPU a frame (the shadow copies) and gives about +24 % untraced fps.
   - Its earlier Control Center faults (root361-362) did not recur in 25 opens.
   - The faults that remain are at boot, in tt=0 and tt=1 alike. The new fault map (`gpuf MAP`) places them at stray low addresses in
     unmapped holes (0x14864c0), not past an image's end.

**Guardrail incidents:**
- root369: a reset reading EPSS vote words a domain does not have (`dvfs lut`). Fixed; the dump now runs first in a boot.
- diag=1 boots (root370, root373): a Control Center GPU hang in each (one with an SMMU fault, FAR 0x4abf4000). diag=1 is not used again.
- On a cool night the stock caps lift (skin under 25 C). root371's boot reached 76 C and the 74 C guard stopped it; root372's untraced
  P7 open read 75 C and `t-bal.sh` stopped at 70 C. Runs now hold the caps with `sko=4`.
- The 74 C guard was never disabled. No panics during the opens, no boot loops.

**Path to 60 fps** (frame now about 18.5 ms: GPU 17-18 ms busy 90-94 %, cpu7 about 15 ms of work):
1. Cut GPU work outside passes:
   - the remaining shadow copies (0.42 a pass with tt=1, root356; census needed);
   - barriers, cache flushes and clears between the about 38 passes a frame.

   About 2 ms of GPU reaches 60 fps.
2. Then cpu7, at about 15 ms. The compositing thread (5.5 ms) and the encoding thread (6.5 ms) share it, so 60 fps needs both
   under 16.7 ms together.

**Path to 120 fps (8.3 ms):** about 2.1x less GPU and 1.8x less cpu7 work.
- *GPU:*
  - fewer passes and GMEM loads and stores of full-screen layers (2.6 a frame, loaded and stored);
  - UBWC for the render server's own intermediates;
  - no shadow copies at all.
- *CPU:*
  - parallel encoding (the largest command buffer on cpu7, the rest on cpu5-6; cpu2-6 alone is 1.6-1.8x slower per load, `cpub`);
  - `end_pass` structure reuse;
  - SpringBoard's own work (2.4-3.4 ms on cpu7).
- Only cpu7 is fast: the compositing, encoding and SpringBoard threads cannot all have it at 120 fps.

## Changes that helped (opt-in boot arguments)

| Change | Evidence |
|---|---|
| `vsync=120`: publish a 120 Hz refresh to Core Animation's `CADisplayTimer` (it used its own 1/60 s clock) | root182: home swipe 76-81 fps and Settings scroll 81-91 fps inside the animation, from ~30 fps |
| `gpu-opp=stock`: the stock 12-level A750 table, up to 903 MHz (speed bin 5 accepted) | root178 -> root179: frames with >= 16 ms GPU time 32 % -> 12 %; mean commit-to-complete 18.3 -> 11.9 ms |
| Seed `leanlc2`: navd and routined removed from the lean launchd cache | root188: navd and CoreRoutineHelperService looked up the removed `com.apple.geod` ~4000 times a second; launchd logged each failure at 1.3 cores. root189: launchd gone from `top`, its log ~50 lines a second |

## Where it stands (root191-root193, `vsync=120 gpu-opp=stock`, seed leanlc2)

| Operation | Animation frame rate |
|---|---|
| Settings scroll | 77-90 fps |
| Home page swipe | 62-75 fps |
| Control Center close | 52-59 fps |
| Settings close (swipe up) | 37-56 fps |
| Search open / close | 30-43 fps |
| Settings open | 26-38 fps |

Not yet at 90 fps. The limit is GPU time per frame (root193, `mt` histograms differenced over three repetitions of each action):
Search frames take >= 8 ms of GPU time in 68 % of frames (180 of 321 at 16-64 ms), Settings open/close 42 %, home swipes 39 %;
encoding stays under 2 ms for most frames. All passes run in GMEM (`RM gmem 12131 11572M`, root178), about one screen of pixels per
frame, so the cost is per pass or per texture read rather than pixel count. Next: export Turnip's per-pass ring (`tu_pass_ring`:
mode, size, linear attachments, shader statistics) and find the passes of the blur materials.

### What the per-pass trace shows (root194, `diag=1`, `t-passes.sh` / `decode-pass-trace.py`)

The `mt` "frame-gpu" times are not GPU execution. Turnip's own pass timestamps (frames.trace type 7) put the GPU at 6-7 % busy over
two Search or Settings runs and 21 % over Control Center; a Search command buffer holds about 2.9 ms of pass time. Per render command
buffer (pass clock aligned to the CPU clock by the minimum record-to-start gap, so lower bounds): commit to the first pass 1.6-3.7 ms
(p90 up to 9 ms), first to last pass 4.6 ms for Search and 14.3 ms for Control Center with 2.9 / 5.9 ms of it in passes, GPU end to the
completion callback 1.3-2.2 ms. Core Animation waits for every render command buffer (`waitUntilCompleted`, one per frame, one thread:
median 2.7 ms Search, 10.8 ms Control Center), so frames do not overlap and those gaps are frame time. Kernel submissions are not the
wait: about 60 us each, no wait for an unsubmitted sync, no ring-space waits. `tu-debug=sysmem` (root195) did not change the rates
(Search 31-34 fps), so GMEM load/store is not the main cost either.

Next: what fills the gaps between passes of one command buffer (its 2.5-3.5 kernel submissions, the presenter's copy, cache
maintenance), and why Core Animation waits for completion instead of handing the swap a fence.

### BO churn, the BO cache and the wallpaper poster killer (root196-root201)

Console `gm` (GPU driver method counters) around repeated actions: every action allocated, GPU-mapped and CPU-mapped hundreds of
BOs (root197: Search 491 allocations and 520 GPU maps per run, Control Center 876). backboardd's Turnip BO cache missed them: it
evicts a freed BO after one second, so each action started cold (33-52 % hits). `turnip-darwin-bo-cache-age.patch`: boot argument
`tu-bc=N` keeps freed BOs N seconds under a 256 MiB cap; with `tu-bc=8` Search hit 70 % (root201: 937 lookups, 653 hits, its 284
misses = the kernel's 298 allocations).

`houji_no_posters=1` (kill the wallpaper's MercuryPosterExtension every 3 s) cost more than it saved and caused the wallpaper to go
dark for about a second at each kill. root199 (killer on) vs root200 (off), both `tu-bc=8`: idle `top` widgets ~93 % each and the
extension 24 % vs backboardd 7 % and nothing else above 3 %; the mean GPU map 286 -> 57 us, kernel GPU-map time per Search 107.5 ->
24.9 ms; Settings scroll 95 fps (round 1). The killer is off from root200 on. `diag=1` itself costs smoothness (root201 vs root200:
18-27 % vs 51-65 % of swipe frames within 20 ms): frame-rate runs go without it.

Search after these (root201 vs root194, per render command buffer): commit to the first kernel submission median 3.1 -> 1.4 ms,
commit to the last pass 9.9 -> 5.2 ms; the slow tail stays (p90 15-16 ms), and Search renders only about 38 command buffers in 2.6 s.

### Asynchronous submission (`m2v-async=1`, root204)

root202: during Search, backboardd's display thread spent 352 of 1000 samples in `CA::OGL::MetalContext::flush` ->
`-[NBCommandBuffer commitAndWaitUntilSubmitted]` -> `waitUntilScheduled` -> `encodeDrain()` (symbolized with the plugin's link map,
base 0x115f00000): every frame's flush waited until the encoding thread had translated and submitted the whole frame, so
CoreAnimation's encoding and the runtime's never overlapped. `adrenometal-async-submit.patch` (boot argument `m2v-async=1`) returns
from that call once the commit is recorded, on the render server's threads only; order, read-backs and completion waits are unchanged.

root204 (`gpu-opp=stock vsync=120 tu-bc=8 m2v-async=1`, killer off), rounds 1 / 2: Settings scroll 105 / 112 fps (median gap 8-9 ms),
Settings close 95 / 57, home swipe 53 / 74 (median 9 ms), Control Center close 70, Control Center open 62 / 49, Settings open
30 / 48, Search open 10 / 31, Search close 54 / 46. Screenshots of Settings, Control Center and the home screen are complete (the
Settings row icons, blank grey in root180, now draw); no GPU submit errors or map refusals.

### Render lowering on the encoding thread (`metal-flow=6`, root205-root206)

root205 (`m2v-async=1`): during Search the encoding thread waited in metal2vulkan's `CommandBuffer::settle_ended`, under
`render_command_encoder` from `nmb_render_encoder_new`: each new render encoder waited for the lowering thread to finish the last
one's end, a hand-off per pass in CoreAnimation's blur chains. The existing boot argument `metal-flow=6` (bit 1 off: no lowering
thread, render encoder calls lowered at the call) removes it. root206 (`m2v-async=1 metal-flow=6`), rounds 1 / 2: home swipe 72 / 72 fps
(median gap 9 ms), Search open 38 / 40, Search close 51 / 54, Settings close 58 / 69, Settings scroll 112 (round 2; in round 1 Settings
did not open, its screenshot shows the home screen), Control Center open 24 / 45, close 52 / 91 (4 gaps). One boot each: the
differences to root204 are within the run-to-run spread except Search open (10-31 -> 38-40) and the home swipe.

### A/B of `metal-flow=6` over four boots (root208-root211)

`t-ab-flow.sh`: A (`metal-flow=6`) and B (the lowering thread), both `gpu-opp=stock vsync=120 tu-bc=8 m2v-async=1`, alternating
A B A B; each boot settles 90 s after the UI is up (`t-round.sh` SETTLE: 67-68 C down to 53-54 C, no guard trip), then two rounds
of t-actions. Animation frame rates (rounds where Settings did not open, and windows of 4 gaps, left out):

| Operation | A (root208, root210) | B (root209, root211) |
|---|---|---|
| Settings scroll | 112-115 | 109-114 |
| Home swipe | 68-73 (mean 70) | 65-73 (mean 68) |
| Control Center close | 56-77 (mean 65) | 48-77 (mean 58) |
| Settings open | 43-65 | 53-57 |
| Settings close | 52-61 (mean 57) | 50-57 (mean 54) |
| Search close | 52 | 47-57 (mean 51) |
| Search open | 38-42 (mean 40) | 30-36 (mean 34) |
| Control Center open | 21-35 (mean 30) | 28-35 (mean 32) |

A is kept. Control Center open is the slowest: its first frame comes 200-380 ms after the gesture in most runs, and root207 found
backboardd's encoding thread busy through it (668 of 1000 samples running), partly in pipeline variant compilation
(`RenderVariant::compiled` under `Device::owned_kernel`).

### What limits Search and Control Center now (root212-root219)

- **Not pipeline compiles.** root212 (`diag=1`, `t-mtdiff.sh`): one Control Center open made no pipeline; three made 9 (12 ms),
  every driver shader-cache lookup a hit; three Searches made 25 pipelines (50 ms). Search draws ~2900 render passes per run.
- **Not backboardd.** root213: its CoreAnimation display thread waited for the next vsync in 764 of 1000 samples during Search
  (637 during Control Center), in `swap_wait_timeout` 26-34. The frames are not produced faster than the content changes.
- **Spotlight spins.** root214/215 (`ustack` now 20 frames): the Spotlight process's main thread runs at 100 % of a core from the first
  Search on, idle included (1500 of 1500 samples running; `top` 1018-1019 per mille). Its deepest full stacks:
  AssistantServices -> `-[UIDictationConnection dictationConnnectionDidChangeAvailability:]` -> `-[UIDictationController
  postNotificationName:userInfo:]` -> `-[UISystemKeyboardDockController updateDockItemsVisibility]` -> `configuredGlyphWithName:` ->
  `+[UIImage imageNamed:inBundle:...]` -> `_UIImageCollectImagePathsForPath`: the keyboard dock is rebuilt, its glyphs looked up by
  path, again and again. Seed `leanlc3` (`com.apple.assistant.support` `Dictation Enabled` and `Assistant Enabled` false, root217)
  changes the branch taken (`+0x4dc` instead of `+0x510`) but not the spin; launchd logs no lookup storm (root218: 261 lines in
  12 s); booting without the Wi-Fi arguments (root219) does not stop it either. The trigger is queued to the main queue and is not
  on these stacks.
- **The key store sees the loop.** Console `aks` (root224/225): `aks_get_device_state` (selector 17) is called about 365-450 times a
  second by Spotlight (pid 305, `ps`) after a Search, every other selector quiet; our AppleKeyStore sends no notifications, so these
  calls are the loop's lock-state reads (`deviceStateIsLocked`), not its trigger. Reporting the lock state as unlocked everywhere
  (`aks-sls=3` left out, root225) does not change the rate. In root224/225 `top` 20 s after the Search no longer lists Spotlight
  (busiest kernel_task 3.4 %) while the reads go on, against 100 % 10 s after it in root215/217/219: the spin fades, the polling stays.
- **Where the availability callback comes from** (`dsc-dis.py` now rebases addresses, follows branch islands and names their
  destinations; `dsc-xref.py` finds adrp+add references): the two AssistantServices frames under the delegate call are block
  functions (0x1992fe714: load a weak reference, call a block; 0x1992fe770: respondsToSelector:, then the delegate method with the
  stored argument). 0x1992fe770 is referenced once, at 0x199305094, in a function (0x199305054) that builds that block and hands it
  to a queue; 0x1992fe714 at 0x19934f058. Who calls 0x199305054, with what availability, is the next step up.
- **It is the network availability that flips.** The delegate's `-[AFDictationConnection _tellSpeechDelegateAvailabilityChanged]`
  (found through the relative method list with the cache's selector base, `dsc-method.py`) comes from `networkAvailability:isAvailable:`,
  whose block announces a change only when the new value differs from `_lastBroadcastNetworkAvailability`: Spotlight's
  `AFNetworkAvailability` (a path monitor, the `daemon.network.available` notify state, live DNS reachability) flips between
  available and unavailable hundreds of times a second on this phone, which has no joined network. Seed `leanlc4` (assistantd also
  removed, root226/227) does not change it: Spotlight 1018 per mille, ~470 device-state reads a second. A joined Wi-Fi network would
  settle it, but joining stops at the 4-way handshake: the Wi-Fi driver receives no data frames (status-ios27.md, root239).
- **assetsd** sometimes sits at ~100 % at idle too (root214/215), and two widget extensions after a data reset.
- Wi-Fi: one boot without `wifi=5 wifi-userspace=1` (root219) looked smoother, but the A/B over four boots (`t-ab-wifi.sh`,
  root220-root223, seed leanlc3) shows no difference: without / with Wi-Fi, Settings scroll 114-115 / 111-116 fps, home swipe 68-75 /
  68-75, Control Center close 58-84 / 54-89, Settings close 55-67 / 53-64, Search close 55-69 / 49-66, Search open 35-41 / 40-50,
  Control Center open 34-38 / 19-62. Wi-Fi stays.

### Targeted drain wake-ups (`m2v-wake=1`, root228-root235)

root228 (Control Center open, seed leanlc4): backboardd's encoding thread ran in `psynch_cvbroad` (syscall 303 under
`pthread_cond_broadcast`) in 108-114 of 1000 samples (root207: 162-184). The frontend's encoding thread broadcast `idle` after every
batch, waking a thread in `encodeDrain` that then found its own batch not done and slept again. `adrenometal-drain-wake.patch`
(boot argument `m2v-wake=1`) wakes the drainers only once the lowest batch one of them waits for is done. root230: 0 samples in
`psynch_cvbroad`; encoding thread 1622 ms per 8.2 s against 1717.

A/B over four boots (`t-ab-wake.sh`, root231-root234, A with it, B without; all `NOSLS=1`, which is needed to fit the
1000-byte command line), fps per valid round:

| Operation | A (root231, root233) | B (root232, root234) |
|---|---|---|
| Settings scroll | 111, 114, 114 | 118, 112 |
| Home swipe | 71, 69, 76 (mean 72) | 72, 74, 66, 76 (72) |
| Control Center close | 62, 87, 61, 88 (75) | 56, 88, 89 (78) |
| Settings close | 64, 59, 63 (62) | 54, 60, 58, 58 (58) |
| Search close | 53, 58, 60, 60 (58) | 55, 53, 55 (54) |
| Control Center open | 50, 33, 66 (50) | 32, 56, 25, 51 (41) |
| Search open | 31, 40, 43 (38) | 40, 44, 40 (41) |

The wake-ups are gone but frame rates differ only within the run-to-run spread; `m2v-wake=1` stays opt-in.
root230 also showed 193 samples of a 2016-byte copy into a fresh 9456-byte `RenderLowering` box (`RenderLowering::assemble`), possibly
page faults on memory xzone malloc's deferred reclaim had returned. root235 did not see it in four captures, and the
`vm.reclaim.*` sysctls are not writable from the console (error 1), so that lead is closed.

### Render pipeline latency with asynchronous submission (root236)

Pass and command buffer trace (`diag=1`, `NOSLS=1` to fit the command line; seed leanlc4, `gpu-opp=stock vsync=120 tu-bc=8
m2v-async=1 metal-flow=6`), per render command buffer (`cb-timing.py`):

| | Search, root201 (before) | Search, root236 | Control Center, root236 |
|---|---|---|---|
| commit -> first kernel submit, median / p90 | 1.4 / 14.6 ms | 1.9 / 2.5 ms | 1.0 / 3.0 ms |
| commit -> last pass end, median / p90 / max | 5.2 / 15.7 / 20.0 ms | 2.4 / 3.8 / 5.9 ms | 2.4 / 7.9 / 16.4 ms |
| GPU busy per command buffer, median / p90 | 0.8 / 4.4 ms | 0.7 / 0.9 ms | 0.7 / 3.5 ms |

Every Search frame now reaches the GPU's end within a 120 Hz frame, so Search's 31-44 fps is limited before the render server (the
Spotlight network availability loop). Control Center's tail comes from one pass kind: 16 passes of 2.95 ms each (87 draws, 790
fragment instructions, 10 textures; the Control Center backdrop blur), at 680-903 MHz under the touch boost.

The GPU clock is not the limit during gestures: the governor's touch boost (`gpu-dcvs-boost-ms`, 1000 ms) holds at least 680 MHz
after every touch; in root234 the GPU spent 30-70 % of each 10 s window at 903 MHz with the GPU zone at 53-61 C, below its cap.

### Deferred reclaim off (`vrc=0`, root237, root241-root245)

root237 (SpringBoard main thread, inclusive over its running samples): Control Center opens 39 % in `CA::Transaction::commit`, 30 % in
`CA::Context::retain_all_contexts`, 37 % in xzone malloc's slow path (`__xzm_xzone_malloc_freelist_outlined`); home swipes 15 % commit,
44 % UIKit's update scheduler. Suspecting page faults on blocks whose pages the deferred reclaim had returned, `vrc=0` (in the
Data-mount hook before launchd: `vm_reclaim_enabled = false`, writable on this DEVELOPMENT kernel; the `vm.reclaim.*` sysctls
answer error 1 to the console) starts every process without a reclaim buffer. A/B (`t-ab-vrc.sh`, `NOSLS=1`, settle 90 s;
root243 hit the 70 C guard before measuring and was replaced by root245), mean fps of the rounds:

| Operation | A `vrc=0` (root241, root245) | B (root242, root244) |
|---|---|---|
| Settings scroll | 114 | 111 |
| Home swipe | 70 | 72 |
| Control Center close | 79 | 65 |
| Search close | 68 (59 without one 95) | 61 |
| Settings close | 53 | 53 |
| Settings open | 48 | 52 |
| Search open | 40 | 38 |
| Control Center open | 31 | 41 |

No consistent gain (Control Center close better, open worse, the rest within the spread); `vrc=0` stays opt-in and off.

### Page faults, assetsd and the thermal caps (root246-root254)

- **Faults.** System faults (console `vm`): 1300-2000/s at idle, 3400-6200/s through Control Center opens (root246). Per task
  (console `top` now lists `HOUJI TOPF`, TASK_EVENTS_INFO faults; root247): assetsd ~2000/s at all times (and a full core), backboardd
  ~7500 and SpringBoard ~3200 per Control Center open. Page zeroing is not the cost (console `zbench`: `pmap_zero_page` ~70 ns per
  4 KiB, root248), and SpringBoard's main thread spends 8 % of its CPU in the kernel through Control Center opens (`threads` now prints
  system time, root249: 296 of 3720 ms), backboardd's render threads 17-25 %. `gpu-pf=1` (`unixnu-adreno-prefault.patch`,
  kIOMapPrefault on BO CPU views) did not lower backboardd's faults (root253: 19997 over two opens): IOKit prefaults only memory
  wired through prepare(), and device-pager views are populated at map time already, so the BO views are not the source. Next
  suspect: IOSurface imports (host-pointer BOs) of new layer surfaces.
- **assetsd** removed in seed `leanlc5` (launchd cache CDHash 8bbe3068...); no other effect seen yet (root251).
- **Thermal.** With `houji_thermal_trigger_c=60` the governor sat at levels 1-2 through the tests (hottest TSENS 60-66 C) and capped the
  big clusters at 1.4-2.0 GHz. `t-round.sh` now uses `TRIG` (default 68) and the guard `GUARD_C` 74 (below the governor's 75 C
  TSENS emergency); a 68 C trigger under a 70 C guard tripped the guard during boot (root252, 72 C). root254 (68/74, leanlc5): no guard
  trip (peak 73.5 C at boot), level 0 through most of the actions, clusters at 2.2-2.4 GHz, now capped by the stock skin table
  (battery 37 C on USB power: 2.19-2.32 GHz; the table never allows more than 2.63 GHz). Frame rates against root251 (throttled):
  home swipe 71/78 vs 68/71, Control Center close 58/88 vs 63/85, Settings close 58/64 vs 61/58, Search 39-52 vs 33-50, Control
  Center open 20/48 vs 57/47, Settings scroll 114 vs 112-113. Roughly 40 % more clock moved the rates by a few fps: the limit is not
  CPU throughput.

### Where the frames go: per-frame gaps (root236, root254)

Render command buffer commits (root236 trace, `diag=1`): within an animation the render server commits every 8.3 ms, 26-30 frames in a
row, each complete 3-5 ms after its commit; between them 100 ms cadence (Control Center at rest) or 33-42 ms (Search, Spotlight's
updates). The render server and the GPU keep 120 Hz. Frame gaps per action from the swap ring (root254, 0.8 s after the first frame):

- home page swipe and Settings scroll: 7-9 ms almost throughout, single 17-34 ms gaps and a 33 ms tail pull the averages down;
- Search open: 23-33 ms throughout (Spotlight);
- Control Center open: 166 ms to the first frame, then 6-41 ms, irregular;
- Settings open: a 211 ms gap first, irregular, then 8 ms;
- closing with the bottom swipe: 8 ms, then 15-35 ms including runs of exactly 16-17 ms (60 Hz), then 8 ms again.

The low rates are in the phases where an application or SpringBoard has to produce new content (Spotlight, Preferences, Control
Center's first frames) and in 60 Hz stretches of some system animations, not in the render server.

### One fixed 120 Hz refresh (`vsync-fixed=1`, root255-root258) and a shorter test cycle

With `vsync=120` the framebuffer publishes `IOMFBDisplayRefresh` with a 1/120 s minimum and the original 1/24 s maximum interval, a
24-120 Hz range; `vsync-fixed=1` (an existing GopFramebuffer switch, never measured) publishes one fixed 120 Hz rate. Suspected cause
of the 60 Hz-paced stretches (gaps of exactly 15-18 ms, `frame-gaps.py`). A/B over four boots (seed leanlc5, trigger 68 C, guard
74 C): share of 15-18 ms gaps when closing an application 20 / 14 % with it, 24 / 20 % without; home swipe 4-8 % both; frame rates
within the spread (home swipe 66-74 / 68-80, Search open 38-45 / 40-43, Settings close 56-71 / 57-63, Control Center close 52-74 /
59-73). Not the cause; it stays off.

Test cycle: the fixed 300 s cool-down in fastboot barely changed the boot (first reading 43-49 C after 61-120 s, 36-48 C after
300 s, 57-67 C 60 s into the boot either way, root232-root258). `t-round.sh` `COOL=auto` waits 60 s plus 5 s per degree the last
reading was above 60 C (at most 120 s), and `SETTLE` is now a maximum that ends once the hottest sensor reads `SETTLE_C` (default 50 C),
after at least 20 s.

### Control Center open: thread timelines, BO cache floor and lowering box reuse (root261-root273)

Console `tl` (1 ms samples of chosen threads with their user stacks; root261 on) with `tl-view.py` (per frame gap), `tl-cat.py`
(per category) and `tl-first.py` (gesture to first frame). Control Center open, backboardd's encoding thread, root265 (2 s after the
gesture): `m2v-wake=1` cut its condition-variable broadcasts 190 -> 16 ms (root264 vs root265) and stays on. Of what was left, 72 ms
were 70 Turnip `BoAlloc` calls and 43 ms 42 `BoMap` calls, nearly all for command stream chunks (`tu_cs_reserve_space` ->
`tu_bo_init_new_explicit_iova` -> `darwin_bo_init`, symbolized with the unstripped Turnip): the BO cache had aged out its BOs
(`tu-bc=8`) since the last action. And 54 of its 79 ms of `madvise` were libmalloc returning an emptied small chunk at once after
freeing metal2vulkan's `RenderLowering` box (about 10 KiB) at every pass end (`RenderLowering::end_pass`, symbolized with the plugin's
link map). `MallocXzoneDeferSmall=1` for SpringBoard and backboardd (seed leanlc6) changed nothing measurable.

Two opt-in fixes (GPU userspace gpu9, seed leanlc7): `tu-bk=M` keeps the newest M MiB of the BO cache past its age
(`turnip-darwin-bo-cache-keep.patch`; memory pressure and the 256 MiB cap still empty it) and `m2v-lbox=1` keeps up to 8 ended
lowering boxes for the next render encoders (`m2v-lowering-box.patch`). Encoding thread `madvise` in an open 87 -> 30 ms (root267 vs
root268). Control Center only A/B (`t-ab-cc.sh`: six opens 12 s apart, home button between them): with both, opens at 23-54 and
34-81 fps (root269, root271, mean 52); without, 21-54 fps (root270, mean 42). Kept on.

What the open waits for now (root273, `tl` with each run's CPU and runnable waits): from the gesture to the first frame 167-312 ms, of
which SpringBoard's main thread ran 116-240 ms on the big cores (CPUs 2-6, moving among five of them), waiting for a CPU at most 7 ms;
backboardd's threads ran under 10 ms. The later gaps of 58-180 ms are the same thread. It is CPU-bound in SpringBoard's own building of
the Control Center views (UIKit, CoreAnimation, malloc). The governor log shows the stock skin table capping the clusters at
2.03 / 2.19 / 1.82 (prime) / 2.19 GHz at a 38 C battery on USB power.

More CPU clock did not help (Wi-Fi off, `t-ab-cc.sh` / `t-ab-ccf.sh`, open fps means per boot): C (the two fixes) 61 / 44 / 60,
D (+ `houji_skin_offset_c=-14`, caps 2.63 GHz) 58 / 48, E (+ `houji_cpu_frame_pct=100`) 57, F (+ `houji_cpu_load_sum=1`, new: a
domain's load summed over its cores) 50. The Control Center close swipe sometimes paged Control Center to its music page; the tests
close it with the home button from root281 on.

The shared L3 cache clock does (console `l3`, `l3 vote N`, `l3 mode N`; one boot alternating settings per round, `t-cc1.sh` with
console `dvfs set` / `l3` commands, no reboot per setting). The bootloader leaves the EPSS L3 vote at row 12 (1555 MHz of 365-2035)
and nothing in this kernel changed it. root283: L3 at 365 MHz, opens 24-44 fps (mean 36), closes 60-86 (69); at 2035 MHz opens 48-85
(65), closes 93-98 (95). root284: left at 1555 MHz opens mean 49, closes 86; `houji_l3=1` (new: the top row while touched or drawing,
else the lowest row at or above the fastest CPU domain's request) 64 / 96; always the top row 55 / 95.

**Those Control Center numbers were mostly its music page.** Screenshots after every open (`t-actions.sh` `CCSHOTS=1`, root286-root288):
only the first open of a boot showed the main page. The open drag to y=1500 carried on into Control Center's pages once it was warm,
and it reopens on the last page shown. A drag to y=700 opens the main page every time (eight opens in a row, root288); the bottom-edge
swipe closes it. `t-cc1.sh` (one boot, settings switched per round through console commands) uses these from root288 on. Real main-page
opens: 35-41 fps, first frame 165-190 ms.

- **DDR / LLCC vote** (console `ddr Y`, RPMh BCM MC0 and SH0 through the application RSC): top vote against none, opens 55 / 61 and
  65 / 51 fps (root285, root286): no gain, and the hottest sensor rose from 48 to 66 C within seconds. Not used.
- **Turnip host allocations** (`turnip-darwin-alloc-cache.patch`, `tu-ac=1`, GPU userspace gpu10, seed leanlc8): Turnip makes and frees
  a 7072-byte `tu_framebuffer` per render pass (`tu_CreateFramebuffer` -> `vk_object_multizalloc`); the boot argument keeps freed
  4-64 KiB blocks of Mesa's default allocator per malloc size class (8 MiB in all). Opens 41-47 fps, mean 44.8, against 33-38, mean
  35.5 (root290 / root291, eight opens each, `houji_l3=1` both). Kept.
- **Metal bridge allocations** (`adrenometal-recycle-alloc.patch`, `m2v-ac=1`, gpu11, seed leanlc9: a recycling global allocator for
  the Rust bridge): opens 44.0 against 42.9 fps (root294 / root295). No gain.
- **The `tl` sampler was biased.** It read a running thread's saved user state, which is only as new as the thread's last kernel entry,
  so running samples piled up where the thread had last faulted or called the kernel: root296 put 807 of 3691 ms of the encoding thread
  in the last store of one 456-byte `memcpy`, while `threads` showed that thread at 4.5 % system time over three opens. From kernel
  `7e515eb9` `tl` reads a running thread's registers by a cross-call to its CPU (`houji_sample_running`, HoujiCpuIdle.c; 2100 of 7500
  samples a capture). The madvise and malloc slow-path shares measured with the old sampler (root261-root296) are overstated.
- **What limits the open now** (root297, cross-call samples, seed leanlc9, `tu-ac=1`): the first frame 37-189 ms (SpringBoard's main
  thread, mostly UIKit layout); the first ten or so frames at 6-12 ms; then from about +320 to +950 ms every frame lasts as long as
  backboardd's encoding thread works on it (17-30 ms, never waiting for a CPU), against 3-5 ms in the fast frames. Over that stretch
  (1566 ms of encoding over three opens) draws are 46 % (metal2vulkan `draw_indexed` / `draw_resident`), `end_encoding` 24 %,
  render encoder and command buffer setup most of the rest, with no single hot function: the translation's per-draw cost. Reduce
  Transparency and Reduce Motion were already on in the seed.
- **Not helping either:** metal2vulkan's lowering thread (`metal-flow=7`): opens 37.9 against 44.4 fps (root298 / root299); the
  governor's summed load or a 100 % frame floor at run time (console `dvfs set`, root300): 37.5 / 38.5 against 37.0.
- **Per call** (`adrenometal-zz-counts.patch`, `m2v-count=1`, GPU userspace gpu12, seed leanlc10: the bridge counts its draws, render
  encoders made and ended and commits with their time, and notes the totals to the kernel's pass log, console `notes`). Control
  Center open, steady part (root302, 524 ms): 6712 draws at 31 us each (211 ms), 838 render encoders made at 34 us and 879 ended at
  88 us (105 ms), 79 commits at 181 us (14 ms): 63 % of the time on the encoding thread, about 22 frames, so per frame about 305
  draws (9.5 ms) and 38 render passes (4.6 ms). A draw's 31 us is spread over `RenderLowering::draw_resident`'s steps (recording,
  settling the bind state, the pass begun inline at its first draw, binding; root302 cross-call samples) with no single hot function.
  120 Hz needs about 8 ms a frame: draws at about a third of their cost and passes at about half.

### Compositor fast path: the census (milestone 1, root316-root319)

The largest command buffer of a steady Control Center frame carries 62 % of the frame's encoding work (median of 16 frames,
`m2v-count=2` per-commit notes, `cb-split.py`, root317): too little to split for parallel encoding to reach 8 ms a frame, so the plan
is a reduced translation path for the render server's simple passes. Its census (`m2v-lowering-box.patch` `houji_census.rs`, the bridge's
census notes, console `notes`; GPU userspace gpu17, seed leanlc15; `t-census.sh`, `census.py`), five Control Center opens and closes
(root319, backboardd, 24679 passes, 121226 draws):

- **Passes:** 80.3 % use none of the features counted (rate map, instance splits, stencil shadow, depth stand-in, imageblock, tile
  dispatches, executions, pending pipelines, the replay log, colour groups, barriers inside, unnamed memory reads, visibility queries);
  19.7 % read input attachments (framebuffer fetch), and those hold 81 % of the draws. Encoding time from a pass's making to its end,
  its draws included: 4663 ms for the input-attachment passes (959 us each), 4079 ms for the plain ones (206 us each).
- **Draws:** none uses a counted feature (tessellation, meshes, shader tests, memoryless textures, barriers, the replay log, colour
  groups, rerouting, replaying, extended range); 24.4 % are the existing "drawn again" path at 14 us each (420 ms), 75.6 % the full path
  at 49 us each (4458 ms). 21 % of draws use the pipeline of the draw before; 137 pipelines since boot, 11 cover 50 % of draws, 46 90 %,
  85 99 %.
- **Set calls per draw:** 1.10 buffers, 1.27 buffer offsets, 0.60 textures, 0.40 samplers, 0.86 pipelines, 0.60 viewport/scissor, 0.01
  bytes; 550 ms in all against 4878 ms of draws (a pipeline set 2.2 us, the rest 0.4-1.2 us; bytes 23.6 us but rare).
- **Where a draw's and a pass's time goes** (root319, three opens, cross-call samples, `split.py`, by the innermost own frame): draws
  81 % metal2vulkan and the bridge, 12 % the system (libmalloc, memcpy, locks, kernel), 7 % Turnip; render encoders made 51 / 37 / 12 %,
  ended 55 / 32 / 13 %.

### Compositor fast path: milestone 2, first step (root323-root331)

- **The 14 us floor** of the runtime's own "drawn again" path (`draw_rebound`, root321 cross-call samples, `rebtree.py`): Turnip's
  recording about 16 %, the push of set 0 about 10 %, the rest the rebind's bookkeeping (`refresh_changed`, `keep_draw`, the uses and
  their notes, `retain_into`, image descriptors); the bridge and the entry are negligible.
- **Why draws miss it** (root323, gpu19, five opens): 27 % rebound, 7 % unlike the kept draw, 12 % rebind stopped, about 53 % with no
  kept draw. The runtime already keeps a settled bind state per pipeline inside an encoder (`LiveBind::select`); across encoders only
  the last kept draw carries, into the next encoder of the same pass shape, and that never happened: 4396 carries dropped for another
  pass layout, 111 for a call between, 0 carried.
- **The shelf** (`m2v-lowering-box.patch`: a command buffer keeps up to four more carried bind states, the opening encoder takes the
  newest of its shape; switched at run time by console `m2v fast N`, which sets the kernel's host name `m2v-fast-N` that the bridge reads
  at most every 50 ms; boot argument `m2v-fast=1` would pass the 1000-byte command line). A sysctl of the console's own could not carry
  the switch (root325), and writing the boot arguments panicked the phone (root327). Result (root331, gpu22, windows on/off/on/off):
  with the shelf on, every pass missed it (27876 and 29248 misses, 0 taken, 0 evicted, 11334 and 11924 let go at commits): **within a
  command buffer no carried pass shape comes again**. Per draw 38.9 / 36.8 us on against 47.9 / 35.9 us off: no change. The repetition is
  across frames (command buffers), which the carry's lifetime (its settle's objects are its command buffer's) does not reach.
- **The test guard** read the CPU governor's DVFS lines, whose instantaneous die sensor spikes to 75-77 C for 50 ms under the boot's
  full load at skin 22 C (root329, root330 stopped at random); it now reads the 5 s `HOUJI THERMAL` hottest sensor (still 74 C).

### Compositor fast path: milestone 2, why settled draws miss the rebind (root333-root339)

- **Dry run of a cross-frame key** (census `CNSX`, root337): half the draws (50 %) are their pipeline's first use in their encoder and
  all take every step; key A (pipeline, pass shape, layers) of every one of them was seen in an earlier command buffer (98 % within
  100 ms), and so was key B with the attachments' textures and render area (98 %): the key reaches no per-frame object. Key A' (with the
  draw's call and primitive type) too, 100 %; but only 72.7 % of those keys' last draws kept a draw that rebinds.
- **Why draws of a pipeline already settled in the pass take every step** (`CNSW`, `CNSL`, `CNSR`, root335/root337): of the same
  pipeline as the draw before, 82 % unlike the kept draw, by another primitive type (5923) or call (4153) or index buffer (4207); of a
  return to an earlier pipeline (A>B>A), 45 % a kept draw that does not rebind, 30 % no kept draw, 17 % a stopped rebind. Every kept draw
  that does not rebind (35 % of kept draws) holds uses beyond its plan's, and those are its **input attachments' alone** (framebuffer
  fetch): the rebind remade a draw's uses of its plan alone.
- **Input attachment draws rebind** (`m2v fast`; the keep allows the plan's uses plus the input attachments' exactly, a draw rebound
  makes those again of its pass, `input_attachments_into`, and its check of the attachments' reads leaves them out as every step does;
  root339, gpu26, one boot, windows on/off/on/off, no crash): drawn again 32.8 / 33.6 % of draws on against 24.9 / 24.2 % off, stopped
  rebinds 4.1 / 4.2 % against 12.5 / 12.3 %, about 11000 draws a window rebound with their input attachments, and the cross-frame
  projected hit 100 % against 72.9 %. Per draw 39.9 / 38.5 us on against 47.9 / 39.1 us off (the second window of a boot reads high in
  every run): no clear gain yet, as the full path's draws left are the dearer ones (50-52 us against 47-58 us).

### Compositor fast path: milestone 2, what a first use costs (root341-root347)

- **Per draw by path** (root345, gpu29, fat LTO, `m2v-count` census on): drawn again 14 us, rebound with its input attachments made again
  18 us, every step of a pipeline the same as the draw before's 26 us, back to an earlier one's 33-37 us, the **first use of a pipeline
  state in its encoder 58-62 us**: half the draws and about three quarters of draw time. The same-pass draws the input attachment rebind
  took were already cheap (26-37 us), so the mean moved little.
- **What a cross-frame entry could hold.** A kept draw's bind state holds the bound buffers and textures themselves, its descriptor
  writes, vertex buffers and recording raw Vulkan handles (`VkImageView`, `VkBuffer`, `VkPipeline`), all of a frame; stripped of those,
  an entry holds the settle alone (variant, plan, shape, facts), which a pipeline state keeps already (`GraphicsState::settle_shape`, 4
  settles).
- **The settle hits** (root347, gpu30, first uses that took every step): 93 % by identity, 5-8 % by an equal shape, 1.5 % made again.
- **48 set-aside states** (`m2v fast` bit 1, windows 1/3/1/3, no throttling): a first use finds its pipeline state held 93 % of the time
  against 30 %, but a held one is no cheaper (56-65 us against 53-54 us), the mean per draw 42.9 / 38.1 us against 38.3 / 37.9 us, and
  passes cost the same (made 38-40 us, ended 85-101 us).
- **Where a first use goes** (root347, two opens, cross-call samples under `houji_first_use`, `firstuse.py`; 79 % of draw time):
  spread over the steps, none above 11 %. Turnip's recording 11 % (and 2 % beginning the pass's first instance), `draw_resident` itself
  9 %, the settle's checks of the bind state (`settle_draw`: shape, `refresh_reading`, comparisons) about 14 %, instantiating the plan's
  commands 6 %, the pass's first instance (`inline_begin`, its framebuffer) 4 %, the push of set 0 4 %, the uses 3 %; by leaf 71 %
  metal2vulkan, 15 % the system (allocation, copies), 14 % Turnip.
- **The flat profile's tail** (`firsttail.py`, same samples, by the innermost metal2vulkan function and its Turnip or system leaves):
  no cross-cutting cost; drops and reference counts 3 %, allocation about 4 % (`finish_grow` and its leaves), locks 0.2 %. The larger
  pieces: the recording of the draw's state commands with its Turnip and system leaves about 24 %, the settle's checks about 13 %,
  `draw_resident`'s own (inlined) body 10 %, the framebuffer of the pass's first instance about 6.5 %, the push of set 0 about 6 %.
- **The settle's steps, timed** (root349, gpu31, first uses that took every step, about 62000 a window, tier on and off alike):
  `settle_draw` 7.2-7.3 us whole: the bind state refreshed from the encoder's tables 2.6-2.7 us, its fields reshaped 0.6 us, its shape
  found 2.4-2.5 us (kept 40 %, recent 13 %, among the pipeline state's settles 45 %, made 1.4 %), its settle found 0.9 us. A first use
  in a new encoder reads fresh tables, so the refresh is its own work; at best a shape kept across encoders saves about 2 us.
- **Passes and commits** (root349): a render encoder made 35-42 us, ended 83-100 us; a commit 380-409 us.

### Milestone 3: passes, commits and shadow copies, profiled (root347-root353)

- **Where they go** (root347, two Control Center opens, cross-call samples, `passprof.py`): a pass's end has 2.8 times the samples of
  its opening and 4 times those of the commits. Its end: `end_pass` 83 % of it; shadow copies and their planning 20 % (the copies'
  commands recorded, `shadow_copies` > `record_gated`, 9 %; their objects retained 3 %; `plan_shadows` and its helpers 6 %); allocation and
  drops 19 %, nearly all `end_pass` dropping the encoder's own structures (argument tables, retained objects, the bound record, the
  extended shadows, the shape memo); `release_pass` 9 % (and Turnip under it 7 %); the framebuffer 1.6 %. Its opening: the command
  buffer's allocation (system) and beginning (Turnip) about 24 %, the render pass's lookup 4.5 %, the encoder's own assembly and layout
  about 29 %. A commit: submission, scheduling and fences 52 %; the census's own work about 14 % (absent without `m2v-count`).
- **Why the copies** (root351-root353, census `CNSH`, `CNSI`, `CNSA`): every shadowed texture (160 after five opens) is refused in place
  for its placement, and every placement refused misses on size, none on alignment or memory type: the linear image takes more bytes
  than its memory holds from the texels on (the last: 8192 bytes at offset 64 of 8128 bytes of room). A pass takes 1.02 copies, all in,
  each the texture's first in its command buffer (95 % of the texels IOSurfaces'), none skipped; 5118 passes in a window render into a
  shadowed attachment.
- **The placements' spread** (root354, census `CNSG`, `CNS0`-`CNS5`, 533 misses): all size, all in memory that ends on a 4 KiB page, the
  driver's pitch the texture's in every one. The excess is Turnip's: `fd6_layout.c` rounds a last level's height to 4 rows (the GMEM
  blits' 16x4 over-fetch) for every linear image, and, for a 2D image it lays out itself, its size to a page. Full-screen 1200x2670
  BGRA8 layers (178 of them, most of the copied texels) are 2 rows over, RGBA16F 256x1 / 609x273 / 633x273 / 831x1077 ones 3 rows,
  small A8 ones a page at offset 64. Upstream's `turnip-render-mode-for-attachment-memory.patch` already loads and stores a linear
  attachment over host or explicit-layout memory over its texels alone where its height is not whole blocks
  (`tu_attachment_unpadded`), in GMEM; the IOSurfaces themselves are Apple's IOSurface kext's.
- **Unpadded linear images** (`turnip-tight-linear.patch`, boot argument `tt=1`: such an image, linear, one level, one layer, 2D, no UBWC or
  sparse, by the same test as `tu_attachment_unpadded`, takes no over-fetch rows and no page rounding). Census (root356, gpu35, census on,
  unthrottled): 0 placement misses (837 fit), 0 shadowed textures, shadow copies 0.42 a pass (from 1.03), a pass ended in 76.8 us (from
  93.5-101.6, root353/root354), made in 34.6 us (from 36.9-39.4), a commit 296 us (from 412-437); no GPU hang, restart or SMMU fault
  (console `gpuf`), no crash. Control Center (root357 against root358, the same seed, `tu-ac=1`, `l3 mode 1`, five rounds each, the same
  thermals, GPU busy 6 % both): opens 36.4 against 36.8 fps, closes 66.4 against 69.0 fps; the first frame's delay bimodal in both (3-30 or
  130-176 ms; 1 of 5 slow against 3 of 5). Not yet checked: the pixels, the copies left (no texture shadowed for its placement), and
  Turnip's render-mode counters, which read 0.

### What an open frame is made of (root292-root359)

- **The fps measure** (`t-actions.sh`): the swaps' gaps in the 0.8 s from the first frame on, gaps over 100 ms left out; the first
  frame's delay is apart. 44 against 36 fps is no change of configuration: the boot arguments of root299 (44.4) and root357 (36.4) are
  the same 57, the L3 votes the same, and root299 and root300 (37.0), 15 minutes apart, ran the same build and seed. Opens by vsync
  periods: the fast boots 22-28 % of their frames in one (8.3 ms) and 3-6 % at 50 ms or more, the slow ones 5-10 % and 10-12 %; most
  frames in both take three (25 ms).
- **The frame budget** (console tl, `framebud.py`: SpringBoard's main thread, backboardd's CoreAnimation render thread and its Metal
  encoding thread, by state, over the open's frames): root359 (gpu35, `tt=1`, no census, three opens) 25.0-26.6 ms a frame, the encoding
  thread running 19.5-20.8 ms of it, SpringBoard's main thread 12.1-13.2 ms beside it, the render thread about 1 ms, none of them
  4.1-4.4 ms, runnable and not running about 0.1 ms. A fast boot (root292) 21.7-23.0 ms a frame with 17.5-18.9 ms of encoding, a slow one
  (root296) 26.8-28.9 ms with 21.0-23.6: the frame follows the encoding thread, 3-4 ms past it. The first frame (157-187 ms) is
  SpringBoard's main thread's (110-134 ms: UIKit layout and traits, the CA commit).
- **Where the encoding thread runs** (the same samples): cpu2-cpu5, never the prime core cpu7, in fast and slow boots alike.
- **The encoding thread on the prime core** (root361, gpu35, `tt=1`, one boot, sides unbound / bound / unbound / bound, two opens each;
  `houji-bind-cpu.patch` and console `bind PID:NAME cpuN`: the scheduler's clusters, console `topo`, are cpu0-1 and cpu2-7, so a cluster
  bind cannot reach cpu7 alone). Bound: the encoding thread 10.0-10.8 ms a frame against 19.0-21.1 ms, at a like clock (its domain
  1.76-1.98 GHz against 1.72-2.15 GHz; the prime domain's governor then keeps it near 1.8 GHz); opens 35.6 / 45.5 / 41.4 / 45.5 fps against
  39.4 / 37.0 / 41.2, frames 22.0-28.1 ms against 24.3-27.0 ms. The encoding thread now waits 11.1-13.7 ms a frame between its runs (from
  4.1-5.7 ms): the frame is no longer its own. SpringBoard's main thread 11.9-14.9 ms a frame beside it, the render thread about 1.3 ms;
  no runnable time to speak of. The boot also had one GPU hang and restart at fence 519, at boot before the sides, with one SMMU
  translation fault (FSR 0x80000002, FAR 0x11d6680), console `gpuf`; root356 (also `tt=1`) had none.
- **GPU hangs with and without `tt=1`** (root362-1 to -4, `t-hang.sh`, tt 0/1/0/1, the best config, console `gpuf` and `gpus` after the
  boot's settle and after eight Control Center opens): at boot, hangs in both (tt=0: 2 and 0; tt=1: 2 and 3, and 1 in root361), the
  faults at low or null addresses (0, 0x5ae1000, 0x1ad7340): an older fault, not this patch's. During the opens: none with tt=0, 2 and 1
  with tt=1, read faults at user addresses (0x15a770a40, 0x158ac8940), the CP stalled at the same IB's start both times (event writes
  0x19, 0x18, 0x28, 0x33, then a wait for idle). `tt=1` stays off until those are understood.
- **What the encoding thread waits for on the prime core** (root361, `waits.py`, the tl samples' waiting stacks): one run segment a frame,
  each woken in the bridge's encode thread loop, where it waits 12.8 ms a frame bound against 2.2 ms unbound: it is starved, waiting for
  the render server's next frame of commands, not for a fence or the vsync. SpringBoard's main thread runs 15.1 ms a frame, 6.5 ms of it
  while the encoding thread runs (unbound: 10.6 of 11.8 ms); the render thread waits about 92 % of the time in a kernel trap (a message
  receive, as far as the unsymbolized stack shows).
- **SpringBoard's main thread over the open** (root361, `sbprof.py`, tl stacks symbolized through the shared cache): steady frames
  11.9-14.9 ms running a frame, UIKit (inclusive) 6.9-7.9, the leaf in the Objective-C runtime, objc_msgSend or malloc 4.3-5.9,
  Swift/SwiftUI 2-3, Control Center modules 2.4-2.9, CoreAnimation commit about 1, syscalls and IPC about 0.8, software rendering
  about 0. First frame: 93-118 ms running, UIKit 48-78, the runtime and allocator leaf 47-55, layout and traits 13-18, Swift 7-14.
- **Clock floors at the max** (root363, tt=0, encoding bound to cpu7, sides B/H/B/H, H = `dvfs set framepct 100` and `touchpct 100`):
  the domains went from about 1.7-1.9 to 2.63 GHz (+50 %); encoding 11.0 to 9.1 ms a frame (mean, -18 %), SpringBoard's main thread
  13.4 to 12.7 ms (-5 %), the frame interval 27.1 to 24.7 ms, the open 37.2 to 40.6 fps (B 32.7/41.4/36.9/37.7, H 41.8/42.5/38.2/39.8,
  within the per-boot spread). SpringBoard's time barely follows the core clock: governor lag is not what holds it.
- **L3 and DDR at the top, and SpringBoard on the prime core** (root364, `t-bind.sh` sides B/M/B/M/S, encoding bound to cpu7 in B and M):
  the L3 read during every open was already at its top row (2035 MHz, `l3 mode 1`) in B, so M (`l3 mode 2`, `ddr 16383`) tested the
  DDR/LLCC vote alone (no read-back of the DDR clock): real opens SpringBoard 12.5 against 12.4 ms a frame, 40.1 against 40.7 fps: no
  effect. S (SpringBoard's main thread bound to cpu7, encoding to cpu5): SpringBoard 6.4 ms a frame (from about 12.5) and a 67 ms first
  frame (from 100-118) at about 2.0 GHz, the same clock as on cpu2-6; encoding on cpu5 21.6 ms at 2.6 GHz, the open 36.3 fps. Both
  threads run about twice as fast on the prime core (Cortex-X4, its 2 MiB L2) as on the others at one clock: a core and L2 limit, not
  the memory clocks. There is one prime core.
- **SpringBoard's runtime bucket** (root361 and root363 real opens): steady 4.4 of 12.7 ms a frame, libobjc (retain, weak references)
  2.1, objc_msgSend 1.7, malloc (xzone, no instrumented paths) 0.6; dispatch slow paths 0.03; no per-frame trait or Auto Layout
  invalidation (0.01 each). First frame: 21.6 / 16.2 / 6.9 ms, with trait changes 6, hashing 8.3, strings 4.6, Auto Layout 2.9.
- **All three threads on the prime core** (root365, `t-bind.sh` sides B/P/P+ twice, three opens each; P: SpringBoard's main thread,
  backboardd's render thread and the encoding thread bound to cpu7; P+: also the frame and touch floors at 100 %): open fps mean
  48.6 / 49.3 / 51.2 (B 43.6-51.2, P 45.7-50.4, P+ 49.7-52.2), frame interval about 20.6 / 20.3 / 19.5 ms. SpringBoard running 11.0 /
  4.8 / 4.2 ms a frame, but runnable and waiting 0.1 / 8.4 / 6.8 ms; encoding 9.2 / 8.1 / 7.2 ms; the first frame 166-183 / 128-134 /
  110-117 ms (SpringBoard 119-131 / 78 / 61-67 ms). Peak hottest 61 C in the first B side, about 46 C on P and P+. The frames' timelines
  (`ftl`-style, 0.5 ms cells): SpringBoard is ready at each vsync and waits while the previous frame's encoding runs: the encoding
  thread copies the render thread's fixed priority 54 (`encodeAdoptReference`), SpringBoard's main thread is at 47.
- **Why the floors stop at 2.63 GHz:** the stock thermal policy (`houji_cpu_policy=stock`) caps cpu2, cpu5 and cpu7 at 2630.4 MHz from
  25.0 C skin (the first step of `kStockCpu2/5/7`), and the skin reads about 27 C on a cool phone: in effect always.
- **Clocks and caches by core** (console `cpub N`, a kernel thread bound to one CPU, root365 after the opens): 200M dependent adds in
  76.1 ms on cpu7 (2628 MHz, the reported 2630), 80.5 ms on cpu5 and cpu2 (about 2485 MHz, 5.5 % under the reported 2630). A random
  pointer chase, ns per load at 32K/64K/128K/256K/512K/1M/2M/4M: cpu7 1.5/1.5/1.9/2.4/2.5/5.5/6.4/7.1, cpu5 1.5/1.5/3.4/3.9/4.6/5.4/
  5.7/5.7: L1 and past 1 MiB alike, 1.6-1.8 times the latency from 128 to 512 KiB on cpu5. Matches the 2x of SpringBoard and of the
  encoding thread on cpu2-6: their L2, at their real clocks; whether a cache control bit is set differently was not read. So encoding
  spread over cpu2-6 alone would not beat cpu7.
- **Priorities on the shared prime core, and the prime core over 2.63 GHz** (root366, `t-bind.sh` sides P+/Q/R/Q7/R7 twice, three
  opens each, all three threads on cpu7 with the floors at 100 %; console `bind PID:NAME priN` / `tsN`, `dvfs set p7cap`): Q (encoding
  at fixed 45, under SpringBoard's 47) and R (SpringBoard at fixed 56) take SpringBoard's runnable wait from 7.0-8.1 ms a frame to
  0.1-1.7, but the encoding thread then waits 5.7-8.9 ms, SpringBoard's own running grows from 4.1-6.1 to 5.8-9.3 ms, and the opens
  fall: P+ 45.2-49.3 fps, Q 35.9-42.9, R 37.7-41.6, Q7 36.4-43.0, R7 37.2-43.0 (frame interval 20.3-22.1 against 23.2-27.9 ms).
  One core runs the two in turn whichever goes first; priority only moves the wait. In every side 7-9 ms a frame pass with none of
  the three threads running. p7cap 3149: the domain reports 3149 MHz, but `cpub 7` then times the add chain at 2629 MHz (and at
  2627 MHz with the domain reporting 499 MHz), and the opens match Q and R: the reported clock is not the core's, and the cap
  override does not raise it. Kept off. The houji thermal trigger (66 C) switched levels 0 and 1 six times during the run.
- **The prime core's clock by vote** (root367, `dvfs set p7pin MHz` holds cpu7's vote at a table row, `cpub 7`): voted 499, 1497,
  2630 and 3148 MHz (the last with `p7cap`), the add chain ran at 2626, 2626, 2625 and 2627 MHz. The vote does not set cpu7's clock:
  it stays at about 2.63 GHz whatever the governor asks, so the P to P+ and B to H differences were not cpu7's clock.
- **Frame timelines with every CPU** (root367, console tl now also records which thread each CPU runs every millisecond and the GPU's
  interrupts and fence passes, `gapbud.py`): other threads on cpu7 delay one of the tracked threads only 13-35 ms per 2.9 s capture;
  the largest is the timeline's own sampling thread (a new kernel thread each capture, up to 28 % of a core). In two of six captures
  BackgroundShortcutRunner used 4.2-4.9 s of CPU in 2.9 s, mostly off cpu7. The kernel's vsync tick never ran (the display reports
  `sent=0`): nothing registers for it on iOS 27, frames are paced in backboardd. And the thread followed as "the render thread" since
  root361 was the wrong one: the console cuts names at 23 characters, so `com.apple.coreanimation.display` matched tid 775, the
  render-server thread (200 ms of CPU in the boot); backboardd's compositing thread is tid 785, `com.apple.coreanimation.display.`
  (5978 ms, second to the encoding thread), never followed or bound. While none of the followed threads ran, it ran 63-107 ms of
  650, about 2.5-4 ms a frame, on cpu2-6. Threads can now be named by id (`PID:#TID`).
- **The compositing thread on the prime core, and the GPU's time** (root368, `t-bind.sh` P+ and P4 twice, three opens each; the
  compositing thread `com.apple.coreanimation.display.` is followed as `PID:#TID`, tid 789 that boot; P4 binds it to cpu7 as well; tl
  also records each ring write pointer write, `writeWptr`): it runs 9.5-11.4 ms a frame on cpu2-6 (P+), 5.5-6.8 ms on cpu7 (P4), more
  than SpringBoard's main thread. Opens: P+ 34.2-42.0 fps (mean 38.9), P4 42.9-45.4 (mean 44.1); frame interval P+ 23.8-29.3 ms, P4
  22.0-23.3 ms. With it followed the idle time is gone (`gapbud.py`, eight frames each): P+ 24.2 ms a frame with a followed thread
  running, 2.4 ms only the GPU, 1.1 ms nothing; P4 23.0, 0.7 and 0. In P4 cpu7 is full: about 17.5 ms of work a frame, and
  SpringBoard waits runnable 15-18 ms, the compositing thread 7.6-9.6, encoding 4.3-5.8. The GPU: 3.2-5.6 ring writes a frame, each
  answered by its interrupt a median 10.7-14.3 ms later; the union of those spans covers 55-60 % of the frames (13-16 ms a frame), an
  upper bound on the GPU's work. The GPU DCVS reads 8-14 % busy at its top level (903 MHz) over 10 s windows that hold about 1-1.5 s
  of opens: the 5-8 % read before was the window's idle, not the frames'. Swaps retire 36-68 ms after their submission.
- **hakios-arm64e-dada's in-place rendering of padded targets** (its `metal2vulkan-720ef1c-dada.patch`, `texture.rs`
  `stores_stay_in_rows`: one slice and level, height a multiple of 4, the 16-aligned row within the pitch, the last aligned row within
  the buffer), against root354's census of shadowed textures (`CNS0`-`CNS5`, 457 of the 533 with their shapes): 1 passes (187x52 A8),
  0.2 % of the textures and 0.0017 % of their texels. 456 fail the height (275 at height % 4 = 1, 180 at 2, 1 at 3: the 178 full-screen
  1200x2670 layers, the 216 256x1, the 273- and 1077-row ones), 609x273 the row too; with the height test swapped for "the 4-row tail
  fits the buffer" 3 tiny A8 ones pass (the full-screen buffers keep 384 bytes past 2670 rows). dada's panel is 1200x2670 too
  (`base/common/devices/dada.platform.c`): its full-screen layers are shadowed as well. Not ported.
- **Render pass objects per pass** (metal2vulkan 555b13d, the base on the Mac): no churn. `render_target::final_pass` (called at
  every pass end, `render.rs` 5449, 5847, 6126) and `compatible_pass` (per encoder, `render.rs` 2046) both go through `create`
  (`render_target.rs` 1629), which returns the device's kept `VkRenderPass` of the same shape, operations and layouts
  (`KeptRenderPasses`, up to `KEPT_PASSES` = 256) and makes one (`render_passes_made`) only on a miss: what dada's
  `kept_final_pass` (256) adds to its older base. Nothing to port.
- **The prime core's domain** (code audit, `HoujiCpuDvfs.cpp`): EPSS domains 0-3 are cpu0-1, cpu5-6, cpu7, cpu2-4 (`kCpuDomain`,
  `kCounts`, the stock tables' order); cpu7 is domain 2 (table cap 3052.8 MHz). The opt-in `p7cap` / `p7pin` (root366, root367) acted
  on domain 3, cpu2-4, and `cpub` and `framebud.py` named the domains as if cpu7 were 3: root366's Q7 shows cpu2-4 at 3149 MHz and
  cpu7 at 2630 (`framebud.py` corrected), root367's sweep pinned cpu2-4 while timing cpu7. Corrected (`kPrime = kCpuDomain[7]`);
  cpu7's scaling is untested so far.
- **Placement A/B and the GPU's own time** (root369, `t-bal.sh`, tt=0, the floors at 100 %, the tl sampler on cpu0-1; P4: SpringBoard's
  main thread, the compositing thread (tid 841 that boot), the render-server thread and the encoding thread on cpu7; PC: the same with
  SpringBoard's main thread on cpu3; traced opens with tl and the driver's job timeline switched on from the console, `gtl`, untraced
  ones with neither): untraced P4 43.3 fps (6 opens, 39.4-48.9, first frame about 100 ms), PC 41.3 (40.9-41.6, first frame 151-163
  ms); traced P4 38.2-43.9, PC 39.5-46.0. In PC SpringBoard's main thread runs 10-14 ms a frame on cpu3 (2.4-3.4 on cpu7), and on cpu7
  the compositing thread still waits 8.5-9.6 ms a frame runnable, the encoding thread 5-6. 0 GPU hangs, 0 faults, no crash.
  The GPU (CP always-on stamps around each job, `gpuact.py`): in the opens' steady window it is active 82-98 % of the time, 20.4-23.6 ms
  a frame, about 3.2 jobs a frame, each queued a median 13.5-15.5 ms before it starts. A frame is four back-to-back jobs (2 us apart):
  about 1.2, 0.56, 13-16 and 7-9.5 ms. The GPU DCVS spends 56-75 % of each 10 s window at its top level (903 MHz) and reads 8-22 %
  busy, about one open's 0.7-0.8 s of saturated GPU: the two agree. The GPU, at its top clock, is now what sets the frame time.
  root369 then reset in `dvfs lut` (reading vote words past a domain's own; fixed, and the dump now runs first in a boot).
- **cpu7 scales on its own domain** (root370, `p7pin` / `p7cap` now on domain 2, `cpub 7`): pinned at 1500 MHz it timed 1474 MHz, at
  2630 2627, at 3052.8 (the cap lifted) 3047 MHz. Its factory table (`dvfs lut`): rows to 3052.8 MHz with bits 18:16 = 3, then 3110.4,
  3187.2, 3244.8 and 3302.4 MHz with 1 (Linux `qcom-cpufreq-hw`'s boost rows), the last repeated; `kCaps` stops at 3052.8. Domain 3
  (cpu2-4) has no boost row to 3148.8. Control `0xb0` = 0x9b30 in every domain (bit 0 clear: one vote word a domain).
- **The stock caps follow the room** (root370, root371): the skin estimate (battery temperature) read 22.7-23.7 C, under the stock
  tables' first step (25.0 C), and every domain ran to its table cap (cpu2-4 3148.8 MHz). root370's opens peaked at 75.1 C (one
  sensor, between 42-45 C readings); root371's boot load reached 76.3 C and `t-round.sh`'s 74 C guard sent it to the bootloader. Runs
  now hold the stock caps with a skin offset (`sko=4`, a 7-byte alias of `houji_skin_offset_c`; +4 C keeps 22.7-27 C inside 25-32 C,
  where cpu7's table stays at its first step).
- **A GPU hang in a diag=1 boot** (root370): one, no fault, no guilty job, at fence 6383, the job after the last one of an open's
  settle (fences 5883-6382): a Control Center hang. root369 (gtl from the console, diag=0) had none in 24 opens. diag=1 stays a
  measurement boot only.
- **cpu7 at 3052.8 MHz** (root372, `t-bal.sh` P4 / P7 twice traced, then untraced; `sko=4` held the stock caps: skin 28.5-29.3 C,
  cpu2-6 at 2630.4 MHz; P7 lifts cpu7's alone, `dvfs set p7cap 3053`; `cpub 7` timed 3049 MHz there): traced P4 40.8 fps (6 opens,
  34.7-46.7), P7 43.3 (41.4-45.7); cpu7 averaged 2.68-3.00 GHz over P7's frames; the compositing thread 6.4 -> 5.4 ms a frame, the
  encoding thread 8.4 -> 7.4. The GPU stayed 95-97 % busy, 20.5-23.4 ms a frame (P4 77-97 %, 21.2-23.9). Untraced P4 42.6 (3 opens);
  P7's first untraced open (39.1 fps) read 75 C and `t-bal.sh` stopped at its 70 C limit. 0 GPU hangs, 0 faults in 15 opens. About 6 %
  traced, not confirmed untraced, at a thermal cost: `p7c` stays opt-in and off. The GPU sets the frame time.
- **The GPU's time inside and outside render passes** (root373, diag=1, P4 six opens; the driver's CP stamps per job joined with
  Turnip's pass stamps from frames.trace, both on the CP's always-on counter, `gpujoin.py`): 154 frames, 21.7 ms of GPU a frame, 1.8 ms of
  it inside the passes read. The read is incomplete: 0.7-12.9 passes a frame of about 38 (Turnip's ring holds 512 passes, about 0.3 s
  at this rate, and the frontend reads it 300 ms late). Of the passes read, the full-screen 1200x2670 BGRA ones (GMEM, loaded and
  stored, 2.6 a frame) take 1.9 ms a frame. Even three times the passes read leaves most of the GPU's time between passes (copies,
  blits, clears, barriers and flushes), as root194 found (5.9 of 14.3 ms of a Control Center command buffer in passes). A second
  diag=1 boot, a second Control Center GPU hang: side 2, SMMU fault (FSR 0x80000002, FAR 0x4abf4000, FSYNR0 0x170001), guilty job.
  diag=1 boots 2 of 2 with a hang in the opens, diag=0 boots root369 and root372 0 in 39: diag=1 is not used again.
- **Shadow copies are about 5 ms of GPU a frame** (root374, `tt=1`: Turnip's unpadded linear images, no shadowed texture; P4, the
  stock caps held, CP stamps from the console, diag=0): the GPU 16.3-18.3 ms a frame (75-93 % of the window) against 21.2-23.9 with
  tt=0 (root372); the encoding thread 5.9-7.4 ms against about 8.4; traced opens 52.2 fps (6, 45.0-56.2) against 40.8; untraced 53.4
  (6, 47.1-58.2) against 42.6. 0 GPU hangs in the 12 opens; 2 at boot (fences to 1444, before the first open's 4973; FAR 0x4e904000,
  low, like the tt=0 boots' boot hangs). The tt=1 Control Center faults of root361-362 (user addresses 0x15a770a40, 0x158ac8940) did
  not recur here; the next boot repeats tt=1 for 16 opens with the driver naming the mappings around any fault (console `gpuf`).
- **tt=1 again, with the fault map** (root375, P4, 16 opens; the driver records the mappings around a fault's address, `gpuf MAP`):
  0 GPU hangs or faults in the opens; 2 hangs at boot (fence 1706, before the first open's jobs at 7797). The boot fault's address,
  0x14864c0, lies in a hole of a VM holding two driver buffers (kind 0, 0x302000 +4 KiB and 0x10000000 +4.1 MiB): a stray low address,
  not an over-read past an image. GPU 17.2-18.0 ms a frame (90-94 %), traced 54.3 fps (6, 52.0-56.0), untraced 52.6 (6, 49.1-56.4).
  With root374, tt=1 has 0 Control Center hangs or faults in 2 boots and 25 opens: it meets the bar for an opt-in setting. The frame is
  GPU-bound at about 17.5 ms, with about 15 ms of work on cpu7.
- **Concurrent binning on top of tt=1** (root376, `tu-cb=1`: Turnip's BV thread bins a pass while BR renders the one before; P4,
  16 opens, 998 bytes of command line): GPU 17.5-20.7 ms a frame (86-95 %) against 16.3-18.3; the encoding thread 7.2-9.4 ms against
  5.9-7.4; traced 47.9 fps (8, 45.3-52.8), untraced 43.4 (5, 38.5-48.7) against 52.6-53.4. 0 GPU hangs or faults the whole boot. Slower:
  stays off.
- **Where the GPU's time outside render passes goes: a code and data audit** (no boot; root370, root372-375):
  - *Not ring overflow.* Within every run of passes read, Turnip's pass numbers are contiguous (up to 526, no duplicates, 1-3 single
    gaps), and Turnip notes every render pass at one place (`tu_cmd_render`). About 13 render passes a frame is real.
  - *No missing Metal passes.* The "38 a frame" was an estimate: a census window is five open+close cycles with no frame count.
    Per cycle metal2vulkan made about 5,200 passes (root354) and Turnip about 6,500 (root373). A Metal pass is one or more Vulkan
    render pass instances, never none (`render.rs`); the carry (`render/carried.rs`) and inline-first (`render/inline_first.rs`)
    paths reuse a bind state and record inline, and merge nothing.
  - *The census render-mode counter (CNSM) reads 0 because of its gate.* `tu_render_mode_counts` is counted only under
    `tu_pass_trace_on()` (SHENG_DIAG).
  - *Where the outside time sits in a job* (root373, 248 jobs of 3 ms or more with passes, tt=0): before the first pass 2.26 ms mean
    (median 1.75), between passes 2.40 (0.72), after the last 2.76 (2.52). Stamps bracket the whole tile loop, loads and stores included.
  - *The jobs with no render pass.* The bridge's scanout presenter (`GpuScanoutPresenter.h`) copies each frame (1200x2670 BGRX,
    12.8 MB) from the drawable into the scanout buffer with a compute dispatch (`scanout_opaque`), its own command buffer: the
    0.5-0.6 ms passless job of every frame, 0.50 ms a frame with tt=0 and tt=1 alike.
  - *GPU ms a frame by job length*, tt=0 (root372, 161 frames) vs tt=1 (root374-375, 405 frames): under 0.8 ms 0.50 / 0.50, 0.8-2 ms
    0.85 / 0.78, 2-6 ms 0.64 / 1.61, 6-10 ms 7.23 / 4.75, 10 ms and over 13.30 / 9.81; 22.5 / 17.5 in all. The shadow copies were inside
    Core Animation's big jobs. With tt=1 about 14.5 ms a frame is in jobs over 6 ms, of four passes (about 1.5 ms) each: about
    10-12 ms a frame outside render passes is still unattributed (compute dispatches, transfers, barriers and flushes).
- **The GPU's time of an open frame, accounted** (root377 reset boot, root378-root379; GPU userspace gpu36, seed leanlc34: `tt=1`,
  P4, `sko=4`, `m2v-lbox=1`, the tl sampler on cpu0-1; `turnip-houji-op-trace.patch` stamps compute dispatches, the ten transfer
  commands, cache flushes outside passes, event waits and primary command buffers into Turnip's pass ring, now 8192 entries, each with
  CP_EVENT_WRITE7 RB_DONE_TS of the always-on counter before and after as u_trace does, nothing without SHENG_DIAG; render-mode counts
  no longer need it; `gtr=1` (`adrenometal-zzzz-gtr.patch`) sets SHENG_DIAG alone, without diag=1's kernel diagnostics: 996 bytes of
  command line; `gpuops.py`, every 10 us of a job to the innermost entry over it or to where in the job it lies):
  - Safety: root379 0 GPU hangs or faults the whole boot; root378 one, before the measured opens (fence 5563, the first open's jobs
    from 17471), at 0x38d60000 in a hole between two driver buffers, no guilty job: the boot-time kind.
  - Overhead: GPU 15.4-17.2 ms a frame traced (16.3-18.3 without the trace, root374-375), traced 51.8 fps (4 opens, 46.2-55.4).
  - 121 frames, 16.37 ms of GPU a frame: render passes 7.76 ms (47 %: fullscreen 1200x2670 BGRA GMEM passes over unpadded linear
    surfaces 6.58, 9.2 a frame, about 0.7 ms each even with 1-3 draws, their whole surface loaded and stored; fullscreen SYSMEM passes
    the autotuner chose 0.70, 7.0 a frame, about 0.1 ms each; 28.7 small GMEM passes 0.48); each job's head before its first command
    buffer 2.01 and tail after its last 1.99 (24 %: about 0.6 ms at each end of 3.2-3.5 jobs a frame, where the kernel's submission
    prologue and fence epilogue run; the job stamps are CP-time, Turnip's end-of-pipe); gaps of 0.5 ms or more between command
    buffers 2.21 (13.5 %: mostly right after the scanout copy's command buffer, 1-7 ms with nothing stamped: the GPU waiting inside
    the job); jobs with no command buffer entry 0.94; compute 0.71 (the scanout copy, shader 9257011e, grid 75x334, 16x8, about
    0.49 ms each); cache flushes outside passes 0.46 (bits 0x21ff 20.8 a frame, 0x3 16.6); command buffers otherwise 0.11, small gaps
    0.13, transfers 0.06. About 45 render passes a frame (the "13" of root370-373 was the 512-entry ring read late).
- **The three largest GPU costs, audited** (root379's traces and the sources, no boot):
  - *Fullscreen GMEM passes.* All 9.2 a frame are single-sample without depth/stencil, render area 1200x2670; the ring keeps no input
    attachment flag. The same fragment shader with as many draws costs 5-10 times less where the autotuner chose SYSMEM: `7ebccf52`
    over 40 draws 2.76 ms (76 draws) in GMEM against 0.33 (47) in SYSMEM, 11-40 draws 1.25 against 0.20; `b0100a10` 11-40 1.15
    against 0.11, over 40 2.86 against 0.48. In GMEM they pay about 30-40 us a draw over a 0.7 ms floor (1 draw 0.72 ms, 3 draws 0.72),
    in SYSMEM 5-7 us. Turnip's BANDWIDTH autotuner (`tu_autotune.cc` 1009-1033) weighs load+store bytes x pixels x 1.1 against the
    draws' samples x bytes, and nothing of the unpadded attachment's GMEM loads and stores or of draws replayed per tile. Passes of
    more than 3 draws: 4.2 a frame, 5.0 ms; of 1-3 draws 5.0 a frame, 3.7 ms. The mode table's 8.72 ms against the exclusive 6.58: no
    overlap with other entries (0.00 ms); 2.1 ms of pass time lies outside its job's stamps (CP-time job stamps against end-of-pipe
    pass stamps). Boot argument `tum=s` (`tu_frame_diagnostic.h`) renders every pass in SYSMEM: a test needs no build.
  - *The gaps after the scanout copy.* 90 % of the gap time of 0.5 ms or more (244 of 272 ms) follows the scanout copy's command
    buffer (compute `9257011e`), a median 4.2 ms after each (p90 5.6); jobs ending with the copy have a median tail of 2.2 ms (375 ms in
    all), the others 0.33 (77 ms). Turnip's Darwin submission emits no GPU wait (no `CP_WAIT_*` in `tu_knl_darwin.cc`). The copy is a
    compute dispatch, which RB_DONE_TS does not follow: its end stamp comes before its writes end, and the rest shows as the gap and
    the job's tail, until the next flush or the job's fence waits for it. The copy is then about 0.71 + 2.0 + 1.9 = 4.6 ms of GPU a
    frame: 12.8 MB read and 12.8 MB written in about 4-5 ms, about 5 GB/s, as if the GPU wrote the scanout pages uncached (they are
    an import, `newBufferWithBytesNoCopy`, whose owner may map them uncached, driver kind `ImportUncached`; not verified). The
    frontend's own record, offer to copy returned: median 5.4 ms, p90 24.8 (queued behind the frame's work).
  - *Job heads and tails.* Tails: 83 % is the copy's (above). Heads: about 0.6 ms per job (median 602 us; 807 after a copy), 2.0 ms a
    frame at 3.2-3.5 jobs: the kernel's prologue flush, invalidate and wait for idle come before the job's start stamp, so a head is
    the time from there to the first command buffer's start, real per-submission work. Jobs by content: copy and render 99, copy, other
    and render 51, other and render 93, render 50, copy alone 26 (median 2.5 ms).
- **Every pass in system memory** (root380 / root381, gpu36, seed leanlc34, P4, `tt=1`, `sko=4`, no gtr; root381 adds `tum=s`,
  Turnip's existing switch that renders every pass in SYSMEM and backs transient attachments at the pass; untraced opens with the
  driver's job stamps from the console, `gtlbusy.py`, then a traced side): untraced 40.2 fps (4 opens, 38.2-43.9), GPU 19.7 ms a frame,
  first frame 126 ms, against **64.4 fps (5 opens, 55.6-77.5), GPU 8.1 ms a frame, first frame 111 ms**; traced 44.4 against 71.8
  (65.4-78.0). On cpu7 with tum=s: the compositing thread 3.4-5.0 ms a frame, the encoding thread 3.2-5.8, SpringBoard's main thread
  0.9-5.4 (from 6.3, 8.2-9.6 and its usual 2.5-3.5). Peak hottest 67.3 C in both boots. Hangs: root380 one before the opens (fence
  1617, no fault); root381 none in its 8 untraced opens, **two in the traced side** (fence 27593, a guilty job, SMMU read fault at
  0x149816880, about 360 MiB past the last mapping of the VM, a 80 MiB system-memory BO at 0x12d537000, kind write-combined, nothing
  mapped above it). tum=s as it is fails the guardrail; it also backs Metal's memoryless attachments in memory, which a narrower switch
  would not.
- **The scanout copy's mappings and kernel** (sources): the scanout target is mapped Normal non-cacheable (Turnip's imports ask
  writeback 0, outer-shareable; the SMMU's MAIR0 0xff44 makes index 0 Normal NC, index 1 write-back), not Device memory; the drawable
  it reads is an import the GPU only reads, mapped write-back inner-shareable and IO-coherent (`NativeAdrenoUserClient.cpp`), its
  reads snooped. The kernel (`shaders/scanout_convert.ll`, `scanout_opaque`) copies one pixel a thread, a 4-byte load and a 4-byte
  store, alpha forced to 0xff because the display blends the scanout by its alpha (SSPP SRC_FORMAT 0x237ff); 3.2 M threads in 16x8
  groups. Core Animation cannot draw into the scanout: one firmware framebuffer, no flip.
- **A narrow SYSMEM switch and a four-pixel scanout copy** (GPU userspace gpu37, seed leanlc35; boot argument `hs=N`,
  `turnip-houji-sysmem-unpadded.patch`: a pass over an unpadded linear colour attachment whose render area is the framebuffer's, a
  million pixels or more, renders in system memory, bit 0 every one, bit 1 those of more than 3 draws, tested after the lazily
  allocated attachments' check, so a pass with a memoryless attachment stays in GMEM; bit 2 the scanout copy four pixels a thread,
  `scanout_opaque4` in `adrenometal-zzzzz-hs-copy4.patch`; P4, `tt=1`, `sko=4`, `m2v-lbox=1`, 995 bytes; 12 untraced and 4 traced
  opens a boot, the driver's job stamps): untraced fps and GPU ms a frame: root383 `hs=5` 45.9 (35.6-56.8), 18.96; root384 `hs=6`
  47.3, 19.61; root385 `hs=1` 52.9 (6 opens), 17.41; root386 `hs=5` 54.4 (7 opens), 16.74; against tt=1 alone 40.2 / 19.7 (root380)
  and 52.6-53.4 / 17.5 (root374-375): no gain beyond the boots' spread. cpu7 a frame: the compositing thread 4.7-6.9 ms, encoding
  6.0-8.6, SpringBoard's main thread 1.2-3.7 (tum=s's 3.2-5.8 for encoding did not come with it). Peaks 70.0-72.3 C. Hangs: root383,
  root384, root386 at boot only (fences 2384-2444, before the opens' 8964 and up; root386's two at 0x944000 between the two driver
  buffers); root385 (`hs=1`, the old copy) one in the opens (fence 19943, after the 15th open's jobs), no fault, no guilty job. `hs=5`:
  0 Control Center hangs or faults in 2 boots and 32 opens; a screenshot with Control Center open (root386) shows it drawn whole, no
  tinted or translucent blocks. Why no gain (root387, `gtr=1 hs=5`, without m2v-lbox for the bytes): of the full-screen unpadded passes
  774 rendered in system memory by the switch and 1704 stayed in GMEM, all of them with 3 colour attachments; the switch's conditions
  leave a pass in GMEM where it has a memoryless attachment without memory (the check before it, kept on purpose) or where its render
  area is not the framebuffer's; the ring cannot tell which. tum=s moved those passes too, backing the memoryless attachments. The
  four-pixel copy (`43c8f163`, grid 19x334) took 0.90 ms stamped and was followed by 3.06 ms of gaps a frame (17 frames): no faster
  than the one-pixel copy, so its limit is not the instruction count.
- **Memoryless attachments in system memory: why the passes stayed in GMEM and a premature unmap** (sources, no boot):
  - *Why 1704 of the full-screen unpadded passes stayed in GMEM under hs.* The switch is tested after `use_sysmem_rendering`'s check
    that returns GMEM for a pass with a lazily allocated attachment without memory, and its own conditions are a render area equal to the
    framebuffer and a million pixels; metal2vulkan's framebuffer is the pass layout's extent (`render_target.rs`
    `framebuffer_over(..., layout.extent)`), the attachments', and these passes' render area is 1200x2670, so the remaining cause is the
    lazily allocated attachment: a Metal memoryless texture is a `TRANSIENT_ATTACHMENT` image (`image_plan.rs` 631) bound to a lazily
    allocated memory type (`memory.rs`, `MemoryClass::Memoryless`). All these passes have 3 colour attachments; the ring keeps only the
    first one's flags (linear, unpadded, loaded, stored), so which of the other two is memoryless, and its load and store actions, is not
    recorded. tum=s (`strict_sysmem`) skips that check and backs them (`tu_allocate_transient_attachments`, `tu_allocate_lazy_memory`).
  - *Their memory's lifetime.* Turnip allocates a lazily allocated memory's BO when a pass first needs it, at a GPU address reserved at
    `vkAllocateMemory` (`tu_bo_init_new_explicit_iova`), and frees it with the memory. Turnip's Darwin submission names only the
    command-stream BOs to the kernel (`darwin_submit_add_entries`), so a job does not keep its data BOs mapped; a freed BO is kept until
    the newest fence submitted by then passes (`turnip-darwin-free-during-submit.patch`) and then goes to the BO cache, where it stays
    mapped tu-bc seconds (8 here), or is unmapped. A lazily allocated BO is never cacheable (`tu_knl_darwin.cc` 946:
    `cacheable = cache_enabled && !(flags & TU_BO_ALLOC_REPLAYABLE) && !lazy_vma`): it is unmapped as soon as that fence passes. A
    memory freed while a command buffer that uses it is recorded but not yet submitted is then unmapped before the GPU reads it; every
    other BO is covered by the cache's seconds. metal2vulkan releases an image once no texture object and no command buffer holds it
    (`texture.rs` `ImageObjects::drop`); whether every path that ends with a memoryless attachment's release holds it to completion was
    not proven from the sources.
  - *Against the faults.* tum=s's fault (0x149816880) lies past every mapped BO of its VM, nothing above: where explicit addresses, not
    the pool, are; root362's tt=1 Control Center faults (0x15a770a40, 0x158ac8940) are in the same range, and in a default boot a
    lazily allocated attachment gets memory in a pass the autotuner renders in system memory, the same path, more rarely.
  - *Fix designs.* (a) Keep a freed lazily allocated BO mapped as the cache keeps the others (an aged list, the same tu-bc seconds and a
    size cap), then let the switch take passes with memoryless attachments too; (b) a pooled per-device scratch BO for transient
    attachments: their GPU address is fixed at `vkAllocateMemory` (the lazy VMA), so a shared BO cannot simply be bound; (a) is the
    smaller change.
- **hs=9: the full-screen unpadded passes in system memory, memoryless attachments too** (GPU userspace gpu38, seed leanlc36;
  `turnip-houji-sysmem-unpadded.patch` now also holds hs bit 3: the switch's passes go to system memory before the memoryless check,
  their lazily allocated attachments backed at the pass, and a freed lazily allocated BO stays mapped tu-bc seconds after its fence,
  over 256 MiB kept the oldest first but none freed in the last 500 ms, counts in `lazy-aged.txt`; root389, root390: P4, `tt=1`,
  `sko=4`, `m2v-lbox=1`, `hs=9`, 995 bytes, 12 untraced and 4 traced opens each): untraced **69.6 fps** (5 opens, 64.5-72.2), GPU
  **8.31 ms** a frame, first frame 108 ms; and **73.4** (7 opens, 65.0-90.4), **7.79 ms**, 37 ms; traced 75.2 and 78.9. cpu7 a frame:
  the compositing thread 4.1-4.5 ms, encoding 4.5-5.5, SpringBoard's main thread 0.8-2.1. Peaks 70.8 C. Lazily allocated BOs freed
  1 and 3 a boot, all kept past their fence and released by age, none by the cap, 68 MiB kept at most: the memoryless textures live
  long, the deferral barely ran. A screenshot with Control Center open is whole. Hangs: root389 none; root390 one in a Control Center
  cycle (fence 22811, between the jobs of open 10, up to 22714, and open 11, from 22839), no fault, no guilty job; its stall snapshot
  (FSR 0x400, FAR 0xedf7feffbf19, no IB found, the job pending about 1 s) matches root385's stall in the opens (hs=1) and root380's at
  boot (tt=1 alone, FSR 0x400, FAR 0xedf7ffffbf19): a stall seen without hs too, not a fault. hs=9 does not meet the guardrail of 0
  Control Center hangs (1 in 32 opens).
- **The FSR 0x400 stall: a ring switch the CP never finished** (sources and logs, no boot). The stall snapshot's `ring` words
  (`native_adreno_stall_ring`, `NativeAdrenoSubmit.cpp` `snapshot`): root380 (tt=1, boot) next 1617, waited 1621 on ring 1, CP on
  ring 1, a switch in flight, unfinished 2 on ring 0 and 5 on ring 1, waited 818 ms; root385 (hs=1) 19942/19943 on ring 0, CP on ring 1,
  in flight, 5 and 1, 506 ms; root390 (hs=9) 22810/22811 on ring 0, CP on ring 1, in flight, 5 and 0, 1014 ms. FSR 0x400 is the SMMU
  context's FSR.FORMAT (AArch64 descriptors), no fault bit: FAR and FSYNR0 are stale; no IB was found under either read pointer.
  - *Not a lost interrupt.* A switch completes when CP_CONTEXT_SWITCH_CNTL reads with STOP (bit 0) clear, in the interrupt or in any
    `driveSwitch` (every submission, retirement and every turn of a wait, `waitOnRing`), so a lost CP_SW interrupt delays it by at
    most a turn; for a second STOP stayed set (or the register could not be read). IFPC is off (`NativeAdrenoService.cpp`), so the
    write was not dropped by a power collapse. *Not a job's hang either:* after `PreemptSwitchTimeoutUs` (1 s) `driveSwitch` gives
    the second ring up (`markPreemptDead(2)`), `hangCheck` finds a ring-0 job on a CP left on ring 1 and calls it stranded: a restart
    with no guilty job and the jobs replayed on ring 0 (root390: hangs 1, guilty 0, replayed 4). The second ring stays given up for
    the rest of the boot (`preempt_dead_` is never cleared), so a boot has at most one such stall.
  - *Not decided from the logs:* whether the CP sat in ring-1 work it would not yield (root380: 5 ring-1 jobs left with the CP on
    ring 1) or idle on an empty ring 1 (root390: none left). The words that tell (the switch log `native_adreno_switch_log`: control
    word, RBBM_STATUS, CP_RB_RPTR/WPTR, both records; the timeout rows `native_adreno_switch_timeout`; the raw stall words) are
    recorded but no console command prints them, nor `gtl dump` a job's ring (`native_adreno_tl` slot 6).
  - *hs.* GTL dumps: hs runs 74-85 jobs/s against 40-58 (tt=1, hs=5/6), the render server's GPU timeline 205-208 idle gaps over 1 ms
    an open against 143-169; jobs of other processes 0.4-4.6/s, not following hs. A switch to ring 1 needs ring 0 idle, and each
    one needs a switch back: more gaps can mean more switches, but the count is not in the logs.
- **hs=9 on one GPU ring (`gpu-preempt=0`)** (root391, root392; kernel with the console's `gpup` and the ring fields of `gtl dump`;
  P4, `tt=1`, `sko=4`, `m2v-lbox=1`, `hs=9`, `gpu-preempt=0`, `tu-bk=64` left out for room: 999 bytes; 12 untraced and 4 traced
  opens each). Every job ran on ring 0 (`gtl dump`: r0 l0 in all 5781 jobs of root392's untraced opens), `gpup`: no second ring, no
  switch. Untraced **73.0 fps** (63.2-84.1), GPU **7.71 ms** a frame, and **70.5** (49.4-106.6), **7.95 ms**; first frame 113 ms;
  traced 66.2 and 74.5. cpu7 a frame: the compositing thread 4.0-5.6 ms, encoding 4.5-7.2, SpringBoard's main thread 0.9-2.7. Peaks
  71.6 C. Screenshots whole. Hangs: **none in the 32 Control Center opens**; root392 none at all; root391 two at boot (fence 12393,
  the opens from 17872), each with an SMMU fault (FSR 0x80000002, FAR 0x131e440, between the driver's 0x302000 and 0x10000000
  buffers, guilty jobs), the boot-time fault seen before in root386 under two rings: a different class from the switch stall. Against
  root389-390 (two rings): the same fps and GPU time within the run-to-run spread, the switch stall gone by construction.
  Taken as the baseline (hs=9, gpu-preempt=0). Known issue left open: the boot-time SMMU fault at 0x131e440 (root391, root386).
- **Toward 120 fps: cpu7 and the scanout copy, audited** (root389-root392 traced opens, `framebud.py`; sources; no boot):
  - *cpu7 is shared, not slow.* All domains ran at 2.58-2.63 GHz (sko=4's caps), cpu7 too. The four threads pinned there ran 10.4-14.4 ms
    a frame (compositing 4.0-5.6, encoding 4.5-7.2, SpringBoard's main thread 0.8-2.7) and waited for it runnable: SpringBoard's main
    thread 10.0-14.9 ms a frame, compositing 4.3-6.6, encoding 3.0-4.0. The encoding thread then idles 4.4-6.5 ms to the swap (the GPU
    and the copy). The opens with more encoding are the slower ones (root391 6.1-6.5 ms in three of four, root392's first 7.2); without
    a BO cache count in the logs, tu-bk=64's absence is not separable from the boots' spread.
  - *Cuts, by cpu7 ms a frame:* 3.05 GHz on cpu7 (p7c; Branch A measured about 1 ms each off compositing and encoding) 1.5-2;
    parallel encoding (the largest command buffer, 62 % in root317 under GMEM, stays; the rest to cpu5-6 at 1.6-1.8x the cost)
    1.7-2.7; SpringBoard's main thread off cpu7 0.8-2.7 (it ran 10-14 ms a frame on cpu3 in root369); tu-bk back 0-1 on cold opens;
    metal2vulkan and Turnip per-pass and per-draw work unknown under hs=9 (no function profile since GMEM). The compositing thread is
    QuartzCore's: only its clock and its core help it.
  - *The scanout copy.* `GpuScanoutPresenter.h` forces alpha with a compute kernel and falls back to a blit (`copyFromBuffer`, one call
    where strides allow). The SSPP format 0x237ff (ARGB8888, bit 8 alpha enable) was read on another board (device records,
    gpucomp26); houji's pipes, format and blend were never read. Clearing bit 8 (0x236ff, XRGB as the upstream DPU writes it) on the
    pipes that scan the framebuffer would let a blit replace the kernel and fix the translucent-alpha case by construction, but the copy
    moves the same 12.8 MB read (IO-coherent, snooped) and 12.8 MB written (Normal NC) a frame, and the four-pixel kernel showed its
    limit is not instructions: a blit's gain is unmeasured and may be small, heat nearly none. The driver's direct scanout
    (`NativeLegacyDisplay.cpp`, `dpu-scanout=2`: the pipes fetch the compositor's surface, no copy) needs `native_ufs_dpu_map`, a display
    SMMU window houji does not have (its constants are another panel's), and the same alpha change first.
- **Placement, cpu7's clock, the display pipes and the encoding thread's profile** (root393, root394; kernel with the console's `dpu`
  and the boot argument aliases `gp=N` (gpu-preempt) and `htc=N` (houji_thermal_trigger_c); P4/P7/PC/C7 sides, 3 untraced and 3
  traced opens each, `tt=1 hs=9 gp=0`, `tu-bk=64` and `m2v-lbox=1` kept: 979 bytes; root394 adds `m2v-count=2`, 991 bytes, the
  bridge's per-commit notes after each traced open and three opens with the encoding thread's user stacks sampled):
  - *Display (console `dpu`, read only, both boots alike, no fault).* MDSS 0xa0000000 (DPU 10). CTL0 alone active (INTF_ACTIVE 0x2),
    fetching VIG0 and VIG1 (FETCH_PIPE_ACTIVE 0x30000), staged on LM0 and LM1 (CTL_LAYER 0x100002d: both pipes at mix value 5, border
    out). Each pipe 600x2670 (VIG1 from x 600), SRC0_ADDR 0xfc400000, the framebuffer's physical address (a 32-bit fetch, no
    translation), stride 4800, **SRC_FORMAT 0x237ff** (ARGB8888 with bit 8, alpha enable), UNPACK 0x03020001, OP_MODE 0x80000000.
    Mixers: every blend stage read (0-3) has OP 0x400 and alpha word 0x00ff0000: the foreground's alpha is its constant 0xff (FG_CONST,
    the background the inverse), as upstream programs an opaque plane, so the mixer does not use the pixels' alpha. If the pipes'
    stage (mix 5, upstream's DPU_STAGE_3, base 0x68) is that one, forcing alpha in the copy is not needed on this panel and the
    presenter's blit would show the same image; not seen on the panel yet (the console's screenshot reads memory, not the mixer).
    Direct scanout of the compositor's surfaces (above 0x8C0000000) needs a display SMMU translation: the pipes fetch 32-bit addresses.
  - *Safety.* root393: 0 hangs, 0 faults. root394: one hang with an SMMU fault (FSR 0x80000002, FAR 0x51098740, between driver
    buffers at 0x30008000 and 0x100000000, a guilty job) at fence 27622, between the second and third traced P4 opens (480-509 s): the
    driver-buffer-hole fault of the boot-time hangs (root378, root386, root391), here inside a Control Center cycle.
  - *Placement (root393, traced, ms a frame on cpu7 running / waiting).* P4: compositing 4.5-4.9 / 6.2-6.5, encoding 6.1-6.2 / 3.7-4.5,
    SpringBoard 0.7-1.1 / 13.0-13.2, 68.8-72.1 fps. P7 (cpu7's cap 3053 MHz): cpu7 averaged 2853-2870 MHz, encoding 6.2-6.4,
    67.8-70.4 fps: no gain (P7u peak 70.6 C). PC (SpringBoard on cpu3): SpringBoard runs 5.4-6.6 ms there (6x), waits 0; compositing
    still waits 5.3-6.9, encoding 3.5-4.2; 70.7-78.4 fps. C7: 73.3-78.9 fps, encoding waits 2.5-3.5. Untraced (2-3 opens a side):
    P4 64.6, P7 68.9, PC 63.7, C7 77.0 fps; GPU 8.5-9.4 ms a frame. root394 (counting on) was slower in every side (traced 55-72 fps).
    With SpringBoard gone from cpu7 the compositing and encoding threads still queue on each other (about 10 ms of running a frame).
  - *tu-bk=64 back:* P4 encoding 6.1-6.2 ms (root393), not the 4.5-5.5 of root389-390: the rise was not tu-bk's absence.
  - *The largest command buffer's share* (`cb-split.py`; the 512-row note ring kept only an open's last ~200 commits): in the 12 heavy
    frames captured (P4, P7: 4.6-17.8 ms of encoding a frame) 55-64 % work-weighted, as under GMEM (62 %, root317).
  - *The encoding thread's samples* (3 x 1.5 s, 2 ms; `encprof.py` with gpu38's link map, `dsc-sym.py`; the console prints the top 24
    stacks, 850 of 1635 runnable samples): 44.5 % woken and waiting for a CPU in `pthread_cond_wait` (runnable); of the rest
    memcpy/memset 49 % (named callers `EncoderCore::end_with_replay` 69 samples, `EncoderCore::retain` 55, `made_framebuffer` 10),
    kernel traps 36 % (`Group::submit` 21, `CommandBufferShared::schedule` 10, the rest without a plugin frame), malloc 9 %, the
    plugin's own code 7 %.
- **The faults below 4 GiB, and the encoding thread's memory churn** (sources and logs, no boot):
  - *Where the faults are.* Every process's GPU space keeps the kernel's buffers at fixed addresses under 4 GiB (SQE 0x100000, ring
    0x200000, memptrs 0x300000, scratch 0x302000, preemption records 0x10000000, their SMMU pages 0x20000000, the second ring and its
    memptrs 0x30000000 / 0x30008000; `NativeAdrenoGpuContext.h`) and maps client buffers only from 0x1_0000_0000
    (`GpuClientVaStart`). All four faults read (FSYNR0 WNR 0, FSR TF) inside those gaps: root386 0x944000 and root391 0x131e440 between
    scratch and the records, root378 0x38d60000 and root394 0x51098740 between the second ring's memptrs and the first client buffer
    (page-table level 2, level 1 for root394: no table there at all). No client buffer can be under 4 GiB, and each address is the low
    word of a plausible client address (0x1_00944000 ... 0x1_51098740; client buffers seen at 0x1_0000_0000-0x1_6000_0000): a 64-bit
    client address read with its upper word 0.
  - *Not the kernel's or the CP's fetch.* The kernel writes each IB's address as two words (`NativeAdrenoSubmit.cpp` 605-606); its
    32-bit casts are its own buffers under 4 GiB. At root394's hang BR stood at word 0 of the job's first IB (CP_THREAD_CONTROL,
    events, CP_WAIT_FOR_IDLE) and BV in register setup: the read came from a GPU memory client, not the command fetch. metal2vulkan's
    heap entries keep both words (`bind/heap.rs` 1857).
  - *Not found from the logs:* which block read it and where the address came from. The driver reads FSR, FAR and FSYNR0 but not FSYNR1
    (the block, as msm decodes it), and its search of the guilty job's IBs for the low word (`findFault`, `native_adreno_stall_fault`:
    a match followed by a 0 word would be a command-stream address with no upper word) is recorded but no console command prints it;
    the guilty job's process is not recorded. The newest process to open the GPU changed at 3 of the 5 faults (widget extensions:
    chrono, Unity, com.apple.*; TVWidg at root394), the guilty spaces held 95, 574 and 3630 buffers: not one young process.
  - *The encoding thread's samples, read again.* `ustack` takes the registers saved at the thread's last kernel entry, and counts a
    runnable thread as running: its hot spots are where the thread enters the kernel, not where its user time goes. With the
    unstripped Turnip (`build/mesa-tu-ios27`), they are first touches of fresh allocations and submissions: `vk_object_multizalloc`
    (85 samples, the zeroing of Turnip's `tu_framebuffer`, which metal2vulkan creates for every pass, `render_target.rs`
    `made_framebuffer`, no cache), `EncoderCore::retain` (55: a 2320-byte tuple boxed at every pass's end, `render.rs` 6365),
    `end_with_replay` (69: a 456-byte record appended to the command buffer's state, its vector regrown), `darwin_queue_submit` (21);
    `madvise` and the other kernel traps without a frame are the rest. Their milliseconds a frame are not in these samples.
- **Fault attribution, the compositing thread off cpu7, the encoding thread's system time** (root395-root397; kernel with
  `unixnu-adreno-fault-diag.patch`: FSYNR1, UCHE_CLIENT_PF and the guilty job's pid, queue and fence in `gpuf`, the IB search in
  `gpus`; `tt=1 hs=9 gp=0`, `tu-bk=64`, `m2v-lbox=1`, 979 bytes, root397 `gtr=1` 985; `threads 65` before and after every open):
  - *Faults.* root395: one at boot (fence 3496, the opens from 18352), FAR 0x1_38808380, past the guilty space's last mapping (a 12.4
    MiB buffer at 0x1_2477f000), as tum=s's. root396: one at boot (fence 2531, FAR 0xc261000) and **one in a Control Center cycle**
    (fence 35525, between the opens ending 35440 and starting 35738), FAR 0x39bfc000. root397: none. Both captured: FSYNR1 0, which
    msm's `a6xx_fault_block` names the CP; UCHE_CLIENT_PF 0x81; guilty pid 65 (backboardd), queue 0. root396's search found the low word
    once, followed by 0, in an IB1 at 0x1_0f147000, dword 288: `70e30001 00000000` (CP_SET_MODE 0), two register writes, then
    `70bf8003 39bfc000 00000000`: **CP_INDIRECT_BUFFER to 0x39bfc000 with no upper word**, the order of Turnip's SYSMEM pass
    (`tu6_sysmem_render_begin`, then `tu_cs_emit_call(&cmd->draw_cs)`). The CP fetched an IB2 whose address Turnip built as
    `entry->bo->iova + entry->offset` (`tu_cs_emit_ib`, 64-bit), so that BO's `iova` read under 4 GiB, which no live BO has (Turnip's
    Darwin VA allocator hands out only from va_start, 0x1_0000_0000; freed BOs' structs are zeroed, `darwin_bo_finish`). Both faults
    are the CP fetching a command buffer at an address no live mapping has: a command-stream BO's lifetime or state at record time,
    the case `turnip-darwin-skip-freed-entries.patch` already met at the top level (submissions naming freed BOs).
  - *Placement (both boots).* Untraced P4 72.6 fps (10 opens, 61.5-87.6), GPU 7.85 ms, first frame 116 ms; Q4 (the compositing
    thread on cpu5 alone; the scheduler has no cpu5-6 cluster) 97.9 (12 opens, 72.8-113.2), 8.77 ms, **first frame 394 ms**: in Q4
    the later the first frame the higher the fps (365-466 ms: 98.7-113.2; 177-267 ms: 72.8-85.1), the animation's tail alone, so the
    untraced fps overstates it. Traced, first frames 109-152 ms in both: P4 68.7-78.0 fps (compositing 4.3-4.8 ms on cpu7, waiting
    5.3-5.9; encoding 5.4-5.7, waiting 3.5-4.2; SpringBoard 0.9-2.1, waiting 11.5-12.2), Q4 92.5-97.6 (compositing 6.7-7.2 on cpu5,
    waiting 0; encoding 6.2-6.5, waiting 0; SpringBoard 1.2-1.5, waiting 6.4-7.0). Peaks 57.9-66.4 C.
  - *Encoding thread's system time* (`threads-delta.py`, 56 opens): 9-12 % of its CPU (P4u 94 of 821 ms an open, P4 98 of 1032):
    page faults, `madvise` and submissions together are about 0.5-0.6 ms of its 5.5 ms a frame. The compositing thread 14-16 %.
  - *gtr=1 (root397)*: not measured. frames.trace grows about 10 MB a traced open under hs=9 and reached its 64 MiB cap by the third;
    the 1 MB tail read covered a fraction of an open (88 % of its GPU time "without a command buffer entry").
- **Why the IB2 address had no upper word: a freed BO's slot zeroed after another thread reused it** (Turnip sources, no boot):
  - `tu_cs_emit_ib` emits `entry->bo->iova + entry->offset` in 64 bits, and the Darwin VA allocator hands out nothing under va_start
    (0x1_0000_0000). But a `tu_cs_entry` keeps a 32-bit offset computed from CPU pointers when the entry is made
    (`tu_cs_add_entry`: `tu_cs_get_offset` = `cs->start - tu_cs_current_bo(cs)->map`, in dwords, times 4, all `uint32_t`). If the
    stream's current `tu_bo` was zeroed at that moment (map NULL, iova 0), the entry's offset is the low word of the CPU address of the
    commands, and its IB2 call goes to that low word with no upper word (root396: 0x39bfc000; backboardd's mappings lie at
    0x1_xxxx_xxxx); if the slot is filled by another BO before the call is emitted, the address is that BO's iova plus the garbage
    offset, which nothing maps (root395: 0x1_38808380). It is also the case `turnip-darwin-skip-freed-entries.patch` met at submission
    (entries whose BO had handle 0).
  - What zeroed a live BO: `darwin_bo_finish` put the freed BO's record in the retire ring and reaped it (into the BO cache at once
    when its fence had passed), released the retire lock, and only then zeroed the struct (`memset(bo, 0, ...)`). A reused BO keeps
    its kernel handle, and the BO structs are a sparse array indexed by the handle: another thread's allocation (`darwin_bo_init`
    takes cached BOs under the same lock, and no `dma_bo_lock`) could take the BO in between and fill the same slot, which the late
    memset then wiped under its new owner, typically the encoding thread growing a command stream. Upstream's `msm_bo_finish` zeroes
    the struct before closing the handle. More SYSMEM passes (hs) and a contended cpu7 (preemption between the unlock and the
    memset) widen the window.
  - Fix (`turnip-houji-sysmem-unpadded.patch`, GPU userspace gpu39): the struct zeroed while the retire lock is still held, before any
    allocation can take its BO. Counters in `lazy-aged.txt`: slots an allocation found live, IB2 calls to a freed BO or under 4 GiB
    (`tu_cs_emit_ib`), with the last handle and address. Also `sbl=1` (`adrenometal-zzzzz-hs-copy4.patch`): the presenter's blit
    instead of the scanout kernels.
- **The first frame off cpu7, the copy's share, and the race once more** (root398, root399, GPU userspace gpu38, seed leanlc36,
  `tt=1 hs=9 gp=0`, 979 / 985 bytes; gpu39 with the fix built, seed leanlc37, not booted yet):
  - *Faults, before the fix.* root398: one in an open (fence 28269, Q4 side), FAR 0x1_50d74f80, guilty backboardd. root399: two at
    boot, guilty **pid 141**, the last FAR 0x663f000: its IB1 holds `70bf8003 06de1000 00000001 00000121` (an IB2 call to
    0x1_06de1000) followed by `70bf8003 0663f000 00000000`, two entries of one stream, the second with no upper word: the slot race
    in another process.
  - *Q4's late first frame.* Untraced, the compositing thread on cpu5 (Q4) or on cpu3 (Q3): first frames 248-441 ms (Q3 336-369),
    P4 28-115 ms; so not cpu5's domain. Its user stacks over the first 500 ms of each open (`ustack`, read from a kernel thread):
    running or runnable in 138-143 of 250 samples in Q4, 143-147 in Q3, 135-141 in P4, the rest blocked in its run loop
    (`CA::WindowServer::IOMFBServer::thread_body`, `CFRunLoopServiceMachPort`) alike: it is neither starved of its CPU nor blocked
    more than on cpu7. Not yet explained.
  - *The scanout copy's GPU time* (`copyshare.py`, root399's four traced opens read 8 MiB each, 166 frames, 8.58 ms a frame): jobs of
    the copy alone 0.30 ms a frame, jobs with the copy and other work 0.75 from the copy's start to the job's end (and 1.00 before
    it), other work 2.29, jobs with no ring entry at all 4.25 (every job backboardd's: entries the trace lost). The copy shows in
    about 0.45 jobs a frame of one a presented frame: at about 2.3 ms a copy, **2-2.5 ms a frame, a quarter to a third** of the GPU
    frame, not the 4.6 ms (over half) under GMEM (root379).
- **The slot fix holds: gpu39 guardrail clean** (root400 reset on seed leanlc37; root401, root402: `tt=1 hs=9 gp=0`, P4, 979 bytes,
  fault map armed, 16 untraced opens each):
  - `gpuf` at the end of each boot: **hangs 0, faults 0, for the whole boot**, the boot-time hangs and faults every earlier config had
    included; `GPUS FIND` empty; `lazy-aged.txt`: slots live at allocation 0, bad IB2 0. No backboardd or SpringBoard crash.
  - Untraced: 66.9 fps (root401, 11 opens measured: five lost their `GESTURE AT` line, their swaps and GPU jobs are there) and 70.7
    (root402, 16), 52.1-83.3 per open, mean 69.1 over 27; GPU 8.64 / 8.58 ms a frame (gpu38 boots: 7.78-9.13, so within the spread
    across boots, not shown to be a cost of the fix); first frame median 107 / 112 ms; peak hottest 69.5 C (root401's first side, from
    55 C), otherwise 56.3-67.9 C.
  - Not yet shown: that the fix holds over more boots, or under Q4.
- **Q4's "late first frame" was the swap readout, not the open** (root403, gpu39, seed leanlc37, `tt=1 hs=9 gp=0`, 979 bytes; sides
  Q4u, Q4n (a tl of SpringBoard's main thread alone from 0.2 s before the swipe), Q4w (only the 0.2 s wait), traced Q4, P4u):
  - Faults: hangs 0, faults 0 for the whole boot, slots live 0, bad IB2 0: the third clean gpu39 boot, the first with Q4.
  - First swap after the gesture: Q4u 122, 221-386 ms; Q4n 153, 369-464; Q4w 318-412; traced Q4 16-151; P4u 35-114. Neither the
    wait nor a tl brought Q4's down.
  - But backboardd's GPU jobs (gtl, submit times on the gesture's clock) start at about +110 ms in Q4 as in P4, and go on every 8 ms,
    long before the "first frame". Console `swaps` keeps the newest 128 swaps, read 1.56 s after the swipe (1.37 s traced): in P4 the
    oldest kept lies 0.4-1.8 s before the gesture, in **22 of 23 untraced Q4 opens and 3 of 4 traced ones it lies after it**: the
    "first frame" was the oldest swap kept, and the Open fps window (0.8 s from it) started late in the animation.
  - So every Q4 figure so far (traced 92.5-97.6, untraced 97.9 fps, root395/396/398) measured a later part of the open, not the open:
    void until measured again. The P4 figures are not affected (their readouts reach before the gesture).
  - `t-bal.sh` now reads `swaps` twice (0.8 s and 1.5 s after the swipe, traced 0.9 and 1.5), merged by the scripts
    (`framebud.py` now merges duplicates too).

## Faults found on the way

- backboardd SIGSEGV (root191, 250 s): `tu_init_hw<A7XX>` from `tu_BeginCommandBuffer` wrote through a NULL command stream after a
  BO map failed (`x0 = -5`, `VK_ERROR_MEMORY_MAP_FAILED`; upstream `tu_cs_reserve` only asserts). `unixnu-adreno-cpumap-refusals.patch`
  counts BoCpuMap refusals (console `vm`); root192/193 saw none, and no view refusals.
- backboardd EXC_GUARD code 0x105 (root193, during Control Center repetitions): not analysed yet.
- A home-button press (console `home`) starts the app-close animation 0.8-1.1 s late; the tests close with a bottom-edge swipe.
