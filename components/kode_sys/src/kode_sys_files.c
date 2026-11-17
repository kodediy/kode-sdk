/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_files.c
 * @brief The files through AO_Storage: the requests, and the blocking calls
 *        behind one claim, a stale answer waited out.
 */
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kode_sys_priv.h"

#include "kaos.h"
#include "khal_sync.h"
#include "khal_system.h"

#include <string.h>

/*
 * ---- Files state ----
 * The generation guards the answer; s_fs_stale guards the buffers. A call that
 * times out leaves AO_Storage holding pointers into s_fs_wait, s_fs_buf and
 * s_fs_rows, so fs_await() latches its generation and fs_enter() makes the next
 * caller wait that generation out before touching them.
 */
static khal_sem_t   s_fs_sem;    /**< Every blocking file call sleeps here.              */
static khal_mutex_t s_fs_lock;   /**< Guards s_fs_busy, s_fs_gen and s_fs_stale.         */
static bool         s_fs_busy;   /**< A file call holds the claim.                       */
static uint32_t     s_fs_gen;    /**< Tags requests apart; never 0.                      */
static uint32_t     s_fs_stale;  /**< The generation of a request left unanswered, or 0. */

_Static_assert(KODE_SYS_FILE_READ_MAX <= KODE_FS_CAT_MAX_CEIL,
               "a window is more than AO_Storage reads in one pass");

/** @brief The bytes of a read, or of a write; PSRAM, taken by fs_enter() once. */
static char *s_fs_buf;

_Static_assert(KODE_SYS_FILE_LIST_MAX == KODE_STORAGE_LIST_MAX,
               "kode_sys.h's listing ceiling and AO_Storage's have drifted apart");

/** @brief Where a listing lands before any callback runs. */
static StorageRow s_fs_rows[KODE_SYS_FILE_LIST_MAX];

/** @brief Where AO_Storage writes its answers; @c done is set by kode_sys_files_init(), @c text by fs_enter(). */
static StorageWaiter s_fs_wait = { .rows = s_fs_rows };

/** @brief How long a blocking file call waits for AO_Storage: a liveness bound, past the worst SD mount. */
#define KODE_FS_WAIT_MS  5000U

void kode_sys_files_init(void)
{
    s_fs_sem       = khal_sem_create();
    s_fs_lock      = khal_mutex_create();
    s_fs_busy      = false;
    s_fs_gen       = 0U;
    s_fs_stale     = 0U;
    s_fs_wait.done = s_fs_sem;
    s_fs_wait.ack  = 0U;
}

/*
 * ---- Files, blocking ----
 * Each kode_block_file_* call takes the shared claim, draws a generation, posts one request to
 * AO_Storage naming s_fs_wait, sleeps on s_fs_sem, copies its answer out once
 * its generation came back, and releases.
 */

/** @brief Give the shared claim back. */
static void fs_release(void)
{
    khal_mutex_lock(s_fs_lock);
    s_fs_busy = false;
    khal_mutex_unlock(s_fs_lock);
}

/**
 * @brief Take the shared claim, and wait out any request an earlier caller abandoned.
 *
 * The abandoned request's generation is latched in s_fs_stale: AO_Storage may
 * still write into s_fs_wait, s_fs_buf and s_fs_rows for it. A caller whose
 * predecessor did not time out pays nothing. When the stale answer never lands
 * this refuses with KODE_DENIED, nothing having been posted.
 * @return KODE_OK when the claim is held and fs_release() is owed; KODE_DENIED
 *         or KODE_NO_RESOURCES (no PSRAM for the buffer) when it is not.
 */
static kode_status_t fs_enter(void)
{
    uint32_t stale = 0U;

    khal_mutex_lock(s_fs_lock);
    if ( s_fs_busy )
    {
        khal_mutex_unlock(s_fs_lock);
        return KODE_DENIED;
    }
    s_fs_busy = true;
    stale     = s_fs_stale;
    khal_mutex_unlock(s_fs_lock);

    /* Kept for the boot: an abandoned request holds a pointer into it. */
    if ( NULL == s_fs_buf )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; khal_alloc_psram() answers void *: PSRAM is taken by name (rule 8) */
        s_fs_buf = (char *)khal_alloc_psram(sizeof(uint32_t), (size_t)KODE_SYS_FILE_READ_MAX);
        if ( NULL == s_fs_buf )
        {
            fs_release();
            return KODE_NO_RESOURCES;
        }
        s_fs_wait.text = s_fs_buf;
    }

    if ( 0U == stale )
    {
        return KODE_OK;
    }
    if ( !kode_sys_answer_await(s_fs_sem, &s_fs_wait.ack, stale, KODE_FS_WAIT_MS) )
    {
        /* Still owed: the latch stays, and holding the claim would add nothing. */
        fs_release();
        return KODE_DENIED;
    }

    khal_mutex_lock(s_fs_lock);
    s_fs_stale = 0U;
    khal_mutex_unlock(s_fs_lock);
    return KODE_OK;
}

/**
 * @brief Draw the tag for one request; the calls that ask AO_Storage twice give each question its own.
 * @return The next generation, never zero.
 */
static uint32_t fs_next_gen(void)
{
    khal_mutex_lock(s_fs_lock);
    s_fs_gen += 1U;
    if ( 0U == s_fs_gen )
    {
        s_fs_gen = 1U;
    }
    uint32_t const gen = s_fs_gen;
    khal_mutex_unlock(s_fs_lock);
    return gen;
}

/**
 * @brief Sleep until the answer to @p gen arrives, or KODE_FS_WAIT_MS runs out.
 *
 * Giving up latches @p gen in s_fs_stale, so fs_enter() makes the next caller
 * wait it out before reusing the storage AO_Storage may still write.
 * @param gen The generation this caller's request carried.
 * @return KODE_TIMEOUT when nothing this caller asked for came back, else AO_Storage's s_fs_wait.rc.
 */
static kode_status_t fs_await(uint32_t gen)
{
    if ( !kode_sys_answer_await(s_fs_sem, &s_fs_wait.ack, gen, KODE_FS_WAIT_MS) )
    {
        khal_mutex_lock(s_fs_lock);
        s_fs_stale = gen;
        khal_mutex_unlock(s_fs_lock);
        return KODE_TIMEOUT;
    }
    return (kode_status_t)s_fs_wait.rc;
}

/**
 * @brief Ask AO_Storage what is at @p path, with the claim already held.
 * @param path Absolute, virtual.
 * @param[out] out Filled on KODE_OK, as kode_block_file_status() fills it.
 * @return As kode_block_file_status().
 */
static kode_status_t fs_stat_locked(const char *path, kode_sys_file_status_t *out)
{
    uint32_t const gen = fs_next_gen();

    storage_path_request_wait(&s_fs_wait, gen, (uint8_t)STORAGE_PATH_STAT,
                              path);

    kode_status_t const rc = fs_await(gen);
    if ( KODE_OK == rc )
    {
        out->exists = s_fs_wait.exists;
        out->is_dir = s_fs_wait.is_dir;
        out->size   = s_fs_wait.size;
    }
    return rc;
}

/**
 * @brief The body of mkdir, remove and rename: one path-shaped request whose whole answer is its status.
 * @param op   storage_path_op_t.
 * @param path Absolute, virtual.
 * @return As the calling public function.
 */
static kode_status_t fs_path_op_locked(uint8_t op, const char *path)
{
    uint32_t const gen = fs_next_gen();

    storage_path_request_wait(&s_fs_wait, gen, op, path);
    return fs_await(gen);
}

kode_status_t kode_block_file_status(const char *path, kode_sys_file_status_t *out)
{
    if ( (NULL == path) || (NULL == out) )
    {
        return KODE_ERROR;
    }
    kode_status_t const claimed = fs_enter();
    if ( KODE_OK != claimed )
    {
        return claimed;
    }

    kode_status_t const rc = fs_stat_locked(path, out);
    fs_release();
    return rc;
}

kode_status_t kode_block_file_space(const char *path, kode_sys_file_space_t *out)
{
    if ( (NULL == path) || (NULL == out) )
    {
        return KODE_ERROR;
    }
    kode_status_t const claimed = fs_enter();
    if ( KODE_OK != claimed )
    {
        return claimed;
    }

    uint32_t const gen = fs_next_gen();

    storage_path_request_wait(&s_fs_wait, gen, (uint8_t)STORAGE_PATH_SPACE, path);

    kode_status_t const rc = fs_await(gen);
    if ( KODE_OK == rc )
    {
        out->total_kb = s_fs_wait.total_kb;
        out->free_kb  = s_fs_wait.free_kb;
    }
    fs_release();
    return rc;
}

kode_status_t kode_block_file_list_each(const char *path, uint32_t skip, kode_sys_file_entry_cb cb,
                                        void *ctx)
{
    kode_sys_file_status_t st;

    if ( (NULL == path) || (NULL == cb) )
    {
        return KODE_ERROR;
    }
    kode_status_t const claimed = fs_enter();
    if ( KODE_OK != claimed )
    {
        return claimed;
    }

    kode_status_t rc = fs_stat_locked(path, &st);
    if ( KODE_OK != rc )
    {
        fs_release();
        return rc;
    }
    if ( !st.exists || !st.is_dir )
    {
        /* Refuses as `cd` does: AO_Storage's list terminator carries no status, so a missing path would read as empty. */
        fs_release();
        return KODE_DENIED;
    }

    s_fs_wait.cap = (uint32_t)KODE_SYS_FILE_LIST_MAX;
    s_fs_wait.got = 0U;
    s_fs_wait.cut = false;

    uint32_t const gen = fs_next_gen();
    storage_list_request_wait(&s_fs_wait, gen, path, skip);

    rc = fs_await(gen);
    if ( KODE_OK != rc )
    {
        fs_release();
        return rc;
    }

    /* The claim is held across the callbacks: s_fs_rows must not be reused while they read it. */
    uint32_t const rows = s_fs_wait.got;
    for ( uint32_t i = 0U; i < rows; ++i )
    {
        if ( !cb(s_fs_rows[i].name, s_fs_rows[i].is_dir, s_fs_rows[i].size,
                ctx) )
        {
            break;
        }
    }

    bool const cut = s_fs_wait.cut;
    fs_release();
    if ( cut )
    {
        /* Every row that fitted was offered; say the rest was cut instead of swallowing it. */
        return KODE_NO_RESOURCES;
    }
    return KODE_OK;
}

kode_status_t kode_block_file_create_folder(const char *path)
{
    if ( NULL == path )
    {
        return KODE_ERROR;
    }
    kode_status_t const claimed = fs_enter();
    if ( KODE_OK != claimed )
    {
        return claimed;
    }

    kode_status_t const rc =
        fs_path_op_locked((uint8_t)STORAGE_PATH_MKDIR, path);
    fs_release();
    return rc;
}

kode_status_t kode_block_file_delete(const char *path)
{
    kode_sys_file_status_t st;

    if ( NULL == path )
    {
        return KODE_ERROR;
    }
    kode_status_t const claimed = fs_enter();
    if ( KODE_OK != claimed )
    {
        return claimed;
    }

    /* STAT first: FAT has one verb for a file and another for a directory (and rmdir refuses a non-empty one). */
    kode_status_t rc = fs_stat_locked(path, &st);
    if ( KODE_OK != rc )
    {
        fs_release();
        return rc;
    }

    uint8_t op = (uint8_t)STORAGE_PATH_UNLINK;
    if ( st.exists && st.is_dir )
    {
        op = (uint8_t)STORAGE_PATH_RMDIR;
    }

    rc = fs_path_op_locked(op, path);
    fs_release();
    return rc;
}

kode_status_t kode_block_file_rename_path(const char *from, const char *to)
{
    if ( (NULL == from) || (NULL == to) )
    {
        return KODE_ERROR;
    }
    kode_status_t const claimed = fs_enter();
    if ( KODE_OK != claimed )
    {
        return claimed;
    }

    uint32_t const gen = fs_next_gen();
    storage_pair_request_wait(&s_fs_wait, gen, (uint8_t)STORAGE_PAIR_MOVE,
                              from, to);

    kode_status_t const rc = fs_await(gen);
    fs_release();
    return rc;
}

kode_status_t kode_block_file_read_part(const char *path, uint32_t offset, void *buf,
                                        uint32_t cap, uint32_t *got)
{
    if ( (NULL == path) || (NULL == buf) || (0U == cap) )
    {
        return KODE_ERROR;
    }
    if ( (uint32_t)KODE_SYS_FILE_READ_MAX < cap )
    {
        /* A programmer's error: @p got below @p cap must keep meaning the end. */
        return KODE_ERROR;
    }
    kode_status_t const claimed = fs_enter();
    if ( KODE_OK != claimed )
    {
        return claimed;
    }

    s_fs_wait.cap = cap;
    s_fs_wait.got = 0U;
    s_fs_wait.cut = false;

    uint32_t const gen = fs_next_gen();
    storage_cat_request_wait(&s_fs_wait, gen, path, offset, cap);

    kode_status_t const rc = fs_await(gen);
    if ( KODE_OK != rc )
    {
        fs_release();
        return rc;
    }

    (void)memcpy(buf, s_fs_buf, (size_t)s_fs_wait.got);
    if ( NULL != got )
    {
        *got = s_fs_wait.got;
    }
    fs_release();
    return KODE_OK;
}

kode_status_t kode_block_file_write_buffer(const char *path, const void *data, uint32_t len,
                            bool append)
{
    if ( (NULL == path) || ((NULL == data) && (0U < len)) )
    {
        return KODE_ERROR;
    }
    if ( (uint32_t)KODE_SYS_FILE_READ_MAX < len )
    {
        /* Refused whole, not cut to fit. */
        return KODE_NO_RESOURCES;
    }

    kode_status_t const claimed = fs_enter();
    if ( KODE_OK != claimed )
    {
        return claimed;
    }

    uint32_t const gen = fs_next_gen();

    /* Copied into s_fs_buf, which outlives the caller's frame; safe because fs_enter() waited out any abandoned request. */
    if ( 0U < len )
    {
        (void)memcpy(s_fs_buf, data, (size_t)len);
    }

    const void *payload = NULL;
    if ( 0U < len )
    {
        payload = s_fs_buf;
    }
    storage_text_request_wait(&s_fs_wait, gen, path, payload, len, append);

    kode_status_t const rc = fs_await(gen);
    fs_release();
    return rc;
}

/* ---- kode_sys's requests: AO_Storage answers the asking AO ---- */

_Static_assert(KODE_SYS_FILE_PATH_MAX == KODE_PATH_MAX,
               "a path the facade takes must be one a request carries whole");
_Static_assert(KODE_SYS_FILE_READ_MAX <= UINT16_MAX,
               "a write the facade takes must be one a request counts");

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_file_status(QActive *me, const char *path)
{
    if ( !kode_sys_text_fits(path, KODE_SYS_FILE_PATH_MAX) )
    {
        return KODE_DENIED;
    }
    return storage_stat_ask(me, path);
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_file_space(QActive *me, const char *volume)
{
    if ( !kode_sys_text_fits(volume, KODE_SYS_FILE_PATH_MAX) )
    {
        return KODE_DENIED;
    }
    return storage_space_ask(me, volume);
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_file_list(QActive *me, const char *path, uint16_t skip)
{
    if ( !kode_sys_text_fits(path, KODE_SYS_FILE_PATH_MAX) )
    {
        return KODE_DENIED;
    }
    return storage_list_ask(me, path, skip);
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_file_create_folder(QActive *me, const char *path)
{
    if ( !kode_sys_text_fits(path, KODE_SYS_FILE_PATH_MAX) )
    {
        return KODE_DENIED;
    }
    return storage_mkdir_ask(me, path);
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_file_delete(QActive *me, const char *path)
{
    if ( !kode_sys_text_fits(path, KODE_SYS_FILE_PATH_MAX) )
    {
        return KODE_DENIED;
    }
    return storage_delete_ask(me, path);
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_file_rename(QActive *me, const char *from, const char *to)
{
    if ( !kode_sys_text_fits(from, KODE_SYS_FILE_PATH_MAX)
        || !kode_sys_text_fits(to, KODE_SYS_FILE_PATH_MAX) )
    {
        return KODE_DENIED;
    }
    return storage_rename_ask(me, from, to);
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_file_read(QActive *me, const char *path, uint32_t offset, void *buf,
                                 uint32_t cap)
{
    if ( !kode_sys_text_fits(path, KODE_SYS_FILE_PATH_MAX) || (NULL == buf) || (0U == cap)
        || ((uint32_t)KODE_SYS_FILE_READ_MAX < cap) )
    {
        return KODE_DENIED;
    }
    return storage_read_ask(me, path, offset, buf, cap);
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_file_write(QActive *me, const char *path, const void *data, uint32_t len,
                                  bool append)
{
    if ( !kode_sys_text_fits(path, KODE_SYS_FILE_PATH_MAX) || ((NULL == data) && (0U < len))
        || ((uint32_t)KODE_SYS_FILE_READ_MAX < len) )
    {
        return KODE_DENIED;
    }
    return storage_write_ask(me, path, data, len, append);
}
