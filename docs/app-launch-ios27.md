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
