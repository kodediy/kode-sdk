/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kregistry.c
 * @brief The registry: the variable, function and setting tables the console asks about.
 */
#include "kregistry.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "kode_log.h"
#include "kqp_config.h"

_Static_assert(KODE_CONFIG_KEY_MAX == KODE_ROW_NAME_MAX,
               "a setting's key is a row name");
_Static_assert(KODE_CONFIG_VALUE_MAX == KODE_VALUE_STR_MAX,
               "a setting's value is a value word");

#define TAG "kaos.registry"

static const kode_variable_t *s_vtables[KODE_VARIABLE_TABLES];
static size_t                 s_vcounts[KODE_VARIABLE_TABLES];
static size_t                 s_nvtables;

static const kode_function_t *s_ftables[KODE_FUNCTION_TABLES];
static size_t                 s_fcounts[KODE_FUNCTION_TABLES];
static size_t                 s_nftables;

static const kode_config_t   *s_ctables[KODE_CONFIG_TABLES];
static size_t                 s_ccounts[KODE_CONFIG_TABLES];
static QActive               *s_cowners[KODE_CONFIG_TABLES];  /**< Who answers each table. */
static uint16_t               s_cmargins[KODE_CONFIG_TABLES]; /**< The room its requests leave. */
static size_t                 s_nctables;

/**
 * @brief How many tables a registry holds, read with acquire: a table counted
 *        here was stored before the release that counted it, on either core.
 * @param count The registry's count.
 * @return The tables in.
 */
static size_t tables_in(const size_t *count)
{
    /* cppcheck-suppress misra-c2012-17.3 ; GCC's __atomic builtins are the compiler's own, never declared in a header */
    return __atomic_load_n(count, __ATOMIC_ACQUIRE);
}

/**
 * @brief Publish a registry's count with release, after the slots it counts.
 * @param count The registry's count.
 * @param n     The new count.
 */
static void tables_publish(size_t *count, size_t n)
{
    /* cppcheck-suppress misra-c2012-17.3 ; GCC's __atomic builtins are the compiler's own, never declared in a header */
    __atomic_store_n(count, n, __ATOMIC_RELEASE);
}

/** @brief Whether @p c may appear in a name or an enum word. */
static bool name_char(char c)
{
    return (('a' <= c) && ('z' >= c)) || (('0' <= c) && ('9' >= c)) || ('_' == c);
}

bool kregistry_name_ok(const char *s, size_t max)
{
    size_t n = 0U;
    bool   ok = (NULL != s);
    while ( ok && ('\0' != s[n]) )
    {
        ok = name_char(s[n]);
        ++n;
        if ( n > max )
        {
            ok = false;
        }
    }
    return ok && (0U < n);
}

#if CONFIG_KODEOS_CONSOLE
/* The checks and the tables' doors. A build without a console has neither:
 * nothing could reach a row, so the doors below the #else refuse every table. */

/**
 * @brief Whether a row's description fits: none, or at most KODE_ROW_DESC_MAX characters.
 * @param desc The row's description; may be NULL.
 * @return true when it is NULL or short enough.
 */
static bool desc_ok(const char *desc)
{
    return (NULL == desc) || (strlen(desc) <= KODE_ROW_DESC_MAX);
}

/** @brief An enum argument's list: 1..KODE_VALUE_ENUMS_MAX words, each a
 *         valid name. */
static bool enums_ok(const char *const *enums, uint8_t n_enums)
{
    bool ok = (NULL != enums) && (0U < n_enums)
           && (KODE_VALUE_ENUMS_MAX >= n_enums);
    for ( uint8_t i = 0U; ok && (i < n_enums); ++i )
    {
        ok = kregistry_name_ok(enums[i], KODE_ROW_NAME_MAX);
    }
    return ok;
}

/** @brief Every rule one variable has to satisfy on its own. */
static bool variable_ok(const kode_variable_t *r, const char **why)
{
    bool ok = true;
    if ( !kregistry_name_ok(r->name, KODE_ROW_NAME_MAX) )
    {
        *why = "name"; ok = false;
    }
    else if ( NULL == r->get )
    {
        *why = "no get"; ok = false;
    }
    else if ( !desc_ok(r->desc) )
    {
        *why = "desc"; ok = false;
    }
    else
    {
        /* A name, and something to read. */
    }
    return ok;
}

/** @brief Every rule one function has to satisfy on its own. */
static bool function_ok(const kode_function_t *r, const char **why)
{
    bool ok = true;

    /* First: every branch below, and the console's type word, reads the type
     * as an index, so an out-of-range type must be refused here. */
    if ( (unsigned)r->arg > (unsigned)KODE_TYPE_STRING )
    {
        *why = "arg"; ok = false;
    }
    else if ( !kregistry_name_ok(r->name, KODE_ROW_NAME_MAX) )
    {
        *why = "name"; ok = false;
    }
    else if ( NULL == r->run )
    {
        *why = "no run"; ok = false;
    }
    else if ( (KODE_TYPE_ENUM == r->arg) && !enums_ok(r->enums, r->n_enums) )
    {
        *why = "enum words"; ok = false;
    }
    else if ( (KODE_TYPE_INT == r->arg) && (r->min > r->max) )
    {
        *why = "min above max"; ok = false;
    }
    else if ( !desc_ok(r->desc) )
    {
        *why = "desc"; ok = false;
    }
    else if ( KODE_FUNCTION_ANSWER_MS_MAX < r->answer_ms )
    {
        *why = "answer_ms"; ok = false;
    }
    else
    {
        /* A function with a name and something to run. */
    }
    return ok;
}

/** @brief Every rule one setting has to satisfy on its own. */
static bool setting_ok(const kode_config_t *r, const char **why)
{
    bool ok = true;

    /* First, for the reason function_ok() gives; a setting always has a value. */
    if ( ((unsigned)r->type > (unsigned)KODE_TYPE_STRING) || (KODE_TYPE_NONE == r->type) )
    {
        *why = "type"; ok = false;
    }
    else if ( !kregistry_name_ok(r->name, KODE_ROW_NAME_MAX) )
    {
        *why = "name"; ok = false;
    }
    else if ( (KODE_TYPE_ENUM == r->type) && !enums_ok(r->enums, r->n_enums) )
    {
        *why = "enum words"; ok = false;
    }
    else if ( (KODE_TYPE_INT == r->type) && (r->min > r->max) )
    {
        *why = "min above max"; ok = false;
    }
    else
    {
        /* A key and a type. */
    }
    return ok;
}

/**
 * @brief The refusals on room, which every kind makes before looking at a row.
 *
 * They run before any row is checked, so the first row's name may be NULL
 * and is guarded.
 */
static kode_status_t room_ok(const char *first, size_t ntables, size_t max_tables,
                             size_t rows, size_t n, size_t max_rows)
{
    const char * const name = (NULL != first) ? first : "?";
    if ( ntables >= max_tables )
    {
        KLOG_E(TAG, "%s: no table slot left", name);
        return KODE_NO_RESOURCES;
    }
    if ( (rows + n) > max_rows )
    {
        KLOG_E(TAG, "%s: over %u rows", name, (unsigned)max_rows);
        return KODE_NO_RESOURCES;
    }
    return KODE_OK;
}

/** @brief Whether @p name already means something: one namespace, both kinds. */
static bool name_taken(const char *name)
{
    return (kvariables_find(name) != NULL) || (kfunctions_find(name) != NULL);
}

kode_status_t kode_variables_add(const kode_variable_t *table, size_t n)
{
    if ( (NULL == table) || (0U == n) )
    {
        KLOG_E(TAG, "an empty table");
        return KODE_DENIED;
    }
    kode_status_t const room = room_ok(table[0].name, tables_in(&s_nvtables),
                                       KODE_VARIABLE_TABLES, kvariables_count(),
                                       n, KODE_VARIABLES_MAX);
    if ( KODE_OK != room )
    {
        return room;
    }
    for ( size_t i = 0U; i < n; ++i )
    {
        const char *why = "";
        if ( !variable_ok(&table[i], &why) )
        {
            KLOG_E(TAG, "variable %u '%s': %s", (unsigned)i,
                   (NULL != table[i].name) ? table[i].name : "?", why);
            return KODE_DENIED;
        }
        if ( name_taken(table[i].name) )
        {
            KLOG_E(TAG, "'%s' is already registered", table[i].name);
            return KODE_EXISTS;
        }
        for ( size_t j = 0U; j < i; ++j )
        {
            if ( strcmp(table[i].name, table[j].name) == 0 )
            {
                KLOG_E(TAG, "'%s' twice in one table", table[i].name);
                return KODE_EXISTS;
            }
        }
    }
    s_vtables[tables_in(&s_nvtables)] = table;
    s_vcounts[tables_in(&s_nvtables)] = n;
    /* Last, so a command parsing right now sees the registry before this
     * call or after it, never with a table half in. */
    tables_publish(&s_nvtables, tables_in(&s_nvtables) + 1U);
    return KODE_OK;
}

kode_status_t kode_functions_add(const kode_function_t *table, size_t n)
{
    if ( (NULL == table) || (0U == n) )
    {
        KLOG_E(TAG, "an empty table");
        return KODE_DENIED;
    }
    kode_status_t const room = room_ok(table[0].name, tables_in(&s_nftables),
                                       KODE_FUNCTION_TABLES, kfunctions_count(),
                                       n, KODE_FUNCTIONS_MAX);
    if ( KODE_OK != room )
    {
        return room;
    }
    for ( size_t i = 0U; i < n; ++i )
    {
        const char *why = "";
        if ( !function_ok(&table[i], &why) )
        {
            KLOG_E(TAG, "function %u '%s': %s", (unsigned)i,
                   (NULL != table[i].name) ? table[i].name : "?", why);
            return KODE_DENIED;
        }
        if ( name_taken(table[i].name) )
        {
            KLOG_E(TAG, "'%s' is already registered", table[i].name);
            return KODE_EXISTS;
        }
        for ( size_t j = 0U; j < i; ++j )
        {
            if ( strcmp(table[i].name, table[j].name) == 0 )
            {
                KLOG_E(TAG, "'%s' twice in one table", table[i].name);
                return KODE_EXISTS;
            }
        }
    }
    s_ftables[tables_in(&s_nftables)] = table;
    s_fcounts[tables_in(&s_nftables)] = n;
    /* Last, for the reason kode_variables_add() gives. */
    tables_publish(&s_nftables, tables_in(&s_nftables) + 1U);
    return KODE_OK;
}

/**
 * @brief Whether @p owner can take a request that leaves @p margin free.
 *
 * An owner with no queue, or one the margin fills, could never take a
 * request. Runs before any row is checked, so @p first may be NULL.
 *
 * @param owner  The table's owner.
 * @param margin The slots its requests leave free.
 * @param first  The first row's name, for the log line; may be NULL.
 * @return true when the owner is started and its queue is longer than the margin.
 */
static bool owner_ok(QActive const *owner, uint16_t margin, const char *first)
{
    uint16_t const len  = kqp_queue_len(owner);
    const char    *name = "?";
    bool           ok   = true;

    if ( NULL != first )
    {
        name = first;
    }
    if ( 0U == len )
    {
        KLOG_E(TAG, "%s: no owner, or an owner not started", name);
        ok = false;
    }
    else if ( margin >= len )
    {
        KLOG_E(TAG, "%s: a margin of %u leaves no room in a queue of %u",
               name, (unsigned)margin, (unsigned)len);
        ok = false;
    }
    else
    {
        /* Started, with room past the margin. */
    }
    return ok;
}

kode_status_t kode_config_add(QActive *owner, uint16_t margin,
                              const kode_config_t *table, size_t n)
{
    if ( (NULL == table) || (0U == n) )
    {
        KLOG_E(TAG, "an empty table");
        return KODE_DENIED;
    }
    if ( !owner_ok(owner, margin, table[0].name) )
    {
        return KODE_DENIED;
    }
    for ( size_t t = 0U; tables_in(&s_nctables) > t; ++t )
    {
        if ( s_ctables[t] == table )
        {
            /* Already in: a second start of the same owner adds nothing. */
            return KODE_OK;
        }
    }
    kode_status_t const room = room_ok(table[0].name, tables_in(&s_nctables),
                                       KODE_CONFIG_TABLES, kconfig_count(),
                                       n, KODE_CONFIG_MAX);
    if ( KODE_OK != room )
    {
        return room;
    }
    for ( size_t i = 0U; i < n; ++i )
    {
        const char *why = "";
        if ( !setting_ok(&table[i], &why) )
        {
            KLOG_E(TAG, "setting %u '%s': %s", (unsigned)i,
                   (NULL != table[i].name) ? table[i].name : "?", why);
            return KODE_DENIED;
        }
        /* Settings have a verb of their own, so their keys are a namespace
         * of their own: only another setting can take one. */
        if ( kconfig_find(table[i].name) != NULL )
        {
            KLOG_E(TAG, "'%s' is already registered", table[i].name);
            return KODE_EXISTS;
        }
        for ( size_t j = 0U; j < i; ++j )
        {
            if ( strcmp(table[i].name, table[j].name) == 0 )
            {
                KLOG_E(TAG, "'%s' twice in one table", table[i].name);
                return KODE_EXISTS;
            }
        }
    }
    s_ctables[tables_in(&s_nctables)]  = table;
    s_ccounts[tables_in(&s_nctables)]  = n;
    s_cowners[tables_in(&s_nctables)]  = owner;
    s_cmargins[tables_in(&s_nctables)] = margin;
    /* Last, for the reason kode_variables_add() gives. */
    tables_publish(&s_nctables, tables_in(&s_nctables) + 1U);
    return KODE_OK;
}

#else /* !CONFIG_KODEOS_CONSOLE */

/**
 * @brief Refuse a table: with no console, nothing can reach a row.
 * @param what The kind, for the log line.
 * @return KODE_DENIED.
 */
static kode_status_t registry_refuse(const char *what)
{
    KLOG_E(TAG, "no console in this build: nothing can reach a %s", what);
    return KODE_DENIED;
}

kode_status_t kode_variables_add(const kode_variable_t *table, size_t n)
{
    (void)table;
    (void)n;
    return registry_refuse("variable");
}

kode_status_t kode_functions_add(const kode_function_t *table, size_t n)
{
    (void)table;
    (void)n;
    return registry_refuse("function");
}

kode_status_t kode_config_add(QActive *owner, uint16_t margin,
                              const kode_config_t *table, size_t n)
{
    (void)owner;
    (void)margin;
    (void)table;
    (void)n;
    return registry_refuse("setting");
}

#endif /* CONFIG_KODEOS_CONSOLE */

/* ---- Lookup ----------------------------------------------------------------- */

size_t kvariables_count(void)
{
    size_t total = 0U;
    for ( size_t t = 0U; tables_in(&s_nvtables) > t; ++t )
    {
        total += s_vcounts[t];
    }
    return total;
}

const kode_variable_t *kvariables_at(size_t i)
{
    for ( size_t t = 0U; tables_in(&s_nvtables) > t; ++t )
    {
        if ( i < s_vcounts[t] )
        {
            return &s_vtables[t][i];
        }
        i -= s_vcounts[t];
    }
    return NULL;
}

const kode_variable_t *kvariables_find(const char *name)
{
    if ( NULL == name )
    {
        return NULL;
    }
    for ( size_t t = 0U; tables_in(&s_nvtables) > t; ++t )
    {
        for ( size_t i = 0U; i < s_vcounts[t]; ++i )
        {
            if ( strcmp(s_vtables[t][i].name, name) == 0 )
            {
                return &s_vtables[t][i];
            }
        }
    }
    return NULL;
}

size_t kfunctions_count(void)
{
    size_t total = 0U;
    for ( size_t t = 0U; tables_in(&s_nftables) > t; ++t )
    {
        total += s_fcounts[t];
    }
    return total;
}

const kode_function_t *kfunctions_at(size_t i)
{
    for ( size_t t = 0U; tables_in(&s_nftables) > t; ++t )
    {
        if ( i < s_fcounts[t] )
        {
            return &s_ftables[t][i];
        }
        i -= s_fcounts[t];
    }
    return NULL;
}

const kode_function_t *kfunctions_find(const char *name)
{
    if ( NULL == name )
    {
        return NULL;
    }
    for ( size_t t = 0U; tables_in(&s_nftables) > t; ++t )
    {
        for ( size_t i = 0U; i < s_fcounts[t]; ++i )
        {
            if ( strcmp(s_ftables[t][i].name, name) == 0 )
            {
                return &s_ftables[t][i];
            }
        }
    }
    return NULL;
}

size_t kconfig_count(void)
{
    size_t total = 0U;
    for ( size_t t = 0U; tables_in(&s_nctables) > t; ++t )
    {
        total += s_ccounts[t];
    }
    return total;
}

const kode_config_t *kconfig_at(size_t i)
{
    size_t left = i;

    for ( size_t t = 0U; tables_in(&s_nctables) > t; ++t )
    {
        if ( left < s_ccounts[t] )
        {
            return &s_ctables[t][left];
        }
        left -= s_ccounts[t];
    }
    return NULL;
}

QActive *kconfig_owner(const kode_config_t *row, uint16_t *margin)
{
    QActive *owner = NULL;

    for ( size_t t = 0U; (tables_in(&s_nctables) > t) && (NULL == owner); ++t )
    {
        for ( size_t i = 0U; (i < s_ccounts[t]) && (NULL == owner); ++i )
        {
            if ( &s_ctables[t][i] == row )
            {
                owner = s_cowners[t];
                if ( NULL != margin )
                {
                    *margin = s_cmargins[t];
                }
            }
        }
    }
    return owner;
}

const kode_config_t *kconfig_find(const char *name)
{
    if ( NULL == name )
    {
        return NULL;
    }
    for ( size_t t = 0U; tables_in(&s_nctables) > t; ++t )
    {
        for ( size_t i = 0U; i < s_ccounts[t]; ++i )
        {
            if ( strcmp(s_ctables[t][i].name, name) == 0 )
            {
                return &s_ctables[t][i];
            }
        }
    }
    return NULL;
}

/* ---- Words and values ------------------------------------------------------- */

/** @brief Strict signed decimal: an optional `-`, then digits, within int32. */
static bool parse_i32(const char *s, int32_t *out)
{
    bool     neg = false;
    uint64_t val = 0U;
    size_t   i   = 0U;

    if ( '-' == s[0] )
    {
        neg = true;
        i   = 1U;
    }
    if ( '\0' == s[i] )
    {
        return false;
    }
    for ( ; '\0' != s[i]; ++i )
    {
        if ( ('0' > s[i]) || ('9' < s[i]) )
        {
            return false;
        }
        val = (val * 10U) + (uint64_t)(s[i] - '0');
        if ( 2147483648ULL < val )
        {
            return false;
        }
    }
    if ( neg )
    {
        *out = (int32_t)(-(int64_t)val);
        return true;
    }
    if ( 2147483647ULL < val )
    {
        return false;
    }
    *out = (int32_t)val;
    return true;
}

/** @brief A type, the bounds and words that go with it, and the longest string. */
typedef struct {
    kode_type_t        type;
    int32_t            min;
    int32_t            max;
    const char *const *enums;
    uint8_t            n_enums;
    size_t             str_max;
} value_spec_t;

/** @brief Parse @p word against @p row into @p out: the rules kfunctions_parse() states. */
static kode_status_t value_parse(const value_spec_t *row, const char *word,
                                 kode_value_t *out)
{
    (void)memset(out, 0, sizeof(*out));
    if ( (NULL == word) || ('\0' == word[0]) )
    {
        return KODE_DENIED;
    }
    switch ( row->type )
    {
        case KODE_TYPE_INT: {
            int32_t v;
            if ( !parse_i32(word, &v) )
            {
                return KODE_DENIED;
            }
            if ( (row->min != row->max) && ((v < row->min) || (v > row->max)) )
            {
                return KODE_DENIED;
            }
            out->i = v;
            return KODE_OK;
        }
        case KODE_TYPE_BOOL: {
            if ( strcmp(word, "on") == 0 )
            {
                out->b = true;
                return KODE_OK;
            }
            if ( strcmp(word, "off") == 0 )
            {
                out->b = false;
                return KODE_OK;
            }
            return KODE_DENIED;
        }
        case KODE_TYPE_ENUM: {
            for ( uint8_t i = 0U; i < row->n_enums; ++i )
            {
                if ( strcmp(word, row->enums[i]) == 0 )
                {
                    out->e = i;
                    return KODE_OK;
                }
            }
            return KODE_DENIED;
        }
        case KODE_TYPE_STRING: {
            if ( strlen(word) > row->str_max )
            {
                return KODE_DENIED;
            }
            out->s = word;
            return KODE_OK;
        }
        case KODE_TYPE_NONE:
        default:
            return KODE_DENIED;
    }
}

kode_status_t kfunctions_parse(const kode_function_t *row, const char *word,
                             kode_value_t *out)
{
    value_spec_t const spec = {
        .type = row->arg, .min = row->min, .max = row->max,
        .enums = row->enums, .n_enums = row->n_enums,
        .str_max = KODE_ARG_STR_MAX,
    };
    return value_parse(&spec, word, out);
}

kode_status_t kconfig_parse(const kode_config_t *row, const char *word,
                            kode_value_t *out)
{
    value_spec_t const spec = {
        .type = row->type, .min = row->min, .max = row->max,
        .enums = row->enums, .n_enums = row->n_enums,
        .str_max = KODE_VALUE_STR_MAX,
    };
    return value_parse(&spec, word, out);
}

kode_status_t kvariables_read(const kode_variable_t *row, char *buf, size_t cap)
{
    if ( KREGISTRY_WORD_MAX > cap )
    {
        return KODE_DENIED;
    }
    (void)memset(buf, 0, cap);
    kode_status_t const rc = row->get(buf, cap);
    if ( KODE_OK != rc )
    {
        return rc;
    }
    /* Whatever the getter did, the word ends inside the buffer. */
    buf[cap - 1U] = '\0';
    size_t const n = strlen(buf);
    if ( (0U == n) || (KODE_VALUE_STR_MAX < n) )
    {
        return KODE_DENIED;
    }
    /* A quote too: a variable's value goes out bare, and a bare value
     * holding a quote would read to a host as the start of a string. */
    for ( size_t i = 0U; i < n; ++i )
    {
        unsigned char const c = (unsigned char)buf[i];
        if ( ((unsigned char)' ' >= c) || (0x7FU == c)
            || ((unsigned char)'"' == c) )
        {
            buf[i] = '_';
        }
    }
    return KODE_OK;
}

void kvariables_reset(void)
{
    (void)memset(s_vtables, 0, sizeof(s_vtables));
    (void)memset(s_vcounts, 0, sizeof(s_vcounts));
    tables_publish(&s_nvtables, 0U);
}

void kfunctions_reset(void)
{
    (void)memset(s_ftables, 0, sizeof(s_ftables));
    (void)memset(s_fcounts, 0, sizeof(s_fcounts));
    tables_publish(&s_nftables, 0U);
}

/* cppcheck-suppress misra-c2012-8.7 ; called by the host suite, another translation unit */
void kconfig_reset(void)
{
    (void)memset(s_ctables, 0, sizeof(s_ctables));
    (void)memset(s_ccounts, 0, sizeof(s_ccounts));
    (void)memset(s_cowners, 0, sizeof(s_cowners));
    (void)memset(s_cmargins, 0, sizeof(s_cmargins));
    tables_publish(&s_nctables, 0U);
}
