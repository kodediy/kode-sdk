/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_khal_nvs.c
 * @brief An in-memory stand-in for khal_nvs.
 */
#include <stdio.h>
#include <string.h>

#include "fakes.h"
#include "khal_nvs.h"

#define FAKE_NVS_RECORDS  8U
#define FAKE_NVS_NAME_MAX 32U
#define FAKE_NVS_BLOB_MAX 8192U

typedef struct {
    bool    used;
    char    ns[FAKE_NVS_NAME_MAX];
    char    key[FAKE_NVS_NAME_MAX];
    uint8_t blob[FAKE_NVS_BLOB_MAX];
    size_t  len;
} fake_nvs_rec_t;

static fake_nvs_rec_t s_rec[FAKE_NVS_RECORDS];
static bool           s_fail_next_set;
static unsigned       s_sets;   /**< Writes asked for since the reset. */
static void         (*s_set_hook)(void); /**< Run inside every write, or NULL. */

/**
 * @brief Find a record by namespace and key.
 * @param ns  Namespace to match.
 * @param key Key to match.
 * @return Its slot, or NULL when there is none.
 */
static fake_nvs_rec_t *find(const char *ns, const char *key)
{
    for ( unsigned i = 0U; FAKE_NVS_RECORDS > i; ++i )
    {
        if ( s_rec[i].used && (strcmp(s_rec[i].ns, ns) == 0)
            && (strcmp(s_rec[i].key, key) == 0) )
        {
            return &s_rec[i];
        }
    }
    return NULL;
}

/**
 * @brief Take a free slot and name it.
 * @param ns  Namespace to record.
 * @param key Key to record.
 * @return The claimed slot, or NULL when the table is full.
 */
static fake_nvs_rec_t *claim(const char *ns, const char *key)
{
    for ( unsigned i = 0U; FAKE_NVS_RECORDS > i; ++i )
    {
        if ( !s_rec[i].used )
        {
            s_rec[i].used = true;
            (void)snprintf(s_rec[i].ns, sizeof(s_rec[i].ns), "%s", ns);
            (void)snprintf(s_rec[i].key, sizeof(s_rec[i].key), "%s", key);
            return &s_rec[i];
        }
    }
    return NULL;
}

void fake_nvs_reset(void)
{
    (void)memset(s_rec, 0, sizeof(s_rec));
    s_fail_next_set = false;
    s_sets          = 0U;
    s_set_hook      = NULL;
}

void fake_nvs_set_hook(void (*hook)(void))
{
    s_set_hook = hook;
}

unsigned fake_nvs_set_count(void)
{
    return s_sets;
}

void fake_nvs_seed(const char *ns, const char *key,
                   const void *blob, size_t len)
{
    bool const armed = s_fail_next_set;

    s_fail_next_set = false;
    (void)khal_nvs_set_blob(ns, key, blob, len);
    s_fail_next_set = armed;
}

void fake_nvs_fail_next_set(void)
{
    s_fail_next_set = true;
}

unsigned fake_nvs_count(void)
{
    unsigned n = 0U;

    for ( unsigned i = 0U; FAKE_NVS_RECORDS > i; ++i )
    {
        if ( s_rec[i].used )
        {
            ++n;
        }
    }
    return n;
}

kode_status_t khal_nvs_init(void)
{
    return KODE_OK;
}

kode_status_t khal_nvs_get_blob(const char *ns, const char *key,
                                void *out, size_t *len)
{
    const fake_nvs_rec_t *r = find(ns, key);

    if ( NULL == r )
    {
        return KODE_DENIED;
    }
    if ( (NULL != out) && (NULL != len) && (*len >= r->len) )
    {
        (void)memcpy(out, r->blob, r->len);
    }
    if ( NULL != len )
    {
        *len = r->len;
    }
    return KODE_OK;
}

kode_status_t khal_nvs_set_blob(const char *ns, const char *key,
                                const void *value, size_t len)
{
    fake_nvs_rec_t *r;

    s_sets += 1U;
    if ( NULL != s_set_hook )
    {
        s_set_hook();
    }
    if ( s_fail_next_set )
    {
        s_fail_next_set = false;
        return KODE_ERROR;
    }
    if ( (NULL == value) || (FAKE_NVS_BLOB_MAX < len) )
    {
        return KODE_ERROR;
    }

    r = find(ns, key);
    if ( NULL == r )
    {
        r = claim(ns, key);
    }
    if ( NULL == r )
    {
        return KODE_ERROR;
    }
    (void)memcpy(r->blob, value, len);
    r->len = len;
    return KODE_OK;
}

kode_status_t khal_nvs_get_str(const char *ns, const char *key,
                               char *out, size_t cap)
{
    const fake_nvs_rec_t *r = find(ns, key);

    if ( NULL == r )
    {
        return KODE_DENIED;
    }
    if ( (NULL == out) || (0U == cap) )
    {
        return KODE_ERROR;
    }
    (void)snprintf(out, cap, "%s", (const char *)r->blob);
    return KODE_OK;
}

kode_status_t khal_nvs_set_str(const char *ns, const char *key,
                               const char *value)
{
    if ( NULL == value )
    {
        return KODE_ERROR;
    }
    return khal_nvs_set_blob(ns, key, value, strlen(value) + 1U);
}

kode_status_t khal_nvs_get_u8(const char *ns, const char *key, uint8_t *out)
{
    const fake_nvs_rec_t *r = find(ns, key);

    if ( NULL == r )
    {
        return KODE_DENIED;
    }
    if ( (NULL == out) || (1U != r->len) )
    {
        return KODE_ERROR;
    }
    *out = r->blob[0];
    return KODE_OK;
}

kode_status_t khal_nvs_set_u8(const char *ns, const char *key, uint8_t value)
{
    return khal_nvs_set_blob(ns, key, &value, 1U);
}

/* A u64 is kept as its eight bytes: the fake has one kind of record. */
kode_status_t khal_nvs_get_u64(const char *ns, const char *key, uint64_t *out)
{
    const fake_nvs_rec_t *r = find(ns, key);

    if ( NULL == r )
    {
        return KODE_DENIED;
    }
    if ( (NULL == out) || (sizeof(*out) != r->len) )
    {
        return KODE_ERROR;
    }
    (void)memcpy(out, r->blob, sizeof(*out));
    return KODE_OK;
}

kode_status_t khal_nvs_set_u64(const char *ns, const char *key, uint64_t value)
{
    return khal_nvs_set_blob(ns, key, &value, sizeof(value));
}

kode_status_t khal_nvs_erase(const char *ns, const char *key)
{
    fake_nvs_rec_t *r = find(ns, key);

    if ( NULL != r )
    {
        r->used = false;
    }
    return KODE_OK;   /* missing is success: the postcondition holds */
}

kode_status_t khal_nvs_key(const char *ns, uint16_t idx, char *out, size_t cap)
{
    uint16_t seen = 0U;

    if ( (NULL == out) || (0U == cap) )
    {
        return KODE_ERROR;
    }

    for ( unsigned i = 0U; FAKE_NVS_RECORDS > i; ++i )
    {
        if ( !s_rec[i].used || (strcmp(s_rec[i].ns, ns) != 0) )
        {
            continue;
        }
        if ( seen == idx )
        {
            size_t const n = strlen(s_rec[i].key);
            if ( n >= cap )
            {
                return KODE_ERROR;   /* a truncated key opens nothing */
            }
            (void)memcpy(out, s_rec[i].key, n + 1U);
            return KODE_OK;
        }
        ++seen;
    }
    return KODE_DENIED;
}

/**
 * @brief Whether an earlier record carries the same namespace as record @p at.
 * @param at Index of the record to compare.
 * @return true when a lower-numbered used record has its namespace.
 */
static bool namespace_met_before(unsigned at)
{
    for ( unsigned i = 0U; i < at; ++i )
    {
        if ( s_rec[i].used && (strcmp(s_rec[i].ns, s_rec[at].ns) == 0) )
        {
            return true;
        }
    }
    return false;
}

kode_status_t khal_nvs_namespace(uint16_t idx, char *out, size_t cap)
{
    uint16_t seen = 0U;

    if ( (NULL == out) || (0U == cap) )
    {
        return KODE_ERROR;
    }

    for ( unsigned i = 0U; FAKE_NVS_RECORDS > i; ++i )
    {
        if ( !s_rec[i].used || namespace_met_before(i) )
        {
            continue;
        }
        if ( seen == idx )
        {
            size_t const n = strlen(s_rec[i].ns);
            if ( n >= cap )
            {
                return KODE_ERROR;   /* a truncated name opens nothing */
            }
            (void)memcpy(out, s_rec[i].ns, n + 1U);
            return KODE_OK;
        }
        ++seen;
    }
    return KODE_DENIED;
}

kode_status_t khal_nvs_erase_namespace(const char *ns)
{
    if ( NULL == ns )
    {
        return KODE_ERROR;
    }
    for ( unsigned i = 0U; FAKE_NVS_RECORDS > i; ++i )
    {
        if ( s_rec[i].used && (strcmp(s_rec[i].ns, ns) == 0) )
        {
            s_rec[i].used = false;
        }
    }
    return KODE_OK;   /* an empty namespace is success: the postcondition holds */
}
