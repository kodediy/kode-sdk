/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_fs.h
 * @brief The internal FAT partition. Reached through kdrv_storage.
 */
#ifndef KDRV_FS_H_
#define KDRV_FS_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Mount it, formatting if blank. @return The mount point, or NULL. */
const char *kdrv_fs_mount(void);
/** @brief Unmount it. */
void        kdrv_fs_unmount(void);
/** @brief Whether it is currently mounted. */
bool        kdrv_fs_mounted(void);
/** @brief The mount point string, mounted or not. */
const char *kdrv_fs_mount_point(void);
/** @brief Total and free bytes. */
kode_status_t kdrv_fs_usage(uint64_t *total, uint64_t *free_bytes);
/**
 * @brief The volume's allocation unit, in bytes: what a file of one byte takes.
 * @return The cluster size, or 0 when the volume is not mounted or will not say.
 */
uint32_t    kdrv_fs_cluster_bytes(void);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_FS_H_ */
