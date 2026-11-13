/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_fs.c
 * @brief The internal FAT partition: the user's volume.
 */
#include "kdrv_fs.h"

#include "khal_sync.h"

#include <string.h>

#include "diskio_wl.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "wear_levelling.h"

#include "kode_log.h"

#define TAG                "kdrv.fs"
#define KDRV_FS_LABEL    "storage"
#define KDRV_FS_MOUNT    "/storage"

static wl_handle_t       s_wl = WL_INVALID_HANDLE;
static khal_mutex_t      s_lock;

/**
 * @brief Mount the volume once, formatting it first only when @p format.
 *
 * A failed mount leaves the wear-levelling layer mounted under the handle
 * it wrote to s_wl (IDF releases the FAT side only), and there are eight such
 * handles in all, so a failure hands it back here.
 *
 * @param format true to format a partition that holds no volume that mounts.
 * @return What esp_vfs_fat_spiflash_mount_rw_wl() returned; ESP_FAIL is a FAT
 *         that would not mount.
 */
static esp_err_t mount_once(bool format)
{
    const esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = format,
        .max_files              = 4,
        .allocation_unit_size   = CONFIG_WL_SECTOR_SIZE,
    };

    esp_err_t const err = esp_vfs_fat_spiflash_mount_rw_wl(
        KDRV_FS_MOUNT, KDRV_FS_LABEL, &cfg, &s_wl);
    if ( (ESP_OK != err) && (WL_INVALID_HANDLE != s_wl) )
    {
        (void)wl_unmount(s_wl);
        s_wl = WL_INVALID_HANDLE;
    }
    return err;
}

const char *kdrv_fs_mount_point(void)
{
    return KDRV_FS_MOUNT;
}

bool kdrv_fs_mounted(void)
{
    return (WL_INVALID_HANDLE != s_wl);
}

const char *kdrv_fs_mount(void)
{
    if ( NULL == s_lock )
    {
        s_lock = khal_mutex_create();
        if ( NULL == s_lock )
        {
            return NULL;
        }
    }
    khal_mutex_lock(s_lock);

    if ( WL_INVALID_HANDLE != s_wl )
    {
        khal_mutex_unlock(s_lock);
        return KDRV_FS_MOUNT;
    }

    /* A volume that mounts is mounted as it is. One that does not - a
     * factory-fresh partition, or a damaged volume - is formatted, and the
     * log says so before it happens: the volume is shared by kodeOS, every
     * app and the user, so a format is never silent. */
    esp_err_t err = mount_once(false);
    if ( ESP_FAIL == err )
    {
        KLOG_W(TAG, "'%s' will not mount: a new board, or a damaged volume. "
                    "Mounting it again with a format; anything on it is lost",
               KDRV_FS_LABEL);
        err = mount_once(true);
    }
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "mount of '%s' failed (%s)",
               KDRV_FS_LABEL, esp_err_to_name(err));
        khal_mutex_unlock(s_lock);
        return NULL;
    }

    uint64_t total = 0U;
    uint64_t freeb = 0U;
    if ( kdrv_fs_usage(&total, &freeb) == KODE_OK )
    {
        KLOG_I(TAG, "storage mounted at %s (%lluKB free of %lluKB)",
               KDRV_FS_MOUNT, freeb / 1024ULL, total / 1024ULL);
    }
    else
    {
        KLOG_I(TAG, "storage mounted at %s", KDRV_FS_MOUNT);
    }

    khal_mutex_unlock(s_lock);
    return KDRV_FS_MOUNT;
}

void kdrv_fs_unmount(void)
{
    if ( WL_INVALID_HANDLE == s_wl )
    {
        return;
    }
    (void)esp_vfs_fat_spiflash_unmount_rw_wl(KDRV_FS_MOUNT, s_wl);
    s_wl = WL_INVALID_HANDLE;
    KLOG_I(TAG, "storage unmounted");
}

uint32_t kdrv_fs_cluster_bytes(void)
{
    uint32_t bytes = 0U;

    if ( WL_INVALID_HANDLE != s_wl )
    {
        char  drive[3];
        FATFS *fs = NULL;
        DWORD free_clusters = 0U;

        drive[0] = (char)('0' + ff_diskio_get_pdrv_wl(s_wl));
        drive[1] = ':';
        drive[2] = '\0';
        /* The FATFS esp_vfs_fat_info() reads its sizes from comes with the
         * free count. */
        if ( (f_getfree(drive, &free_clusters, &fs) == FR_OK) && (NULL != fs) )
        {
            WORD sector = FF_MIN_SS;
#if FF_MAX_SS != FF_MIN_SS
            sector = fs->ssize;
#endif
            bytes = (uint32_t)fs->csize * (uint32_t)sector;
        }
    }
    return bytes;
}

kode_status_t kdrv_fs_usage(uint64_t *total, uint64_t *free_bytes)
{
    if ( WL_INVALID_HANDLE == s_wl )
    {
        return KODE_DENIED;
    }
    uint64_t t = 0U;
    uint64_t f = 0U;
    if ( esp_vfs_fat_info(KDRV_FS_MOUNT, &t, &f) != ESP_OK )
    {
        return KODE_ERROR;
    }
    if ( NULL != total )
    {
        *total = t;
    }
    if ( NULL != free_bytes )
    {
        *free_bytes = f;
    }
    return KODE_OK;
}
