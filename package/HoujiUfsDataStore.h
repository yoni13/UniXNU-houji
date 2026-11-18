// SPDX-License-Identifier: BSD-2-Clause-Patent
// Included by the generated NativeUfs.cpp, after root layout and transport.
namespace {
struct StoreGeometry {
  bool valid;
  uint64_t first, last, imageBytes, entriesLba;
  uint32_t headerCrc, entriesCrc, entryCount;
};
StoreGeometry gStoreGeometry;
bool gStoreOpened; // only one publication attempt per boot; protected by gHost.lock

static bool readLun0Capacity(uint64_t &last)
{
  const uint8_t cdb[10] = {0x25}; // READ CAPACITY(10)
  uint32_t status = 0;
  IOLockLock(gHost.lock);
  const bool ok = gHost.ready && hostScsi(0, cdb, sizeof(cdb), true, 8, 1000000, &status) == kIOReturnSuccess &&
                  loadBe32(gHost.virt + kDataOffset + 4) == houji_data::blockBytes;
  if (ok) last = loadBe32(gHost.virt + kDataOffset);
  IOLockUnlock(gHost.lock);
  return ok && last != UINT32_MAX;
}

static unsigned verifyStoreGpt()
{
  if (!gStoreGeometry.valid) return 1;
  uint64_t last = 0;
  if (!readLun0Capacity(last)) return 2;
  constexpr size_t bytes = houji_data::maxEntries * houji_data::entryBytes;
  auto *buffer = static_cast<uint8_t *>(IOMalloc(bytes));
  if (!buffer) return 3;
  houji_data::Gpt gpt = {};
  houji_data::Extent extent = {};
  unsigned error = 0;
  if (hostRead4k(0, 1, 1, buffer) != kIOReturnSuccess || !houji_data::gptHeader(buffer, last, gpt) ||
      gpt.headerCrc != gStoreGeometry.headerCrc || gpt.entriesCrc != gStoreGeometry.entriesCrc ||
      gpt.entriesLba != gStoreGeometry.entriesLba || gpt.count != gStoreGeometry.entryCount) error = 4;
  else if (hostRead4k(0, gpt.entriesLba, gpt.entryBlocks, buffer) != kIOReturnSuccess ||
           !houji_data::gptUserdata(gpt, buffer, bytes, extent) ||
           extent.first != gStoreGeometry.first || extent.last != gStoreGeometry.last) error = 5;
  IOFree(buffer, bytes);
  return error;
}
} // namespace

extern "C" unsigned native_ufs_store_mode(void) { return houjiDataArmed() ? 2 : 0; }
extern "C" bool native_ufs_store_reset_requested(void)
{
  return houji_data::resetRequested(PE_boot_args(), BOOT_LINE_LENGTH);
}

extern "C" unsigned native_ufs_store_gpt_check(void)
{
  return houjiDataArmed() ? verifyStoreGpt() : 1;
}

extern "C" int native_ufs_store_open(uint64_t blocks, uint64_t *firstLba, const char **why)
{
  houji_data::Window window = {};
  const char *reason = "";
  if (!houjiDataArmed()) reason = "off";
  else if (blocks != houji_data::storeBlocks) reason = "not the 2 GiB Data seed layout";
  else if (!gStoreGeometry.valid || !houji_data::tailWindow({gStoreGeometry.first, gStoreGeometry.last},
                                                          gStoreGeometry.imageBytes, blocks, window)) reason = "unsafe userdata tail";
  else if (verifyStoreGpt()) reason = "LUN 0 capacity/GPT refused";
  else {
    IOLockLock(gHost.lock);
    if (!gHost.ready || gHost.poisoned || gStoreOpened) reason = "transport unavailable or store already opened";
    else { gHoujiStoreWindow = window; gStoreOpened = true; }
    IOLockUnlock(gHost.lock);
    if (!*reason && firstLba) *firstLba = window.first;
  }
  if (why) *why = reason;
  return !*reason;
}

extern "C" void native_ufs_store_close(void)
{
  IOLockLock(gHost.lock);
  gHoujiStoreWindow = {};
  IOLockUnlock(gHost.lock);
}

// NativeRamBlock opens once before publication. On failure (or stop after draining I/O) it can close.
namespace {
static houji_data::Window storeWindow()
{
  IOLockLock(gHost.lock);
  const auto window = gHoujiStoreWindow;
  IOLockUnlock(gHost.lock);
  return window;
}
} // namespace

extern "C" int native_ufs_store_read(uint64_t rel, uint32_t blocks, void *dst)
{
  const auto window = storeWindow();
  if (!dst || !houji_data::contains({0, window.blocks}, rel, blocks)) return 0;
  while (blocks) {
    const uint32_t chunk = blocks > kDataMax / 4096 ? kDataMax / 4096 : blocks;
    if (hostRead4k(0, window.first + rel, chunk, dst) != kIOReturnSuccess) return 0;
    dst = static_cast<uint8_t *>(dst) + chunk * 4096;
    rel += chunk;
    blocks -= chunk;
  }
  return 1;
}

static int houjiStoreWrite(uint64_t rel, uint32_t blocks, const void *src, bool fua)
{
  const auto window = storeWindow();
  if (!src || !houji_data::contains({0, window.blocks}, rel, blocks)) return 0;
  while (blocks) {
    const uint32_t chunk = blocks > kDataMax / 4096 ? kDataMax / 4096 : blocks;
    if (hostWrite4k(0, window.first + rel, chunk, src, fua) != kIOReturnSuccess) return 0;
    src = static_cast<const uint8_t *>(src) + chunk * 4096;
    rel += chunk;
    blocks -= chunk;
  }
  return 1;
}

extern "C" int native_ufs_store_write(uint64_t rel, uint32_t blocks, const void *src)
{
  return houjiStoreWrite(rel, blocks, src, true);
}
extern "C" int native_ufs_store_write_cached(uint64_t rel, uint32_t blocks, const void *src)
{
  return houjiStoreWrite(rel, blocks, src, false);
}
extern "C" int native_ufs_store_sync(void) { return hostSyncCache() == kIOReturnSuccess; }
