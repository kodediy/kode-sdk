/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_apps_plan.c
 * @brief The apps region's allocator: where an image lands, and what it costs.
 */
#include <string.h>

#include "unity.h"

#include "kapps_fmt.h"
#include "kdrv_apps.h"

/** @brief The real `apps` region, from kbsp/partitions.csv. */
#define REGION_REAL   0xBC0000U

/** @brief The 14080 KB region of the boards the reinstall cases come from. */
#define REGION_14080  0xDC0000U

/**
 * @brief Start a directory with @p count live entries and nothing else.
 * @param d     Receives the directory.
 * @param count How many entries to mark live; their fields are left zero.
 */
static void dir_init(kapps_dir_t *d, uint32_t count)
{
    (void)memset(d, 0, sizeof(*d));
    d->magic   = KAPPS_DIR_MAGIC;
    d->version = KAPPS_FMT_VERSION;
    d->gen     = 1U;
    d->count   = count;
}

/**
 * @brief Fill entry @p i.
 * @param d      The directory.
 * @param i      Entry index.
 * @param off    Region-relative offset.
 * @param size   Image size in bytes.
 * @param seq    Install order; the FIFO reads this.
 * @param valid  Whether to set KAPPS_ENTRY_VALID.
 */
static void dir_entry(kapps_dir_t *d, uint32_t i, uint32_t off, uint32_t size,
                      uint32_t seq, bool valid)
{
    d->e[i].offset = off;
    d->e[i].size   = size;
    d->e[i].seq    = seq;
    d->e[i].flags  = valid ? KAPPS_ENTRY_VALID : 0U;
}

/* ---- The empty region ---------------------------------------------------- */

TEST_CASE("an empty region places the first image at the data offset",
          "[kdrv_apps_plan]")
{
    kapps_dir_t  d;
    kapps_plan_t p;

    dir_init(&d, 0U);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_apps_plan(&d, REGION_REAL, 0x8000U, KAPPS_NO_VICTIM, &p));
    TEST_ASSERT_EQUAL_HEX32(KAPPS_DATA_OFF, p.offset);
    TEST_ASSERT_EQUAL_UINT32(0U, p.evict_count);
}

/* Images must start on a 64KB MMU page or the bootloader cannot map them, so
 * a request that is not a multiple of the alignment reserves the whole page. */
TEST_CASE("an unaligned image size is rounded up to a whole page",
          "[kdrv_apps_plan]")
{
    kapps_dir_t  d;
    kapps_plan_t p;

    dir_init(&d, 1U);
    dir_entry(&d, 0U, KAPPS_DATA_OFF, 0x0001U, 1U, true);   /* one byte */

    /* One byte occupies a whole page, so the next image cannot start before
     * the page after it. */
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_apps_plan(&d, REGION_REAL, 0x10000U, KAPPS_NO_VICTIM, &p));
    TEST_ASSERT_EQUAL_HEX32(KAPPS_DATA_OFF + KAPPS_ALIGN, p.offset);
}

TEST_CASE("every answer is page-aligned and inside the data area",
          "[kdrv_apps_plan]")
{
    kapps_dir_t  d;
    kapps_plan_t p;

    for ( uint32_t size = 1U; (3U * KAPPS_ALIGN) >= size; size += 0x1234U )
    {
        dir_init(&d, 1U);
        dir_entry(&d, 0U, KAPPS_DATA_OFF, 0x20000U, 1U, true);

        TEST_ASSERT_EQUAL_INT(KODE_OK,
            kdrv_apps_plan(&d, REGION_REAL, size, KAPPS_NO_VICTIM, &p));
        TEST_ASSERT_EQUAL_HEX32(0U, p.offset % KAPPS_ALIGN);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KAPPS_DATA_OFF, p.offset);
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(REGION_REAL, p.offset + size);
    }
}

/* ---- Placing after what is already there --------------------------------- */

TEST_CASE("the next image goes after the newest install, not the last slot",
          "[kdrv_apps_plan]")
{
    kapps_dir_t  d;
    kapps_plan_t p;

    dir_init(&d, 2U);
    dir_entry(&d, 0U, 0x10000U, 0x10000U, 1U, true);
    dir_entry(&d, 1U, 0x20000U, 0x8000U,  2U, true);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_apps_plan(&d, 0x90000U, 0x10000U, KAPPS_NO_VICTIM, &p));
    TEST_ASSERT_EQUAL_HEX32(0x30000U, p.offset);
    TEST_ASSERT_EQUAL_UINT32(0U, p.evict_count);
}

/* The newest install is found by seq, not by position in the directory nor by
 * address. A directory whose entries are out of address order is normal - a
 * FIFO eviction reuses whichever extent came free. */
TEST_CASE("the newest install is decided by seq, not by directory order",
          "[kdrv_apps_plan]")
{
    kapps_dir_t  d;
    kapps_plan_t p;

    /* Entry 0 sits at a HIGHER address but was installed FIRST. */
    dir_init(&d, 2U);
    dir_entry(&d, 0U, 0x40000U, 0x10000U, 1U, true);
    dir_entry(&d, 1U, 0x10000U, 0x10000U, 9U, true);

    /* The search starts after the newest (0x10000+0x10000 = 0x20000), so the
     * gap between the two entries is used before the space above them. */
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_apps_plan(&d, 0x90000U, 0x10000U, KAPPS_NO_VICTIM, &p));
    TEST_ASSERT_EQUAL_HEX32(0x20000U, p.offset);
    TEST_ASSERT_EQUAL_UINT32(0U, p.evict_count);
}

/* An entry without KAPPS_ENTRY_VALID is a hole, not an app. Its extent is
 * free space and it must never be counted as something to evict. */
TEST_CASE("an entry that is not valid is free space, not an app",
          "[kdrv_apps_plan]")
{
    kapps_dir_t  d;
    kapps_plan_t p;

    dir_init(&d, 2U);
    dir_entry(&d, 0U, 0x10000U, 0x10000U, 1U, false);   /* a hole */
    dir_entry(&d, 1U, 0x20000U, 0x10000U, 2U, true);

    /* Only one live entry, ending at 0x30000; the hole below is reusable. */
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_apps_plan(&d, 0x40000U, 0x10000U, KAPPS_NO_VICTIM, &p));
    TEST_ASSERT_EQUAL_HEX32(0x30000U, p.offset);
    TEST_ASSERT_EQUAL_UINT32(0U, p.evict_count);
}

/* The gap search wraps: when nothing above the newest install fits, it comes
 * back around to the bottom of the data area rather than giving up. */
TEST_CASE("the search wraps to the bottom when the top is full",
          "[kdrv_apps_plan]")
{
    kapps_dir_t  d;
    kapps_plan_t p;

    /* Newest sits at the top with no room above it; a gap remains below. */
    dir_init(&d, 2U);
    dir_entry(&d, 0U, 0x30000U, 0x10000U, 5U, true);   /* newest, at the top */
    dir_entry(&d, 1U, 0x20000U, 0x10000U, 1U, true);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_apps_plan(&d, 0x40000U, 0x10000U, KAPPS_NO_VICTIM, &p));
    TEST_ASSERT_EQUAL_HEX32(0x10000U, p.offset);   /* wrapped to the bottom */
    TEST_ASSERT_EQUAL_UINT32(0U, p.evict_count);
}

/* ---- Eviction ------------------------------------------------------------ */

TEST_CASE("a full region evicts the oldest install and reuses its extent",
          "[kdrv_apps_plan]")
{
    kapps_dir_t  d;
    kapps_plan_t p;

    dir_init(&d, 3U);
    dir_entry(&d, 0U, 0x10000U, 0x10000U, 1U, true);   /* oldest */
    dir_entry(&d, 1U, 0x20000U, 0x8000U,  2U, true);
    dir_entry(&d, 2U, 0x30000U, 0x60000U, 3U, true);   /* fills to the end */

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_apps_plan(&d, 0x90000U, 0x10000U, KAPPS_NO_VICTIM, &p));
    TEST_ASSERT_EQUAL_HEX32(0x10000U, p.offset);
    TEST_ASSERT_EQUAL_UINT32(1U, p.evict_count);
    TEST_ASSERT_EQUAL_UINT8(0U, p.evict[0]);
}

/* Eviction is a loop, and the indices it reports are DIRECTORY indices. The
 * loop compacts its working set by swapping the evicted slot with the last
 * one, so an implementation that reported working-set positions instead would
 * pass the single-eviction case above and name the wrong app here. */
TEST_CASE("two evictions are reported oldest-first, by directory index",
          "[kdrv_apps_plan]")
{
    kapps_dir_t  d;
    kapps_plan_t p;

    dir_init(&d, 3U);
    dir_entry(&d, 0U, 0x10000U, 0x10000U, 1U, true);   /* oldest      */
    dir_entry(&d, 1U, 0x20000U, 0x10000U, 2U, true);   /* next oldest */
    dir_entry(&d, 2U, 0x30000U, 0x20000U, 3U, true);   /* newest      */

    /* 0x20000 needs two adjacent pages, and only evicting both of the older
     * entries opens one. */
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_apps_plan(&d, 0x50000U, 0x20000U, KAPPS_NO_VICTIM, &p));
    TEST_ASSERT_EQUAL_HEX32(0x10000U, p.offset);
    TEST_ASSERT_EQUAL_UINT32(2U, p.evict_count);
    TEST_ASSERT_EQUAL_UINT8(0U, p.evict[0]);
    TEST_ASSERT_EQUAL_UINT8(1U, p.evict[1]);
}

/* ---- Refusals ------------------------------------------------------------ */

TEST_CASE("an image too big for the whole region is refused before evicting",
          "[kdrv_apps_plan]")
{
    kapps_dir_t  d;
    kapps_plan_t p;

    dir_init(&d, 3U);
    dir_entry(&d, 0U, 0x10000U, 0x10000U, 1U, true);
    dir_entry(&d, 1U, 0x20000U, 0x8000U,  2U, true);
    dir_entry(&d, 2U, 0x30000U, 0x60000U, 3U, true);

    p.evict_count = 0xFFFFFFFFU;
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_apps_plan(&d, 0x90000U, 0x100000U, KAPPS_NO_VICTIM, &p));

    /* Nothing was planned, so nothing may be thrown away: a refusal that had
     * already filled in evictions would cost apps for an install that never
     * happens. */
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFU, p.evict_count);
}

TEST_CASE("an image of no bytes is refused", "[kdrv_apps_plan]")
{
    kapps_dir_t  d;
    kapps_plan_t p;

    dir_init(&d, 0U);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_apps_plan(&d, REGION_REAL, 0U, KAPPS_NO_VICTIM, &p));
}

TEST_CASE("a region with no data area is refused", "[kdrv_apps_plan]")
{
    kapps_dir_t  d;
    kapps_plan_t p;

    dir_init(&d, 0U);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_apps_plan(&d, KAPPS_DATA_OFF, 0x1000U, KAPPS_NO_VICTIM, &p));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_apps_plan(&d, 0U, 0x1000U, KAPPS_NO_VICTIM, &p));
}

TEST_CASE("a null directory or plan is refused rather than dereferenced",
          "[kdrv_apps_plan]")
{
    kapps_dir_t  d;
    kapps_plan_t p;

    dir_init(&d, 0U);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_apps_plan(NULL, REGION_REAL, 0x1000U, KAPPS_NO_VICTIM, &p));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_apps_plan(&d, REGION_REAL, 0x1000U, KAPPS_NO_VICTIM, NULL));
}

/* A directory that claims more entries than the format allows is corrupt.
 * Reading e[] past its end would walk off the struct, so the count is clamped
 * rather than trusted. */
TEST_CASE("a count past the format's maximum is clamped, not trusted",
          "[kdrv_apps_plan]")
{
    kapps_dir_t  d;
    kapps_plan_t p;

    dir_init(&d, KAPPS_MAX_ENTRIES + 32U);
    for ( uint32_t i = 0U; KAPPS_MAX_ENTRIES > i; ++i )
    {
        dir_entry(&d, i, 0U, 0U, 0U, false);   /* all holes */
    }

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_apps_plan(&d, REGION_REAL, 0x10000U, KAPPS_NO_VICTIM, &p));
    TEST_ASSERT_EQUAL_HEX32(KAPPS_DATA_OFF, p.offset);
}

/* ---- At the real scale --------------------------------------------------- */

TEST_CASE("an image filling the real region exactly still fits",
          "[kdrv_apps_plan]")
{
    kapps_dir_t  d;
    kapps_plan_t p;

    dir_init(&d, 0U);

    /* The whole data area, to the byte. */
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_apps_plan(&d, REGION_REAL, REGION_REAL - KAPPS_DATA_OFF,
                       KAPPS_NO_VICTIM, &p));
    TEST_ASSERT_EQUAL_HEX32(KAPPS_DATA_OFF, p.offset);

    /* One page more than the data area does not. */
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_apps_plan(&d, REGION_REAL,
                       (REGION_REAL - KAPPS_DATA_OFF) + 1U, KAPPS_NO_VICTIM, &p));
}

/* ---- The entry the directory's own limit would take ---------------------- */

/* A directory with room for one more entry names no slot victim: the plan
 * alone decides who goes. */
TEST_CASE("a directory with a free entry names no slot victim",
          "[kdrv_apps_plan]")
{
    kapps_dir_t  d;
    kapps_plan_t p;

    dir_init(&d, 1U);
    dir_entry(&d, 0U, KAPPS_DATA_OFF, 0x10000U, 1U, true);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_apps_plan(&d, REGION_REAL, 0x10000U, KAPPS_NO_VICTIM, &p));
    TEST_ASSERT_EQUAL_HEX8(KAPPS_NO_VICTIM, p.slot_victim);
}

/* Sixty-four small apps fill the directory long before the bytes run out.
 * The byte planner evicts nobody, and the entry appdir_make_room() will take
 * after the write is the oldest: the copy has to know it now. */
TEST_CASE("a full directory names its oldest entry as the slot victim",
          "[kdrv_apps_plan]")
{
    kapps_dir_t  d;
    kapps_plan_t p;

    dir_init(&d, KAPPS_MAX_ENTRIES);
    for ( uint32_t i = 0U; KAPPS_MAX_ENTRIES > i; ++i )
    {
        /* seq runs backwards so the oldest is not simply index 0 */
        dir_entry(&d, i, KAPPS_DATA_OFF + (i * KAPPS_ALIGN), 0x1000U,
                  KAPPS_MAX_ENTRIES - i, true);
    }
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_apps_plan(&d, REGION_REAL, 0x1000U, KAPPS_NO_VICTIM, &p));
    TEST_ASSERT_EQUAL_UINT32(0U, p.evict_count);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)(KAPPS_MAX_ENTRIES - 1U), p.slot_victim);
}

/* When the byte planner already evicts, the directory has room again and the
 * slot victim is none: one eviction is never counted twice. */
TEST_CASE("a plan that evicts for bytes leaves no slot victim",
          "[kdrv_apps_plan]")
{
    kapps_dir_t  d;
    kapps_plan_t p;

    dir_init(&d, 1U);
    dir_entry(&d, 0U, KAPPS_DATA_OFF, REGION_REAL - KAPPS_DATA_OFF, 1U, true);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_apps_plan(&d, REGION_REAL, 0x10000U, KAPPS_NO_VICTIM, &p));
    TEST_ASSERT_EQUAL_UINT32(1U, p.evict_count);
    TEST_ASSERT_EQUAL_HEX8(KAPPS_NO_VICTIM, p.slot_victim);
}

/* ---- A new version of an installed app ----------------------------------- */

/* The case that lost an app on a DVT board: an app reinstalled at a bigger
 * size, whose last install sits at the region's start. Its old copy is the
 * newest entry, so a plain FIFO evicts the one other app instead; the old
 * copy is what goes, and nothing else does. */
TEST_CASE("a reinstall evicts its own old copy, not an older app",
          "[kdrv_apps_plan]")
{
    kapps_dir_t  d;
    kapps_plan_t p;

    dir_init(&d, 2U);
    dir_entry(&d, 0U, KAPPS_DATA_OFF, 0x590000U, 5U, true);   /* ko2_dj */
    dir_entry(&d, 1U, 0x6A0000U,      0x390000U, 3U, true);   /* mando_ir */

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_apps_plan(&d, REGION_14080, 0x590000U, KAPPS_NO_VICTIM, &p));
    TEST_ASSERT_EQUAL_UINT32(1U, p.evict_count);
    TEST_ASSERT_EQUAL_UINT8(1U, p.evict[0]);
    TEST_ASSERT_EQUAL_HEX8(KAPPS_NO_VICTIM, p.replaced);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_apps_plan(&d, REGION_14080, 0x590000U, 0U, &p));
    TEST_ASSERT_EQUAL_UINT32(1U, p.evict_count);
    TEST_ASSERT_EQUAL_UINT8(0U, p.evict[0]);
    TEST_ASSERT_EQUAL_HEX8(0U, p.replaced);
    TEST_ASSERT_EQUAL_HEX32(KAPPS_DATA_OFF, p.offset);
}

/* Freeing the old copy first only ever makes gaps bigger, so the other apps
 * it costs are never more than the FIFO's. Here they are the same two; the
 * old copy goes first and is reported as replaced, not lost. */
TEST_CASE("a reinstall costs no other app more than the FIFO would",
          "[kdrv_apps_plan]")
{
    kapps_dir_t  d;
    kapps_plan_t p;

    dir_init(&d, 4U);
    dir_entry(&d, 0U, KAPPS_DATA_OFF, 0x390000U, 1U, true);   /* subghz */
    dir_entry(&d, 1U, 0x3A0000U,      0x3B0000U, 2U, true);   /* kody */
    dir_entry(&d, 2U, 0x750000U,      0x390000U, 3U, true);   /* mando_ir */
    dir_entry(&d, 3U, 0xAE0000U,      0x190000U, 4U, true);   /* ko2_dj */

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_apps_plan(&d, REGION_14080, 0x590000U, KAPPS_NO_VICTIM, &p));
    TEST_ASSERT_EQUAL_UINT32(2U, p.evict_count);
    TEST_ASSERT_EQUAL_UINT8(0U, p.evict[0]);
    TEST_ASSERT_EQUAL_UINT8(1U, p.evict[1]);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_apps_plan(&d, REGION_14080, 0x590000U, 3U, &p));
    TEST_ASSERT_EQUAL_UINT32(3U, p.evict_count);
    TEST_ASSERT_EQUAL_UINT8(3U, p.evict[0]);
    TEST_ASSERT_EQUAL_UINT8(0U, p.evict[1]);
    TEST_ASSERT_EQUAL_UINT8(1U, p.evict[2]);
    TEST_ASSERT_EQUAL_HEX8(3U, p.replaced);
    TEST_ASSERT_EQUAL_HEX32(KAPPS_DATA_OFF, p.offset);
}

/* With room to spare, the app being replaced stays until the write is done:
 * a failed install must not cost the copy that still works. */
TEST_CASE("a reinstall with room evicts nothing, its old copy included",
          "[kdrv_apps_plan]")
{
    kapps_dir_t  d;
    kapps_plan_t p;

    dir_init(&d, 1U);
    dir_entry(&d, 0U, KAPPS_DATA_OFF, 0x100000U, 1U, true);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_apps_plan(&d, REGION_14080, 0x100000U, 0U, &p));
    TEST_ASSERT_EQUAL_UINT32(0U, p.evict_count);
    TEST_ASSERT_EQUAL_HEX8(KAPPS_NO_VICTIM, p.replaced);
    TEST_ASSERT_EQUAL_HEX32(0x110000U, p.offset);
}

/* A full directory and a new version of one of its apps: the namesake frees
 * its own entry after the write, so nobody else is named for the slot. */
TEST_CASE("a reinstall into a full directory names no slot victim",
          "[kdrv_apps_plan]")
{
    kapps_dir_t  d;
    kapps_plan_t p;

    dir_init(&d, KAPPS_MAX_ENTRIES);
    for ( uint32_t i = 0U; KAPPS_MAX_ENTRIES > i; ++i )
    {
        dir_entry(&d, i, KAPPS_DATA_OFF + (i * KAPPS_ALIGN), 0x1000U,
                  KAPPS_MAX_ENTRIES - i, true);
    }
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_apps_plan(&d, REGION_REAL, 0x1000U, 5U, &p));
    TEST_ASSERT_EQUAL_UINT32(0U, p.evict_count);
    TEST_ASSERT_EQUAL_HEX8(KAPPS_NO_VICTIM, p.slot_victim);
}
