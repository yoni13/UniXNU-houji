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
- **assetsd** sometimes sits at ~100 % at idle too (root214/215), and two widget extensions after a data reset.
- Wi-Fi: one boot without `wifi=5 wifi-userspace=1` (root219) looked smoother, but the A/B over four boots (`t-ab-wifi.sh`,
  root220-root223, seed leanlc3) shows no difference: without / with Wi-Fi, Settings scroll 114-115 / 111-116 fps, home swipe 68-75 /
  68-75, Control Center close 58-84 / 54-89, Settings close 55-67 / 53-64, Search close 55-69 / 49-66, Search open 35-41 / 40-50,
  Control Center open 34-38 / 19-62. Wi-Fi stays.

## Faults found on the way

- backboardd SIGSEGV (root191, 250 s): `tu_init_hw<A7XX>` from `tu_BeginCommandBuffer` wrote through a NULL command stream after a
  BO map failed (`x0 = -5`, `VK_ERROR_MEMORY_MAP_FAILED`; upstream `tu_cs_reserve` only asserts). `unixnu-adreno-cpumap-refusals.patch`
  counts BoCpuMap refusals (console `vm`); root192/193 saw none, and no view refusals.
- backboardd EXC_GUARD code 0x105 (root193, during Control Center repetitions): not analysed yet.
- A home-button press (console `home`) starts the app-close animation 0.8-1.1 s late; the tests close with a bottom-edge swipe.
