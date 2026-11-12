/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_khal_sdmmc.c
 * @brief A card in a slot: a real directory for its files, a sparse table for its sectors.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fakes.h"
#include "khal_sdmmc.h"
#include "kode_log.h"

#define SD_SECTOR_SIZE 512U
/** @brief Sectors the table can hold: the probe writes 2048 of them. */
#define SD_STORE_MAX   4096U

typedef struct {
    uint32_t sector;
    bool     used;
    uint8_t  data[SD_SECTOR_SIZE];
} sector_slot_t;

static sector_slot_t s_store[SD_STORE_MAX];

static char s_dir[256];
/* Where the mount point is pointed while the card refuses files: a path under
 * a symlink that points at itself, so every open and every mkdir under it
 * fails with ELOOP. See fake_sdmmc_fail_file_writes(). */
static char s_dir_nowrite[256];
static bool s_fail_file_writes;
static bool s_mounted;
static bool s_refuse;   /**< The controller is not up: hosted, and the link is down. */
static bool s_deny;     /**< The controller went away between the query and the mount. */
static bool s_present  = true;
static bool s_no_volume; /**< A card answers, with no FAT volume on it. */
static bool s_fail_writes;
static bool s_fail_reads;
static bool s_fail_wide_writes;   /**< Writes fail while the mount is wider than one line. */
static bool s_fail_wide_reads;    /**< Reads fail while the mount is wider than one line.  */
static bool     s_reads_limited;  /**< Reads fail once s_reads_left have passed.           */
static unsigned s_reads_left;     /**< Reads that still succeed while limited.             */
static uint8_t  s_fail_mount_width; /**< The width the failing mounts ask for.             */
static unsigned s_fail_mount_left;  /**< How many more mounts of it fail.                  */
static bool s_fail_format;
static uint32_t s_read_ms;    /**< What one raw read is made to cost.        */

static khal_sdmmc_card_t s_card;      /**< What the next mount reports.      */
static khal_sdmmc_card_t s_live;      /**< What the current mount reported.  */
static uint8_t  s_max_width = 4U;     /**< The widest mount that succeeds.   */
static uint32_t s_alias;              /**< Non-zero: the real size, in sectors. */
static uint32_t s_total_mb;           /**< Non-zero: force the volume's size. */
static bool     s_hosted;

static unsigned s_mounts;
static uint8_t  s_last_width;
static uint32_t s_last_khz;
static unsigned s_raw_writes;

/** @brief Calls the trace keeps. */
#define SD_TRACE_MAX  32U
/** @brief Characters of one traced call, the terminator included. */
#define SD_TRACE_LEN  40U

static char     s_trace[SD_TRACE_MAX][SD_TRACE_LEN];
static unsigned s_trace_count;

/* ---- The directory ------------------------------------------------------- */

/**
 * @brief Empty @p dir recursively, leaving the directory itself.
 *
 * Recursive for the same reason the internal partition's own reset is:
 * remove() refuses a non-empty directory, so a case that created a tree on
 * the card - an app data folder, say - used to leave the whole of it behind
 * for every case after it, and a listing would then count entries nobody in
 * that case had made.
 *
 * @param dir Directory to empty; created if it does not exist.
 */
static void dir_reset(const char *dir)
{
    (void)mkdir(dir, 0777);

    DIR *d = opendir(dir);
    if ( NULL == d )
    {
        return;
    }
    struct dirent *ent;
    while ( (ent = readdir(d)) != NULL )
    {
        if ( (strcmp(ent->d_name, ".") == 0) || (strcmp(ent->d_name, "..") == 0) )
        {
            continue;
        }
        char path[512];
        (void)snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

        struct stat st;
        if ( (stat(path, &st) == 0) && S_ISDIR(st.st_mode) )
        {
            dir_reset(path);
            (void)rmdir(path);
        }
        else
        {
            (void)remove(path);
        }
    }
    (void)closedir(d);
}

/** @brief How many bytes the files in the mount point add up to. */
static uint64_t dir_bytes(void)
{
    uint64_t total = 0U;

    DIR *d = opendir(s_dir);
    if ( NULL == d )
    {
        return 0U;
    }
    struct dirent *ent;
    while ( (ent = readdir(d)) != NULL )
    {
        char        path[512];
        struct stat st;

        (void)snprintf(path, sizeof(path), "%s/%s", s_dir, ent->d_name);
        if ( (stat(path, &st) == 0) && S_ISREG(st.st_mode) )
        {
            total += (uint64_t)st.st_size;
        }
    }
    (void)closedir(d);
    return total;
}

/* ---- Controls ------------------------------------------------------------ */

/** @brief The card a case gets when it arranges nothing: healthy, 30000 MB. */
static void default_card(khal_sdmmc_card_t *c)
{
    (void)memset(c, 0, sizeof(*c));
    c->mfg_id   = 0x03U;
    c->oem_id   = 0x5344U;
    (void)snprintf(c->name, sizeof(c->name), "%s", "KODE1");
    c->revision = 0x30U;
    c->serial   = 0xDEADBEEFU;
    c->date     = 0x1A9U;              /* 2026-09 */

    c->csd_ver        = 1U;
    c->sectors        = 61440000U;     /* 30000 MB of 512-byte sectors */
    c->sector_size    = SD_SECTOR_SIZE;
    c->read_block_len = SD_SECTOR_SIZE;
    c->ccc            = 0x5B5U;
    c->tr_speed_khz   = 50000U;

    c->sd_spec     = 2U;
    c->bus_widths  = 0x05U;            /* 1-bit and 4-bit */
    c->erase_state = false;

    c->au_kb           = 4096U;
    c->erase_size_au   = 1U;
    c->erase_timeout_s = 2U;
    c->erase_offset_s  = 1U;
    c->discard         = true;
    c->fule            = false;

    c->high_capacity = true;
    c->bus_width     = 4U;
    c->freq_khz      = 40000U;
    c->max_freq_khz  = 40000U;
    c->rca           = 0x1234U;
    c->is_mmc        = false;
    c->is_uhs1       = false;
}

void fake_sdmmc_reset(void)
{
    /* TMPDIR ends in a slash on macOS and is usually not set at all on Linux,
     * where the fallback below has none - so the separator is written here
     * rather than assumed. Without it the Linux path was "/tmpkode_sd", a
     * directory at the root of the filesystem that only CI's root could even
     * create. */
    const char *tmp = getenv("TMPDIR");
    const char *sep = "/";

    if ( (NULL == tmp) || ('\0' == tmp[0]) )
    {
        tmp = "/tmp";
    }
    if ( '/' == tmp[strlen(tmp) - 1U] )
    {
        sep = "";
    }
    (void)snprintf(s_dir, sizeof(s_dir), "%s%skode_sd", tmp, sep);
    dir_reset(s_dir);

    /* The unwritable mount point, built once and left in place: a symlink
     * pointing at its own name, with a path hanging off it. Resolving it
     * never terminates, so the kernel gives up with ELOOP - and unlike a
     * permission bit, that is true of root as well. */
    /* Smaller than s_dir_nowrite by more than the "/d" appended to it below,
     * so GCC can see the second snprintf cannot truncate. Same buffer size for
     * both and -Wformat-truncation is right to complain. */
    char blocker[192];
    (void)snprintf(blocker, sizeof(blocker), "%s%skode_sd_nowrite", tmp, sep);
    (void)snprintf(s_dir_nowrite, sizeof(s_dir_nowrite), "%s/d", blocker);
    (void)unlink(blocker);
    /* glibc marks symlink() warn_unused_result, which a (void) cast does not
     * silence under GCC; a blocker that could not be made is worth a line. */
    if ( symlink("kode_sd_nowrite", blocker) != 0 )
    {
        (void)fprintf(stderr, "fake_khal_sdmmc: cannot create %s\n", blocker);
    }
    s_fail_file_writes = false;

    (void)memset(s_store, 0, sizeof(s_store));
    default_card(&s_card);
    (void)memset(&s_live, 0, sizeof(s_live));

    s_mounted     = false;
    s_refuse      = false;
    s_deny        = false;
    s_present     = true;
    s_no_volume   = false;
    s_fail_writes = false;
    s_fail_reads  = false;
    s_fail_wide_writes = false;
    s_fail_wide_reads  = false;
    s_reads_limited    = false;
    s_reads_left       = 0U;
    s_fail_mount_width = 0U;
    s_fail_mount_left  = 0U;
    s_fail_format = false;
    s_read_ms     = 0U;
    s_max_width   = 4U;
    s_alias       = 0U;
    s_total_mb    = 0U;
    s_hosted      = false;
    s_mounts      = 0U;
    s_last_width  = 0U;
    s_last_khz    = 0U;
    s_raw_writes  = 0U;
    s_trace_count = 0U;
}

const char *fake_sdmmc_dir(void)
{
    return s_dir;
}

void fake_sdmmc_set_no_volume(bool no_volume)
{
    s_no_volume = no_volume;
}

void fake_sdmmc_set_present(bool present)
{
    s_present = present;
    if ( !present )
    {
        s_mounted = false;
    }
}

void fake_sdmmc_set_card(const khal_sdmmc_card_t *card)
{
    s_card = *card;
}

void fake_sdmmc_default_card(khal_sdmmc_card_t *out)
{
    default_card(out);
}

void fake_sdmmc_set_max_width(uint8_t width)
{
    s_max_width = width;
}

void fake_sdmmc_set_alias(uint32_t real_sectors)
{
    s_alias = real_sectors;
}

void fake_sdmmc_set_total_mb(uint32_t total_mb)
{
    s_total_mb = total_mb;
}

void fake_sdmmc_fail_writes(bool fail)
{
    s_fail_writes = fail;
}

void fake_sdmmc_fail_reads(bool fail)
{
    s_fail_reads = fail;
}

void fake_sdmmc_fail_reads_after(unsigned ok)
{
    s_reads_limited = true;
    s_reads_left    = ok;
}

void fake_sdmmc_fail_wide_writes(bool fail)
{
    s_fail_wide_writes = fail;
}

void fake_sdmmc_fail_wide_reads(bool fail)
{
    s_fail_wide_reads = fail;
}

void fake_sdmmc_fail_mounts(uint8_t width, unsigned n)
{
    s_fail_mount_width = width;
    s_fail_mount_left  = n;
}

void fake_sdmmc_set_read_ms(uint32_t ms)
{
    s_read_ms = ms;
}

void fake_sdmmc_fail_file_writes(bool fail)
{
    /* The failure is a real one at a real path, rather than a flag this fake
     * would have to thread through every call: what fails is fopen() inside
     * kdrv_storage.c, exactly where a card that refuses writes fails.
     *
     * The path is made unopenable by hanging it off a symlink that points at
     * itself, not by taking the directory's write bit away. A permission bit
     * does not stop root, and CI runs the suite as root inside the IDF image,
     * so the chmod this used to do worked on the bench and nowhere else: the
     * case passed here and failed there. ELOOP is nobody's privilege to
     * override.
     *
     * It is deliberately not the ENOTDIR a regular-file parent used to give,
     * which is what a card that refuses writes never produces: ENOTDIR now
     * means "a component of your path is a file" all the way up to the
     * console's `notdir`, and a fake that raised it would have been
     * exercising a path bug rather than a medium that said no. ELOOP is not
     * the EIO a real counterfeit card gives either - no test can make a
     * kernel raise that at will - but it lands in the same bucket, the one
     * kdrv_storage.c hands to KODE_IO_ERROR because the path was fine and the
     * open failed anyway. */
    s_fail_file_writes = fail;
}

void fake_sdmmc_fail_format(bool fail)
{
    s_fail_format = fail;
}

void fake_sdmmc_set_refuse(bool refuse)
{
    s_refuse = refuse;
}

void fake_sdmmc_set_deny(bool deny)
{
    s_deny = deny;
}

unsigned fake_sdmmc_mounts(void)
{
    return s_mounts;
}

uint8_t fake_sdmmc_last_width(void)
{
    return s_last_width;
}

uint32_t fake_sdmmc_last_khz(void)
{
    return s_last_khz;
}

unsigned fake_sdmmc_raw_writes(void)
{
    return s_raw_writes;
}

void fake_sdmmc_trace_reset(void)
{
    s_trace_count = 0U;
    s_raw_writes  = 0U;
}

unsigned fake_sdmmc_trace_count(void)
{
    return s_trace_count;
}

const char *fake_sdmmc_trace_line(unsigned idx)
{
    if ( (idx >= s_trace_count) || (SD_TRACE_MAX <= idx) )
    {
        return NULL;
    }
    return s_trace[idx];
}

void fake_sdmmc_trace_note(const char *what)
{
    if ( SD_TRACE_MAX > s_trace_count )
    {
        (void)snprintf(s_trace[s_trace_count], SD_TRACE_LEN, "%s", what);
    }
    ++s_trace_count;
}

/** @brief Trace one call of the seam: its name and, when it has one, its number. */
static void trace_call(const char *name, uint32_t arg, bool has_arg)
{
    char line[SD_TRACE_LEN];

    if ( has_arg )
    {
        (void)snprintf(line, sizeof(line), "%s %lu", name, (unsigned long)arg);
    }
    else
    {
        (void)snprintf(line, sizeof(line), "%s", name);
    }
    fake_sdmmc_trace_note(line);
}

/* ---- khal_sdmmc ---------------------------------------------------------- */

const char *khal_sdmmc_mount_point(void)
{
    if ( s_fail_file_writes )
    {
        return s_dir_nowrite;
    }
    return s_dir;
}

bool khal_sdmmc_mounted(void)
{
    return s_mounted;
}

void khal_sdmmc_set_hosted(bool hosted)
{
    s_hosted = hosted;
}

bool khal_sdmmc_hosted(void)
{
    return s_hosted;
}

bool khal_sdmmc_ready(void)
{
    return !s_refuse;
}

/**
 * @brief Answer one identification the way the card in the slot would.
 *
 * @param width   Data lines asked for.
 * @param max_khz Clock ceiling asked for.
 * @return KODE_OK with s_live filled in; KODE_ERROR with no card, or with a
 *         card that will not go that wide.
 */
static kode_status_t identify(uint8_t width, uint32_t max_khz)
{
    if ( !s_present )
    {
        /* What ESP-IDF says, under its own tags, of a slot nobody answers
         * from: kdrv_sd holds these tags quiet after the first miss. */
        KLOG_E(FAKE_SLOT_TAG_PROTOCOL, "%s", FAKE_SLOT_LINE);
        KLOG_W(FAKE_SLOT_TAG_MOUNT, "%s", FAKE_SLOT_LINE);
        KLOG_W(FAKE_SLOT_TAG_PINS, "%s", FAKE_SLOT_LINE);
        return KODE_TIMEOUT;
    }
    if ( s_no_volume )
    {
        KLOG_E(FAKE_SLOT_TAG_MOUNT, "%s", FAKE_SLOT_LINE);
        return KODE_IO_ERROR;
    }
    if ( width > s_max_width )
    {
        return KODE_ERROR;
    }
    s_live = s_card;
    /* What was negotiated, not what the card can do. */
    s_live.bus_width = width;
    if ( max_khz < s_live.freq_khz )
    {
        s_live.freq_khz = max_khz;
    }
    return KODE_OK;
}

kode_status_t khal_sdmmc_mount(uint8_t width, uint32_t max_khz,
                               khal_sdmmc_card_t *out)
{
    if ( ((1U != width) && (4U != width)) || (0U == max_khz) )
    {
        return KODE_DENIED;
    }
    if ( s_mounted )
    {
        return KODE_DENIED;
    }
    ++s_mounts;
    trace_call("mount", (uint32_t)width, true);
    if ( s_refuse || s_deny )
    {
        return KODE_DENIED;
    }
    s_last_width = width;
    s_last_khz   = max_khz;

    if ( (0U < s_fail_mount_left) && (width == s_fail_mount_width) )
    {
        --s_fail_mount_left;
        return KODE_TIMEOUT;
    }
    kode_status_t const st = identify(width, max_khz);

    if ( KODE_OK != st )
    {
        return st;
    }
    s_mounted = true;
    if ( NULL != out )
    {
        *out = s_live;
    }
    return KODE_OK;
}

void khal_sdmmc_unmount(void)
{
    trace_call("unmount", 0U, false);
    s_mounted = false;
}

kode_status_t khal_sdmmc_info(uint64_t *total, uint64_t *free_bytes)
{
    if ( !s_mounted )
    {
        return KODE_DENIED;
    }
    uint64_t t;
    if ( 0U != s_total_mb )
    {
        t = (uint64_t)s_total_mb * 1024ULL * 1024ULL;
    }
    else
    {
        /* A megabyte of the card goes to the filesystem itself. */
        t = ((uint64_t)s_live.sectors * (uint64_t)s_live.sector_size)
            - (1024ULL * 1024ULL);
    }
    uint64_t const used = dir_bytes();

    if ( NULL != total )
    {
        *total = t;
    }
    if ( NULL != free_bytes )
    {
        *free_bytes = (used < t) ? (t - used) : 0U;
    }
    return KODE_OK;
}

kode_status_t khal_sdmmc_format(void)
{
    if ( !s_mounted )
    {
        return KODE_DENIED;
    }
    if ( s_fail_format )
    {
        /* The seam's contract: a failed format leaves nothing mounted. */
        s_mounted = false;
        return KODE_ERROR;
    }
    dir_reset(s_dir);
    return KODE_OK;
}

/* ---- The sparse sector table --------------------------------------------- */

/**
 * @brief Find the slot holding @p sector, or claim a free one.
 *
 * @param sector Sector index, already put through the alias.
 * @param claim  true to take a free slot when the sector is not stored yet.
 * @return The slot, or NULL when the sector is absent and none was claimed,
 *         or when the table is full.
 */
static sector_slot_t *slot_of(uint32_t sector, bool claim)
{
    sector_slot_t *spare = NULL;

    for ( unsigned i = 0U; SD_STORE_MAX > i; ++i )
    {
        if ( s_store[i].used )
        {
            if ( s_store[i].sector == sector )
            {
                return &s_store[i];
            }
        }
        else if ( NULL == spare )
        {
            spare = &s_store[i];
        }
        else
        {
            /* keep looking for a match */
        }
    }
    if ( !claim || (NULL == spare) )
    {
        return NULL;
    }
    spare->used   = true;
    spare->sector = sector;
    (void)memset(spare->data, 0, sizeof(spare->data));
    return spare;
}

/** @brief Where a sector really lands on a card that lies about its size. */
static uint32_t aliased(uint32_t sector)
{
    if ( 0U == s_alias )
    {
        return sector;
    }
    return sector % s_alias;
}

kode_status_t khal_sdmmc_read_sectors(uint32_t first, uint32_t count, void *dst)
{
    if ( (!s_mounted) || (NULL == dst) )
    {
        return KODE_DENIED;
    }
    trace_call("read", first, true);
    /* Charged before the failure check, so a case can make a read both slow
     * and doomed if it wants to. */
    if ( 0U != s_read_ms )
    {
        fake_time_advance_ms(s_read_ms);
    }
    if ( s_fail_reads || (s_fail_wide_reads && (1U < s_live.bus_width)) )
    {
        return KODE_ERROR;
    }
    if ( s_reads_limited )
    {
        if ( 0U == s_reads_left )
        {
            return KODE_ERROR;
        }
        --s_reads_left;
    }
    if ( (first + count) > s_live.sectors )
    {
        return KODE_ERROR;
    }
    uint8_t *out = (uint8_t *)dst;

    for ( uint32_t i = 0U; i < count; ++i )
    {
        const sector_slot_t * const slot = slot_of(aliased(first + i), false);
        if ( NULL == slot )
        {
            (void)memset(&out[i * SD_SECTOR_SIZE], 0, SD_SECTOR_SIZE);
        }
        else
        {
            (void)memcpy(&out[i * SD_SECTOR_SIZE], slot->data, SD_SECTOR_SIZE);
        }
    }
    return KODE_OK;
}

kode_status_t khal_sdmmc_write_sectors(uint32_t first, uint32_t count,
                                       const void *src)
{
    if ( (!s_mounted) || (NULL == src) )
    {
        return KODE_DENIED;
    }
    trace_call("write", first, true);
    if ( s_fail_writes || (s_fail_wide_writes && (1U < s_live.bus_width)) )
    {
        return KODE_ERROR;
    }
    if ( (first + count) > s_live.sectors )
    {
        return KODE_ERROR;
    }
    ++s_raw_writes;
    const uint8_t *in = (const uint8_t *)src;

    for ( uint32_t i = 0U; i < count; ++i )
    {
        sector_slot_t * const slot = slot_of(aliased(first + i), true);
        if ( NULL == slot )
        {
            return KODE_ERROR;   /* the table is full: say so, do not pass */
        }
        (void)memcpy(slot->data, &in[i * SD_SECTOR_SIZE], SD_SECTOR_SIZE);
    }
    return KODE_OK;
}
