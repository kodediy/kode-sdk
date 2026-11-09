/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_storage_list.c
 * @brief Listing a medium, and the once-per-boot rule under it.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "fakes.h"
#include "kdrv_storage.h"

/** @brief One row the callback saw. */
typedef struct {
    char     name[64];
    uint32_t size;
} seen_t;

typedef struct {
    seen_t   rows[16];
    unsigned count;
    unsigned stop_after;   /**< 0 = never stop. */
} walk_t;

/**
 * @brief Records an entry, and stops the walk when the case asked it to.
 * @param name Entry name as the directory gave it.
 * @param size Its length in bytes.
 * @param ctx  The walk_t.
 * @return false to end the walk.
 */
static bool record(const char *name, uint32_t size, void *ctx)
{
    walk_t * const w = (walk_t *)ctx;

    if ( 16U > w->count )
    {
        (void)snprintf(w->rows[w->count].name, sizeof(w->rows[0].name), "%s",
                       name);
        w->rows[w->count].size = size;
    }
    ++w->count;

    return (0U == w->stop_after) || (w->count < w->stop_after);
}

/** @brief Put @p bytes of content in @p name on the given directory. */
static void make_file(const char *dir, const char *name, size_t bytes)
{
    char  path[512];
    FILE *f;

    (void)snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    for ( size_t i = 0U; i < bytes; ++i )
    {
        (void)fputc('x', f);
    }
    (void)fclose(f);
}

/** @brief Did the walk see @p name? */
static bool walk_saw(const walk_t *w, const char *name)
{
    for ( unsigned i = 0U; (i < w->count) && (16U > i); ++i )
    {
        if ( strcmp(w->rows[i].name, name) == 0 )
        {
            return true;
        }
    }
    return false;
}

/* ---- Walking a directory --------------------------------------------------- */

TEST_CASE("a listing reports every file, with its length", "[kdrv_storage]")
{
    walk_t w = { .count = 0U, .stop_after = 0U };

    make_file(fake_media_sd_dir(), "one.bin", 100U);
    make_file(fake_media_sd_dir(), "two.txt", 7U);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_storage_list(KDRV_MEDIUM_SD, &record, &w));

    /* Two files, plus "." and ".." - see the case below, which is about
     * exactly that. */
    TEST_ASSERT_EQUAL_UINT(4U, w.count);
    TEST_ASSERT_TRUE(walk_saw(&w, "one.bin"));
    TEST_ASSERT_TRUE(walk_saw(&w, "two.txt"));

    for ( unsigned i = 0U; i < w.count; ++i )
    {
        if ( strcmp(w.rows[i].name, "one.bin") == 0 )
        {
            TEST_ASSERT_EQUAL_UINT32(100U, w.rows[i].size);
        }
        if ( strcmp(w.rows[i].name, "two.txt") == 0 )
        {
            TEST_ASSERT_EQUAL_UINT32(7U, w.rows[i].size);
        }
    }
}

TEST_CASE("an empty medium lists no files and is not an error",
          "[kdrv_storage]")
{
    walk_t w = { .count = 0U, .stop_after = 0U };

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_storage_list(KDRV_MEDIUM_FS, &record, &w));

    /* Only the two directory entries every directory has. */
    TEST_ASSERT_EQUAL_UINT(2U, w.count);
    TEST_ASSERT_TRUE(walk_saw(&w, "."));
    TEST_ASSERT_TRUE(walk_saw(&w, ".."));
}

/* The filter is "not a macOS stub", which is narrower than "a file worth
 * showing": `.` and `..` come straight through to the caller. Every caller in
 * the tree filters again by extension, so nothing shows them today - but
 * `sd_ls` lists whatever it is handed, so this is a wart with a short fuse
 * rather than a bug. Pinned here so it is a decision rather than a surprise. */
TEST_CASE("the directory's own two entries reach the caller", "[kdrv_storage]")
{
    walk_t w = { .count = 0U, .stop_after = 0U };

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_storage_list(KDRV_MEDIUM_FS, &record, &w));

    TEST_ASSERT_TRUE(walk_saw(&w, "."));
    TEST_ASSERT_TRUE(walk_saw(&w, ".."));

    /* Their reported size is whatever stat says of a directory, and the
     * caller is expected to be filtering by name rather than by size. */
    TEST_ASSERT_EQUAL_UINT(2U, w.count);
}

TEST_CASE("the two media list separately", "[kdrv_storage]")
{
    walk_t sd = { .count = 0U, .stop_after = 0U };
    walk_t fs = { .count = 0U, .stop_after = 0U };

    make_file(fake_media_sd_dir(), "card.bin", 1U);
    make_file(fake_media_fs_dir(), "internal.bin", 1U);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_storage_list(KDRV_MEDIUM_SD, &record, &sd));
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_storage_list(KDRV_MEDIUM_FS, &record, &fs));

    TEST_ASSERT_TRUE(walk_saw(&sd, "card.bin"));
    TEST_ASSERT_FALSE(walk_saw(&sd, "internal.bin"));
    TEST_ASSERT_TRUE(walk_saw(&fs, "internal.bin"));
    TEST_ASSERT_FALSE(walk_saw(&fs, "card.bin"));
}

/* ---- The AppleDouble filter ------------------------------------------------ */

/* macOS scatters "._name" stubs over a FAT card whenever it copies a file.
 * They carry the right extension and are a few KB of metadata, so without
 * this filter every app on a card copied from a Mac would appear twice - once
 * real, once as something that cannot possibly run. */
TEST_CASE("macOS metadata stubs are filtered out of a listing",
          "[kdrv_storage]")
{
    walk_t w = { .count = 0U, .stop_after = 0U };

    make_file(fake_media_sd_dir(), "app.bin", 10U);
    make_file(fake_media_sd_dir(), "._app.bin", 4096U);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_storage_list(KDRV_MEDIUM_SD, &record, &w));

    /* The stub is gone; the real file and the two directory entries remain. */
    TEST_ASSERT_EQUAL_UINT(3U, w.count);
    TEST_ASSERT_TRUE(walk_saw(&w, "app.bin"));
    TEST_ASSERT_FALSE(walk_saw(&w, "._app.bin"));
}

/* The filter is exactly "starts with dot-underscore" and nothing wider. A
 * dotfile is not a Mac stub, and hiding it would be a listing that lies. */
TEST_CASE("the filter catches dot-underscore and nothing else",
          "[kdrv_storage]")
{
    walk_t w = { .count = 0U, .stop_after = 0U };

    make_file(fake_media_sd_dir(), "._hidden", 1U);
    make_file(fake_media_sd_dir(), ".hidden", 1U);
    make_file(fake_media_sd_dir(), "_leading", 1U);
    make_file(fake_media_sd_dir(), "a._middle", 1U);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_storage_list(KDRV_MEDIUM_SD, &record, &w));

    TEST_ASSERT_FALSE(walk_saw(&w, "._hidden"));
    TEST_ASSERT_TRUE(walk_saw(&w, ".hidden"));
    TEST_ASSERT_TRUE(walk_saw(&w, "_leading"));
    TEST_ASSERT_TRUE(walk_saw(&w, "a._middle"));
}

/* ---- Stopping early -------------------------------------------------------- */

/* The callback's return is what caps a listing at KODE_STORAGE_LIST_MAX, so a
 * card with two hundred files cannot overrun a subscriber's queue. Entries
 * after the stop must never be delivered. */
TEST_CASE("a callback that says stop ends the walk", "[kdrv_storage]")
{
    walk_t w = { .count = 0U, .stop_after = 2U };

    make_file(fake_media_sd_dir(), "a.bin", 1U);
    make_file(fake_media_sd_dir(), "b.bin", 1U);
    make_file(fake_media_sd_dir(), "c.bin", 1U);
    make_file(fake_media_sd_dir(), "d.bin", 1U);
    make_file(fake_media_sd_dir(), "e.bin", 1U);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_storage_list(KDRV_MEDIUM_SD, &record, &w));

    TEST_ASSERT_EQUAL_UINT(2U, w.count);
}

/* ---- Refusals -------------------------------------------------------------- */

TEST_CASE("a listing with no callback is refused", "[kdrv_storage]")
{
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        kdrv_storage_list(KDRV_MEDIUM_SD, NULL, NULL));
}

TEST_CASE("a medium that is not there cannot be listed", "[kdrv_storage]")
{
    walk_t w = { .count = 0U, .stop_after = 0U };

    fake_media_set_sd_present(false);

    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        kdrv_storage_list(KDRV_MEDIUM_SD, &record, &w));
    TEST_ASSERT_EQUAL_UINT(0U, w.count);
}

/* ---- The probe's latch ----------------------------------------------------- */

/* A card that failed to mount is not asked again for the rest of the boot.
 * Mounting a microSD is slow and a slot with no card in it fails every time,
 * so retrying on every listing would make the whole GUI stutter. The rule is
 * documented at length and is invisible on a bench, because seeing it means
 * having a card that fails once and then works. */
TEST_CASE("a card that failed once is not probed again", "[kdrv_storage]")
{
    fake_media_set_sd_present(false);

    /* The first probe tries, and fails. */
    TEST_ASSERT_NULL(kdrv_storage_probe(KDRV_MEDIUM_SD));

    /* Now the card appears - but nothing asks it, because it already said no. */
    fake_media_set_sd_present(true);
    TEST_ASSERT_NULL(kdrv_storage_probe(KDRV_MEDIUM_SD));
}

/* The console's `sd mount` is the way out of that latch, and it works
 * regardless of what the probe decided earlier. */
TEST_CASE("an explicit mount ignores the latch and tries anyway",
          "[kdrv_storage]")
{
    fake_media_set_sd_present(false);
    TEST_ASSERT_NULL(kdrv_storage_probe(KDRV_MEDIUM_SD));

    fake_media_set_sd_present(true);
    TEST_ASSERT_NOT_NULL(kdrv_storage_mount(KDRV_MEDIUM_SD));

    /* And once it is mounted, the probe answers from the mount rather than
     * from the latch. */
    TEST_ASSERT_NOT_NULL(kdrv_storage_probe(KDRV_MEDIUM_SD));
}

/* The internal partition is never latched: it is always there, and retrying
 * it costs nothing. */
TEST_CASE("the internal partition is always probed", "[kdrv_storage]")
{
    TEST_ASSERT_NOT_NULL(kdrv_storage_probe(KDRV_MEDIUM_FS));
    kdrv_storage_unmount(KDRV_MEDIUM_FS);
    TEST_ASSERT_NOT_NULL(kdrv_storage_probe(KDRV_MEDIUM_FS));
}

TEST_CASE("mounted reports what is mounted, per medium", "[kdrv_storage]")
{
    TEST_ASSERT_NOT_NULL(kdrv_storage_mount(KDRV_MEDIUM_FS));
    TEST_ASSERT_TRUE(kdrv_storage_mounted(KDRV_MEDIUM_FS));
    TEST_ASSERT_NOT_NULL(kdrv_storage_mount_point(KDRV_MEDIUM_FS));

    kdrv_storage_unmount(KDRV_MEDIUM_FS);
    TEST_ASSERT_FALSE(kdrv_storage_mounted(KDRV_MEDIUM_FS));
    TEST_ASSERT_NULL(kdrv_storage_mount_point(KDRV_MEDIUM_FS));
}
