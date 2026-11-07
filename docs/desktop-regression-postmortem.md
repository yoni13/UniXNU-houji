# Desktop regression post-mortem: 1.5 days lost to uncommitted UEFI flags

> **Evidence correction:** This historical account contains earlier hypotheses
> stated too confidently. In particular, the cause of the `backboardd` nil-path
> exception, GPU resets, and frame-rate regression is not established. Present
> counts and display callbacks are not measurements of panel FPS. The later
> weak-link bisect is inconclusive. Do not use this document as proof that the
> keyboard, 60 FPS, or reboot stability is fixed.

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

## Follow-up 2: the real 60 FPS regression — the mode 1 provider gate audit

After the poll fix the desktop still ran at 21-40 ms frame gaps (half the
era's 60 FPS). The decisive telemetry was the 5 s display-counter report
added to the mode 1 worker: display callbacks were a steady 120/s (pacing
fine), GPU submits matched presents exactly (GPU not queuing), and the
hitch log showed the provider gate's audit phase consuming 14.2 of
14.2 ms of gate time between frames. The provider gate ran a full
`auditLive()` plus a table CRC on EVERY gate call in mode 1 — the
pre-desktop strict path — while the 60 FPS era's mode 4 audits once per
VM generation. Fixed by giving mode 1 the same once-per-generation
policy (`4567d48`).

Two supporting fixes landed with it: the coherence qualification now
runs in mode 1 (`ace4b06`) so the WB fast path enables like the era, and
an inconclusive trial (flaky dirty-line precondition, status 11) is
retried and then cleaned up instead of falling into the stop/panic path
that silently rebooted the phone mid-session (`49b3f2e`).

Verified on hardware: a real unlock (swipe 600,2400 -> 600,200, screen
on, home screen animating) presented 108 then 408 frames across its 5 s
windows, and the home screen sustained 543-601 presents per 5 s window —
108-120 FPS, vsync-limited, for over a minute of live rendering. The
earlier "single digit"/30 FPS measurements were taken on a static lock
screen whose wake/sleep cycling produced few frames; they measured the
screen state, not the animation rate.

## Follow-up 3: the keyboard geometry and texture limits were root-image state

The `99beb10` keyboard/status-bar geometry and texture-limit changes are
not kernel code: they live in `/usr/lib/HoujiKeyboardGeometry.dylib`
(weak-linked into 189 APPL main executables) and in the rebuilt
`NativeAdrenoMetal.bundle`. The 2026-11-04 root image that carried them
was an offline-patched copy that no longer exists on disk; rebuilding
the apps root from the `-sched` image silently dropped both. The
keyboard dylib rebuilds from `tools/build-userland-compat.py` (a
prebuilt, source-verified copy exists in the userland artifacts), the
weak link is a plain `LC_LOAD_WEAK_DYLIB` in each executable's header
padding (the apps are ad-hoc signed with no enforced page hashes, so
the header patch does not invalidate anything), and the bundle needs
`tools/build-metal.py` on the macOS CI.

## Follow-up 4: the root image is now fully rebuilt from reproducible parts

The Nov 4 root image (32 apps, registrar, keyboard geometry dylib,
texture-limits Metal bundle) existed only as an offline-patched file that
has since been lost. It is now rebuilt on the `-sched` base image with
committed tooling, restoring every documented item:

- 32 staged apps moved to `/Applications` and the one-shot registrar
  (guestfs offline, same verification as `tools/restore-system-apps.py`).
- `/usr/lib/HoujiKeyboardGeometry.dylib` installed and an
  `LC_LOAD_WEAK_DYLIB` added to all 172 APPL main executables in the
  header padding (`tools/root/install-keyboard-geometry.py`). The apps
  are ad-hoc signed without enforced page hashes, so the patch does not
  invalidate them. An earlier attempt wrote the dylib name offset as 8
  instead of 24; the malformed entries were dropped and re-added
  correctly, then verified by guestfs read-back and a lief parse.
- `NativeAdrenoMetal.bundle` replaced with the CI build that includes
  the `NativeDeviceLimits` texture-limits frontend
  (`tools/root/install-metal-bundle.py`, `gpu.yml` artifact).

Lesson: root-image state that is not reproducible from the repository
does not exist. Every offline patch needs a committed installer and a
built artifact, or a disk loss silently reverts user-visible fixes.

## Follow-up 5: the hard reset at backboardd's GPU open (open investigation)

After the root-image rebuild, boots began resetting ~100% of the time at
a consistent point: the CDC console appears ~25 s in, dies ~65-80 s in,
and the phone lands in recovery with no kernel panic trace and no
committed XNU log beyond the early boot checkpoints. The death moment in
the console captures is backboardd's `IOAcceleratorES -> AGXAccelerator`
matching — its first Metal device open.

Variables eliminated by direct test: the Metal bundle (new CI build vs
the Nov 1 build), the root content (keyboard dylib + weak links vs
none), the kernel (fps-final 4567d48 through thermal 4537b9d), the GPU
DVFS worker (disabled entirely), thermal caps (deepened; the "spikes"
to 94.6 C fall back within one sample and the reset still occurs below
critical with the caps biting), a complete power-off cycle, and
idle-vs-active use. All 172 weak-linked executables were re-validated:
Mach-O structure intact, correct name offset, lief-parseable.

One 30+ minute stable session exists (fps-final2, kernel 4567d48, old
root): 120 FPS sustained, the full stack working. It is the only boot
that survived past the window, and nothing reproducible distinguishes
it. The docs already track a related known issue ("repeated warm resets
leave UEFI's GPU power handoff not ready before userland"), but a
complete power-off did not change the outcome.

The regression in crash RATE (flaky ~50% to ~100%) coincides with the
new root: the one-shot app registrar runs its Launch Services rebuild
(its 46-109 s window brackets the crash) with 172 weak-linked
executables, and `lsd` is killed (sig 9) during it — `lsd` never died
on the old root. The A/B in flight: the pre-apps `-sched` base image
(same kernel) — stable points at the registrar/weak-link churn as the
trigger, unstable points at an environmental or hardware-adjacent race
the software cannot control.

## Follow-up 6: weak-link bisect — inconclusive observations

**Correction:** Earlier versions of this document claimed the weak load
command itself caused the resets and that the problem was solved. Those
claims were not established by the tests. The linked and unlinked images
also differ in signature validity and possible runtime loading of the dylib;
the available boots do not isolate a cause. In particular, a dylib present
but not loaded cannot establish whether its constructor is safe. No panel
frame rate or long-term stability conclusion follows from callback counts.

The hard reset at backboardd/SpringBoard's GPU-adjacent window was
bisected across the root-image content. Each leg is a full verified
restore of a 32 GiB image; the boot kernel was identical throughout
(nodvfs build of 4537b9d).

| Leg | Apps | Weak links | Dylib | Result |
|-----|------|-----------|-------|--------|
| Base `-sched` | no | no | no | stable |
| Leg 2 | yes, pristine | no | no | stable (25+ min) |
| Leg 3 | yes, pristine | no | present, unloaded | stable (14+ min) |
| Leg 1 | yes | v1 (corrupted) | no | crash |
| V2 | yes | v2 (byte-clean) | yes | crash at +57 s |

The v2 Calculator sample differed from a pristine copy in 42 bytes in the
header and padding, but this does not establish that all patched executables
were equivalent, correctly signed, or safe. Earlier claims about the cause
of the v1 file-size difference, page-hash enforcement, and `codesign`
rewriting fixup chains were not verified. The unlinked-dylib leg only tests
the dylib's presence, not its execution.

The Nov 4 record describes weak-linking and ad-hoc re-signing while preserving
entitlements. Reproduce and independently verify that procedure on macOS on
private local copies before considering another device restore. The repository
must not contain vendor binaries; do not publish them as public CI artifacts.
Keyboard geometry, desktop performance, and reboot stability remain unverified.

### Private macOS signing pass (2026-11-07)

On a privately accessed macOS 15.8.2 / Xcode 16.4 host, 172 executables
extracted from the offline `houji-root32-pristine-apps.hfsx` copy were patched
with a weak load command in validated zero header padding, then ad-hoc signed
with their existing identifiers and entitlements (including absence of an
entitlement blob). Each passed `codesign --verify --strict`, a load-command
inspection using `otool -l`, and a comparison of entitlement plist content
before and after signing. The resulting private archive was transferred back
with matching SHA-256. This is an offline result, not a device test; it does
not establish that the keyboard hook loads, fixes geometry, or avoids resets.
No vendor binaries from this pass belong in the public source repository.

### Device result (2026-11-07)

The macOS-signed executables were installed into a copy of the pristine-apps
root (bytes, inode, owner and mode verified per file), restored to userdata
(host and device SHA-256 matched over the full 32 GiB) and booted with
`fps-final.img`. Observed:

- No reset in 6+ minutes, past the ~57 s window where the Linux-patched images
  reset. Only `driverkitd` (memory limit), `CommCenter` and one `mediaserverd`
  crashed; none of the 172 re-signed apps did.
- By visual inspection on the panel: keyboard full width and normal height,
  status bar correctly placed, home-screen scrolling smooth.

This is one boot. The dylib logs nothing, so the console does not prove it
loaded; the geometry result is visual. Smoothness is a visual observation,
not a measured panel frame rate. Compared with the failing v2 image, the
difference is the macOS ad-hoc re-sign, which is consistent with, but does not
prove, an invalid signature on the Linux-patched binaries as the reset cause.
