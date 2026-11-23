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
