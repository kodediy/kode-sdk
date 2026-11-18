/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_apps.c
 * @brief The `apps` region and the `app_pending` mailbox: the only file that knows their layout.
 */
#include "kdrv_apps.h"

#include "kapp_cat.h"
#include "kdrv_kfw.h"
#include "kdrv_desc_fill.h"
#include "kdrv_bounce.h"
#include "kdrv_fs.h"
#include "kdrv_storage.h"

#include <stdio.h>
#include <string.h>

#include "kode_log.h"

#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "khal_crc32.h"
#include "khal_system.h"
#include "kdrv_image_walk.h"
#include "psa/crypto.h"

#define TAG              "kdrv.apps"
#define APPS_SECTOR      0x1000U   /* the 4 KB unit every metadata sector is erased in */
/** Bytes of the region one extent CRC maps at a time. */
#define APPS_CRC_WINDOW  (512U * 1024U)

/* ---- State: the region partition and its RAM directory ------------------- */

static const esp_partition_t *s_part;         /* the `apps` partition, or NULL */
static kapps_dir_t             s_dir;          /* RAM working copy             */
static uint32_t                s_dir_active;   /* KAPPS_DIR_A_OFF or _B_OFF    */
static bool                    s_inited;       /* kdrv_apps_init() succeeded once */

/* ---- State: the write session --------------------------------------------- */

static bool     s_wr_open;
static uint32_t s_wr_off;
static uint32_t s_wr_size;
static uint32_t s_wr_written;
static uint32_t s_wr_erased;   /* region offset erased up to, monotonic */
/* Region offset up to which the flash from s_wr_off on may have been changed:
 * s_wr_erased, and the block of an erase that failed. An empty run (nothing at
 * or past the start) is what a record with nothing in it looks like. */
static uint32_t s_wr_reach;

/**
 * @brief Find the `app_pending` mailbox partition.
 * @return The partition, or NULL when the partition table has none.
 */
static const esp_partition_t *pending_part(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    (esp_partition_subtype_t)KAPPS_PENDING_SUBTYPE,
                                    "app_pending");
}

/**
 * @brief Where the mailbox's intent sector starts, which is also where its
 *        image space ends: the partition's last 4 KB.
 *
 * Derived, never written down. Every reader of the mailbox asks this one
 * function - the write bound, the walk, a container's claimed length, the
 * intent read and the erase - so no two paths on the board can disagree
 * about how much of it an image may use, and the firmware follows the table
 * if app_pending grows. What does not follow by itself is a host tool that
 * writes the intent word at KAPPS_PENDING_INTENT_OFF: see that constant.
 */
static uint32_t pending_intent_off(const esp_partition_t *p)
{
    return (uint32_t)p->size - APPS_SECTOR;
}

/**
 * @brief The CRC32 a directory copy must carry: of every byte before its crc field.
 * @param d The directory.
 * @return The CRC32.
 */
static uint32_t dir_crc(const kapps_dir_t *d)
{
    return esp_rom_crc32_le(UINT32_MAX, (const uint8_t *)d, offsetof(kapps_dir_t, crc));
}

/**
 * @brief Read a directory copy and say what it is.
 *
 * Only a blank or a corrupt copy may be formatted over. A copy that could not
 * be read, or that another format version wrote - the kodeOS a board is rolled
 * back from - holds the only index of the apps there, and this firmware cannot
 * know what it says.
 *
 * @param off Region offset of the copy.
 * @param out Receives the copy.
 * @return Its kind.
 */
static kapps_dir_copy_t dir_read(uint32_t off, kapps_dir_t *out)
{
    kapps_dir_copy_t kind = KAPPS_DIR_COPY_FOREIGN;

    if ( esp_partition_read(s_part, off, out, sizeof(*out)) == ESP_OK )
    {
        kind = kapps_dir_classify(out, dir_crc(out), (uint32_t)s_part->size);
    }
    return kind;
}

/** @brief Erase @p off's sector and write @p d there. */
static kode_status_t dir_write(uint32_t off, const kapps_dir_t *d)
{
    esp_err_t err = esp_partition_erase_range(s_part, off, APPS_SECTOR);
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "dir erase @0x%x failed (%s)", (unsigned)off, esp_err_to_name(err));
        return KODE_ERROR;
    }
    /* The magic goes last. A power cut lands any prefix of what was written,
     * and a copy whose magic landed before its version reads as another
     * format's - FOREIGN, which shuts the region on every boot. Written after
     * everything else, the magic is only there once the copy is whole; before
     * that the sector reads BLANK or CORRUPT and the other copy wins. */
    size_t const body = offsetof(kapps_dir_t, version);
    err = esp_partition_write(s_part, off + body, (const uint8_t *)d + body,
                              sizeof(*d) - body);
    if ( ESP_OK == err )
    {
        err = esp_partition_write(s_part, off, &d->magic, sizeof(d->magic));
    }
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "dir write @0x%x failed (%s)", (unsigned)off, esp_err_to_name(err));
        return KODE_ERROR;
    }
    return KODE_OK;
}

/* ---- The region itself ---------------------------------------------------- */

kode_status_t kdrv_apps_init(void)
{
    /* Safe to call more than once: AO_Boot calls it before asking about the
     * boot request, AO_AppMgr calls it when claiming the region - whoever
     * runs first pays the read, and everyone after gets the same answer for
     * free rather than re-reading flash they already trust. */
    if ( s_inited )
    {
        return KODE_OK;
    }

    /* Before the first manifest is parsed, because a manifest read that
     * cannot see the override table would report the image's own category
     * and cache it upstream. Failure is not fatal: an empty table means no
     * overrides, which is exactly the state before this feature existed. */
    (void)kapp_cat_init();

    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      (esp_partition_subtype_t)KAPPS_PART_SUBTYPE,
                                      "apps");
    if ( NULL == s_part )
    {
        KLOG_E(TAG, "no 'apps' partition in the table");
        return KODE_DENIED;
    }

    kapps_dir_t      a;
    kapps_dir_t      b;
    kapps_dir_copy_t const a_kind = dir_read(KAPPS_DIR_A_OFF, &a);
    kapps_dir_copy_t const b_kind = dir_read(KAPPS_DIR_B_OFF, &b);
    bool const       a_ok   = (KAPPS_DIR_COPY_OK == a_kind);
    bool const       b_ok   = (KAPPS_DIR_COPY_OK == b_kind);

    if ( (KAPPS_DIR_COPY_FOREIGN == a_kind) || (KAPPS_DIR_COPY_FOREIGN == b_kind) )
    {
        /* Neither used nor formatted over: the region stays shut, and every
         * call that guards on s_part refuses. A later init looks again. */
        static const char *const KIND[] = { "ok", "blank", "corrupt", "foreign" };
        KLOG_E(TAG, "apps region directory unreadable or of another format "
                    "(A %s, B %s); left as it is", KIND[a_kind], KIND[b_kind]);
        s_part = NULL;
        return KODE_ERROR;
    }
    kapps_dir_pick_t const pick = kapps_dir_pick(a_ok, a.gen, b_ok, b.gen);

    if ( KAPPS_DIR_PICK_A == pick )
    {
        s_dir        = a;
        s_dir_active = KAPPS_DIR_A_OFF;
    }
    else if ( KAPPS_DIR_PICK_B == pick )
    {
        s_dir        = b;
        s_dir_active = KAPPS_DIR_B_OFF;
    }
    else
    {
        /* Neither copy is valid: a fresh board, or a corrupt region either
         * way. Format: empty directory, gen=1, written to A; B erased so it
         * cannot later be mistaken for something newer. Build the candidate
         * in a local - same adopt-on-success discipline as dir_commit - so a
         * failed write leaves s_dir exactly as it was (all-zero, magic == 0,
         * "not ready") instead of looking valid in RAM while flash disagrees. */
        kapps_dir_t fresh;
        memset(&fresh, 0, sizeof(fresh));
        fresh.magic   = KAPPS_DIR_MAGIC;
        fresh.version = KAPPS_FMT_VERSION;
        fresh.gen     = 1U;
        fresh.crc     = dir_crc(&fresh);
        if ( dir_write(KAPPS_DIR_A_OFF, &fresh) != KODE_OK )
        {
            return KODE_ERROR;
        }
        (void)esp_partition_erase_range(s_part, KAPPS_DIR_B_OFF, APPS_SECTOR);
        s_dir        = fresh;
        s_dir_active = KAPPS_DIR_A_OFF;
        KLOG_N(TAG, "apps region is new: directory formatted (gen=1)");
    }

    KLOG_I(TAG, "apps region ready: %u app(s), gen=%u, %u KB total",
           (unsigned)s_dir.count, (unsigned)s_dir.gen,
           (unsigned)(s_part->size / 1024U));
    s_inited = true;
    return KODE_OK;
}

bool kdrv_apps_present(void)
{
    return (NULL != s_part);
}

uint32_t kdrv_apps_region_size(void)
{
    return (NULL != s_part) ? (uint32_t)s_part->size : 0U;
}

uint32_t kdrv_apps_address(void)
{
    return (NULL != s_part) ? (uint32_t)s_part->address : 0U;
}

const kapps_dir_t *kdrv_apps_dir(void)
{
    return &s_dir;
}

kode_status_t kdrv_apps_dir_commit(const kapps_dir_t *d)
{
    if ( (NULL == s_part) || (NULL == d) )
    {
        return KODE_DENIED;
    }
    /* What a reader would refuse is never sealed with a good CRC. */
    if ( !kapps_dir_sane(d, (uint32_t)s_part->size) )
    {
        KLOG_E(TAG, "a directory that does not fit the region was not "
                    "committed (%u entries)", (unsigned)d->count);
        return KODE_DENIED;
    }

    kapps_dir_t next = *d;
    next.magic   = KAPPS_DIR_MAGIC;
    next.version = KAPPS_FMT_VERSION;
    next.gen     = s_dir.gen + 1U;
    next.crc     = dir_crc(&next);

    /* Always the other copy: the one believed valid stays untouched until this write is good. */
    uint32_t const target = (KAPPS_DIR_A_OFF == s_dir_active) ? KAPPS_DIR_B_OFF
                                                                : KAPPS_DIR_A_OFF;
    kode_status_t const st = dir_write(target, &next);
    if ( KODE_OK != st )
    {
        return st;
    }
    s_dir        = next;      /* only adopted once the write above succeeded */
    s_dir_active = target;
    s_wr_off     = 0U;        /* the new directory is what says which extents hold apps: */
    s_wr_reach   = 0U;        /* no erase made before it speaks for them */
    return KODE_OK;
}

/* ---- The allocator: pure, no partition, no static state -------------------- */

typedef struct {
    uint8_t  idx;      /* index into the caller's d->e[] */
    uint32_t offset;
    uint32_t size;
    uint32_t seq;
} apps_live_t;

_Static_assert(KAPPS_MAX_ENTRIES <= 64U, "kdrv_apps_plan() says which entries are copied in 64 bits");

/**
 * @brief Whether live entry @p a is evicted before live entry @p b.
 *
 * An app with its copy on the card before one without; then the older one.
 *
 * @param a      A live entry.
 * @param b      Another.
 * @param copied Bit n: directory entry n has its copy on the card.
 * @return true when @p a goes first.
 */
static bool plan_goes_first(const apps_live_t *a, const apps_live_t *b, uint64_t copied)
{
    bool const a_copied = ((copied >> a->idx) & 1U) != 0U;
    bool const b_copied = ((copied >> b->idx) & 1U) != 0U;
    bool       first;

    if ( a_copied != b_copied )
    {
        first = a_copied;
    }
    else
    {
        first = a->seq < b->seq;
    }
    return first;
}

kode_status_t kdrv_apps_plan(const kapps_dir_t *d, uint32_t region_size,
                             uint32_t img_size, uint8_t replacing,
                             uint64_t copied, kapps_plan_t *out)
{
    if ( (NULL == d) || (NULL == out) || (KAPPS_DATA_OFF >= region_size) )
    {
        return KODE_ERROR;
    }

    uint32_t const need = (img_size + KAPPS_ALIGN - 1U) & ~(KAPPS_ALIGN - 1U);
    if ( (0U == need) || ((region_size - KAPPS_DATA_OFF) < need) )
    {
        return KODE_ERROR;   /* cannot fit even in the empty region */
    }

    apps_live_t live[KAPPS_MAX_ENTRIES];
    uint32_t n = 0U;
    uint32_t const count = (KAPPS_MAX_ENTRIES < d->count) ? KAPPS_MAX_ENTRIES : d->count;
    for ( uint32_t i = 0U; i < count; ++i )
    {
        if ( (d->e[i].flags & KAPPS_ENTRY_VALID) != 0U )
        {
            live[n].idx    = (uint8_t)i;
            live[n].offset = d->e[i].offset;
            live[n].size   = d->e[i].size;
            live[n].seq    = d->e[i].seq;
            ++n;
        }
    }

    out->evict_count = 0U;
    out->slot_victim = KAPPS_NO_VICTIM;
    out->replaced    = KAPPS_NO_VICTIM;

    for ( ;; )
    {
        /* Sort the working set by offset - n is small, insertion sort is fine. */
        for ( uint32_t i = 1U; i < n; ++i )
        {
            apps_live_t const key = live[i];
            uint32_t j = i;
            while ( (0U < j) && (live[j - 1U].offset > key.offset) )
            {
                live[j] = live[j - 1U];
                --j;
            }
            live[j] = key;
        }

        /* Search starts right after the newest install (by seq), 64 KB-aligned. */
        uint32_t search_start = KAPPS_DATA_OFF;
        if ( 0U < n )
        {
            uint32_t newest = 0U;
            for ( uint32_t i = 1U; i < n; ++i )
            {
                if ( live[i].seq > live[newest].seq )
                {
                    newest = i;
                }
            }
            uint32_t const end = live[newest].offset + live[newest].size;
            search_start = (end + KAPPS_ALIGN - 1U) & ~(KAPPS_ALIGN - 1U);
        }

        /* Two passes over the gaps between consecutive live entries (region
         * start/end included): forward from search_start, then the wrap
         * from KAPPS_DATA_OFF back up to it. First gap that fits wins. */
        bool     found        = false;
        uint32_t found_offset = 0U;
        for ( int pass = 0; (2 > pass) && !found; ++pass )
        {
            uint32_t prev_end = KAPPS_DATA_OFF;
            for ( uint32_t i = 0U; i <= n; ++i )
            {
                uint32_t const gap_start = prev_end;
                uint32_t const gap_end   = (i < n) ? live[i].offset : region_size;
                if ( i < n )
                {
                    uint32_t const entry_end = live[i].offset + live[i].size;
                    prev_end = (entry_end + KAPPS_ALIGN - 1U) & ~(KAPPS_ALIGN - 1U);
                }
                if ( gap_end <= gap_start )
                {
                    continue;
                }
                bool const in_pass = (0 == pass) ? (gap_start >= search_start)
                                                  : (gap_start <  search_start);
                if ( in_pass && ((gap_end - gap_start) >= need) )
                {
                    found_offset = gap_start;
                    found        = true;
                    break;
                }
            }
        }

        if ( found )
        {
            out->offset = found_offset;
            /* The directory's own limit, after this plan: the new entry needs
             * a slot, and with every slot live appdir_make_room() will take
             * the oldest survivor once the write is done - unless the app
             * this image replaces survives, which frees its own entry. */
            bool replaces_live = false;
            uint32_t oldest = 0U;
            for ( uint32_t i = 0U; i < n; ++i )
            {
                if ( live[i].idx == replacing )
                {
                    replaces_live = true;
                }
                if ( plan_goes_first(&live[i], &live[oldest], copied) )
                {
                    oldest = i;
                }
            }
            if ( (KAPPS_MAX_ENTRIES <= n) && !replaces_live )
            {
                out->slot_victim = live[oldest].idx;
            }
            return KODE_OK;
        }

        if ( 0U == n )
        {
            return KODE_ERROR;   /* no entries and still no gap */
        }

        /* No room anywhere: evict the app being replaced, else the oldest
         * app with its copy on the card, else the oldest of all, and retry -
         * the freed extent joins the gap search on the next pass. */
        uint32_t victim = 0U;
        bool     same   = false;
        for ( uint32_t i = 0U; i < n; ++i )
        {
            if ( live[i].idx == replacing )
            {
                victim = i;
                same   = true;
                break;
            }
            if ( plan_goes_first(&live[i], &live[victim], copied) )
            {
                victim = i;
            }
        }
        if ( same )
        {
            out->replaced = replacing;
        }
        out->evict[out->evict_count++] = live[victim].idx;
        live[victim] = live[n - 1U];
        --n;
    }
}

/* ---- Writing an image into the region -------------------------------------- */

/** @brief Where a bounced write lands: a partition and an offset in it. */
typedef struct {
    const esp_partition_t *part;   /**< The partition written.         */
    size_t                 base;   /**< Where this write starts in it. */
} part_put_ctx_t;

/** @brief One bounce into a partition, at its place in the write. */
static esp_err_t part_put(void *ctx, size_t at, const void *src, size_t len)
{
    const part_put_ctx_t * const c = (const part_put_ctx_t *)ctx;
    return esp_partition_write(c->part, c->base + at, src, len);
}

/** Bytes of a write looked at, and skipped when all 0xFF, at a time. */
#define APPS_SKIP_PIECE  0x1000U

/** @brief Whether @p len bytes at @p data are all 0xFF. */
static bool all_ff(const uint8_t *data, size_t len)
{
    for ( size_t i = 0U; len > i; i++ )
    {
        if ( 0xFFU != data[i] )
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief Write @p len bytes at @p base of @p part, leaving out every 4 KB piece
 *        that is all 0xFF.
 *
 * Only for flash that is blank where it lands (erased, or found blank): an
 * erased byte already reads 0xFF, so writing it again costs time and changes
 * nothing. The read-back verify that closes every write covers the pieces
 * left out.
 */
static esp_err_t put_skip_blank(const esp_partition_t *part, size_t base, const void *data, size_t len)
{
    const uint8_t *d = (const uint8_t *)data;
    size_t done = 0U;

    while ( len > done )
    {
        size_t n = len - done;
        if ( APPS_SKIP_PIECE < n )
        {
            n = APPS_SKIP_PIECE;
        }
        if ( !all_ff(&d[done], n) )
        {
            part_put_ctx_t at = { part, base + done };
            esp_err_t const err = kdrv_bounce_write(&part_put, &at, &d[done], n);
            if ( ESP_OK != err )
            {
                return err;
            }
        }
        done += n;
    }
    return ESP_OK;
}

kode_status_t kdrv_apps_write_begin(uint32_t offset, uint32_t size)
{
    if ( (NULL == s_part) || s_wr_open )
    {
        return KODE_DENIED;
    }
    if ( (KAPPS_DATA_OFF > offset) || ((offset % KAPPS_ALIGN) != 0U)
        || (((uint64_t)offset + size) > s_part->size) )
    {
        return KODE_ERROR;   /* below KAPPS_DATA_OFF is the directory/boot-request area */
    }
    s_wr_off     = offset;
    s_wr_size    = size;
    s_wr_written = 0U;
    s_wr_erased  = offset;
    s_wr_reach   = offset;
    s_wr_open    = true;
    return KODE_OK;
}

/** Bytes read at a time when checking a block for blank. */
#define APPS_BLANK_READ  4096U

/**
 * @brief Is the 64 KB block at `off` of a partition all 0xFF?
 *
 * Short reads, each holding the flash for microseconds. The buffer is
 * borrowed from PSRAM for the look and given back: nothing forces it into
 * internal RAM. A read that fails, or no memory for the buffer, counts as
 * not blank, so the block is erased as before.
 *
 * @param p   The partition.
 * @param off Partition offset of the block.
 * @return true when every byte of the block is 0xFF.
 */
static bool part_block_blank(const esp_partition_t *p, uint32_t off)
{
    /* cppcheck-suppress misra-c2012-11.5 ; khal_alloc_psram() hands back an untyped block aligned for any type */
    uint32_t * const buf = (uint32_t *)khal_alloc_psram(sizeof(uint32_t), APPS_BLANK_READ);
    bool             blank = (NULL != buf);

    for ( uint32_t at = 0U; blank && (KAPPS_ALIGN > at); at += APPS_BLANK_READ )
    {
        if ( esp_partition_read(p, off + at, buf, APPS_BLANK_READ) != ESP_OK )
        {
            blank = false;
        }
        for ( uint32_t i = 0U; blank && ((APPS_BLANK_READ / sizeof(uint32_t)) > i); i++ )
        {
            if ( 0xFFFFFFFFU != buf[i] )
            {
                blank = false;
            }
        }
    }
    if ( NULL != buf )
    {
        khal_free(buf);
    }
    return blank;
}

/**
 * @brief Is the 64 KB block at `off` of the apps region all 0xFF?
 *
 * @param off Region offset of the block.
 * @return true when every byte of the block is 0xFF.
 */
static bool block_blank(uint32_t off)
{
    return part_block_blank(s_part, off);
}

kode_status_t kdrv_apps_write(const void *data, size_t len)
{
    if ( !s_wr_open || (NULL == data) )
    {
        return KODE_DENIED;
    }
    if ( ((uint64_t)s_wr_written + len) > s_wr_size )
    {
        return KODE_ERROR;
    }

    /* Erase 64 KB blocks one at a time as the write reaches into them - this
     * is what leaves the GUI room to breathe between chunks, exactly like
     * esp_ota_begin(OTA_WITH_SEQUENTIAL_WRITES) does for the OTA slot. A block
     * that is already blank is not erased: a block erase holds the flash for
     * about 150 ms, long enough to starve the haptic feed and the RFID reader,
     * and a board fresh from the line's full erase is blank everywhere. */
    uint32_t const reach = s_wr_off + s_wr_written + (uint32_t)len;
    while ( s_wr_erased < reach )
    {
        s_wr_reach = s_wr_erased + KAPPS_ALIGN;   /* before the erase: a failed one may have changed the block */
        if ( !block_blank(s_wr_erased) )
        {
            esp_err_t const err = esp_partition_erase_range(s_part, s_wr_erased, KAPPS_ALIGN);
            if ( ESP_OK != err )
            {
                KLOG_E(TAG, "erase @0x%x failed (%s)", (unsigned)s_wr_erased, esp_err_to_name(err));
                return KODE_ERROR;
            }
        }
        s_wr_erased += KAPPS_ALIGN;
    }

    esp_err_t const err = put_skip_blank(s_part, (size_t)s_wr_off + (size_t)s_wr_written, data, len);
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "write @0x%x failed (%s)",
               (unsigned)(s_wr_off + s_wr_written), esp_err_to_name(err));
        return KODE_ERROR;
    }
    s_wr_written += (uint32_t)len;
    return KODE_OK;
}

kode_status_t kdrv_apps_write_end(void)
{
    if ( !s_wr_open )
    {
        return KODE_DENIED;
    }
    s_wr_open = false;
    return kdrv_apps_verify(s_wr_off, s_wr_written, NULL);
}

void kdrv_apps_write_abort(void)
{
    s_wr_open = false;
}

bool kdrv_apps_write_reached(uint32_t offset, uint32_t size)
{
    uint64_t const end = (uint64_t)offset + (uint64_t)size;

    return (0U < size) && (s_wr_off < s_wr_reach)
        && (offset < s_wr_reach) && ((uint64_t)s_wr_off < end);
}

/* ---- Reading an image back ------------------------------------------------- */

kode_status_t kdrv_apps_read(uint32_t offset, void *buf, size_t len)
{
    if ( (NULL == s_part) || (NULL == buf) )
    {
        return KODE_DENIED;
    }
    if ( (KAPPS_DATA_OFF > offset)
        || (((uint64_t)offset + (uint64_t)len) > (uint64_t)s_part->size) )
    {
        return KODE_ERROR;
    }
    if ( esp_partition_read(s_part, offset, buf, len) != ESP_OK )
    {
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t kdrv_apps_extent_crc(uint32_t offset, uint32_t size, uint32_t *crc)
{
    if ( (NULL == s_part) || (NULL == crc) )
    {
        return KODE_DENIED;
    }
    if ( (KAPPS_DATA_OFF > offset)
        || (((uint64_t)offset + (uint64_t)size) > (uint64_t)s_part->size) )
    {
        return KODE_ERROR;
    }
    if ( 0U == size )
    {
        return KODE_OK;
    }
    /* Through the mapping the images are verified and run through, a window
     * at a time: the bytes come at the cache's pace, and nothing holds the
     * flash with the cache off the way a read through the driver does. */
    uint32_t      run  = *crc;
    uint32_t      done = 0U;
    kode_status_t st   = KODE_OK;

    while ( (KODE_OK == st) && (done < size) )
    {
        uint32_t                    n   = size - done;
        const void                 *map = NULL;
        esp_partition_mmap_handle_t h   = 0U;

        if ( APPS_CRC_WINDOW < n )
        {
            n = APPS_CRC_WINDOW;
        }
        if ( esp_partition_mmap(s_part, offset + done, n, ESP_PARTITION_MMAP_DATA,
                                &map, &h) != ESP_OK )
        {
            st = KODE_ERROR;
        }
        else
        {
            run   = khal_crc32(run, map, n);
            done += n;
            esp_partition_munmap(h);
        }
    }
    if ( KODE_OK == st )
    {
        *crc = run;
    }
    return st;
}

/**
 * @brief The SHA-256 of @p size bytes of partition @p p from @p offset, through the mapping.
 *
 * IDF invalidates the cache over every range it erases or writes, so the
 * mapping reads what the flash holds, not what an earlier read left cached.
 *
 * @return KODE_OK with @p out written; KODE_ERROR when a window cannot be mapped or hashed.
 */
static kode_status_t part_sha256(const esp_partition_t *p, uint32_t offset, uint32_t size, uint8_t *out)
{
    psa_hash_operation_t op   = PSA_HASH_OPERATION_INIT;
    uint32_t             done = 0U;
    kode_status_t        st   = KODE_OK;

    (void)psa_crypto_init();   /* idempotent; the hash must not depend on another having run first */
    if ( psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS )
    {
        return KODE_ERROR;
    }
    while ( (KODE_OK == st) && (done < size) )
    {
        uint32_t                    n   = size - done;
        const void                 *map = NULL;
        esp_partition_mmap_handle_t h   = 0U;

        if ( APPS_CRC_WINDOW < n )
        {
            n = APPS_CRC_WINDOW;
        }
        if ( esp_partition_mmap(p, offset + done, n, ESP_PARTITION_MMAP_DATA, &map, &h) != ESP_OK )
        {
            st = KODE_ERROR;
        }
        else
        {
            if ( psa_hash_update(&op, (const uint8_t *)map, n) != PSA_SUCCESS )
            {
                st = KODE_ERROR;
            }
            done += n;
            esp_partition_munmap(h);
        }
    }
    size_t len = 0U;
    if ( (KODE_OK != st) || (psa_hash_finish(&op, out, 32U, &len) != PSA_SUCCESS) || (32U != len) )
    {
        (void)psa_hash_abort(&op);
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t kdrv_apps_named_sha256(const char *project, uint8_t *out)
{
    const kapps_dir_t * const d = kdrv_apps_dir();

    if ( (NULL == s_part) || (NULL == d) || (NULL == project) || (NULL == out) )
    {
        return KODE_DENIED;
    }
    uint32_t count = d->count;

    if ( KAPPS_MAX_ENTRIES < count )
    {
        count = KAPPS_MAX_ENTRIES;
    }
    for ( uint32_t i = 0U; i < count; ++i )
    {
        kdrv_slot_desc_t vd;

        if ( ((d->e[i].flags & KAPPS_ENTRY_VALID) != 0U)
            && (kdrv_apps_desc(&d->e[i], &vd, NULL) == KODE_OK)
            && (strcmp(vd.project, project) == 0)
            && (((uint64_t)d->e[i].offset + (uint64_t)d->e[i].size) <= (uint64_t)s_part->size) )
        {
            return part_sha256(s_part, d->e[i].offset, d->e[i].size, out);
        }
    }
    return KODE_ERROR;
}

/* ---- The region as a kfw_sink_t -------------------------------------------
 * This one needs a context, because where an image lands is the caller's
 * decision: kdrv_apps_plan() runs against a directory the caller is holding,
 * and its answer is what begin() needs.
 */

/**
 * @brief Open a region write session at the context's offset for @p img_len bytes.
 * @param ctx     A kdrv_apps_sink_ctx_t: where kdrv_apps_plan() said to write.
 * @param img_len Decompressed length. 0 is refused: the region cannot place
 *                an image of unknown size.
 * @return KODE_OK; KODE_DENIED with no context; KODE_ERROR for a 0 length;
 *         otherwise kdrv_apps_write_begin()'s answer.
 */
static kode_status_t apps_sink_begin(void *ctx, uint32_t img_len)
{
    const kdrv_apps_sink_ctx_t *c = (const kdrv_apps_sink_ctx_t *)ctx;
    if ( NULL == c )
    {
        return KODE_DENIED;
    }
    if ( 0U == img_len )
    {
        /* The region allocator cannot place an image of unknown size. This is
         * the case a raw .bin over HTTP hits, and the reason an app sent that
         * way needs a container. */
        KLOG_E(TAG, "the region needs a length before it can place an image");
        return KODE_ERROR;
    }
    return kdrv_apps_write_begin(c->offset, img_len);
}

/**
 * @brief Append one chunk to the region, erasing 64 KB blocks as it crosses them.
 * @param ctx  Unused.
 * @param data The chunk.
 * @param len  Its length in bytes.
 * @return KODE_OK, or a reason.
 */
static kode_status_t apps_sink_write(void *ctx, const void *data, size_t len)
{
    (void)ctx;
    return kdrv_apps_write(data, len);
}

/**
 * @brief Close the region's write session and verify the image that was written.
 * @param ctx Unused.
 * @return KODE_OK, or a reason.
 */
static kode_status_t apps_sink_end(void *ctx)
{
    (void)ctx;
    return kdrv_apps_write_end();
}

/**
 * @brief Abandon the region's write session. Whatever was written stays, unverified.
 * @param ctx Unused.
 */
static void apps_sink_abort(void *ctx)
{
    (void)ctx;
    kdrv_apps_write_abort();
}

static const kfw_sink_t APPS_SINK = {
    apps_sink_begin, apps_sink_write, apps_sink_end, apps_sink_abort
};

const kfw_sink_t *kdrv_apps_sink(void)
{
    return &APPS_SINK;
}

kode_status_t kdrv_apps_desc(const kapps_entry_t *e, kdrv_slot_desc_t *out,
                             kapp_desc_t *manifest)
{
    if ( (NULL == s_part) || (NULL == e) || (NULL == out) )
    {
        return KODE_DENIED;
    }

    /* One read, both descriptors: IDF's at KAPP_IDF_DESC_OFF and ours at
     * KAPP_DESC_OFF, both counted from the image's own byte zero - which is
     * why the read below starts at e->offset, not e->offset +
     * KAPP_IDF_DESC_OFF; kapp_desc_parse() indexes KAPP_DESC_OFF from the
     * same origin. Two reads of the same flash page would be two chances
     * to fail for one fact. */
    static uint8_t prefix[KAPP_PREFIX_LEN];
    if ( esp_partition_read(s_part, e->offset, prefix, sizeof(prefix)) != ESP_OK )
    {
        return KODE_ERROR;
    }

    esp_app_desc_t desc;
    (void)memcpy(&desc, &prefix[KAPP_IDF_DESC_OFF], sizeof(desc));
    if ( ESP_APP_DESC_MAGIC_WORD != desc.magic_word )
    {
        return KODE_ERROR;
    }
    kdrv_desc_fill(out, &desc);

    /* Unmanifested is the common case, not an error: say so by zeroing. */
    if ( NULL != manifest )
    {
        if ( kapp_desc_parse(prefix, sizeof(prefix), manifest) != KODE_OK )
        {
            (void)memset(manifest, 0, sizeof(*manifest));
        }
        /* The device's own opinion beats the image's, and beats its
         * SILENCE too: an image that declares nothing still has a project
         * name, so a .bin nobody wrote a manifest for can still be filed.
         * Runs whether or not the parse above succeeded - on failure
         * manifest->category is already empty, which is exactly the
         * unmanifested case an override needs to fill. Resolution is an
         * explicit call rather than a hook inside kapp_desc_parse(),
         * because that parser is pure over bytes and testable without
         * flash - a property worth two call sites. */
        (void)kapp_cat_resolve(out->project, manifest->category,
                               sizeof(manifest->category));
    }
    return KODE_OK;
}

kode_status_t kdrv_apps_verify(uint32_t offset, uint32_t size, uint32_t *img_len)
{
    if ( NULL == s_part )
    {
        return KODE_DENIED;
    }
    /* esp_image_verify() hashes every segment by the length its header
     * declares, and compares the total with `size` only afterwards: a header
     * that declares more than the image holds keeps it reading flash for as
     * long as the lengths say, in the caller's own step. The chain is walked
     * first, and an image whose segments leave its bytes is refused unhashed. */
    if ( (((uint64_t)offset + (uint64_t)size) > (uint64_t)s_part->size)
        || (0U == kdrv_image_walk_len(s_part, offset, size)) )
    {
        KLOG_E(TAG, "verify @0x%x failed (the image does not hold together in its %u bytes)",
               (unsigned)offset, (unsigned)size);
        return KODE_ERROR;
    }
    esp_image_metadata_t meta;
    esp_partition_pos_t const pos = {
        .offset = s_part->address + offset,
        .size   = size,
    };
    esp_err_t const err = esp_image_verify(ESP_IMAGE_VERIFY, &pos, &meta);
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "verify @0x%x failed (%s)", (unsigned)offset, esp_err_to_name(err));
        return KODE_ERROR;
    }
    if ( NULL != img_len )
    {
        *img_len = meta.image_len;
    }
    return KODE_OK;
}

kode_status_t kdrv_apps_build_id(const kapps_entry_t *e, uint8_t *out)
{
    uint8_t head[KAPP_DESC_OFF];

    if ( (NULL == s_part) || (NULL == e) || (NULL == out) )
    {
        return KODE_DENIED;
    }
    if ( esp_partition_read(s_part, e->offset, head, sizeof(head)) != ESP_OK )
    {
        (void)memset(out, 0, KAPP_BUILD_LEN);
        return KODE_ERROR;
    }
    return kapp_build_id(head, sizeof(head), out);
}

kode_status_t kdrv_apps_erase_header(uint32_t offset)
{
    if ( NULL == s_part )
    {
        return KODE_DENIED;
    }
    esp_err_t const err = esp_partition_erase_range(s_part, offset, APPS_SECTOR);
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "erase header @0x%x failed (%s)", (unsigned)offset, esp_err_to_name(err));
        return KODE_ERROR;
    }
    return KODE_OK;
}

/* ---- The boot request: which app the bootloader jumps to next ------------- */

kode_status_t kdrv_apps_req_set(uint32_t offset, uint32_t size, uint32_t state)
{
    if ( NULL == s_part )
    {
        return KODE_DENIED;
    }
    /* Only two words are a request somebody meant to write. TRIED would be
     * writing "already retired", and EMPTY would be writing erased flash with
     * a magic and a valid CRC in front of it - a record the bootloader would
     * have to reason about. Refused here, before the sector is touched. */
    if ( (KAPPS_REQ_STATE_NEW != state) && (KAPPS_REQ_STATE_ALWAYS != state) )
    {
        KLOG_E(TAG, "boot request state 0x%08lx is not a thing; not written",
               (unsigned long)state);
        return KODE_ERROR;
    }
    /* Refuse before writing, rather than leaving the bootloader to refuse
     * after a reset. Both still check - see kapps_req_bounds_ok() - but only
     * this side can say why while there is still a console to say it to. */
    if ( !kapps_req_bounds_ok(offset, size, s_part->size) )
    {
        KLOG_E(TAG, "boot request @0x%06lx +%lu is out of bounds; not written",
               (unsigned long)offset, (unsigned long)size);
        return KODE_ERROR;
    }
    esp_err_t err = esp_partition_erase_range(s_part, KAPPS_REQ_OFF, APPS_SECTOR);
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "req erase failed (%s)", esp_err_to_name(err));
        return KODE_ERROR;
    }

    kapps_req_t req = {
        .magic  = KAPPS_REQ_MAGIC,
        .offset = offset,
        .size   = size,
        .state  = state,
    };
    req.crc = esp_rom_crc32_le(UINT32_MAX, (const uint8_t *)&req, offsetof(kapps_req_t, crc));

    err = esp_partition_write(s_part, KAPPS_REQ_OFF, &req, sizeof(req));
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "req write failed (%s)", esp_err_to_name(err));
        return KODE_ERROR;
    }
    return KODE_OK;
}

/**
 * @brief Read the standing boot request.
 *
 * @param req Where the record goes.
 * @return true when the region is open and the record carries the request
 *         magic and a good CRC; the state is not judged here.
 */
static bool req_read(kapps_req_t *req)
{
    if ( NULL == s_part )
    {
        return false;
    }
    if ( esp_partition_read(s_part, KAPPS_REQ_OFF, req, sizeof(*req)) != ESP_OK )
    {
        return false;
    }
    if ( KAPPS_REQ_MAGIC != req->magic )
    {
        return false;                    /* erased flash: nothing standing */
    }
    uint32_t const crc = esp_rom_crc32_le(UINT32_MAX, (const uint8_t *)req,
                                          offsetof(kapps_req_t, crc));
    return crc == req->crc;
}

/**
 * @brief Is a valid request standing in @p state?
 *
 * @param state  A KAPPS_REQ_STATE_ word.
 * @param offset Receives the request's offset when it does; NULL to skip.
 * @return true when a valid request stands in exactly @p state.
 */
static bool req_in_state(uint32_t state, uint32_t *offset)
{
    kapps_req_t req;
    if ( !req_read(&req) || (req.state != state) )
    {
        return false;
    }
    if ( NULL != offset )
    {
        *offset = req.offset;
    }
    return true;
}

bool kdrv_apps_req_tried(uint32_t *offset)
{
    return req_in_state(KAPPS_REQ_STATE_TRIED, offset);
}

bool kdrv_apps_req_armed(uint32_t *offset)
{
    return req_in_state(KAPPS_REQ_STATE_ARMED, offset);
}

bool kdrv_apps_req_present(void)
{
    kapps_req_t req;
    return req_read(&req) && (KAPPS_REQ_STATE_EMPTY != req.state);
}

void kdrv_apps_req_clear(void)
{
    if ( NULL == s_part )
    {
        return;
    }
    (void)esp_partition_erase_range(s_part, KAPPS_REQ_OFF, APPS_SECTOR);
}

/* ---- app_pending: a fixed mailbox a host tool writes to --------------------
 *
 * Two-phase verification. app_pending sits at 0x1A00000 by design (fixed,
 * >16 MB, "the bootloader will never boot it under any circumstances" - see
 * partitions.csv), and esp_image_verify() calls bootloader_mmap() for every
 * segment. A 24-bit address reaches only the first 16 MB, so with the cache
 * reading in that mode every image at this address failed that call, valid or
 * not: board testing found a complete, hash-verified hello.bin coming back
 * "broken". That is what phase 1 was written for.
 *
 * CONFIG_BOOTLOADER_CACHE_32BIT_ADDR_QUAD_FLASH is now on (sdkconfig.board),
 * so the cache reads with 4-byte-address opcodes and the ceiling is gone.
 * kdrv_apps_pending_verify() now runs that check on the mailbox in place, but
 * only when adoption is about to evict a live app to make room - so a torn
 * upload cannot cost somebody their app - and phase 1 stays either way: it
 * costs a header walk over a partition that is read anyway, and it is what
 * bounds the copy length.
 *
 * Phase 1 walks the image's own header chain by hand through
 * esp_partition_read(), which never had the ceiling - magic, segment count,
 * each segment's declared length, the checksum pad, the optional appended
 * hash - to compute how many bytes to copy. That is a length check, not a
 * cryptographic one: it catches a torn/garbage upload (bad magic, a segment
 * count or length that runs off the end of the partition) but does not
 * validate a hash or a signature.
 *
 * Phase 2 is the real verification: once AppMgr copies those bytes into the
 * apps region, kdrv_apps_write_end() calls kdrv_apps_verify(): the same walk
 * over the bytes written, then esp_image_verify(). Note what the region's
 * growth did to that call - the region now runs from 0xC40000 to 0x1800000
 * and therefore crosses 16 MB, so
 * an image installed in its upper part is verified through the same 32-bit
 * mapping it is executed through. The flag is not optional decoration for
 * this layout; without it, phase 2 fails on exactly the images phase 1 cannot
 * judge. Nothing is ever booted on the strength of the phase-1 walk alone;
 * adopt_finish() only calls boot_app_run_request() after phase 2 has passed.
 */

/**
 * @brief Walk app_pending's image header chain via esp_partition_read (no
 *        mmap, no esp_image_verify - see the section comment above).
 * @return The image length (header + every segment + checksum pad + the
 *         optional appended hash), or 0 if the walk does not hold together:
 *         a bad segment count, or an offset that runs past the partition.
 */
static uint32_t pending_walk_len(const esp_partition_t *p)
{
    /* The image ends where the intent sector begins, not at the end of the
     * partition: an image allowed to run into that sector would be adopted
     * and then have its tail erased by consume(). This is the same ceiling
     * write_begin() enforces, so an esptool push and a cable upload agree. */
    return kdrv_image_walk_len(p, 0U, pending_intent_off(p));
}

kode_status_t kdrv_apps_pending_verify(void)
{
    const esp_partition_t *p = pending_part();
    if ( NULL == p )
    {
        return KODE_ERROR;
    }
    uint8_t first;
    if ( esp_partition_read(p, 0, &first, sizeof(first)) != ESP_OK )
    {
        return KODE_ERROR;
    }
    if ( ESP_IMAGE_HEADER_MAGIC != first )
    {
        /* Not a bare image: a Kode container (unwrapped and verified in the
         * region after the copy) or nothing. This side cannot crypto-verify a
         * container in place without the decoder, so it says OK and leaves the
         * caller's post-copy check to judge it. */
        return KODE_OK;
    }
    uint32_t const len = pending_walk_len(p);
    if ( 0U == len )
    {
        return KODE_ERROR;               /* the headers do not even hold together */
    }
    /* The real phase-2 verification, but on the mailbox in place rather than on
     * the copy. It reaches 0x1A00000 for the same reason phase 2 reaches the
     * region's upper half: the 32-bit cache mapping (see the section comment).
     * A torn data tail or corrupt bytes behind intact headers fail here, before
     * the caller commits any eviction. */
    esp_partition_pos_t const pos = { .offset = p->address, .size = len };
    esp_image_metadata_t meta;
    if ( esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) != ESP_OK )
    {
        return KODE_ERROR;
    }
    return KODE_OK;
}

/**
 * @brief The bytes an app-data block takes in a parked container.
 *
 * The header's payload_len does not count the block: the block states its own
 * length, in the header that follows the container's. A block whose header
 * cannot be read or does not start with its magic counts as none - the decoder
 * refuses that container when the mailbox is adopted, and says why.
 *
 * @param p   The mailbox.
 * @param hdr The container's header, already parsed.
 * @return KFW_AD_HDR_LEN plus the block's two lengths; 0 for no block.
 */
static uint32_t pending_block_len(const esp_partition_t *p, const kfw_hdr_t *hdr)
{
    kfw_ad_hdr_t ad;

    if ( (hdr->flags & KFW_FLAG_APPDATA) == 0U )
    {
        return 0U;
    }
    if ( (esp_partition_read(p, KFW_HDR_LEN, &ad, sizeof(ad)) != ESP_OK)
        || (KFW_AD_MAGIC != ad.magic) )
    {
        return 0U;
    }
    return (uint32_t)KFW_AD_HDR_LEN + (uint32_t)ad.json_len
         + (uint32_t)ad.icon_len;
}

bool kdrv_apps_pending_present(uint32_t *img_len, uint32_t *decoded_len)
{
    const esp_partition_t *p = pending_part();
    if ( NULL == p )
    {
        return false;
    }
    uint8_t first_byte;
    if ( esp_partition_read(p, 0, &first_byte, sizeof(first_byte)) != ESP_OK )
    {
        return false;
    }
    if ( ESP_IMAGE_HEADER_MAGIC != first_byte )
    {
        /* Not a bare image. It may still be a Kode container, which the
         * mailbox has held since it became an `install dst=` destination: its
         * header states its own length, so no walk is needed. Anything else
         * is erased flash or nothing written yet. */
        kfw_hdr_t hdr;
        uint8_t   raw[KFW_HDR_LEN];
        if ( (esp_partition_read(p, 0, raw, sizeof(raw)) == ESP_OK)
            && (kfw_hdr_parse(raw, sizeof(raw), &hdr) == KODE_OK) )
        {
            /* The same ceiling the walk and write_begin() enforce: a header
             * is only a claim, and one whose payload runs into the intent
             * sector - or past the partition, or around 32 bits - describes
             * bytes the mailbox cannot hold. Compared against the room left
             * rather than added up, so a length near UINT32_MAX cannot wrap
             * its way under the limit. Discarded like a broken image, and
             * for the same reason: so it is not re-read every boot. */
            uint32_t const room = pending_intent_off(p) - (uint32_t)KFW_HDR_LEN;
            uint32_t const block = pending_block_len(p, &hdr);
            if ( (hdr.payload_len > room) || ((room - hdr.payload_len) < block) )
            {
                KLOG_W(TAG, "mailbox holds a container claiming %lu payload "
                            "bytes, past the %lu it has room for; discarding it",
                       (unsigned long)hdr.payload_len, (unsigned long)room);
                (void)kdrv_apps_pending_consume();
                return false;
            }
            if ( NULL != img_len )
            {
                *img_len = (uint32_t)KFW_HDR_LEN + block + hdr.payload_len;
            }
            if ( NULL != decoded_len )
            {
                /* What the decoder will place once it has inflated the
                 * payload: the length an eviction plan must be made for. */
                *decoded_len = hdr.image_len;
            }
            return true;
        }
        return false;
    }

    uint32_t const len = pending_walk_len(p);
    if ( 0U == len )
    {
        /* It starts like an image (0xE9) but the header walk does not hold
         * together: garbage, or an upload cut short. Discard it right here
         * so a broken mailbox is not re-checked, and re-logged, on every
         * single boot. */
        KLOG_W(TAG, "mailbox holds a broken image; discarding it");
        (void)kdrv_apps_pending_consume();
        return false;
    }
    if ( NULL != img_len )
    {
        *img_len = len;
    }
    if ( NULL != decoded_len )
    {
        *decoded_len = len;      /* a bare image is its own decoded form */
    }
    return true;
}

kode_status_t kdrv_apps_pending_read(uint32_t offset, void *buf, size_t len)
{
    const esp_partition_t *p = pending_part();
    if ( (NULL == p) || (NULL == buf) )
    {
        return KODE_DENIED;
    }
    if ( esp_partition_read(p, offset, buf, len) != ESP_OK )
    {
        return KODE_ERROR;
    }
    return KODE_OK;
}

/* ---- The mailbox as a destination -----------------------------------------
 * It became one when `install dst=pending` did: parking a download now and
 * installing it later, resuming a bad link, and staging the coprocessor's
 * firmware off the radio that is about to be reflashed all want the same
 * thing, and all three are served by one more sink rather than by a staging
 * step in front of the transfers that need none.
 *
 * What is parked is the container verbatim, header and compression and all,
 * so that reading it back re-verifies rather than trusting a check that
 * happened once, on a different day, over the link that may have been the
 * problem. That is why this sink is the one place the decoder is deliberately
 * NOT in the path.
 */

static bool     s_pwr_open;
static uint32_t s_pwr_written;
static uint32_t s_pwr_size;
static uint32_t s_pwr_erased;

kode_status_t kdrv_apps_pending_write_begin(uint32_t size)
{
    const esp_partition_t *p = pending_part();
    if ( (NULL == p) || s_pwr_open )
    {
        return KODE_DENIED;
    }
    /* The intent sector is the mailbox's last 4 KB and is not image space. */
    uint32_t const usable = pending_intent_off(p);
    if ( (0U == size) || (size > usable) )
    {
        KLOG_E(TAG, "%lu bytes will not fit the mailbox (%lu usable)",
               (unsigned long)size, (unsigned long)usable);
        return KODE_ERROR;
    }
    s_pwr_written = 0U;
    s_pwr_size    = size;
    s_pwr_erased  = 0U;
    s_pwr_open    = true;
    return KODE_OK;
}

kode_status_t kdrv_apps_pending_write(const void *data, size_t len)
{
    const esp_partition_t *p = pending_part();
    if ( !s_pwr_open || (NULL == p) || (NULL == data) )
    {
        return KODE_DENIED;
    }
    if ( ((uint64_t)s_pwr_written + len) > (uint64_t)s_pwr_size )
    {
        return KODE_ERROR;
    }
    /* Erase a sector at a time as the write reaches into it, the same way
     * the region does, so a long erase never holds the cache down in one
     * go. */
    uint32_t const reach = s_pwr_written + (uint32_t)len;
    while ( s_pwr_erased < reach )
    {
        if ( esp_partition_erase_range(p, s_pwr_erased, APPS_SECTOR) != ESP_OK )
        {
            KLOG_E(TAG, "mailbox erase @0x%x failed", (unsigned)s_pwr_erased);
            return KODE_ERROR;
        }
        s_pwr_erased += APPS_SECTOR;
    }
    part_put_ctx_t at = { p, (size_t)s_pwr_written };
    if ( kdrv_bounce_write(&part_put, &at, data, len) != ESP_OK )
    {
        KLOG_E(TAG, "mailbox write @0x%x failed", (unsigned)s_pwr_written);
        return KODE_ERROR;
    }
    s_pwr_written += (uint32_t)len;
    return KODE_OK;
}

kode_status_t kdrv_apps_pending_write_end(void)
{
    if ( !s_pwr_open )
    {
        return KODE_DENIED;
    }
    s_pwr_open = false;
    if ( s_pwr_written != s_pwr_size )
    {
        KLOG_E(TAG, "the mailbox got %lu of the %lu bytes promised",
               (unsigned long)s_pwr_written, (unsigned long)s_pwr_size);
        return KODE_ERROR;
    }
    /* No verification here on purpose: what was parked is a container, and
     * the thing that checks a container is the decoder, when it is read
     * back out. Verifying here would check it once, in the place where a
     * later corruption is not what anybody is worried about. */
    KLOG_I(TAG, "mailbox now holds %lu bytes", (unsigned long)s_pwr_written);
    return KODE_OK;
}

void kdrv_apps_pending_write_abort(void)
{
    s_pwr_open = false;
}

/**
 * @brief Open the mailbox for @p n bytes, container header included.
 * @param ctx Unused.
 * @param n   Bytes the source will deliver. 0, or more than the mailbox's
 *            image space, is refused.
 * @return KODE_OK, or a reason.
 */
static kode_status_t pend_sink_begin(void *ctx, uint32_t n)
{
    (void)ctx;
    return kdrv_apps_pending_write_begin(n);
}
/**
 * @brief Append one chunk to the mailbox, erasing 4 KB sectors as it reaches them.
 * @param ctx Unused.
 * @param d   The chunk.
 * @param n   Its length in bytes.
 * @return KODE_OK, or a reason.
 */
static kode_status_t pend_sink_write(void *ctx, const void *d, size_t n)
{
    (void)ctx;
    return kdrv_apps_pending_write(d, n);
}
/**
 * @brief Close the mailbox session. Checks the byte count, not the content.
 * @param ctx Unused.
 * @return KODE_OK; KODE_DENIED with no session open; KODE_ERROR when fewer
 *         bytes arrived than begin() was told.
 */
static kode_status_t pend_sink_end(void *ctx)
{
    (void)ctx;
    return kdrv_apps_pending_write_end();
}
/**
 * @brief Abandon the mailbox session. What was written stays.
 * @param ctx Unused.
 */
static void pend_sink_abort(void *ctx)
{
    (void)ctx;
    kdrv_apps_pending_write_abort();
}

static const kfw_sink_t PEND_SINK = {
    pend_sink_begin, pend_sink_write, pend_sink_end, pend_sink_abort
};

const kfw_sink_t *kdrv_apps_pending_sink(void)
{
    return &PEND_SINK;
}

bool kdrv_apps_pending_os_intent(void)
{
    const esp_partition_t *p = pending_part();
    if ( NULL == p )
    {
        return false;
    }
    uint32_t word = 0U;
    if ( esp_partition_read(p, pending_intent_off(p), &word, sizeof(word)) != ESP_OK )
    {
        return false;
    }
    return (KAPPS_OS_INTENT_MAGIC == word);
}

kode_status_t kdrv_apps_pending_consume(void)
{
    const esp_partition_t *p = pending_part();
    if ( NULL == p )
    {
        return KODE_DENIED;
    }
    /* Both sectors, every time: the intent flag must never survive an
     * unrelated future mailbox write (an app upload after an OTA one, or
     * the other way round) and outlive the image it was meant to describe. */
    kode_status_t out = KODE_OK;
    if ( esp_partition_erase_range(p, 0, APPS_SECTOR) != ESP_OK )
    {
        out = KODE_ERROR;
    }
    if ( esp_partition_erase_range(p, pending_intent_off(p), APPS_SECTOR) != ESP_OK )
    {
        out = KODE_ERROR;
    }
    return out;
}

/* ---- The storage partition as a destination ---------------------------------
 * `install dst=storage` writes the image as it is into the `storage` partition,
 * the wear-levelled FAT image the factory builds: kode/ and .kode/. The
 * FAT is unmounted for the whole write and mounted again, never formatted,
 * when it ends. What is written is read back and its CRC-32 compared with the
 * CRC-32 of what was received.
 */

#define STO_LABEL        "storage"   /**< The partition's label in the table. */
#define STO_VERIFY_READ  1024U       /**< Bytes read back at a time.          */

static const esp_partition_t *s_sto_part;      /* the partition while a session is open, else NULL */
static uint32_t               s_sto_size;      /* bytes the session was told to expect             */
static uint32_t               s_sto_written;   /* bytes written so far                             */
static uint32_t               s_sto_erased;    /* partition offset erased or found blank up to     */
static uint32_t               s_sto_crc;       /* CRC-32 of the bytes received                     */

/**
 * @brief Take the storage partition: refuse an image that cannot fit, then unmount the FAT.
 * @param ctx Unused.
 * @param n   Bytes the source will deliver. 0, or more than the partition
 *            holds, is refused.
 * @return KODE_OK; KODE_DENIED with a session open or a file of the volume
 *         open; KODE_ERROR for no partition or a size that does not fit.
 */
static kode_status_t sto_sink_begin(void *ctx, uint32_t n)
{
    (void)ctx;
    if ( NULL != s_sto_part )
    {
        return KODE_DENIED;
    }
    const esp_partition_t * const p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, STO_LABEL);
    if ( NULL == p )
    {
        KLOG_E(TAG, "no '%s' partition in the partition table", STO_LABEL);
        return KODE_ERROR;
    }
    if ( (0U == n) || (n > (uint32_t)p->size) )
    {
        KLOG_E(TAG, "%lu bytes do not fit '%s' (%lu bytes)",
               (unsigned long)n, STO_LABEL, (unsigned long)p->size);
        return KODE_ERROR;
    }
    if ( kdrv_storage_fs_files_open() )
    {
        KLOG_E(TAG, "a file of the internal volume is open; '%s' is not written", STO_LABEL);
        return KODE_DENIED;
    }
    if ( kdrv_fs_raw_begin() != KODE_OK )
    {
        KLOG_E(TAG, "the internal volume cannot be taken for a raw write");
        return KODE_DENIED;
    }
    s_sto_size    = n;
    s_sto_written = 0U;
    s_sto_erased  = 0U;
    s_sto_crc     = KHAL_CRC32_INIT;
    s_sto_part    = p;
    KLOG_N(TAG, "writing %lu bytes into '%s'", (unsigned long)n, STO_LABEL);
    return KODE_OK;
}

/**
 * @brief Append one chunk, erasing each 64 KB block as the write reaches it unless it is blank.
 * @param ctx  Unused.
 * @param data The chunk.
 * @param len  Its length in bytes.
 * @return KODE_OK; KODE_DENIED with no session open; KODE_ERROR past the
 *         announced size or when the flash refuses.
 */
static kode_status_t sto_sink_write(void *ctx, const void *data, size_t len)
{
    (void)ctx;
    const esp_partition_t * const p = s_sto_part;
    if ( (NULL == p) || (NULL == data) )
    {
        return KODE_DENIED;
    }
    if ( ((uint64_t)s_sto_written + len) > (uint64_t)s_sto_size )
    {
        return KODE_ERROR;
    }
    uint32_t const reach = s_sto_written + (uint32_t)len;
    while ( s_sto_erased < reach )
    {
        if ( !part_block_blank(p, s_sto_erased) )
        {
            esp_err_t const err = esp_partition_erase_range(p, s_sto_erased, KAPPS_ALIGN);
            if ( ESP_OK != err )
            {
                KLOG_E(TAG, "'%s' erase @0x%x failed (%s)", STO_LABEL,
                       (unsigned)s_sto_erased, esp_err_to_name(err));
                return KODE_ERROR;
            }
        }
        s_sto_erased += KAPPS_ALIGN;
    }
    if ( put_skip_blank(p, (size_t)s_sto_written, data, len) != ESP_OK )
    {
        KLOG_E(TAG, "'%s' write @0x%x failed", STO_LABEL, (unsigned)s_sto_written);
        return KODE_ERROR;
    }
    s_sto_crc      = khal_crc32(s_sto_crc, data, len);
    s_sto_written += (uint32_t)len;
    return KODE_OK;
}

/**
 * @brief Read what was written back, compare its CRC-32, and mount the FAT again.
 * @param ctx Unused.
 * @return KODE_OK when the bytes verify and the volume mounts; KODE_DENIED
 *         with no session open; KODE_ERROR for a short count, a CRC that
 *         differs or a volume that does not mount (it is left unmounted).
 */
static kode_status_t sto_sink_end(void *ctx)
{
    static uint8_t s_buf[STO_VERIFY_READ];

    (void)ctx;
    const esp_partition_t * const p = s_sto_part;
    if ( NULL == p )
    {
        return KODE_DENIED;
    }
    s_sto_part = NULL;

    kode_status_t out = KODE_OK;
    if ( s_sto_written != s_sto_size )
    {
        KLOG_E(TAG, "'%s' got %lu of the %lu bytes promised", STO_LABEL,
               (unsigned long)s_sto_written, (unsigned long)s_sto_size);
        out = KODE_ERROR;
    }
    else
    {
        uint32_t crc = KHAL_CRC32_INIT;
        for ( uint32_t at = 0U; (KODE_OK == out) && (at < s_sto_size); at += STO_VERIFY_READ )
        {
            uint32_t n = s_sto_size - at;
            if ( STO_VERIFY_READ < n )
            {
                n = STO_VERIFY_READ;
            }
            if ( esp_partition_read(p, at, s_buf, n) != ESP_OK )
            {
                KLOG_E(TAG, "'%s' read back @0x%x failed", STO_LABEL, (unsigned)at);
                out = KODE_ERROR;
            }
            else
            {
                crc = khal_crc32(crc, s_buf, n);
            }
        }
        if ( (KODE_OK == out) && (crc != s_sto_crc) )
        {
            KLOG_E(TAG, "'%s' reads back crc 0x%08lx, wrote 0x%08lx", STO_LABEL,
                   (unsigned long)crc, (unsigned long)s_sto_crc);
            out = KODE_ERROR;
        }
    }
    if ( kdrv_fs_raw_end() != KODE_OK )
    {
        out = KODE_ERROR;
    }
    if ( KODE_OK == out )
    {
        KLOG_N(TAG, "'%s' holds %lu bytes (crc 0x%08lx) and is mounted", STO_LABEL,
               (unsigned long)s_sto_size, (unsigned long)s_sto_crc);
    }
    return out;
}

/**
 * @brief Abandon the session: the FAT is mounted again if it will mount, as it is. What was written stays.
 * @param ctx Unused.
 */
static void sto_sink_abort(void *ctx)
{
    (void)ctx;
    if ( NULL != s_sto_part )
    {
        s_sto_part = NULL;
        (void)kdrv_fs_raw_end();
    }
}

static const kfw_sink_t STO_SINK = {
    sto_sink_begin, sto_sink_write, sto_sink_end, sto_sink_abort
};

kode_status_t kdrv_apps_storage_sha256(uint8_t *out)
{
    if ( NULL == out )
    {
        return KODE_DENIED;
    }
    const esp_partition_t * const p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, STO_LABEL);
    if ( (NULL == p) || part_block_blank(p, 0U) )
    {
        /* A partition whose first block is blank holds no FAT image: a Kode
         * fresh from the line answers at once instead of hashing 2 MB of 0xFF. */
        return KODE_ERROR;
    }
    return part_sha256(p, 0U, (uint32_t)p->size, out);
}

const kfw_sink_t *kdrv_apps_storage_sink(void)
{
    return &STO_SINK;
}
