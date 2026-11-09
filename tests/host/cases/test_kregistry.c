/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kregistry.c
 * @brief The registry: what a table may declare and how arguments and values are parsed.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "ao_spy.h"
#include "fakes.h"
#include "kregistry.h"
#include "kode_config.h"
#include "kode_functions.h"
#include "kode_defs.h"
#include "kode_variables.h"

static const char   *s_word;                 /**< what word_get() writes      */
static kode_status_t s_word_rc = KODE_OK;    /**< and what it answers         */

static kode_status_t get_ok(char *buf, size_t cap)   { (void)snprintf(buf, cap, "7"); return KODE_OK; }
static kode_status_t word_get(char *buf, size_t cap) { (void)snprintf(buf, cap, "%s", s_word); return s_word_rc; }
static kode_status_t run_ok(const kode_value_t *arg) { (void)arg; return KODE_OK; }

static const char *const MODES[] = { "off", "slow", "fast" };

/** @brief A variable called @p name. */
static kode_variable_t one_var(const char *name)
{
    kode_variable_t r;
    (void)memset(&r, 0, sizeof(r));
    r.name = name;
    r.get  = get_ok;
    return r;
}

/** @brief A function called @p name that takes no argument. */
static kode_function_t one_fn(const char *name)
{
    kode_function_t r;
    (void)memset(&r, 0, sizeof(r));
    r.name = name;
    r.run  = run_ok;
    return r;
}

static void fresh(void)
{
    fake_klog_reset();
    kvariables_reset();
    kfunctions_reset();
}

/* ---- The guard that keeps this file honest -------------------------------- */

TEST_CASE("the build really has the console compiled in", "[kregistry]")
{
#if !CONFIG_KODEOS_CONSOLE
    TEST_FAIL_MESSAGE("CONFIG_KODEOS_CONSOLE is not set: every case in this "
                      "file would be asserting about a refusal");
#endif
    fresh();
    static kode_variable_t v[1];
    static kode_function_t   a[1];
    v[0] = one_var("temp_dc");
    a[0] = one_fn("blink");
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_variables_add(v, 1U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_functions_add(a, 1U));
    TEST_ASSERT_EQUAL_UINT(1U, kvariables_count());
    TEST_ASSERT_EQUAL_UINT(1U, kfunctions_count());
}

/* ---- Names ------------------------------------------------------------------ */

TEST_CASE("a name with a capital, a space or sixteen characters is refused",
          "[kregistry]")
{
    fresh();
    static kode_variable_t t[1];
    t[0] = one_var("Temp");
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_variables_add(t, 1U));
    t[0].name = "temp dc";
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_variables_add(t, 1U));
    t[0].name = "abcdefghijklmnop";   /* 16 */
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_variables_add(t, 1U));
    t[0].name = "";
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_variables_add(t, 1U));
    t[0].name = "abcdefghijklmno";    /* 15: the cap itself is allowed */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_variables_add(t, 1U));
    TEST_ASSERT_EQUAL_UINT(1U, kvariables_count());

    /* A function's name is held to the same rule. */
    static kode_function_t a[1];
    a[0] = one_fn("Blink");
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_functions_add(a, 1U));
    TEST_ASSERT_EQUAL_UINT(0U, kfunctions_count());
}

TEST_CASE("a duplicate name across two tables is KODE_EXISTS", "[kregistry]")
{
    fresh();
    static kode_variable_t a[1];
    static kode_variable_t b[2];
    a[0] = one_var("gain");
    b[0] = one_var("mode"); b[1] = one_var("gain");
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_variables_add(a, 1U));
    TEST_ASSERT_EQUAL_INT(KODE_EXISTS, kode_variables_add(b, 2U));
    /* Refused whole: `mode` from the bad table is not there either. */
    TEST_ASSERT_EQUAL_UINT(1U, kvariables_count());
    TEST_ASSERT_NULL(kvariables_find("mode"));
}

TEST_CASE("a duplicate inside one table is refused the same way", "[kregistry]")
{
    fresh();
    static kode_variable_t t[2];
    t[0] = one_var("gain"); t[1] = one_var("gain");
    TEST_ASSERT_EQUAL_INT(KODE_EXISTS, kode_variables_add(t, 2U));
    TEST_ASSERT_EQUAL_UINT(0U, kvariables_count());

    static kode_function_t a[2];
    a[0] = one_fn("blink"); a[1] = one_fn("blink");
    TEST_ASSERT_EQUAL_INT(KODE_EXISTS, kode_functions_add(a, 2U));
    TEST_ASSERT_EQUAL_UINT(0U, kfunctions_count());
}

TEST_CASE("a name is a variable or a function, never both", "[kregistry]")
{
    fresh();
    static kode_variable_t v[1];
    static kode_function_t   a[2];
    v[0] = one_var("led");
    a[0] = one_fn("blink"); a[1] = one_fn("led");
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_variables_add(v, 1U));
    TEST_ASSERT_EQUAL_INT(KODE_EXISTS, kode_functions_add(a, 2U));
    TEST_ASSERT_EQUAL_UINT(0U, kfunctions_count());

    /* And the other way round. */
    fresh();
    a[1] = one_fn("led");
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_functions_add(&a[1], 1U));
    TEST_ASSERT_EQUAL_INT(KODE_EXISTS, kode_variables_add(v, 1U));
    TEST_ASSERT_EQUAL_UINT(0U, kvariables_count());
}

/* ---- Room ------------------------------------------------------------------- */

TEST_CASE("the thirty-third row and the fifth table are KODE_NO_RESOURCES",
          "[kregistry]")
{
    static char names[33][8];
    static kode_variable_t big[33];
    fresh();
    for ( unsigned i = 0U; 33U > i; ++i )
    {
        (void)snprintf(names[i], sizeof(names[i]), "r%u", i);
        big[i] = one_var(names[i]);
    }
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_variables_add(big, 33U));
    TEST_ASSERT_EQUAL_UINT(0U, kvariables_count());
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_variables_add(big, 32U));
    TEST_ASSERT_EQUAL_UINT(32U, kvariables_count());

    fresh();
    static kode_variable_t t1[1], t2[1], t3[1], t4[1], t5[1];
    t1[0] = one_var("a"); t2[0] = one_var("b"); t3[0] = one_var("c");
    t4[0] = one_var("d"); t5[0] = one_var("e");
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_variables_add(t1, 1U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_variables_add(t2, 1U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_variables_add(t3, 1U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_variables_add(t4, 1U));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_variables_add(t5, 1U));
    TEST_ASSERT_EQUAL_UINT(4U, kvariables_count());
    TEST_ASSERT_EQUAL_STRING("d", kvariables_at(3U)->name);
    TEST_ASSERT_NULL(kvariables_at(4U));
}

TEST_CASE("variables and functions each have caps of their own", "[kregistry]")
{
    static char vnames[KODE_VARIABLES_MAX][8];
    static char anames[KODE_FUNCTIONS_MAX + 1U][8];
    static kode_variable_t vars[KODE_VARIABLES_MAX];
    static kode_function_t   acts[KODE_FUNCTIONS_MAX + 1U];
    fresh();
    for ( unsigned i = 0U; KODE_VARIABLES_MAX > i; ++i )
    {
        (void)snprintf(vnames[i], sizeof(vnames[i]), "v%u", i);
        vars[i] = one_var(vnames[i]);
    }
    for ( unsigned i = 0U; (KODE_FUNCTIONS_MAX + 1U) > i; ++i )
    {
        (void)snprintf(anames[i], sizeof(anames[i]), "a%u", i);
        acts[i] = one_fn(anames[i]);
    }
    /* A full set of variables takes nothing from the functions. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_variables_add(vars, KODE_VARIABLES_MAX));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES,
                          kode_functions_add(acts, KODE_FUNCTIONS_MAX + 1U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_functions_add(acts, KODE_FUNCTIONS_MAX));
    TEST_ASSERT_EQUAL_UINT(KODE_VARIABLES_MAX, kvariables_count());
    TEST_ASSERT_EQUAL_UINT(KODE_FUNCTIONS_MAX, kfunctions_count());

    fresh();
    static kode_function_t t1[1], t2[1], t3[1], t4[1], t5[1];
    t1[0] = one_fn("a"); t2[0] = one_fn("b"); t3[0] = one_fn("c");
    t4[0] = one_fn("d"); t5[0] = one_fn("e");
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_functions_add(t1, 1U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_functions_add(t2, 1U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_functions_add(t3, 1U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_functions_add(t4, 1U));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_functions_add(t5, 1U));
    TEST_ASSERT_EQUAL_UINT(4U, kfunctions_count());
    TEST_ASSERT_EQUAL_STRING("d", kfunctions_at(3U)->name);
    TEST_ASSERT_NULL(kfunctions_at(4U));
}

TEST_CASE("a capacity refusal survives a nameless first row", "[kregistry]")
{
    static char names[31][8];
    static kode_variable_t full[31];
    static kode_variable_t bad[2];

    fresh();
    for ( unsigned i = 0U; 31U > i; ++i )
    {
        (void)snprintf(names[i], sizeof(names[i]), "n%u", i);
        full[i] = one_var(names[i]);
    }
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_variables_add(full, 31U));
    TEST_ASSERT_EQUAL_UINT(31U, kvariables_count());

    /* Two more rows is thirty-three, so the table is refused on room before
     * any row has been looked at - and the line that says so names the
     * first row, which here has no name to print. */
    bad[0] = one_var(NULL);
    bad[1] = one_var("also_fine");
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_variables_add(bad, 2U));
    TEST_ASSERT_EQUAL_UINT(31U, kvariables_count());
    TEST_ASSERT_NOT_NULL(fake_klog_last());
    TEST_ASSERT_NOT_NULL(strstr(fake_klog_last(), "?: over"));

    /* The other refusal on room reads the same field, so it gets the same
     * table: four tables are all there are. */
    fresh();
    static kode_variable_t t1[1], t2[1], t3[1], t4[1];
    t1[0] = one_var("a"); t2[0] = one_var("b");
    t3[0] = one_var("c"); t4[0] = one_var("d");
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_variables_add(t1, 1U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_variables_add(t2, 1U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_variables_add(t3, 1U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_variables_add(t4, 1U));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_variables_add(bad, 2U));
    TEST_ASSERT_EQUAL_UINT(4U, kvariables_count());
    TEST_ASSERT_NOT_NULL(strstr(fake_klog_last(), "?: no table slot left"));
}

/* ---- What a row may be ------------------------------------------------------ */

TEST_CASE("a variable needs get, and a description that fits", "[kregistry]")
{
    fresh();
    static kode_variable_t bad[1];
    bad[0] = one_var("nothing"); bad[0].get = NULL;
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_variables_add(bad, 1U));
    bad[0] = one_var("long");
    bad[0].desc = "0123456789012345678901234567890123456789012345678";   /* 49 */
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_variables_add(bad, 1U));
    bad[0].desc = "012345678901234567890123456789012345678901234567";    /* 48 */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_variables_add(bad, 1U));
    TEST_ASSERT_EQUAL_UINT(1U, kvariables_count());
}

TEST_CASE("a function needs run, and its argument's rules are checked", "[kregistry]")
{
    fresh();
    static kode_function_t ok[3];
    ok[0] = one_fn("blink");
    ok[1] = one_fn("move");  ok[1].arg = KODE_TYPE_INT; ok[1].min = -5; ok[1].max = 5;
    ok[2] = one_fn("fan");   ok[2].arg = KODE_TYPE_ENUM;
    ok[2].enums = MODES; ok[2].n_enums = 3U;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_functions_add(ok, 3U));

    static kode_function_t bad[1];
    bad[0] = one_fn("idle"); bad[0].run = NULL;
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_functions_add(bad, 1U));     /* no run */
    bad[0] = one_fn("mode"); bad[0].arg = KODE_TYPE_ENUM;
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_functions_add(bad, 1U));     /* ENUM, no words */
    bad[0].enums = MODES; bad[0].n_enums = 7U;
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_functions_add(bad, 1U));     /* too many */
    bad[0] = one_fn("lim"); bad[0].arg = KODE_TYPE_INT;
    bad[0].min = 10; bad[0].max = 5;
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_functions_add(bad, 1U));     /* min > max */
    bad[0] = one_fn("long");
    bad[0].desc = "0123456789012345678901234567890123456789012345678";   /* 49 */
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_functions_add(bad, 1U));
    /* An answering row's wait is bounded, so its ticks cannot overflow. */
    bad[0] = one_fn("slow");
    bad[0].answer_ms = KODE_FUNCTION_ANSWER_MS_MAX + 1U;
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_functions_add(bad, 1U));
    bad[0].answer_ms = KODE_FUNCTION_ANSWER_MS_MAX;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_functions_add(bad, 1U));
    TEST_ASSERT_EQUAL_UINT(4U, kfunctions_count());
}

TEST_CASE("an argument type outside the enum is refused before anything indexes it",
          "[kregistry]")
{
    fresh();
    static kode_function_t a[1];
    a[0] = one_fn("stray");
    a[0].arg = (kode_type_t)5;
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_functions_add(a, 1U));
    TEST_ASSERT_EQUAL_UINT(0U, kfunctions_count());
}

TEST_CASE("an enum word that is not a valid name is refused", "[kregistry]")
{
    static const char *const BAD[] = { "off", "Slow" };
    fresh();
    static kode_function_t t[1];
    t[0] = one_fn("mode");
    t[0].arg = KODE_TYPE_ENUM; t[0].enums = BAD; t[0].n_enums = 2U;
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_functions_add(t, 1U));
}

/* ---- A function's argument -------------------------------------------------- */

TEST_CASE("an int parses with its sign and inside its bounds", "[kregistry]")
{
    kode_function_t r = one_fn("t"); r.arg = KODE_TYPE_INT; r.min = -400; r.max = 1250;
    kode_value_t v;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfunctions_parse(&r, "-400", &v));
    TEST_ASSERT_EQUAL_INT32(-400, v.i);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfunctions_parse(&r, "1250", &v));
    TEST_ASSERT_EQUAL_INT32(1250, v.i);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kfunctions_parse(&r, "1251", &v));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kfunctions_parse(&r, "-401", &v));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kfunctions_parse(&r, "-", &v));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kfunctions_parse(&r, "", &v));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kfunctions_parse(&r, "1e3", &v));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kfunctions_parse(&r, "+5", &v));
}

TEST_CASE("an int with min equal to max is unbounded", "[kregistry]")
{
    kode_function_t r = one_fn("t"); r.arg = KODE_TYPE_INT;
    kode_value_t v;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfunctions_parse(&r, "2147483647", &v));
    TEST_ASSERT_EQUAL_INT32(2147483647, v.i);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfunctions_parse(&r, "-2147483648", &v));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kfunctions_parse(&r, "2147483648", &v));
}

TEST_CASE("bool, enum and string parse, and their near misses do not",
          "[kregistry]")
{
    kode_value_t v;
    kode_function_t b = one_fn("b"); b.arg = KODE_TYPE_BOOL;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfunctions_parse(&b, "on", &v));
    TEST_ASSERT_TRUE(v.b);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfunctions_parse(&b, "off", &v));
    TEST_ASSERT_FALSE(v.b);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kfunctions_parse(&b, "ON", &v));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kfunctions_parse(&b, "1", &v));

    kode_function_t e = one_fn("e"); e.arg = KODE_TYPE_ENUM;
    e.enums = MODES; e.n_enums = 3U;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfunctions_parse(&e, "fast", &v));
    TEST_ASSERT_EQUAL_UINT8(2U, v.e);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kfunctions_parse(&e, "faster", &v));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kfunctions_parse(&e, "fas", &v));

    /* A string argument is held to 256 characters, not to a variable's 32:
     * it carries URLs and time zones, and nothing puts it in a record. */
    char text[258];
    (void)memset(text, 'a', sizeof(text));
    text[256] = '\0';
    kode_function_t s = one_fn("s"); s.arg = KODE_TYPE_STRING;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfunctions_parse(&s, text, &v));        /* 256 */
    TEST_ASSERT_EQUAL_STRING(text, v.s);
    text[256] = 'a';
    text[257] = '\0';
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kfunctions_parse(&s, text, &v));    /* 257 */
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kfunctions_parse(&s, "", &v));

    kode_function_t n = one_fn("n");
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kfunctions_parse(&n, "1", &v));    /* takes none */
}

/* ---- A variable's word ------------------------------------------------------ */

TEST_CASE("a variable's word reaches the wire whole, whitespace made safe",
          "[kregistry]")
{
    char buf[KREGISTRY_WORD_MAX];
    kode_variable_t r = one_var("w");
    r.get = word_get;
    s_word_rc = KODE_OK;

    s_word = "-12";
    TEST_ASSERT_EQUAL_INT(KODE_OK, kvariables_read(&r, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("-12", buf);

    s_word = "kode dot\tv2\n";
    TEST_ASSERT_EQUAL_INT(KODE_OK, kvariables_read(&r, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("kode_dot_v2_", buf);

    s_word = "abcdefghijklmnopqrstuvwxyz012345";    /* 32: the cap itself */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kvariables_read(&r, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("abcdefghijklmnopqrstuvwxyz012345", buf);
}

TEST_CASE("an empty word, a long one or a failing get is refused, never cut",
          "[kregistry]")
{
    char buf[KREGISTRY_WORD_MAX];
    kode_variable_t r = one_var("w");
    r.get = word_get;
    s_word_rc = KODE_OK;

    s_word = "abcdefghijklmnopqrstuvwxyz0123456";   /* 33 */
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kvariables_read(&r, buf, sizeof(buf)));
    s_word = "abcdefghijklmnopqrstuvwxyz0123456789abcdef";   /* far over: cut by snprintf */
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kvariables_read(&r, buf, sizeof(buf)));
    s_word = "";
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kvariables_read(&r, buf, sizeof(buf)));

    s_word = "5";
    s_word_rc = KODE_IO_ERROR;
    TEST_ASSERT_EQUAL_INT(KODE_IO_ERROR, kvariables_read(&r, buf, sizeof(buf)));
    s_word_rc = KODE_OK;

    /* And a buffer too small to see an overrun in is refused before get runs. */
    char small[KREGISTRY_WORD_MAX - 1U];
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kvariables_read(&r, small, sizeof(small)));
}

/* ---- Lookup ----------------------------------------------------------------- */

TEST_CASE("find is exact, per kind, and at walks tables in registration order",
          "[kregistry]")
{
    fresh();
    static kode_variable_t a[2];
    static kode_variable_t b[1];
    static kode_function_t   x[1];
    a[0] = one_var("gain"); a[1] = one_var("temp_dc");
    b[0] = one_var("mode");
    x[0] = one_fn("blink");
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_variables_add(a, 2U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_variables_add(b, 1U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_functions_add(x, 1U));
    TEST_ASSERT_EQUAL_PTR(&a[1], kvariables_find("temp_dc"));
    TEST_ASSERT_NULL(kvariables_find("Temp_dc"));
    TEST_ASSERT_NULL(kvariables_find("temp"));
    TEST_ASSERT_EQUAL_STRING("gain",    kvariables_at(0U)->name);
    TEST_ASSERT_EQUAL_STRING("temp_dc", kvariables_at(1U)->name);
    TEST_ASSERT_EQUAL_STRING("mode",    kvariables_at(2U)->name);
    TEST_ASSERT_NULL(kvariables_at(3U));
    /* Each kind answers for its own names only. */
    TEST_ASSERT_EQUAL_PTR(&x[0], kfunctions_find("blink"));
    TEST_ASSERT_NULL(kvariables_find("blink"));
    TEST_ASSERT_NULL(kfunctions_find("gain"));
    /* The last case in the file, so nothing else calls fresh() after it: the
     * registry would otherwise hold these tables for the rest of the process.
     * Every case above leaves static storage registered, which is what
     * registration asks for, and this is the other half of the same rule - a
     * registry is emptied by its owner, not by a scope ending. */
    kvariables_reset();
    kfunctions_reset();
}

/* ---- Settings ---------------------------------------------------------------- */

static const char *const LOCKS[] = { "30s", "never" };

/** @brief A setting called @p name of type @p type. */
static kode_config_t one_cfg(const char *name, kode_type_t type)
{
    kode_config_t r;
    (void)memset(&r, 0, sizeof(r));
    r.name = name;
    r.type = type;
    return r;
}

TEST_CASE("a settings table registers, and find and at walk it in order", "[kregistry]")
{
    fresh();
    kconfig_reset();
    static kode_config_t t[2];
    t[0] = one_cfg("bright", KODE_TYPE_INT);
    t[0].min = 5; t[0].max = 100;
    t[1] = one_cfg("lock", KODE_TYPE_ENUM);
    t[1].enums = LOCKS; t[1].n_enums = 2U;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_config_add(ao_spy(), KODE_CONFIG_MARGIN, t, 2U));
    TEST_ASSERT_EQUAL_UINT(2U, kconfig_count());
    TEST_ASSERT_EQUAL_PTR(&t[1], kconfig_find("lock"));
    TEST_ASSERT_EQUAL_PTR(&t[0], kconfig_at(0U));
    TEST_ASSERT_NULL(kconfig_at(2U));
    TEST_ASSERT_NULL(kconfig_find("Lock"));
    kconfig_reset();
}

/* kodeOS alone registers nineteen settings across two tables, so the cap is
 * room for an app's table on top of them, not a ceiling it already brushes. */
TEST_CASE("the settings' cap is KODE_CONFIG_MAX rows, whole tables refused past it",
          "[kregistry]")
{
    static char          names[KODE_CONFIG_MAX + 1U][8];
    static kode_config_t big[KODE_CONFIG_MAX + 1U];

    fresh();
    kconfig_reset();
    for ( unsigned i = 0U; (KODE_CONFIG_MAX + 1U) > i; ++i )
    {
        (void)snprintf(names[i], sizeof(names[i]), "s%u", i);
        big[i] = one_cfg(names[i], KODE_TYPE_BOOL);
    }
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_config_add(ao_spy(), KODE_CONFIG_MARGIN, big, KODE_CONFIG_MAX + 1U));
    TEST_ASSERT_EQUAL_UINT(0U, kconfig_count());
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_config_add(ao_spy(), KODE_CONFIG_MARGIN, big, KODE_CONFIG_MAX));
    TEST_ASSERT_EQUAL_UINT(KODE_CONFIG_MAX, kconfig_count());
    kconfig_reset();
}

TEST_CASE("a setting with no type, a bad name or bad enums refuses its table", "[kregistry]")
{
    fresh();
    kconfig_reset();
    static kode_config_t t[1];
    t[0] = one_cfg("x", KODE_TYPE_NONE);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_config_add(ao_spy(), KODE_CONFIG_MARGIN, t, 1U));
    t[0] = one_cfg("Bad", KODE_TYPE_BOOL);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_config_add(ao_spy(), KODE_CONFIG_MARGIN, t, 1U));
    t[0] = one_cfg("e", KODE_TYPE_ENUM);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_config_add(ao_spy(), KODE_CONFIG_MARGIN, t, 1U));
    t[0] = one_cfg("i", KODE_TYPE_INT);
    t[0].min = 9; t[0].max = 1;
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_config_add(ao_spy(), KODE_CONFIG_MARGIN, t, 1U));
    TEST_ASSERT_EQUAL_UINT(0U, kconfig_count());
}

/* Every request for a table's keys is posted to the owner it was registered
 * with, leaving the margin it was registered with: the registry answers both
 * for a row, and refuses an owner that could never take one. */
TEST_CASE("a settings table keeps its owner and its margin", "[kregistry]")
{
    fresh();
    kconfig_reset();
    static kode_config_t t[2];
    static kode_config_t stray[1];
    uint16_t margin = 0U;
    stray[0] = one_cfg("lock", KODE_TYPE_BOOL);
    t[0] = one_cfg("lock", KODE_TYPE_BOOL);
    t[1] = one_cfg("clock", KODE_TYPE_BOOL);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_config_add(ao_spy(), AO_SPY_QUEUE_LEN - 1U, t, 2U));
    TEST_ASSERT_EQUAL_PTR(ao_spy(), kconfig_owner(kconfig_find("clock"), &margin));
    TEST_ASSERT_EQUAL_UINT16(AO_SPY_QUEUE_LEN - 1U, margin);
    TEST_ASSERT_NULL(kconfig_owner(NULL, &margin));
    TEST_ASSERT_NULL(kconfig_owner(&stray[0], &margin));
    kconfig_reset();
}

TEST_CASE("a settings table with no owner, an owner not started or a margin its "
          "queue cannot leave is refused", "[kregistry]")
{
    /* Constructed by nobody and never started: no level, no queue. */
    static QActive unstarted;
    static kode_config_t t[1];

    fresh();
    kconfig_reset();
    t[0] = one_cfg("lock", KODE_TYPE_BOOL);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_config_add(NULL, KODE_CONFIG_MARGIN, t, 1U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_config_add(&unstarted, KODE_CONFIG_MARGIN, t, 1U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_config_add(ao_spy(), AO_SPY_QUEUE_LEN, t, 1U));
    TEST_ASSERT_NOT_NULL(strstr(fake_klog_last(), "leaves no room in a queue of"));
    TEST_ASSERT_EQUAL_UINT(0U, kconfig_count());
    kconfig_reset();
}

TEST_CASE("a setting's key is taken only by another setting", "[kregistry]")
{
    fresh();
    kconfig_reset();
    static kode_function_t f[1];
    f[0] = one_fn("zone");
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_functions_add(f, 1U));
    static kode_config_t a[1];
    static kode_config_t b[2];
    a[0] = one_cfg("zone", KODE_TYPE_STRING);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_config_add(ao_spy(), KODE_CONFIG_MARGIN, a, 1U));
    b[0] = one_cfg("other", KODE_TYPE_BOOL);
    b[1] = one_cfg("zone", KODE_TYPE_BOOL);
    TEST_ASSERT_EQUAL_INT(KODE_EXISTS, kode_config_add(ao_spy(), KODE_CONFIG_MARGIN, b, 2U));
    b[1] = one_cfg("other", KODE_TYPE_BOOL);
    TEST_ASSERT_EQUAL_INT(KODE_EXISTS, kode_config_add(ao_spy(), KODE_CONFIG_MARGIN, b, 2U));
    TEST_ASSERT_EQUAL_UINT(1U, kconfig_count());
    kconfig_reset();
    kfunctions_reset();
}

TEST_CASE("a setting's string is held to a value word, its int to its bounds", "[kregistry]")
{
    kode_value_t v;
    kode_config_t s = one_cfg("zone", KODE_TYPE_STRING);
    kode_config_t i = one_cfg("bright", KODE_TYPE_INT);
    char longest[KODE_VALUE_STR_MAX + 2U];

    i.min = 5; i.max = 100;
    (void)memset(longest, 'a', sizeof(longest));
    longest[KODE_VALUE_STR_MAX] = '\0';
    TEST_ASSERT_EQUAL_INT(KODE_OK, kconfig_parse(&s, longest, &v));
    TEST_ASSERT_EQUAL_STRING(longest, v.s);
    longest[KODE_VALUE_STR_MAX] = 'a';
    longest[KODE_VALUE_STR_MAX + 1U] = '\0';
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kconfig_parse(&s, longest, &v));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kconfig_parse(&i, "40", &v));
    TEST_ASSERT_EQUAL_INT32(40, v.i);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kconfig_parse(&i, "4", &v));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kconfig_parse(&i, "", &v));
}
