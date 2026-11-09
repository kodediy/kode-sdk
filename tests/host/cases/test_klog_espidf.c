/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_klog_espidf.c
 * @brief The ESP-IDF log stream, re-emitted through kodeOS's formatter.
 */
#include <stdarg.h>
#include <string.h>

#include "unity.h"

#include "esp_log.h"

#include "fakes.h"
#include "kode_log.h"

/**
 * @brief Feed one line to whatever klog registered with ESP-IDF.
 *
 * @param text The line exactly as IDF would have produced it, newline and
 *             all. Passed as a `%s` argument rather than as the format, so a
 *             stray `%` in a test string cannot be read as a conversion.
 * @return What the hook returned, which is vsnprintf's count.
 */
static int call_hook(vprintf_like_t hook, const char *fmt, ...)
{
    va_list ap;
    int     rc;

    va_start(ap, fmt);
    rc = hook(fmt, ap);
    va_end(ap);
    return rc;
}

static int feed_idf(const char *text)
{
    /* Take the hook out and put it back, to get a pointer to it. */
    vprintf_like_t const hook = esp_log_set_vprintf(NULL);
    (void)esp_log_set_vprintf(hook);

    TEST_ASSERT_NOT_NULL(hook);

    /* A va_list has to be built by a variadic function; the hook's signature
     * takes one rather than varargs, because that is what IDF hands it. */
    return call_hook(hook, "%s", text);
}

/** @brief Install klog's hook. Idempotent; every case starts with it. */
static void hook_up(void)
{
    kode_log_init();
    fake_klog_reset();   /* drop kode_log_init()'s own announcement */
}

/* ---- The shape IDF actually produces -------------------------------------- */

TEST_CASE("an IDF line becomes a kodeOS line, with kodeOS's own clock",
          "[klog_espidf]")
{
    hook_up();
    fake_time_advance_ms(77U);

    (void)feed_idf("I (1234) wifi: connected\n");

    /* IDF's own 1234 is dropped: the tick is ours, so two sources of lines on
     * one wire share one time base. */
    TEST_ASSERT_EQUAL_STRING("!D 77 wifi connected\n", fake_klog_last());
}

TEST_CASE("every IDF level maps to a kodeOS level", "[klog_espidf]")
{
    hook_up();

    (void)feed_idf("E (1) drv: bad\n");
    TEST_ASSERT_EQUAL_STRING("!E 0 drv bad\n", fake_klog_last());

    (void)feed_idf("W (1) drv: odd\n");
    TEST_ASSERT_EQUAL_STRING("!W 0 drv odd\n", fake_klog_last());

    /* A vendor's INFO is kodeOS's DEBUG: kodeOS tells at INFO what came up. */
    (void)feed_idf("I (1) drv: fine\n");
    TEST_ASSERT_EQUAL_STRING("!D 0 drv fine\n", fake_klog_last());

    /* Both of IDF's quiet levels land on kodeOS's one. */
    (void)feed_idf("D (1) drv: detail\n");
    TEST_ASSERT_EQUAL_STRING("!D 0 drv detail\n", fake_klog_last());

    (void)feed_idf("V (1) drv: more\n");
    TEST_ASSERT_EQUAL_STRING("!D 0 drv more\n", fake_klog_last());
}

/* ---- Every way the shape can fail ----------------------------------------- */

/* A line that is not IDF's shape is still a line, and losing it would be
 * worse than mis-tagging it. All five malformed shapes fall back to the tag
 * "esp" and keep the whole line as the message. */
TEST_CASE("a line that is not IDF's shape is kept whole under a generic tag",
          "[klog_espidf]")
{
    hook_up();

    /* No recognised level character at all. */
    (void)feed_idf("just some output\n");
    TEST_ASSERT_EQUAL_STRING("!D 0 esp just some output\n", fake_klog_last());

    /* Level, but no " (" after it. */
    (void)feed_idf("I nope\n");
    TEST_ASSERT_EQUAL_STRING("!D 0 esp I nope\n", fake_klog_last());

    /* An opening parenthesis that never closes. */
    (void)feed_idf("I (1234 wifi: hm\n");
    TEST_ASSERT_EQUAL_STRING("!D 0 esp I (1234 wifi: hm\n", fake_klog_last());

    /* Closed, but not followed by a space. */
    (void)feed_idf("I (1234)wifi: hm\n");
    TEST_ASSERT_EQUAL_STRING("!D 0 esp I (1234)wifi: hm\n", fake_klog_last());

    /* A tag with no colon to end it. */
    (void)feed_idf("I (1234) wifi connected\n");
    TEST_ASSERT_EQUAL_STRING("!D 0 esp I (1234) wifi connected\n",
                             fake_klog_last());
}

/* The level is still read even when the rest of the shape is wrong, so an
 * IDF error that lost its tag does not quietly become an info line. */
TEST_CASE("a malformed line keeps its level", "[klog_espidf]")
{
    hook_up();

    (void)feed_idf("E (1234) no colon here\n");
    TEST_ASSERT_EQUAL_STRING("!E 0 esp E (1234) no colon here\n",
                             fake_klog_last());
}

/* ---- Trailing whitespace -------------------------------------------------- */

/* IDF ends its lines; klog ends its own. Left alone the two would double up
 * and a host reading lines would see a blank record between every pair. */
TEST_CASE("IDF's own line endings are stripped, not doubled", "[klog_espidf]")
{
    hook_up();

    (void)feed_idf("I (1) drv: one\n");
    TEST_ASSERT_EQUAL_STRING("!D 0 drv one\n", fake_klog_last());

    (void)feed_idf("I (1) drv: two\r\n");
    TEST_ASSERT_EQUAL_STRING("!D 0 drv two\n", fake_klog_last());

    /* Several of them, which the vendor components do produce. */
    (void)feed_idf("I (1) drv: three\r\n\r\n");
    TEST_ASSERT_EQUAL_STRING("!D 0 drv three\n", fake_klog_last());

    /* And a line with none at all still gets exactly one. */
    (void)feed_idf("I (1) drv: four");
    TEST_ASSERT_EQUAL_STRING("!D 0 drv four\n", fake_klog_last());
}

TEST_CASE("a message that is only whitespace collapses to nothing",
          "[klog_espidf]")
{
    hook_up();

    (void)feed_idf("I (1) drv: \n");
    TEST_ASSERT_EQUAL_STRING("!D 0 drv \n", fake_klog_last());
}

/* ---- Bounds --------------------------------------------------------------- */

/* A tag longer than the parser's buffer is clamped rather than overrunning
 * it. This is the one place in the file where the truncation arithmetic is
 * written correctly, and it is worth keeping that way. */
TEST_CASE("an over-long IDF tag is clamped, not overrun", "[klog_espidf]")
{
    static char        big[200];
    char               line[300];
    const char        *out;

    hook_up();

    (void)memset(big, 'x', sizeof(big) - 1U);
    big[sizeof(big) - 1U] = '\0';
    (void)snprintf(line, sizeof(line), "I (1) %s: hello\n", big);

    (void)feed_idf(line);

    out = fake_klog_last();
    TEST_ASSERT_NOT_NULL(out);

    /* 31 characters of tag and a terminator is the parser's ceiling. */
    TEST_ASSERT_EQUAL_MEMORY("!D 0 xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx hello\n",
                             out, 42U);
}

/* ---- Filtering ------------------------------------------------------------ */

/* The per-tag level applies to the tag the parser found, not to "esp". That
 * is what makes `log_level tag="wifi" level=e` able to quiet the WiFi stack -
 * a verb whose whole purpose is silencing somebody else's component. */
TEST_CASE("a per-tag level silences an IDF component by its own name",
          "[klog_espidf]")
{
    hook_up();
    kode_log_set_level("wifi", KODE_LOG_ERROR);

    (void)feed_idf("I (1) wifi: chatter\n");
    (void)feed_idf("W (1) wifi: more chatter\n");
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());

    /* Its errors still get through, and other components are untouched. */
    (void)feed_idf("E (1) wifi: that matters\n");
    (void)feed_idf("I (1) other: unaffected\n");
    TEST_ASSERT_EQUAL_UINT(2U, fake_klog_count());

    kode_log_set_level("wifi", KODE_LOG_DEBUG);
}

/* IDF has no NOTICE. A captured INFO line is below it and a captured WARN
 * is above, so `level=n` quiets a component's chatter and keeps its
 * trouble, the same as it does for kodeOS's own lines. */
TEST_CASE("at NOTICE an IDF component keeps its warnings and loses its info",
          "[klog_espidf]")
{
    hook_up();
    kode_log_set_level("wifi", KODE_LOG_NOTICE);

    (void)feed_idf("I (1) wifi: chatter\n");
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());

    (void)feed_idf("W (1) wifi: that matters\n");
    TEST_ASSERT_EQUAL_UINT(1U, fake_klog_count());

    kode_log_set_level("wifi", KODE_LOG_DEBUG);
}

/* The return value is vsnprintf's count, because that is what a vprintf_like_t
 * promises its caller. IDF does not use it today, which is exactly why it
 * would rot unnoticed. */
TEST_CASE("the hook returns what it would have written", "[klog_espidf]")
{
    hook_up();

    int const n = feed_idf("I (1) drv: hello\n");
    TEST_ASSERT_EQUAL_INT((int)strlen("I (1) drv: hello\n"), n);
}

/* ---- Holds ----------------------------------------------------------------- */

/* ESP-IDF's HTTP client writes a request's headers at DEBUG, the bearer among
 * them. kdrv_net holds that tag at INFO for the length of a PUT, and the hold
 * has to survive the two setters an operator has: `config key="log" value=d`
 * (the global one, which ESP-IDF answers by forgetting every per-tag level)
 * and `log_level tag=` on the tag itself. */
TEST_CASE("a held tag stays quiet whatever the global and its own level say",
          "[klog_espidf]")
{
    kode_log_lvl_t const was = kode_log_get_level();

    hook_up();
    TEST_ASSERT_TRUE(kode_log_hold("HTTP_CLIENT", KODE_LOG_INFO));

    kode_log_set_level(NULL, KODE_LOG_DEBUG);
    (void)feed_idf("D (1) HTTP_CLIENT: Write header[3]: PUT /up/secret\n");
    KLOG_D("HTTP_CLIENT", "a line of klog's own");
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());

    kode_log_set_level("HTTP_CLIENT", KODE_LOG_DEBUG);
    (void)feed_idf("D (1) HTTP_CLIENT: Write header[3]: PUT /up/secret\n");
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());

    /* What the hold does not touch: the tag's louder lines, and every other
     * tag's quiet ones. */
    (void)feed_idf("W (1) HTTP_CLIENT: trouble\n");
    (void)feed_idf("D (1) other: detail\n");
    TEST_ASSERT_EQUAL_UINT(2U, fake_klog_count());

    /* Released, the tag is back at its own level. */
    kode_log_release("HTTP_CLIENT");
    (void)feed_idf("D (1) HTTP_CLIENT: detail\n");
    TEST_ASSERT_EQUAL_UINT(3U, fake_klog_count());
    TEST_ASSERT_EQUAL_STRING("!D 0 HTTP_CLIENT detail\n", fake_klog_last());

    kode_log_set_level(NULL, was);
}

/* A floor, not a level: a tag already quieter than the hold stays quieter. */
TEST_CASE("a hold never makes a quieter tag louder", "[klog_espidf]")
{
    hook_up();
    kode_log_set_level("HTTP_CLIENT", KODE_LOG_ERROR);
    TEST_ASSERT_TRUE(kode_log_hold("HTTP_CLIENT", KODE_LOG_INFO));

    (void)feed_idf("W (1) HTTP_CLIENT: chatter\n");
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());

    kode_log_release("HTTP_CLIENT");
    kode_log_set_level("HTTP_CLIENT", KODE_LOG_DEBUG);
}

TEST_CASE("a hold refuses no tag and a full table, and holding twice is one hold",
          "[klog_espidf]")
{
    static const char *const tags[KODE_LOG_HOLD_MAX] = { "t0", "t1", "t2", "t3" };

    hook_up();
    TEST_ASSERT_FALSE(kode_log_hold(NULL, KODE_LOG_INFO));
    kode_log_release(NULL);                          /* harmless */
    kode_log_release("never-held");                  /* harmless */

    for ( uint32_t i = 0U; KODE_LOG_HOLD_MAX > i; ++i )
    {
        TEST_ASSERT_TRUE(kode_log_hold(tags[i], KODE_LOG_INFO));
    }
    /* The same tag again takes no slot; a fifth tag finds none. */
    TEST_ASSERT_TRUE(kode_log_hold(tags[0], KODE_LOG_WARN));
    TEST_ASSERT_FALSE(kode_log_hold("t4", KODE_LOG_INFO));

    /* The second hold moved the floor of the first. */
    (void)feed_idf("I (1) t0: chatter\n");
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());

    for ( uint32_t i = 0U; KODE_LOG_HOLD_MAX > i; ++i )
    {
        kode_log_release(tags[i]);
    }
    TEST_ASSERT_TRUE(kode_log_hold("t4", KODE_LOG_INFO));
    kode_log_release("t4");
}
