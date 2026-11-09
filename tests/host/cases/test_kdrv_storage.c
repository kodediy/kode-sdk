/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_storage.c
 * @brief The media layer, over two real directories on the host.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "fakes.h"
#include "kdrv_storage.h"

/**
 * @brief Start a case with no file open and two empty media.
 *
 * The close matters as much as the wipe: kdrv_storage holds one handle for
 * the whole system, and Unity abandons a case the moment an assertion fails -
 * so a case that fails before its own close would leave the handle open and
 * every case after it would be refused with "a file is already open",
 * reporting a cascade instead of the one real failure.
 */
static void storage_fresh(void)
{
    kdrv_storage_close();
    fake_media_reset();
}

/**
 * @brief Write a file straight into one of the fake mount points.
 * @param dir  Directory from the media fake.
 * @param name File name.
 * @param body Contents.
 */
static void put_file(const char *dir, const char *name, const char *body)
{
    char path[512];

    (void)snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    (void)fwrite(body, 1U, strlen(body), f);
    (void)fclose(f);
}

/* ---- The bug this file exists for ---------------------------------------- */

/* create → close, then an unrelated open → discard. The discard belongs to
 * nothing: there is no write in flight for it to undo. It must not reach for
 * the file the earlier create happened to name. */
TEST_CASE("discard after an unrelated open leaves the written file alone",
          "[kdrv_storage]")
{
    kdrv_medium_t medium = KDRV_MEDIUM_FS;
    uint32_t      size   = 0U;

    storage_fresh();

    /* Something to open later. */
    put_file(fake_media_fs_dir(), "sleep.bin", "an app");

    /* A capture is written and closed cleanly. */
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_storage_create("shot.bmp", KDRV_MEDIUM_FS, &medium));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_write("BM", 2U));
    kdrv_storage_close();
    TEST_ASSERT_TRUE(fake_media_file_exists(fake_media_fs_dir(), "shot.bmp"));

    /* Later, something else is read, and its reader discards. */
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_storage_open("sleep.bin", &size, &medium));
    kdrv_storage_discard();

    /* The capture is still there. It used to be removed. */
    TEST_ASSERT_TRUE(fake_media_file_exists(fake_media_fs_dir(), "shot.bmp"));
}

/* The other half of the same rule: a discard that *does* belong to an open
 * write must still remove it, or a half-written image survives as a name in
 * a listing that promises bytes it does not have. */
TEST_CASE("discard during a write removes the file it was writing",
          "[kdrv_storage]")
{
    kdrv_medium_t medium = KDRV_MEDIUM_FS;

    storage_fresh();

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_storage_create("half.bin", KDRV_MEDIUM_FS, &medium));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_write("half", 4U));
    kdrv_storage_discard();

    TEST_ASSERT_FALSE(fake_media_file_exists(fake_media_fs_dir(), "half.bin"));
}

TEST_CASE("a discard with nothing open removes nothing", "[kdrv_storage]")
{
    storage_fresh();
    put_file(fake_media_fs_dir(), "keep.bin", "x");

    kdrv_storage_discard();
    kdrv_storage_discard();

    TEST_ASSERT_TRUE(fake_media_file_exists(fake_media_fs_dir(), "keep.bin"));
}

/* ---- Reading ------------------------------------------------------------- */

TEST_CASE("a file is found on the card before the internal partition",
          "[kdrv_storage]")
{
    kdrv_medium_t medium = KDRV_MEDIUM_COUNT;
    uint32_t      size   = 0U;

    storage_fresh();
    put_file(fake_media_sd_dir(), "both.bin", "from the card");
    put_file(fake_media_fs_dir(), "both.bin", "from the partition");

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_storage_open("both.bin", &size, &medium));
    TEST_ASSERT_EQUAL_INT(KDRV_MEDIUM_SD, medium);
    TEST_ASSERT_EQUAL_UINT32(strlen("from the card"), size);
    kdrv_storage_close();
}

TEST_CASE("a file only on the internal partition is still found",
          "[kdrv_storage]")
{
    kdrv_medium_t medium = KDRV_MEDIUM_COUNT;
    uint32_t      size   = 0U;

    storage_fresh();
    put_file(fake_media_fs_dir(), "only.bin", "inside");

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_storage_open("only.bin", &size, &medium));
    TEST_ASSERT_EQUAL_INT(KDRV_MEDIUM_FS, medium);
    kdrv_storage_close();
}

TEST_CASE("a name that is nowhere is an error", "[kdrv_storage]")
{
    kdrv_medium_t medium = KDRV_MEDIUM_FS;
    uint32_t      size   = 0U;

    storage_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_storage_open("nothing.bin", &size, &medium));
}

/* The counterfeit cards make this reachable in the field: a failed write
 * leaves FATFS holding sectors it cannot flush, and every later open on that
 * volume returns length 0. A zero-length file is refused as unusable rather
 * than handed on as an empty image somebody might try to boot. */
TEST_CASE("a zero-length file is refused as unusable, not returned empty",
          "[kdrv_storage]")
{
    kdrv_medium_t medium = KDRV_MEDIUM_FS;
    uint32_t      size   = 0U;

    storage_fresh();
    put_file(fake_media_fs_dir(), "empty.bin", "");

    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        kdrv_storage_open("empty.bin", &size, &medium));

    /* And the handle it opened to find that out was released, so the next
     * open is not refused as "a file is already open". */
    put_file(fake_media_fs_dir(), "real.bin", "bytes");
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_storage_open("real.bin", &size, &medium));
    kdrv_storage_close();
}

TEST_CASE("only one file may be open at a time", "[kdrv_storage]")
{
    kdrv_medium_t medium = KDRV_MEDIUM_FS;
    uint32_t      size   = 0U;

    storage_fresh();
    put_file(fake_media_fs_dir(), "a.bin", "aaaa");
    put_file(fake_media_fs_dir(), "b.bin", "bbbb");

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_open("a.bin", &size, &medium));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        kdrv_storage_open("b.bin", &size, &medium));
    kdrv_storage_close();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_open("b.bin", &size, &medium));
    kdrv_storage_close();
}

TEST_CASE("reading past the end reports no bytes rather than an error",
          "[kdrv_storage]")
{
    kdrv_medium_t medium = KDRV_MEDIUM_FS;
    uint32_t      size   = 0U;
    char          buf[16];
    size_t        got    = 99U;

    storage_fresh();
    put_file(fake_media_fs_dir(), "short.bin", "abc");

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_storage_open("short.bin", &size, &medium));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_read(buf, sizeof(buf), &got));
    TEST_ASSERT_EQUAL_UINT(3U, got);

    /* The documented end-of-file signal is a successful read of nothing. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_read(buf, sizeof(buf), &got));
    TEST_ASSERT_EQUAL_UINT(0U, got);

    kdrv_storage_close();
}

/* ---- Writing ------------------------------------------------------------- */

/* kodeOS writes to the internal partition first on purpose: one failed write
 * to a counterfeit card poisons every later read of that volume for the whole
 * session, so the preference is not a preference about speed. */
TEST_CASE("a create honours the medium it was asked for first",
          "[kdrv_storage]")
{
    kdrv_medium_t medium = KDRV_MEDIUM_COUNT;

    storage_fresh();

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_storage_create("out.bin", KDRV_MEDIUM_FS, &medium));
    TEST_ASSERT_EQUAL_INT(KDRV_MEDIUM_FS, medium);
    kdrv_storage_close();
    TEST_ASSERT_TRUE(fake_media_file_exists(fake_media_fs_dir(), "out.bin"));
    TEST_ASSERT_FALSE(fake_media_file_exists(fake_media_sd_dir(), "out.bin"));
}

/* With no card in the slot the other medium is used rather than the create
 * failing - the fallback the loop exists for. */
TEST_CASE("a create falls back to the other medium when one is absent",
          "[kdrv_storage]")
{
    kdrv_medium_t medium = KDRV_MEDIUM_COUNT;

    storage_fresh();
    fake_media_set_sd_present(false);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_storage_create("out.bin", KDRV_MEDIUM_SD, &medium));
    TEST_ASSERT_EQUAL_INT(KDRV_MEDIUM_FS, medium);
    kdrv_storage_close();
}

TEST_CASE("writing nothing is not an error, and writes nothing",
          "[kdrv_storage]")
{
    kdrv_medium_t medium = KDRV_MEDIUM_FS;

    storage_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_storage_create("z.bin", KDRV_MEDIUM_FS, &medium));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_write("ignored", 0U));
    kdrv_storage_close();

    uint32_t size = 0U;
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        kdrv_storage_open("z.bin", &size, &medium));   /* zero length */
}

TEST_CASE("a write with nothing open is refused", "[kdrv_storage]")
{
    storage_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_storage_write("x", 1U));
}

/* ---- Removing -------------------------------------------------------------- */

/* A file that was written, closed and is no longer wanted. Not the same
 * question as discard(), which is about the file that is open right now. */
TEST_CASE("a file is removed from the medium it was named on", "[kdrv_storage]")
{
    kdrv_medium_t medium = KDRV_MEDIUM_FS;

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_storage_create("gone.bin", KDRV_MEDIUM_SD, &medium));
    TEST_ASSERT_EQUAL_INT((int)KDRV_MEDIUM_SD, (int)medium);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_write("x", 1U));
    kdrv_storage_close();
    TEST_ASSERT_TRUE(fake_media_file_exists(fake_media_sd_dir(), "gone.bin"));

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_storage_remove("gone.bin", KDRV_MEDIUM_SD));
    TEST_ASSERT_FALSE(fake_media_file_exists(fake_media_sd_dir(), "gone.bin"));
}

/* Only the medium it was told about: a delete that went hunting could remove
 * the wrong copy of a name that exists on both. */
TEST_CASE("a file is removed from that medium and no other", "[kdrv_storage]")
{
    kdrv_medium_t medium = KDRV_MEDIUM_SD;

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_storage_create("both.bin", KDRV_MEDIUM_FS, &medium));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_write("x", 1U));
    kdrv_storage_close();

    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_storage_remove("both.bin", KDRV_MEDIUM_SD));
    TEST_ASSERT_TRUE(fake_media_file_exists(fake_media_fs_dir(), "both.bin"));
}

TEST_CASE("removing a file that is nowhere is an error, not a crash",
          "[kdrv_storage]")
{
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_storage_remove("absent.bin", KDRV_MEDIUM_FS));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        kdrv_storage_remove(NULL, KDRV_MEDIUM_FS));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        kdrv_storage_remove("x.bin", KDRV_MEDIUM_COUNT));
}
