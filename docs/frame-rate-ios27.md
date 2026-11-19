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

## Faults found on the way

- backboardd SIGSEGV (root191, 250 s): `tu_init_hw<A7XX>` from `tu_BeginCommandBuffer` wrote through a NULL command stream after a
  BO map failed (`x0 = -5`, `VK_ERROR_MEMORY_MAP_FAILED`; upstream `tu_cs_reserve` only asserts). `unixnu-adreno-cpumap-refusals.patch`
  counts BoCpuMap refusals (console `vm`); root192/193 saw none, and no view refusals.
- backboardd EXC_GUARD code 0x105 (root193, during Control Center repetitions): not analysed yet.
- A home-button press (console `home`) starts the app-close animation 0.8-1.1 s late; the tests close with a bottom-edge swipe.
