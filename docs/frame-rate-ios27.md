# iOS 27 frame rate on houji (24B156): measurements and changes

Measured with `ios27/t-actions.sh` on 8 GiB + persistent Data. Each action is timed from its gesture's start (the console "gesture"
line, mapped to the swap clock) to its first frame, then over the 0.8 s after that frame, from the kernel's ring of the last 128
swaps (console `swaps`). These are frames the render server delivered to the scanout copy, not a measured panel refresh: the panel
keeps the boot firmware's mode and has no TE/vblank sync here.

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

## Faults found on the way

- backboardd SIGSEGV (root191, 250 s): `tu_init_hw<A7XX>` from `tu_BeginCommandBuffer` wrote through a NULL command stream after a
  BO map failed (`x0 = -5`, `VK_ERROR_MEMORY_MAP_FAILED`; upstream `tu_cs_reserve` only asserts). `unixnu-adreno-cpumap-refusals.patch`
  counts BoCpuMap refusals (console `vm`); root192/193 saw none, and no view refusals.
- backboardd EXC_GUARD code 0x105 (root193, during Control Center repetitions): not analysed yet.
- A home-button press (console `home`) starts the app-close animation 0.8-1.1 s late; the tests close with a bottom-edge swipe.
