/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_storage_ao.c
 * @brief AO_Storage: the one file at a time, and who is allowed to close it.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "qpc.h"

#include "ao_spy.h"
#include "ask_support.h"
#include "fake_khal_worker.h"
#include "fakes.h"
#include "kaos.h"
#include "kapp_data.h"
#include "khal_system.h"
#include "kbsp_owners.h"
#include "kdrv_sd.h"
#include "kdrv_storage.h"
#include "kode_sys.h"
#include "kqp.h"
#include "kqp_worker.h"
#include "lowpower_cases.h"
#include "qf_support.h"

/** @brief Put AO_Storage back to a freshly-booted state for each case. */
/** @brief 96 KB: more than one 32 KB chunk, so a write stays in flight. */
#define BIG_PAYLOAD  (96U * 1024U)
static uint8_t s_big[BIG_PAYLOAD];

static void storage_fresh(void)
{
    /* The driver keeps one handle for the whole system and the constructor
     * below only clears the AO's view of it. A file left open by the previous
     * case would make the next open refuse for the wrong reason. */
    kdrv_storage_close();

    qf_support_init();
    (void)ao_spy();
    ao_spy_drain();

    /* The case before may have ended on a call's return, which disarms its
     * deadline and leaves the node linked until a tick; the constructor below
     * would cut QF's timer list there. */
    qf_support_reap_timers();
    Storage_ctor();
    qf_support_give_queue(AO_Storage);
    AO_Storage->prio  = KODE_PRIO_STORAGE;
    AO_Storage->pthre = KODE_PRIO_STORAGE;

    qf_support_register(AO_Storage);
    QASM_INIT(AO_Storage, (void *)0, 0U);
}

/** @brief The name AO_Storage gives its card worker. */
#define SD_WORKER  "storage_sd"

/**
 * @brief Run the card worker's oldest job on the case's thread, and let the
 *        AO read its answer.
 * @return true when the worker had a job.
 */
static bool run_sd_job(void)
{
    bool const ran = fake_khal_worker_run_next_for(SD_WORKER);

    (void)qf_support_settle(AO_Storage);
    return ran;
}

/**
 * @brief Let AO_Storage and its card worker finish whatever they hold.
 *
 * A request that waits on the card is a job, and its answer a done event, so
 * a case that asks for one lets both run before it looks at the result.
 */
static void settle_card(void)
{
    for ( unsigned i = 0U; 64U > i; ++i )
    {
        unsigned n = qf_support_pump(AO_Storage, 8U);

        if ( fake_khal_worker_run_next_for(SD_WORKER) )
        {
            ++n;
        }
        if ( 0U == n )
        {
            break;
        }
    }
}

/** @brief Put @p bytes of content in @p name on the internal partition. */
static void make_file(const char *name, size_t bytes)
{
    char  path[512];
    FILE *f;

    (void)snprintf(path, sizeof(path), "%s/%s", fake_media_fs_dir(), name);
    f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    for ( size_t i = 0U; i < bytes; ++i )
    {
        (void)fputc('x', f);
    }
    (void)fclose(f);
}

/** @brief Dispatch an open request for @p name, from the spy. */
static void ask_open(const char *name)
{
    StorageOpenEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(STORAGE_OPEN_REQ_SIG);
    (void)snprintf(e.name, sizeof(e.name), "%s", name);
    e.who = ao_spy();

    QASM_DISPATCH(AO_Storage, &e.super, 0U);
}

/** @brief Dispatch a whole-payload write request, from the spy. */
static void ask_write(const char *name, const void *data, uint32_t len)
{
    StorageWriteEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(STORAGE_WRITE_REQ_SIG);
    (void)snprintf(e.name, sizeof(e.name), "%s", name);
    e.data = data;
    e.len  = len;
    e.who  = ao_spy();

    QASM_DISPATCH(AO_Storage, &e.super, 0U);
}

/**
 * @brief Drive a write in flight to completion, so its pacer is disarmed.
 *
 * A write longer than one chunk arms a QTimeEvt between pieces, and nothing
 * ticks in this tier. Leaving it armed would hand the next case's constructor
 * a timer that is still linked into QF's list.
 */
static void drain_write(void)
{
    for ( unsigned i = 0U; 64U > i; ++i )
    {
        if ( qf_support_pump(AO_Storage, 8U) == 0U )
        {
            break;
        }
    }
}

/**
 * @brief Take the next answer and assert it is an open response.
 * @return Its status, as a kode_status_t.
 */
static kode_status_t take_open_status(void)
{
    const QEvt * const e = ao_spy_take();

    TEST_ASSERT_NOT_NULL_MESSAGE(e, "the request went unanswered");
    TEST_ASSERT_EQUAL_INT(STORAGE_OPEN_RSP_SIG, (int)e->sig);
    return (kode_status_t)((const StorageOpenRspEvt *)e)->status;
}

/* ---- Bring-up -------------------------------------------------------------- */

TEST_CASE("AO_Storage claims both media before it touches either",
          "[storage_ao]")
{
    storage_fresh();

    TEST_ASSERT_EQUAL_STRING("AO_Storage", kbsp_owner_name(KBSP_RES_SDMMC));
    TEST_ASSERT_EQUAL_STRING("AO_Storage", kbsp_owner_name(KBSP_RES_FATFS));
}

/* ---- Opening --------------------------------------------------------------- */

TEST_CASE("an open is answered with the file's size", "[storage_ao]")
{
    storage_fresh();
    make_file("hello.bin", 128U);

    ask_open("hello.bin");

    const QEvt * const e = ao_spy_take();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(STORAGE_OPEN_RSP_SIG, (int)e->sig);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        (int)((const StorageOpenRspEvt *)e)->status);
    TEST_ASSERT_EQUAL_UINT32(128U, ((const StorageOpenRspEvt *)e)->size);
}

/* A file that is nowhere is still an answer. The console closes its command
 * on the response, so a silent refusal would leave it waiting out its own
 * deadline for something that was never coming. */
TEST_CASE("a file that is nowhere is refused, and still answered",
          "[storage_ao]")
{
    storage_fresh();

    ask_open("absent.bin");

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, take_open_status());
}

TEST_CASE("a second open while one is held is refused, and answered",
          "[storage_ao]")
{
    storage_fresh();
    make_file("first.bin", 16U);
    make_file("second.bin", 16U);

    ask_open("first.bin");
    TEST_ASSERT_EQUAL_INT(KODE_OK, take_open_status());

    ask_open("second.bin");
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, take_open_status());
}

/* ---- The audit's first fix: an open during a write --------------------------- */

/* kdrv_storage keeps one handle for the whole system, and a write session
 * holds it. This path used to look only at the reader, so an open arriving
 * mid-write walked straight into the write's handle and took it - and the
 * write, still believing itself open, carried on into somebody else's file.
 *
 * write_begin() and the manifest read had always asked both halves of the
 * question. This one now does too. */
/** @brief More blocks than the host harness's small pool holds. */
#define POOL_HOLD_MAX 64U

/* An open whose answer the pool could not carry used to keep the reader for a
 * client that never heard it was given one: every later open was refused as
 * busy, and AO_Radio, which has no deadline of its own, waited for good. The
 * answer now goes out anyway, as a static NO_RESOURCES, and the open is
 * undone. */
TEST_CASE("an open the pool cannot answer is undone, and still answered",
          "[storage_ao]")
{
    const QEvt *held[POOL_HOLD_MAX];
    size_t      n = 0U;

    storage_fresh();
    make_file("target.bin", 8U);

    while ( POOL_HOLD_MAX > n )
    {
        StorageOpenRspEvt * const e =
            Q_NEW_X(StorageOpenRspEvt, 0U, STORAGE_OPEN_RSP_SIG);
        if ( NULL == e )
        {
            break;
        }
        held[n] = &e->super;
        ++n;
    }
    ask_open("target.bin");
    for ( size_t i = 0U; i < n; ++i )
    {
        QF_gc(held[i]);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_NO_RESOURCES, take_open_status(),
                                  "a dry pool must still be answered");

    ask_open("target.bin");
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, take_open_status(),
                                  "the unanswered open kept the reader");

    static const QEvt close_evt = QEVT_INITIALIZER(STORAGE_CLOSE_REQ_SIG);
    QASM_DISPATCH(AO_Storage, &close_evt, 0U);
}

TEST_CASE("an open arriving during a write is refused, not allowed to steal "
          "the handle", "[storage_ao]")
{
    storage_fresh();
    make_file("target.bin", 8U);

    /* Longer than one chunk, so the session is genuinely still open when the
     * open below arrives - the AO paces itself between pieces. */
    ask_write("outgoing.bin", s_big, sizeof(s_big));
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());   /* not finished */

    ask_open("target.bin");
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, take_open_status());

    drain_write();
}

/* ---- The audit's second fix: closing somebody else's transfer ---------------- */

/* A close carries no `who`, so any AO can post one. It used to close whatever
 * handle was open: with a write in flight that shut the write's file while the
 * session and its pacer stayed live, and the next chunk went into a closed
 * handle. The guard is the only thing standing between one AO and another's
 * transfer. */
TEST_CASE("a close does not end a write that is in flight", "[storage_ao]")
{
    static const QEvt close_evt = QEVT_INITIALIZER(STORAGE_CLOSE_REQ_SIG);

    storage_fresh();
    make_file("target.bin", 8U);

    ask_write("outgoing.bin", s_big, sizeof(s_big));
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    /* Somebody else's close arrives mid-transfer. */
    QASM_DISPATCH(AO_Storage, &close_evt, 0U);

    /* Where it lands is the tell, not whether it finishes. A close that went
     * through shuts the file while the session and its pacer stay live, the
     * next chunk fails - and the AO's own retry then quietly starts the whole
     * transfer again on the *other* medium. So the byte count still comes back
     * whole and only the destination gives it away: the file ends up somewhere
     * nobody asked for, and the half-written one on the first medium is
     * discarded. That is a worse failure than a short write, because nothing
     * about it looks like a failure. */
    drain_write();

    const QEvt * const rsp = ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(rsp, "the write never reported back");
    TEST_ASSERT_EQUAL_INT(STORAGE_WRITE_RSP_SIG, (int)rsp->sig);

    const StorageWriteRspEvt * const w = (const StorageWriteRspEvt *)rsp;
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)w->status);
    TEST_ASSERT_EQUAL_UINT32(sizeof(s_big), w->written);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_MEDIUM_FS, (int)w->medium,
                                  "the close forced the transfer onto the "
                                  "other medium");
}

/* A cat opens the reader, reads and closes in one step, and the close it
 * calls ends the writer too. With a write in flight it used to go ahead - only
 * another cat stopped it - and the write lost its file mid-transfer. */
TEST_CASE("a cat does not end a write that is in flight", "[storage_ao]")
{
    storage_fresh();
    make_file("target.bin", 8U);

    ask_write("outgoing.bin", s_big, sizeof(s_big));
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    storage_cat_request(ao_spy(), "/storage/target.bin", 4096U);
    (void)qf_support_pump(AO_Storage, 1U);

    const QEvt * const cat = ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(cat, "the cat went unanswered");
    TEST_ASSERT_EQUAL_INT(STORAGE_LINE_SIG, (int)cat->sig);
    const StorageLineEvt * const line = (const StorageLineEvt *)cat;
    TEST_ASSERT_TRUE_MESSAGE(line->eof, "the cat read the file anyway");
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)line->rc);

    /* The same tell as the close case above: a write that lost its file is
     * retried on the other medium, so only the destination gives it away. */
    drain_write();

    const QEvt * const rsp = ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(rsp, "the write never reported back");
    TEST_ASSERT_EQUAL_INT(STORAGE_WRITE_RSP_SIG, (int)rsp->sig);

    const StorageWriteRspEvt * const w = (const StorageWriteRspEvt *)rsp;
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)w->status);
    TEST_ASSERT_EQUAL_UINT32(sizeof(s_big), w->written);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_MEDIUM_FS, (int)w->medium,
                                  "the cat forced the transfer onto the "
                                  "other medium");
}

/* A close with nothing in flight is the ordinary path and must still work,
 * or the guard would have traded one bug for another. */
TEST_CASE("a close ends a read, which is what it is for", "[storage_ao]")
{
    static const QEvt close_evt = QEVT_INITIALIZER(STORAGE_CLOSE_REQ_SIG);

    storage_fresh();
    make_file("one.bin", 16U);
    make_file("two.bin", 16U);

    ask_open("one.bin");
    TEST_ASSERT_EQUAL_INT(KODE_OK, take_open_status());

    QASM_DISPATCH(AO_Storage, &close_evt, 0U);

    /* The handle is free again. */
    ask_open("two.bin");
    TEST_ASSERT_EQUAL_INT(KODE_OK, take_open_status());
}

/* ---- Writing --------------------------------------------------------------- */

/* A write with nothing to write is refused rather than creating an empty
 * file, and it is answered - the caller is waiting on the response. */
TEST_CASE("a write of nothing is refused, and answered", "[storage_ao]")
{
    storage_fresh();

    ask_write("empty.bin", NULL, 0U);

    const QEvt * const e = ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "the write went unanswered");
    TEST_ASSERT_EQUAL_INT(STORAGE_WRITE_RSP_SIG, (int)e->sig);
}

/* A write arriving while a file is open for reading is refused for the same
 * reason an open during a write is: one handle. */
TEST_CASE("a write while a file is open for reading is refused",
          "[storage_ao]")
{
    static const uint8_t payload[8] = { 0U };

    storage_fresh();
    make_file("held.bin", 16U);

    ask_open("held.bin");
    TEST_ASSERT_EQUAL_INT(KODE_OK, take_open_status());

    ask_write("outgoing.bin", payload, sizeof(payload));

    const QEvt * const e = ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "the write went unanswered");
    TEST_ASSERT_EQUAL_INT(STORAGE_WRITE_RSP_SIG, (int)e->sig);
}

/* ---- The audit's third fix: `write` during a read must not steal it -------- */

/* answer_text_write() - the `write` verb - never checked busy() until now. It
 * never needed to while kdrv kept one handle for the whole system, because
 * kdrv_storage_create_path() refused on its own whenever a read was open; the
 * handle split removed that refusal along with the accident it was, and this
 * was the one opener in the file that never asked busy() for itself, because
 * it never had to. A console `write` arriving while AO_AppMgr holds a read
 * open across many dispatch steps - a GUI-driven `app install`, say - must
 * still be refused, and the install's own read must still be exactly as
 * usable afterwards as if the write had never arrived. */
TEST_CASE("a text write while a file is open for reading is refused, and "
          "the read still answers", "[storage_ao]")
{
    StorageTextEvt e;

    storage_fresh();
    make_file("held.bin", 16U);

    ask_open("held.bin");
    TEST_ASSERT_EQUAL_INT(KODE_OK, take_open_status());

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(STORAGE_TEXT_REQ_SIG);
    (void)snprintf(e.path, sizeof(e.path), "%s", "/storage/outgoing.txt");
    (void)snprintf(e.text, sizeof(e.text), "%s", "hi");
    e.len    = 2U;
    e.append = false;
    e.who    = ao_spy();
    QASM_DISPATCH(AO_Storage, &e.super, 0U);

    const QEvt * const wrsp = ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(wrsp, "the write went unanswered");
    TEST_ASSERT_EQUAL_INT(STORAGE_PATH_RSP_SIG, (int)wrsp->sig);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        (int)((const StoragePathRspEvt *)wrsp)->rc);

    /* Refused before kdrv was ever asked, so nothing was created at all -
     * not even the stub a failed create would otherwise leave. */
    TEST_ASSERT_FALSE(
        fake_media_file_exists(fake_media_fs_dir(), "outgoing.txt"));

    /* And the read the write did not touch still works, exactly as it would
     * have if the write had never arrived. */
    static const QEvt read_evt = QEVT_INITIALIZER(STORAGE_READ_REQ_SIG);
    QASM_DISPATCH(AO_Storage, &read_evt, 0U);

    const QEvt * const rrsp = ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(rrsp, "the read went unanswered");
    TEST_ASSERT_EQUAL_INT(STORAGE_DATA_SIG, (int)rrsp->sig);
    const StorageDataEvt * const d = (const StorageDataEvt *)rrsp;
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)d->status);
    TEST_ASSERT_FALSE(d->eof);
    TEST_ASSERT_EQUAL_UINT16(16U, d->len);
}

/* ---- The audit's fourth fix: a copy with nobody to answer ------------------ */

/* storage_pair_request()'s own contract says NULL is served and not answered
 * for MOVE - one rename() and nothing to leak - but COPY's `who` is not just
 * where the answer goes: it is the only flag (busy(), copy_step()'s own
 * stale-tick guard) that says a copy is still running. Serving a COPY with
 * no owner would leave that flag permanently false for a transfer that goes
 * on holding two real kdrv handles and a 32 KB buffer for the rest of the
 * session. Nothing in the tree passes NULL here today - console_fs_pair()
 * always names a real AO - but the header promised it for every op, so this
 * pins the refusal rather than trusting that nobody ever will. */
TEST_CASE("a copy with nobody to answer is refused before it opens anything",
          "[storage_ao]")
{
    StoragePairEvt e;

    storage_fresh();
    make_file("src.bin", 8U);

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(STORAGE_PAIR_REQ_SIG);
    e.op    = (uint8_t)STORAGE_PAIR_COPY;
    (void)snprintf(e.src, sizeof(e.src), "%s", "/storage/src.bin");
    (void)snprintf(e.dst, sizeof(e.dst), "%s", "/storage/dst.bin");
    e.who   = NULL;
    QASM_DISPATCH(AO_Storage, &e.super, 0U);

    /* There was nobody to answer, so nothing was posted - and, more to the
     * point, nothing was left holding the source either: an ordinary open
     * right afterwards still succeeds. Refused, a copy that never started
     * cannot have left kdrv_storage_open_path()'s handle behind; a copy that
     * silently began and then stuck on its own stale-tick guard would leave
     * '/storage/src.bin' open for reading and this open would come back
     * KODE_DENIED instead. */
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    ask_open("src.bin");
    TEST_ASSERT_EQUAL_INT(KODE_OK, take_open_status());
}

/* ---- The audit's fifth fix: a transfer in flight must not lose the ground
 *      underneath it --------------------------------------------------------
 *
 * busy() already refused open/write/copy while something else held the
 * media; it said nothing about the operations that can pull the ground out
 * from under whichever of those IS running - unmounting the very card a cp
 * is reading from, deleting or renaming the file a read has open, rmdir'ing
 * a directory mid-walk, or selftesting a partition a write is still landing
 * on. Same guard, the sites it had not reached yet. */

TEST_CASE("rm is refused while a transfer is in flight, and the file "
          "survives", "[storage_ao]")
{
    storage_fresh();
    make_file("held.bin", 16U);
    make_file("target.bin", 8U);

    ask_open("held.bin");
    TEST_ASSERT_EQUAL_INT(KODE_OK, take_open_status());

    storage_path_request(ao_spy(), (uint8_t)STORAGE_PATH_UNLINK,
                         "/storage/target.bin");
    (void)qf_support_pump(AO_Storage, 8U);

    const QEvt * const e = ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "the rm went unanswered");
    TEST_ASSERT_EQUAL_INT(STORAGE_PATH_RSP_SIG, (int)e->sig);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        (int)((const StoragePathRspEvt *)e)->rc);
    TEST_ASSERT_TRUE_MESSAGE(
        fake_media_file_exists(fake_media_fs_dir(), "target.bin"),
        "rm ran anyway");
}

TEST_CASE("rmdir is refused while a transfer is in flight", "[storage_ao]")
{
    storage_fresh();
    make_file("held.bin", 16U);

    ask_open("held.bin");
    TEST_ASSERT_EQUAL_INT(KODE_OK, take_open_status());

    storage_path_request(ao_spy(), (uint8_t)STORAGE_PATH_RMDIR,
                         "/storage/somedir");
    (void)qf_support_pump(AO_Storage, 8U);

    const QEvt * const e = ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "the rmdir went unanswered");
    TEST_ASSERT_EQUAL_INT(STORAGE_PATH_RSP_SIG, (int)e->sig);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        (int)((const StoragePathRspEvt *)e)->rc);
}

TEST_CASE("mv is refused while a transfer is in flight, and the source "
          "stays put", "[storage_ao]")
{
    storage_fresh();
    make_file("held.bin", 16U);
    make_file("src.bin", 8U);

    ask_open("held.bin");
    TEST_ASSERT_EQUAL_INT(KODE_OK, take_open_status());

    storage_pair_request(ao_spy(), (uint8_t)STORAGE_PAIR_MOVE,
                         "/storage/src.bin", "/storage/dst.bin");
    (void)qf_support_pump(AO_Storage, 8U);

    const QEvt * const e = ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "the mv went unanswered");
    TEST_ASSERT_EQUAL_INT(STORAGE_PATH_RSP_SIG, (int)e->sig);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        (int)((const StoragePathRspEvt *)e)->rc);
    TEST_ASSERT_TRUE_MESSAGE(
        fake_media_file_exists(fake_media_fs_dir(), "src.bin"),
        "mv ran anyway");
}

/* A rescan is the case this exists for: storage_list_request()'s own
 * unmount+mount pair is fire-and-forget, and nothing answers it - so the
 * refusal can only be seen in the log, and the thing that must NOT happen is
 * the listing behind it going silent too. Both are posted here in the same
 * order storage_list_request() uses, ahead of a listing, to prove the second
 * half of that pair keeps working when the first half is refused. */
TEST_CASE("unmount is refused while a transfer is in flight, and the "
          "listing behind it still ends", "[storage_ao]")
{
    storage_fresh();
    make_file("held.bin", 16U);

    ask_open("held.bin");
    TEST_ASSERT_EQUAL_INT(KODE_OK, take_open_status());

    ao_spy_subscribe(STORAGE_LIST_END_SIG);
    fake_klog_reset();

    storage_request(KDRV_MEDIUM_FS, STORAGE_OP_UNMOUNT);
    storage_request(KDRV_MEDIUM_FS, STORAGE_OP_MOUNT);
    storage_list_one(KDRV_MEDIUM_FS, true, true);
    (void)qf_support_pump(AO_Storage, 8U);

    /* Not fake_klog_last(): the mount and the listing behind the refusal
     * both log lines of their own, so the refusal is not the last line by
     * the time this checks - it has to be found among all of them. */
    bool found = false;
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        if ( strstr(fake_klog_line(i), "unmount is refused") != NULL )
        {
            found = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found, "the refusal was not logged");

    const QEvt * const e = ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(e,
        "the rescan's listing never ended - a GUI waiting on it would spin "
        "forever");
    TEST_ASSERT_EQUAL_INT(STORAGE_LIST_END_SIG, (int)e->sig);
}

/* Every entry of a listing carries its place in it, and the end marker says
 * whether another page follows. A one-medium listing is always one page. */
TEST_CASE("a listing numbers its entries and ends with no more", "[storage_ao]")
{
    storage_fresh();
    make_file("a.bin", 1U);
    make_file("b.bin", 1U);
    make_file("c.bin", 1U);
    ao_spy_subscribe(STORAGE_ENTRY_SIG);
    ao_spy_subscribe(STORAGE_LIST_END_SIG);

    storage_list_one(KDRV_MEDIUM_FS, true, true);
    (void)qf_support_pump(AO_Storage, 8U);

    for ( uint16_t i = 0U; 3U > i; ++i )
    {
        StorageEntryEvt const * const en = (StorageEntryEvt const *)ao_spy_take();
        TEST_ASSERT_NOT_NULL(en);
        TEST_ASSERT_EQUAL_INT(STORAGE_ENTRY_SIG, (int)en->super.sig);
        TEST_ASSERT_EQUAL_UINT16(i, en->pos);
    }
    ListEndEvt const * const end = (ListEndEvt const *)ao_spy_take();
    TEST_ASSERT_NOT_NULL(end);
    TEST_ASSERT_EQUAL_INT(STORAGE_LIST_END_SIG, (int)end->super.sig);
    TEST_ASSERT_FALSE(end->more);
}

/* ---- The bootable images, in pages -------------------------------------- */

/** @brief The most pages any case below may take before it calls it a loop. */
#define PAGES_MAX  10U

/** @brief Put a one-byte file called @p name on the card. */
static void make_sd_file(const char *name)
{
    char  path[512];
    FILE *f;

    (void)snprintf(path, sizeof(path), "%s/%s", fake_media_sd_dir(), name);
    f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    (void)fputc('x', f);
    (void)fclose(f);
}

/** @brief What read_pages() saw, for the cases to judge. */
typedef struct {
    unsigned total;              /**< Entries over every page.            */
    unsigned pages;              /**< Pages asked.                        */
    unsigned size[PAGES_MAX];    /**< Entries in each page.               */
    unsigned from_sd[PAGES_MAX]; /**< Of those, how many from the card.   */
    char     name[64][KODE_APP_FILE_MAX]; /**< Every entry, by position.  */
} pages_t;

/**
 * @brief Read the bootable images page by page, the way an asker does.
 *
 * The spy asks and subscribes to nothing: a page is posted to its asker
 * alone, so everything that arrives here came by that post. Every entry
 * must arrive at the position that follows the last one, and every page but
 * the last must say that more follows.
 */
static void read_pages(pages_t *p)
{
    (void)memset(p, 0, sizeof(*p));

    for ( ;; )
    {
        TEST_ASSERT_TRUE_MESSAGE(PAGES_MAX > p->pages, "the pages do not end");
        ao_spy_drain();
        storage_list_page((uint16_t)p->total, ao_spy());
        (void)qf_support_pump(AO_Storage, 8U);

        bool more = false;
        for ( ;; )
        {
            const QEvt * const e = ao_spy_take();
            TEST_ASSERT_NOT_NULL_MESSAGE(e, "the page never ended");
            if ( (QSignal)STORAGE_LIST_END_SIG == e->sig )
            {
                more = ((ListEndEvt const *)e)->more;
                break;
            }
            StorageEntryEvt const * const en = (StorageEntryEvt const *)e;
            TEST_ASSERT_EQUAL_UINT16((uint16_t)p->total, en->pos);
            TEST_ASSERT_TRUE(64U > p->total);
            (void)snprintf(p->name[p->total], KODE_APP_FILE_MAX, "%s", en->name);
            ++p->total;
            ++p->size[p->pages];
            if ( en->from_sd )
            {
                ++p->from_sd[p->pages];
            }
        }
        ++p->pages;
        if ( !more )
        {
            break;
        }
    }
    ao_spy_drain();
}

/** @brief Whether @p name came exactly once. */
static unsigned seen_times(pages_t const *p, const char *name)
{
    unsigned n = 0U;
    for ( unsigned i = 0U; i < p->total; ++i )
    {
        if ( strcmp(p->name[i], name) == 0 )
        {
            ++n;
        }
    }
    return n;
}

TEST_CASE("no images is one empty page", "[storage_ao][pages]")
{
    static pages_t p;

    storage_fresh();
    read_pages(&p);
    TEST_ASSERT_EQUAL_UINT(0U, p.total);
    TEST_ASSERT_EQUAL_UINT(1U, p.pages);
}

TEST_CASE("exactly a page of images is one page", "[storage_ao][pages]")
{
    static pages_t p;
    char name[16];

    storage_fresh();
    for ( unsigned i = 0U; KODE_STORAGE_LIST_MAX > i; ++i )
    {
        (void)snprintf(name, sizeof(name), "app%03u.bin", i);
        make_sd_file(name);
    }
    read_pages(&p);
    TEST_ASSERT_EQUAL_UINT(KODE_STORAGE_LIST_MAX, p.total);
    TEST_ASSERT_EQUAL_UINT(1U, p.pages);
}

TEST_CASE("one image past a page is a second page", "[storage_ao][pages]")
{
    static pages_t p;
    char name[16];

    storage_fresh();
    for ( unsigned i = 0U; (KODE_STORAGE_LIST_MAX + 1U) > i; ++i )
    {
        (void)snprintf(name, sizeof(name), "app%03u.bin", i);
        make_sd_file(name);
    }
    read_pages(&p);
    TEST_ASSERT_EQUAL_UINT(KODE_STORAGE_LIST_MAX + 1U, p.total);
    TEST_ASSERT_EQUAL_UINT(2U, p.pages);
    TEST_ASSERT_EQUAL_UINT(KODE_STORAGE_LIST_MAX, p.size[0]);
    TEST_ASSERT_EQUAL_UINT(1U, p.size[1]);
}

/* Luismi's case (kode-dot-os 5a257b6), ported: no image twice, none missing. */
TEST_CASE("the card's images come in pages past the ceiling",
          "[storage_ao][pages]")
{
    static pages_t p;
    unsigned const total = (2U * KODE_STORAGE_LIST_MAX) + 7U;
    char name[16];

    storage_fresh();
    for ( unsigned i = 0U; i < total; ++i )
    {
        (void)snprintf(name, sizeof(name), "app%03u.bin", i);
        make_sd_file(name);
    }
    read_pages(&p);
    TEST_ASSERT_EQUAL_UINT(total, p.total);
    TEST_ASSERT_EQUAL_UINT(3U, p.pages);
    for ( unsigned i = 0U; i < total; ++i )
    {
        (void)snprintf(name, sizeof(name), "app%03u.bin", i);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, seen_times(&p, name), name);
    }
}

TEST_CASE("the card and the internal partition are one listing",
          "[storage_ao][pages]")
{
    static pages_t p;
    char name[16];

    storage_fresh();
    for ( unsigned i = 0U; 20U > i; ++i )
    {
        (void)snprintf(name, sizeof(name), "sd%02u.bin", i);
        make_sd_file(name);
    }
    for ( unsigned i = 0U; 10U > i; ++i )
    {
        (void)snprintf(name, sizeof(name), "fs%02u.bin", i);
        make_file(name, 1U);
    }
    read_pages(&p);
    TEST_ASSERT_EQUAL_UINT(30U, p.total);
    TEST_ASSERT_EQUAL_UINT(2U, p.pages);
    TEST_ASSERT_EQUAL_UINT(KODE_STORAGE_LIST_MAX, p.size[0]);
    TEST_ASSERT_EQUAL_UINT(20U, p.from_sd[0]);
    TEST_ASSERT_EQUAL_UINT(6U, p.size[1]);
    TEST_ASSERT_EQUAL_UINT(0U, p.from_sd[1]);
    for ( unsigned i = 0U; 10U > i; ++i )
    {
        (void)snprintf(name, sizeof(name), "fs%02u.bin", i);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, seen_times(&p, name), name);
    }
}

TEST_CASE("a name too long for the event is skipped before it is counted",
          "[storage_ao][pages]")
{
    static pages_t p;
    char name[KODE_APP_FILE_MAX + 8U];

    storage_fresh();
    for ( unsigned i = 0U; KODE_STORAGE_LIST_MAX > i; ++i )
    {
        (void)snprintf(name, sizeof(name), "app%03u.bin", i);
        make_sd_file(name);
    }
    (void)memset(name, 'x', KODE_APP_FILE_MAX);
    (void)memcpy(&name[KODE_APP_FILE_MAX - 4U], ".bin", 5U);
    make_sd_file(name);

    read_pages(&p);
    TEST_ASSERT_EQUAL_UINT(KODE_STORAGE_LIST_MAX, p.total);
    TEST_ASSERT_EQUAL_UINT(1U, p.pages);
}

TEST_CASE("a page past the first does not remount the card",
          "[storage_ao][pages]")
{
    storage_fresh();
    fake_klog_reset();

    storage_list_page(KODE_STORAGE_LIST_MAX, ao_spy());
    (void)qf_support_pump(AO_Storage, 8U);

    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        TEST_ASSERT_NULL_MESSAGE(strstr(fake_klog_line(i), "mounted"),
                                 fake_klog_line(i));
    }
    const QEvt * const e = ao_spy_take();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(STORAGE_LIST_END_SIG, (int)e->sig);
    ao_spy_drain();
}

TEST_CASE("the fs selftest is refused while a transfer is in flight",
          "[storage_ao]")
{
    storage_fresh();
    make_file("held.bin", 16U);

    ask_open("held.bin");
    TEST_ASSERT_EQUAL_INT(KODE_OK, take_open_status());

    ao_spy_subscribe(TEST_RESULT_SIG);
    ao_spy_drain();
    fake_klog_reset();

    storage_request(KDRV_MEDIUM_FS, STORAGE_OP_SELFTEST);
    (void)qf_support_pump(AO_Storage, 8U);

    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(fake_klog_last(), "selftest is refused"),
        "the refusal was not logged");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "a verdict was published for a selftest that should have been "
        "refused");
}

/* ---- Reading with nothing open ---------------------------------------------- */

/* A read request that arrives after the file was closed is a straggler from a
 * transfer that has already ended. It must be dropped rather than acted on -
 * and it must not answer, because there is no longer anybody waiting. */
TEST_CASE("a read with no file open is dropped", "[storage_ao]")
{
    static const QEvt read_evt = QEVT_INITIALIZER(STORAGE_READ_REQ_SIG);

    storage_fresh();

    QASM_DISPATCH(AO_Storage, &read_evt, 0U);

    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
}

/* ---- Manifests --------------------------------------------------------------- */

/* The manifest read is deliberately not a transfer: it opens, reads the front
 * of the file and closes inside one step, without taking the reader slot. An
 * open straight afterwards has to succeed, or the launcher's scan would lock
 * out the very file it is scanning. */
TEST_CASE("reading a manifest does not take the reader slot", "[storage_ao]")
{
    StorageDescReqEvt e;

    storage_fresh();
    make_file("app.bin", 512U);

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(STORAGE_DESC_REQ_SIG);
    (void)snprintf(e.name, sizeof(e.name), "%s", "app.bin");
    e.who = ao_spy();

    QASM_DISPATCH(AO_Storage, &e.super, 0U);

    /* It answered. */
    const QEvt * const rsp = ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(rsp, "the manifest request went unanswered");
    TEST_ASSERT_EQUAL_INT(STORAGE_DESC_RSP_SIG, (int)rsp->sig);

    /* And it left the slot free. */
    ask_open("app.bin");
    TEST_ASSERT_EQUAL_INT(KODE_OK, take_open_status());
}

/* ---- The app data folder ----------------------------------------------------- */

/* https://docs.kode.diy/sdk/kdrv/apps: one fields event, then zero or three
 * icon events, then the end marker - to whoever asked, never on the bus. The
 * statuses are the whole point of the reply: every case below reads one, and
 * the conversation has to close even when there is nothing to say. */

/** @brief Post APPDATA_REQ_SIG for @p project with the spy as the asker. */
static void ask_appdata(const char *project)
{
    static AppDataReqEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(APPDATA_REQ_SIG);
    (void)snprintf(e.project, sizeof(e.project), "%s", project);
    e.who = ao_spy();

    QASM_DISPATCH(AO_Storage, &e.super, 0U);
}

/**
 * @brief Take the fields event and assert the conversation closes.
 * @param icons_expected How many icon events must arrive before the end.
 * @return The fields event, still owned by the spy.
 */
static const AppDataFieldsEvt *take_fields(uint8_t icons_expected)
{
    const QEvt *ev = ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(ev, "no fields event");
    TEST_ASSERT_EQUAL_INT(APPDATA_FIELDS_SIG, (int)ev->sig);
    const AppDataFieldsEvt *f = (const AppDataFieldsEvt *)ev;
    for ( uint8_t i = 0U; i < icons_expected; ++i )
    {
        const QEvt *ic = ao_spy_take();
        TEST_ASSERT_NOT_NULL(ic);
        TEST_ASSERT_EQUAL_INT(APPDATA_ICON_SIG, (int)ic->sig);
        TEST_ASSERT_EQUAL_UINT8(i, ((const AppDataIconEvt *)ic)->seq);
    }
    const QEvt *end = ao_spy_take();
    TEST_ASSERT_NOT_NULL(end);
    TEST_ASSERT_EQUAL_INT(APPDATA_END_SIG, (int)end->sig);
    return f;
}

static const char JSON_OK[] =
    "{\"kode\":\"1\",\"name\":\"Scope\",\"version\":\"0.4\",\"color\":\"#8E8E93\"}";

TEST_CASE("no folder anywhere answers none and closes", "[storage_ao][appdata]")
{
    storage_fresh();

    ask_appdata("scope");

    const AppDataFieldsEvt *f = take_fields(0U);
    TEST_ASSERT_EQUAL_UINT8(APPDATA_MEDIUM_NONE, f->medium);
    TEST_ASSERT_EQUAL_UINT8(KAPP_JSON_NONE, f->json_status);
    TEST_ASSERT_EQUAL_UINT8(APPDATA_ICON_NONE, f->icon_status);
}

TEST_CASE("a folder with app.json and no icon", "[storage_ao][appdata]")
{
    storage_fresh();
    fake_media_put_fs_path(".kode/appsdata/scope/app.json", JSON_OK,
                           sizeof(JSON_OK) - 1U);

    ask_appdata("scope");

    const AppDataFieldsEvt *f = take_fields(0U);
    TEST_ASSERT_EQUAL_UINT8(APPDATA_MEDIUM_FS, f->medium);
    TEST_ASSERT_EQUAL_UINT8(KAPP_JSON_OK, f->json_status);
    TEST_ASSERT_EQUAL_STRING("Scope", f->name);
    TEST_ASSERT_TRUE(f->color_valid);
    TEST_ASSERT_EQUAL_UINT8(APPDATA_ICON_NONE, f->icon_status);
}

TEST_CASE("a folder with a right-sized icon streams three thirds",
          "[storage_ao][appdata]")
{
    static uint8_t icon[KAPP_ICON_BYTES];

    storage_fresh();
    for ( size_t i = 0U; sizeof(icon) > i; ++i )
    {
        icon[i] = (uint8_t)i;
    }
    fake_media_put_fs_path(".kode/appsdata/scope/app.json", JSON_OK,
                           sizeof(JSON_OK) - 1U);
    fake_media_put_fs_path(".kode/appsdata/scope/icon.k64", icon, sizeof(icon));

    ask_appdata("scope");

    const AppDataFieldsEvt *f = take_fields(3U);
    TEST_ASSERT_EQUAL_UINT8(APPDATA_ICON_OK, f->icon_status);
}

TEST_CASE("an icon of the wrong size is refused with size",
          "[storage_ao][appdata]")
{
    static uint8_t icon[KAPP_ICON_BYTES - 1U];

    storage_fresh();
    fake_media_put_fs_path(".kode/appsdata/scope/app.json", JSON_OK,
                           sizeof(JSON_OK) - 1U);
    fake_media_put_fs_path(".kode/appsdata/scope/icon.k64", icon, sizeof(icon));

    ask_appdata("scope");

    TEST_ASSERT_EQUAL_UINT8(APPDATA_ICON_SIZE, take_fields(0U)->icon_status);
}

TEST_CASE("the icon key names a file that is not there",
          "[storage_ao][appdata]")
{
    static const char j[] = "{\"kode\":\"1\",\"icon\":\"images/x.k64\"}";

    storage_fresh();
    fake_media_put_fs_path(".kode/appsdata/scope/app.json", j, sizeof(j) - 1U);

    ask_appdata("scope");

    TEST_ASSERT_EQUAL_UINT8(APPDATA_ICON_PATH, take_fields(0U)->icon_status);
}

TEST_CASE("a bad revision and a too-long json are reported, not drawn",
          "[storage_ao][appdata]")
{
    static const char j[] = "{\"kode\":\"9\",\"name\":\"Scope\"}";
    static char       big[KAPP_DATA_JSON_MAX + 8U];

    storage_fresh();
    fake_media_put_fs_path(".kode/appsdata/scope/app.json", j, sizeof(j) - 1U);

    ask_appdata("scope");

    const AppDataFieldsEvt *f = take_fields(0U);
    TEST_ASSERT_EQUAL_UINT8(KAPP_JSON_REVISION, f->json_status);
    TEST_ASSERT_EQUAL_STRING("", f->name);

    (void)memset(big, ' ', sizeof(big));
    fake_media_put_fs_path(".kode/appsdata/scope/app.json", big, sizeof(big));

    ask_appdata("scope");

    TEST_ASSERT_EQUAL_UINT8(KAPP_JSON_SIZE, take_fields(0U)->json_status);
}

TEST_CASE("the internal folder wins over the card's, whole",
          "[storage_ao][appdata]")
{
    static const char card[] = "{\"kode\":\"1\",\"name\":\"Card\",\"version\":\"9\"}";
    static const char fs[]   = "{\"kode\":\"1\",\"name\":\"Board\"}";

    storage_fresh();
    fake_media_set_sd_present(true);
    fake_media_put_sd_path(".kode/appsdata/scope/app.json", card,
                           sizeof(card) - 1U);
    fake_media_put_fs_path(".kode/appsdata/scope/app.json", fs,
                           sizeof(fs) - 1U);

    ask_appdata("scope");

    const AppDataFieldsEvt *f = take_fields(0U);
    TEST_ASSERT_EQUAL_UINT8(APPDATA_MEDIUM_FS, f->medium);
    TEST_ASSERT_EQUAL_STRING("Board", f->name);
    TEST_ASSERT_EQUAL_STRING("", f->version);   /* not merged from the card */
}

TEST_CASE("a card folder alone is found with medium=sd", "[storage_ao][appdata]")
{
    static const char card[] = "{\"kode\":\"1\",\"name\":\"Card\"}";

    storage_fresh();
    fake_media_set_sd_present(true);
    fake_media_put_sd_path(".kode/appsdata/scope/app.json", card,
                           sizeof(card) - 1U);

    ask_appdata("scope");

    TEST_ASSERT_EQUAL_UINT8(APPDATA_MEDIUM_SD, take_fields(0U)->medium);
}

TEST_CASE("a folder is not read while a transfer is in flight",
          "[storage_ao][appdata]")
{
    storage_fresh();
    fake_media_put_fs_path(".kode/appsdata/scope/app.json", JSON_OK,
                           sizeof(JSON_OK) - 1U);
    make_file("app.bin", 512U);

    ask_open("app.bin");                 /* takes the reader slot */
    (void)take_open_status();

    ask_appdata("scope");

    const AppDataFieldsEvt *f = take_fields(0U);
    TEST_ASSERT_EQUAL_UINT8(APPDATA_MEDIUM_NONE, f->medium);
    TEST_ASSERT_EQUAL_UINT8(APPDATA_ICON_BUSY, f->icon_status);
}

/* Every folder question gets its fields and its end marker, even on a dry
 * pool: the launcher asks one folder at a time and starts the next on the end
 * marker, so a question left unanswered stopped its icons for the rest of the
 * boot. The answer is the one a transfer gets - busy, nothing read - so a
 * client never reads the fields of the folder before. */
TEST_CASE("a folder the pool cannot answer is still answered busy, and closes",
          "[storage_ao][appdata]")
{
    const QEvt *held[POOL_HOLD_MAX];
    size_t      n = 0U;

    storage_fresh();
    fake_media_put_fs_path(".kode/appsdata/scope/app.json", JSON_OK,
                           sizeof(JSON_OK) - 1U);

    while ( POOL_HOLD_MAX > n )
    {
        AppDataFieldsEvt * const e =
            Q_NEW_X(AppDataFieldsEvt, 0U, APPDATA_FIELDS_SIG);
        if ( NULL == e )
        {
            break;
        }
        held[n] = &e->super;
        ++n;
    }
    ask_appdata("scope");
    for ( size_t i = 0U; i < n; ++i )
    {
        QF_gc(held[i]);
    }

    const AppDataFieldsEvt *f = take_fields(0U);
    TEST_ASSERT_EQUAL_UINT8(APPDATA_MEDIUM_NONE, f->medium);
    TEST_ASSERT_EQUAL_UINT8(APPDATA_ICON_BUSY, f->icon_status);
}

/* The same promise from the asking side: a question the pool cannot carry
 * never reaches AO_Storage, and is answered busy straight back. */
TEST_CASE("a folder question the pool cannot carry is answered busy at once",
          "[storage_ao][appdata]")
{
    const QEvt *held[POOL_HOLD_MAX];
    size_t      n = 0U;

    storage_fresh();
    while ( POOL_HOLD_MAX > n )
    {
        AppDataReqEvt * const e = Q_NEW_X(AppDataReqEvt, 0U, APPDATA_REQ_SIG);
        if ( NULL == e )
        {
            break;
        }
        held[n] = &e->super;
        ++n;
    }
    appdata_request("scope", ao_spy());
    for ( size_t i = 0U; i < n; ++i )
    {
        QF_gc(held[i]);
    }

    const AppDataFieldsEvt *f = take_fields(0U);
    TEST_ASSERT_EQUAL_UINT8(APPDATA_ICON_BUSY, f->icon_status);
}

/* All three blocks or none: an icon that arrived in pieces would be drawn
 * wrong rather than not drawn, so a short pool is a refusal and not a
 * partial send. The console's own upload is what takes the blocks in the
 * field; two of them here is the same shortage. */
TEST_CASE("an icon is not started when the jumbo pool is short",
          "[storage_ao][appdata]")
{
    static uint8_t icon[KAPP_ICON_BYTES];

    storage_fresh();
    fake_media_put_fs_path(".kode/appsdata/scope/app.json", JSON_OK,
                           sizeof(JSON_OK) - 1U);
    fake_media_put_fs_path(".kode/appsdata/scope/icon.k64", icon, sizeof(icon));

    RecvDataEvt *held[2];
    for ( unsigned i = 0U; 2U > i; ++i )
    {
        held[i] = Q_NEW_X(RecvDataEvt, 0U, RECV_DATA_SIG);
        TEST_ASSERT_NOT_NULL_MESSAGE(held[i], "the jumbo pool is smaller than "
                                              "the board's");
    }

    ask_appdata("scope");

    TEST_ASSERT_EQUAL_UINT8(APPDATA_ICON_BUSY, take_fields(0U)->icon_status);

    for ( unsigned i = 0U; 2U > i; ++i )
    {
        QF_gc(&held[i]->super);
    }
}

/* ---- Deleting one app's data folder ----------------------------------------- */

/** @brief Post APPDATA_RM_REQ_SIG for @p project with the spy as the asker. */
static void ask_appdata_rm(const char *project)
{
    static AppDataReqEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(APPDATA_RM_REQ_SIG);
    (void)snprintf(e.project, sizeof(e.project), "%s", project);
    e.who = ao_spy();

    QASM_DISPATCH(AO_Storage, &e.super, 0U);
}

/** @brief Take the delete's verdict; every request is answered. */
static kode_status_t take_rm_status(void)
{
    const QEvt *ev = ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(ev, "the delete went unanswered");
    TEST_ASSERT_EQUAL_INT(APPDATA_RM_RSP_SIG, (int)ev->sig);
    return (kode_status_t)((const AppDataRmRspEvt *)ev)->status;
}

/* The app data folder (https://docs.kode.diy/sdk/kdrv/apps), and the one rule in it with a bench
 * measurement behind it: the console writes and deletes on the internal
 * partition, because a counterfeit card fails every write and poisons the
 * reads that follow. A card folder of the same name is left exactly where it
 * was, and said so in the log. */
TEST_CASE("appdata_rm removes the internal folder and nothing on the card",
          "[storage_ao][appdata]")
{
    storage_fresh();
    fake_media_set_sd_present(true);
    fake_media_put_fs_path(".kode/appsdata/scope/app.json", JSON_OK,
                           sizeof(JSON_OK) - 1U);
    fake_media_put_sd_path(".kode/appsdata/scope/app.json", JSON_OK,
                           sizeof(JSON_OK) - 1U);

    ask_appdata_rm("scope");

    TEST_ASSERT_EQUAL_INT(KODE_OK, take_rm_status());
    TEST_ASSERT_FALSE(fake_media_file_exists(fake_media_fs_dir(),
                                             ".kode/appsdata/scope/app.json"));
    TEST_ASSERT_TRUE(fake_media_file_exists(fake_media_sd_dir(),
                                            ".kode/appsdata/scope/app.json"));
}

/* An `icon` may point one level down (section B), so the delete reaches one
 * level down - and the folder still goes, which an rmdir of a directory that
 * is not empty would not have done. */
TEST_CASE("appdata_rm empties one level of subfolder as well",
          "[storage_ao][appdata]")
{
    static const uint8_t icon[8] = { 0U };

    storage_fresh();
    fake_media_put_fs_path(".kode/appsdata/scope/app.json", JSON_OK,
                           sizeof(JSON_OK) - 1U);
    fake_media_put_fs_path(".kode/appsdata/scope/images/scope.k64", icon,
                           sizeof(icon));

    ask_appdata_rm("scope");

    TEST_ASSERT_EQUAL_INT(KODE_OK, take_rm_status());
    TEST_ASSERT_FALSE(fake_media_file_exists(
        fake_media_fs_dir(), ".kode/appsdata/scope/images/scope.k64"));
}

/* The same firmware, a delete first: the volume is mounted by the delete. */
TEST_CASE("appdata_rm mounts the internal FAT when nothing has",
          "[storage_ao][appdata]")
{
    storage_fresh();
    fake_media_put_fs_path(".kode/appsdata/scope/app.json", JSON_OK,
                           sizeof(JSON_OK) - 1U);
    kdrv_storage_unmount(KDRV_MEDIUM_FS);
    TEST_ASSERT_FALSE(kdrv_storage_mounted(KDRV_MEDIUM_FS));

    ask_appdata_rm("scope");

    TEST_ASSERT_EQUAL_INT(KODE_OK, take_rm_status());
    TEST_ASSERT_TRUE(kdrv_storage_mounted(KDRV_MEDIUM_FS));
    TEST_ASSERT_FALSE(fake_media_file_exists(fake_media_fs_dir(),
                                             ".kode/appsdata/scope/app.json"));
}

/* A partition that will not mount is refused, and the folder is not touched. */
TEST_CASE("appdata_rm whose mount fails answers an error and keeps the folder",
          "[storage_ao][appdata]")
{
    storage_fresh();
    fake_media_put_fs_path(".kode/appsdata/scope/app.json", JSON_OK,
                           sizeof(JSON_OK) - 1U);
    kdrv_storage_unmount(KDRV_MEDIUM_FS);
    fake_fs_mount_set_fails(true);

    ask_appdata_rm("scope");

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, take_rm_status());
    TEST_ASSERT_FALSE(kdrv_storage_mounted(KDRV_MEDIUM_FS));
    TEST_ASSERT_TRUE(fake_media_file_exists(fake_media_fs_dir(),
                                            ".kode/appsdata/scope/app.json"));
}

/* The mount formats what will not mount, so a name that cannot be a folder
 * never reaches it. */
TEST_CASE("appdata_rm of a name that cannot be a folder never mounts",
          "[storage_ao][appdata]")
{
    storage_fresh();
    kdrv_storage_unmount(KDRV_MEDIUM_FS);

    ask_appdata_rm("bad/name");

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, take_rm_status());
    TEST_ASSERT_EQUAL_UINT(0U, fake_fs_mount_calls());
    TEST_ASSERT_FALSE(kdrv_storage_mounted(KDRV_MEDIUM_FS));
}

/* Nothing to delete is still an answer: the console closes its command on
 * this event and on nothing else. */
TEST_CASE("a delete of a folder that is not there is refused, and answered",
          "[storage_ao][appdata]")
{
    storage_fresh();

    ask_appdata_rm("scope");

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, take_rm_status());
}

/* A folder is never worth displacing a transfer, and the delete says which of
 * the two refusals it is - an operator whose `appdata_rm` was refused while a
 * file streamed needs to know to type it again. */
TEST_CASE("a delete is refused while a transfer is in flight",
          "[storage_ao][appdata]")
{
    storage_fresh();
    fake_media_put_fs_path(".kode/appsdata/scope/app.json", JSON_OK,
                           sizeof(JSON_OK) - 1U);
    make_file("app.bin", 512U);

    ask_open("app.bin");                 /* takes the reader slot */
    (void)take_open_status();

    ask_appdata_rm("scope");

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, take_rm_status());
    TEST_ASSERT_TRUE(fake_media_file_exists(fake_media_fs_dir(),
                                            ".kode/appsdata/scope/app.json"));
}

/* An asker with a delete in flight takes only the answer that names it. */
TEST_CASE("appdata_rm answers with the project it was asked about",
          "[storage_ao][appdata]")
{
    storage_fresh();

    ask_appdata_rm("scope");

    const QEvt * const ev = ao_spy_take();
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_INT(APPDATA_RM_RSP_SIG, (int)ev->sig);
    TEST_ASSERT_EQUAL_STRING("scope", ((const AppDataRmRspEvt *)ev)->project);
}

/* ---- Writing one app's data folder ------------------------------------------ */

/* The folder's third writer, the install of a container that carries
 * metadata. Two steps of the AO - app.json, then icon.k64 after a self-post -
 * and one answer to whoever asked, a refusal included. The bytes are handed
 * over with the request: AO_Storage frees the one buffer on every path, and
 * the cases count it (fake_psram_alloc_count() against the frees). */

static const char PUT_JSON[] = "{\"name\":\"Demo\",\"category\":\"tools\"}";
static uint8_t    s_put_icon[KAPP_ICON_BYTES];

/** @brief Dispatch APPDATA_PUT_REQ_SIG for @p project, from the spy, with a
 *         fresh buffer that is AO_Storage's from here. */
static void ask_appdata_put(const char *project, uint16_t icon_len)
{
    static AppDataPutEvt e;
    uint16_t const       jlen = (uint16_t)(sizeof(PUT_JSON) - 1U);

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(APPDATA_PUT_REQ_SIG);
    (void)snprintf(e.project, sizeof(e.project), "%s", project);
    /* cppcheck-suppress misra-c2012-11.5 ; a byte buffer from a void * allocator */
    e.buf = (uint8_t *)khal_realloc_psram(NULL, (size_t)jlen + (size_t)icon_len);
    TEST_ASSERT_NOT_NULL(e.buf);
    (void)memcpy(e.buf, PUT_JSON, jlen);
    if ( 0U != icon_len )
    {
        (void)memcpy(&e.buf[jlen], s_put_icon, icon_len);
    }
    e.json_len = jlen;
    e.icon_len = icon_len;
    e.who      = ao_spy();

    QASM_DISPATCH(AO_Storage, &e.super, 0U);
}

/** @brief Every buffer a put was handed has been given back. */
static void assert_put_bufs_freed(void)
{
    TEST_ASSERT_EQUAL_UINT_MESSAGE(fake_psram_alloc_count(),
                                   fake_psram_free_count(),
        "AO_Storage kept the buffer a put handed it");
}

/** @brief Take the put's answer; every request is answered. */
static kode_status_t take_put_status(void)
{
    const QEvt *ev = ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(ev, "the put went unanswered");
    TEST_ASSERT_EQUAL_INT(APPDATA_PUT_RSP_SIG, (int)ev->sig);
    return ((const AppDataPutRspEvt *)ev)->rc;
}

/** @brief The size of @p rel under the internal fake, or -1 when absent. */
static long fs_size(const char *rel)
{
    char  path[512];
    FILE *f;
    long  n;

    (void)snprintf(path, sizeof(path), "%s/%s", fake_media_fs_dir(), rel);
    f = fopen(path, "rb");
    if ( NULL == f )
    {
        return -1L;
    }
    (void)fseek(f, 0L, SEEK_END);
    n = ftell(f);
    (void)fclose(f);
    return n;
}

TEST_CASE("put writes app.json and icon.k64 into the internal folder",
          "[storage_ao][appdata]")
{
    char   path[512];
    char   got[sizeof(PUT_JSON)];
    FILE  *f;

    storage_fresh();
    fake_psram_reset();
    fake_media_set_sd_present(true);
    (void)memset(s_put_icon, 0x5A, sizeof(s_put_icon));

    ask_appdata_put("demo", KAPP_ICON_BYTES);
    drain_write();

    TEST_ASSERT_EQUAL_INT(KODE_OK, take_put_status());
    assert_put_bufs_freed();
    (void)snprintf(path, sizeof(path), "%s/.kode/appsdata/demo/app.json",
                   fake_media_fs_dir());
    f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(f);
    (void)memset(got, 0, sizeof(got));
    TEST_ASSERT_EQUAL_UINT(sizeof(PUT_JSON) - 1U,
                           fread(got, 1U, sizeof(got), f));
    (void)fclose(f);
    TEST_ASSERT_EQUAL_STRING(PUT_JSON, got);
    TEST_ASSERT_EQUAL_INT(12288, (int)fs_size(".kode/appsdata/demo/icon.k64"));
    TEST_ASSERT_FALSE(fake_media_file_exists(fake_media_sd_dir(),
                                             ".kode/appsdata/demo/app.json"));
    TEST_ASSERT_FALSE(fake_media_file_exists(fake_media_sd_dir(),
                                             ".kode/appsdata/demo/icon.k64"));
}

TEST_CASE("put without an icon writes app.json only and removes an old icon",
          "[storage_ao][appdata]")
{
    storage_fresh();
    fake_psram_reset();
    fake_media_put_fs_path(".kode/appsdata/demo/icon.k64", s_put_icon,
                           sizeof(s_put_icon));

    ask_appdata_put("demo", 0U);
    drain_write();

    TEST_ASSERT_EQUAL_INT(KODE_OK, take_put_status());
    assert_put_bufs_freed();
    TEST_ASSERT_TRUE(fs_size(".kode/appsdata/demo/app.json") > 0L);
    TEST_ASSERT_FALSE(fake_media_file_exists(fake_media_fs_dir(),
                                             ".kode/appsdata/demo/icon.k64"));
}

TEST_CASE("put replaces a folder that had other files", "[storage_ao][appdata]")
{
    static const uint8_t old_json[3] = { 'o', 'l', 'd' };

    storage_fresh();
    fake_psram_reset();
    fake_media_put_fs_path(".kode/appsdata/demo/logo.k64", s_put_icon, 16U);
    fake_media_put_fs_path(".kode/appsdata/demo/app.json", old_json,
                           sizeof(old_json));

    ask_appdata_put("demo", KAPP_ICON_BYTES);
    drain_write();

    TEST_ASSERT_EQUAL_INT(KODE_OK, take_put_status());
    assert_put_bufs_freed();
    TEST_ASSERT_FALSE(fake_media_file_exists(fake_media_fs_dir(),
                                             ".kode/appsdata/demo/logo.k64"));
    TEST_ASSERT_EQUAL_INT((int)(sizeof(PUT_JSON) - 1U),
                          (int)fs_size(".kode/appsdata/demo/app.json"));
    TEST_ASSERT_EQUAL_INT(12288, (int)fs_size(".kode/appsdata/demo/icon.k64"));
}

TEST_CASE("put creates .kode and appsdata when missing", "[storage_ao][appdata]")
{
    storage_fresh();
    fake_psram_reset();
    TEST_ASSERT_FALSE(fake_media_file_exists(fake_media_fs_dir(), ".kode"));

    ask_appdata_put("demo", 0U);
    drain_write();

    TEST_ASSERT_EQUAL_INT(KODE_OK, take_put_status());
    assert_put_bufs_freed();
    TEST_ASSERT_TRUE(fake_media_file_exists(fake_media_fs_dir(),
                                            ".kode/appsdata/demo/app.json"));
}

/* A firmware that has not scanned the launcher has not mounted the internal
 * FAT: the put mounts it itself. */
TEST_CASE("put mounts the internal FAT when nothing has", "[storage_ao][appdata]")
{
    storage_fresh();
    fake_psram_reset();
    kdrv_storage_unmount(KDRV_MEDIUM_FS);
    TEST_ASSERT_FALSE(kdrv_storage_mounted(KDRV_MEDIUM_FS));

    ask_appdata_put("demo", 0U);
    drain_write();

    TEST_ASSERT_EQUAL_INT(KODE_OK, take_put_status());
    assert_put_bufs_freed();
    TEST_ASSERT_TRUE(kdrv_storage_mounted(KDRV_MEDIUM_FS));
    TEST_ASSERT_TRUE(fake_media_file_exists(fake_media_fs_dir(),
                                            ".kode/appsdata/demo/app.json"));
}

TEST_CASE("put whose mount fails answers an error and writes nothing",
          "[storage_ao][appdata]")
{
    storage_fresh();
    fake_psram_reset();
    kdrv_storage_unmount(KDRV_MEDIUM_FS);
    fake_fs_mount_set_fails(true);

    ask_appdata_put("demo", KAPP_ICON_BYTES);
    drain_write();

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, take_put_status());
    assert_put_bufs_freed();
    TEST_ASSERT_FALSE(kdrv_storage_mounted(KDRV_MEDIUM_FS));
    TEST_ASSERT_FALSE(fake_media_file_exists(fake_media_fs_dir(), ".kode"));
}

TEST_CASE("put refuses a name that cannot be a folder", "[storage_ao][appdata]")
{
    storage_fresh();
    fake_psram_reset();

    ask_appdata_put("bad/name", KAPP_ICON_BYTES);
    drain_write();

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, take_put_status());
    assert_put_bufs_freed();
    TEST_ASSERT_FALSE(fake_media_file_exists(fake_media_fs_dir(), ".kode"));
}

TEST_CASE("put with an icon length the format does not have is refused",
          "[storage_ao][appdata]")
{
    storage_fresh();
    fake_psram_reset();

    ask_appdata_put("demo", 100U);
    drain_write();

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, take_put_status());
    assert_put_bufs_freed();
    TEST_ASSERT_FALSE(fake_media_file_exists(fake_media_fs_dir(), ".kode"));
}

TEST_CASE("put while a transfer runs answers busy", "[storage_ao][appdata]")
{
    storage_fresh();
    fake_psram_reset();
    make_file("app.bin", 512U);

    ask_open("app.bin");                 /* takes the reader slot */
    (void)take_open_status();

    ask_appdata_put("demo", 0U);
    drain_write();

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, take_put_status());
    assert_put_bufs_freed();
    TEST_ASSERT_FALSE(fake_media_file_exists(fake_media_fs_dir(), ".kode"));
}

/* Between the two steps the put owns the AO: busy() reads it, so a second put
 * is refused and the first one still lands whole. */
TEST_CASE("a second put during the first answers busy", "[storage_ao][appdata]")
{
    storage_fresh();
    fake_psram_reset();

    ask_appdata_put("demo", KAPP_ICON_BYTES);   /* step one done, icon pending */
    ask_appdata_put("other", 0U);

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, take_put_status());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_psram_free_count(),
        "the refused put's buffer was not given back, or the running one's was");
    TEST_ASSERT_FALSE(fake_media_file_exists(fake_media_fs_dir(),
                                             ".kode/appsdata/other"));

    drain_write();
    TEST_ASSERT_EQUAL_INT(KODE_OK, take_put_status());
    assert_put_bufs_freed();
    TEST_ASSERT_EQUAL_INT(12288, (int)fs_size(".kode/appsdata/demo/icon.k64"));
}

/* A name that is a directory cannot be created as a file, which is how the
 * second write is made to fail here - and a permission trick would not do:
 * CI runs as root. */
TEST_CASE("a failed icon write leaves no folder", "[storage_ao][appdata]")
{
    char dir[512];

    storage_fresh();
    fake_psram_reset();

    ask_appdata_put("demo", KAPP_ICON_BYTES);
    (void)snprintf(dir, sizeof(dir), "%s/.kode/appsdata/demo/icon.k64",
                   fake_media_fs_dir());
    TEST_ASSERT_EQUAL_INT(0, mkdir(dir, 0755));
    drain_write();

    TEST_ASSERT_NOT_EQUAL(KODE_OK, take_put_status());
    assert_put_bufs_freed();
    TEST_ASSERT_FALSE(fake_media_file_exists(fake_media_fs_dir(),
                                             ".kode/appsdata/demo"));
}

TEST_CASE("put answers after two steps", "[storage_ao][appdata]")
{
    storage_fresh();
    fake_psram_reset();

    ask_appdata_put("demo", KAPP_ICON_BYTES);

    /* The json is down, the icon is not, and nobody has been told. */
    TEST_ASSERT_NULL(ao_spy_take());
    TEST_ASSERT_TRUE(fs_size(".kode/appsdata/demo/app.json") > 0L);
    TEST_ASSERT_EQUAL_INT(-1, (int)fs_size(".kode/appsdata/demo/icon.k64"));

    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Storage, 1U));

    TEST_ASSERT_EQUAL_INT(KODE_OK, take_put_status());
    assert_put_bufs_freed();
    TEST_ASSERT_EQUAL_INT(12288, (int)fs_size(".kode/appsdata/demo/icon.k64"));
}

/* ---- The card's fields ------------------------------------------------------ */

/** @brief Ask AO_Storage for one field, from the spy. */
static void ask_field(uint8_t op, kdrv_sd_field_t f, int32_t value)
{
    StorageFieldReqEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(STORAGE_FIELD_REQ_SIG);
    e.who   = ao_spy();
    e.op    = op;
    e.field = (uint16_t)f;
    e.value = value;

    QASM_DISPATCH(AO_Storage, &e.super, 0U);
}

/** @brief Take the next answer and assert it is a field response. */
static const StorageFieldEvt *take_field(void)
{
    const QEvt * const e = ao_spy_take();

    TEST_ASSERT_NOT_NULL_MESSAGE(e, "the field request went unanswered");
    TEST_ASSERT_EQUAL_INT(STORAGE_FIELD_RSP_SIG, (int)e->sig);
    return (const StorageFieldEvt *)e;
}

/* The console speaks one language to every chip that carries a field table,
 * and the card is one of them now: three verbs, the field already resolved to
 * the driver's own index, and an answer posted back to whoever asked. */
TEST_CASE("a card field is read, written and dumped for whoever asks",
          "[storage_ao]")
{
    storage_fresh();
    fake_psram_reset();

    ask_field(FIELD_OP_GET, KDRV_SD_BUS_WIDTH, 0);
    const StorageFieldEvt *a = take_field();
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)a->rc);
    TEST_ASSERT_EQUAL_INT32(4, a->value);

    /* A write comes back with what the driver kept, not with what was asked. */
    ask_field(FIELD_OP_SET, KDRV_SD_WIDTH_REQ, 1);
    a = take_field();
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)a->rc);
    TEST_ASSERT_EQUAL_INT32(1, a->value);

    ask_field(FIELD_OP_DUMP, KDRV_SD_MOUNTED, 0);
    a = take_field();
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)a->rc);
}

/* A refusal is an answer. The console closes its command on the response, so
 * a silent refusal would leave it waiting out its own deadline. */
TEST_CASE("a card field that cannot be answered is refused, and still answered",
          "[storage_ao]")
{
    storage_fresh();
    fake_psram_reset();

    ask_field(FIELD_OP_SET, KDRV_SD_CAPACITY_MB, 1);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)take_field()->rc);

    ask_field(FIELD_OP_SET, KDRV_SD_WIDTH_REQ, 3);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)take_field()->rc);

    fake_media_set_sd_present(false);
    ask_field(FIELD_OP_GET, KDRV_SD_MFG_ID, 0);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)take_field()->rc);
}

/* Nothing else in kodeOS asks, but an app could, and an event posted to NULL
 * is an assert rather than a refusal. */
TEST_CASE("a field request with nobody to answer is served and dropped",
          "[storage_ao]")
{
    StorageFieldReqEvt e;

    storage_fresh();
    fake_psram_reset();

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(STORAGE_FIELD_REQ_SIG);
    e.who   = NULL;
    e.op    = FIELD_OP_GET;
    e.field = (uint16_t)KDRV_SD_MOUNTED;

    QASM_DISPATCH(AO_Storage, &e.super, 0U);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
}

/* ---- The card test ---------------------------------------------------------- */

/* This row judges THE BOARD, and the difference from judging the card is the
 * whole reason it is shaped the way it is. The counterfeit microSDs shipped
 * with some units read perfectly and refuse to be written to, so a row that
 * failed on a failed write would condemn a good slot for a bad piece of
 * plastic. What is arranged per case is therefore the card, and what is
 * asserted is which tally the failure landed in. */

/** @brief Ask AO_Storage for the card row, the way `test component=sd` does. */
static void ask_sd_test(void)
{
    storage_request(KDRV_MEDIUM_SD, STORAGE_OP_SELFTEST);
    (void)qf_support_pump(AO_Storage, 4U);
}

/** @brief Ask for `sd_verify`, the bench verb that judges the card. */
static void ask_sd_verify(void)
{
    storage_request(KDRV_MEDIUM_SD, STORAGE_OP_VERIFY);
    (void)qf_support_pump(AO_Storage, 4U);
}

/** @brief How many calls the card worker made in the last run_sd_script(). */
static unsigned sd_jobs_run;

/**
 * @brief Run a script to its verdict, advancing the clock between steps.
 *
 * The host clock only moves when somebody moves it, so a rate measured across
 * one of these loops is exactly what the case put there - and, because the AO
 * times the transfer calls rather than the wall clock, it is the delay inside
 * khal_sdmmc's fake that lands in the number.
 */
static void run_sd_script(void)
{
    sd_jobs_run = 0U;
    for ( unsigned i = 0U; 600U > i; ++i )
    {
        unsigned n = qf_support_pump(AO_Storage, 8U);

        if ( fake_khal_worker_run_next_for(SD_WORKER) )
        {
            ++sd_jobs_run;
            ++n;
        }
        if ( 0U == n )
        {
            break;
        }
    }
}

/** @brief Take the next record and assert it is the phase expected. */
static const StorageTestEvt *take_phase(uint8_t phase)
{
    const QEvt * const e = ao_spy_take();

    TEST_ASSERT_NOT_NULL_MESSAGE(e, "the script stopped short of a record");
    TEST_ASSERT_EQUAL_INT(STORAGE_TEST_SIG, (int)e->sig);

    const StorageTestEvt * const t = (const StorageTestEvt *)e;
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(phase, t->phase, "phases out of order");
    return t;
}

/** @brief Take the verdict that closes a script. */
static bool take_verdict(const char *name)
{
    const QEvt * const e = ao_spy_take();

    TEST_ASSERT_NOT_NULL_MESSAGE(e, "the script never reached a verdict");
    TEST_ASSERT_EQUAL_INT(TEST_RESULT_SIG, (int)e->sig);
    TEST_ASSERT_EQUAL_STRING(name, ((const TestResultEvt *)e)->name);
    return ((const TestResultEvt *)e)->ok;
}

/** @brief Put AO_Storage back with the spy listening for a script's events. */
static void sd_script_fresh(void)
{
    storage_fresh();
    fake_psram_reset();
    ao_spy_subscribe(STORAGE_TEST_SIG);
    ao_spy_subscribe(TEST_RESULT_SIG);
    ao_spy_drain();
}

/** @brief Walk the row's five records, keeping each phase's verdict. */
static void take_all_phases(bool *ok)
{
    for ( uint8_t p = SD_TEST_IDENTIFY; SD_TEST_VERIFY >= p; ++p )
    {
        ok[p - 1U] = take_phase(p)->ok;
    }
}

TEST_CASE("the card row walks its five phases and a healthy slot passes",
          "[storage_ao]")
{
    sd_script_fresh();
    ask_sd_test();
    run_sd_script();

    const StorageTestEvt *t = take_phase(SD_TEST_IDENTIFY);
    TEST_ASSERT_TRUE_MESSAGE(t->ok, "a healthy card failed to identify");
    TEST_ASSERT_EQUAL_UINT8(4U, t->width);
    TEST_ASSERT_EQUAL_UINT32(30000U, t->capacity_mb);
    TEST_ASSERT_EQUAL_STRING("KODE1", t->name);

    t = take_phase(SD_TEST_READ4);
    TEST_ASSERT_TRUE(t->ok);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(4U, t->width, "read on four lines");

    t = take_phase(SD_TEST_READ1);
    TEST_ASSERT_TRUE(t->ok);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1U, t->width, "and again on one");

    TEST_ASSERT_TRUE(take_phase(SD_TEST_WRITE)->ok);
    TEST_ASSERT_TRUE(take_phase(SD_TEST_VERIFY)->ok);
    TEST_ASSERT_TRUE(take_verdict("sd"));
}

/* The row is not allowed to change the card it was handed. */
TEST_CASE("the card row leaves nothing of its own behind and does not format",
          "[storage_ao]")
{
    bool ok[SD_TEST_STEPS];
    char path[512];
    FILE *f;

    sd_script_fresh();
    (void)snprintf(path, sizeof(path), "%s/keep.bin", fake_media_sd_dir());
    f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    (void)fputc('x', f);
    (void)fclose(f);

    ask_sd_test();
    run_sd_script();

    take_all_phases(ok);
    TEST_ASSERT_TRUE(take_verdict("sd"));
    TEST_ASSERT_FALSE(
        fake_media_file_exists(fake_media_sd_dir(), ".kodeos_sdtest"));
    TEST_ASSERT_TRUE_MESSAGE(
        fake_media_file_exists(fake_media_sd_dir(), "keep.bin"),
        "the row wiped a card it was only supposed to read");
}

/* It puts the slot back on the board's own width, whatever the middle of the
 * run did with it: leaving a unit at one data line after a production test
 * would be a slow board that nothing explains. */
TEST_CASE("the card row hands the slot back at four data lines",
          "[storage_ao]")
{
    bool    ok[SD_TEST_STEPS];
    int32_t width = 0;

    sd_script_fresh();
    ask_sd_test();
    run_sd_script();

    take_all_phases(ok);
    TEST_ASSERT_TRUE(take_verdict("sd"));
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)kdrv_sd_get(KDRV_SD_BUS_WIDTH, &width));
    TEST_ASSERT_EQUAL_INT32(4, width);
}

/* Three of the four data lines not reaching the slot is a defect of the unit,
 * and every phase below would be measuring the wrong thing. */
TEST_CASE("a card that comes up on one data line fails the board's verdict",
          "[storage_ao]")
{
    sd_script_fresh();
    fake_sdmmc_set_max_width(1U);

    ask_sd_test();
    run_sd_script();

    const StorageTestEvt * const t = take_phase(SD_TEST_IDENTIFY);
    TEST_ASSERT_FALSE(t->ok);
    TEST_ASSERT_EQUAL_UINT8(1U, t->width);
    TEST_ASSERT_FALSE(take_verdict("sd"));
}

TEST_CASE("an empty slot fails the board's verdict", "[storage_ao]")
{
    sd_script_fresh();
    fake_media_set_sd_present(false);

    ask_sd_test();
    run_sd_script();

    TEST_ASSERT_FALSE(take_phase(SD_TEST_IDENTIFY)->ok);
    TEST_ASSERT_FALSE(take_verdict("sd"));
}

/* The heart of the row: a data line that does not carry shows up as a read
 * that will not complete, because the peripheral checks a CRC per line. */
TEST_CASE("a four-line read that will not complete fails the board's verdict",
          "[storage_ao]")
{
    bool ok[SD_TEST_STEPS];

    sd_script_fresh();
    fake_sdmmc_fail_reads(true);

    ask_sd_test();
    run_sd_script();

    take_all_phases(ok);
    TEST_ASSERT_TRUE_MESSAGE(ok[SD_TEST_IDENTIFY - 1U], "the card identified");
    TEST_ASSERT_FALSE_MESSAGE(ok[SD_TEST_READ4 - 1U],
                              "a read that will not complete is a bad line");
    TEST_ASSERT_FALSE(take_verdict("sd"));
}

/* THE case this whole redesign exists for. The counterfeit cards read
 * perfectly and refuse to be written to, and the board they are sitting in is
 * fine. A row that failed here would condemn good hardware. */
TEST_CASE("a card that refuses to be written to does not condemn the slot",
          "[storage_ao]")
{
    bool ok[SD_TEST_STEPS];

    sd_script_fresh();
    fake_sdmmc_fail_file_writes(true);

    ask_sd_test();
    run_sd_script();

    take_all_phases(ok);
    TEST_ASSERT_TRUE_MESSAGE(ok[SD_TEST_IDENTIFY - 1U], "the card identified");
    TEST_ASSERT_TRUE_MESSAGE(ok[SD_TEST_READ4 - 1U], "and read on four lines");
    TEST_ASSERT_FALSE_MESSAGE(ok[SD_TEST_WRITE - 1U], "and refused the file");
    TEST_ASSERT_TRUE_MESSAGE(take_verdict("sd"),
                             "a bad card condemned a good slot");
}

/* The rates are the reason the row is worth running on a board that already
 * mounts: they are measured across the transfers alone, never across the
 * pacing between them, or every number would describe this AO's politeness. */
TEST_CASE("each transfer reports the rate its own transfers took",
          "[storage_ao]")
{
    sd_script_fresh();
    /* Four milliseconds per 32 KB chunk is 128 ms for the megabyte, which is
     * 8000 KB/s. The pacing between chunks is not in it. */
    fake_sdmmc_set_read_ms(4U);

    ask_sd_test();
    run_sd_script();

    TEST_ASSERT_TRUE(take_phase(SD_TEST_IDENTIFY)->ok);

    const StorageTestEvt * const t = take_phase(SD_TEST_READ4);
    TEST_ASSERT_TRUE(t->ok);
    TEST_ASSERT_EQUAL_UINT32(8000U, t->kb_s);
}

/* A clock that did not move says nothing about speed, and must not divide by
 * zero to say it. */
TEST_CASE("a run with a still clock reports no rate rather than a wrong one",
          "[storage_ao]")
{
    bool ok[SD_TEST_STEPS];

    sd_script_fresh();
    ask_sd_test();
    run_sd_script();

    (void)take_phase(SD_TEST_IDENTIFY);
    TEST_ASSERT_EQUAL_UINT32(0U, take_phase(SD_TEST_READ4)->kb_s);
    ok[0] = true;
    (void)ok;
}

/* The row walks the card between widths, so it is not something to start
 * while anything at all has a file open. */
TEST_CASE("the card row is refused while a file is open, and answered",
          "[storage_ao]")
{
    sd_script_fresh();
    make_file("held.bin", 16U);
    ask_open("held.bin");
    TEST_ASSERT_EQUAL_INT(KODE_OK, take_open_status());

    ask_sd_test();
    run_sd_script();

    TEST_ASSERT_FALSE_MESSAGE(take_verdict("sd"), "the row ran with a file open");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "a refused row printed a phase record");
}

/* A rescan from the launcher and a radio link coming up both post a mount or
 * an unmount, and either would pull the card out mid-run. */
TEST_CASE("a mount arriving during the card row is ignored", "[storage_ao]")
{
    bool ok[SD_TEST_STEPS];

    sd_script_fresh();
    ask_sd_test();

    (void)qf_support_pump(AO_Storage, 2U);
    storage_request(KDRV_MEDIUM_SD, STORAGE_OP_UNMOUNT);
    run_sd_script();

    take_all_phases(ok);
    for ( unsigned i = 0U; SD_TEST_STEPS > i; ++i )
    {
        TEST_ASSERT_TRUE_MESSAGE(ok[i], "a stray mount broke the run");
    }
    TEST_ASSERT_TRUE(take_verdict("sd"));
}

/* ---- sd_verify: the card, not the slot -------------------------------------- */

TEST_CASE("sd_verify probes the card's far end and formats it", "[storage_ao]")
{
    sd_script_fresh();
    ask_sd_verify();
    run_sd_script();

    TEST_ASSERT_TRUE(take_phase(SD_VERIFY_PROBE)->ok);

    const StorageTestEvt * const t = take_phase(SD_VERIFY_FORMAT);
    TEST_ASSERT_TRUE(t->ok);
    TEST_ASSERT_EQUAL_UINT32(29999U, t->total_mb);
    TEST_ASSERT_TRUE(take_verdict("sd_verify"));
}

TEST_CASE("sd_verify wipes the card, which the row never does", "[storage_ao]")
{
    char  path[512];
    FILE *f;

    sd_script_fresh();
    (void)snprintf(path, sizeof(path), "%s/keep.bin", fake_media_sd_dir());
    f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    (void)fputc('x', f);
    (void)fclose(f);

    ask_sd_verify();
    run_sd_script();

    (void)take_phase(SD_VERIFY_PROBE);
    (void)take_phase(SD_VERIFY_FORMAT);
    TEST_ASSERT_TRUE(take_verdict("sd_verify"));
    TEST_ASSERT_FALSE(fake_media_file_exists(fake_media_sd_dir(), "keep.bin"));
}

/* The lie sd_verify exists for: a card claiming thirty gigabytes over eight
 * does not refuse the write past its real end - it wraps it. */
TEST_CASE("sd_verify catches a card that wraps its address space",
          "[storage_ao]")
{
    sd_script_fresh();
    fake_sdmmc_set_alias(16777216U);   /* really 8 GB */

    ask_sd_verify();
    run_sd_script();

    TEST_ASSERT_FALSE_MESSAGE(take_phase(SD_VERIFY_PROBE)->ok,
                              "the tail was found where it should not be");
    (void)take_phase(SD_VERIFY_FORMAT);
    TEST_ASSERT_FALSE(take_verdict("sd_verify"));
}

TEST_CASE("sd_verify catches a card whose far end swallows a write",
          "[storage_ao]")
{
    sd_script_fresh();
    fake_sdmmc_fail_writes(true);

    ask_sd_verify();
    run_sd_script();

    TEST_ASSERT_FALSE(take_phase(SD_VERIFY_PROBE)->ok);
    (void)take_phase(SD_VERIFY_FORMAT);
    TEST_ASSERT_FALSE(take_verdict("sd_verify"));
}

/* A filesystem that covers a fraction of the card is a card that did not
 * really format, whatever it answered. */
TEST_CASE("sd_verify catches a filesystem that covers too little",
          "[storage_ao]")
{
    sd_script_fresh();
    fake_sdmmc_set_total_mb(1000U);   /* on a 30000 MB card */

    ask_sd_verify();
    run_sd_script();

    TEST_ASSERT_TRUE(take_phase(SD_VERIFY_PROBE)->ok);
    const StorageTestEvt * const t = take_phase(SD_VERIFY_FORMAT);
    TEST_ASSERT_FALSE(t->ok);
    TEST_ASSERT_EQUAL_UINT32(1000U, t->total_mb);
    TEST_ASSERT_FALSE(take_verdict("sd_verify"));
}

TEST_CASE("sd_verify on a card that will not take a filesystem fails",
          "[storage_ao]")
{
    sd_script_fresh();
    fake_sdmmc_fail_format(true);

    ask_sd_verify();
    run_sd_script();

    TEST_ASSERT_TRUE(take_phase(SD_VERIFY_PROBE)->ok);
    TEST_ASSERT_FALSE(take_phase(SD_VERIFY_FORMAT)->ok);
    TEST_ASSERT_FALSE(take_verdict("sd_verify"));
}

/* The internal partition's row is unchanged: one scratch file, judged on the
 * spot, and no formatting of anything. */
TEST_CASE("the internal partition's self-test is still one scratch file",
          "[storage_ao]")
{
    sd_script_fresh();
    storage_request(KDRV_MEDIUM_FS, STORAGE_OP_SELFTEST);
    (void)qf_support_pump(AO_Storage, 4U);

    const QEvt * const e = ao_spy_take();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(TEST_RESULT_SIG, (int)e->sig);
    TEST_ASSERT_EQUAL_STRING("fs", ((const TestResultEvt *)e)->name);
    TEST_ASSERT_TRUE(((const TestResultEvt *)e)->ok);
}

/* ---- A blocked task's cat, from an offset ---------------------------------- */

/** @brief Bytes of text the offset-cat cases give their waiter. */
#define CAT_CAP  16U

/**
 * @brief Ask for a cat of /storage/digits.txt into a fresh waiter and settle.
 * @param w      The waiter to fill; its semaphore is created here.
 * @param text   Where the bytes land, at least CAT_CAP.
 * @param offset First byte to read.
 * @param max    Ceiling on bytes read.
 */
static void ask_cat_wait(StorageWaiter *w, char *text, uint32_t offset,
                         uint32_t max)
{
    (void)memset(w, 0, sizeof(*w));
    (void)memset(text, 0, CAT_CAP);
    w->done = khal_sem_create();
    w->text = text;
    w->cap  = CAT_CAP;
    w->rc   = 0xFFU;

    storage_cat_request_wait(w, 1U, "/storage/digits.txt", offset, max);
    settle_card();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, w->ack, "AO_Storage never answered");
}

TEST_CASE("a waiter cat reads from the offset it was given", "[storage_ao]")
{
    StorageWaiter w;
    char          text[CAT_CAP];

    storage_fresh();
    fake_media_put_fs_file("digits.txt", "0123456789");

    ask_cat_wait(&w, text, 4U, 3U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)w.rc);
    TEST_ASSERT_EQUAL_UINT32(3U, w.got);
    TEST_ASSERT_EQUAL_STRING("456", text);
    TEST_ASSERT_TRUE(w.cut);
}

TEST_CASE("a waiter cat that reaches the end is not cut", "[storage_ao]")
{
    StorageWaiter w;
    char          text[CAT_CAP];

    storage_fresh();
    fake_media_put_fs_file("digits.txt", "0123456789");

    ask_cat_wait(&w, text, 0U, 10U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)w.rc);
    TEST_ASSERT_EQUAL_UINT32(10U, w.got);
    TEST_ASSERT_EQUAL_STRING("0123456789", text);
    TEST_ASSERT_FALSE(w.cut);

    /* The tail from the middle ends exactly at the end too. */
    ask_cat_wait(&w, text, 6U, 10U);
    TEST_ASSERT_EQUAL_UINT32(4U, w.got);
    TEST_ASSERT_EQUAL_STRING("6789", text);
    TEST_ASSERT_FALSE(w.cut);
}

TEST_CASE("a waiter cat past the end gets nothing and is not cut",
          "[storage_ao]")
{
    StorageWaiter w;
    char          text[CAT_CAP];

    storage_fresh();
    fake_media_put_fs_file("digits.txt", "0123456789");

    ask_cat_wait(&w, text, 20U, 10U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)w.rc);
    TEST_ASSERT_EQUAL_UINT32(0U, w.got);
    TEST_ASSERT_FALSE(w.cut);
}

/* ---- Opening by absolute path ---------------------------------------------- */

TEST_CASE("an absolute path opens that file on that volume, not the card's",
          "[storage_ao]")
{
    storage_fresh();
    fake_psram_reset();
    fake_media_set_sd_present(true);
    fake_media_put_sd_path("kode/kode.json", "{\"sd\":1}", 8U);
    fake_media_put_fs_path("kode/kode.json", "{\"fs\":22}", 9U);

    ask_open("/storage/kode/kode.json");

    const QEvt * const e = ao_spy_take();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(STORAGE_OPEN_RSP_SIG, (int)e->sig);
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)((const StorageOpenRspEvt *)e)->status);
    TEST_ASSERT_EQUAL_UINT32(9U, ((const StorageOpenRspEvt *)e)->size);
    TEST_ASSERT_EQUAL_INT(KDRV_MEDIUM_FS, (int)((const StorageOpenRspEvt *)e)->medium);
    fake_media_set_sd_present(false);
}

TEST_CASE("an absolute path that is not there is an error, with no fallback",
          "[storage_ao]")
{
    storage_fresh();
    fake_psram_reset();
    fake_media_set_sd_present(true);
    fake_media_put_sd_path("kode/kode.json", "{\"sd\":1}", 8U);

    ask_open("/storage/kode/kode.json");

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, (int)take_open_status());
    fake_media_set_sd_present(false);
}

/* ---- The card's worker ------------------------------------------------------ */

/* What waits on the card is a job on a task of its own, and the AO's step is
 * only ever "hand it over" and "read what came back". The cases below pin the
 * seam: which calls are jobs, what the AO does with the requests that arrive
 * while one is out, and what it does with an answer it is no longer waiting
 * for. */

/** @brief Take the card out of its volume, the way a case starts with one
 *         that has to be mounted: setUp leaves it mounted. */
static void card_out(void)
{
    kdrv_storage_unmount(KDRV_MEDIUM_SD);
    TEST_ASSERT_FALSE(kdrv_storage_mounted(KDRV_MEDIUM_SD));
}

/** @brief Ask for the card to be mounted, answered to the spy, and let the AO
 *         hand the call to its worker - without running it. */
static void ask_mount_job(void)
{
    TEST_ASSERT_TRUE(storage_request_answered(KDRV_MEDIUM_SD, STORAGE_OP_MOUNT,
                                              ao_spy()));
    (void)qf_support_pump(AO_Storage, 4U);
}

/** @brief Take the next event and assert it is the command answer. */
static void take_cmd_rsp(void)
{
    const QEvt * const e = ao_spy_take();

    TEST_ASSERT_NOT_NULL_MESSAGE(e, "the command was not answered");
    TEST_ASSERT_EQUAL_INT(STORAGE_CMD_RSP_SIG, (int)e->sig);
}

TEST_CASE("AO_Storage starts its card worker on a PSRAM stack",
          "[storage_ao][worker]")
{
    storage_fresh();

    TEST_ASSERT_TRUE(fake_khal_worker_last_psram_for(SD_WORKER));
    TEST_ASSERT_EQUAL_UINT(0U, fake_khal_worker_pending_for(SD_WORKER));
}

TEST_CASE("an sd mount is a job on the card worker, answered when it returns",
          "[storage_ao][worker]")
{
    storage_fresh();
    card_out();
    TEST_ASSERT_FALSE(kdrv_storage_mounted(KDRV_MEDIUM_SD));

    ask_mount_job();

    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_khal_worker_pending_for(SD_WORKER),
                                   "the mount was not handed to the worker");
    TEST_ASSERT_FALSE_MESSAGE(kdrv_storage_mounted(KDRV_MEDIUM_SD),
                              "the AO mounted the card in its own step");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "the asker was told before the call returned");

    TEST_ASSERT_TRUE(run_sd_job());

    TEST_ASSERT_TRUE(kdrv_storage_mounted(KDRV_MEDIUM_SD));
    take_cmd_rsp();
}

TEST_CASE("an sd unmount is a job, and an open file keeps the card mounted",
          "[storage_ao][worker]")
{
    storage_fresh();
    fake_media_put_sd_path("held.bin", "x", 1U);
    ask_mount_job();
    (void)run_sd_job();
    ao_spy_drain();
    TEST_ASSERT_TRUE(kdrv_storage_mounted(KDRV_MEDIUM_SD));

    /* Idle: the unmount is a call, and the card is up until it returns. */
    TEST_ASSERT_TRUE(storage_request_answered(KDRV_MEDIUM_SD,
                                              STORAGE_OP_UNMOUNT, ao_spy()));
    (void)qf_support_pump(AO_Storage, 4U);
    TEST_ASSERT_EQUAL_UINT(1U, fake_khal_worker_pending_for(SD_WORKER));
    TEST_ASSERT_TRUE(kdrv_storage_mounted(KDRV_MEDIUM_SD));
    TEST_ASSERT_TRUE(run_sd_job());
    TEST_ASSERT_FALSE(kdrv_storage_mounted(KDRV_MEDIUM_SD));
    take_cmd_rsp();

    /* A file open on the card: the unmount is refused and no call is made,
     * so nothing is inside the VFS when the card would have gone. */
    ask_mount_job();
    (void)run_sd_job();
    ao_spy_drain();
    ask_open("/sdcard/held.bin");
    TEST_ASSERT_EQUAL_INT(KODE_OK, take_open_status());

    TEST_ASSERT_TRUE(storage_request_answered(KDRV_MEDIUM_SD,
                                              STORAGE_OP_UNMOUNT, ao_spy()));
    (void)qf_support_pump(AO_Storage, 4U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_khal_worker_pending_for(SD_WORKER),
                                   "an unmount was made under an open file");
    TEST_ASSERT_TRUE(kdrv_storage_mounted(KDRV_MEDIUM_SD));
    take_cmd_rsp();
}

TEST_CASE("a card request that arrives while a call to the card is out is "
          "held, then served", "[storage_ao][worker]")
{
    storage_fresh();
    ask_mount_job();

    storage_path_request(ao_spy(), (uint8_t)STORAGE_PATH_STAT, "/sdcard/x");
    (void)qf_support_pump(AO_Storage, 4U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "the card was asked about during the call");

    TEST_ASSERT_TRUE(run_sd_job());
    settle_card();

    take_cmd_rsp();
    const QEvt * const e = ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "the held request was never served");
    TEST_ASSERT_EQUAL_INT(STORAGE_PATH_RSP_SIG, (int)e->sig);
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)((const StoragePathRspEvt *)e)->rc);
}

TEST_CASE("a request for the internal partition is served while a call to "
          "the card is out", "[storage_ao][worker]")
{
    storage_fresh();
    ask_mount_job();

    storage_path_request(ao_spy(), (uint8_t)STORAGE_PATH_STAT, "/storage/x");
    (void)qf_support_pump(AO_Storage, 4U);

    const QEvt * const e = ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "the partition waited for the card");
    TEST_ASSERT_EQUAL_INT(STORAGE_PATH_RSP_SIG, (int)e->sig);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_khal_worker_pending_for(SD_WORKER),
                                   "the call to the card was not left alone");
    TEST_ASSERT_TRUE(run_sd_job());
    take_cmd_rsp();
}

/* A rescan posts the card's listing and the partition's behind it, and the
 * last one ends the burst. The partition's must not end it first. */
TEST_CASE("a listing behind a held one keeps its place", "[storage_ao][worker]")
{
    storage_fresh();
    ao_spy_subscribe(STORAGE_LIST_END_SIG);
    ask_mount_job();

    storage_list_one(KDRV_MEDIUM_SD, true, false);
    storage_list_one(KDRV_MEDIUM_FS, true, true);
    (void)qf_support_pump(AO_Storage, 8U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "the burst ended ahead of the card's part");

    TEST_ASSERT_TRUE(run_sd_job());
    settle_card();

    take_cmd_rsp();
    const QEvt * const e = ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "the burst never ended");
    TEST_ASSERT_EQUAL_INT(STORAGE_LIST_END_SIG, (int)e->sig);
}

TEST_CASE("a card request over the hold queue's room is refused as busy, "
          "and answered", "[storage_ao][worker]")
{
    storage_fresh();
    ask_mount_job();

    for ( unsigned i = 0U; KODE_STORAGE_DEFER_DEPTH > i; ++i )
    {
        storage_path_request(ao_spy(), (uint8_t)STORAGE_PATH_STAT, "/sdcard/x");
    }
    (void)qf_support_pump(AO_Storage, 16U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "a request that fits was answered early");

    storage_path_request(ao_spy(), (uint8_t)STORAGE_PATH_STAT, "/sdcard/x");
    (void)qf_support_pump(AO_Storage, 4U);

    const QEvt * const e = ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "the request over the room went unanswered");
    TEST_ASSERT_EQUAL_INT(STORAGE_PATH_RSP_SIG, (int)e->sig);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)((const StoragePathRspEvt *)e)->rc);

    TEST_ASSERT_TRUE(run_sd_job());
    settle_card();
    take_cmd_rsp();
    for ( unsigned i = 0U; KODE_STORAGE_DEFER_DEPTH > i; ++i )
    {
        const QEvt * const r = ao_spy_take();

        TEST_ASSERT_NOT_NULL_MESSAGE(r, "a held request was lost");
        TEST_ASSERT_EQUAL_INT(STORAGE_PATH_RSP_SIG, (int)r->sig);
        TEST_ASSERT_EQUAL_INT(KODE_OK, (int)((const StoragePathRspEvt *)r)->rc);
    }
}

TEST_CASE("a worker with no room leaves the mount refused and answered",
          "[storage_ao][worker]")
{
    storage_fresh();
    card_out();
    fake_khal_worker_refuse_for(SD_WORKER, true);

    ask_mount_job();

    TEST_ASSERT_EQUAL_UINT(0U, fake_khal_worker_pending_for(SD_WORKER));
    TEST_ASSERT_FALSE(kdrv_storage_mounted(KDRV_MEDIUM_SD));
    take_cmd_rsp();
}

/* The card hangs, the deadline passes and the asker is told; the call returns
 * long after, with the card gone, and its answer is nobody's any more. */
TEST_CASE("a result after the card is gone is dropped, and the AO keeps what "
          "it was told last", "[storage_ao][worker]")
{
    unsigned const steps =
        QF_SUPPORT_STEPS(KDRV_SD_MOUNT_MAX_MS + KODE_STORAGE_JOB_MARGIN_MS);

    storage_fresh();
    card_out();
    ask_mount_job();

    (void)qf_support_tick(AO_Storage, steps - 1U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "told too early");
    (void)qf_support_tick(AO_Storage, 1U);
    take_cmd_rsp();

    /* While the call is still out its guard holds: a second mount is held. */
    fake_media_set_sd_present(false);
    TEST_ASSERT_TRUE(storage_request_answered(KDRV_MEDIUM_SD, STORAGE_OP_MOUNT,
                                              ao_spy()));
    (void)qf_support_pump(AO_Storage, 4U);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_khal_worker_pending_for(SD_WORKER),
                                   "a second call queued behind the first");

    /* The late call returns (the card is gone, so it failed). Its own asker
     * has been told; only the held mount answers now, as a fresh call. */
    fake_klog_reset();
    TEST_ASSERT_TRUE(run_sd_job());
    TEST_ASSERT_FALSE(kdrv_storage_mounted(KDRV_MEDIUM_SD));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "a late result answered its asker twice");
    TEST_ASSERT_EQUAL_UINT(1U, fake_khal_worker_pending_for(SD_WORKER));
    TEST_ASSERT_TRUE(run_sd_job());
    take_cmd_rsp();
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    fake_media_set_sd_present(true);
}

TEST_CASE("a done event that is not the call's is dropped by its sequence",
          "[storage_ao][worker]")
{
    KqpWorkerDoneEvt stray;

    storage_fresh();
    card_out();

    /* With no call out. */
    (void)memset(&stray, 0, sizeof(stray));
    stray.super = (QEvt)QEVT_INITIALIZER(STORAGE_SDJOB_DONE_SIG);
    stray.seq    = 0xBEEFU;
    stray.status = (uint8_t)KODE_OK;
    QASM_DISPATCH(AO_Storage, &stray.super, 0U);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    /* With one out, under a sequence number it was not given. */
    ask_mount_job();
    QASM_DISPATCH(AO_Storage, &stray.super, 0U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "a stray answer was taken for the call's");
    TEST_ASSERT_FALSE(kdrv_storage_mounted(KDRV_MEDIUM_SD));

    TEST_ASSERT_TRUE(run_sd_job());
    TEST_ASSERT_TRUE(kdrv_storage_mounted(KDRV_MEDIUM_SD));
    take_cmd_rsp();
}

/* ---- Which calls are jobs, and which stay -------------------------------- */

/* The row's remounts and raw reads are jobs; its test file is not, because
 * create() may place it on the internal partition and a PSRAM stack must not
 * touch that flash. So the count of calls the worker makes is the audit: a
 * remount to start, the four-line chunks, a remount onto one line and its
 * chunks, and a remount back before the file - and no call for the file. */
TEST_CASE("the card row's remounts and raw reads are jobs; its file is not",
          "[storage_ao][worker]")
{
    unsigned const chunks = (KODE_SD_TEST_KB * 1024U) / (32U * 1024U);

    sd_script_fresh();
    ask_sd_test();
    run_sd_script();

    TEST_ASSERT_EQUAL_UINT((2U * chunks) + 3U, sd_jobs_run);
    TEST_ASSERT_EQUAL_UINT(0U, fake_khal_worker_pending_for(SD_WORKER));
    TEST_ASSERT_TRUE(take_phase(SD_TEST_IDENTIFY)->ok);
}

/* sd_verify: the remount, a megabyte written and read back in chunks, the
 * wrap scan's five single-sector reads on a 30000 MB claim, and the format. */
TEST_CASE("sd_verify's probe and format are jobs", "[storage_ao][worker]")
{
    unsigned const chunks = (KODE_SD_TEST_KB * 1024U) / (32U * 1024U);

    sd_script_fresh();
    ask_sd_verify();
    run_sd_script();

    TEST_ASSERT_EQUAL_UINT(1U + (2U * chunks) + 5U + 1U, sd_jobs_run);
    TEST_ASSERT_TRUE(take_phase(SD_VERIFY_PROBE)->ok);
    TEST_ASSERT_TRUE(take_phase(SD_VERIFY_FORMAT)->ok);
    TEST_ASSERT_TRUE(take_verdict("sd_verify"));
}

TEST_CASE("a card row with no room for a call is refused and answered",
          "[storage_ao][worker]")
{
    sd_script_fresh();
    fake_khal_worker_refuse_for(SD_WORKER, true);

    ask_sd_test();
    run_sd_script();

    TEST_ASSERT_FALSE(take_verdict("sd"));
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
}

/* ---- A call that overruns inside a script ----------------------------------- */

/* The deadline passes with the worker still inside the call. The run ends
 * there, with the records a failure makes and a false verdict; nothing runs
 * against the card or reuses its buffers until the call is back; and the next
 * script starts cleanly after that. */
TEST_CASE("a call that overruns inside the card row ends the run with a false "
          "verdict, and the next row runs after the late return",
          "[storage_ao][worker]")
{
    unsigned const steps =
        QF_SUPPORT_STEPS(KDRV_SD_MOUNT_MAX_MS + KODE_STORAGE_JOB_MARGIN_MS);
    unsigned const mounts = fake_sdmmc_mounts();

    sd_script_fresh();
    ask_sd_test();
    TEST_ASSERT_EQUAL_UINT(1U, fake_khal_worker_pending_for(SD_WORKER));

    (void)qf_support_tick(AO_Storage, steps);

    TEST_ASSERT_FALSE_MESSAGE(take_phase(SD_TEST_IDENTIFY)->ok,
                              "the phase that overran was not failed");
    TEST_ASSERT_FALSE(take_verdict("sd"));
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    /* The worker is still inside: nothing touched the card, and nothing that
     * would reuse its buffers or open a file is served. */
    TEST_ASSERT_EQUAL_UINT(mounts, fake_sdmmc_mounts());
    ask_open("/storage/anything.bin");
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, take_open_status(),
                                  "a file was opened under a hung call");
    (void)qf_support_tick(AO_Storage, steps);   /* a pacer left behind is a tick */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "the run went on after its verdict");
    TEST_ASSERT_FALSE(fake_media_file_exists(fake_media_sd_dir(),
                                             ".kodeos_sdtest"));

    /* A row asked meanwhile waits for the call. */
    ask_sd_test();
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    /* The late return: dropped, and the held row starts from a clean AO. */
    TEST_ASSERT_TRUE(run_sd_job());
    run_sd_script();
    TEST_ASSERT_TRUE(take_phase(SD_TEST_IDENTIFY)->ok);
    (void)take_phase(SD_TEST_READ4);
    (void)take_phase(SD_TEST_READ1);
    (void)take_phase(SD_TEST_WRITE);
    (void)take_phase(SD_TEST_VERIFY);
    TEST_ASSERT_TRUE_MESSAGE(take_verdict("sd"),
                             "the AO stayed wedged after the late return");
}

/* An overrun in the one-line read leaves the slot on one data line: the width
 * is put back when the call returns, not skipped. */
TEST_CASE("a call that overruns on one data line gives the board's width back "
          "when it returns", "[storage_ao][worker]")
{
    unsigned const steps =
        QF_SUPPORT_STEPS(KDRV_SD_SECTORS_MAX_MS + KODE_STORAGE_JOB_MARGIN_MS);
    unsigned const chunks = (KODE_SD_TEST_KB * 1024U) / (32U * 1024U);
    int32_t        width  = 0;

    sd_script_fresh();
    ask_sd_test();
    /* The remount, the four-line chunks, the remount onto one line... */
    for ( unsigned i = 0U; (chunks + 2U) > i; ++i )
    {
        for ( unsigned k = 0U;
             (4U > k) && (fake_khal_worker_pending_for(SD_WORKER) == 0U); ++k )
        {
            (void)qf_support_pump(AO_Storage, 8U);
        }
        TEST_ASSERT_TRUE(fake_khal_worker_run_next_for(SD_WORKER));
    }
    /* ...and the first one-line chunk is out. */
    (void)qf_support_pump(AO_Storage, 8U);
    TEST_ASSERT_EQUAL_UINT(1U, fake_khal_worker_pending_for(SD_WORKER));
    (void)kdrv_sd_get(KDRV_SD_BUS_WIDTH, &width);
    TEST_ASSERT_EQUAL_INT32(1, width);
    ao_spy_drain();

    (void)qf_support_tick(AO_Storage, steps);
    TEST_ASSERT_FALSE(take_phase(SD_TEST_READ1)->ok);
    TEST_ASSERT_FALSE_MESSAGE(take_verdict("sd"),
                              "an overrun is the slot's, not a pass");

    TEST_ASSERT_TRUE(run_sd_job());     /* the late chunk comes back... */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_khal_worker_pending_for(SD_WORKER),
                                   "the width was not asked for back");
    TEST_ASSERT_TRUE(run_sd_job());     /* ...and the remount puts it right. */
    (void)kdrv_sd_get(KDRV_SD_BUS_WIDTH, &width);
    TEST_ASSERT_EQUAL_INT32(4, width);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
}

TEST_CASE("sd_verify with a call that overruns ends with a false verdict",
          "[storage_ao][worker]")
{
    unsigned const steps =
        QF_SUPPORT_STEPS(KDRV_SD_MOUNT_MAX_MS + KODE_STORAGE_JOB_MARGIN_MS);

    sd_script_fresh();
    ask_sd_verify();
    (void)qf_support_tick(AO_Storage, steps);

    (void)take_phase(SD_VERIFY_PROBE);
    TEST_ASSERT_FALSE(take_verdict("sd_verify"));
    TEST_ASSERT_TRUE(run_sd_job());
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
}

/* ---- A refused info ----------------------------------------------------------- */

TEST_CASE("an info request over the hold queue's room is answered, not lost",
          "[storage_ao][worker]")
{
    storage_fresh();
    ao_spy_subscribe(STORAGE_INFO_SIG);
    ask_mount_job();
    for ( unsigned i = 0U; KODE_STORAGE_DEFER_DEPTH > i; ++i )
    {
        storage_path_request(ao_spy(), (uint8_t)STORAGE_PATH_STAT, "/sdcard/x");
    }
    (void)qf_support_pump(AO_Storage, 16U);
    ao_spy_drain();

    storage_request(KDRV_MEDIUM_SD, STORAGE_OP_INFO);
    (void)qf_support_pump(AO_Storage, 4U);

    const QEvt * const e = ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "sd_info would wait for the console's "
                                    "watchdog");
    TEST_ASSERT_EQUAL_INT(STORAGE_INFO_SIG, (int)e->sig);
    TEST_ASSERT_FALSE(((const StorageInfoEvt *)e)->usage_known);

    TEST_ASSERT_TRUE(run_sd_job());
    settle_card();
    ao_spy_drain();
}

/* The radio asks for the card's unmount WITH an answer before it resets the C5
 * and reads the card's state when it comes: a request the worker cannot take
 * is answered, and the card is then still mounted. */
TEST_CASE("an unmount the worker cannot take is answered and leaves the card "
          "mounted", "[storage_ao][worker]")
{
    storage_fresh();
    TEST_ASSERT_TRUE(kdrv_storage_mounted(KDRV_MEDIUM_SD));
    fake_khal_worker_refuse_for(SD_WORKER, true);

    TEST_ASSERT_TRUE(storage_request_answered(KDRV_MEDIUM_SD,
                                              STORAGE_OP_UNMOUNT, ao_spy()));
    (void)qf_support_pump(AO_Storage, 4U);

    take_cmd_rsp();
    TEST_ASSERT_TRUE(kdrv_storage_mounted(KDRV_MEDIUM_SD));
}

/* The remount that hands the width back at the end of a run overruns: every
 * phase has been judged, so no second record is made for a phase that passed
 * (here the identify's, already published), and the width stays owed - also
 * when the call that pays it overruns in turn. */
TEST_CASE("an overrun of the final width restore adds no record and keeps the "
          "width owed", "[storage_ao][worker]")
{
    unsigned const steps =
        QF_SUPPORT_STEPS(KDRV_SD_MOUNT_MAX_MS + KODE_STORAGE_JOB_MARGIN_MS);

    sd_script_fresh();
    fake_sdmmc_set_max_width(1U);       /* the identify fails; the width is 1 */
    ask_sd_test();

    TEST_ASSERT_TRUE(run_sd_job());     /* the script's remount */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_khal_worker_pending_for(SD_WORKER),
                                   "the width restore was not asked for");
    TEST_ASSERT_FALSE(take_phase(SD_TEST_IDENTIFY)->ok);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    (void)qf_support_tick(AO_Storage, steps);   /* the restore overruns */
    TEST_ASSERT_FALSE(take_verdict("sd"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "a phase that had passed was failed again");

    /* It returns late; the width is asked for again, and that overruns too. */
    TEST_ASSERT_TRUE(run_sd_job());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_khal_worker_pending_for(SD_WORKER),
                                   "the owed width was not asked for");
    (void)qf_support_tick(AO_Storage, steps);
    TEST_ASSERT_TRUE(run_sd_job());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_khal_worker_pending_for(SD_WORKER),
                                   "the width was forgotten after an overrun "
                                   "of its own restore");
    TEST_ASSERT_TRUE(run_sd_job());
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
}

/* A page walks the card, so it waits behind a call to the card like every
 * other card request - the first one comes right behind the remount
 * storage_list_request() asks for. Served at once, it would walk the card
 * while the worker unmounts it. */
TEST_CASE("a page of the image listing waits behind a card call",
          "[storage_ao][worker][pages]")
{
    storage_fresh();
    make_sd_file("a.bin");
    ask_mount_job();

    storage_list_page(0U, ao_spy());
    (void)qf_support_pump(AO_Storage, 8U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "the page was served while the card was busy");

    TEST_ASSERT_TRUE(run_sd_job());
    settle_card();
    take_cmd_rsp();

    StorageEntryEvt const * const en = (StorageEntryEvt const *)ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(en, "the held page never came");
    TEST_ASSERT_EQUAL_INT(STORAGE_ENTRY_SIG, (int)en->super.sig);
    TEST_ASSERT_EQUAL_STRING("a.bin", en->name);
    ListEndEvt const * const end = (ListEndEvt const *)ao_spy_take();
    TEST_ASSERT_NOT_NULL(end);
    TEST_ASSERT_EQUAL_INT(STORAGE_LIST_END_SIG, (int)end->super.sig);
    TEST_ASSERT_FALSE(end->more);
    ao_spy_drain();
}

TEST_CASE("a page over the hold queue's room is answered with its end",
          "[storage_ao][worker][pages]")
{
    storage_fresh();
    make_sd_file("a.bin");   /* served now instead, the page would carry it */
    ask_mount_job();

    for ( unsigned i = 0U; KODE_STORAGE_DEFER_DEPTH > i; ++i )
    {
        storage_path_request(ao_spy(), (uint8_t)STORAGE_PATH_STAT, "/sdcard/x");
    }
    storage_list_page(0U, ao_spy());
    (void)qf_support_pump(AO_Storage, 16U);

    ListEndEvt const * const end = (ListEndEvt const *)ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(end, "the page over the room went unanswered");
    TEST_ASSERT_EQUAL_INT_MESSAGE(STORAGE_LIST_END_SIG, (int)end->super.sig,
                                  "the page walked the busy card instead");
    TEST_ASSERT_FALSE(end->more);

    TEST_ASSERT_TRUE(run_sd_job());
    settle_card();
    take_cmd_rsp();
    ao_spy_drain();
}

/* A page that names nobody has nobody to answer: refused, it must not
 * publish its end to every subscriber either. */
TEST_CASE("a refused page that names nobody sends no end at all",
          "[storage_ao][worker][pages]")
{
    storage_fresh();
    ask_mount_job();

    for ( unsigned i = 0U; KODE_STORAGE_DEFER_DEPTH > i; ++i )
    {
        storage_path_request(ao_spy(), (uint8_t)STORAGE_PATH_STAT, "/sdcard/x");
    }
    ao_spy_subscribe(STORAGE_LIST_END_SIG);
    storage_list_page(0U, NULL);
    (void)qf_support_pump(AO_Storage, 16U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "an end went out with nobody to send it to");
    QActive_unsubscribe(ao_spy(), STORAGE_LIST_END_SIG);

    TEST_ASSERT_TRUE(run_sd_job());
    settle_card();
    take_cmd_rsp();
    ao_spy_drain();
}

/* ---- kode_sys's requests: answered to the asking AO ---------------------------
 * The file requests post and return; AO_Storage posts one answer to the AO that
 * asked, the spy here, its status inside. A read's buffer is lent and written
 * before the answer, a write's data read before it; a refusal touches neither.
 */

/** @brief A static event AO_Storage ignores, to fill a queue with. */
static const QEvt s_sys_filler = QEVT_INITIALIZER(IMU_TEST_REQ_SIG);

/** @brief What a lent buffer holds before the owner touches it. */
#define SYS_LENT_FILL  0x5AU

/** @brief Most blocks a case takes out of a pool to leave it dry. */
#define SYS_DRY_MAX  256U

/** @brief The bytes of the lent buffers below. */
#define SYS_BUF  16U

/** @brief Files on a page and one more: what a second page has to bring. */
#define SYS_PAGE_PLUS  (KODE_STORAGE_LIST_MAX + 3U)

/** @brief A fresh AO_Storage with both askers' mailboxes empty. */
static void sys_fresh(void)
{
    storage_fresh();
    ao_spy_drain();
    ao_spy_other_drain();
}

/** @brief The next answer the spy holds, which must be @p sig. */
static const FileRspEvt *sys_take(enum_t sig)
{
    const QEvt *e = ao_spy_take();

    TEST_ASSERT_NOT_NULL_MESSAGE(e, "every accepted request is answered");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)sig, (uint32_t)e->sig, "the wrong answer");
    return (const FileRspEvt *)(const void *)e;
}

TEST_CASE("kode_sys_file_status answers the asking AO once, and its door opens again",
          "[storage_ao][ask]")
{
    const FileRspEvt *a;

    sys_fresh();
    make_file("here.bin", 7U);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_file_status(ao_spy(), "/storage/here.bin"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, kode_sys_file_status(ao_spy(), "/storage/here.bin"),
                                  "a second request of the kind waits for the first's answer");
    settle_card();
    a = sys_take(FILE_STATUS_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_TRUE(a->stat.exists);
    TEST_ASSERT_FALSE(a->stat.is_dir);
    TEST_ASSERT_EQUAL_UINT32(7U, a->stat.size);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "posted once, not published as well");

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_file_status(ao_spy(), "/storage/gone.bin"),
                                  "the answer opens the door again");
    settle_card();
    a = sys_take(FILE_STATUS_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT_MESSAGE((uint32_t)KODE_OK, a->rc, "nothing there is an answer");
    TEST_ASSERT_FALSE(a->stat.exists);
    ao_spy_drain();
}

TEST_CASE("two AOs asking a file's status are both accepted and both answered",
          "[storage_ao][ask]")
{
    const QEvt *o;

    sys_fresh();
    make_file("both.bin", 3U);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_file_status(ao_spy(), "/storage/both.bin"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_file_status(ao_spy_other(), "/storage/both.bin"),
                                  "the door is the asker's, not the kind's");
    settle_card();
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, sys_take(FILE_STATUS_RSP_SIG)->rc);
    o = ao_spy_other_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(o, "the second asker is answered too");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)FILE_STATUS_RSP_SIG, (uint32_t)o->sig);
    TEST_ASSERT_EQUAL_UINT32(3U, ((const FileRspEvt *)(const void *)o)->stat.size);
    QF_gc(o);
    TEST_ASSERT_NULL(ao_spy_other_take());
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    ao_spy_drain();
}

TEST_CASE("a listing arrives as entries then its end, posted to the asker and published nowhere",
          "[storage_ao][ask]")
{
    const FileRspEvt *end;
    unsigned          entries = 0U;
    const QEvt       *e;

    sys_fresh();
    fake_media_put_fs_dir("box");
    fake_media_put_fs_path("box/a.txt", "a", 1U);
    fake_media_put_fs_path("box/b.txt", "bb", 2U);
    fake_media_put_fs_dir("box/sub");
    /* Subscribed, so a published entry would arrive twice. */
    ao_spy_subscribe(STORAGE_ENTRY_SIG);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_file_list(ao_spy(), "/storage/box", 0U));
    settle_card();
    while ( ((e = ao_spy_take()) != NULL) && ((QSignal)STORAGE_ENTRY_SIG == e->sig) )
    {
        TEST_ASSERT_EQUAL_UINT16((uint16_t)entries, ((const StorageEntryEvt *)(const void *)e)->pos);
        entries++;
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "the listing has no end");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)FILE_LIST_END_SIG, (uint32_t)e->sig);
    end = (const FileRspEvt *)(const void *)e;
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, end->rc);
    TEST_ASSERT_EQUAL_UINT(3U, entries);
    TEST_ASSERT_EQUAL_UINT16(3U, end->count);
    TEST_ASSERT_FALSE(end->more);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "an entry was published as well");
    ao_spy_drain();
}

TEST_CASE("a listing longer than a page says more, and the next page brings the rest",
          "[storage_ao][ask]")
{
    char              name[32];
    const FileRspEvt *end;
    unsigned          entries = 0U;
    const QEvt       *e;

    sys_fresh();
    fake_media_put_fs_dir("many");
    for ( unsigned i = 0U; SYS_PAGE_PLUS > i; ++i )
    {
        (void)snprintf(name, sizeof(name), "many/f%02u.txt", i);
        fake_media_put_fs_path(name, "x", 1U);
    }

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_file_list(ao_spy(), "/storage/many", 0U));
    settle_card();
    while ( ((e = ao_spy_take()) != NULL) && ((QSignal)STORAGE_ENTRY_SIG == e->sig) )
    {
        entries++;
    }
    TEST_ASSERT_NOT_NULL(e);
    end = (const FileRspEvt *)(const void *)e;
    TEST_ASSERT_EQUAL_UINT(KODE_STORAGE_LIST_MAX, entries);
    TEST_ASSERT_EQUAL_UINT16(KODE_STORAGE_LIST_MAX, end->count);
    TEST_ASSERT_TRUE(end->more);
    ao_spy_drain();

    entries = 0U;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_file_list(ao_spy(), "/storage/many",
                                                      (uint16_t)KODE_STORAGE_LIST_MAX));
    settle_card();
    while ( ((e = ao_spy_take()) != NULL) && ((QSignal)STORAGE_ENTRY_SIG == e->sig) )
    {
        TEST_ASSERT_EQUAL_UINT16((uint16_t)(KODE_STORAGE_LIST_MAX + entries),
                                 ((const StorageEntryEvt *)(const void *)e)->pos);
        entries++;
    }
    TEST_ASSERT_NOT_NULL(e);
    end = (const FileRspEvt *)(const void *)e;
    TEST_ASSERT_EQUAL_UINT(SYS_PAGE_PLUS - KODE_STORAGE_LIST_MAX, entries);
    TEST_ASSERT_FALSE(end->more);
    ao_spy_drain();
}

/** @brief Events put in the spy's queue ahead of a listing, to make it a small one. */
#define SYS_QUEUE_TAKEN  20U

/** @brief Entries that fit the spy's queue past SYS_QUEUE_TAKEN, the free slots kept. */
#define SYS_SMALL_PAGE   (AO_SPY_QUEUE_LEN - SYS_QUEUE_TAKEN - KODE_SYS_LIST_FREE_SLOTS)

TEST_CASE("a listing keeps the documented free slots in a small queue and ends its page early",
          "[storage_ao][ask]")
{
    char              name[32];
    uint32_t          entries = 0U;
    uint32_t          fillers = 0U;
    const QEvt       *e;
    const FileRspEvt *end = NULL;
    const FileRspEvt *st  = NULL;

    sys_fresh();
    fake_media_put_fs_dir("deep");
    for ( uint32_t i = 0U; SYS_PAGE_PLUS > i; ++i )
    {
        (void)snprintf(name, sizeof(name), "deep/f%02u.txt", (unsigned)i);
        fake_media_put_fs_path(name, "x", 1U);
    }
    for ( uint32_t i = 0U; SYS_QUEUE_TAKEN > i; ++i )
    {
        QACTIVE_POST(ao_spy(), &s_sys_filler, NULL);
    }

    /* A second answer in flight beside the listing: it needs one of the free slots. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_file_list(ao_spy(), "/storage/deep", 0U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_file_status(ao_spy(), "/storage/deep"));
    settle_card();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(KODE_SYS_LIST_FREE_SLOTS - 2U,
                                     (uint32_t)uxQueueSpacesAvailable(ao_spy()->eQueue),
                                     "the listing took a slot it promised to leave free");

    while ( (e = ao_spy_take()) != NULL )
    {
        if ( (QSignal)STORAGE_ENTRY_SIG == e->sig )
        {
            entries++;
        }
        else if ( (QSignal)FILE_LIST_END_SIG == e->sig )
        {
            end = (const FileRspEvt *)(const void *)e;
        }
        else if ( (QSignal)FILE_STATUS_RSP_SIG == e->sig )
        {
            st = (const FileRspEvt *)(const void *)e;
        }
        else
        {
            fillers++;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(SYS_QUEUE_TAKEN, fillers);
    TEST_ASSERT_NOT_NULL_MESSAGE(end, "the end was lost");
    TEST_ASSERT_NOT_NULL_MESSAGE(st, "the second answer was lost");
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, end->rc);
    TEST_ASSERT_TRUE_MESSAGE(end->more, "a page cut short says more");
    TEST_ASSERT_EQUAL_UINT32(SYS_SMALL_PAGE, entries);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)entries, end->count);
    TEST_ASSERT_TRUE(st->stat.is_dir);
    ao_spy_drain();

    /* The next page continues where this one stopped. */
    entries = 0U;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_file_list(ao_spy(), "/storage/deep", (uint16_t)SYS_SMALL_PAGE));
    settle_card();
    while ( ((e = ao_spy_take()) != NULL) && ((QSignal)STORAGE_ENTRY_SIG == e->sig) )
    {
        TEST_ASSERT_EQUAL_UINT16((uint16_t)(SYS_SMALL_PAGE + entries),
                                 ((const StorageEntryEvt *)(const void *)e)->pos);
        entries++;
    }
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT32(SYS_PAGE_PLUS - SYS_SMALL_PAGE, entries);
    TEST_ASSERT_FALSE(((const FileRspEvt *)(const void *)e)->more);
    ao_spy_drain();
}

TEST_CASE("a listing into a queue no deeper than the free slots brings nothing and says so",
          "[storage_ao][ask]")
{
    char              name[32];
    const QEvt       *e;
    const FileRspEvt *end;

    sys_fresh();
    fake_media_put_fs_dir("deep");
    for ( uint32_t i = 0U; SYS_PAGE_PLUS > i; ++i )
    {
        (void)snprintf(name, sizeof(name), "deep/f%02u.txt", (unsigned)i);
        fake_media_put_fs_path(name, "x", 1U);
    }

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_file_list(ao_spy_other(), "/storage/deep", 0U));
    settle_card();
    e = ao_spy_other_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "the end was lost to a small queue");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)FILE_LIST_END_SIG, (uint32_t)e->sig);
    end = (const FileRspEvt *)(const void *)e;
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_NO_RESOURCES, end->rc);
    TEST_ASSERT_EQUAL_UINT16(0U, end->count);
    TEST_ASSERT_TRUE(end->more);
    QF_gc(e);
    TEST_ASSERT_NULL(ao_spy_other_take());
}

TEST_CASE("a listing of what is not a directory is refused, with no entries",
          "[storage_ao][ask]")
{
    sys_fresh();
    make_file("flat.bin", 2U);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_file_list(ao_spy(), "/storage/flat.bin", 0U));
    settle_card();
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_DENIED, sys_take(FILE_LIST_END_SIG)->rc);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    ao_spy_drain();
}

TEST_CASE("a folder is made, renamed and deleted, each answered to the asker",
          "[storage_ao][ask]")
{
    sys_fresh();
    make_file("doomed.bin", 4U);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_file_create_folder(ao_spy(), "/storage/made"));
    settle_card();
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, sys_take(FILE_FOLDER_RSP_SIG)->rc);
    TEST_ASSERT_TRUE(fake_media_file_exists(fake_media_fs_dir(), "made"));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_file_rename(ao_spy(), "/storage/made", "/storage/kept"));
    settle_card();
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, sys_take(FILE_RENAME_RSP_SIG)->rc);
    TEST_ASSERT_TRUE(fake_media_file_exists(fake_media_fs_dir(), "kept"));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_file_delete(ao_spy(), "/storage/kept"));
    settle_card();
    TEST_ASSERT_EQUAL_UINT_MESSAGE((uint32_t)KODE_OK, sys_take(FILE_DELETE_RSP_SIG)->rc,
                                   "an empty directory goes");
    TEST_ASSERT_FALSE(fake_media_file_exists(fake_media_fs_dir(), "kept"));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_file_delete(ao_spy(), "/storage/doomed.bin"));
    settle_card();
    TEST_ASSERT_EQUAL_UINT_MESSAGE((uint32_t)KODE_OK, sys_take(FILE_DELETE_RSP_SIG)->rc,
                                   "a file goes");
    TEST_ASSERT_FALSE(fake_media_file_exists(fake_media_fs_dir(), "doomed.bin"));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_file_delete(ao_spy(), "/storage/never.bin"));
    settle_card();
    TEST_ASSERT_NOT_EQUAL_UINT_MESSAGE((uint32_t)KODE_OK, sys_take(FILE_DELETE_RSP_SIG)->rc,
                                       "nothing there is a failure, answered");
    ao_spy_drain();
}

TEST_CASE("a read fills the lent window before its answer, and a refused one never touches it",
          "[storage_ao][ask]")
{
    uint8_t           buf[SYS_BUF];
    uint8_t           sentinel[SYS_BUF];
    const FileRspEvt *a;

    sys_fresh();
    fake_media_put_fs_file("digits.txt", "0123456789");
    (void)memset(buf, SYS_LENT_FILL, sizeof(buf));
    (void)memcpy(sentinel, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_file_read(ao_spy(), "/storage/digits.txt", 4U, buf, 3U));
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(sentinel, buf, sizeof(buf), "written before AO_Storage ran");
    settle_card();
    a = sys_take(FILE_READ_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_UINT32(3U, a->got);
    TEST_ASSERT_EQUAL_MEMORY("456", buf, 3U);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&sentinel[3], &buf[3], sizeof(buf) - 3U,
                                     "written past the window it lent");
    ao_spy_drain();

    /* A file that is not there: answered, the buffer as it was. */
    (void)memcpy(buf, sentinel, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_file_read(ao_spy(), "/storage/none.txt", 0U, buf, SYS_BUF));
    settle_card();
    a = sys_take(FILE_READ_RSP_SIG);
    TEST_ASSERT_NOT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_UINT32(0U, a->got);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(sentinel, buf, sizeof(buf), "a refused read wrote the buffer");
    ao_spy_drain();

    /* Busy with a transfer: refused, the buffer as it was. */
    make_file("held.bin", 16U);
    ask_open("held.bin");
    TEST_ASSERT_EQUAL_INT(KODE_OK, take_open_status());
    ao_spy_drain();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_file_read(ao_spy(), "/storage/digits.txt", 0U, buf, SYS_BUF));
    settle_card();
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_DENIED, sys_take(FILE_READ_RSP_SIG)->rc);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(sentinel, buf, sizeof(buf), "a refused read wrote the buffer");
    storage_close_request();
    (void)qf_support_pump(AO_Storage, 8U);
    ao_spy_drain();
}

TEST_CASE("an empty file reads as zero bytes, not as a refusal", "[storage_ao][ask]")
{
    uint8_t           buf[SYS_BUF];
    uint8_t           sentinel[SYS_BUF];
    const FileRspEvt *a;
    StorageWaiter     w;
    char              text[CAT_CAP];

    sys_fresh();
    fake_media_put_fs_file("empty.bin", "");
    (void)memset(buf, SYS_LENT_FILL, sizeof(buf));
    (void)memcpy(sentinel, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_file_read(ao_spy(), "/storage/empty.bin", 0U, buf, SYS_BUF));
    settle_card();
    a = sys_take(FILE_READ_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_UINT32(0U, a->got);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(sentinel, buf, sizeof(buf), "an empty read wrote the buffer");
    ao_spy_drain();

    /* A blocked task's cat of it: the same answer, and not cut. */
    (void)memset(&w, 0, sizeof(w));
    (void)memset(text, 0, sizeof(text));
    w.done = khal_sem_create();
    w.text = text;
    w.cap  = CAT_CAP;
    w.rc   = 0xFFU;
    storage_cat_request_wait(&w, 1U, "/storage/empty.bin", 0U, CAT_CAP);
    settle_card();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, w.ack, "AO_Storage never answered");
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)w.rc);
    TEST_ASSERT_EQUAL_UINT32(0U, w.got);
    TEST_ASSERT_FALSE(w.cut);
    ao_spy_drain();
}

TEST_CASE("a write puts the lent bytes in the file, appends, and leaves them as they were",
          "[storage_ao][ask]")
{
    static const char data[] = "hello";
    static const char more[] = "!!";
    uint8_t           back[SYS_BUF];
    const FileRspEvt *a;

    sys_fresh();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_file_write(ao_spy(), "/storage/out.txt", data, 5U, false));
    settle_card();
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, sys_take(FILE_WRITE_RSP_SIG)->rc);
    TEST_ASSERT_EQUAL_STRING("hello", data);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_file_write(ao_spy(), "/storage/out.txt", more, 2U, true));
    settle_card();
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, sys_take(FILE_WRITE_RSP_SIG)->rc);
    ao_spy_drain();

    (void)memset(back, 0, sizeof(back));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_file_read(ao_spy(), "/storage/out.txt", 0U, back, SYS_BUF));
    settle_card();
    a = sys_take(FILE_READ_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT32(7U, a->got);
    TEST_ASSERT_EQUAL_MEMORY("hello!!", back, 7U);
    ao_spy_drain();
}

TEST_CASE("with no card, a file request on the card is answered with a status",
          "[storage_ao][ask][worker]")
{
    sys_fresh();
    fake_media_set_sd_present(false);
    storage_request(KDRV_MEDIUM_SD, STORAGE_OP_UNMOUNT);
    settle_card();
    ao_spy_drain();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_file_create_folder(ao_spy(), "/sdcard/nope"));
    settle_card();
    TEST_ASSERT_NOT_EQUAL_UINT((uint32_t)KODE_OK, sys_take(FILE_FOLDER_RSP_SIG)->rc);
    ao_spy_drain();
    fake_media_set_sd_present(true);
}

TEST_CASE("a plain task's or a badly asked file request is refused at once and posts nothing",
          "[storage_ao][ask]")
{
    char    longpath[KODE_SYS_FILE_PATH_MAX + 8U];
    uint8_t buf[SYS_BUF];

    sys_fresh();
    (void)memset(longpath, 'a', sizeof(longpath));
    longpath[0] = '/';
    longpath[sizeof(longpath) - 1U] = '\0';

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_file_status(NULL, "/storage/x"));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_file_status(ao_spy(), NULL));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_file_status(ao_spy(), longpath));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_file_list(NULL, "/storage", 0U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_file_create_folder(ao_spy(), ""));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_file_delete(NULL, "/storage/x"));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_file_rename(ao_spy(), "/storage/x", NULL));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_file_read(ao_spy(), "/storage/x", 0U, NULL, SYS_BUF));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_file_read(ao_spy(), "/storage/x", 0U, buf, 0U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_file_read(ao_spy(), "/storage/x", 0U, buf,
                                                         KODE_SYS_FILE_READ_MAX + 1U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_file_write(ao_spy(), "/storage/x", NULL, 1U, false));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_file_write(ao_spy(), "/storage/x", buf,
                                                          KODE_SYS_FILE_READ_MAX + 1U, false));
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Storage->eQueue));

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_file_status(ao_spy(), "/storage/x"),
                                  "a refusal takes no door");
    settle_card();
    (void)sys_take(FILE_STATUS_RSP_SIG);
    ao_spy_drain();
}

TEST_CASE("a full AO_Storage queue refuses kode_sys's file requests with no resources",
          "[storage_ao][ask]")
{
    uint8_t buf[SYS_BUF];

    sys_fresh();
    TEST_ASSERT_TRUE_MESSAGE(qf_support_queue_stuff(AO_Storage, &s_sys_filler), "the queue never filled");
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_file_status(ao_spy(), "/storage/x"));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_file_list(ao_spy(), "/storage", 0U));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_file_rename(ao_spy(), "/storage/x", "/storage/y"));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_file_read(ao_spy(), "/storage/x", 0U, buf, SYS_BUF));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_file_write(ao_spy(), "/storage/x", buf, 1U, false));
    (void)qf_support_settle(AO_Storage);

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_file_status(ao_spy(), "/storage/x"),
                                  "nothing was posted, so the door is open");
    settle_card();
    (void)sys_take(FILE_STATUS_RSP_SIG);
    ao_spy_drain();
}

TEST_CASE("a dry pool refuses kode_sys's file request with no resources and posts nothing",
          "[storage_ao][ask]")
{
    static FileRspEvt *held[SYS_DRY_MAX];
    uint32_t           n = 0U;

    sys_fresh();
    while ( SYS_DRY_MAX > n )
    {
        held[n] = Q_NEW_X(FileRspEvt, 0U, FILE_STATUS_RSP_SIG);
        if ( NULL == held[n] )
        {
            break;
        }
        n++;
    }
    TEST_ASSERT_LESS_THAN_UINT32_MESSAGE(SYS_DRY_MAX, n, "the pool never ran dry");
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_file_status(ao_spy(), "/storage/x"));
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Storage->eQueue));
    for ( uint32_t i = 0U; i < n; i++ )
    {
        QF_gc(&held[i]->super);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_file_status(ao_spy(), "/storage/x"),
                                  "nothing was posted, so the door is open");
    settle_card();
    (void)sys_take(FILE_STATUS_RSP_SIG);
    ao_spy_drain();
}

/** @brief The body of the full-queue case, run in a child: the asker's queue is full, then AO_Storage answers into it. */
static void sys_full_asker_body(void *arg)
{
    (void)arg;
    (void)qf_support_queue_stuff(ao_spy_other(), &s_sys_filler);
    (void)kode_sys_file_status(ao_spy_other(), "/storage/x");
    settle_card();
}

TEST_CASE("a file answer into an asker's full queue is QP's assert, not a drop",
          "[storage_ao][ask]")
{
    sys_fresh();
    TEST_ASSERT_EQUAL_INT(ASK_CHILD_ABORTED, ask_support_fork_run(&sys_full_asker_body, NULL));
    ao_spy_other_drain();
}

/* ---- The board's rest ------------------------------------------------------- */

/* A file is open only while something is transferring, so with nothing in
 * flight the files are closed and the card idle already: the round is ready,
 * the first time and the second (OFF after ASLEEP). */
TEST_CASE("a round with nothing open is ready, twice", "[storage_ao][lowpower]")
{
    storage_fresh();

    lp_round(AO_Storage, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
    lp_expect_no_more();

    lp_round(AO_Storage, KODE_LP_OFF);
    lp_expect_answer(false, NULL);
}

/* Nothing is restored on AWAKE: the next open mounts nothing and closes
 * nothing, it simply opens. */
TEST_CASE("after the rest AWAKE does nothing and the next open is served",
          "[storage_ao][lowpower]")
{
    static const QEvt close_evt = QEVT_INITIALIZER(STORAGE_CLOSE_REQ_SIG);

    storage_fresh();
    make_file("hello.bin", 64U);
    lp_round(AO_Storage, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);

    lp_awake(AO_Storage);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    ask_open("hello.bin");
    TEST_ASSERT_EQUAL_INT(KODE_OK, take_open_status());
    QASM_DISPATCH(AO_Storage, &close_evt, 0U);
}

/* A write in flight is data on its way to a medium. */
TEST_CASE("a write in flight refuses the round and is finished", "[storage_ao][lowpower]")
{
    storage_fresh();

    ask_write("outgoing.bin", s_big, sizeof(s_big));
    lp_round(AO_Storage, KODE_LP_ASLEEP);
    lp_expect_answer(true, "file write");
    lp_expect_no_more();

    drain_write();
    ao_spy_drain();

    lp_round(AO_Storage, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
}

/* A file held open for reading is a transfer too, but not a write. */
TEST_CASE("a file open for reading refuses the round until it is closed",
          "[storage_ao][lowpower]")
{
    static const QEvt close_evt = QEVT_INITIALIZER(STORAGE_CLOSE_REQ_SIG);

    storage_fresh();
    make_file("hello.bin", 64U);
    ask_open("hello.bin");
    TEST_ASSERT_EQUAL_INT(KODE_OK, take_open_status());

    lp_round(AO_Storage, KODE_LP_ASLEEP);
    lp_expect_answer(true, "storage busy");

    QASM_DISPATCH(AO_Storage, &close_evt, 0U);
    lp_round(AO_Storage, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
}

/* The card is inside a call the worker is making. */
TEST_CASE("a call out to the card refuses the round until it returns",
          "[storage_ao][worker][lowpower]")
{
    storage_fresh();
    fake_media_put_sd_path("held.bin", "x", 1U);
    ask_mount_job();

    lp_round(AO_Storage, KODE_LP_ASLEEP);
    lp_expect_answer(true, "storage busy");

    TEST_ASSERT_TRUE(run_sd_job());
    settle_card();
    take_cmd_rsp();
    ao_spy_drain();

    lp_round(AO_Storage, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
}
