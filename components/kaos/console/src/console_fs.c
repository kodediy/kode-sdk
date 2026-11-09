/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file console_fs.c
 * @brief See console_fs.h: the file verbs below cmd_begin(), and the cwd.
 */
#include "console_fs.h"

#include <stdio.h>
#include <string.h>

#include "kaos.h"
#include "kdrv_storage.h"
#include "kode_log.h"

/* Nothing here writes to the wire: the `>|` records and the terminator are AO_Console's alone. */
#define TAG "kaos.fs"

/* AO_Storage's doors, named weakly (KAOS_WEAK, kaos.h): every verb here is
 * refused by the console before it reaches them when the app did not link
 * AO_Storage, and naming them strongly would link it into every console. */
KAOS_WEAK void storage_path_request(QActive *who, uint8_t op, const char *path);
KAOS_WEAK void storage_cat_request(QActive *who, const char *path, uint32_t max);
KAOS_WEAK void storage_pair_request(QActive *who, uint8_t op, const char *src,
                                    const char *dst);
KAOS_WEAK void storage_text_request(QActive *who, const char *path,
                                    const char *text, bool append, uint16_t line);
KAOS_WEAK bool storage_recv_request(QActive *who, const char *path, uint32_t size,
                                    uint32_t crc32, uint8_t src);

/* One working directory, shared by the serial and BLE ports, lost at reset
 * and never stored in NVS. Every verb also takes an absolute path. */
static char s_cwd[KODE_PATH_MAX] = "/";

/* The resolved path of the request in flight. The console runs one command
 * at a time, so one buffer is enough. `stat` prints it; `cd` adopts it. */
static char s_target[KODE_PATH_MAX] = "/";

/* ---- Resolution ---------------------------------------------------------- */

kode_status_t console_fs_resolve_from(const char *cwd, const char *in,
                                      char *out, size_t cap)
{
    /* Twice KODE_PATH_MAX: the join comes before normalising, and only the
     * RESULT must fit KODE_PATH_MAX. */
    char   work[KODE_PATH_MAX * 2U];
    char  *comp[KODE_PATH_MAX / 2U];
    char  *save = NULL;
    char  *tok;
    size_t n    = 0U;
    size_t used = 0U;
    int    wrote;

    if ( (NULL == cwd) || (NULL == in) || (NULL == out) || (0U == cap) )
    {
        return KODE_DENIED;
    }

    /* A relative path is joined to the cwd before anything is interpreted,
     * so there is one string to walk. */
    if ( '/' == in[0] )
    {
        wrote = snprintf(work, sizeof(work), "%s", in);
    }
    else
    {
        wrote = snprintf(work, sizeof(work), "%s/%s", cwd, in);
    }
    if ( (0 > wrote) || ((size_t)wrote >= sizeof(work)) )
    {
        return KODE_DENIED;
    }

    /* strtok_r, not strtok: no hidden cursor shared with other callers. */
    tok = strtok_r(work, "/", &save);
    while ( NULL != tok )
    {
        if ( strcmp(tok, ".") == 0 )
        {
            /* The current directory is where we already are. */
        }
        else if ( strcmp(tok, "..") == 0 )
        {
            /* At the root there is nothing to pop: `..` stays at `/`. */
            if ( 0U < n )
            {
                n--;
            }
        }
        else if ( (sizeof(comp) / sizeof(comp[0])) > n )
        {
            comp[n] = tok;
            n++;
        }
        else
        {
            /* More components than a path this long can possibly have. The
             * result would not fit either; refusing here says so earlier. */
            return KODE_DENIED;
        }
        tok = strtok_r(NULL, "/", &save);
    }

    /* Nothing left standing means the root, which is the one path that is a
     * separator rather than being separated by them. */
    if ( 0U == n )
    {
        if ( 2U > cap )
        {
            return KODE_DENIED;
        }
        out[0] = '/';
        out[1] = '\0';
        return KODE_OK;
    }

    for ( size_t i = 0U; i < n; ++i )
    {
        wrote = snprintf(&out[used], cap - used, "/%s", comp[i]);
        if ( (0 > wrote) || ((size_t)wrote >= (cap - used)) )
        {
            /* Never a truncated path: a path cut short names a different
             * file, and the verbs above this would open it without noticing. */
            return KODE_DENIED;
        }
        used += (size_t)wrote;
    }
    return KODE_OK;
}

kode_status_t console_fs_resolve(const char *in, char *out, size_t cap)
{
    const char *arg = in;

    /* An optional `path=` arrives as NULL rather than as "": the validator
     * leaves an absent key's value alone, and both mean the same thing here. */
    if ( NULL == arg )
    {
        arg = "";
    }
    return console_fs_resolve_from(s_cwd, arg, out, cap);
}

const char *console_fs_cwd(void)
{
    return s_cwd;
}

const char *console_fs_target(void)
{
    return s_target;
}

const char *console_fs_medium_name(const char *path)
{
    kdrv_medium_t const m = kdrv_storage_medium_of(path);

    if ( KDRV_MEDIUM_SD == m )
    {
        return "sd";
    }
    if ( KDRV_MEDIUM_FS == m )
    {
        return "fs";
    }
    /* The root is on neither volume. */
    return "-";
}

/* ---- The verbs ----------------------------------------------------------- */

/**
 * @brief Resolve @p in and remember it as the request in flight.
 *
 * @param in Absolute or relative, or NULL for the working directory.
 * @return KODE_OK, or KODE_DENIED when it does not resolve, s_target untouched.
 */
static kode_status_t aim(const char *in)
{
    char                resolved[KODE_PATH_MAX];
    kode_status_t const rc = console_fs_resolve(in, resolved, sizeof(resolved));

    if ( KODE_OK != rc )
    {
        return rc;
    }
    /* Both buffers are KODE_PATH_MAX, so this cannot truncate. */
    (void)snprintf(s_target, sizeof(s_target), "%s", resolved);
    return KODE_OK;
}

/**
 * @brief Resolve @p in, remember it, and ask AO_Storage what it is.
 *
 * The verbs that act on what a path is ask first; path_answer() in
 * console_ao.c handles the answer per verb.
 *
 * @return KODE_OK when the question went out; KODE_DENIED when @p in does
 *         not resolve, and nothing will answer.
 */
static kode_status_t ask_stat(QActive *who, const char *in)
{
    kode_status_t const rc = aim(in);

    if ( KODE_OK != rc )
    {
        return rc;
    }
    storage_path_request(who, (uint8_t)STORAGE_PATH_STAT, s_target);
    return KODE_OK;
}

kode_status_t console_fs_chdir(QActive *who, const char *in)
{
    return ask_stat(who, in);
}

void console_fs_enter(void)
{
    /* Both buffers are KODE_PATH_MAX, so this cannot truncate. */
    (void)snprintf(s_cwd, sizeof(s_cwd), "%s", s_target);
    KLOG_D(TAG, "working directory is now '%s'", s_cwd);
}

kode_status_t console_fs_ls(QActive *who, const char *in)
{
    /* The LIST terminator carries no status, so an empty directory and a
     * missing one would look the same: STAT first, and
     * console_fs_list_begin() lists once the answer says directory. */
    return ask_stat(who, in);
}

void console_fs_list_begin(QActive *who)
{
    storage_path_request(who, (uint8_t)STORAGE_PATH_LIST, s_target);
}

kode_status_t console_fs_stat(QActive *who, const char *in)
{
    return ask_stat(who, in);
}

kode_status_t console_fs_cat(QActive *who, const char *in)
{
    return ask_stat(who, in);
}

void console_fs_cat_begin(QActive *who, uint32_t max)
{
    storage_cat_request(who, s_target, max);
}

kode_status_t console_fs_mkdir(QActive *who, const char *in)
{
    kode_status_t const rc = aim(in);

    if ( KODE_OK != rc )
    {
        return rc;
    }
    storage_path_request(who, (uint8_t)STORAGE_PATH_MKDIR, s_target);
    return KODE_OK;
}

kode_status_t console_fs_rm(QActive *who, const char *in)
{
    return ask_stat(who, in);
}

void console_fs_rm_begin(QActive *who)
{
    storage_path_request(who, (uint8_t)STORAGE_PATH_UNLINK, s_target);
}

kode_status_t console_fs_rmdir(QActive *who, const char *in)
{
    return ask_stat(who, in);
}

void console_fs_rmdir_begin(QActive *who)
{
    storage_path_request(who, (uint8_t)STORAGE_PATH_RMDIR, s_target);
}

kode_status_t console_fs_pair(QActive *who, uint8_t op, const char *src,
                              const char *dst)
{
    char          rsrc[KODE_PATH_MAX];
    char          rdst[KODE_PATH_MAX];
    kode_status_t rc;

    rc = console_fs_resolve(src, rsrc, sizeof(rsrc));
    if ( KODE_OK != rc )
    {
        return rc;
    }
    rc = console_fs_resolve(dst, rdst, sizeof(rdst));
    if ( KODE_OK != rc )
    {
        return rc;
    }
    storage_pair_request(who, op, rsrc, rdst);
    return KODE_OK;
}

kode_status_t console_fs_write(QActive *who, const char *in, const char *text,
                               bool append, uint16_t line)
{
    kode_status_t const rc = aim(in);

    if ( KODE_OK != rc )
    {
        return rc;
    }
    storage_text_request(who, s_target, text, append, line);
    return KODE_OK;
}

kode_status_t console_fs_put(QActive *who, const char *in, uint32_t size,
                             uint32_t crc32, uint8_t src)
{
    kode_status_t const rc = aim(in);

    if ( KODE_OK != rc )
    {
        return rc;
    }

    /* Refuse a path on no volume (or "/") here, before the wire goes raw, so
     * it reads as a bad `path` like every other file verb. Ask
     * kdrv_storage_medium_of(): do not spell the volume prefixes again. */
    if ( kdrv_storage_medium_of(s_target) == KDRV_MEDIUM_COUNT )
    {
        return KODE_DENIED;
    }

    if ( !storage_recv_request(who, s_target, size, crc32, src) )
    {
        /* An empty event pool, not a bad path: the caller words it apart. */
        return KODE_NO_RESOURCES;
    }
    return KODE_OK;
}

void console_fs_reset(void)
{
    (void)snprintf(s_cwd, sizeof(s_cwd), "%s", "/");
    (void)snprintf(s_target, sizeof(s_target), "%s", "/");
}
