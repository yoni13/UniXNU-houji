# iOS 27 IOMFB frame clock (24B156)

## What Core Animation uses

Shared-cache inspection shows that `CA::WindowServer::IOMFBServer::add_vsync_source` has two clocks. One branch obtains `IOMobileFramebufferGetVSyncRunLoopSource` and calls `IOMobileFramebufferEnableVSyncNotifications`; that becomes user-client selector 72, notification type 5. The branch selected on houji instead creates a `CADisplayTimer`. Its interval comes from the display timing/`IOMFBDisplayRefresh` data, and its callback enters the same `IOMFBServer::vsync_callback`. Therefore `sent=0 arms=0` does **not** mean CA has no clock: it means CA selected its timer rather than type-5 notifications.

iOS 27 also has a separate presentation-feedback path. `IOMFBServer` calls `IOMobileFramebufferSupportedFrameInfo` (selector 74). When keys exist it registers notification type 3, calls `IOMobileFramebufferFrameInfo`, and enables keys with `IOMobileFramebufferChangeFrameInfo` (selector 73). Its callback consumes dictionaries containing keys including `Presentation_time`, `Requested_presentation`, and `Vbl_FrameTime`; kernel `collectCompletionReports` (legacy virtual slot 358) supplies per-swap reports. This feedback is not the render timer itself, but CA uses it to update timing and completion state.

## What houji answers

`NativeLegacyDisplay` implements the legacy type-5 sender and a software timer, but arms it only when the family's type-5 client list is non-empty. iOS 27 registers none on this display, so no tick is sent. More importantly, `registerService` publishes `IOMFBDisplayRefresh` only for rates **above** 60 Hz, and `getRefreshAttributes` likewise refuses to override at 60. Thus `vsync=60` starts an unused kernel timer while CA's selected `CADisplayTimer` lacks the intended fixed-rate publication. The driver also returns `Unsupported`, zero keys, for selector 74 and returns no completion reports, so type-3 FrameInfo is absent. These answers are conservative, but they omit iOS 27 timing feedback.

## Comparison and minimal fix

The iOS 16 houji driver continuously advertised fixed 60/120 Hz through `TimingElements` and `IOMFBDisplayRefresh`, and emitted software type-5 ticks. Its panel still inherited firmware timing; this was not measured physical vblank. The sheng iPadOS 27 evidence similarly shows regular fixed-120-Hz flips (continuous-input median 9.6–15.4 ms) while its legacy diagnostic says CA may use its own display timer; it does not prove type-5 subscription.

First fix: with `fb-vsync-port=1` plus `vsync=60`, publish fixed 60 Hz refresh attributes too. Do not fabricate FrameInfo/VBL timestamps yet. If selector telemetry shows 74/73/type-3 use, next implement the exact 24B156 completion report from measured ABI data.

## Cheap next-run instrumentation

At the original `IOMobileFramebufferUserClient` implementation boundary, count every `externalMethod` selector (count, failures, last result, scalar/structure sizes) and every `registerNotificationPort` type (count, last port-present/refcon/result). Print only nonzero rows with a `fb io` console command. Hook before and after forwarding, so rejected calls remain visible. Do this in the bound user-client dispatch or a PAC-correct wrapper—not the current unsigned copied-vtable probe, which is disabled on arm64e. The decisive rows are selectors 72–74 and notification types 3/5, alongside 4/5/6 swap calls.
