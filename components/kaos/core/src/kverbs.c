/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kverbs.c
 * @brief The registry of the owners' console verbs.
 */
#include "kverbs.h"

#include <inttypes.h>
#include <string.h>

#include "console_verbs.h"
#include "kode_log.h"
#include "kregistry.h"

#define TAG "kaos.verbs"

static const kverbs_t *s_verb_tables[KVERBS_TABLES_MAX];
static size_t          s_verb_ntables;

/**
 * @brief How many tables are in, read with acquire: a table counted here was
 *        stored before the release that counted it, on either core.
 */
static size_t verb_tables_in(void)
{
    /* cppcheck-suppress misra-c2012-17.3 ; GCC's __atomic builtins are the compiler's own, never declared in a header */
    return __atomic_load_n(&s_verb_ntables, __ATOMIC_ACQUIRE);
}

/** @brief Every rule one row satisfies on its own. */
static bool verb_row_ok(const kverb_t *r)
{
    bool ok = kregistry_name_ok(r->verb, KVERB_NAME_MAX) && (NULL != r->fn)
              && (KARG_KEYS_MAX >= r->n_args);

    if ( ok && (0U < r->n_args) )
    {
        ok = (NULL != r->args);
    }
    return ok;
}

/** @brief Whether @p table is registered already. */
static bool verb_table_in(const kverbs_t *table)
{
    bool in = false;
    size_t const tables = verb_tables_in();

    for ( size_t t = 0U; t < tables; ++t )
    {
        if ( s_verb_tables[t] == table )
        {
            in = true;
        }
    }
    return in;
}

/** @brief Check @p table's rows: well formed, and every verb free. */
static kode_status_t verb_rows_check(const kverbs_t *table)
{
    kode_status_t st = KODE_OK;

    for ( size_t i = 0U; (i < table->n) && (KODE_OK == st); ++i )
    {
        const kverb_t * const r = &table->verbs[i];

        if ( !verb_row_ok(r) )
        {
            KLOG_E(TAG, "verb %" PRIu32 ": malformed", (uint32_t)i);
            st = KODE_DENIED;
        }
        else if ( console_system_verb(r->verb)
                 || (kverbs_find(r->verb, NULL) != NULL) )
        {
            KLOG_E(TAG, "'%s' is already a verb", r->verb);
            st = KODE_EXISTS;
        }
        else
        {
            for ( size_t j = 0U; j < i; ++j )
            {
                if ( strcmp(r->verb, table->verbs[j].verb) == 0 )
                {
                    KLOG_E(TAG, "'%s' twice in one table", r->verb);
                    st = KODE_EXISTS;
                }
            }
        }
    }
    return st;
}

kode_status_t kverbs_add(const kverbs_t *table)
{
    kode_status_t st = KODE_OK;

    if ( (NULL == table) || (NULL == table->verbs) || (0U == table->n) )
    {
        KLOG_E(TAG, "an empty table");
        st = KODE_DENIED;
    }
    else if ( verb_table_in(table) )
    {
        /* Already in: a second start of the same owner adds nothing. */
    }
    else if ( verb_tables_in() >= KVERBS_TABLES_MAX )
    {
        KLOG_E(TAG, "'%s': no table slot left, over %" PRIu32, table->verbs[0].verb,
               (uint32_t)KVERBS_TABLES_MAX);
        st = KODE_NO_RESOURCES;
    }
    else
    {
        st = verb_rows_check(table);
        if ( KODE_OK == st )
        {
            size_t const at = verb_tables_in();

            s_verb_tables[at] = table;
            /* Published with release, after the slot: a reader that acquires
             * the new count sees the slot filled. Nothing registers after
             * log_start_reader(), so no line is parsed meanwhile anyway. */
            /* cppcheck-suppress misra-c2012-17.3 ; GCC's __atomic builtins are the compiler's own, never declared in a header */
            __atomic_store_n(&s_verb_ntables, at + 1U, __ATOMIC_RELEASE);
        }
    }
    return st;
}

size_t kverbs_count(void)
{
    size_t total = 0U;
    size_t const tables = verb_tables_in();

    for ( size_t t = 0U; t < tables; ++t )
    {
        total += s_verb_tables[t]->n;
    }
    return total;
}

const kverb_t *kverbs_at(size_t i)
{
    const kverb_t *found = NULL;
    size_t         left  = i;
    size_t const tables = verb_tables_in();

    for ( size_t t = 0U; (t < tables) && (NULL == found); ++t )
    {
        if ( left < s_verb_tables[t]->n )
        {
            found = &s_verb_tables[t]->verbs[left];
        }
        else
        {
            left -= s_verb_tables[t]->n;
        }
    }
    return found;
}

const kverb_t *kverbs_find(const char *verb, const kverbs_t **owner)
{
    const kverb_t *found = NULL;
    size_t const tables = verb_tables_in();

    for ( size_t t = 0U; (NULL != verb) && (t < tables) && (NULL == found); ++t )
    {
        for ( size_t i = 0U; (i < s_verb_tables[t]->n) && (NULL == found); ++i )
        {
            if ( strcmp(s_verb_tables[t]->verbs[i].verb, verb) == 0 )
            {
                found = &s_verb_tables[t]->verbs[i];
                if ( NULL != owner )
                {
                    *owner = s_verb_tables[t];
                }
            }
        }
    }
    return found;
}

const kverbs_t *kverbs_table(size_t i)
{
    const kverbs_t *t = NULL;

    if ( verb_tables_in() > i )
    {
        t = s_verb_tables[i];
    }
    return t;
}

/* cppcheck-suppress misra-c2012-8.7 ; called by the host suite, another translation unit */
void kverbs_reset(void)
{
    for ( size_t t = 0U; KVERBS_TABLES_MAX > t; ++t )
    {
        s_verb_tables[t] = NULL;
    }
    /* cppcheck-suppress misra-c2012-17.3 ; GCC's __atomic builtins are the compiler's own, never declared in a header */
    __atomic_store_n(&s_verb_ntables, 0U, __ATOMIC_RELEASE);
}
