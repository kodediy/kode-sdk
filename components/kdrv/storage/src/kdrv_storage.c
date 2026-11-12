/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_storage.c
 * @brief The media layer: mounting, listing and reading app images.
 */
#include "kdrv_storage.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kdrv_sd.h"
#include "kdrv_fs.h"
#include "kode_log.h"

#define TAG "kdrv.storage"

/* One reader and one writer, and no more of either. There was a single
 * handle for both until copying existed, and copying is exactly the pair: a
 * reader and a writer at the same instant. AO_Storage is the only caller and
 * it is one thread, so what these two are is a copy loop rather than a race -
 * nothing here is any more concurrent than it was with one. */
static FILE *s_read;    /**< kdrv_storage_open_path()   */
static FILE *s_write;   /**< kdrv_storage_create_path() */

/* Whether the two above are one copy rather than two unrelated files. See
 * the Copying section at the end of this file; it lives up here because
 * kdrv_storage_close() clears it, and closing both handles is the end of any
 * copy however it was reached. */
static bool s_copying;

/* Whether the microSD has been tried since boot. See kdrv_storage_probe(). */
static bool s_sd_tried;

const char *kdrv_storage_mount(kdrv_medium_t m)
{
    if ( KDRV_MEDIUM_SD == m )
    {
        s_sd_tried = true;
        return kdrv_sd_mount();
    }
    return kdrv_fs_mount();
}

const char *kdrv_storage_probe(kdrv_medium_t m)
{
    if ( KDRV_MEDIUM_SD != m )
    {
        return kdrv_fs_mount();       /* always there, costs nothing to retry */
    }
    if ( kdrv_sd_mounted() )
    {
        return kdrv_sd_mount_point();
    }
    if ( s_sd_tried )
    {
        return NULL;                  /* it already said no; do not ask again */
    }
    s_sd_tried = true;
    return kdrv_sd_mount();
}

void kdrv_storage_unmount(kdrv_medium_t m)
{
    if ( KDRV_MEDIUM_SD == m )
    {
        kdrv_sd_unmount();
    }
    else
    {
        kdrv_fs_unmount();
    }
}

bool kdrv_storage_mounted(kdrv_medium_t m)
{
    return (KDRV_MEDIUM_SD == m) ? kdrv_sd_mounted() : kdrv_fs_mounted();
}

const char *kdrv_storage_mount_point(kdrv_medium_t m)
{
    return (KDRV_MEDIUM_SD == m) ? kdrv_sd_mount_point() : kdrv_fs_mount_point();
}

kode_status_t kdrv_storage_usage(kdrv_medium_t m, uint64_t *total,
                                 uint64_t *free_bytes)
{
    /* Both media are FAT, so both can answer: the SD used to be KODE_DENIED
     * here ("reports its geometry through _dump") until kdrv_sd_usage()
     * existed. */
    return (KDRV_MEDIUM_SD == m) ? kdrv_sd_usage(total, free_bytes)
                                 : kdrv_fs_usage(total, free_bytes);
}

void kdrv_storage_dump(kdrv_medium_t m)
{
    if ( KDRV_MEDIUM_SD == m )
    {
        kdrv_sd_dump();
        return;
    }
    uint64_t total = 0U;
    uint64_t freeb = 0U;
    const char *mount = kdrv_fs_mount_point();
    if ( kdrv_fs_usage(&total, &freeb) == KODE_OK )
    {
        KLOG_N(TAG, "%s: %lluKB free of %lluKB",
               mount, freeb / 1024ULL, total / 1024ULL);
    }
    else
    {
        KLOG_N(TAG, "%s: not mounted", mount);
    }
}

/* A real path is a mount point plus a virtual tail, and 320 is how long one
 * may get: room for a host temp directory that outruns "/storage" by a wide
 * margin, which is what the off-board tests need, and on the board the mount
 * point is eight characters and nothing comes near it. Every buffer in this
 * file that holds a real path is sized from here - s_write_path included - so
 * the number has one opinion behind it rather than four that have to be kept
 * in step. */
#define KDRV_REAL_PATH_MAX  320U

/* The longest virtual path this component builds. Twinned in kqp_config.h as
 * KODE_PATH_MAX, because kqp sits below kdrv and its events carry a path -
 * the same twinning KAPP_NAME_MAX and the KDRV_NET_* bounds already live
 * under. Change one, change the other.
 *
 * Nothing that reaches the flat calls comes close: a file name there is
 * bounded by KODE_APP_FILE_MAX at 64, and the longest prefix is eight
 * characters. */
#define KDRV_VPATH_MAX  96U

/* The one prefix table. kdrv_storage_medium_of() walks it to answer "which
 * volume", and real_path() below reads the same string back out of it to
 * know where the tail after the prefix begins - so there is exactly one
 * place that spells "/sdcard" and "/storage", not two that have to be kept
 * in step. */
static const char *const s_volume_prefix[KDRV_MEDIUM_COUNT] = {
    "/sdcard",
    "/storage",
};

/* Exported (see the contract in kdrv_storage.h) because real_path() below is
 * no longer its only caller: AO_Storage needs the same answer to fill in
 * StorageEntryEvt.from_sd for a path-based listing, and a later rename needs
 * it to refuse a path that crosses volumes. One table, read from the one
 * place that owns it, rather than a second component matching "/sdcard" for
 * itself. */
kdrv_medium_t kdrv_storage_medium_of(const char *vpath)
{
    if ( NULL == vpath )
    {
        return KDRV_MEDIUM_COUNT;
    }

    for ( int i = 0; (int)KDRV_MEDIUM_COUNT > i; ++i )
    {
        kdrv_medium_t const m   = (kdrv_medium_t)i;
        size_t const        len = strlen(s_volume_prefix[m]);

        if ( strncmp(vpath, s_volume_prefix[m], len) != 0 )
        {
            continue;
        }
        /* "/sdcardium" must not pass as the card: the prefix has to end the
         * component, not merely start it. */
        if ( ('\0' == vpath[len]) || ('/' == vpath[len]) )
        {
            return m;
        }
    }
    return KDRV_MEDIUM_COUNT;
}

/**
 * @brief Is @p vpath the absolute, normalised shape this layer promises?
 *
 * The header states the contract - absolute, no ".", no "..", no empty
 * component, no trailing separator except the root - and this is where it is
 * enforced, in the one function every path call already goes through. It was
 * a contract nothing checked: the console's own resolver normalises before it
 * asks, so the console could never break it, but the facade hands an app's
 * string straight down, and "/storage/../.." would have been walked out
 * of the tree by libc without anything in kodeOS objecting. Checking it here
 * rather than at each door means a caller that arrives later is covered by
 * construction instead of by remembering.
 *
 * @param vpath The virtual path. NULL is not accepted; callers check first.
 * @return true when the path may be mapped.
 */
static bool vpath_is_canonical(const char *vpath)
{
    if ( '/' != vpath[0] )
    {
        return false;
    }
    if ( '\0' == vpath[1] )
    {
        return true;
    }

    size_t const len = strlen(vpath);
    if ( '/' == vpath[len - 1U] )
    {
        return false;
    }

    size_t start = 1U;
    for ( size_t i = 1U; i <= len; ++i )
    {
        if ( ('/' != vpath[i]) && ('\0' != vpath[i]) )
        {
            continue;
        }
        size_t const n = i - start;
        if ( 0U == n )
        {
            return false;
        }
        if ( (1U == n) && ('.' == vpath[start]) )
        {
            return false;
        }
        if ( (2U == n) && ('.' == vpath[start]) && ('.' == vpath[start + 1U]) )
        {
            return false;
        }
        start = i + 1U;
    }
    return true;
}

/**
 * @brief Map a virtual path onto the volume it names.
 *
 * "/storage/a/b" becomes "<the FS mount point>/a/b". The mount points are
 * asked for rather than spelled out, and that is what makes every path
 * function testable off the board: the host suite points them at directories
 * under $TMPDIR, and nothing here knows the difference.
 *
 * The volume is probed, not mounted: an operator who wants the card mounted
 * says so with `sd_mount`, and a stat that mounted as a side effect would
 * cost an LDO setup and two error lines on a board whose C5 does not power
 * the card.
 *
 * @param vpath Absolute virtual path, checked against the header's contract
 *              by vpath_is_canonical() before anything is built from it. "/"
 *              itself has no real counterpart and is refused here; its
 *              callers handle the root before asking.
 * @param out   The real path.
 * @param cap   Size of @p out.
 * @return KODE_OK, KODE_DENIED for a malformed path or one naming no volume,
 *         KODE_ERROR when the volume will not mount.
 */
static kode_status_t real_path(const char *vpath, char *out, size_t cap)
{
    if ( (NULL == vpath) || (NULL == out) || !vpath_is_canonical(vpath) )
    {
        return KODE_DENIED;
    }

    kdrv_medium_t const medium = kdrv_storage_medium_of(vpath);
    if ( KDRV_MEDIUM_COUNT == medium )
    {
        return KODE_DENIED;
    }

    const char *const rest  = &vpath[strlen(s_volume_prefix[medium])];
    const char *const mount = kdrv_storage_probe(medium);
    if ( NULL == mount )
    {
        return KODE_ERROR;
    }
    if ( (size_t)snprintf(out, cap, "%s%s", mount, rest) >= cap )
    {
        return KODE_DENIED;
    }
    return KODE_OK;
}

/**
 * @brief Build the virtual path of @p name on @p m.
 *
 * The inverse of kdrv_storage_medium_of(), reading the same table, and that
 * is the whole reason it exists rather than an snprintf at each call site:
 * the flat calls take a bare name and have to turn it into a path, and if
 * each of them spelled "/sdcard" for itself there would be four copies of a
 * string this file already owns in one place.
 *
 * @param m    The volume. KDRV_MEDIUM_COUNT names none and is refused.
 * @param name The bare file name, as the flat calls take it.
 * @param out  The virtual path. Untouched on false.
 * @param cap  Size of @p out.
 * @return false when @p m names no volume, an argument is NULL, or the result
 *         would not fit - never a truncated path, which would name a
 *         different file.
 */
static bool vpath_of(kdrv_medium_t m, const char *name, char *out, size_t cap)
{
    if ( (KDRV_MEDIUM_COUNT <= m) || (NULL == name) || (NULL == out) )
    {
        return false;
    }

    int const n = snprintf(out, cap, "%s/%s", s_volume_prefix[m], name);
    if ( (0 > n) || ((size_t)n >= cap) )
    {
        return false;
    }
    return true;
}

kode_status_t kdrv_storage_stat(const char *path, kdrv_stat_t *out)
{
    if ( (NULL == path) || (NULL == out) )
    {
        return KODE_DENIED;
    }

    out->exists = false;
    out->is_dir = false;
    out->size   = 0U;

    if ( strcmp(path, "/") == 0 )
    {
        out->exists = true;
        out->is_dir = true;
        return KODE_OK;
    }

    char real[KDRV_REAL_PATH_MAX];
    kode_status_t const rc = real_path(path, real, sizeof(real));
    if ( KODE_OK != rc )
    {
        return rc;
    }

    struct stat st;
    if ( stat(real, &st) != 0 )
    {
        return KODE_OK;             /* not there IS the answer */
    }
    out->exists = true;
    out->is_dir = S_ISDIR(st.st_mode);
    if ( !out->is_dir )
    {
        out->size = (uint32_t)st.st_size;
    }
    return KODE_OK;
}

/**
 * @brief Is this directory entry worth showing to a launcher scan?
 *
 * Rejects the "._name" AppleDouble stubs macOS scatters over a FAT card
 * whenever it copies a file: they carry the right extension, are a few KB of
 * metadata, and would otherwise appear as apps that cannot possibly run.
 *
 * The flat walk below is its only caller, and that is the whole scope of it.
 * kdrv_storage_list_path() deliberately does not filter: `ls` is a general
 * listing, and `stat`, `cat` and `rm` all see those files, so hiding them
 * from the one verb whose job is to say what is there would be a lie by
 * omission about files an operator can go on to act upon.
 */
static bool entry_is_interesting(const char *name)
{
    return (strncmp(name, "._", 2) != 0);
}

/*
 * Deliberately NOT a wrapper over kdrv_storage_list_path() below, which is
 * the one flat call that does not reduce to the path core. The two walks
 * answer different questions and both answers are somebody's interface: this
 * one hands "." and ".." straight to its caller and says nothing about which
 * entries are directories, and `sd_ls` prints whatever it is handed - see
 * test_kdrv_storage_list.c:130, which pins that as a decision rather than a
 * surprise. The launcher's scan is handed them too and never shows them,
 * because it keeps only what carries an app extension. The path walk drops
 * the two, reports is_dir and hides nothing, which is what `ls` needs.
 * Folding them together would quietly change both, so the duplicated readdir
 * loop is the cheaper of the two costs.
 *
 * Its argument is the other half of why it stays as it is. A medium is not a
 * path, and this is the last of the flat calls whose vocabulary is a volume.
 */
kode_status_t kdrv_storage_list(kdrv_medium_t m, kdrv_storage_entry_cb cb, void *ctx)
{
    const char *mount = kdrv_storage_probe(m);
    if ( (NULL == mount) || (NULL == cb) )
    {
        return KODE_DENIED;
    }

    DIR *dir = opendir(mount);
    if ( NULL == dir )
    {
        KLOG_W(TAG, "cannot read %s", mount);
        return KODE_ERROR;
    }

    const struct dirent *ent;
    while ( (ent = readdir(dir)) != NULL )
    {
        if ( !entry_is_interesting(ent->d_name) )
        {
            continue;
        }

        /* The return value is read for the same reason it is read in
         * kdrv_storage_list_path() below, and this loop has to agree with
         * that one rather than pick its own answer: a truncated path names a
         * different file on disk, and stat()ing it would hand this entry's
         * caller that other file's size. GCC can prove the truncation is
         * possible here (mount comes from kdrv_storage_probe(), which it
         * cannot bound) where it could not on the host build, which is the
         * only reason this loop went uncaught as long as it did - the defect
         * was there before the diagnostic was. An entry whose real path will
         * not fit is handed on exactly like one that will not stat: the name
         * is genuinely there, and nothing more about it can honestly be said. */
        char      path[KDRV_REAL_PATH_MAX];
        int const n = snprintf(path, sizeof(path), "%s/%s", mount, ent->d_name);

        struct stat st;
        uint32_t    size = 0U;
        if ( (0 < n) && ((size_t)n < sizeof(path)) && (stat(path, &st) == 0) )
        {
            size = (uint32_t)st.st_size;
        }
        if ( !cb(ent->d_name, size, ctx) )
        {
            break;
        }
    }
    (void)closedir(dir);
    return KODE_OK;
}

kode_status_t kdrv_storage_list_path(const char *path,
                                     kdrv_path_entry_cb cb, void *ctx)
{
    if ( (NULL == path) || (NULL == cb) )
    {
        return KODE_DENIED;
    }

    /* The root is this layer's own, not the filesystem's: it is where the two
     * volumes become one tree. Answered before real_path() because it has no
     * real counterpart to map to, and both volumes are named whether or not
     * either is mounted - see real_path()'s own note on why a stat does not
     * mount as a side effect. An unmounted card missing from this listing
     * would answer "is my file there?" with a lie by omission. */
    if ( strcmp(path, "/") == 0 )
    {
        if ( cb("sdcard", true, 0U, ctx) )
        {
            (void)cb("storage", true, 0U, ctx);
        }
        return KODE_OK;
    }

    char real[KDRV_REAL_PATH_MAX];
    kode_status_t const rc = real_path(path, real, sizeof(real));
    if ( KODE_OK != rc )
    {
        return rc;
    }

    DIR *dir = opendir(real);
    if ( NULL == dir )
    {
        return KODE_ERROR;
    }

    const struct dirent *ent;
    bool go = true;
    while ( go && ((ent = readdir(dir)) != NULL) )
    {
        /* "." and ".." are the only two entries dropped here, and the
         * AppleDouble filter the flat walk applies deliberately is NOT: a
         * general listing that hid a file `stat`, `cat` and `rm` will all
         * happily act on would be answering "what is here" with an omission.
         * Hiding those stubs is the launcher scan's concern, so it stays
         * where the launcher scan reads. */
        if ( (strcmp(ent->d_name, ".") == 0) || (strcmp(ent->d_name, "..") == 0) )
        {
            continue;
        }

        /* The length is read rather than discarded, and not merely to keep
         * the compiler quiet about it: a truncated path names a different
         * file, and stat()ing that would report its type and size for this
         * entry. An entry whose path will not fit is handed on exactly like
         * one that will not stat - the name is genuinely there, and nothing
         * more about it can honestly be said. A directory deep enough to
         * reach 320 characters is not something FAT and this board will
         * produce, so this is a guard rather than a case. */
        char      full[KDRV_REAL_PATH_MAX];
        int const n = snprintf(full, sizeof(full), "%s/%s", real, ent->d_name);

        struct stat st;
        bool     is_dir = false;
        uint32_t size   = 0U;
        if ( (0 < n) && ((size_t)n < sizeof(full)) && (stat(full, &st) == 0) )
        {
            is_dir = S_ISDIR(st.st_mode);
            if ( !is_dir )
            {
                size = (uint32_t)st.st_size;
            }
        }
        go = cb(ent->d_name, is_dir, size, ctx);
    }
    (void)closedir(dir);
    return KODE_OK;
}

/* ---- Changing the tree ---------------------------------------------------
 * mkdir, rm, rmdir and rename/mv. Every one of them but kdrv_storage_unlink()
 * is exactly real_path() then one libc call - the mapping is the only thing
 * this layer adds, and whatever mkdir()/rmdir()/rename() say about the rest
 * (a taken name, a missing parent, a non-empty directory) rides straight
 * through as KODE_ERROR without this file inventing a second vocabulary for
 * it. kdrv_storage_mkdir() is the one exception now, for the reason its own
 * comment gives; kdrv_storage_unlink() is the other, and its own comment
 * says why.
 */

/**
 * @brief Turn the errno a failed create left behind into a status.
 *
 * Shared by kdrv_storage_mkdir() and kdrv_storage_create_path() because the
 * two ask libc the same question - "make this name" - and get the same set of
 * answers back. One table beats two splits that have to be kept in step.
 *
 * The distinction the four named errnos buy is the whole point: only
 * KODE_IO_ERROR asserts that the medium refused, and a caller that turns a
 * status into a word for an operator must not send them to check a directory
 * that was never the problem, nor blame a card for a name that is simply
 * taken. Everything left over is genuinely the device answering no - ESP-IDF's
 * FATFS VFS maps a disk error to EIO, and it never invents ENOTDIR for a path
 * that is merely missing, which is FR_NO_PATH and arrives as ENOENT.
 *
 * @param err The errno value, read immediately after the failed call.
 * @return KODE_ERROR for a missing parent, KODE_EXISTS for a taken name,
 *         KODE_NOT_DIR when a component of the path is a file, KODE_IS_DIR
 *         when the path itself is a directory, KODE_IO_ERROR otherwise.
 */
static kode_status_t status_of_create_errno(int err)
{
    if ( ENOENT == err )
    {
        return KODE_ERROR;
    }
    if ( EEXIST == err )
    {
        return KODE_EXISTS;
    }
    if ( ENOTDIR == err )
    {
        return KODE_NOT_DIR;
    }
    if ( EISDIR == err )
    {
        return KODE_IS_DIR;
    }
    return KODE_IO_ERROR;
}

kode_status_t kdrv_storage_mkdir(const char *path)
{
    char real[KDRV_REAL_PATH_MAX];
    kode_status_t const rc = real_path(path, real, sizeof(real));
    if ( KODE_OK != rc )
    {
        return rc;
    }
    if ( mkdir(real, 0777) != 0 )
    {
        /* ENOENT is the one errno a genuinely missing parent leaves behind -
         * mkdir() walked the path, found nothing at the second-to-last
         * component, and stopped. EEXIST is a taken name, ENOTDIR a path
         * whose parent is a file; each gets its own status because each is a
         * different thing to tell an operator, and none of the three is the
         * medium refusing anything. What is left - EIO above all - is the
         * counterfeit-card story: the volume mounted, the parent is right
         * there, and the write that records the new entry is what failed. */
        return status_of_create_errno(errno);
    }
    return KODE_OK;
}

kode_status_t kdrv_storage_unlink(const char *path)
{
    /* Stats before it deletes, unlike the other three here - remove() on a
     * directory succeeds on some libcs (glibc quietly hands an empty one to
     * rmdir()), and that is not a difference this function is allowed to let
     * through: `rm` takes files only, and the operator must see that refusal
     * every time, not just on the libcs where remove() happens to agree. */
    kdrv_stat_t st;
    kode_status_t rc = kdrv_storage_stat(path, &st);
    if ( KODE_OK != rc )
    {
        return rc;
    }
    if ( !st.exists )
    {
        return KODE_ERROR;
    }
    if ( st.is_dir )
    {
        return KODE_DENIED;
    }

    char real[KDRV_REAL_PATH_MAX];
    rc = real_path(path, real, sizeof(real));
    if ( KODE_OK != rc )
    {
        return rc;
    }
    if ( remove(real) != 0 )
    {
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t kdrv_storage_rmdir(const char *path)
{
    char real[KDRV_REAL_PATH_MAX];
    kode_status_t const rc = real_path(path, real, sizeof(real));
    if ( KODE_OK != rc )
    {
        return rc;
    }
    /* No stat here, and none is needed: rmdir() already refuses a path that
     * is not a directory (ENOTDIR) and one that still holds something
     * (ENOTEMPTY), both folded into KODE_ERROR the same way every other
     * libc refusal in this file is. */
    if ( rmdir(real) != 0 )
    {
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t kdrv_storage_rename(const char *from, const char *to)
{
    if ( (NULL == from) || (NULL == to) )
    {
        return KODE_DENIED;
    }

    /* The volumes are compared before either path is opened, let alone
     * touched: FAT cannot rename across a mount, and finding that out from
     * rename()'s own errno would mean this function sometimes reads a
     * directory entry on one volume before refusing to write it on the
     * other - work a refusal must never have done. */
    kdrv_medium_t const fm = kdrv_storage_medium_of(from);
    kdrv_medium_t const tm = kdrv_storage_medium_of(to);
    if ( (KDRV_MEDIUM_COUNT == fm) || (KDRV_MEDIUM_COUNT == tm) )
    {
        return KODE_DENIED;
    }
    if ( fm != tm )
    {
        return KODE_DENIED;
    }

    char real_from[KDRV_REAL_PATH_MAX];
    kode_status_t rc = real_path(from, real_from, sizeof(real_from));
    if ( KODE_OK != rc )
    {
        return rc;
    }
    char real_to[KDRV_REAL_PATH_MAX];
    rc = real_path(to, real_to, sizeof(real_to));
    if ( KODE_OK != rc )
    {
        return rc;
    }

    /* FatFs refuses a rename onto a name that is there (FR_EXIST) and the
     * VFS passes it on, where POSIX's rename() replaces the target. Said
     * here, so a caller that forgot to remove the target fails on the host as
     * it does on the board. The same object under a name that only differs in
     * case is no other object: FatFs renames it. */
    struct stat taken;
    struct stat moving;
    if ( (0 == stat(real_to, &taken))
        && !((0 == stat(real_from, &moving)) && (taken.st_dev == moving.st_dev)
             && (taken.st_ino == moving.st_ino)) )
    {
        return KODE_ERROR;
    }

    if ( rename(real_from, real_to) != 0 )
    {
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t kdrv_storage_open_path(const char *path, uint32_t *size)
{
    if ( (NULL == path) || (NULL == size) || (NULL != s_read) )
    {
        return KODE_DENIED;
    }

    char                real[KDRV_REAL_PATH_MAX];
    kode_status_t const rc = real_path(path, real, sizeof(real));
    if ( KODE_OK != rc )
    {
        return rc;
    }

    FILE *f = fopen(real, "rb");
    if ( NULL == f )
    {
        return KODE_ERROR;
    }

    (void)fseek(f, 0, SEEK_END);
    long const len = ftell(f);
    (void)fseek(f, 0, SEEK_SET);
    if ( 0 >= len )
    {
        /* KODE_DENIED, not KODE_ERROR: the file IS here, it is just not
         * usable. Telling the operator it was not found would send them
         * looking for a file they are staring at - and this is exactly what
         * a counterfeit card leaves behind, so it is a field condition
         * rather than a corner. */
        (void)fclose(f);
        KLOG_E(TAG, "'%s' is empty", path);
        return KODE_DENIED;
    }

    s_read = f;
    *size  = (uint32_t)len;
    return KODE_OK;
}

kode_status_t kdrv_storage_open(const char *name, uint32_t *size,
                                kdrv_medium_t *medium)
{
    /* The card first, and the reason is a statement of intent rather than a
     * preference: somebody who went to the trouble of inserting a card meant
     * it, and the internal partition is the fallback because it is always
     * there and needs nothing from the ESP32-C5.
     *
     * This array is the only place that order is written down, and it is an
     * array so that it can be. Walked as a loop over the enum the policy
     * would be inferable only from the fact that KDRV_MEDIUM_SD happens to
     * be declared first - a fact about a type standing in for a decision
     * about hardware. */
    static const kdrv_medium_t ORDER[] = { KDRV_MEDIUM_SD, KDRV_MEDIUM_FS };

    if ( (NULL == name) || (NULL == size) )
    {
        return KODE_DENIED;
    }

    for ( size_t i = 0U; (sizeof(ORDER) / sizeof(ORDER[0])) > i; ++i )
    {
        char vpath[KDRV_VPATH_MAX];

        if ( !vpath_of(ORDER[i], name, vpath, sizeof(vpath)) )
        {
            continue;
        }

        kode_status_t const rc = kdrv_storage_open_path(vpath, size);
        if ( KODE_OK == rc )
        {
            if ( NULL != medium )
            {
                *medium = ORDER[i];
            }
            return KODE_OK;
        }
        /* KODE_DENIED ends the search instead of moving it along, and that
         * asymmetry is why open_path() has two ways to fail at all.
         * KODE_ERROR means "not on this volume" and the next one is worth a
         * look. KODE_DENIED means the file is HERE and cannot be used -
         * zero length, or a handle already open - and both of those are
         * answers. Going on past an empty file would report it as missing. */
        if ( KODE_DENIED == rc )
        {
            return KODE_DENIED;
        }
    }
    return KODE_ERROR;   /* nowhere to be found */
}

kode_status_t kdrv_storage_read(void *buf, size_t max, size_t *got)
{
    if ( (NULL == s_read) || (NULL == buf) || (NULL == got) )
    {
        return KODE_DENIED;
    }
    size_t const n = fread(buf, 1U, max, s_read);
    if ( (n < max) && (ferror(s_read) != 0) )
    {
        *got = 0U;
        return KODE_ERROR;
    }
    *got = n;
    return KODE_OK;
}

kode_status_t kdrv_storage_seek(uint32_t offset)
{
    if ( NULL == s_read )
    {
        return KODE_ERROR;
    }
    if ( fseek(s_read, (long)offset, SEEK_SET) != 0 )
    {
        return KODE_ERROR;
    }
    return KODE_OK;
}

/** Where the file being written lives, so a failed write can undo itself. */
static char s_write_path[KDRV_REAL_PATH_MAX];

/* Whether the open write is an append onto a file that may already have held
 * good data, as opposed to a truncating create that started the file from
 * nothing. kdrv_storage_discard() reads this to decide whether removing
 * s_write_path whole is undoing this session's write or destroying a
 * caller's pre-existing file along with it - see the note on discard() in the
 * header for why the two cases cannot be treated the same. */
static bool s_write_append;

kode_status_t kdrv_storage_create_path(const char *path, bool append)
{
    /* The writer alone. A read in flight used to be refused here too, and
     * only because there was one handle to be in the way - not because
     * anybody wanted a rule that a file cannot be written while another is
     * being read. Copying is that pair, and refusing it was the whole
     * obstacle. */
    if ( (NULL == path) || (NULL != s_write) )
    {
        return KODE_DENIED;
    }

    char                real[KDRV_REAL_PATH_MAX];
    kode_status_t const rc = real_path(path, real, sizeof(real));
    if ( KODE_OK != rc )
    {
        return rc;
    }

    const char *mode = "wb";
    if ( append )
    {
        mode = "ab";
    }

    FILE *f = fopen(real, mode);
    if ( NULL == f )
    {
        /* The same table kdrv_storage_mkdir() reads, for the same reason.
         * "wb"/"ab" create the name as part of opening it, so EEXIST never
         * arrives here - but ENOTDIR (a parent that is a file) and EISDIR
         * (the path itself is a directory) both do, and calling either one a
         * device fault would assert that the medium refused when nothing
         * reached the medium at all. EIO on a card refusing the
         * directory-entry write is what genuinely does. */
        return status_of_create_errno(errno);
    }

    s_write = f;
    /* The real path, not the virtual one: kdrv_storage_discard() hands this
     * straight to remove(), which knows nothing about this layer's tree. */
    (void)snprintf(s_write_path, sizeof(s_write_path), "%s", real);
    s_write_append = append;
    return KODE_OK;
}

kode_status_t kdrv_storage_create(const char *name, kdrv_medium_t first,
                                  kdrv_medium_t *medium)
{
    if ( (NULL == name) || (NULL != s_write) )
    {
        return KODE_DENIED;
    }

    /* @p first, then the others in the usual order. There is no ORDER array
     * here the way kdrv_storage_open() has one, and the absence IS the
     * policy: reading has one order for everybody, writing takes its order
     * from the caller, because the caller is the only one who knows whether
     * these bytes are a capture, an app image or a card test. */
    for ( int n = 0; (int)KDRV_MEDIUM_COUNT >= n; ++n )
    {
        kdrv_medium_t m = first;
        if ( 0 < n )
        {
            m = (kdrv_medium_t)(n - 1);
            if ( m == first )
            {
                continue;           /* already tried, as the preference */
            }
        }

        char vpath[KDRV_VPATH_MAX];
        if ( !vpath_of(m, name, vpath, sizeof(vpath)) )
        {
            continue;
        }
        /* Truncating, never appending: this is the create the flat API has
         * always been, and the file it is handed is an image or a capture
         * being written whole. Appending is what a path caller asks for by
         * name. */
        if ( kdrv_storage_create_path(vpath, false) == KODE_OK )
        {
            if ( NULL != medium ) { *medium = m; }
            return KODE_OK;
        }
    }
    KLOG_E(TAG, "no writable medium for '%s'", name);
    return KODE_ERROR;
}

void kdrv_storage_discard(void)
{
    /* The name and the append flag are both taken before closing, because
     * closing forgets both. That order is what keeps this function honest: it
     * can only ever act on the file that was open when it was called, and
     * only in the way that file was opened. */
    char       path[KDRV_REAL_PATH_MAX];
    bool const was_append = s_write_append;
    (void)snprintf(path, sizeof(path), "%s", s_write_path);

    /* The status is ignored here and nowhere else. kdrv_storage_close()
     * answers KODE_ERROR when the writer's last buffered bytes did not reach
     * the medium - which matters to every caller that intends to KEEP the
     * file, and to no caller of this one: a discard is already on its way to
     * removing that file, so "some of it did not land" is news about bytes
     * that are about to stop existing. The one case where it would matter is
     * an append, and there the removal is refused below for a reason that
     * has nothing to do with how the close went. */
    (void)kdrv_storage_close();

    if ( '\0' == path[0] )
    {
        return;
    }

    if ( was_append )
    {
        /* create_path(path, true) can reopen a file that already held good
         * data - a log from an earlier session, a capture resumed after a
         * reset - and this function's contract is to undo the write that
         * just failed, not to take the rest of the file down with it.
         * Removing it whole was correct back when every create() truncated,
         * because "the file being written" and "the file's entire content"
         * were the same thing then. They no longer are once append exists, so
         * removal is refused here instead of quietly carrying the old
         * assumption forward into the day something calls this after an
         * append - `write append=on` and `file_put` both will. */
        KLOG_W(TAG, "discard: '%s' was opened for append, leaving it in place",
               path);
        return;
    }

    (void)remove(path);
}

kode_status_t kdrv_storage_write(const void *buf, size_t len)
{
    if ( (NULL == s_write) || (NULL == buf) )
    {
        return KODE_DENIED;
    }
    if ( 0U == len )
    {
        return KODE_OK;
    }
    if ( fwrite(buf, 1U, len, s_write) != len )
    {
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t kdrv_storage_close(void)
{
    kode_status_t rc = KODE_OK;

    if ( NULL != s_read )
    {
        /* A reader's close is the only fclose() in this file whose answer is
         * genuinely worth nothing: there is no buffer of ours waiting to
         * reach the medium, so the worst it can report is an error the read
         * calls already returned. */
        (void)fclose(s_read);
        s_read = NULL;
    }

    if ( NULL != s_write )
    {
        /* The writer's is the opposite, and this used to be a `(void)` too.
         * fclose() is where a buffered write finally meets the card, so a
         * card that cannot take the bytes says so HERE and nowhere earlier -
         * every kdrv_storage_write() before it can have answered KODE_OK for
         * bytes that only ever reached a FILE buffer. Throwing that away
         * meant reporting success for a file that did not fully land, which
         * is exactly the counterfeit-card failure this component keeps
         * warning about: one failed write leaves FATFS holding sectors it
         * can never flush, and every later open on that volume comes back
         * zero length for the rest of the session. */
        if ( fclose(s_write) != 0 )
        {
            KLOG_E(TAG, "'%s' did not close cleanly; its last bytes may not "
                        "have reached the medium", s_write_path);
            rc = KODE_ERROR;
        }
        s_write = NULL;
    }

    /* kdrv_storage_copy_end() always clears s_copying itself, before it ever
     * calls into this function (directly, or by way of
     * kdrv_storage_discard()) - so seeing it still set here means somebody
     * else closed this file out from under a copy without going through
     * copy_end() at all. That is the one case copy_end()'s own no-stub
     * promise cannot keep, because copy_end() was never called to keep it:
     * the destination is a truncating create with nothing in it that
     * predates this copy, exactly kdrv_storage_discard()'s safe case, so it
     * is removed here rather than left as a name in a listing that promises
     * bytes it does not have. AO_Storage's STORAGE_CLOSE_REQ_SIG guard is
     * what keeps this branch from firing today, but that guard lives one
     * layer up and this file does not get to assume it is always there -
     * a promise this header makes has to hold on its own. */
    if ( s_copying )
    {
        KLOG_W(TAG, "'%s' was mid-copy when it closed; removing the partial "
                    "file", s_write_path);
        if ( '\0' != s_write_path[0] )
        {
            (void)remove(s_write_path);
        }
    }

    /* The write path belongs to the file that was open, not to the session
     * after it. Leaving it set let a later kdrv_storage_discard() - which is
     * for a write that failed half way - remove a file that had been written
     * and closed cleanly, because nothing said a write was what was open.
     * create() then close() then open() then discard() deleted the first
     * file; both halves are ordinary operations. The append flag is reset for
     * the same reason and on the same schedule: it describes the write that
     * just ended, not whatever opens next. Cleared after the log lines above,
     * which need to be able to name the file they are complaining about.
     *
     * s_copying goes with them, and for the same kind of reason: a copy is
     * exactly the two handles this call has just let go of, so there is no
     * copy left to be in after it. Leaving the flag set would refuse every
     * later kdrv_storage_copy_open() for the rest of the boot on the one
     * path nothing else covers - a caller that closed out from under its own
     * copy, which is what a host case does when an assertion abandons it
     * half way. */
    s_write_path[0]  = '\0';
    s_write_append   = false;
    s_copying        = false;
    return rc;
}

/* ---- Copying ---------------------------------------------------------------
 * The reader and the writer above, held at once, which is the one thing the
 * split exists for. There is no buffer here: the caller lends one, because
 * AO_Storage already owns two 32 KB PSRAM buffers for streaming and a third
 * of this layer's own would be 32 KB of internal RAM idle most of the time.
 *
 * s_copying is what tells a step belonging to a copy apart from a reader and
 * a writer that merely happen to both be open - an unrelated `file_get` and
 * an unrelated `write`, say. Without it kdrv_storage_copy_step() would
 * cheerfully pump one into the other. It is declared at the top of this file
 * beside the two handles it describes, and cleared by kdrv_storage_close()
 * along with them.
 */

kode_status_t kdrv_storage_copy_open(const char *from, const char *to,
                                     uint32_t *size)
{
    if ( (NULL == from) || (NULL == to) || (NULL == size) || s_copying )
    {
        return KODE_DENIED;
    }

    /* Before either handle is opened, because the order they would open in
     * is fatal: the source goes first, then the destination truncating, and
     * a copy onto its own source would have destroyed the file before the
     * first step ran. Only the virtual paths are compared - a caller that
     * spells one file two ways is not protected here, which is why the
     * normaliser above this layer is a contract rather than a courtesy. */
    if ( strcmp(from, to) == 0 )
    {
        KLOG_E(TAG, "refusing to copy '%s' onto itself", from);
        return KODE_DENIED;
    }

    kode_status_t rc = kdrv_storage_open_path(from, size);
    if ( KODE_OK != rc )
    {
        return rc;
    }

    /* Truncating, never appending, and that is load-bearing twice over: a
     * copy replaces the destination, and copy_end(false) may then remove it
     * whole because it holds nothing this call put there. */
    rc = kdrv_storage_create_path(to, false);
    if ( KODE_OK != rc )
    {
        /* The source is let go rather than left dangling. A copy that failed
         * to start must leave nothing behind, or the next command is refused
         * with "a file is already open" about a copy that never happened. */
        (void)kdrv_storage_close();
        return rc;
    }

    s_copying = true;
    return KODE_OK;
}

kode_status_t kdrv_storage_copy_step(void *buf, size_t cap, size_t *done)
{
    if ( (NULL == buf) || (NULL == done) || (0U == cap) || !s_copying )
    {
        return KODE_DENIED;
    }

    *done = 0U;

    size_t        got = 0U;
    kode_status_t rc  = kdrv_storage_read(buf, cap, &got);
    if ( KODE_OK != rc )
    {
        return rc;
    }
    if ( 0U == got )
    {
        /* The source ran out. Reported as a successful step that moved
         * nothing, which is kdrv_storage_read()'s own end-of-file signal
         * rather than a second vocabulary invented here for the same event. */
        return KODE_OK;
    }

    rc = kdrv_storage_write(buf, got);
    if ( KODE_OK != rc )
    {
        return rc;
    }
    *done = got;
    return KODE_OK;
}

/* s_copying is cleared here, first, before either arm below ever calls into
 * kdrv_storage_close() (directly, or by way of kdrv_storage_discard()). That
 * order is what lets kdrv_storage_close() tell an ordinary end of copy from
 * one it never heard about: it only removes a stray destination when it
 * finds s_copying STILL set, which - now that this function always clears it
 * first - can only mean somebody closed the file out from under a copy
 * without going through this call at all. */
kode_status_t kdrv_storage_copy_end(bool keep)
{
    s_copying = false;

    if ( !keep )
    {
        /* kdrv_storage_discard() is already exactly this case: the
         * destination was opened with a truncating create, so removing it
         * whole costs nothing that existed before, and a name in a listing
         * promising bytes it does not have is worse than no file at all -
         * `app_run` would try to boot it. It closes the reader as well,
         * because kdrv_storage_close() closes both, which is what an
         * abandoned copy wants. */
        kdrv_storage_discard();
        return KODE_OK;
    }

    /* Taken before the close, which forgets it. */
    char path[KDRV_REAL_PATH_MAX];
    (void)snprintf(path, sizeof(path), "%s", s_write_path);

    kode_status_t const rc = kdrv_storage_close();
    if ( KODE_OK == rc )
    {
        return KODE_OK;
    }

    /* The close is where a card that cannot take the bytes finally says so,
     * and a destination whose last bytes never landed is the same stub the
     * abandoned path above removes - reached by a different route. Keeping
     * it and answering KODE_ERROR would leave a file that looks copied in
     * every listing; removing it says what actually happened. WHY it did not
     * close is kdrv_storage_close()'s own line, a moment ago and naming the
     * same file; this one says only what is being done about it, so the two
     * are one story rather than the same sentence twice. */
    KLOG_E(TAG, "removing the partial copy at '%s'", path);
    if ( '\0' != path[0] )
    {
        (void)remove(path);
    }
    return rc;
}

kode_status_t kdrv_storage_remove(const char *name, kdrv_medium_t m)
{
    if ( (NULL == name) || (KDRV_MEDIUM_COUNT <= m) )
    {
        return KODE_DENIED;
    }

    /* One medium and no loop, which is the whole policy of this call: the
     * caller names where, and a delete that went hunting could remove the
     * wrong copy of a name that exists on both volumes. There is no undo. */
    char vpath[KDRV_VPATH_MAX];
    if ( !vpath_of(m, name, vpath, sizeof(vpath)) )
    {
        return KODE_DENIED;
    }

    char real[KDRV_REAL_PATH_MAX];
    if ( real_path(vpath, real, sizeof(real)) != KODE_OK )
    {
        /* A refusal, not a failed delete. real_path() answers KODE_ERROR for
         * a volume that will not mount, and the header names that case
         * KODE_DENIED: "there is no such volume" is a bad argument, and the
         * caller has to be able to tell it apart from "the file would not
         * go", which is the only thing KODE_ERROR means here. */
        return KODE_DENIED;
    }

    if ( remove(real) != 0 )
    {
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t kdrv_storage_selftest(kdrv_medium_t m)
{
    const char *mount = kdrv_storage_mount(m);
    if ( NULL == mount )
    {
        return KODE_DENIED;
    }
    /* A real path, so it is sized from the one constant that says how long
     * one may get - the off-board suite points the mount points at $TMPDIR
     * and a 64-byte buffer would truncate there. */
    char path[KDRV_REAL_PATH_MAX];
    (void)snprintf(path, sizeof(path), "%s/.kodeos_test", mount);

    static const char payload[] = "kodeos-storage-selftest";
    FILE *f = fopen(path, "wb");
    if ( (NULL == f) || (fwrite(payload, 1, sizeof(payload), f) != sizeof(payload)) )
    {
        if ( NULL != f ) { (void)fclose(f); }
        KLOG_E(TAG, "selftest: write to %s failed", mount);
        return KODE_ERROR;
    }
    (void)fclose(f);

    char back[sizeof(payload)] = {0};
    f = fopen(path, "rb");
    bool const ok = (NULL != f)
                    && (fread(back, 1, sizeof(back), f) == sizeof(back))
                    && (memcmp(back, payload, sizeof(payload)) == 0);
    if ( NULL != f ) { (void)fclose(f); }
    (void)remove(path);

    return ok ? KODE_OK : KODE_ERROR;
}
