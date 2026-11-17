// SPDX-License-Identifier: BSD-2-Clause-Patent
// ADSP images come either from Data through VFS, or from the phone's split images in modem_a/modem_b through read-only NativeUfs.
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <IOKit/IOLib.h>
#include "HoujiFatVolume.h"
#include "HoujiQcomMdt.h"
#include "HoujiUfsPartition.h"

#define HOUJI_FW_MAX (64u * 1024u * 1024u)

static HoujiFatVolume *gVolume;

static int mountModem(void)
{
    static const char *const names[] = {"modem_a", "modem_b"};
    if (gVolume) return 0;
    HoujiUfsPartition *partition = IOMallocType(HoujiUfsPartition);
    HoujiFatVolume *volume = IOMallocType(HoujiFatVolume);
    if (!partition || !volume) {
        if (partition) IOFreeType(partition, HoujiUfsPartition);
        if (volume) IOFreeType(volume, HoujiFatVolume);
        return ENOMEM;
    }
    int rc = houji_ufs_partition_open(partition, names, sizeof names / sizeof names[0]);
    if (!rc && houji_fat_mount(volume, partition->blocks * 4096, houji_ufs_partition_read, partition)) rc = EFTYPE;
    if (rc) {
        IOFreeType(partition, HoujiUfsPartition);
        IOFreeType(volume, HoujiFatVolume);
        return rc;
    }
    gVolume = volume;
    return 0;
}

struct modem_image {
    QcomMdtSplit split;
    HoujiFatFile piece[QCOM_MDT_MAX_SEGMENTS];
};

static int pieceRead(void *context, unsigned piece, uint64_t offset, void *to, size_t length)
{
    struct modem_image *image = context;
    return houji_fat_read(gVolume, &image->piece[piece], offset, to, length);
}

static int validName(const char *name)
{
    size_t length = 0;
    if (!name || !*name) return 0;
    for (; name[length]; ++length) {
        const char c = name[length];
        if (length >= 31 || !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                              (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return 1;
}

static int openModem(const char *name, struct modem_image **result, uint64_t *size)
{
    char path[48];
    if (!validName(name)) return EINVAL;
    int rc = mountModem();
    if (rc) return rc;
    struct modem_image *image = IOMallocType(struct modem_image);
    if (!image) return ENOMEM;
    bzero(image, sizeof *image);

    // Qualcomm split images normally carry the compact headers in NAME.mdt. The proven houji image also has usable headers in NAME.b00.
    HoujiFatFile headers;
    snprintf(path, sizeof path, "image/%s.mdt", name);
    rc = houji_fat_open(gVolume, path, &headers);
    if (rc == HoujiFatNotFound) {
        snprintf(path, sizeof path, "image/%s.b00", name);
        rc = houji_fat_open(gVolume, path, &headers);
    }
    if (rc == HoujiFatNotFound) rc = ENOENT;
    else if (rc) rc = EIO;
    else if (headers.size < 52 || headers.size > 64u * 1024u) rc = EFTYPE;
    uint8_t *bytes = rc ? NULL : IOMallocData(headers.size);
    if (!rc && !bytes) rc = ENOMEM;
    if (!rc && houji_fat_read(gVolume, &headers, 0, bytes, headers.size)) rc = EIO;
    if (!rc && qcom_mdt_split(&image->split, bytes, headers.size)) rc = EFTYPE;
    if (bytes) IOFreeData(bytes, headers.size);
    if (!rc && image->split.size > HOUJI_FW_MAX) rc = EFBIG;

    for (unsigned i = 0; !rc && i < image->split.count; ++i) {
        if (!image->split.piece[i].size) continue;
        snprintf(path, sizeof path, "image/%s.b%02u", name, i);
        int found = houji_fat_open(gVolume, path, &image->piece[i]);
        if (i == 0 && found == HoujiFatNotFound) {
            image->piece[i] = headers;
            found = HoujiFatOk;
        }
        if (found == HoujiFatNotFound) rc = ENOENT;
        else if (found) rc = EIO;
        if (!rc && image->piece[i].size < image->split.piece[i].size) rc = EFTYPE;
    }
    if (rc) {IOFreeType(image, struct modem_image); return rc;}
    *result = image;
    *size = image->split.size;
    return 0;
}

struct houji_fw {
    vnode_t vnode;
    uint64_t size;
    struct modem_image *modem;
};

int houji_fw_open(const char *path, void **handle, uint64_t *size)
{
    if (!path || !handle || !size) return EINVAL;
    struct houji_fw *fw = IOMallocType(struct houji_fw);
    if (!fw) return ENOMEM;
    bzero(fw, sizeof *fw);
    int error;
    if (!strncmp(path, "modem:", 6)) {
        error = openModem(path + 6, &fw->modem, &fw->size);
    } else {
        vfs_context_t ctx = vfs_context_kernel();
        error = vnode_open(path, FREAD, 0, 0, &fw->vnode, ctx);
        if (!error) {
            struct vnode_attr va;
            VATTR_INIT(&va);
            VATTR_WANTED(&va, va_data_size);
            error = vnode_getattr(fw->vnode, &va, ctx);
            if (!error && (vnode_vtype(fw->vnode) != VREG || !VATTR_IS_SUPPORTED(&va, va_data_size) || !va.va_data_size ||
                           va.va_data_size > HOUJI_FW_MAX)) error = EFTYPE;
            if (!error) fw->size = va.va_data_size;
        }
    }
    if (error) {
        if (fw->vnode) vnode_close(fw->vnode, FREAD, vfs_context_kernel());
        IOFreeType(fw, struct houji_fw);
        return error;
    }
    *handle = fw;
    *size = fw->size;
    return 0;
}

int houji_fw_read(void *handle, uint64_t offset, void *to, size_t length)
{
    struct houji_fw *fw = handle;
    if (!fw || !to || offset > fw->size || length > fw->size - offset) return EINVAL;
    if (fw->modem) return qcom_mdt_split_read(&fw->modem->split, pieceRead, fw->modem, offset, to, length) ? EIO : 0;
    vfs_context_t ctx = vfs_context_kernel();
    uint8_t *out = to;
    while (length) {
        const int want = length > (1u << 30) ? (1 << 30) : (int)length;
        int resid = want;
        const int error = vn_rdwr(UIO_READ, fw->vnode, (caddr_t)out, want, (off_t)offset, UIO_SYSSPACE, IO_NOAUTH,
                                  vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
        if (error || resid == want) return error ? error : EIO;
        out += want - resid;
        offset += (uint64_t)(want - resid);
        length -= (size_t)(want - resid);
    }
    return 0;
}

void houji_fw_close(void *handle)
{
    struct houji_fw *fw = handle;
    if (!fw) return;
    if (fw->modem) IOFreeType(fw->modem, struct modem_image);
    if (fw->vnode) vnode_close(fw->vnode, FREAD, vfs_context_kernel());
    IOFreeType(fw, struct houji_fw);
}
