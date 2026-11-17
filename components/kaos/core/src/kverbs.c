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

/**
 * @brief Every rule one row satisfies on its own, its keys apart.
 *
 * @param r   The row.
 * @param why Out: the rule it breaks, for the log, when it breaks one.
 * @return true when it breaks none.
 */
static bool verb_row_ok(const kverb_t *r, const char **why)
{
    bool ok = false;

    if ( !kregistry_name_ok(r->verb, KVERB_NAME_MAX) )
    {
        *why = "the verb is not a name";
    }
    else if ( NULL == r->fn )
    {
        *why = "no handler";
    }
    else if ( KARG_KEYS_MAX < r->n_args )
    {
        *why = "more keys than a line holds";
    }
    else if ( (0U < r->n_args) && (NULL == r->args) )
    {
        *why = "keys, and no specs for them";
    }
    else
    {
        ok = true;
    }
    return ok;
}

/**
 * @brief Whether every key of row @p i, and every member of its keys, is a
 *        name `help` can print in a record of its own. Logs the first that
 *        is not.
 *
 * A key is 1..KARG_KEY_NAME_MAX of [a-z0-9_], a member 1..KARG_MEMBER_NAME_MAX:
 * with a verb of KVERB_NAME_MAX, a `>|member=` record then fits one line.
 *
 * @param r The row, already found well formed by verb_row_ok().
 * @param i Its place in its table, for the log.
 * @return true when they all are.
 */
static bool verb_keys_ok(const kverb_t *r, size_t i)
{
    bool ok = true;

    for ( uint8_t a = 0U; ok && (a < r->n_args); ++a )
    {
        karg_spec_t const * const k = &r->args[a];

        if ( !kregistry_name_ok(k->key, KARG_KEY_NAME_MAX) )
        {
            KLOG_E(TAG, "verb %" PRIu32 " '%s', key %" PRIu32 ": not a name of 1 to "
                        "%" PRIu32 " of a-z, 0-9 and _",
                   (uint32_t)i, r->verb, (uint32_t)a, (uint32_t)KARG_KEY_NAME_MAX);
            ok = false;
        }
        for ( uint8_t m = 0U; ok && (NULL != k->enums) && (m < k->n_enums); ++m )
        {
            if ( !kregistry_name_ok(k->enums[m], KARG_MEMBER_NAME_MAX) )
            {
                KLOG_E(TAG, "verb %" PRIu32 " '%s', key '%s', member %" PRIu32 ": not a "
                            "name of 1 to %" PRIu32 " of a-z, 0-9 and _",
                       (uint32_t)i, r->verb, k->key, (uint32_t)m,
                       (uint32_t)KARG_MEMBER_NAME_MAX);
                ok = false;
            }
        }
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
        const kverb_t * const r   = &table->verbs[i];
        const char           *why = "";

        if ( !verb_row_ok(r, &why) )
        {
            KLOG_E(TAG, "verb %" PRIu32 ": %s", (uint32_t)i, why);
            st = KODE_DENIED;
        }
        else if ( !verb_keys_ok(r, i) )
        {
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

/**
 * @brief Whether the verb @p table says does not wait is one it may name.
 *
 * The console runs it with no values, on top of a command in flight: it has
 * to be a row of the table, and one that takes no keys.
 *
 * @param table The table, its rows already checked.
 * @return true with no such verb, or with one that is a keyless row.
 */
static bool verb_nowait_ok(const kverbs_t *table)
{
    bool ok = (NULL == table->nowait);

    for ( size_t i = 0U; (i < table->n) && !ok; ++i )
    {
        const kverb_t * const r = &table->verbs[i];

        ok = (strcmp(r->verb, table->nowait) == 0) && (0U == r->n_args);
    }
    return ok;
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
        if ( (KODE_OK == st) && !verb_nowait_ok(table) )
        {
            KLOG_E(TAG, "'%s' does not wait, and is not a row with no keys", table->nowait);
            st = KODE_DENIED;
        }
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
