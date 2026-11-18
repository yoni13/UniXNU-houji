# Persistent iOS 27 Data on houji — draft

The current source path publishes authenticated, read-only OS and SystemOS windows. DarwinBoot hands the separate packed Data seed to `NativeRamBlock`; without persistence, Data changes live in RAM and are lost on reboot. The draft is not applied to the active trees. **No hardware behavior or kernel compile/link is verified by this work.**

## What the patches implement

- Exactly one `native-data=userdata` token opts in. Missing, misspelled, longer or duplicate arguments leave the volatile disk, with no UFS WRITE commands. `data-reset=1` explicitly replaces overlay metadata; `data-reset=2` does not. Reboot without `data-reset=1` to retain the overlay.
- The store requires a packed 2 GiB seed and a validated LUN 0 primary GPT. It rechecks READ CAPACITY(10), the 4 KiB logical block size, header and exact entry-array CRCs, geometry, non-overlapping entries and the unique, exact UTF-16 `userdata` name. The GPT must match the layout used to locate the root manifest. The backup header and array are **not** parsed or repaired.
- The 524,306-block overlay ends 1 GiB before the end of `userdata`. Its start must be at least 4 MiB beyond the larger of the manifest image size or the 16,904,093,696-byte SHNGROOT floor. Both the writer and the final doorbell path check the command and bounds; the separate Sheng log/export window and unlocked panic submissions are removed. The legacy probe stays read-only. A timeout quarantines the DMA region, and later commands cannot ring the doorbell or reuse it.
- The disk opens the overlay before APFS can probe it. `NativeRamBlock` serves unchanged pages from the immutable seed and directs changed pages to UFS. `SHNGDATA` v2 binds the header to the seed SHA-256 and page count; each bitmap block has a CRC bound to its index and generation. Wrong, missing or damaged metadata falls back to volatile RAM without writes unless reset was explicitly requested. Reset durably invalidates the old header before replacing the bitmap, then publishes the new header last. v1 metadata is not accepted by this draft.
- Normal writes use the device cache; synchronize, the five-second timer, and `stop()` order data flush before bitmap commit and flush. `data-fua=1` uses WRITE(10) FUA instead. Once active, a store I/O failure reports an error and revokes the write window rather than silently changing a mounted volume to the seed.

## Apply to copies and run host tests

Use `patch -p1` in the device copy for `docs/persistent-data-ios27.patch` and in the UniXNU copy for `scripts/unixnu-persistent-data.patch`. The latter touches the files inside the pinned `drivers/RamBlock` submodule; ensure the copy contains its checked-out files. The device patch expects the `package/PersistentDataCore.h` already present in `91c04f7`, or use the staged copy of that header when starting from a tree that lacks it. Then run:

```sh
timeout 120 bash scripts/test-persistent-data.sh /path/to/pinned/ShengUfs /path/to/legacy/ufs-root /path/to/patched/UniXNU
```

This regenerates the actual UFS headers from ShengUfs `99d8bcd3e43dd24fc3c8437c67b5d4cc71ab94d0`. The test uses production CRC, window, command, transfer-poll, and overlay code with synthetic GPT/metadata and fake MMIO, DMA and clock. It counts doorbell submissions and checks the DMA buffer after an injected timeout. The Linux host run passed 40 checks with GCC and 40 with Clang ASan/UBSan. Four existing RamBlock host tests passed in v1 mode; three also passed in v2 mode (direct, writeback and concurrent). These are not device traces.

## Still unverified on hardware

Treat the partition geometry, block size, real UFS ownership and timeouts, zero-write default, and write confinement as unverified on houji. Before any authorized hardware trial, review the intended tail extent and independently confirm it contains no data to keep. Do not flash a seed or overlay image to `userdata`: this design needs only the tail window and does not replace SHNGROOT. Retain an independently verified recovery backup.

The 24B156 arm64e compile, link/prelink, retained IOStorageFamily/APFS ABI, UUID publication, and mount remain unverified. So do marker persistence across reboot, FUA/cache semantics, idle flush, unmount/stop and power loss. An in-flight write may complete after a timeout; the poison stops new commands, not that write. Metadata CRCs are not authentication or page checksums. An overwritten page can tear on power loss; the overlay is not a transaction log. A GPT mismatch protects Data writes but does not promise the read-only root will boot through a broken GPT.
