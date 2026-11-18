# Persistent iOS 27 Data on houji

## Current flow

`pack.sh` passes a local `.dbr` with `--data-ramdisk`. DarwinBoot records it as `/chosen/memory-map/PackedRAMDisk`; `SourceStorage.cpp` starts `NativeRamBlock`, APFS publishes its configured UUID, and owner-Data code mounts it at `/private/var`. The packed seed is an immutable sparse base plus volatile changed pages, so every restart returns to the seed. `prepare-houji-ufs.py` currently rejects `hostSubmit(direction=1)` and `hostWrite4k`; UFS only publishes authenticated OS and SystemOS windows (`NativeUfs: ... volume ... ready`). Default boot arguments are `native-source-storage=1 native-ufs-root=1`.

## iOS 16 precedent

iOS 16 did not have a separate APFS Data container: one writable HFSX root, including `/private/var`, occupied the first 32 GiB of GPT `userdata` (LUN 0, first LBA 2260480). A verified image was installed with `fastboot flash userdata`; `HoujiUfsRead.cpp` exposed it as `md0` through `mdevadd_ops`, not `IOMedia`. Reads/writes were 512-byte-facing, with 4 KiB read/modify/write and 128 KiB maximum UFS transfers. `RootCommands.h` admitted WRITE(10) only while armed and only inside the compiled extent; synchronize issued SYNCHRONIZE CACHE(10). A valid HFSX block-zero guard and a “do not zero an existing JNLx sector” guard followed two real header-loss incidents. Flush/readback was used for diagnostic writes. Forced resets remain a corruption risk.

## Sheng iPadOS 27 template

Sheng publishes OS/SystemOS as read-only `IOBlockStorageDevice`s. Data remains the `NativeRamBlock` device selected by UUID, but `sheng-data-persist=2` attaches its `PersistOverlay` to a UFS window derived from the exact UTF-16 GPT name `userdata`. The window is after the SHNGROOT image plus a 4 MiB guard and before a fixed tail reserve. Every write is rechecked by `hostInWindow`; WRITE(10) supports FUA, and cached mode orders page writes, SYNCHRONIZE CACHE, bitmap writes, then SYNCHRONIZE CACHE. The overlay header binds page count and seed SHA-256. `data-reset=1/2` alone initializes or replaces it; otherwise blank, damaged, or foreign state is preserved.

## Proposed houji design

Adapt that template, gated only by exact string `native-data=userdata`; absent/other values keep RAM-only behavior and reject all storage writes. Re-enable upstream store writes, retain exact GPT/CRC/duplicate-name checks, and open only a non-overlapping tail window inside that discovered extent. Keep the existing 2 GiB packed seed and `NativeRamBlock` publication: first install with `data-reset=1` writes only overlay metadata; unchanged pages continue from the seed, while changed pages live on UFS. Later boots use only `native-data=userdata`. Use cached WRITE(10), FUA fallback (`data-fua=1`), APFS synchronize, the five-second idle flush, and `NativeRamBlock::stop()` final flush. Never permit writes to OS windows or another partition.

Do **not** fastboot-flash the seed alone. The 30,281,856-byte `.dbr` is packed `DBRDISK1`, reconstructing a 2 GiB raw APFS image; `userdata` is 517,176,537,088 bytes, but both packed and unpacked forms at partition offset zero would destroy the 16,904,093,696-byte SHNGROOT image. A future direct-container design would require an offline composite image and a new bounded Data block device; it is larger and unnecessary.

## Test and recovery

1. Host-test exact-name/duplicate/CRC parsing, integer-overflow and first/last-block write refusal; verify default and wrong arguments issue zero WRITE commands.
2. In stock recovery/fastboot, verify the reported `userdata` size, then flash the verified composite `userdata-verified.img`; keep the local iOS 16 backup.
3. Boot `native-data=userdata data-reset=1 data-fua=1`; confirm GPT checks 0 before/after, state `Fresh`, APFS UUID, and writes confined to the logged window.
4. Create a marker, synchronize, reboot, then boot with `native-data=userdata`; require state `Resumed` and marker persistence. Repeat with cached mode, idle flush, clean reboot, forced reset, and power loss only after backups.
5. Negative-test no argument and a misspelling: volatile seed, no UFS writes. Corrupt only a saved copy of overlay metadata and require fail-closed seed operation.
6. Recovery: boot stock recovery, reflash `userdata-verified.img`, and boot without the opt-in; restore the complete iOS 16 backup only with its matching kernel.

Risks are APFS/overlay corruption after a hang, forced reset, timeout, or lost flush. iOS-initiated shutdown currently hangs in the unmount/reboot path on this kernel (one log completed Data unmount but stalled later), so it is not yet evidence of safe power-off. A UFS timeout must poison the transport and stop later writes.
