/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_sd.c
 * @brief kdrv_sd: the card's table, the bring-up retry, the write check on
 *        four lines, and the two knobs.
 */
#include <string.h>

#include "unity.h"

#include "Mockkhal_gpio.h"

#include "fakes.h"
#include "kbsp.h"
#include "kdrv_sd.h"
#include "kode_log.h"
#include "khal_sdmmc.h"
#include "khal_time.h"

/** @brief A card whose every field is a different, recognisable number. */
static void install_card(void)
{
    khal_sdmmc_card_t c;

    fake_sdmmc_default_card(&c);
    c.mfg_id   = 0x1BU;
    c.oem_id   = 0x534DU;
    (void)snprintf(c.name, sizeof(c.name), "%s", "EB1QT");
    c.revision = 0x30U;
    c.serial   = 0xA1B2C3D4U;
    c.date     = (uint16_t)((21U << 4) | 7U);   /* 2021-07 */

    c.csd_ver        = 1U;
    c.sectors        = 250347520U;              /* 122240 MB */
    c.sector_size    = 512U;
    c.read_block_len = 9U;          /* the CSD's exponent: 512 bytes */
    c.ccc            = 0x5B5U;
    c.tr_speed_khz   = 50000U;

    c.sd_spec     = 2U;
    c.bus_widths  = 0x05U;
    c.erase_state = true;

    c.au_kb           = 4096U;
    c.erase_size_au   = 3U;
    c.erase_timeout_s = 2U;
    c.erase_offset_s  = 1U;
    c.discard         = true;
    c.fule            = false;

    c.high_capacity = true;
    c.bus_width     = 4U;
    c.freq_khz      = 40000U;
    c.max_freq_khz  = 50000U;
    c.rca           = 0xAAAAU;
    c.is_mmc        = false;
    c.is_uhs1       = true;

    fake_sdmmc_set_card(&c);
    kdrv_sd_unmount();
    TEST_ASSERT_NOT_NULL(kdrv_sd_mount());
}

/** @brief Read one field, asserting it answered. */
static int32_t field(kdrv_sd_field_t f)
{
    int32_t value = -1;

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, (int)kdrv_sd_get(f, &value),
                                  "the field refused to answer");
    return value;
}

/* ---- With no card ---------------------------------------------------------- */

/* Everything the card said is refused while there is no card to have said it.
 * The alternative is answering a question about a card somebody removed, with
 * whatever the last one happened to be. */
TEST_CASE("with no card every field of the card is refused", "[kdrv_sd]")
{
    int32_t value = 0;

    kdrv_sd_unmount();

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)kdrv_sd_get(KDRV_SD_MFG_ID, &value));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)kdrv_sd_get(KDRV_SD_CAPACITY_MB, &value));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)kdrv_sd_get(KDRV_SD_BUS_WIDTH, &value));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)kdrv_sd_get(KDRV_SD_TOTAL_MB, &value));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)kdrv_sd_get(KDRV_SD_FREE_MB, &value));
}

/* The three rows that are this driver's own state answer whatever the slot is
 * doing - a refusal there would leave the console unable to ask the one
 * question it most wants to: is there a card? */
TEST_CASE("with no card the driver's own state still answers", "[kdrv_sd]")
{
    kdrv_sd_unmount();

    TEST_ASSERT_EQUAL_INT32(0, field(KDRV_SD_MOUNTED));
    TEST_ASSERT_EQUAL_INT32((int32_t)KDRV_SD_WIDTH_USE, field(KDRV_SD_WIDTH_REQ));
    TEST_ASSERT_EQUAL_INT32((int32_t)KBSP_SD_FREQ_KHZ, field(KDRV_SD_FREQ_REQ_KHZ));

    kdrv_sd_set_hosted(true);
    TEST_ASSERT_EQUAL_INT32(1, field(KDRV_SD_HOSTED));
    kdrv_sd_set_hosted(false);
    TEST_ASSERT_EQUAL_INT32(0, field(KDRV_SD_HOSTED));
}

TEST_CASE("with no card there is no name and no sector to read", "[kdrv_sd]")
{
    char    name[16];
    uint8_t buf[512];

    kdrv_sd_unmount();

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)kdrv_sd_name(name, sizeof(name)));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)kdrv_sd_read_sectors(0U, 1U, buf));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)kdrv_sd_write_sectors(0U, 1U, buf));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)kdrv_sd_format());
}

/* ---- The snapshot ---------------------------------------------------------- */

TEST_CASE("a mount takes the card's identity, and every field reads it back",
          "[kdrv_sd]")
{
    char name[16] = { 0 };

    install_card();

    TEST_ASSERT_EQUAL_INT32(1, field(KDRV_SD_MOUNTED));
    TEST_ASSERT_EQUAL_INT32(0x1B, field(KDRV_SD_MFG_ID));
    TEST_ASSERT_EQUAL_INT32(0x534D, field(KDRV_SD_OEM_ID));
    TEST_ASSERT_EQUAL_INT32(0x30, field(KDRV_SD_REVISION));

    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)kdrv_sd_name(name, sizeof(name)));
    TEST_ASSERT_EQUAL_STRING("EB1QT", name);

    TEST_ASSERT_EQUAL_INT32(1, field(KDRV_SD_CSD_VER));
    TEST_ASSERT_EQUAL_INT32(250347520, field(KDRV_SD_SECTORS));
    TEST_ASSERT_EQUAL_INT32(512, field(KDRV_SD_SECTOR_SIZE));
    /* The CSD holds READ_BL_LEN as an exponent and the seam passes it on
     * unchanged; a record saying 9 bytes would be a lie, so the shift is the
     * driver's. */
    TEST_ASSERT_EQUAL_INT32(512, field(KDRV_SD_READ_BLOCK_LEN));
    TEST_ASSERT_EQUAL_INT32(50000, field(KDRV_SD_TR_SPEED_KHZ));
    TEST_ASSERT_EQUAL_INT32(0x5B5, field(KDRV_SD_CCC));

    TEST_ASSERT_EQUAL_INT32(2, field(KDRV_SD_SD_SPEC));
    TEST_ASSERT_EQUAL_INT32(0x05, field(KDRV_SD_BUS_WIDTHS));
    TEST_ASSERT_EQUAL_INT32(1, field(KDRV_SD_ERASE_STATE));

    TEST_ASSERT_EQUAL_INT32(4096, field(KDRV_SD_AU_KB));
    TEST_ASSERT_EQUAL_INT32(3, field(KDRV_SD_ERASE_SIZE_AU));
    TEST_ASSERT_EQUAL_INT32(2, field(KDRV_SD_ERASE_TIMEOUT_S));
    TEST_ASSERT_EQUAL_INT32(1, field(KDRV_SD_ERASE_OFFSET_S));
    TEST_ASSERT_EQUAL_INT32(1, field(KDRV_SD_DISCARD));
    TEST_ASSERT_EQUAL_INT32(0, field(KDRV_SD_FULE));

    TEST_ASSERT_EQUAL_INT32(1, field(KDRV_SD_HIGH_CAPACITY));
    /* What was negotiated, not what the card offers: an everyday mount asks
     * for the slot's four data lines, whatever the card's SCR says it can do. */
    TEST_ASSERT_EQUAL_INT32((int32_t)KDRV_SD_WIDTH_USE, field(KDRV_SD_BUS_WIDTH));
    TEST_ASSERT_EQUAL_INT32(40000, field(KDRV_SD_FREQ_KHZ));
    TEST_ASSERT_EQUAL_INT32(50000, field(KDRV_SD_CARD_MAX_KHZ));
    TEST_ASSERT_EQUAL_INT32(0xAAAA, field(KDRV_SD_RCA));
    TEST_ASSERT_EQUAL_INT32(0, field(KDRV_SD_MMC));
    TEST_ASSERT_EQUAL_INT32(1, field(KDRV_SD_UHS1));
}

/* A 32-bit serial does not fit an int32 the console prints signed, and the
 * manufacturing date is two numbers packed into one register. Both come out
 * as the fields a person would ask for rather than as the bytes the card
 * sent. */
TEST_CASE("the serial and the date come apart into the fields they are",
          "[kdrv_sd]")
{
    install_card();

    TEST_ASSERT_EQUAL_INT32(0xA1B2, field(KDRV_SD_SERIAL_HI));
    TEST_ASSERT_EQUAL_INT32(0xC3D4, field(KDRV_SD_SERIAL_LO));
    TEST_ASSERT_EQUAL_INT32(2021, field(KDRV_SD_MFG_YEAR));
    TEST_ASSERT_EQUAL_INT32(7, field(KDRV_SD_MFG_MONTH));
}

TEST_CASE("capacity is the sectors times the sector size", "[kdrv_sd]")
{
    install_card();

    /* 250347520 sectors of 512 bytes is 122240 MB. */
    TEST_ASSERT_EQUAL_INT32(122240, field(KDRV_SD_CAPACITY_MB));
}

/* The volume is asked every time rather than snapshotted: how full a card is
 * changes with every file written to it. */
TEST_CASE("the two volume fields come from the filesystem, not the card",
          "[kdrv_sd]")
{
    install_card();
    fake_sdmmc_set_total_mb(1000U);

    TEST_ASSERT_EQUAL_INT32(1000, field(KDRV_SD_TOTAL_MB));
    TEST_ASSERT_EQUAL_INT32(1000, field(KDRV_SD_FREE_MB));
}

/* ---- The bring-up retry ---------------------------------------------------- */

/* Asked for four lines, the driver tries four first, then one. A card that
 * does not honour the 4-bit mode it advertises in its SCR is still worth
 * having on one line - what a 1-bit mount MEANS is judged higher up. */
TEST_CASE("a card that will not come up on four lines is mounted on one",
          "[kdrv_sd]")
{
    unsigned const before = fake_sdmmc_mounts();

    kdrv_sd_unmount();
    fake_sdmmc_set_max_width(1U);

    TEST_ASSERT_NOT_NULL(kdrv_sd_mount());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, fake_sdmmc_mounts() - before,
                                   "four lines, then one");
    TEST_ASSERT_EQUAL_UINT8(1U, fake_sdmmc_last_width());
    TEST_ASSERT_EQUAL_INT32(1, field(KDRV_SD_BUS_WIDTH));
}

TEST_CASE("a slot with no card is not retried into a mount", "[kdrv_sd]")
{
    kdrv_sd_unmount();
    fake_sdmmc_set_present(false);

    TEST_ASSERT_NULL(kdrv_sd_mount());
    TEST_ASSERT_EQUAL_INT32(0, field(KDRV_SD_MOUNTED));
}

/** @brief How many captured lines say the slot is empty for the first time. */
static unsigned told_no_card(void)
{
    unsigned told = 0U;

    for ( unsigned l = 0U; fake_klog_count() > l; ++l )
    {
        const char *line = fake_klog_line(l);

        if ( (NULL != line) && (NULL != strstr(line, "no card"))
             && (NULL == strstr(line, "still no card")) )
        {
            ++told;
        }
    }
    return told;
}

/** @brief How many captured lines are ESP-IDF's account of an empty slot. */
static unsigned idf_slot_lines(void)
{
    unsigned lines = 0U;

    for ( unsigned l = 0U; fake_klog_count() > l; ++l )
    {
        const char *line = fake_klog_line(l);

        if ( (NULL != line) && (NULL != strstr(line, FAKE_SLOT_LINE)) )
        {
            ++lines;
        }
    }
    return lines;
}

/** @brief How many captured lines are at WARN or ERROR. */
static unsigned loud_lines(void)
{
    unsigned loud = 0U;

    for ( unsigned l = 0U; fake_klog_count() > l; ++l )
    {
        const char *line = fake_klog_line(l);

        if ( (NULL != line) && (('E' == line[1]) || ('W' == line[1])) )
        {
            ++loud;
        }
    }
    return loud;
}

/* Every listing asks the slot again. The first miss is a notice, and the ones
 * after it are not news until a card has mounted in between. */
TEST_CASE("an empty slot is told once until a card mounts", "[kdrv_sd]")
{
    fake_sdmmc_set_present(true);
    install_card();
    kdrv_sd_unmount();
    fake_sdmmc_set_present(false);
    fake_klog_reset();

    TEST_ASSERT_NULL(kdrv_sd_mount());
    TEST_ASSERT_NULL(kdrv_sd_mount());
    TEST_ASSERT_NULL(kdrv_sd_mount());
    TEST_ASSERT_EQUAL_UINT(1U, told_no_card());

    fake_sdmmc_set_present(true);
    install_card();
    kdrv_sd_unmount();
    fake_sdmmc_set_present(false);
    fake_klog_reset();

    TEST_ASSERT_NULL(kdrv_sd_mount());
    TEST_ASSERT_EQUAL_UINT(1U, told_no_card());
}

/* ESP-IDF tells an empty slot in lines of its own, under three tags, as
 * errors. Every probe holds the tags quiet and the driver says it once, in a
 * line that is not a fault; a mount that ends the probe releases them. */
TEST_CASE("an empty slot's ESP-IDF lines are held quiet", "[kdrv_sd]")
{
    fake_sdmmc_set_present(true);
    install_card();
    kdrv_sd_unmount();
    fake_sdmmc_set_present(false);
    fake_klog_reset();

    TEST_ASSERT_NULL(kdrv_sd_mount());
    TEST_ASSERT_EQUAL_UINT(0U, idf_slot_lines());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, loud_lines(),
                                   "an empty slot is not a fault");
    TEST_ASSERT_EQUAL_UINT(1U, told_no_card());

    fake_klog_reset();
    TEST_ASSERT_NULL(kdrv_sd_mount());
    TEST_ASSERT_NULL(kdrv_sd_mount());
    TEST_ASSERT_EQUAL_UINT(0U, idf_slot_lines());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, loud_lines(), "nothing at W or E");

    /* The hold ends with the probe: the tags speak again. */
    KLOG_W(FAKE_SLOT_TAG_PINS, "a pin is in conflict");
    KLOG_E(FAKE_SLOT_TAG_PROTOCOL, "a transfer failed");
    TEST_ASSERT_EQUAL_UINT(2U, loud_lines());

    /* A card in the slot ends the miss: the next empty slot is told again. */
    fake_sdmmc_set_present(true);
    TEST_ASSERT_NOT_NULL(kdrv_sd_mount());
    kdrv_sd_unmount();
    fake_sdmmc_set_present(false);
    fake_klog_reset();
    TEST_ASSERT_NULL(kdrv_sd_mount());
    TEST_ASSERT_EQUAL_UINT(0U, idf_slot_lines());
    TEST_ASSERT_EQUAL_UINT(1U, told_no_card());
}

/* A card that answers and holds no FAT volume (exFAT, say) is not an empty
 * slot: one warning that says so, ESP-IDF's errors held, and one line only. */
TEST_CASE("a card with no FAT volume is told once, as itself", "[kdrv_sd]")
{
    fake_sdmmc_set_present(true);
    install_card();
    kdrv_sd_unmount();
    fake_sdmmc_set_no_volume(true);
    fake_klog_reset();

    TEST_ASSERT_NULL(kdrv_sd_mount());
    TEST_ASSERT_NULL(kdrv_sd_mount());
    TEST_ASSERT_EQUAL_UINT(0U, idf_slot_lines());
    TEST_ASSERT_EQUAL_UINT(0U, told_no_card());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, loud_lines(),
                                   "one warning, the driver's own");
    fake_sdmmc_set_no_volume(false);
}

/* While the link holds the controller and is down, the seam is not asked: no
 * try at one line, no "no card", and nothing at warning level or above. */
TEST_CASE("a mount with no controller is not tried and is not a missing card",
          "[kdrv_sd]")
{
    unsigned before;

    install_card();
    kdrv_sd_unmount();
    before = fake_sdmmc_mounts();
    fake_sdmmc_set_refuse(true);
    fake_klog_reset();
    kode_log_set_level(NULL, KODE_LOG_WARN);

    TEST_ASSERT_NULL(kdrv_sd_mount());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_sdmmc_mounts() - before, "no try");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_klog_count(), "nothing above debug is said");

    /* The link is back: the card is asked again, and a first miss still counts. */
    fake_sdmmc_set_refuse(false);
    fake_sdmmc_set_present(false);
    fake_klog_reset();
    TEST_ASSERT_NULL(kdrv_sd_mount());
    TEST_ASSERT_EQUAL_UINT(1U, told_no_card());
}

/* The controller can go between the query and the mount. A refusal is no
 * answer from the card either: one try, no second at one line, no report. */
TEST_CASE("a mount khal refuses is not retried and is not a missing card",
          "[kdrv_sd]")
{
    unsigned before;

    install_card();
    kdrv_sd_unmount();
    before = fake_sdmmc_mounts();
    fake_sdmmc_set_deny(true);
    fake_klog_reset();
    kode_log_set_level(NULL, KODE_LOG_WARN);

    TEST_ASSERT_NULL(kdrv_sd_mount());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_sdmmc_mounts() - before, "no try at one line");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_klog_count(), "nothing above debug is said");

    fake_sdmmc_set_deny(false);
    fake_sdmmc_set_present(false);
    fake_klog_reset();
    TEST_ASSERT_NULL(kdrv_sd_mount());
    TEST_ASSERT_EQUAL_UINT(1U, told_no_card());
}

/* ---- The write check on four lines ---------------------------------------- */

/** @brief Bytes in a sector of the cards the fake holds. */
#define TEST_SECTOR_BYTES  512U

/** @brief The last sector of the card a reset installs. */
static uint32_t default_last_sector(void)
{
    khal_sdmmc_card_t c;

    fake_sdmmc_default_card(&c);
    return c.sectors - 1U;
}

/**
 * @brief Assert call @p idx of the seam's trace reads @p want.
 * @param idx  0-based.
 * @param want The call, as the fake spells it.
 */
static void trace_is(unsigned idx, const char *want)
{
    char        what[48];
    const char *got = fake_sdmmc_trace_line(idx);

    (void)snprintf(what, sizeof(what), "call %u of the trace", idx);
    TEST_ASSERT_NOT_NULL_MESSAGE(got, what);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, got, what);
}

/** @brief The same for a call that names a number: `<name> <n>`. */
static void trace_is_n(unsigned idx, const char *name, uint32_t n)
{
    char want[40];

    (void)snprintf(want, sizeof(want), "%s %lu", name, (unsigned long)n);
    trace_is(idx, want);
}

/**
 * @brief How many captured lines at @p level carry @p text.
 * @param level 'N', 'W', 'E'...: the line's level letter.
 * @param text  What the line says.
 */
static unsigned lines_saying(char level, const char *text)
{
    unsigned n = 0U;

    for ( unsigned l = 0U; fake_klog_count() > l; ++l )
    {
        const char *line = fake_klog_line(l);

        if ( (NULL != line) && (level == line[1]) && (NULL != strstr(line, text)) )
        {
            ++n;
        }
    }
    return n;
}

/**
 * @brief The card's supply, written into the seam's trace so its order
 *        against the mounts shows in one list.
 */
static kode_status_t supply_to_trace(uint8_t pin, bool level, int calls)
{
    (void)calls;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KBSP_PIN_SD_PWR, pin);
    if ( level )
    {
        fake_sdmmc_trace_note("supply off");   /* SD_PWR high is Q1 off */
    }
    else
    {
        fake_sdmmc_trace_note("supply on");
    }
    return KODE_OK;
}

/** @brief Watch SD_PWR: from here on every drive of it shows in the trace. */
static void watch_supply(void)
{
    Mockkhal_gpio_Init();
    khal_gpio_output_StubWithCallback(&supply_to_trace);
    khal_gpio_driven_low_IgnoreAndReturn(false);
}

/** @brief Stop watching SD_PWR. */
static void unwatch_supply(void)
{
    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/** @brief Unmount, then forget everything counted, ready for one mount. */
static void ready_one_mount(void)
{
    kdrv_sd_unmount();
    fake_sdmmc_trace_reset();
    fake_psram_reset();
    fake_klog_reset();
}

/* The slot is wired for four lines and an everyday mount uses them. */
TEST_CASE("an everyday mount asks for the slot's four data lines", "[kdrv_sd]")
{
    kdrv_sd_unmount();

    TEST_ASSERT_EQUAL_UINT32((uint32_t)KBSP_SD_WIDTH, (uint32_t)KDRV_SD_WIDTH_USE);
    TEST_ASSERT_EQUAL_INT32((int32_t)KBSP_SD_WIDTH, field(KDRV_SD_WIDTH_REQ));
    TEST_ASSERT_NOT_NULL(kdrv_sd_mount());
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KBSP_SD_WIDTH, fake_sdmmc_last_width());
    TEST_ASSERT_EQUAL_INT32((int32_t)KBSP_SD_WIDTH, field(KDRV_SD_BUS_WIDTH));
}

/* The proof is a sector read and the same bytes written back at the far end:
 * no partition lives in the last sector, and what the card holds there is
 * what it held. */
TEST_CASE("a four-line mount whose last sector takes the write-back stays on "
          "four lines and changes nothing", "[kdrv_sd]")
{
    uint8_t        pattern[TEST_SECTOR_BYTES];
    uint8_t        back[TEST_SECTOR_BYTES];
    uint32_t const last = default_last_sector();

    for ( unsigned i = 0U; sizeof(pattern) > i; ++i )
    {
        pattern[i] = (uint8_t)((i * 7U) ^ 0xA5U);
    }
    /* A pattern in the last sector, put there through a mount on one line,
     * which checks nothing. */
    kdrv_sd_unmount();
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)kdrv_sd_set(KDRV_SD_WIDTH_REQ, 1));
    TEST_ASSERT_NOT_NULL(kdrv_sd_mount());
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)kdrv_sd_write_sectors(last, 1U, pattern));
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)kdrv_sd_set(KDRV_SD_WIDTH_REQ,
                                                    (int32_t)KBSP_SD_WIDTH));
    ready_one_mount();

    TEST_ASSERT_NOT_NULL(kdrv_sd_mount());

    TEST_ASSERT_EQUAL_INT32(4, field(KDRV_SD_BUS_WIDTH));
    TEST_ASSERT_EQUAL_UINT(3U, fake_sdmmc_trace_count());
    trace_is_n(0U, "mount", 4U);
    trace_is_n(1U, "read", last);
    trace_is_n(2U, "write", last);

    (void)memset(back, 0, sizeof(back));
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)kdrv_sd_read_sectors(last, 1U, back));
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(pattern, back, sizeof(pattern),
                                     "the check changed what the card holds");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(fake_psram_alloc_count(), fake_psram_free_count(),
                                   "the check's buffer is given back");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, lines_saying('N', "four lines"),
                                   "a card that passes is not news");

    /* A card already mounted is not checked again. */
    TEST_ASSERT_NOT_NULL(kdrv_sd_mount());
    TEST_ASSERT_EQUAL_UINT(4U, fake_sdmmc_trace_count());   /* the read above */
}

/* What a counterfeit card does: it reads on four lines and fails the first
 * write there. The controller is shared with the C5's link, so the way home is
 * the card's own supply, not the host. */
TEST_CASE("a write-back that fails ends in an unmount, a supply cycle and a "
          "one-line mount", "[kdrv_sd]")
{
    uint32_t const last = default_last_sector();
    uint32_t       t0;

    fake_sdmmc_fail_wide_writes(true);
    watch_supply();
    ready_one_mount();
    t0 = khal_time_ms();

    TEST_ASSERT_NOT_NULL(kdrv_sd_mount());

    TEST_ASSERT_EQUAL_UINT(8U, fake_sdmmc_trace_count());
    trace_is(0U, "supply on");
    trace_is_n(1U, "mount", 4U);
    trace_is_n(2U, "read", last);
    trace_is_n(3U, "write", last);
    trace_is(4U, "unmount");
    trace_is(5U, "supply off");
    trace_is(6U, "supply on");
    trace_is_n(7U, "mount", 1U);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(KDRV_SD_POWER_OFF_MS + KDRV_SD_POWER_SETTLE_MS,
                                     khal_time_ms() - t0,
                                     "the supply is off for its time and settles");

    /* The snapshot describes the mount that stands. */
    TEST_ASSERT_EQUAL_INT32(1, field(KDRV_SD_MOUNTED));
    TEST_ASSERT_EQUAL_INT32(1, field(KDRV_SD_BUS_WIDTH));
    TEST_ASSERT_EQUAL_UINT(1U, lines_saying('N', "takes no write on four lines"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, loud_lines(), "a card that works is no fault");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(fake_psram_alloc_count(), fake_psram_free_count(),
                                   "the check's buffer is given back");
    unwatch_supply();
}

TEST_CASE("a read-back that fails ends the same way", "[kdrv_sd]")
{
    uint32_t const last = default_last_sector();

    fake_sdmmc_fail_wide_reads(true);
    watch_supply();
    ready_one_mount();

    TEST_ASSERT_NOT_NULL(kdrv_sd_mount());

    TEST_ASSERT_EQUAL_UINT(7U, fake_sdmmc_trace_count());
    trace_is(0U, "supply on");
    trace_is_n(1U, "mount", 4U);
    trace_is_n(2U, "read", last);
    trace_is(3U, "unmount");   /* no write was tried after the read failed */
    trace_is(4U, "supply off");
    trace_is(5U, "supply on");
    trace_is_n(6U, "mount", 1U);

    TEST_ASSERT_EQUAL_INT32(1, field(KDRV_SD_BUS_WIDTH));
    TEST_ASSERT_EQUAL_UINT(1U, lines_saying('N', "cannot be read on four lines"));
    TEST_ASSERT_EQUAL_UINT(0U, fake_sdmmc_raw_writes());
    unwatch_supply();
}

/* The card may need its supply cycled, or a moment, before it answers again. */
TEST_CASE("a one-line mount that fails is tried once more after another cycle",
          "[kdrv_sd]")
{
    uint32_t const last = default_last_sector();
    uint32_t       t0;

    fake_sdmmc_fail_wide_writes(true);
    fake_sdmmc_fail_mounts(1U, 1U);
    watch_supply();
    ready_one_mount();
    t0 = khal_time_ms();

    TEST_ASSERT_NOT_NULL(kdrv_sd_mount());

    TEST_ASSERT_EQUAL_UINT(11U, fake_sdmmc_trace_count());
    trace_is(0U, "supply on");
    trace_is_n(1U, "mount", 4U);
    trace_is_n(2U, "read", last);
    trace_is_n(3U, "write", last);
    trace_is(4U, "unmount");
    trace_is(5U, "supply off");
    trace_is(6U, "supply on");
    trace_is_n(7U, "mount", 1U);   /* does not answer */
    trace_is(8U, "supply off");
    trace_is(9U, "supply on");
    trace_is_n(10U, "mount", 1U);
    TEST_ASSERT_EQUAL_UINT32(2U * (KDRV_SD_POWER_OFF_MS + KDRV_SD_POWER_SETTLE_MS),
                             khal_time_ms() - t0);

    TEST_ASSERT_EQUAL_INT32(1, field(KDRV_SD_BUS_WIDTH));
    TEST_ASSERT_EQUAL_UINT(1U, lines_saying('N', "takes no write on four lines"));
    TEST_ASSERT_EQUAL_UINT(0U, loud_lines());
    unwatch_supply();
}

/* A card that answered on four lines and on neither try on one is not an empty
 * slot, whatever the last try said: the report is the one for a card that would
 * not come up. */
TEST_CASE("a card that comes up on neither is reported as one that would not "
          "come up", "[kdrv_sd]")
{
    int32_t value = 0;

    fake_sdmmc_fail_wide_writes(true);
    fake_sdmmc_fail_mounts(1U, 2U);
    watch_supply();
    ready_one_mount();

    TEST_ASSERT_NULL(kdrv_sd_mount());

    TEST_ASSERT_EQUAL_UINT(11U, fake_sdmmc_trace_count());
    trace_is_n(10U, "mount", 1U);
    TEST_ASSERT_FALSE(kdrv_sd_mounted());
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)kdrv_sd_get(KDRV_SD_BUS_WIDTH, &value));
    TEST_ASSERT_EQUAL_UINT(1U, lines_saying('E', "would not come up"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, told_no_card(), "the card was there");
    TEST_ASSERT_EQUAL_UINT(0U, lines_saying('N', "takes no write"));
    TEST_ASSERT_EQUAL_UINT(fake_psram_alloc_count(), fake_psram_free_count());
    unwatch_supply();
}

TEST_CASE("a mount asked on one line does no write check", "[kdrv_sd]")
{
    kdrv_sd_unmount();
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)kdrv_sd_set(KDRV_SD_WIDTH_REQ, 1));
    ready_one_mount();

    TEST_ASSERT_NOT_NULL(kdrv_sd_mount());

    TEST_ASSERT_EQUAL_UINT(1U, fake_sdmmc_trace_count());
    trace_is_n(0U, "mount", 1U);
    TEST_ASSERT_EQUAL_UINT(0U, fake_sdmmc_raw_writes());
    TEST_ASSERT_EQUAL_UINT(0U, fake_psram_alloc_count());
}

TEST_CASE("an empty slot does no write check", "[kdrv_sd]")
{
    ready_one_mount();
    fake_sdmmc_set_present(false);

    TEST_ASSERT_NULL(kdrv_sd_mount());

    TEST_ASSERT_EQUAL_UINT(1U, fake_sdmmc_trace_count());
    trace_is_n(0U, "mount", 4U);
    TEST_ASSERT_EQUAL_UINT(0U, fake_psram_alloc_count());
}

TEST_CASE("a mount khal refuses does no write check", "[kdrv_sd]")
{
    ready_one_mount();
    fake_sdmmc_set_deny(true);

    TEST_ASSERT_NULL(kdrv_sd_mount());

    TEST_ASSERT_EQUAL_UINT(1U, fake_sdmmc_trace_count());
    trace_is_n(0U, "mount", 4U);
    TEST_ASSERT_EQUAL_UINT(0U, fake_psram_alloc_count());
}

TEST_CASE("a card that came up on one line after the four-line try failed is "
          "not checked either", "[kdrv_sd]")
{
    ready_one_mount();
    fake_sdmmc_set_max_width(1U);

    TEST_ASSERT_NOT_NULL(kdrv_sd_mount());

    TEST_ASSERT_EQUAL_UINT(2U, fake_sdmmc_trace_count());
    trace_is_n(0U, "mount", 4U);
    trace_is_n(1U, "mount", 1U);
    TEST_ASSERT_EQUAL_UINT(0U, fake_sdmmc_raw_writes());
}

/* A check that cannot run proves nothing, and a card that is not proven is
 * mounted where every card works. */
TEST_CASE("with no memory for the check the card is mounted on one line",
          "[kdrv_sd]")
{
    ready_one_mount();
    fake_psram_refuse(1U);

    TEST_ASSERT_NOT_NULL(kdrv_sd_mount());

    TEST_ASSERT_EQUAL_UINT(3U, fake_sdmmc_trace_count());
    trace_is_n(0U, "mount", 4U);
    trace_is(1U, "unmount");
    trace_is_n(2U, "mount", 1U);
    TEST_ASSERT_EQUAL_INT32(1, field(KDRV_SD_BUS_WIDTH));
    TEST_ASSERT_EQUAL_UINT(1U, lines_saying('N', "no memory"));
}

/* ---- The two knobs --------------------------------------------------------- */

TEST_CASE("width_req takes one line or four and nothing between", "[kdrv_sd]")
{
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)kdrv_sd_set(KDRV_SD_WIDTH_REQ, 1));
    TEST_ASSERT_EQUAL_INT32(1, field(KDRV_SD_WIDTH_REQ));
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)kdrv_sd_set(KDRV_SD_WIDTH_REQ, 4));

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)kdrv_sd_set(KDRV_SD_WIDTH_REQ, 0));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)kdrv_sd_set(KDRV_SD_WIDTH_REQ, 2));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)kdrv_sd_set(KDRV_SD_WIDTH_REQ, 3));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)kdrv_sd_set(KDRV_SD_WIDTH_REQ, 8));
    TEST_ASSERT_EQUAL_INT32(4, field(KDRV_SD_WIDTH_REQ));
}

TEST_CASE("freq_req_khz is bounded at both ends", "[kdrv_sd]")
{
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)kdrv_sd_set(KDRV_SD_FREQ_REQ_KHZ, 400));
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)kdrv_sd_set(KDRV_SD_FREQ_REQ_KHZ, 50000));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)kdrv_sd_set(KDRV_SD_FREQ_REQ_KHZ, 399));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)kdrv_sd_set(KDRV_SD_FREQ_REQ_KHZ, 50001));
    TEST_ASSERT_EQUAL_INT32(50000, field(KDRV_SD_FREQ_REQ_KHZ));
}

/* The knobs are only worth having if the next mount reads them. */
TEST_CASE("the next mount asks for what the knobs say", "[kdrv_sd]")
{
    unsigned const before = fake_sdmmc_mounts();

    kdrv_sd_unmount();
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)kdrv_sd_set(KDRV_SD_WIDTH_REQ, 1));
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)kdrv_sd_set(KDRV_SD_FREQ_REQ_KHZ, 20000));

    TEST_ASSERT_NOT_NULL(kdrv_sd_mount());
    TEST_ASSERT_EQUAL_UINT8(1U, fake_sdmmc_last_width());
    TEST_ASSERT_EQUAL_UINT32(20000U, fake_sdmmc_last_khz());
    /* One line was asked for, so there is nothing to fall back to. */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_sdmmc_mounts() - before,
                                   "asking for one line tries once");
}

TEST_CASE("nothing the card said can be written", "[kdrv_sd]")
{
    install_card();

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)kdrv_sd_set(KDRV_SD_MFG_ID, 1));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)kdrv_sd_set(KDRV_SD_CAPACITY_MB, 1));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)kdrv_sd_set(KDRV_SD_BUS_WIDTH, 1));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)kdrv_sd_set(KDRV_SD_MOUNTED, 0));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)kdrv_sd_set(KDRV_SD_TOTAL_MB, 1));
}

/* ---- The table itself ------------------------------------------------------ */

/* Two fields with the same name, or one with none, would both show up here
 * before they showed up as a console verb answering the wrong question. */
TEST_CASE("every field has a name, a unit and a range, and resolves to itself",
          "[kdrv_sd]")
{
    for ( uint16_t i = 0U; (uint16_t)KDRV_SD_FIELD_MAX > i; ++i )
    {
        const kdrv_sd_field_info_t * const info =
            kdrv_sd_field_info((kdrv_sd_field_t)i);
        kdrv_sd_field_t back = KDRV_SD_FIELD_MAX;

        TEST_ASSERT_NOT_NULL(info);
        TEST_ASSERT_NOT_NULL(info->name);
        TEST_ASSERT_NOT_NULL(info->unit);
        TEST_ASSERT_TRUE_MESSAGE('\0' != info->name[0], "a field with no name");
        TEST_ASSERT_TRUE_MESSAGE(info->max >= info->min, "an empty range");
        TEST_ASSERT_EQUAL_UINT16(1U, info->step);

        TEST_ASSERT_EQUAL_INT(KODE_OK, (int)kdrv_sd_field_by_name(info->name, &back));
        TEST_ASSERT_EQUAL_INT_MESSAGE((int)i, (int)back,
                                      "two fields answer to one name");
    }
}

TEST_CASE("exactly two fields are writable, and they are the knobs", "[kdrv_sd]")
{
    unsigned writable = 0U;

    for ( uint16_t i = 0U; (uint16_t)KDRV_SD_FIELD_MAX > i; ++i )
    {
        if ( kdrv_sd_field_info((kdrv_sd_field_t)i)->writable )
        {
            ++writable;
        }
    }
    TEST_ASSERT_EQUAL_UINT(2U, writable);
    TEST_ASSERT_TRUE(kdrv_sd_field_info(KDRV_SD_WIDTH_REQ)->writable);
    TEST_ASSERT_TRUE(kdrv_sd_field_info(KDRV_SD_FREQ_REQ_KHZ)->writable);
}

TEST_CASE("a field nobody named is refused, and so is one past the table",
          "[kdrv_sd]")
{
    kdrv_sd_field_t f     = KDRV_SD_FIELD_MAX;
    int32_t         value = 0;

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)kdrv_sd_field_by_name("ichg_ma", &f));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)kdrv_sd_field_by_name("", &f));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)kdrv_sd_field_by_name(NULL, &f));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        (int)kdrv_sd_get(KDRV_SD_FIELD_MAX, &value));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)kdrv_sd_get(KDRV_SD_MFG_ID, NULL));
    TEST_ASSERT_NULL(kdrv_sd_field_info(KDRV_SD_FIELD_MAX));
}

TEST_CASE("the dump names every field once", "[kdrv_sd]")
{
    install_card();
    fake_klog_reset();
    kdrv_sd_dump();

    for ( uint16_t i = 0U; (uint16_t)KDRV_SD_FIELD_MAX > i; ++i )
    {
        const char * const name =
            kdrv_sd_field_info((kdrv_sd_field_t)i)->name;
        unsigned seen = 0U;

        for ( unsigned l = 0U; fake_klog_count() > l; ++l )
        {
            const char *line = fake_klog_line(l);
            const char *at   = (NULL != line) ? strstr(line, name) : NULL;

            while ( NULL != at )
            {
                /* The name has to be the whole key, or `sectors` would count
                 * itself inside `sector_size`. */
                if ( '=' == at[strlen(name)] )
                {
                    ++seen;
                }
                at = strstr(at + 1, name);
            }
        }
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, seen, name);
    }
}

/* ---- The format ------------------------------------------------------------ */

TEST_CASE("a format empties the card and leaves it mounted", "[kdrv_sd]")
{
    FILE *f;
    char  path[512];

    install_card();
    (void)snprintf(path, sizeof(path), "%s/keep.bin", fake_media_sd_dir());
    f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    (void)fputc('x', f);
    (void)fclose(f);
    TEST_ASSERT_TRUE(fake_media_file_exists(fake_media_sd_dir(), "keep.bin"));

    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)kdrv_sd_format());

    TEST_ASSERT_FALSE(fake_media_file_exists(fake_media_sd_dir(), "keep.bin"));
    TEST_ASSERT_TRUE(kdrv_sd_mounted());
}

/* A format that fails cannot leave a volume behind to talk to, so the driver
 * drops the card with it: the alternative is a snapshot describing something
 * nobody can reach. */
TEST_CASE("a format that fails leaves no card and no identity", "[kdrv_sd]")
{
    int32_t value = 0;

    install_card();
    fake_sdmmc_fail_format(true);

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, (int)kdrv_sd_format());
    TEST_ASSERT_FALSE(kdrv_sd_mounted());
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)kdrv_sd_get(KDRV_SD_MFG_ID, &value));
}

/* ---- Raw sectors ----------------------------------------------------------- */

TEST_CASE("a raw sector reads back what was written to it", "[kdrv_sd]")
{
    uint8_t out[512];
    uint8_t back[512];

    install_card();
    for ( unsigned i = 0U; sizeof(out) > i; ++i )
    {
        out[i] = (uint8_t)(i ^ 0x5AU);
    }
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)kdrv_sd_write_sectors(1000U, 1U, out));

    (void)memset(back, 0, sizeof(back));
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)kdrv_sd_read_sectors(1000U, 1U, back));
    TEST_ASSERT_EQUAL_MEMORY(out, back, sizeof(out));
}

TEST_CASE("a sector nobody wrote reads back as zeros", "[kdrv_sd]")
{
    uint8_t back[512];
    uint8_t zero[512];

    install_card();
    (void)memset(zero, 0, sizeof(zero));
    (void)memset(back, 0xFF, sizeof(back));

    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)kdrv_sd_read_sectors(2000U, 1U, back));
    TEST_ASSERT_EQUAL_MEMORY(zero, back, sizeof(zero));
}

TEST_CASE("a sector past the end of the card is refused", "[kdrv_sd]")
{
    uint8_t buf[512];

    install_card();
    (void)memset(buf, 0, sizeof(buf));

    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        (int)kdrv_sd_write_sectors(250347520U, 1U, buf));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        (int)kdrv_sd_read_sectors(250347520U, 1U, buf));
}

/* The whole point of the probe phase one layer up: a card that lies about its
 * size does not refuse the write, it wraps it. */
TEST_CASE("a card that lies about its size wraps a write onto itself",
          "[kdrv_sd]")
{
    uint8_t out[512];
    uint8_t back[512];

    install_card();
    /* Really eight gigabytes, claiming a hundred and twenty. */
    fake_sdmmc_set_alias(16777216U);

    for ( unsigned i = 0U; sizeof(out) > i; ++i )
    {
        out[i] = (uint8_t)(i + 1U);
    }
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        (int)kdrv_sd_write_sectors(16777216U + 42U, 1U, out));

    (void)memset(back, 0, sizeof(back));
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)kdrv_sd_read_sectors(42U, 1U, back));
    TEST_ASSERT_EQUAL_MEMORY(out, back, sizeof(out));
}
