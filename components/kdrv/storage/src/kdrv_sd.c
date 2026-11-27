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
static bool     s_check_req = true;

/* ESP-IDF tells an empty slot in five lines a mount: the card's protocol, the
 * FAT mount and the pins the slot takes again, as errors. Every probe holds
 * these tags quiet with klog's own hold, and card_up() tells the outcome in
 * one line of its own. */

#define SD_PROBE_TAG_COUNT  3U   /**< Tags in tags. */
#define SD_OTHER_HOLDS      1U   /**< Holds taken elsewhere: kdrv_net's HTTP client. */

_Static_assert((SD_PROBE_TAG_COUNT + SD_OTHER_HOLDS) <= KODE_LOG_HOLD_MAX,
               "klog has a slot for each probe tag beside the other holds");

/* A mount is proved with one sector: read, then written back as it was. One
 * on four lines that fails is taken down to one line, and a mount on one line
 * is proved there. */

#define SD_WIDTH_NARROW   1U    /**< One data line, the width every card takes. */
#define SD_CHECK_SECTORS  1U    /**< Sectors the write check reads and writes back. */
/** @brief Alignment of the check's buffer: a cache line, which a transfer to
 *         or from external memory needs. */
#define SD_CHECK_ALIGN    64U

/* What the driver remembers of the last card it judged, in RAM for the boot:
 * what the card took on four lines, and who it is. A card is known by what the
 * snapshot holds of its CID - manufacturer, OEM, name, revision, serial and
 * date - and a card that differs in any of them is another. */

/** @brief What the last card judged took on four lines. */
typedef enum {
    SD_VERDICT_NONE = 0,   /**< No card judged since the boot or the empty slot. */
    SD_VERDICT_FOUR,       /**< It took a write there: four lines it is.      */
    SD_VERDICT_ONE         /**< It took none: one line it is.                 */
} sd_verdict_t;

/** @brief What a card is known by. */
typedef struct {
    uint8_t  mfg_id;                      /**< CID manufacturer.              */
    uint16_t oem_id;                      /**< CID OEM or application.        */
    char     name[KHAL_SDMMC_NAME_LEN];   /**< CID product name.              */
    uint8_t  revision;                    /**< CID product revision.          */
    uint32_t serial;                      /**< CID serial number.             */
    uint16_t date;                        /**< CID manufacturing date, raw.   */
} card_id_t;

static sd_verdict_t s_verdict = SD_VERDICT_NONE;
static card_id_t    s_judged;   /**< Who s_verdict is about; valid with a verdict. */

/** @brief What the last card proved by a write on the lines it stands on. */
typedef enum {
    SD_PROOF_NONE = 0,   /**< No card asked since the boot or the empty slot. */
    SD_PROOF_TOOK,       /**< It took the write, and no write of a file on it
                          *   has failed since.                               */
    SD_PROOF_REFUSED     /**< It took none, or a write of a file on it failed. */
} sd_proof_t;

static sd_proof_t s_proof = SD_PROOF_NONE;
static card_id_t  s_proved;     /**< Who s_proof is about; valid with a proof. */

/** @brief Whether the mount that stands was asked on everyday terms. */
static bool s_everyday;

/** @brief Why the card is not mounted: what the last bring-up that answered
 *         found. KDRV_SD_WHY_NONE while one is. */
static kdrv_sd_why_t s_why = KDRV_SD_WHY_NONE;

/** @brief Why nothing is mounted, as the log says it, by kdrv_sd_why_t. */
static const char *const WHY_SAID[KDRV_SD_WHY_MAX] = {
    [KDRV_SD_WHY_NONE]    = "not looked for since the boot or the unmount",
    [KDRV_SD_WHY_NO_CARD] = "the slot is empty",
    [KDRV_SD_WHY_NO_FAT]  = "the card has no FAT volume to read",
    [KDRV_SD_WHY_FAILED]  = "the card would not come up",
};

/**
 * @brief Why a bring-up that answered left nothing mounted.
 * @param st What card_mount() said: neither KODE_OK nor KODE_DENIED.
 * @return KDRV_SD_WHY_NO_CARD for no answer, KDRV_SD_WHY_NO_FAT for a card
 *         with no FAT volume, KDRV_SD_WHY_FAILED for any other failure.
 */
static kdrv_sd_why_t why_of(kode_status_t st)
{
    kdrv_sd_why_t why = KDRV_SD_WHY_FAILED;

    if ( KODE_TIMEOUT == st )
    {
        why = KDRV_SD_WHY_NO_CARD;
    }
    else if ( KODE_IO_ERROR == st )
    {
        why = KDRV_SD_WHY_NO_FAT;
    }
    else
    {
        /* a card answered and would not come up */
    }
    return why;
}

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

    [KDRV_SD_TOTAL_MB]        = FAT("total_mb"),
    [KDRV_SD_FREE_MB]         = FAT("free_mb"),

    [KDRV_SD_WIDTH_REQ]       = KNOB("width_req",    "bits", 1, 4),
    [KDRV_SD_FREQ_REQ_KHZ]    = KNOB("freq_req_khz", "kHz",  400, 50000),
    [KDRV_SD_CHECK_REQ]       = KNOB("check_req",    "bool", 0, 1),
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
        case KDRV_SD_WIDTH_REQ:    *value = (int32_t)s_width_req; break;
        case KDRV_SD_FREQ_REQ_KHZ: *value = (int32_t)s_freq_req; break;
        case KDRV_SD_CHECK_REQ:    *value = s_check_req ? 1 : 0; break;

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
    else if ( KDRV_SD_CHECK_REQ == f )
    {
        s_check_req = (0 != value);
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
        s_everyday = false;
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

bool kdrv_sd_mounted_everyday(void)
{
    return khal_sdmmc_mounted() && s_everyday;
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

/** @brief What @p c is known by. */
static card_id_t id_of(const khal_sdmmc_card_t *c)
{
    card_id_t id;

    (void)memset(&id, 0, sizeof(id));
    id.mfg_id   = c->mfg_id;
    id.oem_id   = c->oem_id;
    (void)memcpy(id.name, c->name, sizeof(id.name));
    id.revision = c->revision;
    id.serial   = c->serial;
    id.date     = c->date;
    return id;
}

/** @brief Whether @p c is the card @p who names. */
static bool is_card(const card_id_t *who, const khal_sdmmc_card_t *c)
{
    card_id_t const id = id_of(c);

    return (id.mfg_id == who->mfg_id)
        && (id.oem_id == who->oem_id)
        && (strncmp(id.name, who->name, sizeof(id.name)) == 0)
        && (id.revision == who->revision)
        && (id.serial == who->serial)
        && (id.date == who->date);
}

/** @brief Whether @p c is the card the last verdict is about. */
static bool is_judged(const khal_sdmmc_card_t *c)
{
    return (SD_VERDICT_NONE != s_verdict) && is_card(&s_judged, c);
}

/** @brief Whether @p c is the card the last proof is about. */
static bool is_proved(const khal_sdmmc_card_t *c)
{
    return (SD_PROOF_NONE != s_proof) && is_card(&s_proved, c);
}

/**
 * @brief Remember what the card in @p c proved, for the boot.
 * @param c    The card.
 * @param took Whether it took the write.
 */
static void prove(const khal_sdmmc_card_t *c, bool took)
{
    s_proved = id_of(c);
    s_proof  = SD_PROOF_REFUSED;
    if ( took )
    {
        s_proof = SD_PROOF_TOOK;
    }
}

bool kdrv_sd_took_write(void)
{
    return khal_sdmmc_mounted() && (SD_PROOF_TOOK == s_proof)
        && is_proved(&s_snap);
}

bool kdrv_sd_write_failed(void)
{
    if ( !kdrv_sd_took_write() )
    {
        return false;   /* nothing to take back: said once */
    }
    if ( (SD_WIDTH_NARROW < s_snap.bus_width) && is_judged(&s_snap) )
    {
        /* The counterfeit cards shipped with some units take the check's one
         * sector on four lines, fail the writes of files there and write on
         * one: the next mount is on one line, where the card is checked
         * again. */
        s_verdict = SD_VERDICT_ONE;
        s_proof   = SD_PROOF_NONE;
        KLOG_N(TAG, "a write on the card failed on four lines; its next mount "
                    "is on one, where it is checked again");
    }
    else
    {
        s_proof = SD_PROOF_REFUSED;
        KLOG_N(TAG, "a write on the card failed; until it comes out or the "
                    "boot ends it is not counted as a card that takes a write");
    }
    return true;
}

/**
 * @brief Prove the card takes a write on the lines it came up on, leaving what
 *        it holds as it was.
 *
 * What this proves is the way a write goes: the bus, the card's controller
 * and one sector it accepts. It does not prove the card keeps what it is
 * given, nor that it holds as much as it says: a card of a false size may
 * accept a sector past its real end and drop it or write it over one of its
 * own. The card test's scan for a wrapped size (`sd_verify`) is the check of
 * that.
 *
 * Reads the card's last sector and writes the same bytes back. No partition
 * lives that far out, so a write cut short by a power loss costs nothing.
 *
 * @param[out] asked true when the card was asked and its answer is its
 *                   verdict; false when the check could not be made.
 * @return NULL when the read and the write-back both passed; otherwise why the
 *         card is not trusted on these lines, as a phrase for the log.
 */
static const char *card_check_write(bool *asked)
{
    const char *why = NULL;
    void       *buf = NULL;

    *asked = false;
    if ( (0U != s_snap.sectors) && (0U != s_snap.sector_size) )
    {
        buf = khal_alloc_psram(SD_CHECK_ALIGN,
                               (size_t)s_snap.sector_size * SD_CHECK_SECTORS);
    }
    if ( NULL == buf )
    {
        return "there is no sector or no memory to check a write";
    }

    uint32_t const last = s_snap.sectors - SD_CHECK_SECTORS;

    *asked = true;
    if ( kdrv_sd_read_sectors(last, SD_CHECK_SECTORS, buf) != KODE_OK )
    {
        why = "the card's last sector cannot be read";
    }
    else if ( kdrv_sd_write_sectors(last, SD_CHECK_SECTORS, buf) != KODE_OK )
    {
        why = "the card takes no write";
    }
    else
    {
        /* both transfers passed */
    }
    khal_free(buf);
    return why;
}

/**
 * @brief Take a card that was not shown to take a write down to one line:
 *        unmount it, cycle its supply and mount it on one line, again after
 *        another cycle if it does not answer.
 *
 * Nothing is left describing the mount that failed the check. The host is not
 * touched beyond the unmount and the mount: the C5's link shares its
 * controller.
 *
 * @param why   What the check found, as it is told at NOTICE once the card is
 *              up.
 * @param lines The lines the check was made on, in words: "four lines" or
 *              "one line".
 * @return KODE_OK with s_snap filled in for the one-line mount; KODE_DENIED
 *         when khal refused a mount without a try; KODE_ERROR with s_snap
 *         cleared when the card came up on none of the tries.
 */
static kode_status_t card_narrow(const char *why, const char *lines)
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
        KLOG_N(TAG, "%s on %s; it is mounted on one line", why, lines);
    }
    return st;
}

/**
 * @brief Prove a mount on one line once per card and boot, and take a card
 *        that fails down and up again on one line.
 *
 * A failed write is followed by a cycle of the card's supply, because it may
 * leave the controller unable to read the card until then. A check that could
 * not be made is no proof: the card is asked again at its next mount.
 *
 * @return KODE_OK with s_snap describing the mount that stands; what
 *         card_narrow() answers when the card was taken down.
 */
static kode_status_t card_prove_one(void)
{
    bool asked = false;

    if ( is_proved(&s_snap) )
    {
        return KODE_OK;
    }

    const char * const why = card_check_write(&asked);

    if ( NULL == why )
    {
        prove(&s_snap, true);
        return KODE_OK;
    }
    if ( !asked )
    {
        KLOG_N(TAG, "%s on one line", why);
        return KODE_OK;
    }
    prove(&s_snap, false);
    return card_narrow(why, "one line");
}

/**
 * @brief Settle a mount that came up on four lines: kept when the card is
 *        known to take a write there or proves to, narrowed to one otherwise
 *        and proved there.
 *
 * What a card took is remembered for the boot, so a card is judged once: a
 * card that took the write is not checked again, and one that did not, or
 * that failed a write of a file on four lines later (kdrv_sd_write_failed()),
 * is mounted on one line from then on (card_mount_known()). A check that could
 * not be made is no verdict.
 *
 * @return KODE_OK with s_snap describing the mount that stands; what
 *         card_narrow() answers when the card was narrowed.
 */
static kode_status_t card_settle(void)
{
    card_id_t const id    = id_of(&s_snap);
    bool            asked = false;

    if ( (SD_VERDICT_FOUR == s_verdict) && is_judged(&s_snap) )
    {
        KLOG_D(TAG, "the card took a write on four lines before; it is not "
                    "checked again");
        prove(&s_snap, true);   /* another card's proof may stand since */
        return KODE_OK;
    }

    const char * const why = card_check_write(&asked);

    if ( NULL == why )
    {
        s_judged  = id;
        s_verdict = SD_VERDICT_FOUR;
        prove(&s_snap, true);
        return KODE_OK;
    }

    kode_status_t st = card_narrow(why, "four lines");

    if ( (KODE_OK == st) && asked )
    {
        s_judged  = id;
        s_verdict = SD_VERDICT_ONE;
    }
    if ( KODE_OK == st )
    {
        st = card_prove_one();
    }
    return st;
}

/**
 * @brief Mount on the lines asked for: four first and then one, a four-line
 *        mount settled by the write check when the check is asked for.
 *
 * @return What khal_sdmmc_mount() said of the last try, or card_settle()'s
 *         answer when the card came up on four lines.
 */
static kode_status_t card_mount_asked(void)
{
    kode_status_t st = khal_sdmmc_mount(s_width_req, s_freq_req, &s_snap);

    if ( (KODE_OK != st) && (KODE_DENIED != st) && (KODE_TIMEOUT != st)
        && (SD_WIDTH_NARROW < s_width_req) )
    {
        /* Four lines first, then one. A card that does not honour the 4-bit
         * mode it advertises in its SCR is still worth having on one line.
         * An empty slot is empty at one line too. */
        KLOG_D(TAG, "4-bit bring-up failed; retrying at 1-bit");
        st = khal_sdmmc_mount(SD_WIDTH_NARROW, s_freq_req, &s_snap);
    }
    if ( s_check_req && (KODE_OK == st) && (SD_WIDTH_NARROW < s_snap.bus_width) )
    {
        /* Up on four lines. The counterfeit cards shipped with some units
         * read there and fail the first write, and a failed write leaves the
         * controller unable to bring a card up until the card's supply is
         * cycled, so the write is tried here, on the card's last sector, and
         * not by the first file. */
        st = card_settle();
    }
    else if ( s_check_req && (KODE_OK == st) )
    {
        st = card_prove_one();
    }
    else
    {
        /* no check, or no card */
    }
    return st;
}

/**
 * @brief Mount on one line first, as the last card judged is mounted, after a
 *        cycle of its supply, and take the four-line way when another card is
 *        the one that answers.
 *
 * A card that failed a write may answer nothing until its supply is cycled,
 * on one line too, and a mount that found nothing would forget what was
 * judged of it as of a card taken out.
 *
 * @return What khal_sdmmc_mount() said of a card that does not answer, or
 *         card_mount_asked()'s answer for a card that was not judged.
 */
static kode_status_t card_mount_known(void)
{
    card_cycle();

    kode_status_t st = khal_sdmmc_mount(SD_WIDTH_NARROW, s_freq_req, &s_snap);

    if ( KODE_OK != st )
    {
        return st;
    }
    if ( is_judged(&s_snap) )
    {
        KLOG_D(TAG, "the card took no write on four lines before; it is "
                    "mounted on one");
        return card_prove_one();
    }
    /* Another card: let go of it, and it is judged as a card nobody judged. */
    KLOG_D(TAG, "a card that was not judged is in the slot");
    khal_sdmmc_unmount();
    (void)memset(&s_snap, 0, sizeof(s_snap));
    return card_mount_asked();
}

/**
 * @brief Mount the card, quietly after a miss.
 *
 * With the check asked for, a card remembered as one that takes no write on
 * four lines is mounted on one at once; every other mount is card_mount_asked().
 * With the check off the mount is as asked and nothing is remembered or
 * consulted.
 *
 * @return What the mount said of the card.
 */
static kode_status_t card_mount(void)
{
    kode_status_t st;

    probe_quiet(true);
    if ( s_check_req && (SD_WIDTH_NARROW < s_width_req)
        && (SD_VERDICT_ONE == s_verdict) )
    {
        st = card_mount_known();
    }
    else
    {
        st = card_mount_asked();
    }
    probe_quiet(false);
    return st;
}

/**
 * @brief Power the card and mount its volume, four data lines first and then
 *        one, a four-line mount proved to take a write.
 *
 * @return KODE_OK with s_snap filled in; KODE_DENIED when khal refused the
 *         mount without a try, which is no answer about the card and is said
 *         at debug level only; any other status with s_snap cleared and the
 *         reason kept in s_why, logged when it differs from the last one.
 */
static kode_status_t card_up(void)
{
    s_everyday = false;
    card_power();

    kode_status_t const st = card_mount();

    if ( KODE_TIMEOUT == st )
    {
        /* An empty slot: what was known of the card that was in it goes with
         * it, so a card put back is judged afresh, the one taken out in the
         * middle of a write included. A card that stays in never answers so,
         * and is judged once while it is in. */
        s_proof   = SD_PROOF_NONE;
        s_verdict = SD_VERDICT_NONE;
    }
    if ( KODE_OK == st )
    {
        s_why      = KDRV_SD_WHY_NONE;
        s_everyday = s_check_req && ((uint8_t)KDRV_SD_WIDTH_USE == s_width_req);
    }
    else if ( KODE_DENIED == st )
    {
        KLOG_D(TAG, "the mount was refused");
    }
    else
    {
        kdrv_sd_why_t const why = why_of(st);

        (void)memset(&s_snap, 0, sizeof(s_snap));
        if ( why == s_why )
        {
            /* Every bring-up asks the slot again; what it found was told once. */
            KLOG_D(TAG, "still: %s", WHY_SAID[why]);
        }
        else if ( KDRV_SD_WHY_NO_CARD == why )
        {
            /* An empty slot is a board without a card, not a fault. */
            KLOG_N(TAG, "no card");
            KLOG_D(TAG, "the card's supply is switched by SD_PWR = GPIO%u, "
                        "driven low: a card that is in and silent points at Q1",
                   (unsigned)KBSP_PIN_SD_PWR);
        }
        else if ( KDRV_SD_WHY_NO_FAT == why )
        {
            KLOG_W(TAG, "the card has no FAT volume to read (exFAT, or not "
                        "formatted); it stays unmounted");
        }
        else
        {
            KLOG_E(TAG, "the card would not come up (%d)", (int)st);
        }
        s_why = why;
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

kdrv_sd_why_t kdrv_sd_why(void)
{
    return s_why;
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
    s_everyday = false;
    KLOG_I(TAG, "card unmounted");
}

kode_status_t kdrv_sd_usage(uint64_t *total, uint64_t *free_bytes)
{
    return khal_sdmmc_info(total, free_bytes);
}

uint32_t kdrv_sd_cluster_bytes(void)
{
    return khal_sdmmc_cluster_bytes();
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
        KLOG_N(TAG, "no card mounted: %s", WHY_SAID[s_why]);
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
