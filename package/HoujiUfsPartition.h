/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Read-only byte access to a GPT partition exposed by NativeUfs. */
#ifndef HOUJI_UFS_PARTITION_H
#define HOUJI_UFS_PARTITION_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t lun;
    uint64_t first_lba, blocks;
    uint64_t cached_lba;
    uint8_t block[4096];
} HoujiUfsPartition;

/* Tries each GPT name in order. Returns 0, ENXIO when NativeUfs is unavailable/not ready, or ENOENT. */
int houji_ufs_partition_open(HoujiUfsPartition *partition, const char *const *names, size_t count);
/* Reads partition-relative bytes. Every read is bounded to the partition recorded by open. */
int houji_ufs_partition_read(void *context, uint64_t offset, void *to, size_t length);

#ifdef __cplusplus
}
#endif
#endif
