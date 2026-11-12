/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_sd.c
 * @brief The microSD card: what it says it is, and the state of the volume.
 */
#include "kdrv_sd.h"

#include <stdio.h>
#include <string.h>

#include "kbsp.h"
#include "khal_gpio.h"
#include "khal_system.h"
#include "khal_time.h"
#include "khal_sdmmc.h"
#include "kode_log.h"

#define TAG "kdrv.sd"

/* What the card answered with at its last bring-up. Valid while it is
 * mounted; the table's card fields read it only then and are refused
 * otherwise, so nothing above can be handed a stale identity for a card
 * that is gone. */
static khal_sdmmc_card_t s_snap;

/** @brief Whether SD_PWR has been driven and waited out since boot. */
static bool s_powered;

/* What the next mount asks for. Not the card's, ours. */

static uint8_t  s_width_req = (uint8_t)KDRV_SD_WIDTH_USE;
static uint32_t s_freq_req  = (uint32_t)KBSP_SD_FREQ_KHZ;

/* ESP-IDF tells an empty slot in five lines a mount: the card's protocol, the
 * FAT mount and the pins the slot takes again, as errors. Every probe holds
 * these tags quiet with klog's own hold, and card_up() tells the outcome in
 * one line of its own. */

#define SD_PROBE_TAG_COUNT  3U   /**< Tags in tags. */
#define SD_OTHER_HOLDS      1U   /**< Holds taken elsewhere: kdrv_net's HTTP client. */

_Static_assert((SD_PROBE_TAG_COUNT + SD_OTHER_HOLDS) <= KODE_LOG_HOLD_MAX,
               "klog has a slot for each probe tag beside the other holds");

/* A mount that comes up on four lines is proved with one sector: read, then
 * written back as it was. */

#define SD_WIDTH_NARROW   1U    /**< One data line, the width every card takes. */
#define SD_CHECK_SECTORS  1U    /**< Sectors the write check reads and writes back. */
/** @brief Alignment of the check's buffer: a cache line, which a transfer to
 *         or from external memory needs. */
#define SD_CHECK_ALIGN    64U

/* ---- The table ----------------------------------------------------------- */

/* Where a field's value comes from, which is also what decides its refusal. */
#define SRC_CARD 0U   /* the snapshot: refused while nothing is mounted  */
#define SRC_FAT  1U   /* the volume, asked afresh: refused the same way  */
#define SRC_SELF 2U   /* this driver's own state: always has an answer   */

/** @brief One field: what the outside sees, then where it comes from. */
typedef struct {
    kdrv_sd_field_info_t info;
    uint8_t              src;
} field_t;

/* Ranges are spelled out per row rather than derived from a width, because
 * nothing here is a bitfield of a register: these are decoded values, and
 * what bounds them is what the SD specification lets them be. Every step is
 * 1, so field_info() can hand back a pointer into the table. */
#define ROW(nm, un, mn, mx, s, rw) \
    { { (nm), (un), (mn), (mx), 1U, (rw) }, (s) }
#define CARD(nm, un, mn, mx)  ROW(nm, un, mn, mx, SRC_CARD, false)
#define CBOOL(nm)             ROW(nm, "bool", 0, 1, SRC_CARD, false)
#define FAT(nm)               ROW(nm, "MB", 0, SD_MB_MAX, SRC_FAT, false)
#define SELF(nm, un, mn, mx)  ROW(nm, un, mn, mx, SRC_SELF, false)
#define KNOB(nm, un, mn, mx)  ROW(nm, un, mn, mx, SRC_SELF, true)

/* A megabyte count that fits an int32: two terabytes, past anything an SDXC
 * card can be. */
#define SD_MB_MAX   2097151
#define SD_KHZ_MAX  1000000

static const field_t FIELDS[KDRV_SD_FIELD_MAX] = {
    [KDRV_SD_MFG_ID]          = CARD("mfg_id",          "code",  0, 255),
    [KDRV_SD_OEM_ID]          = CARD("oem_id",          "code",  0, 65535),
    [KDRV_SD_REVISION]        = CARD("revision",        "code",  0, 255),
    [KDRV_SD_SERIAL_HI]       = CARD("serial_hi",       "code",  0, 65535),
    [KDRV_SD_SERIAL_LO]       = CARD("serial_lo",       "code",  0, 65535),
    [KDRV_SD_MFG_YEAR]        = CARD("mfg_year",        "year",  2000, 2255),
    [KDRV_SD_MFG_MONTH]       = CARD("mfg_month",       "month", 0, 15),

    [KDRV_SD_CSD_VER]         = CARD("csd_ver",         "code",  0, 3),
    [KDRV_SD_CAPACITY_MB]     = CARD("capacity_mb",     "MB",    0, SD_MB_MAX),
    [KDRV_SD_SECTORS]         = CARD("sectors",         "512B",  0, INT32_MAX),
    [KDRV_SD_SECTOR_SIZE]     = CARD("sector_size",     "B",     0, 4096),
    [KDRV_SD_READ_BLOCK_LEN]  = CARD("read_block_len",  "B",     1, 32768),
    [KDRV_SD_TR_SPEED_KHZ]    = CARD("tr_speed_khz",    "kHz",   0, SD_KHZ_MAX),
    [KDRV_SD_CCC]             = CARD("ccc",             "mask",  0, 4095),

    [KDRV_SD_SD_SPEC]         = CARD("sd_spec",         "code",  0, 15),
    [KDRV_SD_BUS_WIDTHS]      = CARD("bus_widths",      "mask",  0, 15),
    [KDRV_SD_ERASE_STATE]     = CBOOL("erase_state"),

    [KDRV_SD_AU_KB]           = CARD("au_kb",           "KB",    0, 65535),
    [KDRV_SD_ERASE_SIZE_AU]   = CARD("erase_size_au",   "AU",    0, 65535),
    [KDRV_SD_ERASE_TIMEOUT_S] = CARD("erase_timeout_s", "s",     0, 255),
    [KDRV_SD_ERASE_OFFSET_S]  = CARD("erase_offset_s",  "s",     0, 255),
    [KDRV_SD_DISCARD]         = CBOOL("discard"),
    [KDRV_SD_FULE]            = CBOOL("fule"),

    [KDRV_SD_HIGH_CAPACITY]   = CBOOL("high_capacity"),
    [KDRV_SD_BUS_WIDTH]       = CARD("bus_width",       "bits",  0, 8),
    [KDRV_SD_FREQ_KHZ]        = CARD("freq_khz",        "kHz",   0, SD_KHZ_MAX),
    [KDRV_SD_CARD_MAX_KHZ]    = CARD("card_max_khz",    "kHz",   0, SD_KHZ_MAX),
    [KDRV_SD_RCA]             = CARD("rca",             "code",  0, 65535),
    [KDRV_SD_MMC]             = CBOOL("mmc"),
    [KDRV_SD_UHS1]            = CBOOL("uhs1"),

    [KDRV_SD_MOUNTED]         = SELF("mounted",  "bool", 0, 1),
    [KDRV_SD_HOSTED]          = SELF("hosted",   "bool", 0, 1),

    [KDRV_SD_TOTAL_MB]        = FAT("total_mb"),
    [KDRV_SD_FREE_MB]         = FAT("free_mb"),

    [KDRV_SD_WIDTH_REQ]       = KNOB("width_req",    "bits", 1, 4),
    [KDRV_SD_FREQ_REQ_KHZ]    = KNOB("freq_req_khz", "kHz",  400, 50000),
};

/** @brief Megabytes in @p sectors of @p sector_size bytes. */
static int32_t to_mb(uint32_t sectors, uint16_t sector_size)
{
    uint64_t const bytes = (uint64_t)sectors * (uint64_t)sector_size;

    return (int32_t)(bytes / (1024ULL * 1024ULL));
}

/**
 * @brief Read one of the two fields the filesystem owns.
 * @param[in]  f     KDRV_SD_TOTAL_MB or KDRV_SD_FREE_MB.
 * @param[out] value Megabytes.
 * @return KODE_OK, or KODE_ERROR when the volume will not answer.
 */
static kode_status_t fat_field(kdrv_sd_field_t f, int32_t *value)
{
    uint64_t total = 0U;
    uint64_t freeb = 0U;

    if ( khal_sdmmc_info(&total, &freeb) != KODE_OK )
    {
        return KODE_ERROR;
    }
    uint64_t const bytes = (KDRV_SD_TOTAL_MB == f) ? total : freeb;
    *value = (int32_t)(bytes / (1024ULL * 1024ULL));
    return KODE_OK;
}

/**
 * @brief Read one field of the card's snapshot or of this driver's state.
 * @param[in]  f     Which field; already known to be in the table.
 * @param[out] value Its value.
 */
static void snap_field(kdrv_sd_field_t f, int32_t *value)
{
    switch ( f )
    {
        case KDRV_SD_MFG_ID:          *value = (int32_t)s_snap.mfg_id; break;
        case KDRV_SD_OEM_ID:          *value = (int32_t)s_snap.oem_id; break;
        case KDRV_SD_REVISION:        *value = (int32_t)s_snap.revision; break;
        case KDRV_SD_SERIAL_HI:
            *value = (int32_t)((s_snap.serial >> 16) & 0xFFFFU);
            break;
        case KDRV_SD_SERIAL_LO:
            *value = (int32_t)(s_snap.serial & 0xFFFFU);
            break;
        /* MDT packs the year as an offset from 2000 in bits 11:4 and the
         * month, one-based, in bits 3:0. */
        case KDRV_SD_MFG_YEAR:
            *value = 2000 + (int32_t)((s_snap.date >> 4) & 0xFFU);
            break;
        case KDRV_SD_MFG_MONTH:
            *value = (int32_t)(s_snap.date & 0x0FU);
            break;

        case KDRV_SD_CSD_VER:         *value = (int32_t)s_snap.csd_ver; break;
        case KDRV_SD_CAPACITY_MB:
            *value = to_mb(s_snap.sectors, s_snap.sector_size);
            break;
        case KDRV_SD_SECTORS:         *value = (int32_t)s_snap.sectors; break;
        case KDRV_SD_SECTOR_SIZE:     *value = (int32_t)s_snap.sector_size; break;
        /* The CSD carries READ_BL_LEN as an exponent, and so does the seam.
         * Bytes are what the unit says and what somebody reading a record
         * expects, so the shift happens here rather than in the log. */
        case KDRV_SD_READ_BLOCK_LEN:
            *value = (int32_t)(1UL << (s_snap.read_block_len & 0x0FU));
            break;
        case KDRV_SD_TR_SPEED_KHZ:    *value = (int32_t)s_snap.tr_speed_khz; break;
        case KDRV_SD_CCC:             *value = (int32_t)s_snap.ccc; break;

        case KDRV_SD_SD_SPEC:         *value = (int32_t)s_snap.sd_spec; break;
        case KDRV_SD_BUS_WIDTHS:      *value = (int32_t)s_snap.bus_widths; break;
        case KDRV_SD_ERASE_STATE:     *value = s_snap.erase_state ? 1 : 0; break;

        case KDRV_SD_AU_KB:           *value = (int32_t)s_snap.au_kb; break;
        case KDRV_SD_ERASE_SIZE_AU:   *value = (int32_t)s_snap.erase_size_au; break;
        case KDRV_SD_ERASE_TIMEOUT_S: *value = (int32_t)s_snap.erase_timeout_s; break;
        case KDRV_SD_ERASE_OFFSET_S:  *value = (int32_t)s_snap.erase_offset_s; break;
        case KDRV_SD_DISCARD:         *value = s_snap.discard ? 1 : 0; break;
        case KDRV_SD_FULE:            *value = s_snap.fule ? 1 : 0; break;

        case KDRV_SD_HIGH_CAPACITY:   *value = s_snap.high_capacity ? 1 : 0; break;
        case KDRV_SD_BUS_WIDTH:       *value = (int32_t)s_snap.bus_width; break;
        case KDRV_SD_FREQ_KHZ:        *value = (int32_t)s_snap.freq_khz; break;
        case KDRV_SD_CARD_MAX_KHZ:    *value = (int32_t)s_snap.max_freq_khz; break;
        case KDRV_SD_RCA:             *value = (int32_t)s_snap.rca; break;
        case KDRV_SD_MMC:             *value = s_snap.is_mmc ? 1 : 0; break;
        case KDRV_SD_UHS1:            *value = s_snap.is_uhs1 ? 1 : 0; break;

        case KDRV_SD_MOUNTED:  *value = khal_sdmmc_mounted() ? 1 : 0; break;
        case KDRV_SD_HOSTED:   *value = khal_sdmmc_hosted() ? 1 : 0; break;
        case KDRV_SD_WIDTH_REQ:    *value = (int32_t)s_width_req; break;
        case KDRV_SD_FREQ_REQ_KHZ: *value = (int32_t)s_freq_req; break;

        default:
            *value = 0;
            break;
    }
}

kode_status_t kdrv_sd_get(kdrv_sd_field_t f, int32_t *value)
{
    if ( (NULL == value) || (KDRV_SD_FIELD_MAX <= f) )
    {
        return KODE_DENIED;
    }
    const field_t * const row = &FIELDS[f];

    if ( (SRC_SELF != row->src) && !khal_sdmmc_mounted() )
    {
        return KODE_DENIED;
    }
    if ( SRC_FAT == row->src )
    {
        return fat_field(f, value);
    }
    snap_field(f, value);
    return KODE_OK;
}

kode_status_t kdrv_sd_set(kdrv_sd_field_t f, int32_t value)
{
    if ( KDRV_SD_FIELD_MAX <= f )
    {
        return KODE_DENIED;
    }
    const field_t * const row = &FIELDS[f];

    if ( !row->info.writable )
    {
        return KODE_DENIED;
    }
    if ( (value < row->info.min) || (value > row->info.max) )
    {
        return KODE_DENIED;
    }
    if ( KDRV_SD_WIDTH_REQ == f )
    {
        /* One line or four. A slot has no other width, so 2 and 3 are inside
         * the range and still impossible. */
        if ( (1 != value) && (4 != value) )
        {
            return KODE_DENIED;
        }
        s_width_req = (uint8_t)value;
    }
    else
    {
        s_freq_req = (uint32_t)value;
    }
    return KODE_OK;
}

const kdrv_sd_field_info_t *kdrv_sd_field_info(kdrv_sd_field_t f)
{
    if ( KDRV_SD_FIELD_MAX <= f )
    {
        return NULL;
    }
    return &FIELDS[f].info;
}

kode_status_t kdrv_sd_field_by_name(const char *name, kdrv_sd_field_t *f)
{
    if ( (NULL == name) || (NULL == f) )
    {
        return KODE_DENIED;
    }
    for ( uint16_t i = 0U; (uint16_t)KDRV_SD_FIELD_MAX > i; ++i )
    {
        if ( strcmp(FIELDS[i].info.name, name) == 0 )
        {
            *f = (kdrv_sd_field_t)i;
            return KODE_OK;
        }
    }
    return KODE_DENIED;
}

/* ---- What the table cannot say ------------------------------------------- */

kode_status_t kdrv_sd_name(char *out, size_t len)
{
    if ( (NULL == out) || (0U == len) || !khal_sdmmc_mounted() )
    {
        return KODE_DENIED;
    }
    (void)snprintf(out, len, "%s", s_snap.name);
    return KODE_OK;
}

kode_status_t kdrv_sd_format(void)
{
    kode_status_t const st = khal_sdmmc_format();

    if ( KODE_OK != st )
    {
        /* The seam leaves nothing mounted on any failure, so the identity
         * goes with it: answering questions about a card nobody can reach is
         * exactly what the snapshot must not do. */
        (void)memset(&s_snap, 0, sizeof(s_snap));
    }
    return st;
}

kode_status_t kdrv_sd_read_sectors(uint32_t first, uint32_t count, void *dst)
{
    return khal_sdmmc_read_sectors(first, count, dst);
}

kode_status_t kdrv_sd_write_sectors(uint32_t first, uint32_t count,
                                    const void *src)
{
    return khal_sdmmc_write_sectors(first, count, src);
}

/* ---- Mounting ------------------------------------------------------------ */

const char *kdrv_sd_mount_point(void)
{
    return khal_sdmmc_mount_point();
}

bool kdrv_sd_mounted(void)
{
    return khal_sdmmc_mounted();
}

void kdrv_sd_set_hosted(bool hosted)
{
    khal_sdmmc_set_hosted(hosted);
}

/**
 * @brief Drive SD_PWR low, the card's supply, and wait out the rail once per boot.
 *
 * Driven at the first bring-up of a boot, and again whenever it is not low,
 * which removes the question of who left it how. A software reset leaves the
 * pin as it was: already low, the rail has been up since the last firmware
 * and there is nothing to wait for.
 */
static void card_power(void)
{
    bool const already = khal_gpio_driven_low((uint8_t)KBSP_PIN_SD_PWR);

    if ( s_powered && already )
    {
        return;   /* driven low this boot and still low: nothing to write */
    }
    if ( khal_gpio_output((uint8_t)KBSP_PIN_SD_PWR, false) != KODE_OK )
    {
        KLOG_E(TAG, "SD_PWR (GPIO%u) could not be driven; the card has no "
                    "supply", (unsigned)KBSP_PIN_SD_PWR);
        return;
    }
    if ( !s_powered )
    {
        /* The rail behind Q1 has to rise before a card can answer CMD0, and
         * a bring-up already blocks for far longer than this. */
        if ( !already )
        {
            khal_delay_ms(KDRV_SD_POWER_SETTLE_MS);
        }
        s_powered = true;
    }
}

/**
 * @brief Cut the card's supply for its off time and switch it on again.
 *
 * What brings a card back to answering after a failed transfer: the host
 * cannot be reset for it, because the controller is shared with the C5's link.
 */
static void card_cycle(void)
{
    if ( kdrv_sd_park() != KODE_OK )
    {
        KLOG_W(TAG, "SD_PWR (GPIO%u) could not be driven; the card's supply "
                    "is not cycled", (unsigned)KBSP_PIN_SD_PWR);
    }
    khal_delay_ms(KDRV_SD_POWER_OFF_MS);
    card_power();
}

/**
 * @brief Hold ESP-IDF's account of an empty slot quiet, or let it speak again.
 *
 * A tag that cannot be held (klog's table is full) is left to speak.
 *
 * @param quiet true to hold the probe tags, false to release them.
 */
static void probe_quiet(bool quiet)
{
    /* ESP-IDF's tags that tell an empty slot. */
    static const char *const tags[SD_PROBE_TAG_COUNT] = {
        "sdmmc_common", "vfs_fat_sdmmc", "gpio"
    };

    for ( size_t i = 0U; SD_PROBE_TAG_COUNT > i; ++i )
    {
        if ( quiet )
        {
            (void)kode_log_hold(tags[i], KODE_LOG_OFF);
        }
        else
        {
            kode_log_release(tags[i]);
        }
    }
}

/**
 * @brief Prove the card takes a write on the lines it came up on, leaving what
 *        it holds as it was.
 *
 * Reads the card's last sector and writes the same bytes back. No partition
 * lives that far out, so a write cut short by a power loss costs nothing.
 *
 * @return NULL when the read and the write-back both passed; otherwise why the
 *         card is not trusted on these lines, as a phrase for the log.
 */
static const char *card_check_write(void)
{
    const char *why = NULL;
    void       *buf = NULL;

    if ( (0U != s_snap.sectors) && (0U != s_snap.sector_size) )
    {
        buf = khal_alloc_psram(SD_CHECK_ALIGN,
                               (size_t)s_snap.sector_size * SD_CHECK_SECTORS);
    }
    if ( NULL == buf )
    {
        return "there is no sector or no memory to check a write on four lines";
    }

    uint32_t const last = s_snap.sectors - SD_CHECK_SECTORS;

    if ( kdrv_sd_read_sectors(last, SD_CHECK_SECTORS, buf) != KODE_OK )
    {
        why = "the card's last sector cannot be read on four lines";
    }
    else if ( kdrv_sd_write_sectors(last, SD_CHECK_SECTORS, buf) != KODE_OK )
    {
        why = "the card takes no write on four lines";
    }
    else
    {
        /* both transfers passed */
    }
    khal_free(buf);
    return why;
}

/**
 * @brief Take a card that was not shown to take a write on four lines down to
 *        one: unmount it, cycle its supply and mount it on one line, again
 *        after another cycle if it does not answer.
 *
 * Nothing is left describing the four-line mount. The host is not touched
 * beyond the unmount and the mount: the C5's link shares its controller.
 *
 * @param why What the check found, as it is told at NOTICE once the card is up.
 * @return KODE_OK with s_snap filled in for the one-line mount; KODE_DENIED
 *         when khal refused a mount without a try; KODE_ERROR with s_snap
 *         cleared when the card came up on none of the tries.
 */
static kode_status_t card_narrow(const char *why)
{
    kode_status_t st   = KODE_ERROR;
    uint8_t       left = (uint8_t)KDRV_SD_ONE_LINE_TRIES;

    khal_sdmmc_unmount();
    (void)memset(&s_snap, 0, sizeof(s_snap));

    while ( (0U < left) && (KODE_ERROR == st) )
    {
        --left;
        card_cycle();
        st = khal_sdmmc_mount(SD_WIDTH_NARROW, s_freq_req, &s_snap);
        if ( (KODE_OK != st) && (KODE_DENIED != st) )
        {
            /* A card that answered on four lines is no empty slot, whatever
             * this try said: the caller reports it as one that would not come
             * up. */
            KLOG_D(TAG, "1-bit mount after the supply's cycle failed (%d)",
                   (int)st);
            st = KODE_ERROR;
        }
    }
    if ( KODE_OK == st )
    {
        KLOG_N(TAG, "%s; it is mounted on one line", why);
    }
    return st;
}

/**
 * @brief Mount on the lines asked for, quietly after a miss: four first and
 *        then one, and a four-line mount proved to take a write.
 *
 * @return What khal_sdmmc_mount() said of the last try, or card_narrow()'s
 *         answer when the card took no write on four lines.
 */
static kode_status_t card_mount(void)
{
    kode_status_t st;

    probe_quiet(true);
    st = khal_sdmmc_mount(s_width_req, s_freq_req, &s_snap);
    if ( (KODE_OK != st) && (KODE_DENIED != st) && (KODE_TIMEOUT != st)
        && (SD_WIDTH_NARROW < s_width_req) )
    {
        /* Four lines first, then one. A card that does not honour the 4-bit
         * mode it advertises in its SCR is still worth having on one line.
         * An empty slot is empty at one line too. */
        KLOG_D(TAG, "4-bit bring-up failed; retrying at 1-bit");
        st = khal_sdmmc_mount(SD_WIDTH_NARROW, s_freq_req, &s_snap);
    }
    if ( (KODE_OK == st) && (SD_WIDTH_NARROW < s_snap.bus_width) )
    {
        /* Up on four lines. The counterfeit cards shipped with some units
         * read there and fail the first write, and a failed write leaves the
         * controller unable to bring a card up until the card's supply is
         * cycled, so the write is tried here, on the card's last sector, and
         * not by the first file. */
        const char * const why = card_check_write();

        if ( NULL != why )
        {
            st = card_narrow(why);
        }
    }
    probe_quiet(false);
    return st;
}

/**
 * @brief Power the card and mount its volume, four data lines first and then
 *        one, a four-line mount proved to take a write.
 *
 * @return KODE_OK with s_snap filled in; KODE_DENIED when no mount was tried
 *         (the link holds the controller and is down) or khal refused it
 *         without a try, which is no answer about the card and is said at
 *         debug level only; KODE_ERROR with s_snap cleared and the reason
 *         logged.
 */
static kode_status_t card_up(void)
{
    /* Whether the last bring-up found no card it could mount and said so:
     * a repeat is not news. */
    static bool s_missing = false;

    card_power();
    if ( !khal_sdmmc_ready() )
    {
        KLOG_D(TAG, "the mount waits: the link's controller is not up");
        return KODE_DENIED;
    }

    kode_status_t const st = card_mount();

    if ( KODE_OK == st )
    {
        s_missing = false;
    }
    else if ( KODE_DENIED == st )
    {
        KLOG_D(TAG, "the mount was refused");
    }
    else if ( s_missing )
    {
        /* Every listing asks the slot again; an empty one was told once. */
        (void)memset(&s_snap, 0, sizeof(s_snap));
        KLOG_D(TAG, "still no card");
    }
    else
    {
        (void)memset(&s_snap, 0, sizeof(s_snap));
        if ( KODE_TIMEOUT == st )
        {
            /* An empty slot is a board without a card, not a fault. */
            KLOG_N(TAG, "no card");
            KLOG_D(TAG, "the card's supply is switched by SD_PWR = GPIO%u, "
                        "driven low: a card that is in and silent points at Q1",
                   (unsigned)KBSP_PIN_SD_PWR);
        }
        else if ( KODE_IO_ERROR == st )
        {
            KLOG_W(TAG, "the card has no FAT volume to read (exFAT, or not "
                        "formatted); it stays unmounted");
        }
        else
        {
            KLOG_E(TAG, "the card would not come up (%d)", (int)st);
        }
        s_missing = true;
    }
    return st;
}

const char *kdrv_sd_mount(void)
{
    if ( khal_sdmmc_mounted() )
    {
        return khal_sdmmc_mount_point();
    }
    if ( card_up() != KODE_OK )
    {
        return NULL;
    }

    KLOG_I(TAG, "card mounted at %s: '%s', %luMB, %u-bit, %lukHz",
           khal_sdmmc_mount_point(), s_snap.name,
           (unsigned long)(((uint64_t)s_snap.sectors
                            * (uint64_t)s_snap.sector_size) / (1024ULL * 1024ULL)),
           (unsigned)s_snap.bus_width,
           (unsigned long)s_snap.freq_khz);
    return khal_sdmmc_mount_point();
}

kode_status_t kdrv_sd_park(void)
{
    /* SD_PWR low turns Q1 on, so high is the card unpowered. */
    s_powered = false;
    return khal_gpio_output((uint8_t)KBSP_PIN_SD_PWR, true);
}

void kdrv_sd_unmount(void)
{
    if ( !khal_sdmmc_mounted() )
    {
        return;
    }
    khal_sdmmc_unmount();
    (void)memset(&s_snap, 0, sizeof(s_snap));
    KLOG_I(TAG, "card unmounted");
}

kode_status_t kdrv_sd_usage(uint64_t *total, uint64_t *free_bytes)
{
    return khal_sdmmc_info(total, free_bytes);
}

/* ---- Dump ---------------------------------------------------------------- */

#define SD_DUMP_LINE 96U

/** @brief Append @p text to @p line, flushing it as a log line when full. */
static void dump_append(char *line, size_t *n, const char *text)
{
    size_t const len = strlen(text);

    if ( (*n + len) >= SD_DUMP_LINE )
    {
        KLOG_N(TAG, "%s", line);
        *n = 0U;
        line[0] = '\0';
    }
    (void)memcpy(&line[*n], text, len + 1U);
    *n += len;
}

void kdrv_sd_dump(void)
{
    char   line[SD_DUMP_LINE + 32U];
    size_t n = 0U;

    if ( !khal_sdmmc_mounted() )
    {
        KLOG_N(TAG, "no card mounted");
        return;
    }
    /* Through klog, NOT sdmmc_card_print_info(stdout, ...): a raw printf
     * writes around AO_Log and lands on the console without the '!' framing,
     * which a host would misread as a broken response line. */
    KLOG_N(TAG, "'%s' at %s", s_snap.name, khal_sdmmc_mount_point());

    line[0] = '\0';
    for ( uint16_t i = 0U; (uint16_t)KDRV_SD_FIELD_MAX > i; ++i )
    {
        char    pair[48];
        int32_t value = 0;

        if ( kdrv_sd_get((kdrv_sd_field_t)i, &value) == KODE_OK )
        {
            (void)snprintf(pair, sizeof(pair), "%s=%ld ",
                           FIELDS[i].info.name, (long)value);
        }
        else
        {
            (void)snprintf(pair, sizeof(pair), "%s=? ", FIELDS[i].info.name);
        }
        dump_append(line, &n, pair);
    }
    if ( 0U < n )
    {
        KLOG_N(TAG, "%s", line);
    }
}
