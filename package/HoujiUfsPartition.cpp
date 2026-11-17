// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <sys/errno.h>
#include <sys/systm.h>
#include "HoujiUfsPartition.h"

extern "C" int native_ufs_part_find(const char *, uint32_t *, uint64_t *, uint64_t *) __attribute__((weak_import));
extern "C" int native_ufs_part_read(uint32_t, uint64_t, uint32_t, void *) __attribute__((weak_import));

extern "C" int houji_ufs_partition_open(HoujiUfsPartition *p, const char *const *names, size_t count)
{
    if (!p || !names || !count) return EINVAL;
    if (!native_ufs_part_find || !native_ufs_part_read) return ENXIO;
    bzero(p, sizeof *p);
    for (size_t i = 0; i < count; ++i) {
        uint64_t blocks = 0;
        if (!names[i]) return EINVAL;
        if (!native_ufs_part_find(names[i], &p->lun, &p->first_lba, &blocks)) continue;
        if (!blocks || p->first_lba > UINT32_MAX || blocks - 1 > UINT32_MAX - p->first_lba) return EFTYPE;
        p->blocks = blocks;
        p->cached_lba = UINT64_MAX;
        return 0;
    }
    return ENOENT;
}

extern "C" int houji_ufs_partition_read(void *context, uint64_t offset, void *to, size_t length)
{
    HoujiUfsPartition *p = static_cast<HoujiUfsPartition *>(context);
    uint8_t *out = static_cast<uint8_t *>(to);
    const uint64_t bytes = p && p->blocks <= UINT64_MAX / 4096 ? p->blocks * 4096 : 0;
    if (!p || !to || offset > bytes || length > bytes - offset || !native_ufs_part_read) return EINVAL;
    while (length) {
        const uint64_t rel = offset / 4096;
        const size_t within = static_cast<size_t>(offset % 4096);
        if (!within && length >= 4096) {
            uint32_t blocks = static_cast<uint32_t>(length / 4096 > 32 ? 32 : length / 4096);
            if (!native_ufs_part_read(p->lun, p->first_lba + rel, blocks, out)) return EIO;
            const size_t taken = static_cast<size_t>(blocks) * 4096;
            out += taken; offset += taken; length -= taken;
        } else {
            if (p->cached_lba != rel) {
                p->cached_lba = UINT64_MAX;
                if (!native_ufs_part_read(p->lun, p->first_lba + rel, 1, p->block)) return EIO;
                p->cached_lba = rel;
            }
            const size_t taken = 4096 - within < length ? 4096 - within : length;
            memcpy(out, p->block + within, taken);
            out += taken; offset += taken; length -= taken;
        }
    }
    return 0;
}
