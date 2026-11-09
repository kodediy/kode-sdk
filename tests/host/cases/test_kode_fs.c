/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_fs.c
 * @brief kode_sys_file_*(): the door kode_sys_flash() left shut, opened the IR/NFC way.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "qpc.h"

#include "ao_spy.h"
#include "fake_khal_worker.h"
#include "fakes.h"
#include "kaos.h"
#include "kbsp_owners.h"
#include "kdrv_storage.h"
#include "khal_sync.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kqp.h"
#include "qf_support.h"

/** @brief The volume every case works on; the card is left absent. */
#define FS_VOL  "/storage"

/**
 * @brief What a stand-in running inside a facade call could not do.
 *
 * Set instead of asserted, and read back by the case once the call it
 * interrupted has returned and given the claim back.
 */
static const char *s_hook_fault;

/** @brief Drop everything sitting, undispatched, in @p ao's own queue. */
static void drain(QActive *ao)
{
    const QEvt *e;
    while ( (NULL != ao->eQueue)
           && (xQueueReceive(ao->eQueue, &e, 0U) == pdTRUE) )
    {
        QF_gc(e);
    }
}

/** @brief How many requests are waiting for AO_Storage. */
static unsigned fs_pending(void)
{
    return (unsigned)uxQueueMessagesWaiting(AO_Storage->eQueue);
}

static void fs_settle_once(void);

/**
 * @brief Let AO_Storage answer, and stay armed for a second round trip.
 *
 * kode_block_file_list_each(), kode_block_file_read_part() and kode_block_file_delete() each STAT before they
 * ask AO_Storage for anything else, 0U, which is two separate waits inside one
 * top-level call - the same two round trips `ls` already takes at the console
 * (see test_console_fs.c's own comment on that). fake_sem_wait_hook() is
 * consumed the moment it runs, so this re-installs itself as its own last
 * act, the way a hook that must survive more than one wait has to.
 */
static void fs_settle_and_rearm(void)
{
    fs_settle_once();
    fake_sem_wait_hook(&fs_settle_and_rearm);
}

/** @brief Arm the settle hook for the next facade call this case makes. */
static void fs_will_settle(void)
{
    fake_sem_wait_hook(&fs_settle_and_rearm);
}

/**
 * @brief Let AO_Storage answer exactly one wait, and no more.
 *
 * fake_sem_wait_hook() consumes whatever is armed the moment it runs, so a
 * stand-in that does NOT re-arm itself answers the first round trip of a call
 * and leaves the second one hanging. That is the only way to reach the state a
 * slow card really produces: a two-step call whose STAT came back and whose
 * real request is still sitting in AO_Storage's queue when the deadline runs
 * out.
 *
 * A lend or a return is a call on AO_Storage's worker; it is run here, on the
 * thread of the task that is waiting for it, and the answer it brings is read.
 */
static void fs_settle_once(void)
{
    (void)qf_support_pump(AO_Storage, 8U);
    while ( fake_khal_worker_run_next_for("storage_sd") )
    {
        (void)qf_support_pump(AO_Storage, 8U);
    }
}
/**
 * @brief Disarm whatever hook is armed, so the next wait finds nobody home.
 *
 * fs_settle_and_rearm() leaves itself armed on purpose, which means a case
 * that wants a call to go unanswered has to say so rather than assume it.
 */
static void fs_no_settle(void)
{
    fake_sem_wait_hook(NULL);
}

/**
 * @brief Prove the facade is clean before a case runs.
 *
 * Two different things an earlier case can leave behind, and this catches
 * both with one probe, because the facade answers both the same way.
 *
 * The first is a leaked claim: it is taken by whichever call is in
 * flight and given back when it returns, and the one way to lose it is a
 * longjmp out of a hook or a callback, which is why neither ever asserts in
 * this file. The second is an answer still owed - a case that let a call time
 * out and then ended without letting AO_Storage finish the abandoned request.
 * The facade latches that and will not lend its buffers again until it lands,
 * which is the whole point of the fix that put it there, and on the host the
 * abandoned request dies with the AO_Storage instance fs_fresh() replaces.
 *
 * So: one ordinary facade call, with AO_Storage free to answer it. KODE_OK
 * means neither hazard is present. KODE_DENIED means one of them is, and
 * naming it here - at the top of the FIRST case after the damage - is worth
 * far more than the cascade of unexplained refusals it would otherwise
 * become. The rule it enforces is short: a case that provokes a timeout must
 * let AO_Storage answer before it ends.
 *
 * The probe leaves nothing behind: it asks about a path that is not there,
 * which is a KODE_OK with `exists` false and touches no file, and the token
 * and the clock are reset after it.
 */
static void fs_claim_barrier(void)
{
    kode_sys_file_status_t st;

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        KODE_OK, kode_block_file_status(FS_VOL "/barrier.none", &st),
        "the facade was still dirty when this case began: an earlier case "
        "either left a kode_sys_file_*() call without releasing its claim, or let "
        "one time out and ended without letting AO_Storage answer it");

    drain(AO_Storage);
    fake_sem_reset();
    fake_time_reset();
}

/**
 * @brief Bring AO_Storage up, fresh, for one case.
 *
 * The same shape storage_fresh() and console_up_once() already give it, minus
 * the registry slot: AO_Storage subscribes to nothing, a publisher needs no
 * slot, and the slot is test_storage_ao.c's to take.
 */
static void fs_fresh(void)
{
    /* kdrv_storage keeps one handle for the whole system; a read or a write
     * an earlier case left open would refuse the next one for a reason that
     * has nothing to do with what it is testing. */
    (void)kdrv_storage_close();

    qf_support_init();
    (void)ao_spy();
    ao_spy_drain();

    qf_support_reap_timers();   /* see storage_fresh() */
    Storage_ctor();
    qf_support_give_queue(AO_Storage);
    AO_Storage->prio  = KODE_PRIO_STORAGE;
    AO_Storage->pthre = KODE_PRIO_STORAGE;
    qf_support_register(AO_Storage);
    QASM_INIT(AO_Storage, (void *)0, 0U);
    kbsp_release(KBSP_RES_SDMMC, AO_Storage);
    kbsp_release(KBSP_RES_FATFS, AO_Storage);

    drain(AO_Storage);

    fake_media_reset();
    fake_sem_reset();
    fake_time_reset();
    s_hook_fault = NULL;

    fs_claim_barrier();
}

/* ---- Building files a C string cannot describe -----------------------------
 * fake_media_put_fs_file() takes a NUL-terminated body, which is exactly
 * wrong for the two things worth proving about a byte-counted facade: a file
 * at the ceiling, and a payload with a NUL in the middle of it.
 * These write the medium directly, the way test_storage_ao.c's own make_file()
 * does.
 */

/**
 * @brief Put @p n bytes of @p body in @p name on the internal partition.
 *
 * @param name File name, no directory part.
 * @param body The bytes.
 * @param n    How many.
 * @return true when the file was written whole. Reported rather than
 *         asserted, because the hooks below call this from inside a facade
 *         call.
 */
static bool put_bytes(const char *name, const char *body, size_t n)
{
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/%s", fake_media_fs_dir(), name);

    FILE * const f = fopen(path, "wb");
    if ( NULL == f )
    {
        return false;
    }
    size_t const wrote = fwrite(body, 1U, n, f);
    (void)fclose(f);
    return (wrote == n);
}

/**
 * @brief Put a file of @p n identical bytes in @p name.
 *
 * @param name File name, no directory part.
 * @param n    How many bytes.
 * @param fill The byte to repeat.
 * @return As put_bytes().
 */
static bool put_filler(const char *name, size_t n, char fill)
{
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/%s", fake_media_fs_dir(), name);

    FILE * const f = fopen(path, "wb");
    if ( NULL == f )
    {
        return false;
    }
    for ( size_t i = 0U; i < n; ++i )
    {
        if ( fputc(fill, f) == EOF )
        {
            (void)fclose(f);
            return false;
        }
    }
    (void)fclose(f);
    return true;
}

/** @brief The byte at position @p i of the patterned files below. */
#define PATTERN_MOD  251U

/**
 * @brief Put a file of @p n bytes in @p name, byte i being i % PATTERN_MOD.
 *
 * @param name File name, no directory part.
 * @param n    How many bytes.
 * @return As put_bytes().
 */
static bool put_pattern(const char *name, size_t n)
{
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/%s", fake_media_fs_dir(), name);

    FILE * const f = fopen(path, "wb");
    if ( NULL == f )
    {
        return false;
    }
    for ( size_t i = 0U; i < n; ++i )
    {
        if ( fputc((int)(i % PATTERN_MOD), f) == EOF )
        {
            (void)fclose(f);
            return false;
        }
    }
    (void)fclose(f);
    return true;
}

/**
 * @brief Do @p n bytes at @p buf equal the pattern from position @p first?
 *
 * @param buf   The bytes.
 * @param first Position in the file of buf[0].
 * @param n     How many.
 * @return true when every one matches.
 */
static bool pattern_matches(const uint8_t *buf, size_t first, size_t n)
{
    for ( size_t i = 0U; i < n; ++i )
    {
        if ( (uint8_t)((first + i) % PATTERN_MOD) != buf[i] )
        {
            return false;
        }
    }
    return true;
}

/* ---- The three cases the brief itself asks for ---------------------------- */

TEST_CASE("the facade answers about a path", "[kode_fs]")
{
    kode_sys_file_status_t st;

    fs_fresh();
    fake_media_put_fs_file("f.txt", "12345");

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_file_status(FS_VOL "/f.txt", &st));
    TEST_ASSERT_TRUE(st.exists);
    TEST_ASSERT_FALSE(st.is_dir);
    TEST_ASSERT_EQUAL_UINT32(5U, st.size);
}

/** @brief What a second, concurrent facade call was answered with. */
static kode_status_t s_second_rc;

/** @brief How many requests AO_Storage held while that second call ran. */
static unsigned s_second_pending;

/**
 * @brief A second kode_block_file_status(), from inside the first one's own wait.
 *
 * The pump afterwards is what lets the FIRST call finish: the stand-in that
 * would normally do it has been displaced by this one for this wait.
 */
static void stat_again_then_settle(void)
{
    static kode_sys_file_status_t st;

    s_second_rc      = kode_block_file_status(FS_VOL "/f.txt", &st);
    s_second_pending = fs_pending();
    (void)qf_support_pump(AO_Storage, 8U);
}

TEST_CASE("the facade refuses a second concurrent caller", "[kode_fs]")
{
    kode_sys_file_status_t st;

    fs_fresh();
    fake_media_put_fs_file("f.txt", "12345");

    s_second_rc      = KODE_OK;
    s_second_pending = 99U;
    fake_sem_wait_hook(&stat_again_then_settle);

    /* Asserted, not discarded: a first call that quietly refused itself would
     * leave the second one's refusal proving nothing. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_file_status(FS_VOL "/f.txt", &st));
    TEST_ASSERT_TRUE(st.exists);

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, s_second_rc,
                                  "the second caller is turned away");
    /* And turned away before AO_Storage heard of it: one request outstanding
     * at that moment, the first caller's own, not two. */
    TEST_ASSERT_EQUAL_UINT(1U, s_second_pending);
}

TEST_CASE("a write through the facade lands where it says", "[kode_fs]")
{
    kode_sys_file_status_t st;

    fs_fresh();

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_block_file_write_buffer(FS_VOL "/w.txt", "abc", 3U, false));

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_file_status(FS_VOL "/w.txt", &st));
    TEST_ASSERT_TRUE(st.exists);
    TEST_ASSERT_EQUAL_UINT32(3U, st.size);
}

TEST_CASE("append keeps what was there and adds after it", "[kode_fs]")
{
    char     buf[16];
    uint32_t got = 0U;

    fs_fresh();
    fake_media_put_fs_file("w.txt", "abc");

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_block_file_write_buffer(FS_VOL "/w.txt", "de", 2U, true));

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_block_file_read_part(FS_VOL "/w.txt", 0U, buf, sizeof(buf),
                                       &got));
    TEST_ASSERT_EQUAL_UINT32(5U, got);
    TEST_ASSERT_EQUAL_MEMORY("abcde", buf, 5U);
}

/* ---- A window of a file, 32 KB a call --------------------------------------
 * The numbers below are literals on purpose: the cases around them follow
 * whatever KODE_SYS_FILE_READ_MAX is, these pin what it is.
 */

TEST_CASE("read_part reads the middle of a file", "[kode_fs]")
{
    static uint8_t buf[5000];
    uint32_t       got = 0U;

    fs_fresh();
    TEST_ASSERT_TRUE(put_pattern("p.bin", 40000U));

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_block_file_read_part(FS_VOL "/p.bin", 33000U, buf,
                                                    (uint32_t)sizeof(buf), &got));
    TEST_ASSERT_EQUAL_UINT32(5000U, got);
    TEST_ASSERT_TRUE(pattern_matches(buf, 33000U, 5000U));
}

TEST_CASE("read_part at the end answers got short", "[kode_fs]")
{
    static uint8_t buf[5000];
    uint32_t       got = 0U;

    fs_fresh();
    TEST_ASSERT_TRUE(put_pattern("p.bin", 40000U));

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_block_file_read_part(FS_VOL "/p.bin", 39000U, buf,
                                                    (uint32_t)sizeof(buf), &got));
    TEST_ASSERT_EQUAL_UINT32(1000U, got);
    TEST_ASSERT_TRUE(pattern_matches(buf, 39000U, 1000U));
}

TEST_CASE("read_part past the end answers 0", "[kode_fs]")
{
    static uint8_t buf[64];
    uint32_t       got = 99U;

    fs_fresh();
    TEST_ASSERT_TRUE(put_pattern("p.bin", 40000U));

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_block_file_read_part(FS_VOL "/p.bin", 40000U, buf,
                                                    (uint32_t)sizeof(buf), &got));
    TEST_ASSERT_EQUAL_UINT32(0U, got);

    got = 99U;
    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_block_file_read_part(FS_VOL "/p.bin", 90000U, buf,
                                                    (uint32_t)sizeof(buf), &got));
    TEST_ASSERT_EQUAL_UINT32(0U, got);
}

TEST_CASE("read_part moves a whole 32,768-byte window", "[kode_fs]")
{
    static uint8_t buf[32768];
    uint32_t       got = 0U;

    fs_fresh();
    TEST_ASSERT_TRUE(put_pattern("p.bin", 40000U));

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_block_file_read_part(FS_VOL "/p.bin", 5U, buf,
                                                    (uint32_t)sizeof(buf), &got));
    TEST_ASSERT_EQUAL_UINT32(32768U, got);
    TEST_ASSERT_TRUE(pattern_matches(buf, 5U, 32768U));
}

TEST_CASE("read_part refuses a NULL buf, a NULL path and a zero cap",
          "[kode_fs]")
{
    uint8_t  buf[8];
    uint32_t got = 99U;

    fs_fresh();

    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
                          kode_block_file_read_part(FS_VOL "/p.bin", 0U, NULL, 8U,
                                                    &got));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
                          kode_block_file_read_part(NULL, 0U, buf,
                                                    (uint32_t)sizeof(buf), &got));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
                          kode_block_file_read_part(FS_VOL "/p.bin", 0U, buf, 0U,
                                                    &got));
    TEST_ASSERT_EQUAL_UINT32(99U, got);
    TEST_ASSERT_EQUAL_UINT(0U, fs_pending());
}

TEST_CASE("read_part refuses a window over the ceiling, posting nothing",
          "[kode_fs]")
{
    static uint8_t buf[32769];
    uint32_t       got = 99U;

    fs_fresh();

    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
                          kode_block_file_read_part(FS_VOL "/p.bin", 0U, buf,
                                                    (uint32_t)sizeof(buf), &got));
    TEST_ASSERT_EQUAL_UINT32(99U, got);
    TEST_ASSERT_EQUAL_UINT(0U, fs_pending());
}

TEST_CASE("read_part answers what AO_Storage answers when it cannot open",
          "[kode_fs]")
{
    uint8_t  buf[8];
    uint32_t got = 99U;

    fs_fresh();

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
                          kode_block_file_read_part(FS_VOL "/nope.bin", 0U, buf,
                                                    (uint32_t)sizeof(buf), &got));
    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
                          kode_block_file_read_part(FS_VOL "/../..", 0U, buf,
                                                    (uint32_t)sizeof(buf), &got));
    TEST_ASSERT_EQUAL_UINT32(99U, got);
}

TEST_CASE("read_part of an empty file is KODE_OK with nothing copied",
          "[kode_fs]")
{
    uint8_t  buf[8];
    uint32_t got = 99U;

    fs_fresh();
    TEST_ASSERT_TRUE(put_bytes("empty.bin", "", 0U));
    (void)memset(buf, 0xA5, sizeof(buf));

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_block_file_read_part(FS_VOL "/empty.bin", 0U, buf,
                                                    (uint32_t)sizeof(buf), &got));
    TEST_ASSERT_EQUAL_UINT32(0U, got);
    TEST_ASSERT_EQUAL_UINT8(0xA5U, buf[0]);

    /* The claim was given back: the next call is served, not refused. */
    TEST_ASSERT_TRUE(put_pattern("p.bin", 8U));
    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_block_file_read_part(FS_VOL "/p.bin", 0U, buf,
                                                    (uint32_t)sizeof(buf), &got));
    TEST_ASSERT_EQUAL_UINT32(8U, got);
}

TEST_CASE("a read reads a 32,768-byte file whole", "[kode_fs]")
{
    static uint8_t buf[32768];
    uint32_t       got = 0U;

    fs_fresh();
    TEST_ASSERT_TRUE(put_pattern("p.bin", 32768U));

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_block_file_read_part(FS_VOL "/p.bin", 0U, buf, (uint32_t)sizeof(buf), &got));
    TEST_ASSERT_EQUAL_UINT32(32768U, got);
    TEST_ASSERT_TRUE(pattern_matches(buf, 0U, 32768U));
}

TEST_CASE("write_buffer writes 32,768 bytes and refuses 32,769", "[kode_fs]")
{
    static uint8_t         data[32769];
    kode_sys_file_status_t st;

    fs_fresh();
    for ( size_t i = 0U; sizeof(data) > i; ++i )
    {
        data[i] = (uint8_t)(i % PATTERN_MOD);
    }

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_block_file_write_buffer(FS_VOL "/w.bin", data, 32768U,
                                                       false));
    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_file_status(FS_VOL "/w.bin", &st));
    TEST_ASSERT_EQUAL_UINT32(32768U, st.size);

    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES,
                          kode_block_file_write_buffer(FS_VOL "/w2.bin", data, 32769U,
                                                       false));
    TEST_ASSERT_EQUAL_UINT(0U, fs_pending());
}

/* ---- The one ceiling, in both directions -----------------------------------
 * KODE_SYS_FILE_READ_MAX is what a read may hand back and what a write may accept,
 * one number for both, and the two cases at each edge of it are the only
 * thing standing between that contract and a `>` where a `>=` belongs.
 */

TEST_CASE("write accepts exactly its own ceiling", "[kode_fs]")
{
    static char at_ceiling[KODE_SYS_FILE_READ_MAX];
    kode_sys_file_status_t st;

    fs_fresh();
    (void)memset(at_ceiling, 'y', sizeof(at_ceiling));

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_block_file_write_buffer(FS_VOL "/w.txt", at_ceiling,
                                        (uint32_t)sizeof(at_ceiling), false));

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_file_status(FS_VOL "/w.txt", &st));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_SYS_FILE_READ_MAX, st.size);
}

TEST_CASE("write refuses one byte over its ceiling, posting nothing",
          "[kode_fs]")
{
    static char over[KODE_SYS_FILE_READ_MAX + 1U];

    fs_fresh();
    (void)memset(over, 'x', sizeof(over));

    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES,
                          kode_block_file_write_buffer(FS_VOL "/w.txt", over,
                                        (uint32_t)sizeof(over), false));
    TEST_ASSERT_EQUAL_UINT(0U, fs_pending());
}

TEST_CASE("read accepts a file exactly at the ceiling", "[kode_fs]")
{
    static char buf[KODE_SYS_FILE_READ_MAX];
    uint32_t    got = 0U;

    fs_fresh();
    TEST_ASSERT_TRUE(put_filler("big.txt", (size_t)KODE_SYS_FILE_READ_MAX, 'z'));

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_block_file_read_part(FS_VOL "/big.txt", 0U, buf, sizeof(buf),
                                       &got));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_SYS_FILE_READ_MAX, got);
    TEST_ASSERT_EQUAL_UINT8('z', (uint8_t)buf[KODE_SYS_FILE_READ_MAX - 1U]);
}

TEST_CASE("read reassembles more than one line with the newline in place",
          "[kode_fs]")
{
    char     buf[16];
    uint32_t got = 0U;

    fs_fresh();
    fake_media_put_fs_file("f.txt", "one\ntwo");

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_block_file_read_part(FS_VOL "/f.txt", 0U, buf, sizeof(buf),
                                       &got));
    TEST_ASSERT_EQUAL_UINT32(7U, got);
    TEST_ASSERT_EQUAL_MEMORY("one\ntwo", buf, 7U);
}

/* A run of bytes with no `\n` anywhere in it is what the console's own reader
 * cuts at KODE_FS_CAT_LINE_MAX-1 (199) before it ever reaches the wire. The
 * facade does not go that way at all - AO_Storage copies the file's bytes
 * across verbatim into the waiter - and this is the case that says so, at a
 * length the line-oriented path could not have carried. */
TEST_CASE("read carries a run far longer than a console line", "[kode_fs]")
{
    char     buf[512];
    uint32_t got = 0U;

    fs_fresh();
    TEST_ASSERT_TRUE(put_filler("long.txt", 250U, 'x'));

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_block_file_read_part(FS_VOL "/long.txt", 0U, buf, sizeof(buf),
                                       &got));
    TEST_ASSERT_EQUAL_UINT32(250U, got);
    TEST_ASSERT_EQUAL_UINT8('x', (uint8_t)buf[249]);
}

TEST_CASE("read accepts a file exactly the size of the caller's buffer",
          "[kode_fs]")
{
    char     buf[5];
    uint32_t got = 0U;

    fs_fresh();
    fake_media_put_fs_file("f.txt", "abcde");

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_block_file_read_part(FS_VOL "/f.txt", 0U, buf, sizeof(buf),
                                       &got));
    TEST_ASSERT_EQUAL_UINT32(5U, got);
}

/* ---- KODE_OK means the question was answered ------------------------------
 * A missing path is not a refusal - it is an answer, the same way a 404 is
 * still KODE_OK from kode_block_http_get_buffer(). Flattening the two would leave an app
 * unable to tell "nothing there" from "the volume will not mount".
 */

TEST_CASE("a missing path is KODE_OK with exists false, not a refusal",
          "[kode_fs]")
{
    kode_sys_file_status_t st = { true, true, 99U };

    fs_fresh();

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_file_status(FS_VOL "/nope.txt", &st));
    TEST_ASSERT_FALSE(st.exists);
}

/* ---- What is refused before anything is posted ---------------------------- */

TEST_CASE("kode_block_file_status refuses a NULL path or a NULL out, posting nothing",
          "[kode_fs]")
{
    kode_sys_file_status_t st;

    fs_fresh();

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kode_block_file_status(NULL, &st));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kode_block_file_status(FS_VOL "/f.txt", NULL));
    TEST_ASSERT_EQUAL_UINT(0U, fs_pending());
}

TEST_CASE("kode_block_file_write_buffer refuses a NULL payload with a length, posting nothing",
          "[kode_fs]")
{
    fs_fresh();

    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
                          kode_block_file_write_buffer(FS_VOL "/w.txt", NULL, 3U, false));
    TEST_ASSERT_EQUAL_UINT(0U, fs_pending());
}

/* The documented other half of that rule: NULL is legal when there is nothing
 * to point at, and it means an empty file rather than a refusal. Truncating a
 * file to nothing is the only way an app has of emptying one, so this path is
 * a feature and not a corner. */
TEST_CASE("kode_block_file_write_buffer of nothing at all empties the file", "[kode_fs]")
{
    kode_sys_file_status_t st;

    fs_fresh();
    fake_media_put_fs_file("w.txt", "something");

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_block_file_write_buffer(FS_VOL "/w.txt", NULL, 0U, false));

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_file_status(FS_VOL "/w.txt", &st));
    TEST_ASSERT_TRUE(st.exists);
    TEST_ASSERT_EQUAL_UINT32(0U, st.size);
}

/* A payload is bytes, not a C string, and the whole reason the request
 * underneath carries a pointer and a count rather than an array is that a NUL
 * in the middle of one has to reach the medium like any other byte. Written
 * and read back, because only the round trip proves it. */
TEST_CASE("a NUL inside the payload is written like any other byte",
          "[kode_fs]")
{
    static const char data[5] = { 'a', 'b', '\0', 'c', 'd' };
    char              buf[8];
    uint32_t          got = 0U;

    fs_fresh();

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_block_file_write_buffer(FS_VOL "/w.bin", data, 5U, false));

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_block_file_read_part(FS_VOL "/w.bin", 0U, buf, sizeof(buf),
                                       &got));
    TEST_ASSERT_EQUAL_UINT32(5U, got);
    TEST_ASSERT_EQUAL_MEMORY(data, buf, 5U);
}

/* ---- mkdir / remove / rename round trips ----------------------------------- */

TEST_CASE("mkdir makes a directory kode_block_file_status then sees", "[kode_fs]")
{
    kode_sys_file_status_t st;

    fs_fresh();

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_file_create_folder(FS_VOL "/logs"));

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_file_status(FS_VOL "/logs", &st));
    TEST_ASSERT_TRUE(st.exists);
    TEST_ASSERT_TRUE(st.is_dir);
}

TEST_CASE("remove deletes a file kode_block_file_status then no longer sees",
          "[kode_fs]")
{
    kode_sys_file_status_t st;

    fs_fresh();
    fake_media_put_fs_file("gone.txt", "x");

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_file_delete(FS_VOL "/gone.txt"));

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_file_status(FS_VOL "/gone.txt", &st));
    TEST_ASSERT_FALSE(st.exists);
}

/* The counterpart of mkdir, and the reason kode_block_file_delete() STATs first: FAT
 * has two verbs for deleting and only a STAT knows which one a path wants. A
 * facade that can make a directory and not unmake it is one an application
 * has to reach around. */
TEST_CASE("remove also unmakes a directory kode_block_file_create_folder made", "[kode_fs]")
{
    kode_sys_file_status_t st;

    fs_fresh();

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_file_create_folder(FS_VOL "/logs"));

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_file_delete(FS_VOL "/logs"));

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_file_status(FS_VOL "/logs", &st));
    TEST_ASSERT_FALSE(st.exists);
}

/* And it is still not an `-r`: rmdir() refuses a directory with anything in
 * it, exactly as it does at the console, so a whole tree cannot leave through
 * one call by accident. */
TEST_CASE("remove will not unmake a directory that still has something in it",
          "[kode_fs]")
{
    kode_sys_file_status_t st;

    fs_fresh();
    fake_media_put_fs_dir("logs");
    fake_media_put_fs_file("logs/a.txt", "x");

    fs_will_settle();
    TEST_ASSERT_NOT_EQUAL_INT(KODE_OK, kode_block_file_delete(FS_VOL "/logs"));

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_file_status(FS_VOL "/logs", &st));
    TEST_ASSERT_TRUE(st.exists);
    TEST_ASSERT_TRUE(st.is_dir);
}

TEST_CASE("rename moves a file within one volume", "[kode_fs]")
{
    kode_sys_file_status_t st;

    fs_fresh();
    fake_media_put_fs_file("old.txt", "hi");

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_block_file_rename_path(FS_VOL "/old.txt",
                                         FS_VOL "/new.txt"));

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_file_status(FS_VOL "/old.txt", &st));
    TEST_ASSERT_FALSE(st.exists);

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_file_status(FS_VOL "/new.txt", &st));
    TEST_ASSERT_TRUE(st.exists);
}

TEST_CASE("rename refuses a crossing between volumes", "[kode_fs]")
{
    fs_fresh();
    fake_media_set_sd_present(true);
    fake_media_put_fs_file("old.txt", "hi");

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
                          kode_block_file_rename_path(FS_VOL "/old.txt",
                                         "/sdcard/old.txt"));
}

/* ---- kode_block_file_read_part: the bytes it hands back ------------------------------------ */

TEST_CASE("read copies a file's exact bytes back out", "[kode_fs]")
{
    char     buf[32];
    uint32_t got = 99U;

    fs_fresh();
    fake_media_put_fs_file("f.txt", "hello world");

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_block_file_read_part(FS_VOL "/f.txt", 0U, buf, sizeof(buf),
                                       &got));
    TEST_ASSERT_EQUAL_UINT32(11U, got);
    TEST_ASSERT_EQUAL_MEMORY("hello world", buf, 11U);
}

/* The facade's header promises KODE_DENIED for a malformed path, and until
 * the check moved into real_path() nothing kept that promise: the console
 * normalises before it asks, but an app calls straight in, so "/storage/.."
 * reached libc as written. It is refused now for every kode_sys_file_* call at
 * once, because they all go through the same mapping. */
TEST_CASE("the facade refuses a path that is not normalised", "[kode_fs]")
{
    char           buf[8];
    kode_sys_file_status_t st;

    fs_fresh();

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
                          kode_block_file_read_part(FS_VOL "/../..", 0U, buf, sizeof(buf),
                                       NULL));

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
                          kode_block_file_status(FS_VOL "/../sdcard/x", &st));

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_block_file_create_folder(FS_VOL "/./d"));
}

/* ---- kode_block_file_list_each ----------------------------------------------------------- */

/**
 * @brief What one kode_block_file_list_each() callback run collected.
 *
 * Recorded rather than asserted from inside the callback, deliberately: the
 * callback runs on the caller's own thread while kode_block_file_list_each() still holds
 * the facade's one shared claim (see that call's own comment on why), and a
 * TEST_ASSERT_*() failing there would longjmp straight out of this test
 * function - past kode_block_file_list_each()'s own release - and leave the claim stuck
 * for every case that runs after this one. Every assertion here runs after
 * kode_block_file_list_each() has already returned instead.
 */
typedef struct {
    unsigned calls;
    bool     saw_file;
    bool     file_is_dir;
    uint32_t file_size;
    bool     saw_dir;
    bool     dir_is_dir;
} list_capture_t;

static bool list_collect(const char *name, bool is_dir, uint32_t size,
                         void *ctx)
{
    list_capture_t * const c = (list_capture_t *)ctx;

    ++c->calls;
    if ( strcmp(name, "notes.txt") == 0 )
    {
        c->saw_file    = true;
        c->file_is_dir = is_dir;
        c->file_size   = size;
    }
    else if ( strcmp(name, "logs") == 0 )
    {
        c->saw_dir    = true;
        c->dir_is_dir = is_dir;
    }
    return true;
}

TEST_CASE("list offers every entry of a directory to the callback",
          "[kode_fs]")
{
    list_capture_t cap = { 0U, false, false, 0U, false, false };

    fs_fresh();
    fake_media_put_fs_file("notes.txt", "hello");
    fake_media_put_fs_dir("logs");

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_block_file_list_each(FS_VOL, &list_collect, &cap));
    TEST_ASSERT_EQUAL_UINT(2U, cap.calls);
    TEST_ASSERT_TRUE(cap.saw_file);
    TEST_ASSERT_FALSE(cap.file_is_dir);
    TEST_ASSERT_EQUAL_UINT32(5U, cap.file_size);
    TEST_ASSERT_TRUE(cap.saw_dir);
    TEST_ASSERT_TRUE(cap.dir_is_dir);
}

/** @brief Count entries and take every one of them; @p ctx is a uint32_t. */
static bool list_count_only(const char *name, bool is_dir, uint32_t size,
                            void *ctx)
{
    uint32_t * const calls = (uint32_t *)ctx;
    (void)name;
    (void)is_dir;
    (void)size;
    ++(*calls);
    return true;
}

/** @brief Count one entry and then stop the walk; @p ctx is a uint32_t. */
static bool list_stop_after_one(const char *name, bool is_dir, uint32_t size,
                                void *ctx)
{
    uint32_t * const calls = (uint32_t *)ctx;
    (void)name;
    (void)is_dir;
    (void)size;
    ++(*calls);
    return false;
}

TEST_CASE("list stops offering entries once the callback says so",
          "[kode_fs]")
{
    uint32_t calls = 0U;

    fs_fresh();
    fake_media_put_fs_file("a.txt", "1");
    fake_media_put_fs_file("b.txt", "22");

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_block_file_list_each(FS_VOL, &list_stop_after_one, &calls));
    TEST_ASSERT_EQUAL_UINT32(1U, calls);
}

/* The truncation signal, and the reason it exists: a directory of two hundred
 * entries answering KODE_OK after twenty-four is indistinguishable from a
 * directory of exactly twenty-four. The rows that DID fit are still offered
 * first - a caller that only wanted a look gets one - and then the truth
 * about the rest is told rather than swallowed. */
TEST_CASE("list says so when there was more than it could carry", "[kode_fs]")
{
    uint32_t calls = 0U;
    char     name[32];

    fs_fresh();
    for ( unsigned i = 0U; (KODE_STORAGE_LIST_MAX + 6U) > i; ++i )
    {
        (void)snprintf(name, sizeof(name), "f%02u.txt", i);
        fake_media_put_fs_file(name, "x");
    }

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES,
                          kode_block_file_list_each(FS_VOL, &list_count_only, &calls));
    TEST_ASSERT_EQUAL_UINT32(KODE_STORAGE_LIST_MAX, calls);
}

/* And the edge under it, which is what makes the signal worth anything: a
 * directory of exactly the ceiling is not truncated and must not say it was. */
TEST_CASE("list of exactly the ceiling is not a truncation", "[kode_fs]")
{
    uint32_t calls = 0U;
    char     name[32];

    fs_fresh();
    for ( unsigned i = 0U; KODE_STORAGE_LIST_MAX > i; ++i )
    {
        (void)snprintf(name, sizeof(name), "f%02u.txt", i);
        fake_media_put_fs_file(name, "x");
    }

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_block_file_list_each(FS_VOL, &list_count_only, &calls));
    TEST_ASSERT_EQUAL_UINT32(KODE_STORAGE_LIST_MAX, calls);
}

/**
 * @brief A callback that records whether it ran, rather than failing the
 *        case from inside kode_block_file_list_each()'s own claim - see list_collect()'s
 *        comment for why a callback never asserts directly.
 */
static bool list_never_called(const char *name, bool is_dir, uint32_t size,
                              void *ctx)
{
    bool * const ran = (bool *)ctx;
    (void)name;
    (void)is_dir;
    (void)size;
    *ran = true;
    return false;
}

TEST_CASE("list refuses a path that is not there before ever asking",
          "[kode_fs]")
{
    bool ran = false;

    fs_fresh();

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
                          kode_block_file_list_each(FS_VOL "/nope", &list_never_called,
                                       &ran));
    TEST_ASSERT_FALSE_MESSAGE(ran,
        "the callback ran for a listing that should have been refused first");
}

TEST_CASE("list refuses a file the same way cd refuses one", "[kode_fs]")
{
    bool ran = false;

    fs_fresh();
    fake_media_put_fs_file("notes.txt", "hello");

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
                          kode_block_file_list_each(FS_VOL "/notes.txt",
                                       &list_never_called, &ran));
    TEST_ASSERT_FALSE_MESSAGE(ran,
        "the callback ran for a listing that should have been refused first");
}

/* A facade listing is answered into the caller's own rows and published to
 * nobody: no pool block per entry, nothing on the bus for AO_Gui or
 * AO_Console to sort out of their own listings, and no window in which an
 * operator's `ls` could interleave with an app's. The second half of this
 * case is what makes the first half mean anything - the same walk, asked for
 * by an Active Object, still arrives as STORAGE_ENTRY_SIG events. */
TEST_CASE("a facade listing puts nothing on the bus", "[kode_fs]")
{
    list_capture_t cap = { 0U, false, false, 0U, false, false };

    fs_fresh();
    fake_media_put_fs_file("notes.txt", "hello");

    ao_spy_subscribe(STORAGE_ENTRY_SIG);
    ao_spy_subscribe(STORAGE_LIST_END_SIG);
    ao_spy_drain();

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_block_file_list_each(FS_VOL, &list_collect, &cap));
    TEST_ASSERT_EQUAL_UINT(1U, cap.calls);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "a facade listing reached the bus, where an operator's own `ls` and "
        "the GUI's file scan would have to tell it apart from theirs");

    storage_path_request(ao_spy(), (uint8_t)STORAGE_PATH_LIST, FS_VOL);
    (void)qf_support_pump(AO_Storage, 8U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, ao_spy_count(),
        "the same walk asked for by an Active Object should still arrive as "
        "one entry and a terminator");

    ao_spy_drain();
    QActive_unsubscribe(ao_spy(), STORAGE_ENTRY_SIG);
    QActive_unsubscribe(ao_spy(), STORAGE_LIST_END_SIG);
}

/* The other direction of the same seam, and the one nothing tested until a
 * mutation went looking. AO_Storage carries the blocked task it is answering
 * in a file-static, s_waiter, set at the top of each request's dispatch and
 * cleared at the bottom - and the clearing is not tidiness. A console `cat`
 * is walked across several dispatch steps by a pacer, and the step that
 * finishes it calls post_cat_end() from OUTSIDE any request's dispatch, where
 * whatever s_waiter last held is what it would find. Leave a facade request's
 * waiter set and the console's terminator goes to the app instead: the app
 * gets an answer it never asked for, and the operator's `cat` never ends.
 *
 * So: a console cat mid-walk, one facade call dispatched into the middle of
 * it, and the terminator still has to come out where it was going.
 */
TEST_CASE("a facade call mid-cat does not steal the console's terminator",
          "[kode_fs]")
{
    kode_sys_file_status_t st;
    char           body[256];
    size_t         pos = 0U;

    fs_fresh();

    /* One line more than a single step carries, which is the whole point:
     * cat_step() runs its first batch inline from do_path_cat(), and a file
     * that fits in that batch never reaches the pacer at all - so a shorter
     * file would test nothing here. */
    for ( unsigned n = 0U; 25U > n; ++n )
    {
        int const written = snprintf(&body[pos], sizeof(body) - pos, "L%u\n",
                                     n);
        pos += (size_t)written;
    }
    fake_media_put_fs_file("many.txt", body);

    ao_spy_drain();

    /* An Active Object's cat, and only its first batch: twenty-four lines
     * out, the twenty-fifth and the terminator still owed to the pacer. */
    storage_cat_request(ao_spy(), FS_VOL "/many.txt", 4096U);
    (void)qf_support_pump(AO_Storage, 1U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(24U, ao_spy_count(),
        "the cat should have gone out one batch at a time, leaving the rest "
        "to the pacer");
    ao_spy_drain();

    /* A facade call landing in the middle of that walk. Its own dispatch is
     * what sets AO_Storage's waiter; what matters is what it leaves behind,
     * because the pacer's next step runs outside any request's dispatch. */
    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_file_status(FS_VOL "/many.txt", &st));
    TEST_ASSERT_TRUE(st.exists);

    /* Now let the walk finish. */
    (void)qf_support_pump(AO_Storage, 8U);
    (void)qf_support_pump(AO_Storage, 8U);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, ao_spy_count(),
        "the rest of the console's cat should have come back as one line and "
        "a terminator");

    const QEvt *e = ao_spy_take();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(STORAGE_LINE_SIG, (int)e->sig);
    TEST_ASSERT_FALSE(((const StorageLineEvt *)e)->eof);

    e = ao_spy_take();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(STORAGE_LINE_SIG, (int)e->sig);
    TEST_ASSERT_TRUE_MESSAGE(((const StorageLineEvt *)e)->eof,
        "the terminator went to the facade's waiter instead of the Active "
        "Object that asked for the cat");

    ao_spy_drain();
}

/* ---- The shared claim, across every call ------------------------------------ */

/** @brief How many calls the overlap case tries, one slot each below. */
#define OVERLAP_CALLS  8U

/** @brief What each call answered a second, overlapping caller. */
static kode_status_t s_call_rc[OVERLAP_CALLS];

/** @brief Whether the refused listing still ran its callback. */
static uint32_t s_call_list_calls;

/**
 * @brief Every call, from inside another call's own wait, then settle.
 *
 * Records only; the case reads the array back once the call that was
 * interrupted has returned and given the claim up.
 */
static void every_call_again_then_settle(void)
{
    static kode_sys_file_status_t st;
    static char           buf[16];

    s_call_rc[0] = kode_block_file_status(FS_VOL "/f.txt", &st);
    s_call_rc[1] = kode_block_file_list_each(FS_VOL, &list_stop_after_one,
                                 &s_call_list_calls);
    s_call_rc[2] = kode_block_file_create_folder(FS_VOL "/d");
    s_call_rc[3] = kode_block_file_delete(FS_VOL "/f.txt");
    s_call_rc[4] = kode_block_file_rename_path(FS_VOL "/a", FS_VOL "/b");
    s_call_rc[5] = kode_block_file_read_part(FS_VOL "/f.txt", 0U, buf, sizeof(buf), NULL);
    s_call_rc[6] = kode_block_file_write_buffer(FS_VOL "/f.txt", "x", 1U, false);
    s_call_rc[7] = kode_block_file_read_part(FS_VOL "/f.txt", 0U, buf, sizeof(buf), NULL);

    s_second_pending = fs_pending();
    (void)qf_support_pump(AO_Storage, 8U);
}

TEST_CASE("every file call is refused while another already holds the claim",
          "[kode_fs]")
{
    kode_sys_file_status_t st;

    fs_fresh();
    fake_media_put_fs_file("f.txt", "12345");

    for ( uint32_t i = 0U; OVERLAP_CALLS > i; ++i )
    {
        s_call_rc[i] = KODE_OK;
    }
    s_call_list_calls = 0U;
    s_second_pending   = 99U;

    fake_sem_wait_hook(&every_call_again_then_settle);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_file_status(FS_VOL "/f.txt", &st));

    for ( uint32_t i = 0U; OVERLAP_CALLS > i; ++i )
    {
        TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, s_call_rc[i],
            "a file call was let through while another held the claim");
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, s_call_list_calls,
                                   "the refused list still ran its callback");
    /* And none of the others posted anything: the holder's own request is the
     * only thing AO_Storage ever heard about. */
    TEST_ASSERT_EQUAL_UINT(1U, s_second_pending);

    /* The claim really was given back, so one overlapping caller does not
     * turn into a permanent lockout. */
    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_file_status(FS_VOL "/f.txt", &st));
}

/* ---- What AO_Storage never answering looks like ---------------------------- */

TEST_CASE("kode_block_file_status reports a timeout when AO_Storage never answers",
          "[kode_fs]")
{
    kode_sys_file_status_t st;

    fs_fresh();

    /* Nothing armed, so nothing ever pumps AO_Storage and the fake semaphore
     * has nothing to signal. */
    fs_no_settle();
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_file_status(FS_VOL "/f.txt", &st));

    /* The request really did reach AO_Storage - this is a system gone quiet,
     * not a refusal made up on the spot. */
    TEST_ASSERT_EQUAL_UINT(1U, fs_pending());

    /* And the claim was given back despite the timeout, so the system is not
     * wedged for every call after this one. The abandoned request is NOT
     * thrown away to prove it: on the board nothing can throw one away, and
     * the next call is supposed to wait it out - which is exactly what this
     * one does, from inside its own wait, before asking anything of its own.
     * See "a call whose predecessor is still owed an answer is refused" for
     * the other half, where the answer never comes. */
    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_file_status(FS_VOL "/f.txt", &st));
}

/* The abandoned answer, which is the whole reason a request carries a
 * generation at all. A call that gave up leaves an answer still owed; it
 * arrives later, on this same binary semaphore, into this same storage, quite
 * possibly while a LATER call is asleep on it. A token is only an invitation
 * to look - what decides is the generation AO_Storage stamped in before it
 * signalled.
 *
 * The token is planted directly, because that is what the fake semaphore's
 * one process-wide flag IS: khal_sem_signal() leaves it set for whoever waits
 * next, exactly as an abandoned answer does on the target. Nothing is armed
 * to answer the second call, so the ONLY thing that could end its wait early
 * is that stale token - and reading the previous call's own answer out of it
 * is precisely the failure this proves cannot happen. */
TEST_CASE("a token left over from an abandoned answer is not mistaken for one",
          "[kode_fs]")
{
    kode_sys_file_status_t st = { false, false, 0U };

    fs_fresh();
    fake_media_put_fs_file("f.txt", "12345");

    /* A first call that really was answered, so the facade's storage now
     * holds a KODE_OK and the generation that earned it. */
    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_file_status(FS_VOL "/f.txt", &st));
    TEST_ASSERT_TRUE(st.exists);

    fs_no_settle();
    khal_sem_signal(NULL);

    st.exists = false;
    st.size   = 0U;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        KODE_TIMEOUT, kode_block_file_status(FS_VOL "/nope.txt", &st),
        "a token this call never earned ended its wait");
    TEST_ASSERT_FALSE_MESSAGE(st.exists,
        "the previous call's answer was copied out for this one");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, st.size,
        "the previous call's answer was copied out for this one");

    /* The call that just timed out left an answer owed, and this file's rule
     * is that a case which provokes one lets AO_Storage finish before it
     * ends - see fs_claim_barrier(). Letting it land here also says the
     * facade came back on its own. */
    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_file_status(FS_VOL "/f.txt", &st));
    TEST_ASSERT_TRUE(st.exists);
}

/* ---- What the generation alone does NOT protect ----------------------------
 * The generation guards the ANSWER: a call woken by a token it did not earn
 * reads a stamp that is not its own and keeps waiting, so nothing ever copies
 * out somebody else's status. It does nothing whatever for the BUFFERS. A
 * request that was abandoned is still sitting in AO_Storage's queue holding
 * pointers into s_fs_buf and s_fs_rows, and the next caller through the door
 * would otherwise take the claim, fill that same buffer and post its own
 * request behind the abandoned one.
 *
 * That is not an exotic sequence. It is what an ordinary five-second timeout
 * on a slow card produces: a read gives up with its CAT still queued, a write
 * claims and copies its payload into the shared buffer, and AO_Storage then
 * dispatches the CAT FIRST - filling that buffer with the file the read
 * asked for - before serving the write out of the bytes the CAT just put
 * there. The write goes on to say KODE_OK having put somebody else's file on
 * the medium, which is exactly the lie the whole "nothing here truncates in
 * silence, and if a call said KODE_OK what you got is all of it" contract
 * exists to refuse.
 *
 * So the claim has to survive the timeout. Whoever comes next either waits
 * for the abandoned answer to land before touching anything, or is refused.
 */

TEST_CASE("an abandoned read cannot put its bytes in the write after it",
          "[kode_fs]")
{
    kode_sys_file_status_t st;
    char           big[64];
    char           back[16];
    uint32_t       got = 0U;

    fs_fresh();
    TEST_ASSERT_TRUE(put_filler("big.txt", sizeof(big), 'Z'));

    /* AO_Storage does not run: the read is left in its queue naming the
     * facade's own buffer, which is precisely where a deadline that ran out
     * leaves it on the board. */
    fs_no_settle();
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT,
                          kode_block_file_read_part(FS_VOL "/big.txt", 0U, big, sizeof(big),
                                                    &got));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fs_pending(),
        "the abandoned read is not really still queued, so this case is not "
        "reproducing anything");

    /* A write arriving while AO_Storage is still quiet cannot wait the debt
     * out, so it is refused - and being refused must not cancel the debt.
     * Without this middle step the case would still pass on a facade that
     * forgets what it is owed the moment it turns somebody away. */
    fs_no_settle();
    TEST_ASSERT_EQUAL_INT(
        KODE_DENIED, kode_block_file_write_buffer(FS_VOL "/w.txt", "abc", 3U, false));

    /* Now a write, with AO_Storage free to run again. It must not be served
     * out of a buffer the abandoned read is about to fill. */
    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_block_file_write_buffer(FS_VOL "/w.txt", "abc", 3U, false));

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_file_status(FS_VOL "/w.txt", &st));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3U, st.size,
        "the write put a different number of bytes on the medium than it "
        "was handed");

    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_block_file_read_part(FS_VOL "/w.txt", 0U, back, sizeof(back),
                                       &got));
    TEST_ASSERT_EQUAL_UINT32(3U, got);
    TEST_ASSERT_EQUAL_INT8_ARRAY_MESSAGE(
        "abc", back, 3,
        "the write said KODE_OK having put the abandoned read's bytes on the "
        "medium instead of its own");
}

TEST_CASE("a call whose predecessor is still owed an answer is refused",
          "[kode_fs]")
{
    kode_sys_file_status_t st;

    fs_fresh();
    fake_media_put_fs_file("f.txt", "12345");

    /* One abandoned request, still queued. */
    fs_no_settle();
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_file_status(FS_VOL "/f.txt", &st));
    TEST_ASSERT_EQUAL_UINT(1U, fs_pending());

    /* A write arriving while AO_Storage stays quiet. It cannot wait the
     * abandoned answer out, so it gives up rather than lend the same buffer
     * twice - and it gives up having posted nothing, which is what the queue
     * depth says. */
    fs_no_settle();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        KODE_DENIED, kode_block_file_write_buffer(FS_VOL "/w.txt", "abc", 3U, false),
        "a caller was served while AO_Storage still held a pointer into the "
        "buffer it was about to fill");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fs_pending(),
        "the refused caller posted its request anyway");

    /* And the moment the abandoned answer lands, the facade is working
     * again: this is a barrier, not a wedge. */
    fs_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_block_file_write_buffer(FS_VOL "/w.txt", "abc", 3U, false));
}
