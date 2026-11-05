# Desktop regression post-mortem: 1.5 days lost to uncommitted UEFI flags

2026-11-05. The iOS desktop (lock screen, SpringBoard, backboardd) worked on 2026-11-01
images and failed on every later boot. This page records the actual causes so the same
class of failure does not repeat.

## What happened

Between the working Nov 1 images and the Nov 5 debugging session, three independent
problems stacked up. Each one alone breaks the desktop, and the debugging session
initially chased the wrong layer (the root image, then the kernel) because the evidence
pointed there.

1. **`rootrw=0` — an uncommitted local edit in the UEFI working tree.**
   `uefi/overlay/.../HoujiStorageProbe.c` in the *build tree* (`unixnu-work/usb-bringup/uefi`)
   had `rootrw=0` and `houji_gpu_runtime=3`, while the committed fork source always had
   `rootrw=1`. Every image packaged from that tree mounted the root filesystem read-only.
   `backboardd` dies during HID plugin setup with
   `NSInvalidArgumentException: -[NSURL initFileURLWithPath:]: nil string parameter`
   when its state cannot be created on a read-only volume, and crash-loops, so
   SpringBoard never presents. `cryptexd` crashed on `mkdirat: Read-only file system`
   in the same logs — visible evidence that was initially misread as a "dirty root"
   symptom.

2. **`houji_gpu_runtime` mode semantics.** The fork kernel's GPU full-host probe is a
   bounded diagnostic that always ends in a deliberate panic ("retained noFS PSCI
   recovery"). Mode 3 stops after ~360 s; mode 4 runs the desktop until its live trial
   ends (~20 min); only mode 1 holds forever. The good Nov 1 images used a pre-fork
   kernel that had none of this. Booting a fork kernel without mode 1 means the session
   always returns to recovery by design — which looks exactly like a crash if you do
   not know the mode table.

3. **Mode 1 blocked userspace.** `houjiGpuFullHostProbeOnce()` runs inline in
   `IOFindBSDRoot()` (via `houji_ufs_root_attach`). Its mode 1 hold was
   `for(;;)IOSleep(1000)` inline, which blocks the boot thread: the GPU comes up, the
   kernel logs stream, but launchd never starts. The RAM log showed zero userspace
   lines for the whole session. The hold must run on its own thread (fixed in
   `d1fed76`), and the live worker must not break on its check counter in mode 1
   (fixed in `590d956`).

A fourth, smaller trap cost extra hours: `pack-uefi.py` embeds the *prebuilt*
`Build/.../SILICIUM_UEFI.fd`, and `make-payload.py` copies `StorageProbe.efi` from the
*current build output*. Editing `HoujiStorageProbe.c` has no effect unless the FD is
rebuilt **and** the payload is regenerated **after** that rebuild, in that order.
Several images were packaged with stale boot-args this way and invalidated test runs.

## Why it took 1.5 days

- The first hypothesis (the `-sched` root image was dirty) was plausible: restoring
  the Nov 1 root did not fix the desktop, because the real cause was `rootrw`.
- The "kernel regression" hypothesis followed: the good images embed a pre-fork
  kernel, and no fork kernel had ever shown a desktop, so a bisect was started. The
  bisect was correct in principle but aimed at the wrong layer — the kernel code was
  fine; its *inputs* (boot-args from the build tree) were wrong.
- The decisive evidence only appeared when the exact good image was booted with its
  own UEFI (lock screen presented), and when the running fork kernel was booted with
  `rootrw=1` and backboardd stopped crash-looping within the same session.
- The `HOUJI SCREEN: 10 s without a touch on the lock screen` line is a kernel
  heuristic message, not proof the lock screen is on the panel. It misled the
  investigation once; do not treat it as display evidence.

## Rules that would have prevented this

1. **Never package from a dirty build tree.** Before packaging an image, `git diff`
   the UEFI tree against the committed source and reconcile every difference. The
   `rootrw=0` edit sat in the tree for days because the tree is not a checkout of
   record.
2. **Record the boot-args with every image.** The packer should embed the exact
   `BaseBootArgs` string in the image's JSON sidecar, so a boot can be attributed to
   its inputs without unpacking the FD.
3. **Boot-args are part of the kernel contract.** `rootrw`, `houji_gpu_runtime`,
   `houji_return_recovery` change desktop behavior qualitatively. Any test matrix
   that varies the kernel must hold them fixed and print them.
4. **The GPU runtime mode table is load-bearing** (1 = hold forever, 2 = 300 s probe,
   3 = 360 s desktop trial, 4 = unbounded desktop with live trial, 5 = coherence then
   3). A boot that "returns to recovery by itself" should first be checked against
   this table before suspecting a crash.
5. **Screen-state claims need the panel.** The kernel's screen messages are
   heuristics; only the user (or a camera on the device) can confirm presentation.

## Outcome

- `rootrw=1` + `houji_gpu_runtime=1` + the two mode-1 fixes (own hold thread,
  non-exiting worker) produce a persistent desktop on the current fork kernel:
  lock screen presenting, backboardd alive, no crash-loop (`7a07f17`).
- The Nov 1 root image lacks the 32 registered system apps (that registration was
  on-device state, lost when userdata was restored). The `-sched` image copy with
  the `tools/restore-system-apps.py` move and `tools/register-system-apps.py`
  registrar applied offline is the apps-complete root; see `docs/system-apps.md`.

## Follow-up: single-digit FPS with the persistent desktop

The first mode-1 build had a second regression of its own making: the live
worker polled `native_adreno_provider_poll()` at a 100 ms cadence. Unlike
`native_adreno_provider_try_poll()` (which the desktop modes use, 500 ms, and
which skips the board quiesce when no jobs are outstanding), `provider_poll`
always takes the issuer lock and runs the complete GPU idle gate — CRC-set and
CRC-check audits of ~15 ms each — even when the GPU is idle. Holding that gate
roughly a third of the time starved every backboardd submit; the desktop ran
at single-digit FPS with the CPU thermals spiking to 80°C, and it did not
depend on Wi-Fi. The fix (mode 1 polls with `try_poll` at 500 ms and treats a
contended lock like mode 4 does) restores exactly the poll path the smooth
pre-fork-era boots used. Lesson: on this driver, any recurring kernel-side
poll of the GPU must use the try-lock/no-work path; the full quiesce is for
teardown and diagnostics only.

Verified after the fix (`979333b` added a 30 s display-counter report to the
mode 1 worker): sustained continuous page-swipe windows present at up to
~58 FPS (1732 presents in one 30 s window), unlock animations burst at
hundreds of frames per window, and the idle lock screen is event-driven
(0–19 per window) as expected. CPU load sits at 0–20 % with no thermal
spikes, Wi-Fi on or off. Single-digit FPS is gone; the residual stutter
matches the era's documented GPU state, not the regression.
