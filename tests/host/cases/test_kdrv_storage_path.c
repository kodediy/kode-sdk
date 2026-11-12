/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_storage_path.c
 * @brief The path core, over two real directories on the host.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "unity.h"

#include "fakes.h"
#include "kdrv_storage.h"

/** @brief Start a case with no file open and two empty media. */
static void storage_fresh(void)
{
    kdrv_storage_close();
    fake_media_reset();
}

/**
 * @brief Write a file straight onto the card fake.
 *
 * The FS side has a fake helper of its own (fake_media_put_fs_file()) because
 * every path case file needs it. The card side is only ever written from
 * this one case, so a twin in the fake would exist for a single call site -
 * a local static earns its keep here instead.
 */
static void put_sd_file(const char *name, const char *body)
{
    char path[512];

    (void)snprintf(path, sizeof(path), "%s/%s", fake_media_sd_dir(), name);
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    (void)fwrite(body, 1U, strlen(body), f);
    (void)fclose(f);
}

/** @brief Make one directory straight on the card fake. Same reason as
 *         put_sd_file(): a single call site does not earn a twin in the
 *         fake itself. */
static void put_sd_dir(const char *name)
{
    char path[512];

    (void)snprintf(path, sizeof(path), "%s/%s", fake_media_sd_dir(), name);
    TEST_ASSERT_EQUAL_INT(0, mkdir(path, 0777));
}

TEST_CASE("stat tells a file from a directory", "[kdrv_storage_path]")
{
    kdrv_stat_t st;

    storage_fresh();
    fake_media_put_fs_file("notes.txt", "hello");
    fake_media_put_fs_dir("logs");

    (void)memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_stat("/storage/notes.txt", &st));
    TEST_ASSERT_TRUE(st.exists);
    TEST_ASSERT_FALSE(st.is_dir);
    TEST_ASSERT_EQUAL_UINT32(5U, st.size);

    (void)memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_stat("/storage/logs", &st));
    TEST_ASSERT_TRUE(st.exists);
    TEST_ASSERT_TRUE(st.is_dir);
}

TEST_CASE("a path that is not there is an answer, not an error",
          "[kdrv_storage_path]")
{
    kdrv_stat_t st;

    storage_fresh();

    (void)memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_stat("/storage/nope", &st));
    TEST_ASSERT_FALSE(st.exists);
}

TEST_CASE("stat reaches the card as well as the partition",
          "[kdrv_storage_path]")
{
    kdrv_stat_t st;

    storage_fresh();
    put_sd_file("onsd.bin", "0123456789");

    (void)memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_stat("/sdcard/onsd.bin", &st));
    TEST_ASSERT_TRUE(st.exists);
    TEST_ASSERT_EQUAL_UINT32(10U, st.size);
}

TEST_CASE("the root exists and is a directory", "[kdrv_storage_path]")
{
    kdrv_stat_t st;

    storage_fresh();

    (void)memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_stat("/", &st));
    TEST_ASSERT_TRUE(st.exists);
    TEST_ASSERT_TRUE(st.is_dir);
}

/* The header promises an absolute, NORMALISED path and real_path() is where
 * that promise is kept, for every caller at once. The console's own resolver
 * normalises before it asks, so the console could never have broken it - but
 * kode_block_file_read_part() hands an app's string straight down, and "/storage/../.."
 * would otherwise have been walked out of the tree by libc with nothing in
 * kodeOS objecting. A refusal, not a repair: normalising here would be a
 * second normaliser disagreeing with the console's. */
TEST_CASE("a path that is not normalised is refused", "[kdrv_storage_path]")
{
    kdrv_stat_t st;

    storage_fresh();
    fake_media_put_fs_file("notes.txt", "hello");

    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
                          kdrv_storage_stat("/storage/../sdcard/x", &st));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_storage_stat("/storage/../..", &st));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_storage_stat("/storage/./x", &st));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_storage_stat("/storage//x", &st));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_storage_stat("/storage/", &st));

    /* Every mutation goes through the same helper, so the refusal is not
     * the reading side's alone. */
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_storage_mkdir("/storage/../d"));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_storage_unlink("/storage/./x"));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
                          kdrv_storage_create_path("/storage//x", false));

    /* And what the console's resolver actually produces still passes - the
     * check refuses malformed paths, not deep ones. */
    (void)memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_stat("/storage/notes.txt", &st));
    TEST_ASSERT_TRUE(st.exists);
}

TEST_CASE("a path naming no volume is refused", "[kdrv_storage_path]")
{
    kdrv_stat_t st;

    storage_fresh();

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_storage_stat("/nowhere/x", &st));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_storage_stat("relative", &st));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_storage_stat(NULL, &st));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_storage_stat("/storage/x", NULL));

    /* A volume prefix has to end the component, not merely start it -
     * "/sdcardium" reads as a prefix match on "/sdcard" byte for byte, and
     * "/storagex" runs out of path before the component ever closes. Both
     * name no volume for exactly the same reason and must be refused the
     * same way. */
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_storage_stat("/sdcardium/x", &st));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_storage_stat("/storagex", &st));
}

/**
 * @brief Write a file inside a subdirectory of the FS fake.
 *
 * fake_media_put_fs_file() only reaches the top level of the internal
 * partition, which is what every other case file needs. This one case needs
 * a file a level deeper, so - the same idiom as put_sd_file() above - a local
 * static earns its keep here rather than widening the fake for a single call
 * site.
 */
static void put_fs_file(const char *subdir, const char *name, const char *body)
{
    char path[512];

    (void)snprintf(path, sizeof(path), "%s/%s/%s", fake_media_fs_dir(), subdir, name);
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    (void)fwrite(body, 1U, strlen(body), f);
    (void)fclose(f);
}

/** @brief What a listing walk saw, for the cases below. */
typedef struct {
    unsigned count;
    char     names[8][64];
    bool     dirs[8];
} walk_t;

/** @brief Collect one entry. Stops at eight, which no case needs to exceed. */
static bool walk_cb(const char *name, bool is_dir, uint32_t size, void *ctx)
{
    walk_t * const w = (walk_t *)ctx;

    (void)size;
    if ( 8U > w->count )
    {
        (void)snprintf(w->names[w->count], sizeof(w->names[0]), "%s", name);
        w->dirs[w->count] = is_dir;
        w->count++;
    }
    return true;
}

/** @brief Collect one entry and refuse the rest. */
static bool walk_stop_cb(const char *name, bool is_dir, uint32_t size, void *ctx)
{
    (void)walk_cb(name, is_dir, size, ctx);
    return false;
}

/** @brief Whether @p name was seen, and whether it was a directory. */
static bool walk_saw(const walk_t *w, const char *name, bool is_dir)
{
    for ( unsigned i = 0U; i < w->count; ++i )
    {
        if ( (strcmp(w->names[i], name) == 0) && (w->dirs[i] == is_dir) )
        {
            return true;
        }
    }
    return false;
}

TEST_CASE("a listing carries directories as well as files",
          "[kdrv_storage_path]")
{
    walk_t w;

    storage_fresh();
    fake_media_put_fs_file("notes.txt", "hello");
    fake_media_put_fs_dir("logs");

    (void)memset(&w, 0, sizeof(w));
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_list_path("/storage", walk_cb, &w));
    TEST_ASSERT_TRUE(walk_saw(&w, "notes.txt", false));
    TEST_ASSERT_TRUE(walk_saw(&w, "logs", true));
}

/** @brief Collect one flat-walk entry, which carries no is_dir. */
static bool flat_cb(const char *name, uint32_t size, void *ctx)
{
    return walk_cb(name, false, size, ctx);
}

/* The two walks disagree about the "._name" stubs macOS leaves on a card, and
 * the disagreement is the design: the flat walk feeds the launcher's app
 * scan, where a stub carrying the right extension would show as an app that
 * cannot run, while the path walk feeds `ls`, where hiding a file that
 * `stat`, `cat` and `rm` will all act on would be a lie by omission. */
TEST_CASE("the path walk shows a macOS stub the flat walk hides",
          "[kdrv_storage_path]")
{
    walk_t w;

    storage_fresh();
    fake_media_put_fs_file("._notes.txt", "junk");

    (void)memset(&w, 0, sizeof(w));
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_list_path("/storage", walk_cb, &w));
    TEST_ASSERT_TRUE(walk_saw(&w, "._notes.txt", false));

    (void)memset(&w, 0, sizeof(w));
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_list(KDRV_MEDIUM_FS, flat_cb, &w));
    TEST_ASSERT_FALSE(walk_saw(&w, "._notes.txt", false));
}

TEST_CASE("a listing walks one level, not the tree", "[kdrv_storage_path]")
{
    walk_t w;

    storage_fresh();
    fake_media_put_fs_dir("logs");
    put_fs_file("logs", "yesterday.txt", "x");

    (void)memset(&w, 0, sizeof(w));
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_list_path("/storage", walk_cb, &w));
    TEST_ASSERT_FALSE(walk_saw(&w, "yesterday.txt", false));

    (void)memset(&w, 0, sizeof(w));
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_list_path("/storage/logs", walk_cb, &w));
    TEST_ASSERT_TRUE(walk_saw(&w, "yesterday.txt", false));
}

TEST_CASE("the root lists the two volumes", "[kdrv_storage_path]")
{
    walk_t w;

    storage_fresh();

    (void)memset(&w, 0, sizeof(w));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_list_path("/", walk_cb, &w));
    TEST_ASSERT_EQUAL_UINT(2U, w.count);
    TEST_ASSERT_TRUE(walk_saw(&w, "sdcard", true));
    TEST_ASSERT_TRUE(walk_saw(&w, "storage", true));
}

TEST_CASE("the root still lists a card that is not there",
          "[kdrv_storage_path]")
{
    walk_t w;

    storage_fresh();
    fake_media_set_sd_present(false);

    (void)memset(&w, 0, sizeof(w));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_list_path("/", walk_cb, &w));
    TEST_ASSERT_EQUAL_UINT(2U, w.count);
    TEST_ASSERT_TRUE(walk_saw(&w, "sdcard", true));
}

TEST_CASE("a callback that says stop is obeyed", "[kdrv_storage_path]")
{
    walk_t w;

    storage_fresh();
    fake_media_put_fs_file("a.txt", "1");
    fake_media_put_fs_file("b.txt", "2");
    fake_media_put_fs_file("c.txt", "3");

    (void)memset(&w, 0, sizeof(w));
    /* walk_cb keeps eight; this one keeps one and then refuses. */
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_list_path("/storage", walk_stop_cb, &w));
    TEST_ASSERT_EQUAL_UINT(1U, w.count);
}

/* ---- Opening and creating a path ------------------------------------------
 * The two path functions carry the mechanics of a file; the flat calls above
 * them carry the policy - where to look, and where to write. These cases are
 * about the seam between the two, so they assert both halves: that a path
 * function does exactly what it is told, and that the flat call still makes
 * the same decision it always made on top of it.
 */

TEST_CASE("open_path finds a file where it is told, and only there",
          "[kdrv_storage_path]")
{
    uint32_t size = 0U;

    storage_fresh();
    fake_media_put_fs_file("twice.bin", "internal");
    put_sd_file("twice.bin", "card");

    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_open_path("/storage/twice.bin", &size));
    TEST_ASSERT_EQUAL_UINT32(8U, size);
    kdrv_storage_close();

    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_open_path("/sdcard/twice.bin", &size));
    TEST_ASSERT_EQUAL_UINT32(4U, size);
    kdrv_storage_close();
}

TEST_CASE("the flat open still looks at the card first",
          "[kdrv_storage_path]")
{
    uint32_t      size   = 0U;
    kdrv_medium_t medium = KDRV_MEDIUM_FS;

    storage_fresh();
    fake_media_put_fs_file("twice.bin", "internal");
    put_sd_file("twice.bin", "card");

    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_open("twice.bin", &size, &medium));
    TEST_ASSERT_EQUAL_INT(KDRV_MEDIUM_SD, medium);
    TEST_ASSERT_EQUAL_UINT32(4U, size);
    kdrv_storage_close();
}

/* The card's copy being unusable is an answer, not a reason to go on to the
 * partition, and that is what makes KODE_DENIED a stop rather than a skip in
 * the flat open's loop. A zero-length file is precisely what a counterfeit
 * card leaves behind after one failed write, and it is visible in `sd_ls` -
 * so reporting it as "not found" would send an operator looking for a file
 * they are staring at. The only case that pins the difference between the
 * two ways a path can fail to open.
 */
TEST_CASE("an unusable file on the card stops the search",
          "[kdrv_storage_path]")
{
    uint32_t      size   = 0U;
    kdrv_medium_t medium = KDRV_MEDIUM_FS;

    storage_fresh();
    put_sd_file("app.bin", "");
    fake_media_put_fs_file("app.bin", "a real image");

    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
                          kdrv_storage_open("app.bin", &size, &medium));
    kdrv_storage_close();
}

TEST_CASE("create_path appends when asked", "[kdrv_storage_path]")
{
    uint32_t size = 0U;
    char     body[16];
    size_t   got  = 0U;

    storage_fresh();

    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_create_path("/storage/log.txt", false));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_write("one", 3U));
    kdrv_storage_close();

    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_create_path("/storage/log.txt", true));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_write("two", 3U));
    kdrv_storage_close();

    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_open_path("/storage/log.txt", &size));
    TEST_ASSERT_EQUAL_UINT32(6U, size);
    (void)memset(body, 0, sizeof(body));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_read(body, sizeof(body) - 1U, &got));
    TEST_ASSERT_EQUAL_STRING("onetwo", body);
    kdrv_storage_close();
}

/* ---- Seeking the reader ---------------------------------------------------- */

TEST_CASE("seek moves the reader", "[kdrv_storage_path]")
{
    uint32_t size = 0U;
    char     body[8];
    size_t   got  = 0U;

    storage_fresh();
    fake_media_put_fs_file("digits.txt", "0123456789");

    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_open_path("/storage/digits.txt", &size));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_seek(4U));
    (void)memset(body, 0, sizeof(body));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_read(body, 3U, &got));
    TEST_ASSERT_EQUAL_UINT32(3U, (uint32_t)got);
    TEST_ASSERT_EQUAL_STRING("456", body);
    kdrv_storage_close();
}

TEST_CASE("seek past the end reads nothing", "[kdrv_storage_path]")
{
    uint32_t size = 0U;
    char     body[8];
    size_t   got  = 99U;

    storage_fresh();
    fake_media_put_fs_file("digits.txt", "0123456789");

    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_open_path("/storage/digits.txt", &size));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_seek(20U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_read(body, sizeof(body), &got));
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)got);
    kdrv_storage_close();
}

TEST_CASE("seek with no reader is an error", "[kdrv_storage_path]")
{
    storage_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_storage_seek(0U));
}

/* discard() after a failed append must not remove a pre-existing file - see
 * kdrv_storage_create_path()'s @param append and kdrv_storage_discard()'s own
 * note. This is the append half of the write; the truncating half is already
 * pinned by test_kdrv_storage.c's "discard during a write removes the file it
 * was writing", which must keep passing exactly as it is.
 */
TEST_CASE("discard after a failed append leaves the file it reopened alone",
          "[kdrv_storage_path]")
{
    uint32_t size = 0U;
    char     body[16];
    size_t   got  = 0U;

    storage_fresh();

    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_create_path("/storage/log.txt", false));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_write("kept", 4U));
    kdrv_storage_close();

    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_create_path("/storage/log.txt", true));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_write("bad", 3U));
    kdrv_storage_discard();

    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_open_path("/storage/log.txt", &size));
    TEST_ASSERT_EQUAL_UINT32(7U, size);
    (void)memset(body, 0, sizeof(body));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_read(body, sizeof(body) - 1U, &got));
    TEST_ASSERT_EQUAL_STRING("keptbad", body);
    kdrv_storage_close();
}

/* ---- The two header-documented guards -------------------------------------
 * Finding 3: both of these are behaviour the header promises and, until now,
 * nothing pinned - the same shape of gap that raised the "/sdcardium" finding
 * against kdrv_storage_medium_of() earlier in this file.
 */

/* kdrv_storage_open()'s header says size is required and a NULL is
 * KODE_DENIED, unlike medium, which may be NULL. A file is left on the
 * partition for this call to find, so the assertion is about the guard
 * itself and not a side effect of there being nothing to open. */
TEST_CASE("open with no size out-param is refused, not answered with none",
          "[kdrv_storage_path]")
{
    kdrv_medium_t medium = KDRV_MEDIUM_FS;

    storage_fresh();
    fake_media_put_fs_file("app.bin", "a real image");

    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
                          kdrv_storage_open("app.bin", NULL, &medium));
}

/* kdrv_storage_create()'s header says KDRV_MEDIUM_COUNT as @p first is
 * refused rather than falling through - it used to reach probe()'s
 * "m != SD" branch, create silently on the internal partition and report
 * medium = KDRV_MEDIUM_COUNT, a value with no volume behind it. vpath_of()
 * now refuses COUNT before a real_path() is ever built from it, so the loop
 * falls through to the normal order instead: SD, since fake_media_reset()
 * mounts both media, is where it lands and what gets reported. */
TEST_CASE("create refuses KDRV_MEDIUM_COUNT as first instead of falling through",
          "[kdrv_storage_path]")
{
    kdrv_medium_t medium = KDRV_MEDIUM_COUNT;

    storage_fresh();

    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_create("shot.bmp", KDRV_MEDIUM_COUNT, &medium));
    TEST_ASSERT_EQUAL_INT(KDRV_MEDIUM_SD, medium);
    TEST_ASSERT_TRUE(fake_media_file_exists(fake_media_sd_dir(), "shot.bmp"));
    kdrv_storage_close();
}

/* ---- Changing the tree ----------------------------------------------------
 * mkdir, rm, rmdir and mv/rename. Every one of them is "real_path() then one
 * libc call" (kdrv_storage_unlink() is the one exception - see its own case
 * below), so what is worth pinning here is the libc call's own refusals
 * showing up unchanged: a taken name, a missing parent, a directory that
 * still holds something, a rename asked to cross a mount.
 */

TEST_CASE("mkdir makes one level and says when it cannot",
          "[kdrv_storage_path]")
{
    kdrv_stat_t st;

    storage_fresh();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_mkdir("/storage/logs"));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_stat("/storage/logs", &st));
    TEST_ASSERT_TRUE(st.is_dir);

    /* Two levels at once is not a thing here: the parent has to exist. */
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_storage_mkdir("/storage/a/b"));
}

TEST_CASE("rm takes files and rmdir takes empty directories",
          "[kdrv_storage_path]")
{
    kdrv_stat_t st;

    storage_fresh();
    fake_media_put_fs_file("gone.txt", "x");
    fake_media_put_fs_dir("full");
    put_fs_file("full", "inside.txt", "x");
    fake_media_put_fs_dir("empty");

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_unlink("/storage/gone.txt"));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_stat("/storage/gone.txt", &st));
    TEST_ASSERT_FALSE(st.exists);

    /* A directory refuses rm exactly as remove() would have said yes on some
     * libcs - the whole reason kdrv_storage_unlink() stats before it deletes. */
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_storage_unlink("/storage/empty"));

    /* Non-empty refuses rmdir; empty does not. */
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_storage_rmdir("/storage/full"));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_rmdir("/storage/empty"));
}

TEST_CASE("rename moves within a volume and refuses to cross one",
          "[kdrv_storage_path]")
{
    kdrv_stat_t st;

    storage_fresh();
    fake_media_put_fs_file("before.txt", "x");

    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_rename("/storage/before.txt",
                                              "/storage/after.txt"));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_stat("/storage/after.txt", &st));
    TEST_ASSERT_TRUE(st.exists);

    /* A move, not a copy: the name it moved from has to be gone, or a
     * rename() that quietly copied instead would pass on the line above
     * alone. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_stat("/storage/before.txt", &st));
    TEST_ASSERT_FALSE(st.exists);

    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
                          kdrv_storage_rename("/storage/after.txt",
                                              "/sdcard/after.txt"));
}

/* FatFs refuses a rename onto a name that is there, and POSIX replaces it:
 * the driver says no on both, so a caller that forgot to remove the target
 * fails in this suite and not only on the board. */
TEST_CASE("rename onto a name that is there is refused, and both files stay",
          "[kdrv_storage_path]")
{
    storage_fresh();
    fake_media_put_fs_file("from.txt", "new");
    fake_media_put_fs_file("to.txt", "old");

    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
                          kdrv_storage_rename("/storage/from.txt",
                                              "/storage/to.txt"));
    TEST_ASSERT_TRUE(fake_media_file_exists(fake_media_fs_dir(), "from.txt"));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_unlink("/storage/to.txt"));
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_rename("/storage/from.txt",
                                              "/storage/to.txt"));
    TEST_ASSERT_FALSE(fake_media_file_exists(fake_media_fs_dir(), "from.txt"));
}

/* ---- Two handles, and the copy they exist for -----------------------------
 * kdrv_storage kept a single FILE * for the whole system until copying
 * existed, and copying is exactly the pair a single handle cannot hold: a
 * reader and a writer at the same instant. The promise is now one of each,
 * and these cases pin both halves of that - what is newly allowed, and what
 * is still refused. Nothing else was widened: two readers is still one too
 * many, and so is two writers.
 *
 * Every case that claims a copy landed reads the destination's CONTENT back
 * rather than trusting a return code, and checks the source is still there.
 * `cp` is not `mv`, and a copy_open() that renamed instead would pass on the
 * destination alone.
 */

/** @brief Read @p path back through the reader and assert it holds @p want. */
static void reads_back(const char *path, const char *want)
{
    uint32_t size = 0U;
    char     body[64];
    size_t   got  = 0U;

    (void)memset(body, 0, sizeof(body));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_open_path(path, &size));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)strlen(want), size);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_read(body, sizeof(body) - 1U, &got));
    TEST_ASSERT_EQUAL_UINT(strlen(want), got);
    TEST_ASSERT_EQUAL_STRING(want, body);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_close());
}

/**
 * @brief Step an open copy to the end through a @p cap-byte window.
 *
 * @return How many steps it took, the last one included - the step that
 *         reports zero bytes is what says the source ran out, so a copy of
 *         anything at all costs at least two.
 */
static unsigned copy_drain(void *buf, size_t cap)
{
    unsigned steps = 0U;

    for ( ;; )
    {
        size_t done = 0U;

        /* A guard, not an assertion about the copy: a step that never
         * reported zero would otherwise hang the whole suite rather than
         * fail one case. */
        TEST_ASSERT_TRUE(1000U > steps);
        TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_copy_step(buf, cap, &done));
        ++steps;
        if ( 0U == done )
        {
            break;
        }
    }
    return steps;
}

TEST_CASE("a reader and a writer may be open at once", "[kdrv_storage_path]")
{
    uint32_t size = 0U;

    storage_fresh();
    fake_media_put_fs_file("src.txt", "abcdef");

    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_open_path("/storage/src.txt", &size));
    /* The line that used to be KODE_DENIED. */
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_create_path("/storage/dst.txt", false));

    /* And both handles really are live, rather than the create having taken
     * the read's place: a read and a write both work between them. */
    char   body[16];
    size_t got = 0U;
    (void)memset(body, 0, sizeof(body));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_read(body, 6U, &got));
    TEST_ASSERT_EQUAL_UINT(6U, got);
    TEST_ASSERT_EQUAL_STRING("abcdef", body);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_write(body, 6U));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_close());
    reads_back("/storage/dst.txt", "abcdef");
}

TEST_CASE("a second reader is refused, and so is a second writer",
          "[kdrv_storage_path]")
{
    uint32_t size = 0U;

    storage_fresh();
    fake_media_put_fs_file("src.txt", "abcdef");
    fake_media_put_fs_file("other.txt", "ghijkl");

    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_open_path("/storage/src.txt", &size));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
                          kdrv_storage_open_path("/storage/other.txt", &size));

    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_create_path("/storage/dst.txt", false));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
                          kdrv_storage_create_path("/storage/dst2.txt", false));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_close());

    /* The refused create must not have left a name behind: a second writer
     * that opened the file and then reported KODE_DENIED would leave an
     * empty dst2.txt in every listing. */
    TEST_ASSERT_FALSE(fake_media_file_exists(fake_media_fs_dir(), "dst2.txt"));
}

TEST_CASE("copy carries the bytes across volumes and leaves the source",
          "[kdrv_storage_path]")
{
    uint8_t  buf[64];
    uint32_t size = 0U;

    storage_fresh();
    put_sd_file("onsd.txt", "carried");

    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_copy_open("/sdcard/onsd.txt",
                                                 "/storage/onsd.txt", &size));
    TEST_ASSERT_EQUAL_UINT32(7U, size);
    TEST_ASSERT_EQUAL_UINT(2U, copy_drain(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_copy_end(true));

    reads_back("/storage/onsd.txt", "carried");
    /* A copy, not a move. */
    reads_back("/sdcard/onsd.txt", "carried");
}

TEST_CASE("copy crosses the other way too", "[kdrv_storage_path]")
{
    uint8_t  buf[64];
    uint32_t size = 0U;

    storage_fresh();
    fake_media_put_fs_file("inside.txt", "outbound");

    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_copy_open("/storage/inside.txt",
                                                 "/sdcard/inside.txt", &size));
    TEST_ASSERT_EQUAL_UINT32(8U, size);
    (void)copy_drain(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_copy_end(true));

    reads_back("/sdcard/inside.txt", "outbound");
    reads_back("/storage/inside.txt", "outbound");
}

/* The copy is chunked because AO_Storage runs it a chunk per dispatch step,
 * and a step that carried the whole file would hold everything below
 * AO_Storage - AO_Gui among them - for as long as the copy took. A four-byte
 * window here is the same shape as the board's 32 KB one, small enough for a
 * case to count the steps: ten bytes is 4 + 4 + 2, and then the step that
 * reports nothing. */
TEST_CASE("a copy longer than one window takes several steps",
          "[kdrv_storage_path]")
{
    uint8_t  buf[4];
    uint32_t size = 0U;

    storage_fresh();
    fake_media_put_fs_file("ten.txt", "0123456789");

    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_copy_open("/storage/ten.txt",
                                                 "/storage/copy.txt", &size));
    TEST_ASSERT_EQUAL_UINT32(10U, size);
    TEST_ASSERT_EQUAL_UINT(4U, copy_drain(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_copy_end(true));

    reads_back("/storage/copy.txt", "0123456789");
}

/* A file that promises bytes it does not have is worse than no file at all:
 * `app_run` would try to boot it. kdrv_storage_discard() already says this
 * about a failed write, and an abandoned copy is the same story told about
 * the destination it had started. */
TEST_CASE("an abandoned copy leaves no stub behind", "[kdrv_storage_path]")
{
    uint8_t     buf[4];
    size_t      done = 0U;
    kdrv_stat_t st;
    uint32_t    size = 0U;

    storage_fresh();
    put_sd_file("onsd.txt", "carried");

    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_copy_open("/sdcard/onsd.txt",
                                                 "/storage/onsd.txt", &size));
    /* Part way through, so the stub genuinely exists on disk before it is
     * abandoned - a copy_end(false) called before any byte moved would pass
     * even if it removed nothing. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_copy_step(buf, sizeof(buf),
                                                          &done));
    TEST_ASSERT_EQUAL_UINT(4U, done);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_copy_end(false));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_stat("/storage/onsd.txt", &st));
    TEST_ASSERT_FALSE(st.exists);

    /* And the source is untouched by the abandonment. */
    reads_back("/sdcard/onsd.txt", "carried");
}

/* Both handles are let go by copy_end(), whichever way it ended, or the next
 * command on the console would be refused with "a file is already open" for
 * the rest of the session. */
TEST_CASE("a copy hands both handles back when it ends",
          "[kdrv_storage_path]")
{
    uint8_t  buf[64];
    uint32_t size = 0U;

    storage_fresh();
    fake_media_put_fs_file("a.txt", "first");

    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_copy_open("/storage/a.txt",
                                                 "/storage/b.txt", &size));
    (void)copy_drain(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_copy_end(true));

    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_open_path("/storage/a.txt", &size));
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_create_path("/storage/c.txt", false));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_close());

    /* The abandoned half of the same question. */
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_copy_open("/storage/a.txt",
                                                 "/storage/d.txt", &size));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_copy_end(false));
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_open_path("/storage/a.txt", &size));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_close());
}

/* The one refusal that is about data rather than about arguments: a copy onto
 * its own source opens the file for reading and then truncates it, and the
 * bytes are gone before the first step ever runs. */
TEST_CASE("a copy onto its own source is refused before it truncates it",
          "[kdrv_storage_path]")
{
    uint32_t size = 0U;

    storage_fresh();
    fake_media_put_fs_file("same.txt", "still here");

    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
                          kdrv_storage_copy_open("/storage/same.txt",
                                                 "/storage/same.txt", &size));
    reads_back("/storage/same.txt", "still here");
}

TEST_CASE("a copy that cannot start says so and leaves nothing open",
          "[kdrv_storage_path]")
{
    uint32_t size = 0U;

    storage_fresh();
    fake_media_put_fs_file("real.txt", "bytes");

    /* No such source: kdrv_storage_open_path()'s own KODE_ERROR, unchanged. */
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
                          kdrv_storage_copy_open("/storage/nope.txt",
                                                 "/storage/dst.txt", &size));
    /* A destination naming no volume: refused, and the source it had already
     * opened let go again rather than left dangling. */
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
                          kdrv_storage_copy_open("/storage/real.txt",
                                                 "/nowhere/dst.txt", &size));

    /* Neither attempt left a handle behind. */
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_open_path("/storage/real.txt", &size));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_close());
    /* Nor a stub for the destination that never got written. */
    TEST_ASSERT_FALSE(fake_media_file_exists(fake_media_fs_dir(), "dst.txt"));
}

/* copy_step() outside a copy would otherwise read whatever reader happens to
 * be open and write it into whatever writer happens to be open - two handles
 * that were never paired. */
TEST_CASE("a copy step outside a copy is refused", "[kdrv_storage_path]")
{
    uint8_t  buf[16];
    size_t   done = 0U;
    uint32_t size = 0U;

    storage_fresh();
    fake_media_put_fs_file("src.txt", "abcdef");

    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
                          kdrv_storage_copy_step(buf, sizeof(buf), &done));

    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_open_path("/storage/src.txt", &size));
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_create_path("/storage/dst.txt", false));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
                          kdrv_storage_copy_step(buf, sizeof(buf), &done));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_close());
}

/* kdrv_storage_close() used to throw away what fclose() said, so a write that
 * failed only on its final buffered flush reported success for bytes that
 * never landed - which is precisely the counterfeit-card story this layer
 * keeps warning about. The failing flush itself is not reachable from a host
 * case (nothing here can fill a filesystem to order), so what is pinned is
 * the contract's other half: the answer exists, and a close that did work
 * says so. */
TEST_CASE("close answers for the write it closed", "[kdrv_storage_path]")
{
    storage_fresh();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_close());   /* nothing open */

    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_create_path("/storage/out.txt", false));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_write("landed", 6U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_close());

    reads_back("/storage/out.txt", "landed");
}

/* The card warning is not this layer's job at all. It used to live here too,
 * as a second, byte-for-byte copy of kaos.storage's own warn_if_card() - one
 * called from kdrv_storage_unlink()/kdrv_storage_rename() past their early
 * exits, the other called from storage_ao.c before the kdrv call, both
 * logging the same line under a different tag. Two mechanisms answering the
 * same question is a bug in this codebase, not a style choice, and the
 * question itself - when to caution an operator before a write - is a
 * timing and policy decision that belongs to the owner of the card
 * (AO_Storage), not to the driver that merely knows how to speak to it.
 * kdrv_storage.c went back to knowing nothing about warning; every case
 * below runs straight past the kdrv layer's own fake_klog, mkdir/rm/rmdir/mv
 * included, on both a real card write and one that never happens. */
TEST_CASE("the driver never warns about writing to the card",
          "[kdrv_storage_path]")
{
    storage_fresh();
    put_sd_dir("full");
    put_sd_file("full/inside.txt", "x");

    fake_klog_reset();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_mkdir("/sdcard/newdir"));
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());

    fake_klog_reset();
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_storage_unlink("/sdcard/gone.txt"));
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());

    fake_klog_reset();
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_storage_unlink("/sdcard/full"));
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());

    fake_klog_reset();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_unlink("/sdcard/full/inside.txt"));
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());

    fake_klog_reset();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_storage_rmdir("/sdcard/newdir"));
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());

    fake_klog_reset();
    fake_media_put_fs_file("before.txt", "x");
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
                          kdrv_storage_rename("/storage/before.txt",
                                              "/sdcard/before.txt"));
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());

    fake_klog_reset();
    put_sd_file("moveme.txt", "x");
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_storage_rename("/sdcard/moveme.txt",
                                              "/sdcard/moved.txt"));
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());
}
