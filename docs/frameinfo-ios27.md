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

Checked in the 24B156 shared cache (sub-cache `.38`): `FrameInfoNotifyFunc` (`0x22cdd8ea8`) rejects fewer than 2 arguments (`cmp w3,#1; b.hi`), reads `args[0]` as the mask and `args[1]` as the swap ID (`ldp x9,x20,[x2]`), then, for each set bit in bit order, takes the key name from the selector-74 table (`[x19+0xa70][bit]`) and the next value from `args+16`, up to `count-2` values. Bit 31 of the mask selects paired 32-bit halves. The driver's `{mask, swapID, value per enabled bit}` with `2 + popcount(mask)` arguments matches.

## Slots 324, 325, 359: family hooks (found after the root104/105 resets)

All three are pure in the family (`pure_slots` in `profiles/os/ios-27.2.1-24B156/abi/original-classes.json`), and this driver filled them with leaf guards (`native_legacy_backend_guard`, which panics; the USB console dies with it, so no panic text reaches the log).

- **324, request added/removed.** Registration appends a type-3 request to its TAILQ (`+0x1a98` first, `+0x1aa0` last; node `next` +0, `prev` +8), then calls `+0xa20` as `(this, request, 1)` (`0xfffffff00a1da38c`). Removal calls it with `0` (`0xfffffff00a1da5b8`). The results are ignored.
- **325, per-request dispatch.** `0xfffffff00a1daa10(this, arg)` (reached through the gate trampoline `0xfffffff00a1dfdac`) walks the list and calls `+0xa28` as `(this, request, arg)`, then `+0xa30` (slot 326, `bti c; ret` in the family).
- **359, after the sender.** The sender loop sends `sendAsyncResult64(node+24, 0, args, 3, 1)` per node (`0xfffffff00a1dac20`, which confirms the `+0x18` async reference), then calls `+0xb38` as `(this, arg)` (`0xfffffff00a1dac48`).

- **214, FrameInfo key change.** `0xfffffff00a1d8da4(this, uint32 type, void *request, uint32 mask)` runs the thunk `0xfffffff00a1dfd78` through the command gate. The thunk tail-calls `+0x6b0` (slot 214), and its IOReturn goes back to the user client. root109 logged type 3 with the mask growing 0, 1, 3, 7 as the render server enabled the three selector-74 keys one at a time, so this is the selector-73 enable path. It is declared typed, as `IOReturn frameInfo(uint32_t, void *, uint32_t)` under the name `frameInfo_11704`, whose mangled form hashes to slot 214's original PAC diversity `0xb486`; a `void` no-op would leave garbage in `x0`. For type 3 it now keeps the mask and answers success. `sendFrameInfo` (level 2) sends one value per enabled bit.

root109 (slot 214 typed, answered unsupported) ran to 976 s with no panic: selector 74 three times, hook 324 once, slot 214 with masks 0/1/3/7, and `collectCompletionReports` on every swap. Drawing was unchanged (0 fps during swipes), since level 1 sends no reports.

root108 (`fb-frameinfo=1`, no reports sent) got the panic text out through the USB drain: `NativeLegacyDisplay: leaf=214`, after `selector74=1..3` (all three keys) and `hook=324 calls=1 caller=fffffff00a1da3ac`. So selector 74 and the slot-324 no-op work, and slot 214 is the next call. root104-106 left no panic text.

root105 (slot 325 fixed) still reset at 68.3 s with no `HOUJI FRAMEINFO` line, right after the render server's first `IOMobileFramebufferUserClient` call. That fits slot 324 firing at registration. With `fb-frameinfo=1`, all three hooks are counted no-ops (`HOUJI FRAMEINFO: hook=N calls= caller=`). This is not yet verified on the phone.

## Implementation consequences

Both slots are ordinary virtual overrides. `build-houji-display.py` emits and verifies a full authenticated arm64e subclass vtable, including original per-slot discriminators, so no unsigned copied vtable is required. The existing user-client copied-vtable telemetry is deliberately disabled under PAC; there is no established PAC-safe selector-73 boundary hook. The draft therefore counts selector-74 calls, completion callbacks, deferred early completions, sends/drops, and prints bounded/power-of-two samples.

The iOS 16 `SweetFramebuffer.cpp` only implemented selectors 3–8, 18, 28 and 72 plus software type-5 vsync. Selectors 73/74 fell through its generic success/log path; it emitted no type-3 FrameInfo or completion reports.

`fb-frameinfo=1` reports GPU/CPU copy completion as all presentation/VBL values. This is intentionally **not** physical scanout or measured vblank. Root101 (1029 delivered frames by 598 s, finger-up received, settling frames stop until another touch) makes missing completion feedback plausible but does not validate this synthesized timestamp contract.
