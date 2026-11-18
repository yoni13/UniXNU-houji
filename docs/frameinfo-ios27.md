# iOS 27 FrameInfo ABI (24B156)

## Kernel ABI

The reviewed fixture records the `IOMobileFramebufferLegacy` vtable at `0xfffffff0082e0820`. Slot 216 (`+0x6c0`) has mangled-signature diversity `0xbd70` in the fixture (`ipsw kernel cpp` reports the inherited abstract base entry's own diversity separately) and is:

```c++
IOReturn supportedFrameInfo(uint32_t index, uint32_t *count,
                            char key[64], uint32_t *transport);
```

Selector 74 calls it once per index. `IOMobileFramebuffer::_InfoKeyInitialize` (`0x22cdd3088`) allocates `count+1` pointers and turns each successful 64-byte, NUL-terminated key into a CFString. Transport 0 is Mach async notification; the proposed keys are `Presentation_time`, `Requested_presentation`, and `Vbl_FrameTime`. Selector 73 sends scalars `{3, enable}` and the unchanged family converts its key list to a bit mask (`_kern_ChangeFrameInfo`, `0x22cdd4770`; `_figure_frame_info_mask`, `0x22cdd37b8`), so the subclass needs no separate selector-73 override.

Slot 358 (`+0xb30`, diversity `0x13ff`) is:

```c++
IOReturn collectCompletionReports(uint32_t swapID, void *reports,
                                  uint32_t *count, uint32_t capacity);
```

`completeCurrentSwap`'s helper at `0xfffffff00a1d3304` calls it at `0xfffffff00a1d33c4` as `(this,current->swapID,NULL,NULL,0)`. Thus the generic legacy completion path consumes no returned C report array; the platform override triggers/dispatches telemetry. The 24B156 DCP override (`0xfffffff00921f430`) likewise ignores those arguments and sends command `0x41333530` to its coprocessor.

The observable report ABI is the type-3 Mach payload: `uint64_t {mask, swapID, value_for_each_set_bit}`. `FrameInfoNotifyFunc` (`0x22cdd8ea8`) accepts either 64-bit values or, when mask bit 31 is set, pairs of 32-bit halves, then creates a dictionary using the selector-74 key table. CA's callback (`0x184307ab4`) reads the three named values and passes them to `CATimeWithHostTime`; units therefore are Mach host/absolute-time ticks. The family type-3 request list is at display `+0x1a98` (head; `+0x1aa0` tail), as shown by registration at `0xfffffff00a1da2f4` and dispatch at `0xfffffff00a1daa30`; each request's async reference starts at `+0x18` (same node shape used by the adjacent sender at `0xfffffff00a1daba8`).

The current swap request stores the requested host time at `+0x390` (copied from the swap timing block at `0xfffffff00a1d6334`); family current/head/tail are display `+0x1a40/+0x1a48/+0x1a50`.

## Implementation consequences

Both slots are ordinary virtual overrides. `build-houji-display.py` emits and verifies a full authenticated arm64e subclass vtable, including original per-slot discriminators, so no unsigned copied vtable is required. The existing user-client copied-vtable telemetry is deliberately disabled under PAC; there is no established PAC-safe selector-73 boundary hook. The draft therefore counts selector-74 calls, completion callbacks, deferred early completions, sends/drops, and prints bounded/power-of-two samples.

The iOS 16 `SweetFramebuffer.cpp` only implemented selectors 3–8, 18, 28 and 72 plus software type-5 vsync. Selectors 73/74 fell through its generic success/log path; it emitted no type-3 FrameInfo or completion reports.

`fb-frameinfo=1` reports GPU/CPU copy completion as all presentation/VBL values. This is intentionally **not** physical scanout or measured vblank. Root101 (1029 delivered frames by 598 s, finger-up received, settling frames stop until another touch) makes missing completion feedback plausible but does not validate this synthesized timestamp contract.
