/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_c5.c
 * @brief kdrv_c5: the coprocessor's table, filled from the link once and served from a snapshot.
 */
#include <string.h>

#include "unity.h"

#include "fakes.h"
#include "kdrv_c5.h"
/* The host's own esp-hosted version macros, from the suite's shim: the three
 * host_* rows must be exactly these, and a literal here would pass on the
 * day the component is bumped and the rows are not. */
#include "esp_hosted.h"

/** @brief Whether any captured log line contains @p needle. */
static bool logged(const char *needle)
{
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, needle) != NULL) )
        {
            return true;
        }
    }
    return false;
}

/** @brief Read @p f, asserting it answers, and return the value. */
static int32_t get(kdrv_c5_field_t f)
{
    int32_t v = -12345;

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kdrv_c5_get(f, &v),
                                  kdrv_c5_field_info(f)->name);
    return v;
}

/** @brief Assert that @p f is refused and its output untouched. */
static void refused(kdrv_c5_field_t f)
{
    int32_t v = -12345;

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, kdrv_c5_get(f, &v),
                                  kdrv_c5_field_info(f)->name);
    TEST_ASSERT_EQUAL_INT32(-12345, v);
}

/* ---- The table ------------------------------------------------------------- */

TEST_CASE("the table has a name, a unit and a range for every field", "[kdrv_c5]")
{
    for ( uint16_t i = 0U; (uint16_t)KDRV_C5_FIELD_MAX > i; ++i )
    {
        const kdrv_c5_field_info_t * const info =
            kdrv_c5_field_info((kdrv_c5_field_t)i);
        TEST_ASSERT_NOT_NULL(info);
        TEST_ASSERT_NOT_NULL(info->name);
        TEST_ASSERT_TRUE('\0' != info->name[0]);
        TEST_ASSERT_NOT_NULL(info->unit);
        TEST_ASSERT_TRUE(info->min <= info->max);
        TEST_ASSERT_EQUAL_UINT16(1U, info->step);
        TEST_ASSERT_FALSE_MESSAGE(info->writable, "the table has no knobs");
    }
    TEST_ASSERT_NULL(kdrv_c5_field_info(KDRV_C5_FIELD_MAX));
}

TEST_CASE("field_by_name resolves every printed name and refuses one nobody printed",
          "[kdrv_c5]")
{
    kdrv_c5_field_t f = KDRV_C5_FIELD_MAX;

    for ( uint16_t i = 0U; (uint16_t)KDRV_C5_FIELD_MAX > i; ++i )
    {
        const char * const name = kdrv_c5_field_info((kdrv_c5_field_t)i)->name;
        TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_field_by_name(name, &f));
        TEST_ASSERT_EQUAL_INT((int)i, (int)f);
    }
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_c5_field_by_name("ichg_ma", &f));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_c5_field_by_name(NULL, &f));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_c5_field_by_name("valid", NULL));
}

TEST_CASE("every write is refused: the table has no knobs", "[kdrv_c5]")
{
    kdrv_c5_forget();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_refresh());
    for ( uint16_t i = 0U; (uint16_t)KDRV_C5_FIELD_MAX > i; ++i )
    {
        TEST_ASSERT_EQUAL_INT(KODE_DENIED,
                              kdrv_c5_set((kdrv_c5_field_t)i, 1));
    }
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_c5_set(KDRV_C5_FIELD_MAX, 1));
}

/* ---- Before and after a refresh --------------------------------------------- */

/* A snapshot nobody has taken answers exactly the rows that are not about
 * the C5 at all - this image's esp-hosted version and the two facts the link
 * itself reported - and refuses the rest rather than serving zeros. */
TEST_CASE("before any refresh only the driver's own rows answer", "[kdrv_c5]")
{
    char    s[16];
    uint8_t mac[6];

    kdrv_c5_forget();

    TEST_ASSERT_EQUAL_INT32(0, get(KDRV_C5_VALID));
    TEST_ASSERT_EQUAL_INT32(0, get(KDRV_C5_APP_DESC));
    TEST_ASSERT_EQUAL_INT32(ESP_HOSTED_VERSION_MAJOR_1, get(KDRV_C5_HOST_MAJOR));
    (void)get(KDRV_C5_RESET_REASON);
    (void)get(KDRV_C5_LINK_MS);

    refused(KDRV_C5_CHIP_ID);
    refused(KDRV_C5_FW_MAJOR);
    refused(KDRV_C5_PROJECT_KODE);
    refused(KDRV_C5_MAC_HI);
    refused(KDRV_C5_HEAP_FREE_KB);
    refused(KDRV_C5_SDIO_WIDTH);
    refused(KDRV_C5_SD_CD);

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_c5_target(s, sizeof(s)));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_c5_project_name(s, sizeof(s)));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_c5_mac(mac));
}

TEST_CASE("a refresh takes the slave's identity, version and MAC into the snapshot",
          "[kdrv_c5]")
{
    char    s[16];
    uint8_t mac[6];

    kdrv_c5_forget();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_refresh());

    TEST_ASSERT_EQUAL_INT32(1, get(KDRV_C5_VALID));
    TEST_ASSERT_EQUAL_INT32(KDRV_C5_CHIP_ESP32C5, get(KDRV_C5_CHIP_ID));
    TEST_ASSERT_EQUAL_INT32(2, get(KDRV_C5_FW_MAJOR));
    TEST_ASSERT_EQUAL_INT32(12, get(KDRV_C5_FW_MINOR));
    TEST_ASSERT_EQUAL_INT32(9, get(KDRV_C5_FW_PATCH));
    TEST_ASSERT_EQUAL_INT32(0x3844, get(KDRV_C5_MAC_HI));
    TEST_ASSERT_EQUAL_INT32(0xBE1C, get(KDRV_C5_MAC_MID));
    TEST_ASSERT_EQUAL_INT32(0x1D7E, get(KDRV_C5_MAC_LO));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_target(s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("esp32c5", s);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_mac(mac));
    TEST_ASSERT_EQUAL_HEX8(0x38, mac[0]);
    TEST_ASSERT_EQUAL_HEX8(0x7E, mac[5]);
}

TEST_CASE("the host version rows are the esp-hosted host macros", "[kdrv_c5]")
{
    TEST_ASSERT_EQUAL_INT32(ESP_HOSTED_VERSION_MAJOR_1, get(KDRV_C5_HOST_MAJOR));
    TEST_ASSERT_EQUAL_INT32(ESP_HOSTED_VERSION_MINOR_1, get(KDRV_C5_HOST_MINOR));
    TEST_ASSERT_EQUAL_INT32(ESP_HOSTED_VERSION_PATCH_1, get(KDRV_C5_HOST_PATCH));
}

/* ---- The descriptor ---------------------------------------------------------- */

TEST_CASE("project_kode is true for kode_c5 and false for a stock slave",
          "[kdrv_c5]")
{
    char s[32];

    kdrv_c5_forget();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_refresh());
    TEST_ASSERT_EQUAL_INT32(1, get(KDRV_C5_APP_DESC));
    TEST_ASSERT_EQUAL_INT32(1, get(KDRV_C5_PROJECT_KODE));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_project_name(s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING(KDRV_C5_PROJECT_NAME, s);

    fake_hosted_set_project("network_adapter");
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_refresh());
    TEST_ASSERT_EQUAL_INT32(1, get(KDRV_C5_APP_DESC));
    TEST_ASSERT_EQUAL_INT32(0, get(KDRV_C5_PROJECT_KODE));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_project_name(s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("network_adapter", s);
}

TEST_CASE("the descriptor's strings come through, and the sha with them",
          "[kdrv_c5]")
{
    char    s[32];
    uint8_t sha[32];

    fake_hosted_set_app_strings("2.12.9", "v5.5.1", "Sep  5 2026", "10:00:00");
    kdrv_c5_forget();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_refresh());

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_app_version(s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("2.12.9", s);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_idf_ver(s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("v5.5.1", s);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_build_date(s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("Sep  5 2026", s);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_build_time(s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("10:00:00", s);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_elf_sha(sha));
    TEST_ASSERT_EQUAL_HEX8(0xA0, sha[0]);
    TEST_ASSERT_EQUAL_HEX8(0xBF, sha[31]);
}

/* A slave built with an esp-hosted from before the descriptor RPC still
 * answers everything else, and the table says which rows it could not. */
TEST_CASE("an unsupported app-descriptor RPC leaves the project rows unknown and the rest intact",
          "[kdrv_c5]")
{
    char s[32];

    fake_hosted_app_desc_unsupported(true);
    kdrv_c5_forget();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_refresh());

    TEST_ASSERT_EQUAL_INT32(1, get(KDRV_C5_VALID));
    TEST_ASSERT_EQUAL_INT32(0, get(KDRV_C5_APP_DESC));
    refused(KDRV_C5_PROJECT_KODE);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_c5_project_name(s, sizeof(s)));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_c5_build_date(s, sizeof(s)));
    TEST_ASSERT_EQUAL_INT32(KDRV_C5_CHIP_ESP32C5, get(KDRV_C5_CHIP_ID));
    TEST_ASSERT_EQUAL_INT32(0x3844, get(KDRV_C5_MAC_HI));
}

/* A slave that answers the descriptor without the full one leaves the build
 * date and time empty; an empty string is refused, not served. */
TEST_CASE("descriptor strings the slave left empty are refused, not served",
          "[kdrv_c5]")
{
    char s[32];

    fake_hosted_set_app_strings("2.12.9", "v5.5.1", "", "");
    kdrv_c5_forget();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_refresh());

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_app_version(s, sizeof(s)));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_c5_build_date(s, sizeof(s)));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_c5_build_time(s, sizeof(s)));
}

/* ---- The transport, the heap, the pins ---------------------------------------- */

TEST_CASE("the SDIO rows are what the transport negotiated", "[kdrv_c5]")
{
    kdrv_c5_forget();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_refresh());
    TEST_ASSERT_EQUAL_INT32(40000, get(KDRV_C5_SDIO_KHZ));
    TEST_ASSERT_EQUAL_INT32(4, get(KDRV_C5_SDIO_WIDTH));

    fake_hosted_set_sdio(20000U, 1U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_refresh());
    TEST_ASSERT_EQUAL_INT32(20000, get(KDRV_C5_SDIO_KHZ));
    TEST_ASSERT_EQUAL_INT32(1, get(KDRV_C5_SDIO_WIDTH));
}

TEST_CASE("the heap rows come from the one-shot memory monitor", "[kdrv_c5]")
{
    fake_hosted_set_heap(100U * 1024U, 50U * 1024U);
    kdrv_c5_forget();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_refresh());

    TEST_ASSERT_EQUAL_INT32(100, get(KDRV_C5_HEAP_FREE_KB));
    TEST_ASSERT_EQUAL_INT32(50, get(KDRV_C5_HEAP_LARGEST_KB));
    TEST_ASSERT_EQUAL_UINT(1U, fake_hosted_mem_queries());
}

TEST_CASE("sd_cd is the level the RPC expander reads back on IO6", "[kdrv_c5]")
{
    kdrv_c5_forget();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_refresh());
    TEST_ASSERT_EQUAL_INT32(1, get(KDRV_C5_SD_CD));

    fake_hosted_set_level(6U, 0);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_refresh());
    TEST_ASSERT_EQUAL_INT32(0, get(KDRV_C5_SD_CD));
}

/* The snapshot only reads the pin: the card's supply is the P4's own
 * (kdrv_sd drives it), so a refresh must not configure or write anything
 * on the C5. */
TEST_CASE("a refresh reads the pin and never configures or drives one",
          "[kdrv_c5]")
{
    kdrv_c5_forget();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_refresh());
    TEST_ASSERT_EQUAL_UINT(0U, fake_hosted_gpio_configs());
}

TEST_CASE("pwr_ext reads IO5 as the pull-up leaves it until it is driven",
          "[kdrv_c5]")
{
    kdrv_c5_forget();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_refresh());
    TEST_ASSERT_EQUAL_INT32(1, get(KDRV_C5_PWR_EXT));

    fake_hosted_set_level(5U, 0);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_refresh());
    TEST_ASSERT_EQUAL_INT32(0, get(KDRV_C5_PWR_EXT));
}

TEST_CASE("exp_power drives IO5 as an output it can read back, either way",
          "[kdrv_c5]")
{
    kdrv_c5_forget();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_refresh());

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_exp_power(false));
    TEST_ASSERT_EQUAL_UINT(1U, fake_hosted_gpio_configs());
    TEST_ASSERT_EQUAL_UINT32((uint32_t)H_CP_GPIO_MODE_INPUT_OUTPUT,
                             fake_hosted_last_gpio_mode());
    TEST_ASSERT_EQUAL_INT(0, fake_hosted_level(5U));
    TEST_ASSERT_EQUAL_INT32(0, get(KDRV_C5_PWR_EXT));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_exp_power(true));
    TEST_ASSERT_EQUAL_INT(1, fake_hosted_level(5U));
    TEST_ASSERT_EQUAL_INT32(1, get(KDRV_C5_PWR_EXT));
}

TEST_CASE("exp_power with a GPIO service that does not answer is an error",
          "[kdrv_c5]")
{
    fake_hosted_gpio_dead(true);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_c5_exp_power(false));
    TEST_ASSERT_TRUE(logged("would not drive the header's supply"));
}

TEST_CASE("a GPIO service that does not answer leaves the pin row unknown",
          "[kdrv_c5]")
{
    fake_hosted_gpio_dead(true);
    kdrv_c5_forget();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_refresh());

    TEST_ASSERT_EQUAL_INT32(1, get(KDRV_C5_VALID));
    refused(KDRV_C5_SD_CD);
    refused(KDRV_C5_PWR_EXT);
    TEST_ASSERT_EQUAL_INT32(KDRV_C5_CHIP_ESP32C5, get(KDRV_C5_CHIP_ID));
}

/* ---- Failure and forgetting --------------------------------------------------- */

TEST_CASE("a refresh with the link down fails and leaves the snapshot invalid",
          "[kdrv_c5]")
{
    kdrv_c5_forget();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_refresh());
    TEST_ASSERT_EQUAL_INT32(1, get(KDRV_C5_VALID));

    fake_hosted_link_down(true);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_c5_refresh());
    TEST_ASSERT_EQUAL_INT32(0, get(KDRV_C5_VALID));
    refused(KDRV_C5_CHIP_ID);
    refused(KDRV_C5_SD_CD);
}

TEST_CASE("forget clears everything but the driver's own rows", "[kdrv_c5]")
{
    kdrv_c5_note_reset(1);
    kdrv_c5_note_link_ms(1234U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_refresh());
    TEST_ASSERT_EQUAL_INT32(1, get(KDRV_C5_VALID));

    kdrv_c5_forget();
    TEST_ASSERT_EQUAL_INT32(0, get(KDRV_C5_VALID));
    TEST_ASSERT_EQUAL_INT32(0, get(KDRV_C5_APP_DESC));
    refused(KDRV_C5_CHIP_ID);
    refused(KDRV_C5_HEAP_FREE_KB);
    TEST_ASSERT_EQUAL_INT32(1, get(KDRV_C5_RESET_REASON));
    TEST_ASSERT_EQUAL_INT32(1234, get(KDRV_C5_LINK_MS));
    TEST_ASSERT_EQUAL_INT32(ESP_HOSTED_VERSION_MINOR_1, get(KDRV_C5_HOST_MINOR));
}

TEST_CASE("the link's two facts are recorded whatever the snapshot says",
          "[kdrv_c5]")
{
    kdrv_c5_forget();
    kdrv_c5_note_reset(3);
    kdrv_c5_note_link_ms(870U);
    TEST_ASSERT_EQUAL_INT32(3, get(KDRV_C5_RESET_REASON));
    TEST_ASSERT_EQUAL_INT32(870, get(KDRV_C5_LINK_MS));
}

/* ---- The dump -------------------------------------------------------------------- */

TEST_CASE("the dump logs every field and the strings the table cannot hold",
          "[kdrv_c5]")
{
    kdrv_c5_forget();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_refresh());
    fake_klog_reset();

    kdrv_c5_dump();

    TEST_ASSERT_TRUE(logged("chip_id=23"));
    TEST_ASSERT_TRUE(logged("sdio_width=4"));
    TEST_ASSERT_TRUE(logged("sd_cd=1"));
    TEST_ASSERT_TRUE(logged("esp32c5"));
    TEST_ASSERT_TRUE(logged("kode_c5"));
    TEST_ASSERT_TRUE(logged("38:44:be:1c:1d:7e"));
}

TEST_CASE("the dump marks the rows a refresh could not fill", "[kdrv_c5]")
{
    fake_hosted_app_desc_unsupported(true);
    kdrv_c5_forget();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_refresh());
    fake_klog_reset();

    kdrv_c5_dump();

    TEST_ASSERT_TRUE(logged("project_kode=?"));
    TEST_ASSERT_TRUE(logged("chip_id=23"));
}
