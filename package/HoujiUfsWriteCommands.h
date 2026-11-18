// SPDX-License-Identifier: BSD-2-Clause-Patent
// Included inside the root transport namespace by the generated RootCommands.h.
// gHoujiStoreWindow is set only by native_ufs_store_open, under the host lock.
static houji_data::Window gHoujiStoreWindow;

static IOReturn hostWrite4k(uint8_t lun, uint64_t lba, uint32_t blocks, const void *src, bool fua)
{
  if (!src || !blocks || blocks > kDataMax / 4096 || lun != 0) return kIOReturnBadArgument;
  IOLockLock(gHost.lock);
  IOReturn result = kIOReturnNotPermitted;
  if (gHost.ready && !gHost.poisoned && houjiDataArmed() && houji_data::contains(gHoujiStoreWindow, lba, blocks)) {
    // A waiting writer must recheck ownership after obtaining the lock, before touching DMA.
    if (!hostClearUcd()) result = kIOReturnNotReady;
    else {
      gRootDiagnostic.lba = lba;
      memcpy(gHost.virt + kDataOffset, src, blocks * 4096U);
      const uint8_t cdb[10] = {0x2a, uint8_t(fua ? 0x08 : 0), uint8_t(lba >> 24), uint8_t(lba >> 16),
                               uint8_t(lba >> 8), uint8_t(lba), 0, uint8_t(blocks >> 8), uint8_t(blocks), 0};
      uint32_t status = 0;
      result = hostScsiDir(lun, cdb, sizeof(cdb), 1, blocks * 4096U, 2000000, &status);
    }
  }
  IOLockUnlock(gHost.lock);
  return result;
}

static IOReturn hostSyncCache()
{
  IOLockLock(gHost.lock);
  IOReturn result = kIOReturnNotReady;
  if (gHost.ready && !gHost.poisoned && houjiDataArmed() && gHoujiStoreWindow.blocks) {
    const uint8_t cdb[10] = {0x35}; // SYNCHRONIZE CACHE(10) on LUN 0
    uint32_t status = 0;
    result = hostScsiDir(0, cdb, sizeof(cdb), 0, 0, 10000000, &status);
  }
  IOLockUnlock(gHost.lock);
  return result;
}
