// SPDX-License-Identifier: BSD-2-Clause-Patent
// Firmware files for the remote processors (they are never part of the build), from one of two places:
// - "modem:NAME": the device's own split image NAME.b00, NAME.b01, ... in /image of its firmware partition (modem_a, else modem_b), a
//   FAT volume read in place through the UFS driver (read only; nothing is installed and the boot image does not grow);
// - any other path: a file on a mounted volume (such as adsp.mbn installed by the owner below /private/var/firmware), read through VFS.
// Plain reads on the caller's thread: the ADSP thread is the only caller and it runs no interrupt work, so a slow read only delays that
// bring-up.
#include <sys/param.h>
#include <sys/systm.h>
#include <IOKit/IOLib.h>
#include <kern/locks.h>
#include "FatVolume.h"
#include "QcomMdt.h"

int sheng_fw_open(const char *path, void **handle, uint64_t *size);
int sheng_fw_read(void *handle, uint64_t offset, void *to, size_t length);
void sheng_fw_close(void *handle);

extern int native_ufs_part_find(const char *name, uint32_t *lun, uint64_t *first, uint64_t *blocks);
extern int native_ufs_part_read(uint32_t lun, uint64_t lba, uint32_t blocks, void *dst);

// ---- the firmware partition ----
struct partition {
    uint32_t lun;
    uint64_t first;
    uint64_t cached;  // block in block (UINT64_MAX: none)
    uint8_t block[4096];
};
// Volume 0: the firmware partition (modem_a, else modem_b); volume 1 (houji): the modem's own, modemfirmware_a or _b ("modemfw:" paths).
static struct partition *gParts[2];
static FatVolume *gVolumes[2];
#define gVolume gVolumes[0]
// The ADSP and Wi-Fi threads share the volume and its one-block cache.
static lck_grp_t *gFwLockGroup;
static lck_mtx_t *gFwLock;
static void fwLock(void)
{
    if (!__atomic_load_n(&gFwLock, __ATOMIC_ACQUIRE)) {
        static int once;
        if (!__atomic_exchange_n(&once, 1, __ATOMIC_ACQ_REL)) {
            gFwLockGroup = lck_grp_alloc_init("houji-fw", LCK_GRP_ATTR_NULL);
            __atomic_store_n(&gFwLock, lck_mtx_alloc_init(gFwLockGroup, LCK_ATTR_NULL), __ATOMIC_RELEASE);
        }
        while (!__atomic_load_n(&gFwLock, __ATOMIC_ACQUIRE)) IODelay(100);
    }
    lck_mtx_lock(gFwLock);
}
static void fwUnlock(void) {lck_mtx_unlock(gFwLock);}

// Bytes of the partition: whole blocks straight into to, the ends of a range through the one-block cache.
static int partitionRead(void *context, uint64_t offset, void *to, size_t length)
{
    struct partition *p = context;
    uint8_t *out = to;
    while (length) {
        const uint64_t lba = offset / 4096;
        const size_t within = (size_t)(offset % 4096);
        if (!within && length >= 4096) {
            const uint32_t n = length / 4096 > 32 ? 32 : (uint32_t)(length / 4096);
            if (!native_ufs_part_read(p->lun, p->first + lba, n, out)) return EIO;
            out += (size_t)n * 4096; offset += (uint64_t)n * 4096; length -= (size_t)n * 4096;
            continue;
        }
        if (p->cached != lba) {
            p->cached = UINT64_MAX;
            if (!native_ufs_part_read(p->lun, p->first + lba, 1, p->block)) return EIO;
            p->cached = lba;
        }
        const size_t take = 4096 - within < length ? 4096 - within : length;
        memcpy(out, p->block + within, take);
        out += take; offset += take; length -= take;
    }
    return 0;
}

static int mountVolume(unsigned which)
{
    static const char *const names[2][2] = {{"modem_a", "modem_b"}, {"modemfirmware_a", "modemfirmware_b"}};
    if (gVolumes[which]) return 0;
    struct partition *p = IOMallocType(struct partition);
    FatVolume *v = IOMallocType(FatVolume);
    int rc = ENOENT;
    for (unsigned i = 0; i < 2 && rc; ++i) {
        uint64_t blocks = 0;
        // ENXIO while the UFS driver is not ready yet: the caller tries again later
        if (!native_ufs_part_find(names[which][i], &p->lun, &p->first, &blocks)) {rc = ENXIO; continue;}
        p->cached = UINT64_MAX;
        rc = fat_mount(v, blocks * 4096, partitionRead, p) ? EFTYPE : 0;
    }
    if (rc) {IOFreeType(p, struct partition); IOFreeType(v, FatVolume); return rc;}
    gParts[which] = p;
    gVolumes[which] = v;
    return 0;
}
static int mountPartition(void) {return mountVolume(0);}

struct modem_image {
    FatVolume *volume;
    QcomMdtSplit split;
    FatFile piece[QCOM_MDT_MAX_SEGMENTS];
};

static int pieceRead(void *context, unsigned piece, uint64_t offset, void *to, size_t length)
{
    struct modem_image *m = context;
    return fat_read(m->volume, &m->piece[piece], offset, to, length);
}

static int openModem(unsigned which, const char *name, struct modem_image **image, uint64_t *size)
{
    char path[48];
    int rc = mountVolume(which);
    if (rc) return rc;
    FatVolume *const vol = gVolumes[which];
    struct modem_image *m = IOMallocType(struct modem_image);
    m->volume = vol;
    snprintf(path, sizeof path, "image/%s.b00", name);
    FatFile headers;
    rc = fat_open(vol, path, &headers);
    if (rc == FatNotFound) rc = ENOENT;
    else if (rc) rc = EIO;
    else if (headers.size < 52 || headers.size > 64 * 1024) rc = EFTYPE;
    uint8_t *bytes = rc ? NULL : IOMallocData(headers.size);
    if (!rc && fat_read(vol, &headers, 0, bytes, headers.size)) rc = EIO;
    if (!rc && qcom_mdt_split(&m->split, bytes, headers.size)) rc = EFTYPE;
    if (bytes) IOFreeData(bytes, headers.size);
    for (unsigned i = 0; !rc && i < m->split.count; ++i) {
        if (!m->split.piece[i].size) continue;
        snprintf(path, sizeof path, "image/%s.b%02u", name, i);
        if (fat_open(vol, path, &m->piece[i])) rc = ENOENT;
        else if (m->piece[i].size < m->split.piece[i].size) rc = EFTYPE;
    }
    if (rc) {IOFreeType(m, struct modem_image); return rc;}
    *image = m;
    *size = m->split.size;
    return 0;
}

// ---- the partition source only (houji: no VFS fallback) ----
struct sheng_fw {
    struct modem_image *modem;
};

int sheng_fw_open(const char *path, void **handle, uint64_t *size)
{
    const unsigned which = !strncmp(path, "modemfw:", 8) ? 1 : 0;
    if (!which && strncmp(path, "modem:", 6)) return ENOENT;
    struct sheng_fw *fw = IOMallocType(struct sheng_fw);
    fwLock();
    const int error = openModem(which, path + (which ? 8 : 6), &fw->modem, size);
    fwUnlock();
    if (error) {IOFreeType(fw, struct sheng_fw); return error;}
    *handle = fw;
    return 0;
}

int sheng_fw_read(void *handle, uint64_t offset, void *to, size_t length)
{
    struct sheng_fw *fw = handle;
    fwLock();
    const int rc = qcom_mdt_split_read(&fw->modem->split, pieceRead, fw->modem, offset, to, length) ? EIO : 0;
    fwUnlock();
    return rc;
}

void sheng_fw_close(void *handle)
{
    struct sheng_fw *fw = handle;
    if (!fw) return;
    if (fw->modem) IOFreeType(fw->modem, struct modem_image);
    IOFreeType(fw, struct sheng_fw);
}

// A whole file of the firmware partition (such as "image/kiwi/amss20.bin"), read into memory from IOMallocData; the caller frees it with
// IOFreeData(*data, *size). Returns 0, ENXIO while the storage driver is not up yet, ENOENT, EFBIG (over max bytes), ENOMEM or EIO.
// which: 0 the firmware partition (modem_a), 1 the modem's (modemfirmware_a). An empty file comes back as one allocated byte, size 0.
int houji_fw_file(unsigned which, const char *path, uint8_t **data, size_t *size, size_t max)
{
    fwLock();
    int rc = which > 1 ? EINVAL : mountVolume(which);
    FatFile file;
    if (!rc) {
        const int found = fat_open(gVolumes[which], path, &file);
        rc = found == FatNotFound ? ENOENT : found ? EIO : 0;
    }
    uint8_t *bytes = NULL;
    const size_t alloc = rc ? 0 : (file.size ? file.size : 1);
    if (!rc && file.size > max) rc = EFBIG;
    if (!rc && !(bytes = IOMallocData(alloc))) rc = ENOMEM;
    if (!rc && file.size && fat_read(gVolumes[which], &file, 0, bytes, file.size)) {IOFreeData(bytes, alloc); rc = EIO;}
    fwUnlock();
    if (rc) return rc;
    *data = bytes;
    *size = file.size;
    return 0;
}
int houji_modem_file(const char *path, uint8_t **data, size_t *size, size_t max)
{
    size_t n = 0;
    const int rc = houji_fw_file(0, path, data, &n, max);
    if (!rc && !n) {IOFreeData(*data, 1); return EFBIG;}
    if (!rc) *size = n;
    return rc;
}
