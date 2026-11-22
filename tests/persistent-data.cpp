// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test using the generated production command path. Only MMIO, DMA/cache,
// the clock, allocation, and locking are faked. No files are opened as disks.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#include "PersistentDataCore.h"
#include "DirectDisk.h"
#include "RootDiagnostics.h"
#include "RootTransportLayout.h"
#include "TransferPoll.h"

using IOReturn = uint32_t;
constexpr IOReturn kIOReturnSuccess = 0, kIOReturnError = 1, kIOReturnBadArgument = 2,
    kIOReturnIOError = 3, kIOReturnBusy = 4, kIOReturnTimeout = 5, kIOReturnNotReady = 6, kIOReturnNotPermitted = 7;
constexpr unsigned kMicrosecondScale = 1, BOOT_LINE_LENGTH = 1024;
constexpr uint32_t capacityLast = 126000000;
constexpr uint64_t partFirst = 2260480, partLast = capacityLast - 10;
using Block = std::array<uint8_t, houji_data::blockBytes>;
static std::map<uint32_t, Block> disk;
static UfsRootDiagnostics gRootDiagnostic;
alignas(4096) static uint8_t dma[kDmaBytes];
static struct { uint8_t *virt; uint64_t phys; bool ready, poisoned; uintptr_t hc; void *lock; } gHost;
static char boot[BOOT_LINE_LENGTH];
static unsigned writes, rings, syncs, locks, blockSize;
static uint32_t db, is;
static uint64_t now;
static bool timeout;
struct Transfer { uint8_t opcode, flags; uint32_t lba; };
static std::vector<Transfer> changes;
static int passed, failed;
static bool check(bool result, const char *label)
{
    std::printf("%s: %s\n", result ? "PASS" : "FAIL", label);
    result ? ++passed : ++failed;
    return result;
}

static char *PE_boot_args() { return boot; }
static bool houjiDataArmed() { return houji_data::optedIn(PE_boot_args(), sizeof(boot)); }
static void *IOMalloc(size_t size) { return std::malloc(size); }
static void IOFree(void *p, size_t) { std::free(p); }
static void IOLockLock(void *) { if (locks++) std::abort(); }
static void IOLockUnlock(void *) { if (locks-- != 1) std::abort(); }
static IOReturn recordTransport(IOReturn value, uint32_t) { return value; }
static void cacheCivac(const void *, size_t) {}
static void cacheInvalidate(const void *, size_t) {}
static uint32_t read32(uintptr_t, unsigned reg) { return reg == 0x58 ? db : is; }
static uint64_t mach_absolute_time() { return now; }
static void IODelay(unsigned) { now += 2000000; }
static void clock_interval_to_deadline(uint32_t interval, unsigned scale, uint64_t *end) { *end = now + interval * scale; }
static void hostWrite(unsigned reg, uint32_t value);
#include "RootCommands.h"
#include "HoujiUfsDataStore.h"

static void hostWrite(unsigned reg, uint32_t value)
{
    if (reg == 0x20) { is &= ~value; return; }
    if (reg != 0x58 || value != 1 || locks != 1) std::abort();
    const auto *ucd = dma + kUcdOffset;
    const auto *cdb = ucd + 16;
    ++rings;
    if (cdb[0] == 0x2a) ++writes;
    if (cdb[0] == 0x35) ++syncs;
    if (cdb[0] == 0x2a || cdb[0] == 0x35) changes.push_back({cdb[0], cdb[1], loadBe32(cdb + 2)});
    if (timeout) { db = 1; return; }
    auto *data = dma + kDataOffset;
    const uint32_t lba = loadBe32(cdb + 2), count = uint32_t(cdb[7]) << 8 | cdb[8];
    if (cdb[0] == 0x25) {
        putBe32(data, capacityLast);
        putBe32(data + 4, blockSize);
    } else if (cdb[0] == 0x28 || cdb[0] == 0x2a) {
        for (uint32_t i = 0; i < count; ++i) {
            auto &block = disk[lba + i];
            if (cdb[0] == 0x28) memcpy(data + i * block.size(), block.data(), block.size());
            else memcpy(block.data(), data + i * block.size(), block.size());
        }
    }
    auto *rsp = dma + kUcdOffset + kRspOffset;
    rsp[0] = 0x21;
    rsp[2] = ucd[2];
    is = 1;
    reinterpret_cast<uint32_t *>(dma)[2] = 0;
}

static void resetHost(const char *line)
{
    std::snprintf(boot, sizeof(boot), "%s", line);
    memset(dma, 0, sizeof(dma));
    gHost = {dma, 0x10000, true, false, 0, nullptr};
    gHoujiStoreWindow = {};
    gStoreGeometry = {};
    gStoreOpened = false;
    writes = rings = syncs = locks = db = is = 0;
    changes.clear();
    blockSize = 4096;
    now = 0;
    timeout = false;
}

struct Fixture {
    Block header{}, entries{};
    void entry(unsigned index, const char *name, uint64_t first, uint64_t last) {
        auto *p = entries.data() + 128 * index;
        p[0] = 1; // nonzero type GUID
        persist::put64(p + 32, first);
        persist::put64(p + 40, last);
        for (unsigned i = 0; name[i]; ++i) p[56 + 2 * i] = name[i];
    }
    void seal() {
        persist::put32(header.data() + 88, persist::crc32(entries.data(), 3 * 128));
        persist::put32(header.data() + 16, 0);
        persist::put32(header.data() + 16, persist::crc32(header.data(), 92));
    }
    Fixture() {
        memcpy(header.data(), "EFI PART", 8);
        persist::put32(header.data() + 8, 0x10000);
        persist::put32(header.data() + 12, 92);
        persist::put64(header.data() + 24, 1);
        persist::put64(header.data() + 32, capacityLast);
        persist::put64(header.data() + 40, 3);
        persist::put64(header.data() + 48, partLast);
        persist::put64(header.data() + 72, 2);
        persist::put32(header.data() + 80, 3);
        persist::put32(header.data() + 84, 128);
        entry(0, "userdata", partFirst, partLast);
        entry(1, "userdata_extra", 100, 130);
        seal();
    }
    bool parse(houji_data::Extent &extent) const {
        houji_data::Gpt gpt{};
        return houji_data::gptHeader(header.data(), capacityLast, gpt) &&
            houji_data::gptUserdata(gpt, entries.data(), entries.size(), extent);
    }
    void install() const {
        disk.clear(); disk[1] = header; disk[2] = entries;
        gStoreGeometry = {true, partFirst, partLast, houji_data::rootBytes, 2,
                          houji_data::le32(header.data() + 16), houji_data::le32(header.data() + 88), 3};
    }
};

struct Store {
    bool cached = false;
    bool refuseBitmap = false;
    bool read(uint64_t b, uint32_t n, void *d) { return native_ufs_store_read(b, n, d); }
    bool write(uint64_t b, uint32_t n, const void *s) {
        if (refuseBitmap && b == 1) return false;
        return cached ? native_ufs_store_write_cached(b, n, s) : native_ufs_store_write(b, n, s);
    }
    bool sync() { return native_ufs_store_sync(); }
};

struct Buffer {
    Block &bytes;
    uint64_t copy(uint64_t at, const void *p, uint64_t n) { memcpy(bytes.data() + at, p, n); return n; }
    uint64_t read(uint64_t at, void *p, uint64_t n) { memcpy(p, bytes.data() + at, n); return n; }
};

static int rawWrite(uint64_t lba, unsigned direction = 1, uint8_t opcode = 0x2a, uint8_t lun = 0, uint8_t blocks = 1)
{
    const uint8_t cdb[10] = {opcode, 0, uint8_t(lba >> 24), uint8_t(lba >> 16), uint8_t(lba >> 8), uint8_t(lba), 0, blocks, 0};
    uint32_t scsi = 0;
    IOLockLock(gHost.lock);
    auto result = hostScsiDir(lun, cdb, sizeof(cdb), direction, direction ? 4096 : 0, 1, &scsi);
    IOLockUnlock(gHost.lock);
    return result;
}

int main()
{
    using namespace houji_data;
    const Fixture valid;
    Extent extent{};
    Fixture f = valid;
    f.header[100] ^= 1; f.entries[500] ^= 1; // CRCs must NOT include sector padding
    check(f.parse(extent) && extent.first == partFirst && extent.last == partLast, "valid GPT, exact CRC byte ranges and UTF-16 name");
    f = valid; f.header[60] ^= 1;
    check(!f.parse(extent), "bad GPT header CRC refused");
    f = valid; f.entries[383] ^= 1;
    check(!f.parse(extent), "bad GPT entries CRC refused");
    f = valid; f.entry(2, "userdata", 140, 160); f.seal();
    check(!f.parse(extent), "duplicate non-overlapping userdata names refused");
    f = valid; f.entries[57] = 1; f.seal();
    check(!f.parse(extent), "missing exact userdata (prefix and non-ASCII near matches) refused");
    f = valid; f.entry(2, "firmware", partFirst - 1, partFirst + 1); f.seal();
    check(!f.parse(extent), "overlapping GPT entries refused");
    f = valid; persist::put64(f.header.data() + 72, UINT64_MAX); f.seal();
    check(!f.parse(extent), "overflowing GPT entry array LBA refused");
    f = valid; persist::put64(f.entries.data() + 40, UINT64_MAX); f.seal();
    check(!f.parse(extent), "overflowing partition end refused");
    Window w{};
    const uint64_t minimum = rootBytes / blockBytes + guardBlocks + reserveBlocks + storeBlocks;
    check(tailWindow({partFirst, partFirst + minimum - 1}, rootBytes, storeBlocks, w) &&
          w.first == partFirst + rootBytes / blockBytes + guardBlocks && w.blocks == storeBlocks,
          "exact fit stays after root plus guard and before tail reserve");
    check(!tailWindow({partFirst, partFirst + minimum - guardBlocks - 2}, 4096, storeBlocks, w),
          "OS overlap refused even if manifest claims a smaller image");
    check(!tailWindow({partFirst, partFirst + minimum - 2}, rootBytes, storeBlocks, w) &&
          !tailWindow({partFirst, partLast}, UINT64_MAX, storeBlocks, w) &&
          !tailWindow({partFirst, UINT64_MAX}, rootBytes, storeBlocks, w) &&
          !tailWindow({partFirst, partLast}, rootBytes, UINT64_MAX, w), "guard, huge image, LBA and length overflow refused");

    Block page{};
    for (auto line : {"", "data-reset=1", "native-data=userdat", "native-data=userdata_extra",
                      "native-data=userdata native-data=bad", "native-data=userdata native-data", "-native-data=userdata",
                      "native-data =userdata", "x=native-data=userdata", "sheng-data-persist=2 sheng-log=1"}) {
        resetHost(line);
        valid.install();
        bool off = !native_ufs_store_mode() && !native_ufs_store_open(storeBlocks, nullptr, nullptr);
        gHoujiStoreWindow = {100, 2}; // stale/accidental window must not bypass opt-in at either layer
        off &= !native_ufs_store_reset_requested() && !native_ufs_store_write(0, 1, page.data()) &&
               !native_ufs_store_write_cached(0, 1, page.data()) && rawWrite(100) == kIOReturnNotPermitted &&
               !native_ufs_store_sync() && !rings && !writes;
        char label[180]; std::snprintf(label, sizeof(label), "zero submitted writes without exact opt-in [%s]", line);
        check(off, label);
    }
    resetHost("native-data=userdata data-reset=01");
    const char reset2[] = "native-data=userdata data-reset=2";
    check(native_ufs_store_mode() == 2 && !native_ufs_store_reset_requested() && !resetRequested(reset2, sizeof(reset2)) &&
          !optedIn("native-data=userdata", sizeof("native-data=userdata") - 1), "non-exact reset and unterminated command line refused");
    // The 5-byte alias dr=1 resets exactly as data-reset=1 does, and only as exactly.
    const char drOk[] = "native-data=userdata dr=1", drAlone[] = "dr=1", drPadded[] = "native-data=userdata dr=01",
               drTwo[] = "native-data=userdata dr=2", drTwice[] = "native-data=userdata dr=1 dr=1";
    check(resetRequested(drOk, sizeof(drOk)) && !resetRequested(drAlone, sizeof(drAlone)) &&
          !resetRequested(drPadded, sizeof(drPadded)) && !resetRequested(drTwo, sizeof(drTwo)) &&
          !resetRequested(drTwice, sizeof(drTwice)), "the dr=1 alias resets exactly as data-reset=1");

    resetHost("native-data=userdata data-reset=1"); valid.install();
    blockSize = 512;
    check(!native_ufs_store_open(storeBlocks, nullptr, nullptr) && !writes, "READ CAPACITY block size mismatch refuses store");
    blockSize = 4096; disk[2][40] ^= 1;
    check(!native_ufs_store_open(storeBlocks, nullptr, nullptr) && !writes, "changed on-device GPT before open refuses store");
    valid.install();
    uint64_t first = 0;
    if (!check(native_ufs_store_reset_requested() && native_ufs_store_gpt_check() == 0 &&
               native_ufs_store_open(storeBlocks, &first, nullptr) && first == partLast + 1 - reserveBlocks - storeBlocks &&
               !native_ufs_store_open(storeBlocks, nullptr, nullptr), "production store verifies GPT and opens only fixed tail once")) return 1;
    const uint64_t last = first + storeBlocks - 1;
    const unsigned before = rings;
    check(hostWrite4k(0, first - 1, 1, page.data(), true) != kIOReturnSuccess &&
          hostWrite4k(0, last + 1, 1, page.data(), true) != kIOReturnSuccess &&
          hostWrite4k(0, last, 2, page.data(), true) != kIOReturnSuccess &&
          hostWrite4k(0, UINT64_MAX, 1, page.data(), true) != kIOReturnSuccess &&
          !native_ufs_store_write(UINT64_MAX, 1, page.data()) && !native_ufs_store_write(storeBlocks, 1, page.data()) &&
          rawWrite(first - 1) == kIOReturnNotPermitted && rawWrite(last + 1) == kIOReturnNotPermitted &&
          rawWrite(first, 1, 0x2a, 1) == kIOReturnNotPermitted && rawWrite(first, 1, 0x2a, 0, 2) == kIOReturnNotPermitted &&
          rawWrite(first, 2) == kIOReturnNotPermitted && rawWrite(first, 0) == kIOReturnNotPermitted &&
          rawWrite(first, 1, 0x8a) == kIOReturnNotPermitted && rings == before && !writes,
          "both write paths reject first/last crossings, truncation, wrong LUN, opcode, direction and length");
    check(native_ufs_store_write(0, 1, page.data()) && native_ufs_store_write(storeBlocks - 1, 1, page.data()) && writes == 2,
          "the first and last blocks inside the window can be written");
    check(changes.size() == 2 && changes[0].flags == 8 && changes[1].flags == 8, "write-through sets WRITE(10) FUA");

    static uint8_t bits[persist::kBlock * 17], dirty[17], seed[32] = {3};
    check(storeBlocks == persist::windowBlocks(seedPages) && persist::kBitsPerBlock == bitmapBitsPerBlock,
          "RamBlock v2 layout and transport window agree");
    Store store;
    persist::Overlay overlay;
    const auto blankWrites = writes;
    check(overlay.open(store, seedPages, seed, false, bits) == persist::Open::Kept && !overlay.active && writes == blankWrites,
          "blank overlay without reset falls back without writes");
    changes.clear();
    check(overlay.open(store, seedPages, seed, false, bits, native_ufs_store_reset_requested()) == persist::Open::Fresh &&
          overlay.active && !overlay.present && syncs == 3 && changes.size() == 6 &&
          changes[0].lba == first && changes[1].opcode == 0x35 && changes[2].lba == first + 1 &&
          changes[3].opcode == 0x35 && changes[4].lba == first && changes[5].opcode == 0x35,
          "exact reset writes invalid header, fresh bitmap, then valid header");
    overlay.dirty = dirty;
    store.cached = true; page[0] = 17;
    changes.clear();
    const uint64_t index = bitmapBitsPerBlock;
    Block base{}; base.fill(3);
    std::vector<uint8_t *> seedIndex(seedPages);
    seedIndex[index] = base.data();
    RamSparseDisk sparse = {seedIndex.data(), seedPages * blockBytes, blockBytes};
    Block stage{}, result{};
    Buffer input{page}, output{result};
    bool storeError = false;
    check(directDiskIO(sparse, overlay, store, stage.data(), 1, true, index * blockBytes, 512, input, &storeError) == 512 &&
          !storeError && overlay.flush(store) && syncs == 5 && changes.size() == 4 &&
          changes[0].lba == first + persist::pageBlock(seedPages, index) && !changes[0].flags &&
          changes[1].opcode == 0x35 && changes[2].lba == first + 2 && !changes[2].flags && changes[3].opcode == 0x35 &&
          overlay.open(store, seedPages, seed, false, bits) == persist::Open::Resumed && overlay.has(index) &&
          directDiskIO(sparse, overlay, store, stage.data(), 1, false, index * blockBytes, blockBytes, output, &storeError) == blockBytes &&
          !storeError && result[0] == 17 && result[512] == 3 && base[0] == 3,
          "partial write keeps seed immutable and commits page then bitmap across reopen");
    const Block savedHeader = disk[uint32_t(first)];
    const Block savedBits = disk[uint32_t(first + 1)];
    const auto resumeWrites = writes;
    seed[0] ^= 1;
    check(overlay.open(store, seedPages, seed, false, bits) == persist::Open::Kept && !overlay.active && writes == resumeWrites,
          "wrong seed hash falls back without modifying metadata");
    seed[0] ^= 1; disk[uint32_t(first)][56] ^= 1;
    check(overlay.open(store, seedPages, seed, false, bits) == persist::Open::Kept && !overlay.active && writes == resumeWrites,
          "corrupt overlay header falls back without writes");
    disk[uint32_t(first)] = savedHeader;
    disk[uint32_t(first + 1)][0] ^= 1;
    check(overlay.open(store, seedPages, seed, false, bits) == persist::Open::Failed && !overlay.active && writes == resumeWrites,
          "bitmap CRC catches in-range bit corruption, with no repair writes");
    disk[uint32_t(first + 1)] = savedBits;
    disk[uint32_t(first + 1)] = disk[uint32_t(first + 2)];
    check(overlay.open(store, seedPages, seed, false, bits) == persist::Open::Failed && !overlay.active && writes == resumeWrites,
          "bitmap CRC binds its index and rejects relocated blocks");
    disk[uint32_t(first + 1)] = savedBits;

    store.cached = false; store.refuseBitmap = true;
    check(overlay.open(store, seedPages, seed, false, bits, true) == persist::Open::Failed && !overlay.active &&
          overlay.open(store, seedPages, seed, false, bits) == persist::Open::Kept && !overlay.active,
          "interrupted replacement cannot resume old header with a partly replaced bitmap");

    timeout = true;
    const auto beforeTimeout = rings;
    check(hostWrite4k(0, first, 1, page.data(), true) == kIOReturnTimeout && gHost.poisoned && !gHost.ready &&
          rings == beforeTimeout + 1, "write timeout poisons the real command transport");
    auto *snapshot = new uint8_t[kDmaBytes]; memcpy(snapshot, dma, kDmaBytes);
    timeout = false; db = 0; page[0] = 90;
    check(!native_ufs_store_write(0, 1, page.data()) && !native_ufs_store_write_cached(0, 1, page.data()) &&
          !native_ufs_store_read(0, 1, page.data()) && rawWrite(first) == kIOReturnNotReady && !native_ufs_store_sync() &&
          rings == beforeTimeout + 1 &&
          !memcmp(snapshot, dma, kDmaBytes), "after timeout no doorbell and no DMA modification even if doorbell clears");
    delete[] snapshot;
    native_ufs_store_close();
    check(!native_ufs_store_read(0, 1, page.data()) && !native_ufs_store_write(0, 1, page.data()) &&
          !gHoujiStoreWindow.blocks && rings == beforeTimeout + 1,
          "closing the store clears both read and write grants");
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
