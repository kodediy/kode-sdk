/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_ir_file.c
 * @brief The capture file's text: what the writer emits and what the reader accepts or refuses.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "kode.h"
#include "kode_ir_file.h"

/** @brief Bytes of the text a case builds. */
#define TEST_TEXT_BYTES 120000U

/** @brief The value of a duration that fills five digits. */
#define TEST_WIDE_US 65535U

/** @brief The carrier that fills seven digits. */
#define TEST_WIDE_HZ 1000000U

/** @brief The signals a case reads into: one more than a file may carry. */
static kode_ir_signal_t s_sig[KODE_IR_FILE_SIGNALS_MAX + 1U];

/** @brief The text a case builds or the writer fills. */
static char s_text[TEST_TEXT_BYTES];

/** @brief Read @p text into s_sig. @return what kode_ir_file_parse() answered. */
static bool parse_text(const char *text, uint32_t *n, uint32_t *bad_line, const char **why)
{
    return kode_ir_file_parse(text, (uint32_t)strlen(text), s_sig, KODE_IR_FILE_SIGNALS_MAX, n,
                              bad_line, why);
}

/** @brief Expect @p text to be refused at line @p line for @p reason. */
static void expect_refused(const char *text, uint32_t line, const char *reason)
{
    uint32_t    n   = 77U;
    uint32_t    bad = 0U;
    const char *why = NULL;

    TEST_ASSERT_FALSE_MESSAGE(parse_text(text, &n, &bad, &why), text);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(line, bad, text);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(reason, why, text);
}

/** @brief Expect @p text to read as @p want signals. */
static void expect_signals(const char *text, uint32_t want)
{
    uint32_t    n   = 77U;
    uint32_t    bad = 0U;
    const char *why = NULL;

    TEST_ASSERT_TRUE_MESSAGE(parse_text(text, &n, &bad, &why), why);
    TEST_ASSERT_EQUAL_UINT32(want, n);
}

/** @brief Append @p count durations of @p value, one per call to snprintf, to the case's text. */
static uint32_t text_durations(uint32_t at, uint32_t count, uint32_t value)
{
    for ( uint32_t i = 0U; i < count; i++ )
    {
        at += (uint32_t)snprintf(&s_text[at], sizeof(s_text) - at, "%" PRIu32 " ", value);
    }
    return at;
}

/* ---- The writer ------------------------------------------------------------- */

TEST_CASE("the writer emits the header, a blank line and one block per signal", "[kode_ir_file]")
{
    static kode_ir_signal_t sig[2];
    const char              want[] =
        "# kode ir capture v1\n"
        "\n"
        "nec 0x0012 0x00AB\n"
        "\n"
        "raw 38000\n"
        "1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16\n"
        "17 18\n";

    (void)memset(sig, 0, sizeof(sig));
    sig[0].address = 0x12U;
    sig[0].command = 0xABU;
    sig[1].raw        = true;
    sig[1].carrier_hz = 38000U;
    sig[1].count      = 18U;
    for ( uint32_t i = 0U; 18U > i; i++ )
    {
        sig[1].us[i] = (uint16_t)(i + 1U);
    }

    const uint32_t len = kode_ir_file_format(sig, 2U, s_text, sizeof(s_text));

    TEST_ASSERT_EQUAL_UINT32(sizeof(want) - 1U, len);
    TEST_ASSERT_EQUAL_MEMORY(want, s_text, len);
}

TEST_CASE("the writer's text reads back as the same signals", "[kode_ir_file]")
{
    static kode_ir_signal_t sig[3];
    uint32_t                n   = 0U;
    uint32_t                bad = 0U;
    const char             *why = NULL;

    (void)memset(sig, 0, sizeof(sig));
    sig[0].address = 0xFFFFU;
    sig[0].command = 0U;
    sig[1].raw        = true;
    sig[1].carrier_hz = 56000U;
    sig[1].count      = 5U;
    sig[1].us[0] = 9000U; sig[1].us[1] = 4500U; sig[1].us[2] = 560U;
    sig[1].us[3] = 1690U; sig[1].us[4] = TEST_WIDE_US;
    sig[2].raw        = true;
    sig[2].carrier_hz = 1U;
    sig[2].count      = KODE_IR_RAW_MAX;
    for ( uint32_t i = 0U; KODE_IR_RAW_MAX > i; i++ )
    {
        sig[2].us[i] = (uint16_t)(i + 1U);
    }

    const uint32_t len = kode_ir_file_format(sig, 3U, s_text, sizeof(s_text));

    TEST_ASSERT_TRUE(0U < len);
    TEST_ASSERT_TRUE_MESSAGE(kode_ir_file_parse(s_text, len, s_sig, KODE_IR_FILE_SIGNALS_MAX, &n,
                                                &bad, &why), why);
    TEST_ASSERT_EQUAL_UINT32(3U, n);
    TEST_ASSERT_FALSE(s_sig[0].raw);
    TEST_ASSERT_EQUAL_UINT32(0xFFFFU, s_sig[0].address);
    TEST_ASSERT_EQUAL_UINT32(0U, s_sig[0].command);
    TEST_ASSERT_TRUE(s_sig[1].raw);
    TEST_ASSERT_EQUAL_UINT32(56000U, s_sig[1].carrier_hz);
    TEST_ASSERT_EQUAL_UINT32(5U, s_sig[1].count);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(sig[1].us, s_sig[1].us, 5U);
    TEST_ASSERT_EQUAL_UINT32(1U, s_sig[2].carrier_hz);
    TEST_ASSERT_EQUAL_UINT32(KODE_IR_RAW_MAX, s_sig[2].count);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(sig[2].us, s_sig[2].us, KODE_IR_RAW_MAX);
}

TEST_CASE("the writer's longest text is the size the capture sizes its block by", "[kode_ir_file]")
{
    static kode_ir_signal_t sig[KODE_IR_FILE_SIGNALS_MAX];
    const uint32_t          max = kode_ir_file_size_max();

    for ( uint32_t s = 0U; KODE_IR_FILE_SIGNALS_MAX > s; s++ )
    {
        sig[s].raw        = true;
        sig[s].carrier_hz = TEST_WIDE_HZ;
        sig[s].count      = KODE_IR_RAW_MAX;
        for ( uint32_t i = 0U; KODE_IR_RAW_MAX > i; i++ )
        {
            sig[s].us[i] = TEST_WIDE_US;
        }
    }
    TEST_ASSERT_TRUE(0U < max);
    TEST_ASSERT_TRUE(sizeof(s_text) > max);
    TEST_ASSERT_EQUAL_UINT32(max, kode_ir_file_format(sig, KODE_IR_FILE_SIGNALS_MAX, s_text, max));
    /* One byte less does not fit. */
    TEST_ASSERT_EQUAL_UINT32(0U, kode_ir_file_format(sig, KODE_IR_FILE_SIGNALS_MAX, s_text, max - 1U));
}

TEST_CASE("the writer refuses what the reader would refuse, and a NULL", "[kode_ir_file]")
{
    static kode_ir_signal_t sig[1];

    (void)memset(sig, 0, sizeof(sig));
    TEST_ASSERT_TRUE(kode_ir_file_format(sig, 1U, s_text, sizeof(s_text)) > 0U);
    TEST_ASSERT_EQUAL_UINT32(0U, kode_ir_file_format(NULL, 1U, s_text, sizeof(s_text)));
    TEST_ASSERT_EQUAL_UINT32(0U, kode_ir_file_format(sig, 1U, NULL, sizeof(s_text)));
    TEST_ASSERT_EQUAL_UINT32(0U, kode_ir_file_format(sig, KODE_IR_FILE_SIGNALS_MAX + 1U, s_text, sizeof(s_text)));
    TEST_ASSERT_EQUAL_UINT32(0U, kode_ir_file_format(sig, 1U, s_text, 5U));
    sig[0].address = KODE_IR_FILE_VALUE_MAX + 1U;
    TEST_ASSERT_EQUAL_UINT32(0U, kode_ir_file_format(sig, 1U, s_text, sizeof(s_text)));
    sig[0].address = 0U;
    sig[0].raw     = true;
    TEST_ASSERT_EQUAL_UINT32(0U, kode_ir_file_format(sig, 1U, s_text, sizeof(s_text)));  /* no carrier, no durations */
    sig[0].carrier_hz = 38000U;
    sig[0].count      = 1U;
    sig[0].us[0]      = 0U;
    TEST_ASSERT_EQUAL_UINT32(0U, kode_ir_file_format(sig, 1U, s_text, sizeof(s_text)));  /* a zero duration */
    sig[0].us[0] = 1U;
    TEST_ASSERT_TRUE(kode_ir_file_format(sig, 1U, s_text, sizeof(s_text)) > 0U);
}

/* ---- The reader ------------------------------------------------------------- */

TEST_CASE("an empty text and a text of comments and blank lines hold no signal", "[kode_ir_file]")
{
    expect_signals("", 0U);
    expect_signals("\n\n\n", 0U);
    expect_signals("# kode ir capture v1\n\n   # nothing yet\n\t\n", 0U);
}

TEST_CASE("CRLF line ends, tabs and keywords in any case are accepted", "[kode_ir_file]")
{
    uint32_t    n   = 0U;
    uint32_t    bad = 0U;
    const char *why = NULL;

    TEST_ASSERT_TRUE_MESSAGE(parse_text("# c\r\n\r\nNEC\t0x12\t 34\r\n\r\nRaw 38000\r\n9000\t4500\r\n560\r\n",
                                        &n, &bad, &why), why);
    TEST_ASSERT_EQUAL_UINT32(2U, n);
    TEST_ASSERT_FALSE(s_sig[0].raw);
    TEST_ASSERT_EQUAL_UINT32(0x12U, s_sig[0].address);
    TEST_ASSERT_EQUAL_UINT32(34U, s_sig[0].command);
    TEST_ASSERT_TRUE(s_sig[1].raw);
    TEST_ASSERT_EQUAL_UINT32(38000U, s_sig[1].carrier_hz);
    TEST_ASSERT_EQUAL_UINT32(3U, s_sig[1].count);
    TEST_ASSERT_EQUAL_UINT16(560U, s_sig[1].us[2]);
}

TEST_CASE("numbers are decimal or 0x hex, in any case", "[kode_ir_file]")
{
    uint32_t    n   = 0U;
    uint32_t    bad = 0U;
    const char *why = NULL;

    TEST_ASSERT_TRUE_MESSAGE(parse_text("nec 0XfF 0xAb\n\nnec 65535 0\n", &n, &bad, &why), why);
    TEST_ASSERT_EQUAL_UINT32(2U, n);
    TEST_ASSERT_EQUAL_UINT32(0xFFU, s_sig[0].address);
    TEST_ASSERT_EQUAL_UINT32(0xABU, s_sig[0].command);
    TEST_ASSERT_EQUAL_UINT32(65535U, s_sig[1].address);
    TEST_ASSERT_EQUAL_UINT32(0U, s_sig[1].command);
}

TEST_CASE("the durations may follow the keyword on its line, and comments sit anywhere", "[kode_ir_file]")
{
    uint32_t    n   = 0U;
    uint32_t    bad = 0U;
    const char *why = NULL;

    TEST_ASSERT_TRUE_MESSAGE(parse_text("raw 38000 100 200\n# between\n  300\n\nnec 1 2", &n, &bad, &why), why);
    TEST_ASSERT_EQUAL_UINT32(2U, n);
    TEST_ASSERT_EQUAL_UINT32(3U, s_sig[0].count);
    TEST_ASSERT_EQUAL_UINT16(300U, s_sig[0].us[2]);
    TEST_ASSERT_EQUAL_UINT32(2U, s_sig[1].command);
}

TEST_CASE("an unknown keyword, a short or long nec line and a bad number are refused with their line",
          "[kode_ir_file]")
{
    expect_refused("sony 1 2\n", 1U, "unknown signal type (use nec or raw)");
    expect_refused("# kode ir capture v1\n\nnec 1\n", 3U, "nec needs an address and a command");
    expect_refused("nec\n", 1U, "nec needs an address and a command");
    expect_refused("nec 1 2 3\n", 1U, "nec takes only an address and a command");
    expect_refused("nec 70000 1\n", 1U, "out of range (0 to 65535)");
    expect_refused("nec 1 0x10000\n", 1U, "out of range (0 to 65535)");
    expect_refused("nec 1 99999999999999\n", 1U, "out of range (0 to 65535)");
    expect_refused("nec one 2\n", 1U, "not a number");
    expect_refused("nec 0x 2\n", 1U, "not a number");
    expect_refused("nec -1 2\n", 1U, "not a number");
    expect_refused("nec 1 2x\n", 1U, "not a number");
    expect_refused("\n\n   necx 1 2\n", 3U, "unknown signal type (use nec or raw)");
}

TEST_CASE("a raw block with no carrier, a bad carrier, no durations or a bad duration is refused", "[kode_ir_file]")
{
    expect_refused("raw\n", 1U, "raw needs a carrier in Hz");
    expect_refused("raw 0\n", 1U, "carrier out of range (1 to 1000000)");
    expect_refused("raw 1000001\n", 1U, "carrier out of range (1 to 1000000)");
    expect_refused("raw khz\n", 1U, "not a number");
    /* The block is wrong where it began. */
    expect_refused("nec 1 2\n\nraw 38000\n", 3U, "raw has no durations");
    expect_refused("raw 38000\n\nnec 1 2\n", 1U, "raw has no durations");
    expect_refused("raw 38000\n100 0 200\n", 2U, "duration out of range (1 to 65535)");
    expect_refused("raw 38000\n100 65536\n", 2U, "duration out of range (1 to 65535)");
    expect_refused("raw 38000\n100 abc\n", 2U, "not a number");
}

TEST_CASE("a raw block holds up to 512 durations and the 513th is refused at its line", "[kode_ir_file]")
{
    uint32_t    n   = 0U;
    uint32_t    bad = 0U;
    const char *why = NULL;
    uint32_t    at  = (uint32_t)snprintf(s_text, sizeof(s_text), "raw 38000\n");

    at = text_durations(at, KODE_IR_RAW_MAX, 7U);
    TEST_ASSERT_TRUE_MESSAGE(parse_text(s_text, &n, &bad, &why), why);
    TEST_ASSERT_EQUAL_UINT32(1U, n);
    TEST_ASSERT_EQUAL_UINT32(KODE_IR_RAW_MAX, s_sig[0].count);

    at += (uint32_t)snprintf(&s_text[at], sizeof(s_text) - at, "\n8\n");
    expect_refused(s_text, 3U, "raw holds at most 512 durations");
}

TEST_CASE("a second signal without a blank line between is refused", "[kode_ir_file]")
{
    expect_refused("nec 1 2\nnec 3 4\n", 2U, "leave a blank line between signals");
    expect_refused("nec 1 2\n100\n", 2U, "leave a blank line between signals");
    expect_refused("raw 38000 1 2\nnec 3 4\n", 2U, "leave a blank line between signals");
    expect_refused("raw 38000 1 2\nraw 38000 3\n", 2U, "leave a blank line between signals");
}

TEST_CASE("a file holds 32 signals and the 33rd block is refused at its line", "[kode_ir_file]")
{
    uint32_t at = 0U;
    uint32_t n  = 0U;
    uint32_t bad = 0U;
    const char *why = NULL;

    for ( uint32_t i = 0U; KODE_IR_FILE_SIGNALS_MAX > i; i++ )
    {
        at += (uint32_t)snprintf(&s_text[at], sizeof(s_text) - at, "nec %" PRIu32 " 1\n\n", i);
    }
    TEST_ASSERT_TRUE_MESSAGE(parse_text(s_text, &n, &bad, &why), why);
    TEST_ASSERT_EQUAL_UINT32(KODE_IR_FILE_SIGNALS_MAX, n);
    TEST_ASSERT_EQUAL_UINT32(KODE_IR_FILE_SIGNALS_MAX - 1U, s_sig[KODE_IR_FILE_SIGNALS_MAX - 1U].address);

    at += (uint32_t)snprintf(&s_text[at], sizeof(s_text) - at, "nec 99 1\n");
    expect_refused(s_text, (2U * KODE_IR_FILE_SIGNALS_MAX) + 1U, "a file holds at most 32 signals");
}

TEST_CASE("the reader refuses NULL arguments without touching anything", "[kode_ir_file]")
{
    uint32_t    n   = 5U;
    uint32_t    bad = 0U;
    const char *why = NULL;

    TEST_ASSERT_FALSE(kode_ir_file_parse(NULL, 4U, s_sig, 1U, &n, &bad, &why));
    TEST_ASSERT_FALSE(kode_ir_file_parse("x", 1U, NULL, 1U, &n, &bad, &why));
    TEST_ASSERT_FALSE(kode_ir_file_parse("x", 1U, s_sig, 1U, NULL, &bad, &why));
    TEST_ASSERT_FALSE(kode_ir_file_parse("x", 1U, s_sig, 1U, &n, NULL, &why));
    TEST_ASSERT_FALSE(kode_ir_file_parse("x", 1U, s_sig, 1U, &n, &bad, NULL));
}

TEST_CASE("a NUL inside the text is an ordinary character in a word, never the end of the text",
          "[kode_ir_file]")
{
    static const char text[] = "nec 1 2\n\nnec 3\0 4\n\nnec 5 6\n";

    uint32_t    n   = 0U;
    uint32_t    bad = 0U;
    const char *why = NULL;

    TEST_ASSERT_FALSE(kode_ir_file_parse(text, (uint32_t)sizeof(text) - 1U, s_sig,
                                         KODE_IR_FILE_SIGNALS_MAX, &n, &bad, &why));
    TEST_ASSERT_EQUAL_UINT32(3U, bad);
    TEST_ASSERT_EQUAL_STRING("not a number", why);
}

TEST_CASE("the reader stops at the length it is given, not at the end of the string", "[kode_ir_file]")
{
    static const char text[] = "nec 1 2\n\nnec 3 4\n\nbogus\n";

    uint32_t    n   = 77U;
    uint32_t    bad = 0U;
    const char *why = NULL;

    /* Only the first signal and the blank line are in the text. */
    TEST_ASSERT_TRUE(kode_ir_file_parse(text, 9U, s_sig, KODE_IR_FILE_SIGNALS_MAX, &n, &bad, &why));
    TEST_ASSERT_EQUAL_UINT32(1U, n);
    /* Two signals are in the first 17 bytes. */
    TEST_ASSERT_TRUE(kode_ir_file_parse(text, 17U, s_sig, KODE_IR_FILE_SIGNALS_MAX, &n, &bad, &why));
    TEST_ASSERT_EQUAL_UINT32(2U, n);
    TEST_ASSERT_EQUAL_UINT32(3U, s_sig[1].address);
    TEST_ASSERT_EQUAL_UINT32(4U, s_sig[1].command);
    TEST_ASSERT_TRUE(kode_ir_file_parse(text, 0U, s_sig, KODE_IR_FILE_SIGNALS_MAX, &n, &bad, &why));
    TEST_ASSERT_EQUAL_UINT32(0U, n);
}

TEST_CASE("a raw line of 600 numbers is refused at that line, not overrun", "[kode_ir_file]")
{
    uint32_t at = (uint32_t)snprintf(s_text, sizeof(s_text), "raw 38000 ");

    (void)text_durations(at, 600U, 9U);
    expect_refused(s_text, 1U, "raw holds at most 512 durations");
}

TEST_CASE("a twelve digit duration, carrier, address and command are out of range, not wrapped",
          "[kode_ir_file]")
{
    expect_refused("raw 38000 100000000000\n", 1U, "duration out of range (1 to 65535)");
    expect_refused("raw 38000 4294967297\n", 1U, "duration out of range (1 to 65535)");
    expect_refused("raw 100000000000\n", 1U, "carrier out of range (1 to 1000000)");
    expect_refused("raw 4294967297 5\n", 1U, "carrier out of range (1 to 1000000)");
    expect_refused("nec 100000000000 1\n", 1U, "out of range (0 to 65535)");
    expect_refused("nec 1 0x100000000001\n", 1U, "out of range (0 to 65535)");
    /* Leading zeros are digits of a small number. */
    expect_signals("nec 1 000000000000000000042\n", 1U);
    TEST_ASSERT_EQUAL_UINT32(42U, s_sig[0].command);
}
