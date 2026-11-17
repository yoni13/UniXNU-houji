/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* A read-only FAT16/FAT32 volume over a byte read callback: enough to find a file by its long or short (8.3) name in a directory path and read
 * any range of it. Used to take the device's own firmware files from its firmware partition in place. No allocation: the one sector of
 * the allocation table in use is cached in the volume. The volume's metadata is untrusted: every cluster number and chain is bounds
 * checked and every chain walk is bounded, so a damaged volume gives an error, never a loop or a read outside the partition. */
#ifndef HOUJI_FAT_VOLUME_H
#define HOUJI_FAT_VOLUME_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Reads length bytes at offset (from the partition start) into to; returns 0 on success. */
typedef int (*HoujiFatRead)(void *context, uint64_t offset, void *to, size_t length);

enum {
    HoujiFatOk = 0,
    HoujiFatReadError = -1,
    HoujiFatNotFat = -2,     /* no boot sector of a FAT16 or FAT32 volume */
    HoujiFatNotFound = -3,
    HoujiFatNotDirectory = -4,
    HoujiFatIsDirectory = -5,
    HoujiFatDamaged = -6,    /* a cluster chain leaves the volume, loops or is shorter than the file */
    HoujiFatRange = -7,      /* the read goes past the end of the file */
};

typedef struct {
    HoujiFatRead read;
    void *context;
    uint64_t size;                                    /* partition bytes */
    uint8_t fat32;
    uint32_t sector_bytes, cluster_bytes, clusters;   /* clusters: data clusters, numbered 2 .. clusters + 1 */
    uint64_t fat_offset, root_offset, data_offset;    /* bytes from the partition start; root_offset is FAT16 only */
    uint32_t root_entries, root_cluster;              /* root_entries: FAT16 fixed root; root_cluster: FAT32 */
    uint64_t cached_sector;                           /* UINT64_MAX: nothing cached */
    uint8_t sector[4096];
} HoujiFatVolume;

typedef struct {
    uint32_t first_cluster, size;
    uint8_t directory;
} HoujiFatFile;

int houji_fat_mount(HoujiFatVolume *v, uint64_t size, HoujiFatRead read, void *context);
/* path: names separated by '/', each matched in any case against the long name, else the 8.3 name ("image/adsp.b00"). */
int houji_fat_open(HoujiFatVolume *v, const char *path, HoujiFatFile *file);
int houji_fat_read(HoujiFatVolume *v, const HoujiFatFile *file, uint64_t offset, void *to, size_t length);

#ifdef __cplusplus
}
#endif
#endif
