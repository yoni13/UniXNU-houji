# App launch (Settings) — houji iOS 27

Baseline root420 (first measurement, 2026-11-24): `tt=1 hs=9 gp=0` (979 bytes), Q4 binds, `leanlc39`/gpu41, killpid kernel (`b8216d8`).
Benchmark: `t-launch.sh` — vision-free launch (`tap 740 2000`, Settings icon first page 4th row 3rd column, verified by `ps` printing Preferences pid), cold = `killpid` SIGKILL verified gone before tap, warm = resident (Preferences survives open/close, probe root418).

## Baseline numbers (root420, 4 rounds each, guard clean: 0 hangs, 0 faults, no crashes)

| | cold | warm |
|---|---|---|
| tap -> first app frame | 272-387 ms (387/339/326/272) | 18-75 ms (18/75/21/75) |
| launch animation fps | 96 / 104 / 89 / 102 | 53 / 89 / 79 / 85 |
| frames <=9 ms | 80-87 % | 46-64 % |
| median frame gap | 8-9 ms | 8-10 ms |

Oddities: warm round 1 ran 53 fps (the first warm after a cold sequence; unexplained). The settle metric over-counts (idle
redraws after the animation); to be replaced by "last gap <=100 ms" stop. `launchan.py`'s cross-check mis-echoed home-swipe
lines as launches; its tap-only fix is pending; the t-launch.sh prints are the valid numbers.

## Next (workflow step 2)

Profile root420's cold 300-380 ms: dvfs policy lines in the window (domain clocks under sko=4), Preferences' threads CPU
(dyld/spawn vs first frame), UFS I/O lines, GPU job stamps (gtl) over the same window.

## Cross-check (launchan.py, values agree with t-launch.sh)

Cold (killpid-verified, 4): first frame 326-427 ms (median ~350), fps 96/104/89/102, <=9 ms 80-87 %.
Warm (4): first frame 18-75 ms, fps 53/89/79/85, <=9 ms 46-64 %.
Both tools agree on the values; launchan's cold/warm label needed the kill lines on the kernel clock (fixed).
GPU stamps: 393 gtl jobs over the run; per-launch GPU analysis next.
Guard: gpuf 0 hangs 0 faults; slots live 0, bad IB2 0 (boot end).

## Corrected baseline (second run, root420 live console, metric fixed: animation = the long run of <=100 ms gaps, settle = its end)

| | cold (5) | warm (4) |
|---|---|---|
| tap -> first app frame | 256-423 ms | -18-91 ms |
| animation duration | 560-1083 ms | 648-790 ms |
| animation fps | 90-98 (median 97) | 88-96 (median 92) |
| frames <=9 ms | 47-65 % | 47-54 % |

Findings:
- The earlier warm "53-89 fps" and "0 frames over 0 ms" were metric artifacts (settle tail pollution; a leading <-100 ms stall
  after warm's first frame ended the one-sided trim). Warm does NOT genuinely drop frames: its animation is 88-96 fps, the same
  band as cold's 90-98.
- The launch zoom is capped at ~90-98 fps in BOTH cases -> the limiter is common (not dyld/spawn, not a DVFS cold ramp). Median
  gap 8 ms: frames run at 8.33 ms when they come, but ~40-50 % of gaps exceed 9 ms.
- Cold's 256-423 ms tap->first frame is the latency problem; the earlier ps shows ~15 Settings AppIntents/Keyboard extensions
  spawning at cold launch (ClassKit/Developer/SafetyCheck/Camera/Spotlight/Installed/TV/Passcode/Music/Keyboard, ppid 1).
- Guard clean on every pass: gpuf 0 hangs 0 faults.

## Step-2 breakdown (root420 profiled rounds, + gtl dumps + threads + dvfs)

- Clocks: at tap the touch floor lifts d0 1132->2035 MHz (its cap; skin 28.5 C lifts stock caps); d1/d2 hold 2630. Not clock-starved.
- Fresh Preferences: main thread 1104-1122 ms CPU (210 ms sys) in its first ~1 s; 9 small helper threads 10-92 ms.
- GPU during the launch animation: 600 jobs dumped per launch window, GPU busy 4-13 %, median job 0.53-0.55 ms.
  **The zoom is NOT GPU-bound** -> the ~90-98 fps cap common to cold and warm is a CPU-side frame-production limit
  (mid-animation commits arriving late: 47-65 % of gaps <=9 ms, median 8 ms).
- Cold first frame 256-423 ms: SpringBoard's zoom starts only after Settings' spawn+scene work; the app's main thread is busy
  the whole window. Extension fleet (~15 AppIntents processes) spawns at cold launch.
- Next lever (step 3, zero-build): static touchpct/framepct through the launch window, then p7cap lift - keeping
  tt=1 hs=9 gp=0 + Q4; verify across >=2 clean boots, 0 hangs/faults.

## Step 3, first lever (root420/root420b same boot): framepct 100 + touchpct 100 static through the launch window

- Applied at runtime (dvfs set, verified by DVFS SET lines); the boot line and CC baseline untouched. Guard: gpuf 0 hangs,
  0 faults after every pass.
- Cold (8): first frame 256-393 ms (median 330; baseline 256-423 median ~350), anim fps 88-113 (median 94; baseline 90-98
  median 97). Warm (5): first frame -12-48 ms (median 16; baseline -18-91 median ~50), anim fps 87-93 (median 92; baseline
  88-96 median 92).
- **Verdict: neutral.** Neither the animation fps nor the <=9 ms share moved beyond boot noise (two launches hit 108-113 fps,
  within the same spread as baseline's best). Static boost does not lift the ~90-98 fps cap -> confirms the cap is not a boost
  or ramp issue; the frame producer (commit path on CPU) is the limiter, as the 4-13 % GPU load suggested.
- Measurement notes: the first A/B attempt lost its capture (capture.py's 1700 s timeout expired mid-run; raised to 2900 s in
  t-round.sh, and the capture is now started nohup+disown). t-launch.sh metric fixes: both-sides animation trim (settle at the
  real last frame), and the act window had a stale `last` variable (fixed to w[-1]).

## Placement A/B (arms E/F, same boot) + clocks + a traced cold launch

- SpringBoard's main thread (30:314) placement through the launch window (backboardd's Q4 binds untouched throughout):
  reference cpu7 (arms A-D, previous rounds), **cpu6 (E)**, unbound/Clutch (F), restored cpu7 after.
  - E (SB main on cpu6): cold 2 rounds: first frame 479(lost swap row)/-median ~470, warm +22/+75 ms; fps: one 111 fps launch
    (best single launch so far), others 90-99 - not a consistent gain.
  - F (SB unbound): cold 347/284 ms at 96/90 fps, warm 5 ms at 90 fps - same band.
  - Arms A-D (previous turn, same boot): all reference-band. **Verdict: placement of SpringBoard's main thread is not the
    launch fps limiter either.**
- Actual clocks during the launch window (TL CLK, every 10 ms through a cold launch): d0 2035 MHz, d1 2630, d2 2630, d3 2630 -
  every domain at its cap for the whole window; `cpub` measures 2628 MHz effective on cpu3/5/6/7. Not clock-starved at any point.
- Traced cold launch (tl of SB main, SB UIKit animation thread, SB 314; ustack of SB main + backboardd compositing mid-window;
  gtl dumps):
  - SB main (30:314): **blocked 399 of 400 samples** mid-animation, in the same wait each time; running 1 sample.
  - backboardd compositing (65:921): **blocked 394 of 400**, running 6 - the compositing server is idle-waiting, not contending.
  - SB's UIKit in-process animation thread (30:4021, 8.5 CPU-seconds cumulative, pri 53): the tl shows it making the
    animation frames in ~1.2-1.3 ms runs on a ~11.5 kHz-ish steady cadence (the R2 rows' short runs on cpu7).
  - GPU (gtl through the window): unchanged, a few % busy.
- **Root cause evidence so far:** during the zoom, both SB main and backboardd compositing are WAITING (blocked), and the
  frame producer is SB's UIKit animation thread + Preferences' own layout work. The 17-100 ms hitches line up with the fresh
  Preferences' main thread CPU bursts (its 1.1 CPU-s in the first second) - i.e., the app's own first-layout passes stall the
  animation handshake; the 350 ms cold wait is spawn + first-scene layout on that same thread before the first swappable frame.
  Neither clocks, placement of SB/backboardd, nor boosts move it; the work is inside Preferences/SB UIKit.

## Corrected attribution + the sko cap question (last measurements of the session)

- The earlier "SB 4021 makes the frames" was an artifact: tl's PID:NAME takes a thread NAME, not a tid; '30:4021' never tracked it.
  Retraced with 30:#4021 + the fresh Preferences' main (pid 2880 tid 42931, WARM launch):
  - Preferences main: 695 samples, running 531, median run 1.30 ms; cpus: 2 (495 rows), 3 (99), 4 (63), others sparse.
    Running TIME heavily on cpu2 (d3, 2630 MHz domain) and never on cpu7 during the zoom.
  - One corrected note: 30:4021 was NOT tracked in the earlier "blocked 399/400" claims either - that ustack was SB main (314).
- The user's open objection stands and is unresolved: sko=4 holds d1-d3 at 2630 MHz through the launch burst while the fresh
  app runs on those domains; native d2/d3 tops are higher. NEVER tested: a higher sko/runtime p-cap during the launch window.
- Guard at session end: gpuf hangs 0, faults 0 (root420b last read); boot 995 bytes (tte/tt=1 hs=9 gp=0 + prof args) intact.

## Zero-boot audit of the existing traces (2026-11-26) and a 1-boot A/B (root443)

- **The earlier placement arms never bound anything.** The console's `bind PID:NAME` takes a thread *name* (`PID:0` the main
  thread, `PID:#TID` a tid). Every `30:314` and `30:4021` bind in root420/root420b found no thread (`HOUJI BIND: ... tid 0 ...: 4`,
  KERN_INVALID_ARGUMENT); only `30:0` (SpringBoard's main, tid 314) bound, and stayed on cpu7. Arms B-F above and "placement of
  SpringBoard's main thread is not the limiter" are therefore unmeasured. `t-launch.sh WARM_BIND` and `t-launch-ab.sh` use `PID:0`.
- **SpringBoard's `#4021` is idle during the launch:** in root420b's traced warm launch (tl 30:#4021 + Preferences main) it is in one
  blocked state for the whole 3 s. Not a frame producer; nothing to pin.
- **That traced warm launch** (root420b, tl from 1665.7 s; the trace's own reader, kernel_task tid 43443, ran 1.5 s on cpu0-1, so
  times are inflated): tap at ~365 ms trace time, the first swap at 1182 ms. Before it Preferences main (42931) ran 578 ms, 515 on
  cpu2 (d3, 2630 MHz); SpringBoard's main did not run. The animation (1182-2708 ms) ran 106 fps, median 8.3 ms, 94 % of gaps
  <= 9 ms, with hitches of 93, 71, 35, 18 ms. In the 93 ms one (the first gap) d3 (cpu2-4) read 614-844 MHz while backboardd's new
  thread 43476 ran 41 ms on cpu4 and Preferences' 43458 on cpu2-4: the frame floor only engages after swaps in 2 of 4 samples (40 ms
  each), so it cannot cover an animation's first frames. Caveat: `TL CLK` is the domains' current state, not the governor's vote
  (an idle cluster reports its minimum whatever the vote); the 5 s policy lines do not resolve 40 ms steps.
- **The touch floor held ~50-100 ms, not 300.** The console's `tap` reaches `houji_touch_contact` (down), so `touching` should hold
  d1-d3 at their caps for `houji_cpu_touch_ms` (default 300) after it; in the trace d1/d2 rose one sample after the tap and read
  their minimum ~100 ms after it. Not explained (vote against state, above).
- **The cap question:** domain tops d0 2265.6, d1 (cpu5-6) 2956.8, d2 (cpu7) 3052.8, d3 (cpu2-4) 3148.8 MHz; the stock policy's
  first step (skin >= 25.0 C) is 2035 / 2630 / 2630 / 2630. `sko=4` adds 4 C to the battery-based skin estimate; recent boots' real
  skin was 25.6-26.5 C, so even `dvfs set skin 0` keeps the first step. Lifting d3 would need a negative offset (overriding the stock
  thermal policy: not done). The one sanctioned lift is `dvfs set p7cap` (cpu7 alone, first step only).
- **`t-launch.sh` first-frame fix:** the tap time is now the tap's own `GESTURE AT` kernel time (the swaps' clock), else the echo's
  capture time plus the boot-wide median offset ("tap inferred"); the capture side stamps lines in ~50 ms batches, which with a
  window-only offset (0 where the window lost its line) gave warm launches -18 ms. Replayed on root420b: no negative first frames.
- **root443 A/B** (leanlc46, `tt=1 hs=9 gp=0`, `HTC=1`, 979 bytes; arms interleaved round by round, `t-launch-ab.sh`): A reference
  (Q4 binds, SB main cpu7, floors 100 %), B + `p7cap 3053`, C + B with the resident Preferences' main bound to cpu7 for warm
  launches (`PID:0`, bound: ret 0), D SpringBoard's main unbound (`30:0 cpu-1`, ret 0). Hangs 0, faults 0, no crash, peak 62 C.
  - **Ran hot:** the skin estimate went 36.4 -> 39.7 C (the battery warm), so the stock steps held cpu7 at 1824 -> 1709 MHz and
    d1/d3 at 2189 -> 1824/2035 MHz the whole session; `p7cap` applies only at the first step (< 32 C for cpu7): **B lifted nothing.**
  - Per arm (n 2-3 each; A and B round 1 lost their swap readouts): cold first frame A 292/437, B 307/363, C 389/402/407,
    D 474/358/453 ms, fps 90-115; warm first frame 0-77 ms, fps A 80/89, B 86/93, C 94/79/89, D 95/80/88. No arm separates.
  - Across arms: **warm launches pace worse than cold** - gaps <= 9 ms median 68 % warm against 88 % cold (n 10 each; the 9 warm-up
    launches, cold too, 87 %), fps median 88.5 against 98, first frame 22 against 396 ms. The warm zoom starts within ~20 ms of the
    tap, from idle clocks, before the frame floor can engage.
  - `t-launch-ab.sh` warmed up before every arm (a pipe ran its `if` in a subshell); fixed.
- **Not verified:** whether warm's first frames run at a low *vote* (needs a traced warm launch reading the vote, or a longer touch
  window, `houji_cpu_touch_ms`, 24 boot-line bytes past the base 979); cpu7's lift on a cool phone (skin < 32 C).
