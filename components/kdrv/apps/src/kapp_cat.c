/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kapp_cat.c
 * @brief The category list: splitting, matching and normalising one, plus
 *        the live override table that sits behind NVS and a lock.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "kapp_cat.h"
#include "khal_nvs.h"
#include "khal_sync.h"
#include "khal_system.h"
#include "kode_log.h"

#define TAG "kdrv.cat"

const char *kapp_cat_next(const char *cursor, char *item, size_t len)
{
    size_t n = 0U;

    if ( (NULL == cursor) || (NULL == item) || (0U == len) )
    {
        return NULL;
    }
    while ( ',' == *cursor )
    {       /* an empty item is not an item */
        ++cursor;
    }
    if ( '\0' == *cursor )
    {
        return NULL;
    }
    while ( ('\0' != *cursor) && (',' != *cursor) )
    {
        if ( (len - 1U) > n )
        {
            item[n] = *cursor;
            ++n;
        }
        ++cursor;
    }
    item[n] = '\0';
    return cursor;
}

bool kapp_cat_has(const char *list, const char *cat)
{
    char        item[KAPP_CATEGORY_ITEM_MAX];
    const char *c = list;

    if ( (NULL == list) || (NULL == cat) || ('\0' == cat[0]) )
    {
        return false;
    }
    while ( (c = kapp_cat_next(c, item, sizeof(item))) != NULL )
    {
        if ( strcmp(item, cat) == 0 )
        {
            return true;
        }
    }
    return false;
}

void kapp_cat_normalise(const char *in, char *out, size_t len)
{
    /* Wider than one item on purpose: an over-long item must be detected
     * and dropped, and a buffer that truncates it cannot tell the
     * difference between "too long" and "exactly at the limit". */
    char        item[KAPP_CATEGORY_MAX];
    const char *c    = in;
    size_t      used = 0U;
    uint8_t     kept = 0U;

    if ( (NULL == out) || (0U == len) )
    {
        return;
    }
    out[0] = '\0';
    if ( NULL == in )
    {
        return;
    }

    while ( (c = kapp_cat_next(c, item, sizeof(item))) != NULL )
    {
        char  *s = item;
        size_t n;

        while ( ' ' == *s )
        {
            ++s;
        }
        n = strlen(s);
        while ( (0U < n) && (' ' == s[n - 1U]) )
        {
            s[n - 1U] = '\0';
            --n;
        }
        if ( 0U == n )
        {
            continue;
        }
        if ( KAPP_CATEGORY_ITEM_MAX <= n )
        {
            KLOG_W(TAG, "category '%s' is too long; dropped", s);
            continue;
        }
        for ( size_t i = 0U; i < n; ++i )
        {
            s[i] = (char)tolower((unsigned char)s[i]);
        }
        if ( kapp_cat_has(out, s) )
        {
            continue;
        }
        if ( KAPP_CATEGORIES_MAX <= kept )
        {
            KLOG_W(TAG, "more than %u categories; '%s' dropped",
                   (unsigned)KAPP_CATEGORIES_MAX, s);
            continue;
        }
        if ( (used + n + ((0U < kept) ? 1U : 0U)) >= len )
        {
            break;
        }
        if ( 0U < kept )
        {
            out[used] = ',';
            ++used;
        }
        (void)memcpy(&out[used], s, n);
        used += n;
        out[used] = '\0';
        ++kept;
    }
}

/**
 * @brief The blob's header. Written and read field by field rather than as
 *        a struct, so no padding assumption travels to flash.
 */
#define HDR_LEN 12U

size_t kapp_cat_serialise(const kapp_cat_row_t *rows, uint8_t count,
                          uint8_t *buf, size_t len)
{
    size_t const need = HDR_LEN + ((size_t)count * sizeof(kapp_cat_row_t));
    uint32_t const magic = KAPP_CAT_BLOB_MAGIC;
    uint32_t const ver   = KAPP_CAT_BLOB_VERSION;
    uint32_t const n     = (uint32_t)count;

    if ( (NULL == buf) || (KAPP_CAT_ROWS_MAX < count) || (len < need)
        || ((NULL == rows) && (0U < count)) )
    {
        return 0U;
    }
    (void)memcpy(&buf[0], &magic, 4U);
    (void)memcpy(&buf[4], &ver,   4U);
    (void)memcpy(&buf[8], &n,     4U);
    if ( 0U < count )
    {
        (void)memcpy(&buf[HDR_LEN], rows,
                     (size_t)count * sizeof(kapp_cat_row_t));
    }
    return need;
}

kode_status_t kapp_cat_deserialise(const uint8_t *buf, size_t len,
                                   kapp_cat_row_t *rows, uint8_t *count)
{
    uint32_t magic;
    uint32_t ver;
    uint32_t n;

    if ( (NULL == buf) || (NULL == rows) || (NULL == count)
        || (HDR_LEN > len) )
    {
        return KODE_ERROR;
    }
    (void)memcpy(&magic, &buf[0], 4U);
    (void)memcpy(&ver,   &buf[4], 4U);
    (void)memcpy(&n,     &buf[8], 4U);

    if ( (KAPP_CAT_BLOB_MAGIC != magic) || (KAPP_CAT_BLOB_VERSION != ver)
        || (KAPP_CAT_ROWS_MAX < n)
        || ((HDR_LEN + ((size_t)n * sizeof(kapp_cat_row_t))) > len) )
    {
        return KODE_ERROR;
    }
    /* Header and length add up, but the row bytes themselves are still
     * untrusted flash content: a raw memcpy of an unterminated project[] or
     * category[] would let cat_find()'s strcmp and kapp_cat_resolve()'s
     * snprintf read past s_rows[] into whatever .bss follows it. Check
     * every row before touching either output - all-or-nothing, same as
     * the header checks above. */
    for ( uint32_t i = 0U; i < n; ++i )
    {
        const uint8_t *row = &buf[HDR_LEN + ((size_t)i * sizeof(kapp_cat_row_t))];

        if ( (memchr(row, '\0', sizeof(rows[0].project)) == NULL)
            || (memchr(&row[sizeof(rows[0].project)], '\0',
                       sizeof(rows[0].category)) == NULL) )
        {
            return KODE_ERROR;
        }
    }
    if ( 0U < n )
    {
        (void)memcpy(rows, &buf[HDR_LEN],
                     (size_t)n * sizeof(kapp_cat_row_t));
    }
    /* Normalise every row's category on the way in, so kapp_cat_has()'s
     * documented precondition (an already-normalised list) holds no matter
     * what actually made it into flash - this is the only path by which a
     * non-normalised list could otherwise reach it. */
    for ( uint32_t i = 0U; i < n; ++i )
    {
        char clean[KAPP_CATEGORY_MAX];

        kapp_cat_normalise(rows[i].category, clean, sizeof(clean));
        (void)snprintf(rows[i].category, sizeof(rows[i].category), "%s", clean);
    }
    *count = (uint8_t)n;
    return KODE_OK;
}

/* ---- The live table: RAM rows, a lock, and NVS behind both --------------- */

#define CAT_NS   "kode"
#define KAPP_CAT_ALIGN  4U   /**< PSRAM alignment of the two buffers: a word. */
#define CAT_KEY  "app_cats"

/* The rows, KAPP_CAT_ROWS_MAX of them, in PSRAM: taken once by
 * kapp_cat_init() and never freed. Nothing reads them from an interrupt or
 * with the cache off, so internal RAM would buy nothing. */
static kapp_cat_row_t *s_rows;
static uint8_t        s_count;
static khal_mutex_t   s_lock;

/*
 * One buffer, not one per caller. AO_AppMgr is this table's only writer by
 * design, and it is worth knowing that the ownership table in kbsp does not
 * say so: a claim there records who *should* write a resource and refuses a
 * second claimant of the name, but it cannot stop anything that links this
 * file from calling kapp_cat_set()/kapp_cat_clear() directly - and two
 * callers really do reach here today: AO_AppMgr through APP_CAT_REQ_SIG,
 * and the `apps` self-test (test_apps_catlive(), run from AO_Autotest at
 * priority 10, which preempts AO_AppMgr at priority 9) exercising this
 * file's contract on the board with real NVS. So this is not a resource
 * with one owner at all, whatever a claim would have printed; s_persist_lock
 * below is the whole protection.
 *
 * The hazard the lock closes: each cat_persist() serialises the table into
 * this static, then writes s_blob to NVS with its own captured length. Two
 * callers interleaved there do not just overwrite each other with something
 * stale-but-consistent - the second's serialise() can splice into the bytes
 * the first is still handing to khal_nvs_set_blob(), corrupting that write
 * into a length/content mismatch.
 */
static uint8_t *s_blob;   /* KAPP_CAT_BLOB_LEN bytes of PSRAM, as s_rows. */

/**
 * @brief Guards the whole of cat_persist(), s_blob included.
 *
 * Separate from s_lock, and taken outermost: s_lock's job is keeping a
 * *reader* (AO_Storage, resolving a category mid-parse) off a table being
 * updated, for the microseconds a row copy takes, and it is released
 * before any flash access - a reader must never wait out a flash erase.
 * s_persist_lock's job is different: it serialises the two *writers*
 * across the whole commit, s_blob included, because that buffer is only
 * safe with exactly one writer inside it at a time. Lock order is fixed -
 * s_persist_lock first, always, s_lock only strictly inside it - or the
 * two could wait on each other.
 */
static khal_mutex_t s_persist_lock;

/**
 * @brief Serialise the table and write it to NVS, one writer at a time.
 *
 * s_persist_lock covers the whole call, s_blob included, because two
 * writers racing through it can splice one's serialise() into the bytes
 * the other is still handing to khal_nvs_set_blob() - see the note there
 * and by s_persist_lock's declaration. s_lock is still taken only for the
 * row copy, and released before khal_nvs_set_blob() runs: holding it
 * across the write would park AO_Storage for the length of a flash erase
 * every time a category is assigned.
 */
static kode_status_t cat_persist(void)
{
    size_t        n;
    kode_status_t st;

    khal_mutex_lock(s_persist_lock);

    khal_mutex_lock(s_lock);
    n = kapp_cat_serialise(s_rows, s_count, s_blob, KAPP_CAT_BLOB_LEN);
    khal_mutex_unlock(s_lock);

    st = (0U == n) ? KODE_ERROR
                   : khal_nvs_set_blob(CAT_NS, CAT_KEY, s_blob, n);

    khal_mutex_unlock(s_persist_lock);
    return st;
}

/** @brief Index of @p project in the table, or -1. Caller holds the lock. */
static int cat_find(const char *project)
{
    for ( uint8_t i = 0U; i < s_count; ++i )
    {
        if ( strcmp(s_rows[i].project, project) == 0 )
        {
            return (int)i;
        }
    }
    return -1;
}

kode_status_t kapp_cat_init(void)
{
    size_t        len = KAPP_CAT_BLOB_LEN;
    kode_status_t st;

    /*
     * No lock held below: kdrv_apps_init() calls this before any Active
     * Object task is running - QP/C executes every AO's initial transition
     * synchronously on the caller of QACTIVE_START(), which has not
     * happened yet this early in boot. kdrv_apps_init()'s own s_inited
     * guard is what stops a second call from reaching here once AOs do
     * exist, which is why this function never needs s_lock for its own
     * writes to s_rows/s_count/s_blob.
     */
    /* The two buffers first: a lock that exists says the table is usable,
     * and every accessor refuses while it does not. */
    if ( NULL == s_rows )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; khal_alloc_psram() hands back an untyped block aligned for any type */
        s_rows = (kapp_cat_row_t *)khal_alloc_psram(
            KAPP_CAT_ALIGN, (size_t)KAPP_CAT_ROWS_MAX * sizeof(kapp_cat_row_t));
    }
    if ( NULL == s_blob )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; khal_alloc_psram() hands back an untyped block aligned for any type */
        s_blob = (uint8_t *)khal_alloc_psram(KAPP_CAT_ALIGN, KAPP_CAT_BLOB_LEN);
    }
    if ( (NULL == s_rows) || (NULL == s_blob) )
    {
        KLOG_E(TAG, "no PSRAM for the category table; overrides are off");
        return KODE_ERROR;
    }
    if ( NULL == s_lock )
    {
        s_lock = khal_mutex_create();
        if ( NULL == s_lock )
        {
            KLOG_E(TAG, "no memory for the category lock");
            return KODE_ERROR;
        }
    }
    if ( NULL == s_persist_lock )
    {
        s_persist_lock = khal_mutex_create();
        if ( NULL == s_persist_lock )
        {
            KLOG_E(TAG, "no memory for the category persist lock");
            return KODE_ERROR;
        }
    }
    if ( khal_nvs_init() != KODE_OK )
    {
        KLOG_W(TAG, "NVS is not available; overrides are off this boot");
        return KODE_OK;                 /* an empty table is still a table */
    }

    st = khal_nvs_get_blob(CAT_NS, CAT_KEY, s_blob, &len);
    if ( KODE_DENIED == st )
    {
        s_count = 0U;                   /* first boot: nothing stored yet */
        return KODE_OK;
    }
    if ( KODE_OK != st )
    {
        KLOG_W(TAG, "category table unreadable; starting empty");
        s_count = 0U;
        return KODE_OK;
    }
    if ( kapp_cat_deserialise(s_blob, len, s_rows, &s_count) != KODE_OK )
    {
        KLOG_E(TAG, "category table does not add up (%u bytes); discarded",
               (unsigned)len);
        s_count = 0U;
        return KODE_OK;
    }
    KLOG_I(TAG, "category overrides: %u", (unsigned)s_count);
    return KODE_OK;
}

bool kapp_cat_resolve(const char *project, char *cats, size_t len)
{
    bool found = false;
    int  idx;

    if ( (NULL == project) || (NULL == cats) || (0U == len)
        || (NULL == s_lock) )
    {
        return false;
    }
    khal_mutex_lock(s_lock);
    idx = cat_find(project);
    if ( 0 <= idx )
    {
        (void)snprintf(cats, len, "%s", s_rows[idx].category);
        found = true;
    }
    khal_mutex_unlock(s_lock);
    return found;
}

/*
 * Mutates s_rows/s_count first, persists after. When cat_persist() fails
 * (NVS full, flash error) the RAM table has already moved and stays ahead
 * of what is in flash until the next boot re-reads it - kapp_cat_resolve()
 * and kapp_cat_row() serve the new value in the meantime, even though the
 * caller is correctly handed back cat_persist()'s failure. Acceptable: the
 * console reports the failure honestly, and a reboot resyncs from flash.
 * Just not documented until now.
 */
kode_status_t kapp_cat_set(const char *project, const char *cats)
{
    char clean[KAPP_CATEGORY_MAX];
    int  idx;

    if ( (NULL == project) || ('\0' == project[0]) || (NULL == cats)
        || (NULL == s_lock) )
    {
        return KODE_ERROR;
    }
    kapp_cat_normalise(cats, clean, sizeof(clean));
    if ( '\0' == clean[0] )
    {
        return KODE_ERROR;              /* clearing is a different verb */
    }

    khal_mutex_lock(s_lock);
    idx = cat_find(project);
    if ( 0 > idx )
    {
        if ( KAPP_CAT_ROWS_MAX <= s_count )
        {
            khal_mutex_unlock(s_lock);
            return KODE_DENIED;
        }
        idx = (int)s_count;
        (void)snprintf(s_rows[idx].project, sizeof(s_rows[idx].project),
                       "%s", project);
        ++s_count;
    }
    (void)snprintf(s_rows[idx].category, sizeof(s_rows[idx].category),
                   "%s", clean);
    khal_mutex_unlock(s_lock);

    return cat_persist();
}

kode_status_t kapp_cat_clear(const char *project)
{
    int idx;

    if ( (NULL == project) || (NULL == s_lock) )
    {
        return KODE_ERROR;
    }
    khal_mutex_lock(s_lock);
    idx = cat_find(project);
    if ( 0 <= idx )
    {
        /* Close the gap: the table is a list, not a sparse array, and the
         * blob's count is what says how far it runs. */
        for ( uint8_t i = (uint8_t)idx; (i + 1U) < s_count; ++i )
        {
            s_rows[i] = s_rows[i + 1U];
        }
        --s_count;
    }
    khal_mutex_unlock(s_lock);

    return (0 <= idx) ? cat_persist() : KODE_OK;
}

uint8_t kapp_cat_count(void)
{
    uint8_t n;

    if ( NULL == s_lock )
    {
        return 0U;
    }
    khal_mutex_lock(s_lock);
    n = s_count;
    khal_mutex_unlock(s_lock);
    return n;
}

bool kapp_cat_row(uint8_t idx, char *project, size_t plen,
                  char *cats, size_t clen)
{
    bool ok = false;

    if ( (NULL == project) || (NULL == cats) || (NULL == s_lock) )
    {
        return false;
    }
    khal_mutex_lock(s_lock);
    if ( idx < s_count )
    {
        (void)snprintf(project, plen, "%s", s_rows[idx].project);
        (void)snprintf(cats, clen, "%s", s_rows[idx].category);
        ok = true;
    }
    khal_mutex_unlock(s_lock);
    return ok;
}
