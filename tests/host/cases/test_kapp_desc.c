/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kapp_desc.c
 * @brief The manifest parser, over every way an image can lie.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "esp_app_desc.h"   /* to lay IDF's own descriptor in a prefix */
#include "kapp_cat.h"
#include "kapp_desc.h"

static uint8_t s_prefix[KAPP_PREFIX_LEN];

/**
 * @brief Lay a well-formed v2 manifest into a synthetic image prefix.
 * @param name     Display name to declare.
 * @param category Category list to declare, before normalisation.
 * @param out      Receives the struct as written, for cases that corrupt it;
 *                 may be NULL.
 */
static void put_v2(const char *name, const char *category, kapp_desc_t *out)
{
    kapp_desc_t d;

    (void)memset(&d, 0, sizeof(d));
    d.magic   = KAPP_DESC_MAGIC;
    d.version = KAPP_DESC_VERSION;
    (void)snprintf(d.name, sizeof(d.name), "%s", name);
    (void)snprintf(d.category, sizeof(d.category), "%s", category);

    (void)memset(s_prefix, 0, sizeof(s_prefix));
    (void)memcpy(&s_prefix[KAPP_DESC_OFF], &d, sizeof(d));

    if ( NULL != out )
    {
        *out = d;
    }
}

/** @brief Overwrite the manifest sitting in the prefix with @p d. */
static void put_raw(const kapp_desc_t *d)
{
    (void)memcpy(&s_prefix[KAPP_DESC_OFF], d, sizeof(*d));
}

TEST_CASE("a well-formed manifest parses and its category is lowered",
          "[kapp_desc]")
{
    kapp_desc_t got;

    put_v2("Kode Runner", "Game", NULL);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kapp_desc_parse(s_prefix, sizeof(s_prefix), &got));
    TEST_ASSERT_EQUAL_STRING("Kode Runner", got.name);
    TEST_ASSERT_EQUAL_STRING("game", got.category);
}

TEST_CASE("no magic means an app that never used the macro", "[kapp_desc]")
{
    kapp_desc_t good;
    kapp_desc_t got;

    put_v2("Kode Runner", "Game", &good);
    good.magic = 0U;
    put_raw(&good);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kapp_desc_parse(s_prefix, sizeof(s_prefix), &got));
}

TEST_CASE("a version from the future is refused, not guessed at", "[kapp_desc]")
{
    kapp_desc_t good;
    kapp_desc_t got;

    put_v2("Kode Runner", "Game", &good);
    good.version = KAPP_DESC_VERSION + 1U;
    put_raw(&good);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kapp_desc_parse(s_prefix, sizeof(s_prefix), &got));
}

/* Every app installed on every board before revision 2 is a v1 image. If this
 * case ever fails they all lose their category at once, and the symptom looks
 * like a broken launcher rather than a reader running 48 bytes too far. */
TEST_CASE("a version one image is read by its own size, not the struct's",
          "[kapp_desc]")
{
    struct {
        uint32_t magic;
        uint32_t version;
        char     name[KAPP_NAME_MAX];
        char     category[KAPP_CATEGORY_ITEM_MAX];
        uint32_t reserved[8];
    } v1 = { .magic = KAPP_DESC_MAGIC, .version = 1U,
             .name = "Old App", .category = "System" };
    kapp_desc_t got;

    TEST_ASSERT_EQUAL_UINT(KAPP_DESC_V1_SIZE, sizeof(v1));

    (void)memset(s_prefix, 0xA5, sizeof(s_prefix));   /* garbage after it */
    (void)memcpy(&s_prefix[KAPP_DESC_OFF], &v1, sizeof(v1));

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kapp_desc_parse(s_prefix, sizeof(s_prefix), &got));
    TEST_ASSERT_EQUAL_STRING("Old App", got.name);
    TEST_ASSERT_EQUAL_STRING("system", got.category);
}

TEST_CASE("several categories are normalised on read", "[kapp_desc]")
{
    kapp_desc_t got;

    put_v2("Kode Runner", "Game, demo ,game", NULL);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kapp_desc_parse(s_prefix, sizeof(s_prefix), &got));
    TEST_ASSERT_EQUAL_STRING("game,demo", got.category);
}

TEST_CASE("an unterminated category is refused", "[kapp_desc]")
{
    kapp_desc_t good;
    kapp_desc_t got;

    put_v2("Kode Runner", "Game", &good);
    (void)memset(good.category, 'x', sizeof(good.category));
    put_raw(&good);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kapp_desc_parse(s_prefix, sizeof(s_prefix), &got));
}

TEST_CASE("an unterminated name is refused", "[kapp_desc]")
{
    kapp_desc_t good;
    kapp_desc_t got;

    put_v2("Kode Runner", "Game", &good);
    (void)memset(good.name, 'x', sizeof(good.name));
    put_raw(&good);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kapp_desc_parse(s_prefix, sizeof(s_prefix), &got));
}

TEST_CASE("an empty category is not a category", "[kapp_desc]")
{
    kapp_desc_t good;
    kapp_desc_t got;

    put_v2("Kode Runner", "Game", &good);
    good.category[0] = '\0';
    put_raw(&good);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kapp_desc_parse(s_prefix, sizeof(s_prefix), &got));
}

TEST_CASE("a prefix that stops short of the manifest is refused", "[kapp_desc]")
{
    kapp_desc_t got;

    put_v2("Kode Runner", "Game", NULL);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kapp_desc_parse(s_prefix, KAPP_DESC_OFF + 4U, &got));
}

TEST_CASE("a null argument is refused rather than dereferenced", "[kapp_desc]")
{
    kapp_desc_t got;

    put_v2("Kode Runner", "Game", NULL);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kapp_desc_parse(NULL, sizeof(s_prefix), &got));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kapp_desc_parse(s_prefix, sizeof(s_prefix), NULL));
}

TEST_CASE("an absent IDF descriptor yields no project name", "[kapp_desc]")
{
    char out[64];

    /* Nothing was written at KAPP_IDF_DESC_OFF, so the magic word is missing
     * and the reader must say so rather than returning whatever is there. */
    (void)memset(s_prefix, 0, sizeof(s_prefix));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kapp_project_name(s_prefix, sizeof(s_prefix), out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

/* ---- What the image header and IDF's descriptor say ----------------------
 * Two readers over the same prefix, both pure over bytes: the chip an image
 * was built for (the header's chip id, which is what keeps a P4 image out of
 * the C5's flash) and the project name plus version IDF stamped into it
 * (what a read-back after a coprocessor update is compared against).
 */

/** @brief Lay an image header with @p chip at the front of the prefix. */
static void put_header(uint16_t chip)
{
    (void)memset(s_prefix, 0, sizeof(s_prefix));
    s_prefix[0]  = 0xE9U;                       /* ESP_IMAGE_HEADER_MAGIC */
    s_prefix[12] = (uint8_t)(chip & 0xFFU);     /* esp_image_header_t.chip_id */
    s_prefix[13] = (uint8_t)(chip >> 8);
}

/** @brief The build date a descriptor carries unless a case sets another. */
#define BUILD_DATE "Oct 24 2026"
/** @brief The build time a descriptor carries unless a case sets another. */
#define BUILD_TIME "21:14:03"

/**
 * @brief Lay IDF's own descriptor at its fixed offset, on top of a header.
 * @param project The project name to stamp.
 * @param version The version to stamp.
 * @param out     Receives the descriptor as written, for cases that corrupt
 *                it; may be NULL.
 */
static void put_idf_desc_raw(const char *project, const char *version,
                             esp_app_desc_t *out)
{
    esp_app_desc_t d;

    (void)memset(&d, 0, sizeof(d));
    d.magic_word = ESP_APP_DESC_MAGIC_WORD;
    (void)snprintf(d.project_name, sizeof(d.project_name), "%s", project);
    (void)snprintf(d.version, sizeof(d.version), "%s", version);
    (void)snprintf(d.date, sizeof(d.date), "%s", BUILD_DATE);
    (void)snprintf(d.time, sizeof(d.time), "%s", BUILD_TIME);
    (void)memcpy(&s_prefix[KAPP_IDF_DESC_OFF], &d, sizeof(d));
    if ( NULL != out )
    {
        *out = d;
    }
}

/** @brief Lay IDF's own descriptor with the default date and time. */
static void put_idf_desc(const char *project, const char *version)
{
    put_idf_desc_raw(project, version, NULL);
}

/** @brief Overwrite the descriptor sitting in the prefix with @p d. */
static void put_idf_raw(const esp_app_desc_t *d)
{
    (void)memcpy(&s_prefix[KAPP_IDF_DESC_OFF], d, sizeof(*d));
}

TEST_CASE("the chip id is read from the image header", "[kapp_desc]")
{
    uint16_t chip = 0U;

    put_header(0x0017U);   /* ESP_CHIP_ID_ESP32C5 */
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kapp_image_chip(s_prefix, sizeof(s_prefix), &chip));
    TEST_ASSERT_EQUAL_HEX16(0x0017U, chip);

    put_header(0x0012U);   /* ESP_CHIP_ID_ESP32P4 */
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kapp_image_chip(s_prefix, sizeof(s_prefix), &chip));
    TEST_ASSERT_EQUAL_HEX16(0x0012U, chip);
}

TEST_CASE("a prefix without the image magic has no chip id", "[kapp_desc]")
{
    uint16_t chip = 0x1234U;

    put_header(0x0017U);
    s_prefix[0] = 0x00U;
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kapp_image_chip(s_prefix, sizeof(s_prefix), &chip));
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(0x1234U, chip,
        "a refused read must leave the output alone");
}

TEST_CASE("a prefix shorter than the image header has no chip id, and a null "
          "argument is refused rather than dereferenced", "[kapp_desc]")
{
    uint16_t chip = 0U;

    put_header(0x0017U);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kapp_image_chip(s_prefix, 8U, &chip));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kapp_image_chip(NULL, sizeof(s_prefix), &chip));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kapp_image_chip(s_prefix, sizeof(s_prefix), NULL));
}

TEST_CASE("the IDF descriptor's project name and version are both read",
          "[kapp_desc]")
{
    char project[32];
    char version[32];

    put_header(0x0017U);
    put_idf_desc("kode_c5", "2.12.9");
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kapp_idf_desc(s_prefix, sizeof(s_prefix),
                      project, sizeof(project), version, sizeof(version),
                      NULL, 0U, NULL, 0U));
    TEST_ASSERT_EQUAL_STRING("kode_c5", project);
    TEST_ASSERT_EQUAL_STRING("2.12.9", version);
}

TEST_CASE("the IDF descriptor's build date and time are read with the rest",
          "[kapp_desc]")
{
    char project[32];
    char version[32];
    char date[KAPP_IDF_DATE_MAX];
    char time_of_day[KAPP_IDF_TIME_MAX];

    put_header(0x0012U);
    put_idf_desc("blinky", "1.0.3");
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kapp_idf_desc(s_prefix, sizeof(s_prefix),
                      project, sizeof(project), version, sizeof(version),
                      date, sizeof(date), time_of_day, sizeof(time_of_day)));
    TEST_ASSERT_EQUAL_STRING("blinky", project);
    TEST_ASSERT_EQUAL_STRING("1.0.3", version);
    TEST_ASSERT_EQUAL_STRING("Oct 24 2026", date);
    TEST_ASSERT_EQUAL_STRING("21:14:03", time_of_day);
}

/* The compiler pads __DATE__ with a space where the day has one digit
 * ("Jan  1 2026"); it is copied as it stands, and a date that has the whole
 * array is still terminated. */
TEST_CASE("a build date keeps the compiler's padding and a full-width one "
          "still ends", "[kapp_desc]")
{
    esp_app_desc_t d;
    char           date[KAPP_IDF_DATE_MAX];
    char           time_of_day[KAPP_IDF_TIME_MAX];

    put_header(0x0012U);
    put_idf_desc_raw("blinky", "1.0.3", &d);
    (void)memset(d.date, 0, sizeof(d.date));
    (void)snprintf(d.date, sizeof(d.date), "%s", "Jan  1 2026");
    (void)memset(d.time, '9', sizeof(d.time) - 1U);   /* 15 chars and a NUL */
    put_idf_raw(&d);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kapp_idf_desc(s_prefix, sizeof(s_prefix), NULL, 0U, NULL, 0U,
                      date, sizeof(date), time_of_day, sizeof(time_of_day)));
    TEST_ASSERT_EQUAL_STRING("Jan  1 2026", date);
    TEST_ASSERT_EQUAL_STRING("999999999999999", time_of_day);
}

/* A date that fills its array without a terminator runs on into the time
 * that sits right behind it, and that into the next field. The copy stops at
 * the array's end whatever the buffer allows. */
TEST_CASE("a build date and time with no terminator are cut at their arrays, "
          "never read past", "[kapp_desc]")
{
    esp_app_desc_t d;
    char           date[KAPP_IDF_DATE_MAX + 24U];
    char           time_of_day[KAPP_IDF_TIME_MAX + 24U];

    put_header(0x0012U);
    put_idf_desc_raw("blinky", "1.0.3", &d);
    (void)memset(d.date, 'D', sizeof(d.date));
    (void)memset(d.time, 'T', sizeof(d.time));
    put_idf_raw(&d);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kapp_idf_desc(s_prefix, sizeof(s_prefix), NULL, 0U, NULL, 0U,
                      date, sizeof(date), time_of_day, sizeof(time_of_day)));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(KAPP_IDF_DATE_MAX, strlen(date),
        "the date ran past its array into the time");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(KAPP_IDF_TIME_MAX, strlen(time_of_day),
        "the time ran past its array into the next field");
    TEST_ASSERT_EQUAL_CHAR('D', date[KAPP_IDF_DATE_MAX - 1U]);
    TEST_ASSERT_EQUAL_CHAR('T', time_of_day[KAPP_IDF_TIME_MAX - 1U]);
}

TEST_CASE("a buffer smaller than the date or the time takes what fits and "
          "ends", "[kapp_desc]")
{
    char date[4];
    char time_of_day[6];

    put_header(0x0012U);
    put_idf_desc("blinky", "1.0.3");
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kapp_idf_desc(s_prefix, sizeof(s_prefix), NULL, 0U, NULL, 0U,
                      date, sizeof(date), time_of_day, sizeof(time_of_day)));
    TEST_ASSERT_EQUAL_STRING("Oct", date);
    TEST_ASSERT_EQUAL_STRING("21:14", time_of_day);
}

TEST_CASE("an absent IDF descriptor yields no project name, version, date or "
          "time", "[kapp_desc]")
{
    char project[32]     = "stale";
    char version[32]     = "stale";
    char date[16]        = "stale";
    char time_of_day[16] = "stale";

    (void)memset(s_prefix, 0, sizeof(s_prefix));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kapp_idf_desc(s_prefix, sizeof(s_prefix),
                      project, sizeof(project), version, sizeof(version),
                      date, sizeof(date), time_of_day, sizeof(time_of_day)));
    TEST_ASSERT_EQUAL_STRING("", project);
    TEST_ASSERT_EQUAL_STRING("", version);
    TEST_ASSERT_EQUAL_STRING("", date);
    TEST_ASSERT_EQUAL_STRING("", time_of_day);
}

TEST_CASE("any part of the descriptor read may be left out", "[kapp_desc]")
{
    char project[32];
    char version[32];
    char date[KAPP_IDF_DATE_MAX];
    char time_of_day[KAPP_IDF_TIME_MAX];

    put_header(0x0017U);
    put_idf_desc("kode_c5", "2.12.9");
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kapp_idf_desc(s_prefix, sizeof(s_prefix), project, sizeof(project),
                      NULL, 0U, NULL, 0U, NULL, 0U));
    TEST_ASSERT_EQUAL_STRING("kode_c5", project);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kapp_idf_desc(s_prefix, sizeof(s_prefix), NULL, 0U,
                      version, sizeof(version), NULL, 0U, NULL, 0U));
    TEST_ASSERT_EQUAL_STRING("2.12.9", version);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kapp_idf_desc(s_prefix, sizeof(s_prefix), NULL, 0U, NULL, 0U,
                      date, sizeof(date), NULL, 0U));
    TEST_ASSERT_EQUAL_STRING("Oct 24 2026", date);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kapp_idf_desc(s_prefix, sizeof(s_prefix), NULL, 0U, NULL, 0U,
                      NULL, 0U, time_of_day, sizeof(time_of_day)));
    TEST_ASSERT_EQUAL_STRING("21:14:03", time_of_day);
}
