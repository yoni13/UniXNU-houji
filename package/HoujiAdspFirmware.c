// SPDX-License-Identifier: BSD-2-Clause-Patent
// Local houji ADSP images. Firmware is staged on Data; no vendor bytes belong in this repository.
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <IOKit/IOLib.h>

struct houji_fw {
    vnode_t vnode;
    uint64_t size;
};

int houji_fw_open(const char *path, void **handle, uint64_t *size)
{
    if (!path || !handle || !size) return EINVAL;
    struct houji_fw *fw = IOMallocType(struct houji_fw);
    if (!fw) return ENOMEM;
    bzero(fw, sizeof *fw);
    vfs_context_t ctx = vfs_context_kernel();
    int error = vnode_open(path, FREAD, 0, 0, &fw->vnode, ctx);
    if (!error) {
        struct vnode_attr va;
        VATTR_INIT(&va);
        VATTR_WANTED(&va, va_data_size);
        error = vnode_getattr(fw->vnode, &va, ctx);
        if (!error && (vnode_vtype(fw->vnode) != VREG || !VATTR_IS_SUPPORTED(&va, va_data_size) || !va.va_data_size ||
                       va.va_data_size > 64u * 1024u * 1024u)) error = EFTYPE;
        if (!error) fw->size = va.va_data_size;
    }
    if (error) {
        if (fw->vnode) vnode_close(fw->vnode, FREAD, ctx);
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
    if (fw->vnode) vnode_close(fw->vnode, FREAD, vfs_context_kernel());
    IOFreeType(fw, struct houji_fw);
}
