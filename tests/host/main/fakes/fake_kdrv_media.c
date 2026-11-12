/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_kdrv_media.c
 * @brief The internal partition, as a real directory on the host.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fakes.h"
#include "kbsp.h"
#include "kdrv_fs.h"
#include "kdrv_sd.h"

static char s_fs_dir[256];
static bool s_fs_mounted;

/* What kdrv_fs_usage() says is free; fake_media_reset() puts it back. */
#define FAKE_FS_FREE_BYTES  (8ULL * 1024ULL * 1024ULL)
static uint64_t s_fs_free = FAKE_FS_FREE_BYTES;

/* The failure knob and the call count of kdrv_fs_mount(); fake_media_reset()
 * puts both back. */
static bool     s_fs_mount_fails;
static unsigned s_fs_mount_calls;

/**
 * @brief Empty @p dir recursively, leaving the directory itself.
 *
 * Recursive since directories exist: remove() refuses a non-empty one, so a
 * case that created /storage/logs used to leave it behind for every case
 * after it. Depth is bounded by what a case can create, which is a handful.
 *
 * @param dir Directory to empty; created if it does not exist.
 */
static void dir_reset(const char *dir)
{
    (void)mkdir(dir, 0777);

    DIR *d = opendir(dir);
    if ( NULL == d )
    {
        return;
    }
    struct dirent *ent;
    while ( (ent = readdir(d)) != NULL )
    {
        if ( (strcmp(ent->d_name, ".") == 0) || (strcmp(ent->d_name, "..") == 0) )
        {
            continue;
        }
        char path[512];
        (void)snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

        struct stat st;
        if ( (stat(path, &st) == 0) && S_ISDIR(st.st_mode) )
        {
            dir_reset(path);
            (void)rmdir(path);
        }
        else
        {
            (void)remove(path);
        }
    }
    (void)closedir(d);
}

void fake_media_reset(void)
{
    /* TMPDIR ends in a slash on macOS and is usually not set at all on Linux,
     * where the fallback below has none - so the separator is written here
     * rather than assumed. Without it the Linux path was "/tmpkode_fs", a
     * directory at the root of the filesystem that only CI's root could even
     * create. */
    const char *tmp = getenv("TMPDIR");
    const char *sep = "/";

    if ( (NULL == tmp) || ('\0' == tmp[0]) )
    {
        tmp = "/tmp";
    }
    if ( '/' == tmp[strlen(tmp) - 1U] )
    {
        sep = "";
    }
    (void)snprintf(s_fs_dir, sizeof(s_fs_dir), "%s%skode_fs", tmp, sep);
    dir_reset(s_fs_dir);

    /* The card goes first: unmount before the seam under it is reset, so the
     * driver's own idea of what is mounted cannot outlive the fake's. */
    kdrv_sd_unmount();
    fake_sdmmc_reset();

    /* The two next-mount knobs are statics in a driver that is real here, so
     * they outlive a case unless somebody puts them back. */
    (void)kdrv_sd_set(KDRV_SD_WIDTH_REQ, (int32_t)KDRV_SD_WIDTH_USE);
    (void)kdrv_sd_set(KDRV_SD_FREQ_REQ_KHZ, (int32_t)KBSP_SD_FREQ_KHZ);

    /* Both media come back MOUNTED, which models a board that probed them at
     * boot. It is not cosmetic: kdrv_storage latches "the card was tried" once
     * per boot and will not ask again, so a fake that reset to unmounted would
     * make the card permanently invisible after the first case that touched
     * it - and every later case would silently be testing the internal
     * partition instead of what it named. */
    s_fs_mounted = true;
    s_fs_free = FAKE_FS_FREE_BYTES;
    s_fs_mount_fails = false;
    s_fs_mount_calls = 0U;
    (void)kdrv_sd_mount();
}

void fake_media_set_sd_present(bool present)
{
    /* Putting a card in does not mount it: something has to ask. Taking one
     * out does unmount it, because a mount cannot outlive the card. */
    if ( !present )
    {
        kdrv_sd_unmount();
    }
    fake_sdmmc_set_present(present);
}

const char *fake_media_sd_dir(void)
{
    return fake_sdmmc_dir();
}

void fake_media_set_fs_free(uint64_t free_bytes)
{
    s_fs_free = free_bytes;
}

const char *fake_media_fs_dir(void)
{
    return s_fs_dir;
}

bool fake_media_file_exists(const char *dir, const char *name)
{
    char        path[512];
    struct stat st;

    (void)snprintf(path, sizeof(path), "%s/%s", dir, name);
    return (stat(path, &st) == 0);
}

void fake_media_put_fs_file(const char *name, const char *body)
{
    /* s_fs_dir is where the FS fake actually put its directory - reading it
     * back here is what keeps this the only place that knows the answer. A
     * case that built the path itself from TMPDIR would agree with this by
     * luck, not by construction. */
    char  path[512];
    FILE *f;

    (void)snprintf(path, sizeof(path), "%s/%s", s_fs_dir, name);
    f = fopen(path, "wb");
    if ( NULL == f )
    {
        return;
    }
    (void)fwrite(body, 1U, strlen(body), f);
    (void)fclose(f);
}

void fake_media_put_fs_dir(const char *name)
{
    char path[512];

    (void)snprintf(path, sizeof(path), "%s/%s", s_fs_dir, name);
    (void)mkdir(path, 0777);
}

/**
 * @brief Write @p body at @p rel under @p root, making every directory first.
 *
 * The host's own mkdir, which a fake is allowed: what stands in for a medium
 * here is a real directory, so a tree a case needs is a real tree.
 *
 * @param root One of the two fake mount directories.
 * @param rel  Path within it; every prefix is created, EEXIST ignored.
 * @param body Contents, written verbatim.
 * @param len  How many bytes of @p body.
 */
static void put_path(const char *root, const char *rel, const void *body,
                     size_t len)
{
    char path[512];
    FILE *f;

    if ( (size_t)snprintf(path, sizeof(path), "%s/%s", root, rel) >= sizeof(path) )
    {
        return;
    }
    /* From after the root's own slash, so none of the root's components is
     * re-created - it is already there and may not be ours to make. */
    for ( char *p = &path[strlen(root) + 1U]; '\0' != *p; ++p )
    {
        if ( '/' != *p )
        {
            continue;
        }
        *p = '\0';
        (void)mkdir(path, 0777);
        *p = '/';
    }

    f = fopen(path, "wb");
    if ( NULL == f )
    {
        return;
    }
    (void)fwrite(body, 1U, len, f);
    (void)fclose(f);
}

void fake_media_put_fs_path(const char *rel, const void *body, size_t len)
{
    put_path(s_fs_dir, rel, body, len);
}

void fake_media_put_sd_path(const char *rel, const void *body, size_t len)
{
    put_path(fake_sdmmc_dir(), rel, body, len);
}

/* ---- kdrv_fs ------------------------------------------------------------- */

void fake_fs_mount_set_fails(bool fails)
{
    s_fs_mount_fails = fails;
}

unsigned fake_fs_mount_calls(void)
{
    return s_fs_mount_calls;
}

const char *kdrv_fs_mount(void)
{
    s_fs_mount_calls++;
    if ( s_fs_mount_fails )
    {
        return NULL;
    }
    s_fs_mounted = true;
    return s_fs_dir;
}

void kdrv_fs_unmount(void)
{
    s_fs_mounted = false;
}

bool kdrv_fs_mounted(void)
{
    return s_fs_mounted;
}

const char *kdrv_fs_mount_point(void)
{
    return s_fs_mounted ? s_fs_dir : NULL;
}

kode_status_t kdrv_fs_usage(uint64_t *total, uint64_t *free_bytes)
{
    if ( !s_fs_mounted )
    {
        return KODE_DENIED;
    }
    if ( NULL != total )      { *total = 12ULL * 1024ULL * 1024ULL; }
    if ( NULL != free_bytes ) { *free_bytes = s_fs_free; }
    return KODE_OK;
}
