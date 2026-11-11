/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_file.c
 * @brief Paths, busy-retry, status phrases, per-task state, the files lock and the file calls.
 */
#include "kode_file.h"

#include <string.h>

#include "khal_once.h"
#include "khal_sync.h"
#include "khal_system.h"
#include "kode_entry.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kode_task.h"

/** @brief What the user types for the card. */
#define KODE_FILE_SD_NAME "sd"

/** @brief What the user types for the internal volume. */
#define KODE_FILE_FLASH_NAME "flash"

/** @brief The card's volume path. */
#define KODE_FILE_SD_ROOT "/sdcard"

/** @brief The internal volume's path. */
#define KODE_FILE_FLASH_ROOT "/storage"

/** @brief Bytes of the longest volume name typed, "flash", NUL included. */
#define KODE_FILE_VOLUME_BYTES ((uint32_t)sizeof(KODE_FILE_FLASH_NAME))

/** @brief The most a volume path grows a typed path: "sd" to "/sdcard". */
#define KODE_FILE_GROWTH_MAX ((uint32_t)(sizeof(KODE_FILE_SD_ROOT) - sizeof(KODE_FILE_SD_NAME)))

/** @brief What the error text shows for a path that is NULL. */
#define KODE_FILE_NULL_SHOWN "(null)"

/** @brief What kode_file_why() says for a status with no phrase of its own. */
#define KODE_FILE_WHY_OTHER "the call failed"

/* "sd" grows to "/sdcard": the longest typed path must still fit KODE_PATH_MAX. */
_Static_assert((KODE_FILE_PATH_MAX + KODE_FILE_GROWTH_MAX) <= KODE_PATH_MAX,
               "the longest typed path, grown to a volume path, outgrows the extended layer's buffer");

_Static_assert((sizeof(KODE_FILE_FLASH_ROOT) - sizeof(KODE_FILE_FLASH_NAME)) <= KODE_FILE_GROWTH_MAX,
               "a volume path grows a typed path by more than KODE_FILE_GROWTH_MAX");

_Static_assert(sizeof(KODE_FILE_FLASH_NAME) >= sizeof(KODE_FILE_SD_NAME),
               "the longest volume name typed is no longer \"flash\"");

_Static_assert(KODE_FILE_READ_MAX == (4U * 1024U * 1024U),
               "the error text of a file over the ceiling says 4 MiB");

_Static_assert(KODE_FILE_LIST_MAX == KODE_SYS_FILE_LIST_MAX,
               "kode.h's listing and kode_sys.h's have drifted apart");

_Static_assert(KODE_FILE_NAME_MAX == KODE_APP_FILE_MAX,
               "kode.h's name and AO_Storage's file name have drifted apart");

/** @brief What a task keeps for its file calls, each block its own PSRAM. */
struct kode_file_state {
    char             *text;   /**< The last kode_file_read() text.      */
    uint8_t          *bytes;  /**< The last kode_file_read_bytes() data. */
    kode_file_list_t *list;   /**< The last kode_file_list() listing.   */
};

/** @brief What one try of kode_file_sys() carries. */
typedef struct {
    kode_status_t (*op)(void *ctx);  /**< The storage call.     */
    void           *ctx;             /**< Handed to the call.   */
    kode_status_t   status;          /**< Its last answer.      */
} sys_try_t;

/** @brief The path to show in an error: @p user, or "(null)" if it is NULL. */
static const char *path_shown(const char *user)
{
    const char *shown = KODE_FILE_NULL_SHOWN;

    if ( NULL != user )
    {
        shown = user;
    }
    return shown;
}

/** @brief Keep @p user in the task's error as a path that is not one. */
static void path_bad(const char *user)
{
    kode_err_set("bad path '%s': start with sd/ or flash/", path_shown(user));
}

/** @brief Keep @p user in the task's error as a path with a bad part. */
static void path_bad_part(const char *user)
{
    kode_err_set("bad path '%s': no empty, '.' or '..' part", user);
}

/** @brief Keep @p user in the task's error as a path too long. */
static void path_too_long(const char *user)
{
    kode_err_set("path '%s' is too long", user);
}

/**
 * @brief Length of @p user, counted up to KODE_FILE_PATH_MAX.
 * @param user A NUL-terminated string.
 * @return Its length; KODE_FILE_PATH_MAX if that long or longer.
 */
static uint32_t path_length(const char *user)
{
    uint32_t n = 0U;

    while ( (KODE_FILE_PATH_MAX > n) && ('\0' != user[n]) )
    {
        n++;
    }
    return n;
}

/**
 * @brief The volume path for the volume word typed.
 * @param user The typed path.
 * @param n    Characters of the volume word.
 * @return "/sdcard" or "/storage"; NULL for any other word.
 */
static const char *volume_root(const char *user, uint32_t n)
{
    char        word[KODE_FILE_VOLUME_BYTES];
    const char *root = NULL;

    if ( KODE_FILE_VOLUME_BYTES > n )
    {
        (void)memcpy(word, user, n);
        word[n] = '\0';
        if ( kode_name_is(word, KODE_FILE_SD_NAME) )
        {
            root = KODE_FILE_SD_ROOT;
        }
        else if ( kode_name_is(word, KODE_FILE_FLASH_NAME) )
        {
            root = KODE_FILE_FLASH_ROOT;
        }
        else
        {
            /* Not one of ours. */
        }
    }
    return root;
}

/**
 * @brief Whether a path part is usable.
 * @param part The part's first character.
 * @param n    Its length.
 * @return true unless it is empty, "." or "..".
 */
static bool part_ok(const char *part, uint32_t n)
{
    bool ok = (0U < n);

    if ( ok && ('.' == part[0]) )
    {
        ok = !((1U == n) || ((2U == n) && ('.' == part[1])));
    }
    return ok;
}

/**
 * @brief Whether every "/"-separated part is usable.
 * @param rest The characters after the volume's "/".
 * @param n    How many.
 * @return true when no part is empty, "." or "..".
 */
static bool parts_ok(const char *rest, uint32_t n)
{
    bool     ok    = (0U < n);
    uint32_t start = 0U;

    for ( uint32_t i = 0U; ok && (i <= n); i++ )
    {
        if ( (i == n) || ('/' == rest[i]) )
        {
            ok    = part_ok(&rest[start], i - start);
            start = i + 1U;
        }
    }
    return ok;
}

bool kode_file_path(const char *user, char *out, uint32_t cap)
{
    if ( NULL == user )
    {
        path_bad(user);
        return false;
    }

    uint32_t len = path_length(user);

    if ( KODE_FILE_PATH_MAX <= len )
    {
        path_too_long(user);
        return false;
    }
    if ( (0U < len) && ('/' == user[len - 1U]) )
    {
        len--;
    }

    uint32_t vol = 0U;

    while ( (vol < len) && ('/' != user[vol]) )
    {
        vol++;
    }

    const char *const root = volume_root(user, vol);

    if ( NULL == root )
    {
        path_bad(user);
        return false;
    }
    if ( (vol < len) && !parts_ok(&user[vol + 1U], len - vol - 1U) )
    {
        path_bad_part(user);
        return false;
    }

    const uint32_t root_len = (uint32_t)strlen(root);
    const uint32_t rest_len = len - vol;  /* the "/" and the parts, or nothing */

    if ( (NULL == out) || ((root_len + rest_len + 1U) > cap) )
    {
        path_too_long(user);
        return false;
    }
    (void)memcpy(out, root, root_len);
    (void)memcpy(&out[root_len], &user[vol], rest_len);
    out[root_len + rest_len] = '\0';
    return true;
}

struct kode_file_state *kode_file_state_ensure(void)
{
    kode_task_t *const task = kode_task_ensure();

    if ( NULL == task )
    {
        return NULL;
    }
    if ( NULL == task->file )
    {
        struct kode_file_state *s = NULL;

        /* cppcheck-suppress misra-c2012-11.5 ; the allocator returns void *; the block is a kode_file_state */
        s = (struct kode_file_state *)khal_alloc_psram(sizeof(void *), sizeof(*s));

        if ( NULL != s )
        {
            (void)memset(s, 0, sizeof(*s));
            task->file = s;
        }
    }
    return task->file;
}

void kode_file_state_free(struct kode_file_state *s)
{
    if ( NULL != s )
    {
        khal_free(s->text);
        khal_free(s->bytes);
        khal_free(s->list);
        khal_free(s);
    }
}

/** @brief One try of a storage call: true unless storage answered KODE_DENIED. */
static bool sys_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the sys_try_t kode_file_sys() passed */
    sys_try_t *const t = (sys_try_t *)ctx;

    t->status = t->op(t->ctx);
    return KODE_DENIED != t->status;
}

kode_status_t kode_file_sys(kode_status_t (*op)(void *ctx), void *ctx)
{
    sys_try_t t = { op, ctx, KODE_ERROR };

    if ( NULL != op )
    {
        (void)kode_retry(&sys_attempt, &t, KODE_FILE_BUSY_MS);
    }
    return t.status;
}

/** @brief The lock every public file call holds; made on the first call. */
static khal_mutex_t s_files_lock;

/** @brief Make the files lock. @return true when it exists. */
static bool files_lock_make(void)
{
    s_files_lock = khal_mutex_create();
    return NULL != s_files_lock;
}

/**
 * @brief Take the files lock, making it on the first call. Released by files_leave().
 * @return true when this caller holds the lock.
 */
static bool files_lock_take(void)
{
    static khal_once_t lock_once;  /* the one-shot the lock is made by */
    const bool         ok = khal_once_try(&lock_once, &files_lock_make);

    if ( ok )
    {
        khal_mutex_lock(s_files_lock);
    }
    return ok;
}

/**
 * @brief The prologue of a public file call: kode_enter(), then its turn.
 *
 * Another task's file call, whatever it does, finishes before this one starts.
 * Released by files_leave(); no public call calls another while it holds it.
 *
 * @param name The public name of the call.
 * @return true when the call has its turn; false with the error set.
 */
static bool files_enter(const char *name)
{
    bool ok = kode_enter(name);

    if ( ok )
    {
        ok = files_lock_take();
        if ( !ok )
        {
            kode_err_set("%s: no memory for the files lock", name);
        }
    }
    return ok;
}

/** @brief Give the turn back, at the end of a call files_enter() let in. */
static void files_leave(void)
{
    khal_mutex_unlock(s_files_lock);
}

const char *kode_file_why(kode_status_t rc)
{
    const char *why = KODE_FILE_WHY_OTHER;

    switch ( rc )
    {
    case KODE_DENIED:
        why = "storage is busy";
        break;
    case KODE_ERROR:
        why = "the volume is not there or the call failed";
        break;
    case KODE_TIMEOUT:
        why = "storage did not answer";
        break;
    case KODE_NO_RESOURCES:
        why = "out of memory";
        break;
    case KODE_NOT_DIR:
        why = "a folder in the path is a file";
        break;
    case KODE_IS_DIR:
        why = "it is a folder";
        break;
    case KODE_EXISTS:
        why = "already exists";
        break;
    case KODE_IO_ERROR:
        why = "the volume refused the write";
        break;
    default:
        /* No phrase of its own. */
        break;
    }
    return why;
}

/** @brief What one STAT try carries. */
typedef struct {
    const char            *path;  /**< The volume path.     */
    kode_sys_file_status_t st;    /**< What storage said.   */
} stat_try_t;

/** @brief What one read try carries. */
typedef struct {
    const char *path;    /**< The volume path.                   */
    uint32_t    offset;  /**< Where the window starts.           */
    uint8_t    *buf;     /**< Where the window goes.             */
    uint32_t    cap;     /**< Bytes of the window asked for.     */
    uint32_t    got;     /**< Bytes storage gave.                */
    uint32_t    size;    /**< What the file held when it was asked about. */
} read_try_t;

/** @brief What one write try carries. */
typedef struct {
    const char    *path;    /**< The volume path.                   */
    const uint8_t *data;    /**< The bytes of this part.            */
    uint32_t       len;     /**< How many.                          */
    bool           append;  /**< Keep what is there.                */
} write_try_t;

/** @brief One STAT of a path. */
static kode_status_t stat_op(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the stat_try_t the caller passed */
    stat_try_t *const t = (stat_try_t *)ctx;

    return kode_block_file_status(t->path, &t->st);
}

/** @brief One window of a file. */
static kode_status_t read_op(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the read_try_t the caller passed */
    read_try_t *const t  = (read_try_t *)ctx;
    kode_status_t     rc = kode_block_file_read_part(t->path, t->offset, t->buf, t->cap, &t->got);

    /* A file that is no longer the size it was asked about is no wait: the
     * window is empty and the walk reports the change. */
    if ( KODE_DENIED == rc )
    {
        kode_sys_file_status_t st    = { false, false, 0U };
        const bool             known = (kode_block_file_status(t->path, &st) == KODE_OK);

        if ( known && (!st.exists || (st.size != t->size)) )
        {
            t->got = 0U;
            rc     = KODE_OK;
        }
    }
    return rc;
}

/** @brief One part of a write. */
/* cppcheck-suppress constParameterCallback ; the signature is the one kode_file_sys() takes */
static kode_status_t write_op(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the write_try_t the caller passed */
    const write_try_t *const t = (const write_try_t *)ctx;

    return kode_block_file_write_buffer(t->path, t->data, t->len, t->append);
}

/**
 * @brief What a volume path is. The caller holds the files lock.
 * @param vol The volume path.
 * @param st  Out: the answer.
 * @return The last storage answer.
 */
static kode_status_t stat_run(const char *vol, kode_sys_file_status_t *st)
{
    stat_try_t          t  = { vol, { false, false, 0U } };
    const kode_status_t rc = kode_file_sys(&stat_op, &t);

    *st = t.st;
    return rc;
}

kode_status_t kode_file_stat(const char *vol, kode_sys_file_status_t *st)
{
    kode_status_t rc = KODE_NO_RESOURCES;

    *st = (kode_sys_file_status_t){ false, false, 0U };
    if ( files_lock_take() )
    {
        rc = stat_run(vol, st);
        files_leave();
    }
    return rc;
}

kode_status_t kode_file_window(const char *vol, uint32_t size, uint32_t offset, void *buf,
                               uint32_t cap, uint32_t *got)
{
    kode_status_t rc = KODE_NO_RESOURCES;

    *got = 0U;
    if ( files_lock_take() )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; the caller's window holds bytes */
        read_try_t t = { vol, offset, (uint8_t *)buf, cap, 0U, size };

        rc   = kode_file_sys(&read_op, &t);
        *got = t.got;
        files_leave();
    }
    return rc;
}

/**
 * @brief Ask what a path is and check it is a file a read can take.
 * @param user The path as typed, for the error text.
 * @param vol  The volume path.
 * @param size Out: the file's size.
 * @return true for a file within KODE_FILE_READ_MAX; false with the error set.
 */
static bool load_status(const char *user, const char *vol, uint32_t *size)
{
    kode_sys_file_status_t st = { false, false, 0U };
    const kode_status_t    rc = stat_run(vol, &st);
    bool                   ok = false;

    if ( KODE_OK != rc )
    {
        kode_err_set("%s: %s", user, kode_file_why(rc));
    }
    else if ( !st.exists )
    {
        kode_err_set("%s: no such file", user);
    }
    else if ( st.is_dir )
    {
        kode_err_set("%s: it is a folder", user);
    }
    else if ( KODE_FILE_READ_MAX < st.size )
    {
        /* The text says 4 MiB for KODE_FILE_READ_MAX. */
        kode_err_set("%s: too big (over 4 MiB)", user);
    }
    else
    {
        *size = st.size;
        ok    = true;
    }
    return ok;
}

/** @brief One window of a walk, handed to the sink. */
typedef struct walk walk_t;

/** @brief What a walk does with each window it reads. */
typedef bool (*walk_sink_t)(const walk_t *w, uint32_t got, uint32_t offset);

/** @brief A file read in windows from its start to its end. */
struct walk {
    const char  *user;      /**< The path as typed, for the error text.            */
    const char  *vol;       /**< The volume path.                                  */
    uint32_t     size;      /**< What the file held when it was asked about.       */
    uint8_t     *buf;       /**< Where windows land: size + 1 bytes, or one window. */
    bool         in_place;  /**< Each window lands at its offset in buf, not at 0.  */
    const char  *read_fail; /**< Put before the reason of a failed read.            */
    const char  *verb;      /**< "read" or "moved", in "changed while it was ...".  */
    walk_sink_t  sink;      /**< Takes each window; NULL when buf is the result.    */
    const void  *sink_ctx;  /**< Whatever the sink needs.                           */
};

/**
 * @brief Read a file in windows, handing each to the sink.
 * @param w The walk.
 * @return true when exactly the size arrived; false with the error set.
 */
static bool walk_file(const walk_t *w)
{
    /* Each window asks for one byte past the end: a file that has grown
     * answers with it, one that has shrunk answers short. A file of no bytes
     * is no read; the sink sees one empty window. */
    uint32_t offset = 0U;
    bool     ended  = false;
    bool     ok     = true;

    while ( ok && !ended )
    {
        uint32_t cap = (w->size + 1U) - offset;
        uint32_t got = 0U;

        if ( KODE_SYS_FILE_READ_MAX < cap )
        {
            cap = KODE_SYS_FILE_READ_MAX;
        }
        if ( 0U == w->size )
        {
            ended = true;  /* No bytes, no read. */
        }
        else
        {
            uint8_t *window = w->buf;

            if ( w->in_place )
            {
                window = &w->buf[offset];
            }

            read_try_t          t  = { w->vol, offset, window, cap, 0U, w->size };
            const kode_status_t rc = kode_file_sys(&read_op, &t);

            if ( KODE_OK != rc )
            {
                kode_err_set("%s: %s%s", w->user, w->read_fail, kode_file_why(rc));
                ok = false;
            }
            got = t.got;
            /* Short: the end. The extra byte arrived: the file grew. */
            ended = (got < cap) || ((offset + got) > w->size);
        }
        if ( ok && (NULL != w->sink) && ((0U < got) || (0U == offset)) )
        {
            ok = w->sink(w, got, offset);
        }
        offset += got;
    }
    if ( ok && (offset != w->size) )
    {
        kode_err_set("%s: changed while it was %s", w->user, w->verb);
        ok = false;
    }
    return ok;
}

/**
 * @brief Read a whole file into a new PSRAM block, NUL added.
 * @param user  The path as typed.
 * @param block Out: the block, or NULL for an empty file.
 * @param size  Out: the bytes in the file.
 * @return true on success; false with the error set and nothing taken.
 */
static bool file_load(const char *user, uint8_t **block, uint32_t *size)
{
    char     vol[KODE_PATH_MAX];
    uint32_t n  = 0U;
    bool     ok = kode_file_path(user, vol, sizeof(vol)) && load_status(user, vol, &n);

    *block = NULL;
    *size  = 0U;
    if ( ok && (0U < n) )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; the allocator returns void *; the block holds bytes */
        uint8_t *const b = (uint8_t *)khal_alloc_psram(sizeof(void *), n + 1U);

        if ( NULL == b )
        {
            kode_err_set("%s: %s", user, kode_file_why(KODE_NO_RESOURCES));
            ok = false;
        }
        else
        {
            const walk_t w = { user, vol, n, b, true, "", "read", NULL, NULL };

            if ( walk_file(&w) )
            {
                b[n]   = 0U;
                *block = b;
            }
            else
            {
                khal_free(b);
                ok = false;
            }
        }
    }
    if ( ok )
    {
        *size = n;
    }
    return ok;
}

const char *kode_file_read(const char *path)
{
    static const char no_text[] = "";  /* what a read that found nothing gives */
    const char       *text      = no_text;

    if ( files_enter("kode_file_read") )
    {
        struct kode_file_state *const s = kode_file_state_ensure();

        if ( NULL == s )
        {
            kode_err_set("%s: %s", path_shown(path), kode_file_why(KODE_NO_RESOURCES));
        }
        else
        {
            uint8_t *block = NULL;
            uint32_t size  = 0U;

            khal_free(s->text);
            s->text = NULL;
            if ( file_load(path, &block, &size) && (NULL != block) )
            {
                /* cppcheck-suppress misra-c2012-11.3 ; the block holds the file's characters */
                s->text = (char *)block;
                text    = s->text;
            }
        }
        files_leave();
    }
    return text;
}

const uint8_t *kode_file_read_bytes(const char *path, uint32_t *len)
{
    static const uint8_t no_bytes[1] = { 0U };  /* the bytes of an empty file */
    const uint8_t       *bytes       = NULL;

    if ( NULL != len )
    {
        *len = 0U;
    }
    if ( files_enter("kode_file_read_bytes") )
    {
        struct kode_file_state *const s = kode_file_state_ensure();

        if ( NULL == s )
        {
            kode_err_set("%s: %s", path_shown(path), kode_file_why(KODE_NO_RESOURCES));
        }
        else
        {
            uint8_t *block = NULL;
            uint32_t size  = 0U;

            khal_free(s->bytes);
            s->bytes = NULL;
            if ( file_load(path, &block, &size) )
            {
                s->bytes = block;
                bytes    = no_bytes;
                if ( NULL != block )
                {
                    bytes = block;
                }
                if ( NULL != len )
                {
                    *len = size;
                }
            }
        }
        files_leave();
    }
    return bytes;
}

bool kode_file_read_owned(const char *name, const char *path, uint8_t **block, uint32_t *len)
{
    bool ok = false;

    *block = NULL;
    *len   = 0U;
    if ( files_enter(name) )
    {
        ok = file_load(path, block, len);
        files_leave();
    }
    return ok;
}

/**
 * @brief Length of the folder part of a volume path.
 * @param vol A volume path.
 * @return The index of its last '/'; 0 when it has none or only the first.
 */
static uint32_t parent_len(const char *vol)
{
    uint32_t n     = (uint32_t)strlen(vol);
    bool     found = false;

    while ( (0U < n) && !found )
    {
        n--;
        found = ('/' == vol[n]);
    }
    if ( !found )
    {
        n = 0U;
    }
    return n;
}

/**
 * @brief Ask what a volume path is.
 * @param user The path as typed, for the error text.
 * @param vol  The volume path.
 * @param st   Out: what storage said; all false when it said nothing.
 * @return true when storage answered; false with the error set.
 */
static bool vol_stat(const char *user, const char *vol, kode_sys_file_status_t *st)
{
    const kode_status_t rc = stat_run(vol, st);

    if ( KODE_OK != rc )
    {
        kode_err_set("%s: %s", user, kode_file_why(rc));
    }
    return KODE_OK == rc;
}

/**
 * @brief Say if the folder above a volume path is there.
 * @param user The path as typed, for the error text.
 * @param vol  Its volume path.
 * @return true when it is a folder; false with the error set.
 */
static bool parent_ready(const char *user, const char *vol)
{
    char           parent[KODE_PATH_MAX];
    const uint32_t n  = parent_len(vol);
    bool           ok = true;

    if ( 0U < n )
    {
        kode_sys_file_status_t st;

        (void)memcpy(parent, vol, n);
        parent[n] = '\0';
        ok        = vol_stat(user, parent, &st);
        if ( ok && !st.exists )
        {
            kode_err_set("%s: the folder above does not exist", user);
            ok = false;
        }
        else if ( ok && !st.is_dir )
        {
            kode_err_set("%s: a folder in the path is a file", user);
            ok = false;
        }
        else
        {
            /* The folder is there. */
        }
    }
    return ok;
}

/**
 * @brief Say why a file or folder could not be created.
 *
 * A refusal to open or write may be for want of the folder above; that is
 * said first. Any other reason is storage's.
 *
 * @param user The path as typed.
 * @param vol  The volume path.
 * @param rc   What storage answered.
 */
static void create_failed(const char *user, const char *vol, kode_status_t rc)
{
    if ( ((KODE_ERROR != rc) && (KODE_IO_ERROR != rc)) || parent_ready(user, vol) )
    {
        kode_err_set("%s: %s", user, kode_file_why(rc));
    }
}

/**
 * @brief Say why a write stopped, and how far it had got.
 * @param user  The path as typed.
 * @param vol   The volume path.
 * @param rc    What storage answered.
 * @param done  Bytes already written.
 * @param total Bytes asked for.
 */
static void put_failed(const char *user, const char *vol, kode_status_t rc, uint32_t done,
                       uint32_t total)
{
    if ( 0U < done )
    {
        kode_err_set("%s: wrote %lu of %lu bytes: %s", user, (unsigned long)done,
                     (unsigned long)total, kode_file_why(rc));
    }
    else
    {
        create_failed(user, vol, rc);
    }
}

/**
 * @brief Write or append bytes in parts. The caller holds the files lock.
 * @param user   The path as typed.
 * @param vol    The volume path.
 * @param data   The bytes.
 * @param total  How many.
 * @param append Add at the end rather than replace.
 * @return true when all of it is written; false with the error set.
 */
static bool put_parts(const char *user, const char *vol, const uint8_t *data, uint32_t total, bool append)
{
    uint32_t done    = 0U;
    bool     started = false;
    bool     ok      = true;

    while ( ok && (!started || (done < total)) )
    {
        uint32_t part = total - done;

        if ( KODE_SYS_FILE_READ_MAX < part )
        {
            part = KODE_SYS_FILE_READ_MAX;
        }

        write_try_t         t  = { vol, &data[done], part, append || started };
        const kode_status_t rc = kode_file_sys(&write_op, &t);

        if ( KODE_OK == rc )
        {
            done   += part;
            started = true;
        }
        else
        {
            put_failed(user, vol, rc, done, total);
            ok = false;
        }
    }
    return ok;
}

bool kode_file_put_bytes(const char *user, const uint8_t *data, uint32_t len, bool append)
{
    char vol[KODE_PATH_MAX];
    bool ok = kode_file_path(user, vol, sizeof(vol));

    if ( ok && (NULL == data) )
    {
        kode_err_set("%s: no data to write", user);
        ok = false;
    }
    if ( ok )
    {
        ok = files_lock_take();
        if ( ok )
        {
            ok = put_parts(user, vol, data, len, append);
            files_leave();
        }
        else
        {
            kode_err_set("%s: no memory for the files lock", user);
        }
    }
    return ok;
}

/**
 * @brief Write or append @p text, for the two public calls.
 * @param name   The public name of the call.
 * @param path   The path as typed.
 * @param text   The text; NULL is refused.
 * @param append Add at the end rather than replace.
 * @return true when all of it is written; false with the error set.
 */
static bool text_put(const char *name, const char *path, const char *text, bool append)
{
    bool ok = false;

    if ( kode_enter(name) )
    {
        char vol[KODE_PATH_MAX];

        if ( NULL == text )
        {
            /* A bad path is the error that wins; only a good one reports the missing text. */
            if ( kode_file_path(path, vol, sizeof(vol)) )
            {
                kode_err_set("%s: no text to write", path);
            }
        }
        else
        {
            /* cppcheck-suppress misra-c2012-11.3 ; the text is its own bytes */
            ok = kode_file_put_bytes(path, (const uint8_t *)text, (uint32_t)strlen(text), append);
        }
    }
    return ok;
}

bool kode_file_write(const char *path, const char *text)
{
    return text_put("kode_file_write", path, text, false);
}

bool kode_file_append(const char *path, const char *text)
{
    return text_put("kode_file_append", path, text, true);
}

/** @brief What one list try carries. */
typedef struct {
    const char       *path;  /**< The volume path.     */
    kode_file_list_t *list;  /**< Where the entries go. */
} list_try_t;

/** @brief What one create-folder or delete try carries. */
typedef struct {
    const char *path;  /**< The volume path. */
} path_try_t;

/**
 * @brief Ask what a typed path is.
 * @param user The path as typed, for the error text.
 * @param vol  Receives the volume path.
 * @param cap  Bytes of @p vol.
 * @param st   Out: what storage said; all false when it said nothing.
 * @return true when storage answered; false with the error set.
 */
static bool file_stat(const char *user, char *vol, uint32_t cap, kode_sys_file_status_t *st)
{
    const bool ok = kode_file_path(user, vol, cap);

    st->exists = false;
    st->is_dir = false;
    st->size   = 0U;
    return ok && vol_stat(user, vol, st);
}

kode_file_status_t kode_file_status(const char *path)
{
    kode_file_status_t out = { false, false, 0U };

    if ( files_enter("kode_file_status") )
    {
        char                   vol[KODE_PATH_MAX];
        kode_sys_file_status_t st;

        if ( file_stat(path, vol, sizeof(vol), &st) && st.exists )
        {
            out.exists    = true;
            out.is_folder = st.is_dir;
            if ( !st.is_dir )
            {
                out.size = st.size;
            }
        }
        files_leave();
    }
    return out;
}

/** @brief Bytes in the kilobyte a volume's space is counted in. */
#define KODE_FILE_KB_BYTES 1024ULL

/** @brief What one space try carries. */
typedef struct {
    const char           *path;   /**< The volume's root.      */
    kode_sys_file_space_t space;  /**< What storage said.      */
} space_try_t;

/** @brief One count of a volume. */
static kode_status_t space_op(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the space_try_t the caller passed */
    space_try_t *const t = (space_try_t *)ctx;

    return kode_block_file_space(t->path, &t->space);
}

/**
 * @brief Whether a volume path is a volume's root and nothing inside it.
 * @param vol The volume path.
 * @return true for "/sdcard" and "/storage".
 */
static bool is_volume_root(const char *vol)
{
    return (strcmp(vol, KODE_FILE_SD_ROOT) == 0) || (strcmp(vol, KODE_FILE_FLASH_ROOT) == 0);
}

/**
 * @brief Count a volume named as typed. The caller holds the files lock.
 * @param user       The volume as typed, for the error text.
 * @param total      Out: the volume's bytes; both pointers are set to 0 first.
 * @param free_bytes Out: what is left of them.
 * @return true when counted; false with the error set.
 */
static bool space_run(const char *user, uint64_t *total, uint64_t *free_bytes)
{
    char vol[KODE_PATH_MAX];
    bool ok = kode_file_path(user, vol, sizeof(vol));

    if ( ok && !is_volume_root(vol) )
    {
        kode_err_set("'%s' is a path, not a volume: sd or flash", user);
        ok = false;
    }
    if ( ok && ((NULL == total) || (NULL == free_bytes)) )
    {
        kode_err_set("%s: no place for the sizes", user);
        ok = false;
    }
    if ( ok )
    {
        space_try_t         t  = { vol, { 0U, 0U } };
        const kode_status_t rc = kode_file_sys(&space_op, &t);

        if ( KODE_OK == rc )
        {
            *total      = (uint64_t)t.space.total_kb * KODE_FILE_KB_BYTES;
            *free_bytes = (uint64_t)t.space.free_kb * KODE_FILE_KB_BYTES;
        }
        else
        {
            kode_err_set("%s: %s", user, kode_file_why(rc));
            ok = false;
        }
    }
    return ok;
}

bool kode_file_space(const char *volume, uint64_t *total_bytes, uint64_t *free_bytes)
{
    bool ok = false;

    if ( NULL != total_bytes )
    {
        *total_bytes = 0U;
    }
    if ( NULL != free_bytes )
    {
        *free_bytes = 0U;
    }
    if ( files_enter("kode_file_space") )
    {
        ok = space_run(volume, total_bytes, free_bytes);
        files_leave();
    }
    return ok;
}

bool kode_file_exists(const char *path)
{
    bool found = false;

    if ( files_enter("kode_file_exists") )
    {
        char                   vol[KODE_PATH_MAX];
        kode_sys_file_status_t st;

        found = file_stat(path, vol, sizeof(vol), &st) && st.exists;
        files_leave();
    }
    return found;
}

/** @brief Copy @p name into @p out, cut to KODE_FILE_NAME_MAX with its NUL. */
static void name_copy(char *out, const char *name)
{
    uint32_t n = 0U;

    while ( ((KODE_FILE_NAME_MAX - 1U) > n) && ('\0' != name[n]) )
    {
        out[n] = name[n];
        n++;
    }
    out[n] = '\0';
}

/** @brief Keep one entry of a listing; stop when the list is full. */
static bool list_collect(const char *name, bool is_dir, uint32_t size, void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the kode_file_list_t list_op() passed */
    kode_file_list_t *const list = (kode_file_list_t *)ctx;
    const bool              room = (KODE_FILE_LIST_MAX > list->count);

    if ( room )
    {
        kode_file_entry_t *const e = &list->entries[list->count];

        name_copy(e->name, name);
        e->is_folder = is_dir;
        e->size      = 0U;
        if ( !is_dir )
        {
            e->size = size;
        }
        list->count++;
    }
    return room;
}

/** @brief One listing; a retry starts the list again. */
/* cppcheck-suppress constParameterCallback ; the signature is the one kode_file_sys() takes */
static kode_status_t list_op(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the list_try_t the caller passed */
    const list_try_t *const t = (const list_try_t *)ctx;

    (void)memset(t->list, 0, sizeof(*t->list));
    return kode_block_file_list_each(t->path, &list_collect, t->list);
}

/**
 * @brief Fill @p list with the entries of a folder.
 * @param user The folder as typed.
 * @param list Out: the entries; zeroed first.
 * @return true on success, `more` set when there were others; false with the
 *         error set.
 */
static bool list_fill(const char *user, kode_file_list_t *list)
{
    char                   vol[KODE_PATH_MAX];
    kode_sys_file_status_t st;
    bool                   ok = file_stat(user, vol, sizeof(vol), &st);

    (void)memset(list, 0, sizeof(*list));
    if ( ok )
    {
        if ( !st.exists )
        {
            kode_err_set("%s: no such folder", user);
            ok = false;
        }
        else if ( !st.is_dir )
        {
            kode_err_set("%s: it is a file", user);
            ok = false;
        }
        else
        {
            list_try_t          t  = { vol, list };
            const kode_status_t rc = kode_file_sys(&list_op, &t);

            /* The STAT above took the sys buffer: NO_RESOURCES here means more entries only. */
            if ( KODE_NO_RESOURCES == rc )
            {
                list->more = true;
            }
            else if ( KODE_OK != rc )
            {
                (void)memset(list, 0, sizeof(*list));
                kode_err_set("%s: %s", user, kode_file_why(rc));
                ok = false;
            }
            else
            {
                /* All of it. */
            }
        }
    }
    return ok;
}

const kode_file_list_t *kode_file_list(const char *folder)
{
    static const kode_file_list_t no_list = { 0U };  /* a failed listing */
    const kode_file_list_t       *list = &no_list;

    if ( files_enter("kode_file_list") )
    {
        struct kode_file_state *const s = kode_file_state_ensure();

        if ( (NULL != s) && (NULL == s->list) )
        {
            /* cppcheck-suppress misra-c2012-11.5 ; the allocator returns void *; the block is a kode_file_list_t */
            s->list = (kode_file_list_t *)khal_alloc_psram(sizeof(void *), sizeof(*s->list));
        }
        if ( (NULL == s) || (NULL == s->list) )
        {
            kode_err_set("%s: %s", path_shown(folder), kode_file_why(KODE_NO_RESOURCES));
        }
        else
        {
            (void)list_fill(folder, s->list);
            list = s->list;
        }
        files_leave();
    }
    return list;
}

/** @brief Whether @p vol is a volume itself, "/sdcard" or "/storage". */
static bool is_volume(const char *vol)
{
    return (strcmp(vol, KODE_FILE_SD_ROOT) == 0) || (strcmp(vol, KODE_FILE_FLASH_ROOT) == 0);
}

/** @brief One delete. */
/* cppcheck-suppress constParameterCallback ; the signature is the one kode_file_sys() takes */
static kode_status_t delete_op(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the path_try_t the caller passed */
    const path_try_t *const t = (const path_try_t *)ctx;

    return kode_block_file_delete(t->path);
}

/** @brief One create-folder. */
/* cppcheck-suppress constParameterCallback ; the signature is the one kode_file_sys() takes */
static kode_status_t mkdir_op(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the path_try_t the caller passed */
    const path_try_t *const t = (const path_try_t *)ctx;

    return kode_block_file_create_folder(t->path);
}

bool kode_file_delete(const char *path)
{
    bool ok = false;

    if ( files_enter("kode_file_delete") )
    {
        char                   vol[KODE_PATH_MAX];
        kode_sys_file_status_t st;

        if ( !kode_file_path(path, vol, sizeof(vol)) )
        {
            /* The error is set. */
        }
        else if ( is_volume(vol) )
        {
            kode_err_set("%s: a volume cannot be deleted", path);
        }
        else if ( vol_stat(path, vol, &st) )
        {
            if ( st.exists )
            {
                path_try_t          t  = { vol };
                const kode_status_t rc = kode_file_sys(&delete_op, &t);

                ok = (KODE_OK == rc);
                if ( !ok && st.is_dir && (KODE_ERROR == rc) )
                {
                    kode_err_set("%s: the folder is not empty", path);
                }
                else if ( !ok )
                {
                    kode_err_set("%s: %s", path, kode_file_why(rc));
                }
                else
                {
                    /* Gone. */
                }
            }
            else
            {
                kode_err_set("%s: no such file", path);
            }
        }
        else
        {
            /* The error is set. */
        }
        files_leave();
    }
    return ok;
}

/**
 * @brief Say why a folder could not be made.
 * @param user The path as typed.
 * @param vol  The volume path.
 * @param rc   What storage answered.
 * @return true when the folder is there after all; false with the error set.
 */
static bool mkdir_failed(const char *user, const char *vol, kode_status_t rc)
{
    bool there = false;

    if ( KODE_EXISTS == rc )
    {
        kode_sys_file_status_t st;

        if ( vol_stat(user, vol, &st) )
        {
            there = st.is_dir;
            if ( !there )
            {
                kode_err_set("%s: a file has that name", user);
            }
        }
    }
    else
    {
        create_failed(user, vol, rc);
    }
    return there;
}

bool kode_file_create_folder(const char *path)
{
    bool ok = false;

    if ( files_enter("kode_file_create_folder") )
    {
        char vol[KODE_PATH_MAX];

        if ( kode_file_path(path, vol, sizeof(vol)) )
        {
            path_try_t          t  = { vol };
            const kode_status_t rc = kode_file_sys(&mkdir_op, &t);

            ok = (KODE_OK == rc) || mkdir_failed(path, vol, rc);
        }
        files_leave();
    }
    return ok;
}

/** @brief What one rename try carries. */
typedef struct {
    const char *from;  /**< The volume path now.   */
    const char *to;    /**< The volume path after. */
} pair_try_t;

/** @brief One rename inside a volume. */
/* cppcheck-suppress constParameterCallback ; the signature is the one kode_file_sys() takes */
static kode_status_t rename_op(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the pair_try_t the caller passed */
    const pair_try_t *const t = (const pair_try_t *)ctx;

    return kode_block_file_rename_path(t->from, t->to);
}

/** @brief Characters of the volume word of @p vol, its first '/' included. */
static uint32_t root_len(const char *vol)
{
    uint32_t n = 1U;

    while ( ('\0' != vol[n]) && ('/' != vol[n]) )
    {
        n++;
    }
    return n;
}

/** @brief Whether two volume paths are on the same volume. */
static bool same_volume(const char *a, const char *b)
{
    const uint32_t n = root_len(a);

    return (root_len(b) == n) && (strncmp(a, b, n) == 0);
}

/** @brief Whether the first @p n characters of @p a and @p b match, ignoring ASCII case. */
static bool prefix_match_ci(const char *a, const char *b, uint32_t n)
{
    bool same = true;

    for ( uint32_t i = 0U; same && (i < n); i++ )
    {
        same = (kode_ascii_lower(a[i]) == kode_ascii_lower(b[i]));
    }
    return same;
}

/** @brief Whether @p a and @p b are the same string, ignoring ASCII case. */
static bool names_match_ci(const char *a, const char *b)
{
    const uint32_t n = (uint32_t)strlen(a);

    return (strlen(b) == n) && prefix_match_ci(a, b, n);
}

/** @brief Whether @p vol is @p folder or lies below it, ignoring ASCII case. */
static bool path_inside(const char *folder, const char *vol)
{
    const uint32_t n = (uint32_t)strlen(folder);

    return (strlen(vol) >= n) && prefix_match_ci(folder, vol, n)
           && (('\0' == vol[n]) || ('/' == vol[n]));
}

/**
 * @brief Rename inside one volume.
 * @param user The path as typed, for the error text.
 * @param vf   The volume path now.
 * @param vt   The volume path after.
 * @return true when renamed; false with the error set.
 */
static bool rename_same(const char *user, const char *vf, const char *vt)
{
    pair_try_t          t  = { vf, vt };
    const kode_status_t rc = kode_file_sys(&rename_op, &t);

    if ( KODE_OK != rc )
    {
        kode_err_set("%s: %s", user, kode_file_why(rc));
    }
    return KODE_OK == rc;
}

/**
 * @brief Whether @p name is one usable name.
 * @param name The new name.
 * @return true unless it is NULL, empty, "." or "..", or has a '/'.
 */
static bool name_ok(const char *name)
{
    bool ok = (NULL != name);

    if ( ok )
    {
        const uint32_t n = (uint32_t)strlen(name);

        ok = part_ok(name, n) && (strchr(name, '/') == NULL);
    }
    return ok;
}

/**
 * @brief Say if a path is free to be taken by a rename or a move.
 * @param user The path as typed, for the error text.
 * @param vol  Its volume path.
 * @return true when storage answered and nothing is there; false with the
 *         error set.
 */
static bool target_free(const char *user, const char *vol)
{
    kode_sys_file_status_t st;
    bool                   ok = vol_stat(user, vol, &st);

    if ( ok && st.exists )
    {
        kode_err_set("%s: already exists", user);
        ok = false;
    }
    return ok;
}

/**
 * @brief Say if a source is there, and what it is.
 * @param user The path as typed.
 * @param vol  Its volume path.
 * @param st   Out: what storage said.
 * @return true when it exists; false with the error set.
 */
static bool source_there(const char *user, const char *vol, kode_sys_file_status_t *st)
{
    bool ok = vol_stat(user, vol, st);

    if ( ok && !st->exists )
    {
        kode_err_set("%s: no such file", user);
        ok = false;
    }
    return ok;
}

/**
 * @brief Characters a typed path keeps when its last name is replaced.
 * @param user A typed path that names something inside a volume.
 * @return The folder part and its "/": "flash/dir/a" keeps 10.
 */
static uint32_t typed_keep(const char *user)
{
    uint32_t n = (uint32_t)strlen(user);

    if ( (0U < n) && ('/' == user[n - 1U]) )
    {
        n--;
    }
    while ( (0U < n) && ('/' != user[n - 1U]) )
    {
        n--;
    }
    return n;
}

bool kode_file_rename(const char *path, const char *new_name)
{
    bool ok = false;

    if ( files_enter("kode_file_rename") )
    {
        char                   vol[KODE_PATH_MAX];
        char                   dst[KODE_PATH_MAX];
        kode_sys_file_status_t st;

        if ( !kode_file_path(path, vol, sizeof(vol)) )
        {
            /* The error is set. */
        }
        else if ( is_volume(vol) )
        {
            kode_err_set("%s: a volume cannot be renamed", path);
        }
        else if ( !name_ok(new_name) )
        {
            kode_err_set("bad name '%s'", path_shown(new_name));
        }
        else
        {
            const uint32_t keep = parent_len(vol) + 1U;
            const uint32_t len  = (uint32_t)strlen(new_name);

            /* The typed path must fit too: it is the one the caller types again. The
             * volume path then fits dst, by the assert at the top. */
            if ( (typed_keep(path) + len + 1U) > KODE_FILE_PATH_MAX )
            {
                kode_err_set("bad name '%s': the path would be too long", new_name);
            }
            else
            {
                (void)memcpy(dst, vol, keep);
                (void)memcpy(&dst[keep], new_name, len + 1U);
                /* A change of case only names the entry that is there already. */
                if ( source_there(path, vol, &st)
                    && (names_match_ci(&vol[keep], new_name) || target_free(new_name, dst)) )
                {
                    ok = rename_same(path, vol, dst);
                }
            }
        }
        files_leave();
    }
    return ok;
}

/** @brief Delete a path that may not be there, and say nothing. */
static void discard(const char *vol)
{
    stat_try_t t = { vol, { false, false, 0U } };

    if ( (kode_file_sys(&stat_op, &t) == KODE_OK) && t.st.exists )
    {
        path_try_t d = { vol };

        (void)kode_file_sys(&delete_op, &d);
    }
}

/** @brief The walk's sink for a move: write the window onto the copy. */
static bool copy_sink(const walk_t *w, uint32_t got, uint32_t offset)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the sink's context is the copy's volume path */
    const char *const vt = (const char *)w->sink_ctx;
    write_try_t         t  = { vt, w->buf, got, 0U < offset };
    const kode_status_t rc = kode_file_sys(&write_op, &t);

    if ( KODE_OK != rc )
    {
        kode_err_set("%s: move failed while writing the copy: %s", w->user, kode_file_why(rc));
    }
    return KODE_OK == rc;
}

/**
 * @brief Move a file to the other volume: copy, then delete the original.
 * @param from The source as typed.
 * @param vf   The source's volume path.
 * @param vt   The destination's volume path.
 * @param size The source's size.
 * @return true when the copy is there and the original is gone; false with the
 *         error set, the copy removed and the original kept.
 */
static bool copy_across(const char *from, const char *vf, const char *vt, uint32_t size)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the allocator returns void *; the block holds bytes */
    uint8_t *const buf = (uint8_t *)khal_alloc_psram(sizeof(void *), KODE_SYS_FILE_READ_MAX);
    bool           ok  = false;

    if ( NULL == buf )
    {
        kode_err_set("%s: %s", from, kode_file_why(KODE_NO_RESOURCES));
    }
    else
    {
        const walk_t w = { from, vf, size, buf, false, "move failed while reading: ", "moved",
                           &copy_sink, vt };

        ok = walk_file(&w);
        khal_free(buf);
        if ( ok )
        {
            path_try_t          d  = { vf };
            const kode_status_t rc = kode_file_sys(&delete_op, &d);

            if ( KODE_OK != rc )
            {
                kode_err_set("%s: move failed while removing the original: %s", from,
                             kode_file_why(rc));
                ok = false;
            }
        }
        if ( !ok )
        {
            discard(vt);
        }
    }
    return ok;
}

bool kode_file_move(const char *from, const char *to)
{
    bool ok = false;

    if ( files_enter("kode_file_move") )
    {
        char                   vf[KODE_PATH_MAX];
        char                   vt[KODE_PATH_MAX];
        kode_sys_file_status_t st;

        if ( !kode_file_path(from, vf, sizeof(vf)) || !kode_file_path(to, vt, sizeof(vt)) )
        {
            /* The error is set. */
        }
        else if ( is_volume(vf) )
        {
            kode_err_set("%s: a volume cannot be moved", from);
        }
        else if ( source_there(from, vf, &st) )
        {
            const bool same   = same_volume(vf, vt);
            /* A change of case only names the entry that is there already. */
            const bool recase = same && (strcmp(vf, vt) != 0) && names_match_ci(vf, vt);

            if ( !same && st.is_dir )
            {
                kode_err_set("%s: only files move between sd/ and flash/", from);
            }
            else if ( !recase && same && st.is_dir && path_inside(vf, vt) )
            {
                kode_err_set("%s: a folder cannot move into itself", from);
            }
            else if ( recase || (target_free(to, vt) && parent_ready(to, vt)) )
            {
                if ( same )
                {
                    ok = rename_same(from, vf, vt);
                }
                else
                {
                    ok = copy_across(from, vf, vt, st.size);
                }
            }
            else
            {
                /* The error is set. */
            }
        }
        else
        {
            /* The error is set. */
        }
        files_leave();
    }
    return ok;
}
