/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file krows.c
 * @brief The registry of the owners' self-test rows.
 */
#include "krows.h"

#include <inttypes.h>
#include <string.h>

#include "kode_log.h"
#include "kqp.h"

#define TAG "kaos.rows"

static const krow_t *s_row_tables[KROWS_TABLES_MAX];
static uint8_t       s_row_counts[KROWS_TABLES_MAX];
static size_t        s_row_ntables;

/**
 * @brief How many tables are in, read with acquire: a table counted here was
 *        stored before the release that counted it, on either core.
 */
static size_t row_tables_in(void)
{
    /* cppcheck-suppress misra-c2012-17.3 ; GCC's __atomic builtins are the compiler's own, never declared in a header */
    return __atomic_load_n(&s_row_ntables, __ATOMIC_ACQUIRE);
}

/** @brief Every rule one row satisfies on its own. */
static bool row_ok(const krow_t *r)
{
    return (NULL != r->name) && ('\0' != r->name[0])
           && (strlen(r->name) < (size_t)KODE_TEST_NAME_LEN)
           && (NULL != r->fn) && (AUTOTEST_ID_NONE != r->id)
           && (NULL != r->label) && (strlen(r->label) <= (size_t)KROW_LABEL_MAX)
           && (NULL != r->problem) && (strlen(r->problem) <= (size_t)KROW_PROBLEM_MAX);
}

/** @brief Whether a registered row, or one of @p rows before @p upto, takes what @p r would. */
static bool row_taken(const krow_t *r, const krow_t *rows, size_t upto)
{
    bool taken = false;

    for ( size_t i = 0U; krows_count() > i; ++i )
    {
        const krow_t * const o = krows_at(i);

        taken = taken || (strcmp(o->name, r->name) == 0) || (o->id == r->id)
                || (o->order == r->order);
    }
    for ( size_t j = 0U; j < upto; ++j )
    {
        taken = taken || (strcmp(rows[j].name, r->name) == 0)
                || (rows[j].id == r->id) || (rows[j].order == r->order);
    }
    return taken;
}

/** @brief Whether @p rows is registered already. */
static bool row_table_in(const krow_t *rows)
{
    bool in = false;
    size_t const tables = row_tables_in();

    for ( size_t t = 0U; t < tables; ++t )
    {
        if ( s_row_tables[t] == rows )
        {
            in = true;
        }
    }
    return in;
}

kode_status_t krows_add(const krow_t *rows, size_t n)
{
    kode_status_t st = KODE_OK;

    if ( (NULL == rows) || (0U == n) )
    {
        KLOG_E(TAG, "an empty table");
        st = KODE_DENIED;
    }
    else if ( row_table_in(rows) )
    {
        /* Already in: a second start of the same owner adds nothing. */
    }
    else if ( row_tables_in() >= KROWS_TABLES_MAX )
    {
        KLOG_E(TAG, "no table slot left, over %" PRIu32, (uint32_t)KROWS_TABLES_MAX);
        st = KODE_NO_RESOURCES;
    }
    else if ( (krows_count() + n) > KROWS_ROWS_MAX )
    {
        KLOG_E(TAG, "over %" PRIu32 " rows", (uint32_t)KROWS_ROWS_MAX);
        st = KODE_NO_RESOURCES;
    }
    else
    {
        for ( size_t i = 0U; (i < n) && (KODE_OK == st); ++i )
        {
            if ( !row_ok(&rows[i]) )
            {
                KLOG_E(TAG, "row %" PRIu32 ": malformed", (uint32_t)i);
                st = KODE_DENIED;
            }
            else if ( row_taken(&rows[i], rows, i) )
            {
                KLOG_E(TAG, "'%s': its name, id or order is taken", rows[i].name);
                st = KODE_EXISTS;
            }
            else
            {
                /* A row of its own. */
            }
        }
        if ( KODE_OK == st )
        {
            size_t const at = row_tables_in();

            s_row_tables[at] = rows;
            s_row_counts[at] = (uint8_t)n;
            /* Published with release, after the slot: a walk that acquires
             * the new count sees the slot filled. Every owner registers from
             * kode_init(), before any run can be asked for. */
            /* cppcheck-suppress misra-c2012-17.3 ; GCC's __atomic builtins are the compiler's own, never declared in a header */
            __atomic_store_n(&s_row_ntables, at + 1U, __ATOMIC_RELEASE);
        }
    }
    return st;
}

size_t krows_count(void)
{
    size_t total = 0U;
    size_t const tables = row_tables_in();

    for ( size_t t = 0U; t < tables; ++t )
    {
        total += s_row_counts[t];
    }
    return total;
}

const krow_t *krows_at(size_t i)
{
    const krow_t *found = NULL;
    size_t        left  = i;
    size_t const tables = row_tables_in();

    for ( size_t t = 0U; (t < tables) && (NULL == found); ++t )
    {
        if ( left < s_row_counts[t] )
        {
            found = &s_row_tables[t][left];
        }
        else
        {
            left -= s_row_counts[t];
        }
    }
    return found;
}

const krow_t *krows_by_id(uint8_t id)
{
    const krow_t *found = NULL;

    for ( size_t i = 0U; (krows_count() > i) && (NULL == found); ++i )
    {
        if ( krows_at(i)->id == id )
        {
            found = krows_at(i);
        }
    }
    return found;
}

/* cppcheck-suppress misra-c2012-8.7 ; called by the host suite, another translation unit */
void krows_reset(void)
{
    for ( size_t t = 0U; KROWS_TABLES_MAX > t; ++t )
    {
        s_row_tables[t] = NULL;
    }
    (void)memset(s_row_counts, 0, sizeof(s_row_counts));
    /* cppcheck-suppress misra-c2012-17.3 ; GCC's __atomic builtins are the compiler's own, never declared in a header */
    __atomic_store_n(&s_row_ntables, 0U, __ATOMIC_RELEASE);
}
