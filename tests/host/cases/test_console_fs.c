/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_fs.c
 * @brief The file verbs: resolution, the working directory, the refusals.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "qpc.h"

#include "ao_spy.h"
#include "console_fs.h"
#include "fakes.h"
#include "kaos.h"
#include "kbsp_owners.h"
#include "kcli_proto.h"
#include "kdrv_storage.h"
#include "khal_crc32.h"
#include "kode_log.h"
#include "kqp.h"
#include "qf_support.h"
#include "owners_support.h"

/* Long enough to expire whatever an earlier case file left in flight - a
 * console watchdog is five seconds, a `test` row's budget the better part of
 * a minute - repeated here rather than imported for the reason
 * test_console_sd.c repeats it: the numbers are private to console_ao.c and a
 * test that shared them would agree with it by construction. */
#define ANY_BUDGET_TICKS   ((200000U * 100U) / 1000U)

/* ---- Resolution ---------------------------------------------------------- */

/** @brief Resolve @p in against @p cwd and assert the result. */
static void resolves(const char *cwd, const char *in, const char *want)
{
    char out[KODE_PATH_MAX];

    (void)memset(out, 0, sizeof(out));
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          console_fs_resolve_from(cwd, in, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(want, out);
}

TEST_CASE("an absolute path resolves to itself", "[console_fs]")
{
    resolves("/storage", "/sdcard/x.bin", "/sdcard/x.bin");
    resolves("/storage", "/", "/");
}

TEST_CASE("a relative path resolves against the cwd", "[console_fs]")
{
    resolves("/storage", "notes.txt", "/storage/notes.txt");
    resolves("/storage/logs", "a/b.txt", "/storage/logs/a/b.txt");
    resolves("/", "sdcard", "/sdcard");
}

TEST_CASE("dot and dot-dot are applied, not passed through", "[console_fs]")
{
    resolves("/storage/logs", "..", "/storage");
    resolves("/storage/logs", "../notes.txt", "/storage/notes.txt");
    resolves("/storage", "./notes.txt", "/storage/notes.txt");
    resolves("/storage", "a/../b.txt", "/storage/b.txt");
}

TEST_CASE("dot-dot at the root stays at the root", "[console_fs]")
{
    resolves("/", "..", "/");
    resolves("/sdcard", "../..", "/");
    resolves("/", "../../storage", "/storage");
}

TEST_CASE("repeated and trailing separators collapse", "[console_fs]")
{
    resolves("/storage", "a//b.txt", "/storage/a/b.txt");
    resolves("/storage", "logs/", "/storage/logs");
    resolves("/storage", "", "/storage");
}

TEST_CASE("a result too long to carry is refused, not truncated",
          "[console_fs]")
{
    char out[KODE_PATH_MAX];
    char deep[KODE_PATH_MAX * 2];
    char part[24];

    (void)snprintf(deep, sizeof(deep), "%s", "/storage");
    (void)snprintf(part, sizeof(part), "/%s", "0123456789abcdef0123");
    while ( strlen(deep) < (size_t)KODE_PATH_MAX )
    {
        (void)strncat(deep, part, sizeof(deep) - strlen(deep) - 1U);
    }

    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
                          console_fs_resolve_from("/", deep, out, sizeof(out)));
}

/* ---- The verbs on the wire ----------------------------------------------- */

/**
 * @brief Close whatever receive is in flight, on both ends of it.
 *
 * The one command kind this file cannot tick its way out of, and the reason
 * is the protocol working exactly as designed: AO_Console gives its own
 * watchdog away the moment it hears `>|ready`, because only the receiver -
 * AO_AppMgr for a terminal install, AO_Storage for `file_put` - can tell a
 * slow host from a dead one. From that point nothing AO_Console owns will
 * ever close the command, however far the clock is advanced - so a case that
 * leaves a transfer open hands the NEXT case a console answering `busy` to
 * everything it types, and the case after that, and every case in every file
 * that runs after this one. The first mutation run here turned six genuine
 * kills into ninety-four failures that way, which is a count nobody can read.
 *
 * That is the half of the story AO_Console owns. AO_Storage owns the other
 * half, and closing only the first one is worse than it looks, because of
 * what QP does with a QTimeEvt that was ever armed and is then reconstructed
 * without being properly unlinked first. `r_idle` lives on the one static
 * `Storage` instance for the whole binary, and console_up_once() reconstructs
 * it every case with Storage_ctor() -> QTimeEvt_ctorX(), which resets the
 * timer's own `next` pointer and flags in place. Disarming `r_idle` is only
 * half of what that needs: QTimeEvt_disarm() sets its tick counter to zero
 * and nothing else - qf_time.c's own comment calls that "schedule removal
 * from the list" - because QTimeEvt_tick_() is the ONLY code that actually
 * walks the chain and unlinks a node from it. Until a tick does that, the
 * node's QTE_FLAG_IS_LINKED bit is still set, which means some earlier node's
 * `next` still points at it. Reconstructing it while that is true truncates
 * the chain right there: whatever used to sit behind it - AO_Console's own
 * `tmo` among it - keeps QTE_FLAG_IS_LINKED set forever after, which is
 * exactly the flag armX() and rearm() both trust before they will touch a
 * timer again. Nothing crashes. A timer just stops counting for the rest of
 * the binary, and the first sign of it is a timeout assertion that should
 * have fired and quietly did not - in a suite that still reports green,
 * because the case that lost its watchdog is not the case that broke it.
 *
 * So this closes AO_Storage's side the way its own idle watchdog would, and
 * then makes the closing real: STORAGE_RECV_IDLE_SIG is the signal `r_idle`
 * fires with, and dispatching it directly runs recv_stall() on demand,
 * exactly as if the deadline had expired - discarding the stub, clearing
 * `r_who`, and disarming `r_idle`. Guarded on `s_storage_up`, not on
 * `AO_Storage->eQueue` - test_console_flow.c hands AO_Storage a queue of its
 * own without ever calling Storage_ctor() or QASM_INIT() on it, so a queue
 * existing proves nothing about whether this file has taken AO_Storage
 * through its own initial transition yet. Dispatching to an AO whose state
 * machine has never run QASM_INIT() is not "quiet on nothing to do" the way
 * an idle receive is - it is a jump through whatever `state.fun` happens to
 * hold in freshly-linked BSS, and on this port that segfaults rather than
 * asserting. `s_storage_up` is this file's own record of whether ITS
 * Storage_ctor() below has run at least once; nothing else's use of the same
 * static `Storage` instance can make it true early. Quiet on a receive that
 * already is not running, the same "ignored, not an error" shape recv_stall()
 * gives any other stale tick.
 *
 * The one real QP tick that follows is not optional, and skipping it would
 * put this function back where it started: it is what turns "disarmed" into
 * "unlinked", running QTimeEvt_tick_() once so it walks the whole chain and
 * drops every zero-counter node it finds - `r_idle` among them, now that the
 * dispatch above zeroed it - before Storage_ctor() next case ever reaches it.
 * One pass is enough; QTimeEvt_tick_() does not process one node per call, it
 * walks the entire list in the call it is given. That same tick also
 * dispatches whatever recv_close() just posted back to AO_Console (its own
 * verdict on a transfer already closed synchronously above, by the direct
 * RECV_FAIL_SIG dispatch), which is answered a second time for nothing worse
 * than that - the handlers are keyed on `inflight`, already idle by then.
 *
 * A verdict aimed at a console that is not receiving is ignored by the
 * handler itself - so calling this costs a case that closed cleanly nothing
 * at all. Every case in the file_put section below ends with it, and
 * console_up_once() calls it too, before Storage_ctor() runs rather than
 * after, for the case that could not release itself because an assertion
 * fired part way through.
 */

/** @brief Whether THIS file's own Storage_ctor()/QASM_INIT() has ever run. */
static bool s_storage_up;

static void console_release(void)
{
    static const QEvt recv_fail = QEVT_INITIALIZER(RECV_FAIL_SIG);
    static const QEvt recv_idle = QEVT_INITIALIZER(STORAGE_RECV_IDLE_SIG);

    QASM_DISPATCH(AO_Console, &recv_fail, 0U);

    if ( s_storage_up )
    {
        QASM_DISPATCH(AO_Storage, &recv_idle, 0U);
    }

    (void)qf_support_tick(AO_Console, 1U);
}

/** @brief Bring AO_Console and AO_Storage up, fresh, for one case. */
static void console_up_once(void)
{
    const QEvt *e;

    /* kdrv_storage's own handles sit below AO_Storage's view of them, and
     * Storage_ctor() below has never known how to reach them - it resets
     * this AO's reader/writer/copy bookkeeping, not the driver underneath
     * it. A read or a copy an earlier case abandoned mid-flight left a real
     * handle open here, and the next case's own open would have been
     * refused for a reason that had nothing to do with what it was testing.
     * test_storage_ao.c's storage_fresh() does the same, first, for the same
     * reason. */
    (void)kdrv_storage_close();

    qf_support_init();
    (void)ao_spy();
    ao_spy_drain();

    owners_up();
    if ( qf_support_claim_ao(AO_Console) )
    {
        Console_ctor();
        qf_support_give_queue(AO_Console);
        AO_Console->prio  = KODE_PRIO_CONSOLE;
        AO_Console->pthre = KODE_PRIO_CONSOLE;
        QActive_register_(AO_Console);
        QASM_INIT(AO_Console, (void *)0, 0U);
        qf_support_background(AO_Console);
    }

    /* The safety net for a case that died mid-transfer: see console_release()'s
     * own comment for the QP hazard this order avoids. This MUST run before
     * Storage_ctor() below reconstructs `r_idle` - releasing after would only
     * clean up a timer whose chain is already corrupted, one QTimeEvt_ctorX()
     * call too late to matter. It still goes in before the sink is cleared at
     * the bottom of this function, so any `>err` it produces belongs to the
     * case that is ending, not this one. */
    console_release();

    /* AO_Storage answers the path requests, so it has to be real here. No
     * QActive_register_(): it subscribes to nothing, a publisher needs no
     * registry slot, and the slot is test_storage_ao.c's to take.
     *
     * Reconstructed every case, not just the first. `reader`/`writer`/
     * `p_who` are this AO's OWN tell for "something is still in flight" -
     * busy()'s whole input - and a case that left a copy or a write open
     * (an assertion firing mid-transfer, say) used to leak that straight
     * into the next case as a stale refusal, one it had no way to explain.
     * kbsp_claim() is idempotent for the same owner (`&l_storage` is one
     * static instance for the whole binary, so the owner token never
     * changes), which is what makes re-running the initial transition here
     * exactly as safe as test_storage_ao.c's own per-case reset - and the
     * two kbsp_release() calls below still run every time, so this file
     * keeps handing both resources back before test_kbsp_owners.c looks for
     * them free. */
    Storage_ctor();
    qf_support_give_queue(AO_Storage);
    AO_Storage->prio  = KODE_PRIO_STORAGE;
    AO_Storage->pthre = KODE_PRIO_STORAGE;
    qf_support_register(AO_Storage);
    QASM_INIT(AO_Storage, (void *)0, 0U);
    s_storage_up = true;
    kbsp_release(KBSP_RES_SDMMC, AO_Storage);
    kbsp_release(KBSP_RES_FATFS, AO_Storage);

    /* AO_Log is a stand-in with a queue and no task, exactly as it is in
     * test_appmgr_ao.c and for the same reason: `file_put` hands the console's
     * input stream over by posting CONSOLE_RAW_REQ_SIG there, and takes it
     * back with CONSOLE_RAW_END_SIG when a transfer dies. A post into an AO
     * with no queue is a FreeRTOS assertion rather than a test failure, so the
     * queue has to exist even for the cases that never look in it. */
    qf_support_give_queue(AO_Log);

    /* Expire whatever an earlier file left in flight, before the sink is
     * cleared - otherwise its `>err code=6` lands in this case's replies. */
    (void)qf_support_tick(AO_Console, ANY_BUDGET_TICKS);

    while ( xQueueReceive(AO_Storage->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }

    /* And AO_Log's, for the same reason AO_Storage's is emptied here: a
     * transfer case that ended without reading it would otherwise hand the
     * next case a stale raw-mode request to trip over. */
    while ( xQueueReceive(AO_Log->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }

    fake_media_reset();
    console_fs_reset();
    fake_klog_reset();
    fake_neighbours_reset();
}

/** @brief Send one line in, exactly as AO_Log publishes it. */
static void type_line(const char *text)
{
    ConsoleLineEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(CONSOLE_LINE_SIG);
    e.src   = (uint8_t)KODE_CONSOLE_SRC_SERIAL;
    (void)snprintf(e.line, sizeof(e.line), "%s", text);
    QASM_DISPATCH(AO_Console, &e.super, 0U);
}

/**
 * @brief Let AO_Storage answer, then let the console say what it made of it.
 *
 * Two steps and in this order, because qf_support_pump() empties every
 * background AO's queue before it dispatches - and AO_Console is one. Its
 * answer only survives because AO_Storage posts it inside the pump's dispatch
 * loop, after both drains; qf_support_tick() then drains everybody EXCEPT the
 * console and hands the console its own queue.
 */
static void settle(void)
{
    (void)qf_support_pump(AO_Storage, 8U);
    (void)qf_support_tick(AO_Console, 1U);
}

/** @brief Whether any reply contained @p needle. */
static bool replied(const char *needle)
{
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, needle) != NULL) )
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Whether any reply ENDS with @p needle, rather than merely holding it.
 *
 * The difference is not pedantry, and finding that out cost a mutation run:
 * `>|path="/storage/nope"` contains `>|path="/` byte for byte, so a substring
 * assertion that the working directory was still the root passes happily
 * against a `cd` that moved it to a file whenever its needle stops one byte
 * short of the value's closing quote. Every path assertion in this file is
 * an end-of-line assertion for that reason, closing quote included.
 *
 * A captured line carries klog's newline, and may carry a colour reset, so
 * "the end" is any of those rather than the terminator alone.
 */
static bool replied_line(const char *needle)
{
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( NULL == l )
        {
            continue;
        }
        const char * const at = strstr(l, needle);
        if ( NULL == at )
        {
            continue;
        }
        char const after = at[strlen(needle)];
        if ( ('\0' == after) || ('\r' == after) || ('\n' == after)
            || ('\x1b' == after) )
        {
            return true;
        }
    }
    return false;
}

TEST_CASE("pwd starts at the root", "[console_fs]")
{
    console_up_once();

    type_line("pwd");
    TEST_ASSERT_TRUE(replied_line(">|path=\"/\""));
    TEST_ASSERT_TRUE(replied_line(">ok"));
}

TEST_CASE("cd moves the working directory and pwd says so", "[console_fs]")
{
    console_up_once();

    type_line("cd path=\"/storage\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">ok"));

    fake_klog_reset();
    type_line("pwd");
    TEST_ASSERT_TRUE(replied_line(">|path=\"/storage\""));
}

TEST_CASE("cd to something that is not a directory is refused",
          "[console_fs]")
{
    console_up_once();
    fake_media_put_fs_file("notes.txt", "hello");

    type_line("cd path=\"/storage/notes.txt\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=notdir"));

    fake_klog_reset();
    type_line("pwd");
    TEST_ASSERT_TRUE(replied_line(">|path=\"/\""));
}

/* The three ways a `cd` can fail are three different things to tell an
 * operator, and the one that costs a bench session is the pair at the ends:
 * "that path is not there" and "that volume will not answer" mean completely
 * different next steps. They are told apart by rc and `exists` in the
 * response, which is why StoragePathRspEvt carries both. */
TEST_CASE("the three ways a cd fails say three different things",
          "[console_fs]")
{
    console_up_once();

    /* Not there: the volume answered, and the answer was "nothing". */
    type_line("cd path=\"/storage/nope\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=missing"));

    /* A file: there, and not a directory. */
    fake_klog_reset();
    fake_media_put_fs_file("notes.txt", "hello");
    type_line("cd path=\"/storage/notes.txt\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=notdir"));

    /* No such volume: refused before any filesystem is touched. */
    fake_klog_reset();
    type_line("cd path=\"/nowhere\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=3 msg=path"));

    /* A real volume that will not mount: the card is gone. This is the pair
     * "not there" needs to be kept apart from - both are code 5, and this is
     * the only one of the two the bench cannot reach without pulling the
     * card out by hand. */
    fake_klog_reset();
    fake_media_set_sd_present(false);
    type_line("cd path=\"/sdcard\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=volume"));

    /* Through all of it the working directory never moved. */
    fake_klog_reset();
    type_line("pwd");
    TEST_ASSERT_TRUE(replied_line(">|path=\"/\""));
}

/* `ls` is a STAT and then, only once that says directory, a LIST - two
 * round trips to AO_Storage, not one, so every case below settles twice:
 * once for the STAT, once for the burst the STAT's answer asks for. */

TEST_CASE("ls of the root names both volumes", "[console_fs]")
{
    console_up_once();

    type_line("ls");
    settle();
    settle();
    /* The whole record for each, not a floating "type=dir" either volume
     * could have satisfied on its own - the same anchoring every other
     * listing case here uses. */
    TEST_ASSERT_TRUE(replied_line(">|name=\"sdcard\" type=dir size=0 medium=sd"));
    TEST_ASSERT_TRUE(replied_line(">|name=\"storage\" type=dir size=0 medium=fs"));
    TEST_ASSERT_TRUE(replied_line(">ok"));
}

TEST_CASE("ls names a file's type, size and volume", "[console_fs]")
{
    console_up_once();
    fake_media_put_fs_file("notes.txt", "hello");
    fake_media_put_fs_dir("logs");

    type_line("ls path=\"/storage\"");
    settle();
    settle();
    TEST_ASSERT_TRUE(
        replied_line(">|name=\"notes.txt\" type=file size=5 medium=fs"));
    TEST_ASSERT_TRUE(replied_line(">|name=\"logs\" type=dir size=0 medium=fs"));
}

/* The AppleDouble filter is the launcher scan's, not `ls`'s. `stat`, `cat`
 * and `rm` all see a "._name" stub, so an `ls` that hid one would be
 * answering "what is here" with an omission - and the operator who then
 * cannot account for the volume's free space has nothing to go on. */
TEST_CASE("ls shows the macOS stubs the launcher scan hides", "[console_fs]")
{
    console_up_once();
    fake_media_put_fs_file("._notes.txt", "junk");

    type_line("ls path=\"/storage\"");
    settle();
    settle();
    TEST_ASSERT_TRUE(
        replied_line(">|name=\"._notes.txt\" type=file size=4 medium=fs"));
}

TEST_CASE("ls with no path lists the working directory", "[console_fs]")
{
    console_up_once();
    fake_media_put_fs_file("notes.txt", "hello");

    type_line("cd path=\"/storage\"");
    settle();
    fake_klog_reset();

    type_line("ls");
    settle();
    settle();
    TEST_ASSERT_TRUE(
        replied_line(">|name=\"notes.txt\" type=file size=5 medium=fs"));
}

/* A listing answers with an unconditional terminator and no return code, so
 * a path naming no volume would otherwise come back as an empty, cheerful
 * `>ok` - which reads as "that directory is empty" and is not. This one
 * never reaches a second round trip: the STAT itself is refused. */
TEST_CASE("ls of a path on no volume is refused, not answered empty",
          "[console_fs]")
{
    console_up_once();

    type_line("ls path=\"/nowhere\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=3 msg=path"));
    TEST_ASSERT_FALSE(replied_line(">ok"));
}

/* `ls` of a missing directory used to print a cheerful, empty `>ok` -
 * indistinguishable from an actually empty directory, which is the single
 * most common question an operator asks `ls`. Now it STATs first and
 * refuses exactly as `cd` does: same code, same token, no listing follows. */
TEST_CASE("ls of a missing directory is refused, the same way cd refuses it",
          "[console_fs]")
{
    console_up_once();

    type_line("ls path=\"/storage/nope\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=missing"));
    TEST_ASSERT_FALSE(replied_line(">ok"));
}

TEST_CASE("stat says what one path is", "[console_fs]")
{
    console_up_once();
    fake_media_put_fs_file("notes.txt", "hello");

    type_line("stat path=\"/storage/notes.txt\"");
    settle();
    TEST_ASSERT_TRUE(
        replied_line(">|path=\"/storage/notes.txt\" type=file size=5 medium=fs"));
    TEST_ASSERT_TRUE(replied_line(">ok"));

    fake_klog_reset();
    type_line("stat path=\"/storage/nope\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=missing"));
}

/* A relative path is what the cwd is FOR, and a verb that quietly ignored it
 * would look right in every absolute-path test above. */
TEST_CASE("a verb reads a relative path against the working directory",
          "[console_fs]")
{
    console_up_once();
    fake_media_put_fs_file("notes.txt", "hello");

    type_line("cd path=\"/storage\"");
    settle();
    fake_klog_reset();

    type_line("stat path=\"notes.txt\"");
    settle();
    TEST_ASSERT_TRUE(
        replied_line(">|path=\"/storage/notes.txt\" type=file size=5 medium=fs"));
}

/* `cat` is a STAT and then, only once that says file, a read - two round
 * trips to AO_Storage, not one, so every case below settles twice: once for
 * the STAT, once for the line records it releases. */

TEST_CASE("cat prints a file as line records", "[console_fs]")
{
    console_up_once();
    fake_media_put_fs_file("hello.txt", "one\ntwo\n");

    type_line("cat path=\"/storage/hello.txt\"");
    settle();
    settle();
    TEST_ASSERT_TRUE(replied_line(">|line=\"one\""));
    TEST_ASSERT_TRUE(replied_line(">|line=\"two\""));
    TEST_ASSERT_TRUE(replied_line(">ok"));
}

/**
 * @brief A file with more lines than one dispatch step may turn into
 *        records proves do_path_cat() no longer walks its whole buffer in
 *        the step that read it - that is what sent this task back for
 *        Critical 1. Thirty lines against a 24-line step means the round
 *        cannot close on the settle() that runs the first step: if it did,
 *        the assertion between the two settle() calls below would fail,
 *        which is what a step cap alone - without going back to the queue
 *        in between - would still let happen.
 */
TEST_CASE("cat of many lines is paced across steps, not walked in one",
          "[console_fs]")
{
    char     body[400];
    size_t   pos = 0U;
    unsigned n;

    console_up_once();
    for ( n = 0U; 30U > n; ++n )
    {
        int const written = snprintf(&body[pos], sizeof(body) - pos, "L%u\n",
                                     n);
        pos += (size_t)written;
    }
    fake_media_put_fs_file("many.txt", body);

    type_line("cat path=\"/storage/many.txt\"");
    settle();
    settle();
    TEST_ASSERT_TRUE(replied_line(">|line=\"L0\""));
    TEST_ASSERT_TRUE(replied_line(">|line=\"L23\""));
    TEST_ASSERT_FALSE(replied_line(">|line=\"L24\""));
    TEST_ASSERT_FALSE(replied_line(">ok"));

    settle();
    TEST_ASSERT_TRUE(replied_line(">|line=\"L24\""));
    TEST_ASSERT_TRUE(replied_line(">|line=\"L29\""));
    TEST_ASSERT_TRUE(replied_line(">ok"));
}

/**
 * Five short lines, each nowhere near KODE_FS_CAT_LINE_MAX (200) on its own,
 * so the only thing that can cut this file short is `max=`.
 *
 * A single 255-byte line with no '\n' was tried here once and it passed for
 * the wrong reason: 255 already exceeds KODE_FS_CAT_LINE_MAX by itself, so
 * the per-line cut fired whether or not `max=` capping worked at all, and a
 * bare `replied("!")` could not tell the two apart. Five 11-byte lines keep
 * that path out of reach - `max=16` reads exactly one whole line plus five
 * bytes of the next, so what stops this file is `max=` and nothing else.
 */
TEST_CASE("cat says when it stopped early", "[console_fs]")
{
    console_up_once();
    fake_media_put_fs_file("short.txt",
        "0123456789\n0123456789\n0123456789\n0123456789\n0123456789\n");

    type_line("cat path=\"/storage/short.txt\" max=16");
    settle();
    settle();
    TEST_ASSERT_TRUE(replied_line(">|line=\"0123456789\""));
    TEST_ASSERT_TRUE(replied_line(
        "cat: '/storage/short.txt' stopped at 16 of 55 bytes (max=16)"));
    TEST_ASSERT_TRUE(replied_line(">ok"));
}

/* The mirror image of "cd to something that is not a directory is refused"
 * above: `cat` needs a FILE, so a directory is the wrong-type case for this
 * verb, told through the same path_answer() branch as `cd`'s but with the
 * opposite word - `notdir` would claim the path is not a directory, when the
 * whole reason `cat` refuses it is that it IS one. It never reaches a second
 * round trip - the STAT itself is refused, exactly as a missing or
 * unmountable path refuses `ls` and `cd` before either lists or moves. */
TEST_CASE("cat of a directory is refused", "[console_fs]")
{
    console_up_once();
    fake_media_put_fs_dir("logs");

    type_line("cat path=\"/storage/logs\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=isdir"));
    TEST_ASSERT_FALSE(replied_line(">ok"));
}

/* `cat` of a path that is simply not there tells the same story `stat` and
 * `cd` already tell about it - refused at the STAT phase, before AO_Storage
 * is ever asked to open anything. */
TEST_CASE("cat of a missing file is refused, the same way stat refuses it",
          "[console_fs]")
{
    console_up_once();

    type_line("cat path=\"/storage/nope\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=missing"));
    TEST_ASSERT_FALSE(replied_line(">ok"));
}

/* ---- mkdir, rm, rmdir and mv ----------------------------------------------
 * The console can now change the tree, not just read it. `mkdir` and `mv`
 * are one round trip - neither cares what is already at the path, only
 * whether the libc call underneath it says yes - so each of their cases
 * settles once. `rm` and `rmdir` STAT first, exactly as `cat` does, so a
 * `rm` of a directory and an `rmdir` of a file can be refused through
 * path_answer()'s one shared branch instead of a second copy of it; their
 * cases settle twice, once for the STAT and once for the mutation it gates.
 */

/**
 * @brief Write a file inside a subdirectory of the internal FAT fake.
 *
 * fake_media_put_fs_file() only reaches the top level, which is what every
 * other case in this file needs. `rmdir` on a non-empty directory is the one
 * case that needs a file a level deeper, so - the same idiom
 * test_kdrv_storage_path.c's put_fs_file() already uses for the same reason -
 * a local static earns its keep here rather than widening the fake for one
 * call site.
 */
static void put_fs_subfile(const char *subdir, const char *name,
                           const char *body)
{
    char  path[512];
    FILE *f;

    (void)snprintf(path, sizeof(path), "%s/%s/%s", fake_media_fs_dir(),
                   subdir, name);
    f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    (void)fwrite(body, 1U, strlen(body), f);
    (void)fclose(f);
}

/**
 * @brief Write a file straight onto the card fake.
 *
 * Same idiom, and the same reasoning, as put_fs_subfile() just above and
 * test_kdrv_storage_path.c's own put_sd_file(): the SD side of the fake has
 * no fake_media_put_sd_file() of its own because this is the only file in
 * this suite that needs one write onto the card, for the two card-warning
 * cases below.
 */
static void put_sd_file(const char *name, const char *body)
{
    char  path[512];
    FILE *f;

    (void)snprintf(path, sizeof(path), "%s/%s", fake_media_sd_dir(), name);
    f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    (void)fwrite(body, 1U, strlen(body), f);
    (void)fclose(f);
}

TEST_CASE("mkdir makes a directory", "[console_fs]")
{
    console_up_once();

    type_line("mkdir path=\"/storage/logs\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">ok"));

    fake_klog_reset();
    type_line("stat path=\"/storage/logs\"");
    settle();
    TEST_ASSERT_TRUE(
        replied_line(">|path=\"/storage/logs\" type=dir size=0 medium=fs"));
}

/* Two levels at once is not a thing kdrv_storage_mkdir() does - the parent
 * has to exist - and that refusal has to reach the wire as something an
 * operator can act on, not silence. `volume` would be a lie here: the
 * volume mounted fine, mkdir() itself refused the missing parent, so the
 * word says that outright rather than leaving an operator to guess what
 * about the mutation the bare verb name was standing in for. */
TEST_CASE("mkdir whose parent is missing is refused", "[console_fs]")
{
    console_up_once();

    type_line("mkdir path=\"/storage/a/b\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=noparent"));
}

/* fake_sdmmc_fail_file_writes() fails the real mkdir() inside
 * kdrv_storage.c - see its own comment in fake_khal_sdmmc.c - the way a
 * counterfeit card fails the directory-entry write and nothing else: the
 * parent (`/sdcard` itself) is right there, mounted, and the medium is what
 * says no. `noparent` would send an operator looking for a directory that
 * was never missing; this and the case above are the pair that has to stay
 * apart, one word each, or the confusion this case exists to catch is back. */
TEST_CASE("mkdir on a card that refuses the write is not blamed on a "
          "missing parent", "[console_fs]")
{
    console_up_once();
    fake_sdmmc_fail_file_writes(true);

    type_line("mkdir path=\"/sdcard/newdir\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=iofail"));
}

/* The other half of the pair above, on the SAME volume: a parent that is
 * genuinely not there, on a card that is otherwise perfectly healthy, still
 * answers `noparent` - proving the split is on errno, not on which medium
 * the path happens to name. */
TEST_CASE("mkdir on the card whose parent is missing still says noparent",
          "[console_fs]")
{
    console_up_once();

    type_line("mkdir path=\"/sdcard/nosuchdir/child\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=noparent"));
}

/* The card is gone, not merely missing a parent. kdrv_storage_mkdir() folds a
 * volume that will not mount and a missing parent into the same KODE_ERROR -
 * real_path() bails before mkdir() runs in the first case - and mkdir used to
 * answer `noparent` for both, sending an operator to look for a directory that
 * was never the problem on a card that was not even mounted. `ls` and `stat`
 * on the same dead card say `volume`; mkdir now agrees, told apart by the
 * `exists` (volume-mounted) flag the answer carries. */
TEST_CASE("mkdir on a card that will not mount says volume, not noparent",
          "[console_fs]")
{
    console_up_once();
    fake_media_set_sd_present(false);

    type_line("mkdir path=\"/sdcard/newdir\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=volume"));
}

/* The third thing mkdir() can refuse for, and the one an operator reaches by
 * typing the same line twice on a perfectly healthy volume: the parent is
 * right there and the NAME is taken. `noparent` points at a directory that
 * is not the problem, and `iofail` blames a medium that never refused
 * anything - so EEXIST carries its own status all the way up from
 * kdrv_storage_mkdir() and earns its own word. */
TEST_CASE("mkdir of a name that is already taken says exists",
          "[console_fs]")
{
    console_up_once();
    fake_media_put_fs_dir("logs");

    type_line("mkdir path=\"/storage/logs\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=exists"));
    TEST_ASSERT_FALSE(replied_line(">ok"));
}

/* A path whose parent component is a FILE. mkdir() leaves ENOTDIR behind,
 * which nothing about the medium produced - the volume mounted, the write
 * was never attempted - so `iofail` would be asserting a device fault that
 * did not happen, and `noparent` would be wrong in the other direction: the
 * parent is there, it is simply not a directory. */
TEST_CASE("mkdir under a file is refused for what it is", "[console_fs]")
{
    console_up_once();
    fake_media_put_fs_file("notes.txt", "hello");

    type_line("mkdir path=\"/storage/notes.txt/sub\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=notdir"));
    TEST_ASSERT_FALSE(replied_line(">ok"));
}

/* The same errno through the other create in kdrv_storage.c: `write` opens
 * its destination with fopen(), and a parent that is a file fails it exactly
 * the way mkdir() fails above. */
TEST_CASE("write under a file is refused for what it is", "[console_fs]")
{
    console_up_once();
    fake_media_put_fs_file("notes.txt", "hello");

    type_line("write path=\"/storage/notes.txt/sub\" text=\"x\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=notdir"));
    TEST_ASSERT_FALSE(replied_line(">ok"));
}

/* And the mirror image: the path is fine, the volume is fine, and what is at
 * it is a directory - EISDIR, which `write` cannot do anything with. `write`
 * has no STAT phase to catch this ahead of the open, so the word has to come
 * out of the failed create itself. */
TEST_CASE("write onto a directory says isdir", "[console_fs]")
{
    console_up_once();
    fake_media_put_fs_dir("logs");

    type_line("write path=\"/storage/logs\" text=\"x\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=isdir"));
    TEST_ASSERT_FALSE(replied_line(">ok"));
}

TEST_CASE("rm deletes a file", "[console_fs]")
{
    console_up_once();
    fake_media_put_fs_file("gone.txt", "x");

    type_line("rm path=\"/storage/gone.txt\"");
    settle();
    settle();
    TEST_ASSERT_TRUE(replied_line(">ok"));

    fake_klog_reset();
    type_line("stat path=\"/storage/gone.txt\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=missing"));
}

/* rm takes files only, and refuses a directory the way cat does: through the
 * exact path_answer() branch `cat` already uses for the opposite
 * case, word for word - "cat of a directory is refused" above is this case's
 * twin, and a diff of the two ought to be nothing but the verb. */
TEST_CASE("rm refuses a directory, the same story cat tells", "[console_fs]")
{
    console_up_once();
    fake_media_put_fs_dir("logs");

    type_line("rm path=\"/storage/logs\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=isdir"));
    TEST_ASSERT_FALSE(replied_line(">ok"));
}

TEST_CASE("rmdir removes an empty directory", "[console_fs]")
{
    console_up_once();
    fake_media_put_fs_dir("empty");

    type_line("rmdir path=\"/storage/empty\"");
    settle();
    settle();
    TEST_ASSERT_TRUE(replied_line(">ok"));

    fake_klog_reset();
    type_line("stat path=\"/storage/empty\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=missing"));
}

/* rmdir's STAT phase passes - "full" answers and is a directory - so this is
 * refused one round trip later than the cases above, by the libc call
 * itself: rmdir() on a non-empty directory fails exactly as it would from a
 * shell, and that failure reaches the wire through path_answer()'s own
 * "the owning AO could not do it" branch. The word is `notempty`, not
 * `volume` - the volume mounted fine and answered the STAT above; what
 * refused this was rmdir() itself, over a directory that still holds
 * something. */
TEST_CASE("rmdir refuses a non-empty directory", "[console_fs]")
{
    console_up_once();
    fake_media_put_fs_dir("full");
    put_fs_subfile("full", "inside.txt", "x");

    type_line("rmdir path=\"/storage/full\"");
    settle();
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=notempty"));

    fake_klog_reset();
    type_line("stat path=\"/storage/full\"");
    settle();
    TEST_ASSERT_TRUE(
        replied_line(">|path=\"/storage/full\" type=dir size=0 medium=fs"));
}

/* The mirror image of "rm refuses a directory": rmdir wants a directory, so
 * a file is its wrong-type case, told through the same branch cd's `notdir`
 * already comes from - it never reaches rmdir() at all, refused at the STAT
 * that gates it. */
TEST_CASE("rmdir refuses a file, the same story cd tells", "[console_fs]")
{
    console_up_once();
    fake_media_put_fs_file("notes.txt", "hello");

    type_line("rmdir path=\"/storage/notes.txt\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=notdir"));
    TEST_ASSERT_FALSE(replied_line(">ok"));
}

TEST_CASE("mv renames within a volume", "[console_fs]")
{
    console_up_once();
    fake_media_put_fs_file("before.txt", "hello");

    type_line("mv src=\"/storage/before.txt\" dst=\"/storage/after.txt\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">ok"));

    fake_klog_reset();
    type_line("stat path=\"/storage/after.txt\"");
    settle();
    TEST_ASSERT_TRUE(
        replied_line(">|path=\"/storage/after.txt\" type=file size=5 medium=fs"));

    /* A move, not a copy: the name it moved from has to answer "missing",
     * or a `mv` that quietly left a copy behind would pass on the
     * destination check above alone. */
    fake_klog_reset();
    type_line("stat path=\"/storage/before.txt\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=missing"));
}

/* kdrv_storage_rename() refuses a crossing with KODE_DENIED before touching
 * either path, and the console answers it as a bad path. */
TEST_CASE("mv refuses a cross-volume move", "[console_fs]")
{
    console_up_once();
    fake_media_put_fs_file("before.txt", "hello");

    type_line("mv src=\"/storage/before.txt\" dst=\"/sdcard/before.txt\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=3 msg=path"));

    fake_klog_reset();
    type_line("stat path=\"/storage/before.txt\"");
    settle();
    TEST_ASSERT_TRUE(
        replied_line(">|path=\"/storage/before.txt\" type=file size=5 medium=fs"));
}

/* mv has no STAT phase either, the same reason cp does not: kdrv_storage_
 * rename() answering KODE_ERROR for a missing source and for a volume that
 * will not mount is not something a well-formed `src=`/`dst=` line lets the
 * console tell apart in advance. `volume` would have blamed the card for a
 * typo it never touched - see pair_src_missing() in storage_ao.c. */
TEST_CASE("mv of a source that is not there is refused", "[console_fs]")
{
    console_up_once();

    type_line("mv src=\"/storage/nope.txt\" dst=\"/storage/after.txt\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=src"));
    TEST_ASSERT_FALSE(replied_line(">ok"));
}

/* The card warning is AO_Storage's own, not the console's - see kaos.storage's
 * warn_if_card(). mkdir is the simplest of the four verbs to prove it with:
 * one round trip, one path, no STAT phase to settle first. The assertion
 * names the whole line, anchored at its end by replied_line(), rather than a
 * loose substring: this suite has already been caught once by an assertion
 * that a completely unrelated line could also satisfy. The line names the
 * verb, and every case below checks its own: three of the callers delete
 * rather than write, so one shared wording could not be true for all of
 * them. */
TEST_CASE("changing the card is warned about before it happens",
          "[console_fs]")
{
    console_up_once();

    type_line("mkdir path=\"/sdcard/newdir\"");
    settle();
    TEST_ASSERT_TRUE(replied_line("mkdir on the card: /sdcard/newdir"));
    TEST_ASSERT_TRUE(replied_line(">ok"));
}

/* rm and mv are the two verbs the fix restored to warning: before this
 * review, only mkdir/rmdir warned from AO_Storage at all, and rm/mv did not
 * warn until a duplicate copy of warn_if_card() was added inside
 * kdrv_storage.c itself - the bug this review corrects. These two cases
 * prove the console-visible line is back for both, through the single copy
 * of warn_if_card() in storage_ao.c, going through rm's/mv's own STAT round
 * trip first exactly like "rm deletes a file"/"mv renames within a volume"
 * above do. */
TEST_CASE("rm warns about the card before it deletes", "[console_fs]")
{
    console_up_once();
    put_sd_file("gone.bin", "x");

    type_line("rm path=\"/sdcard/gone.bin\"");
    settle();
    settle();
    TEST_ASSERT_TRUE(replied_line("rm on the card: /sdcard/gone.bin"));
    TEST_ASSERT_TRUE(replied_line(">ok"));
}

TEST_CASE("mv warns about the card destination before it moves",
          "[console_fs]")
{
    console_up_once();
    put_sd_file("before.bin", "x");

    type_line("mv src=\"/sdcard/before.bin\" dst=\"/sdcard/after.bin\"");
    settle();
    TEST_ASSERT_TRUE(replied_line("mv on the card: /sdcard/after.bin"));
    TEST_ASSERT_TRUE(replied_line(">ok"));
}

/* ---- cp --------------------------------------------------------------------
 * `cp` is the one file verb that runs as a paced burst rather than a round
 * trip: AO_Storage moves one chunk per dispatch step with a QTimeEvt between
 * them, because AO_Gui, AO_Radio and AO_Net all sit below it and a copy that
 * never went idle would freeze the screen for its whole duration. Under
 * direct dispatch that costs one settle() per step plus one for the step
 * that finds the source exhausted, so every case below settles twice for a
 * copy of a file that fits one chunk.
 *
 * Every case that claims a copy landed reads the destination back with `cat`
 * rather than trusting `>ok`, and checks the source is still there. `cp` is
 * not `mv`, and the two are one word apart on the wire.
 */

TEST_CASE("cp copies a file and leaves the source where it was",
          "[console_fs]")
{
    console_up_once();
    fake_media_put_fs_file("before.txt", "hello");

    type_line("cp src=\"/storage/before.txt\" dst=\"/storage/after.txt\"");
    settle();
    settle();
    TEST_ASSERT_TRUE(replied_line(">ok"));

    fake_klog_reset();
    type_line("cat path=\"/storage/after.txt\"");
    settle();
    settle();
    TEST_ASSERT_TRUE(replied_line(">|line=\"hello\""));

    /* The source, which `mv` would have taken away. */
    fake_klog_reset();
    type_line("stat path=\"/storage/before.txt\"");
    settle();
    TEST_ASSERT_TRUE(
        replied_line(">|path=\"/storage/before.txt\" type=file size=5 medium=fs"));
}

/**
 * @brief A file bigger than one 32 KB chunk proves the copy really is paced
 *        rather than run to completion inside the step that started it.
 *
 * The same shape as "cat of many lines is paced across steps": 40 KB is two
 * chunks and then the step that finds the source exhausted, so the round
 * cannot close on the settle() that starts it - and the assertion BETWEEN
 * the settles is the whole case. A copy that looped inside one dispatch step
 * would answer `>ok` there, and would hold AO_Gui, AO_Radio and AO_Net off
 * for the length of the file on the board.
 */
TEST_CASE("cp of a file bigger than one chunk is paced across steps",
          "[console_fs]")
{
    static char big[40U * 1024U];

    console_up_once();
    (void)memset(big, 'x', sizeof(big) - 1U);
    big[sizeof(big) - 1U] = '\0';
    fake_media_put_fs_file("big.txt", big);

    type_line("cp src=\"/storage/big.txt\" dst=\"/storage/big2.txt\"");
    settle();
    TEST_ASSERT_FALSE(replied_line(">ok"));
    settle();
    TEST_ASSERT_FALSE(replied_line(">ok"));
    settle();
    TEST_ASSERT_TRUE(replied_line(">ok"));

    /* And every byte of it arrived, which `>ok` alone does not say. */
    fake_klog_reset();
    type_line("stat path=\"/storage/big2.txt\"");
    settle();
    TEST_ASSERT_TRUE(
        replied_line(">|path=\"/storage/big2.txt\" type=file size=40959 medium=fs"));
}

/* STORAGE_CLOSE_REQ_SIG carries no `who`, so any AO may post one, and its
 * handler's guard is the only thing stopping one AO from ending another's
 * transfer. A copy is the newest kind of transfer and holds TWO handles, so
 * a close landing mid-copy would take both away and leave the half-written
 * destination behind - the one outcome kdrv_storage_copy_end(false) exists
 * to prevent. The request is posted here directly, between the settle() that
 * runs the first chunk and the ones that finish the copy, because nothing
 * typed at a console can reach it: every AO that posts one asks busy() first.
 */
TEST_CASE("a stray close does not end a copy in flight", "[console_fs]")
{
    static char big[40U * 1024U];

    console_up_once();
    (void)memset(big, 'y', sizeof(big) - 1U);
    big[sizeof(big) - 1U] = '\0';
    fake_media_put_fs_file("big.txt", big);

    type_line("cp src=\"/storage/big.txt\" dst=\"/storage/big3.txt\"");
    settle();
    storage_close_request();
    settle();
    settle();
    settle();
    TEST_ASSERT_TRUE(replied_line(">ok"));

    fake_klog_reset();
    type_line("stat path=\"/storage/big3.txt\"");
    settle();
    TEST_ASSERT_TRUE(
        replied_line(">|path=\"/storage/big3.txt\" type=file size=40959 medium=fs"));
}

/* Crossing volumes is exactly what `cp` is for: `mv` refuses one, because
 * FAT cannot rename across a mount. */
TEST_CASE("cp crosses volumes where mv refuses to", "[console_fs]")
{
    console_up_once();
    put_sd_file("onsd.txt", "carried");

    type_line("cp src=\"/sdcard/onsd.txt\" dst=\"/storage/onsd.txt\"");
    settle();
    settle();
    TEST_ASSERT_TRUE(replied_line(">ok"));

    fake_klog_reset();
    type_line("cat path=\"/storage/onsd.txt\"");
    settle();
    settle();
    TEST_ASSERT_TRUE(replied_line(">|line=\"carried\""));

    fake_klog_reset();
    type_line("cat path=\"/sdcard/onsd.txt\"");
    settle();
    settle();
    TEST_ASSERT_TRUE(replied_line(">|line=\"carried\""));
}

/* The card warning is AO_Storage's own - see warn_if_card() in storage_ao.c -
 * and `cp` is the sixth caller of it, alongside mkdir/rm/rmdir/mv/write. The
 * destination is the path it names, never the source: reading a card is not
 * what poisons a session. */
TEST_CASE("cp warns about the card destination before it copies",
          "[console_fs]")
{
    console_up_once();
    fake_media_put_fs_file("before.txt", "hello");

    type_line("cp src=\"/storage/before.txt\" dst=\"/sdcard/after.txt\"");
    settle();
    settle();
    TEST_ASSERT_TRUE(replied_line("cp on the card: /sdcard/after.txt"));
    TEST_ASSERT_TRUE(replied_line(">ok"));
}

/* `volume` would have blamed the card for a typo it never touched -
 * AO_Storage checks the source alone, after kdrv_storage_copy_open() fails,
 * and `src` is what that check found: nothing there, on a volume that
 * mounted fine. See pair_src_missing() in storage_ao.c. */
TEST_CASE("cp of a source that is not there is refused", "[console_fs]")
{
    console_up_once();

    type_line("cp src=\"/storage/nope.txt\" dst=\"/storage/after.txt\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=src"));
    TEST_ASSERT_FALSE(replied_line(">ok"));

    /* And nothing was left at the destination it never wrote. */
    fake_klog_reset();
    type_line("stat path=\"/storage/after.txt\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=missing"));
}

TEST_CASE("cp to a path naming no volume is refused", "[console_fs]")
{
    console_up_once();
    fake_media_put_fs_file("before.txt", "hello");

    type_line("cp src=\"/storage/before.txt\" dst=\"/nowhere/after.txt\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=3 msg=path"));
    TEST_ASSERT_FALSE(replied_line(">ok"));
}

/* fake_sdmmc_fail_file_writes() fails the real fopen() inside kdrv_storage.c,
 * which is what a card that refuses to be written to does to
 * kdrv_storage_create_path(). The copy never starts, so the source is never
 * read either. The word is `iofail`, the same one `mkdir` and `write` give
 * for the identical cause: the destination's own volume mounted fine and
 * refused the write regardless, and `volume` claims a volume that will not
 * mount, which is the opposite of what happened. */
TEST_CASE("cp onto a card that will not take it is refused",
          "[console_fs]")
{
    console_up_once();
    fake_media_put_fs_file("before.txt", "hello");
    fake_sdmmc_fail_file_writes(true);

    type_line("cp src=\"/storage/before.txt\" dst=\"/sdcard/after.txt\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=iofail"));
    TEST_ASSERT_FALSE(replied_line(">ok"));
}

/* ---- write ----------------------------------------------------------------
 * One round trip, the mkdir shape: no STAT phase, so every case below needs
 * exactly one settle() for the write itself and, where it goes on to check
 * what landed, a second pair of settle()s for `cat`'s own STAT-then-read
 * (see "cat prints a file as line records" above for why that verb needs
 * two). Every case that claims text landed reads it back with `cat` rather
 * than trusting `>ok` alone - the task's own discipline, and the reason: an
 * event that built its StorageTextEvt.len wrong would still answer `>ok`
 * while writing the wrong number of bytes, and only reading the file back
 * catches that.
 */

TEST_CASE("write puts text in a file and append adds to it", "[console_fs]")
{
    console_up_once();

    type_line("write path=\"/storage/n.txt\" text=\"hello there\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">ok"));

    fake_klog_reset();
    type_line("write path=\"/storage/n.txt\" append=on text=\" again\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">ok"));

    fake_klog_reset();
    type_line("cat path=\"/storage/n.txt\"");
    settle();
    settle();
    TEST_ASSERT_TRUE(replied_line(">|line=\"hello there again\""));
    TEST_ASSERT_TRUE(replied_line(">ok"));
}

/* append defaults off - nothing on the line says so - so a second write to
 * the same name has to behave as if kdrv_storage_create_path() had been
 * asked to truncate, not merely as if the first write had never happened:
 * fake_media_put_fs_file() plants content this session never wrote, and the
 * assertion below is only true if `write` actually reopened the name in
 * truncating mode rather than, say, refusing a file that was already there. */
TEST_CASE("write without append truncates what was there", "[console_fs]")
{
    console_up_once();
    fake_media_put_fs_file("n.txt", "the old contents");

    type_line("write path=\"/storage/n.txt\" text=\"new\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">ok"));

    fake_klog_reset();
    type_line("cat path=\"/storage/n.txt\"");
    settle();
    settle();
    TEST_ASSERT_TRUE(replied_line(">|line=\"new\""));
    TEST_ASSERT_FALSE(replied_line(">|line=\"the old contents\""));
    TEST_ASSERT_FALSE(replied_line("old"));
}

TEST_CASE("a backslash-n in a written line becomes a newline",
          "[console_fs]")
{
    console_up_once();

    type_line("write path=\"/storage/n.txt\" text=\"one\\ntwo\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">ok"));

    fake_klog_reset();
    type_line("cat path=\"/storage/n.txt\"");
    settle();
    settle();
    TEST_ASSERT_TRUE(replied_line(">|line=\"one\""));
    TEST_ASSERT_TRUE(replied_line(">|line=\"two\""));
    TEST_ASSERT_TRUE(replied_line(">ok"));
}

/* The same story "ls of a path on no volume is refused" already tells: a path
 * that names no volume at all is a bad VALUE, code 3, not a mutation that was
 * refused - kdrv_storage_create_path() never runs, because real_path() turns
 * it away before fopen() is in reach. */
TEST_CASE("write to a path on no volume is refused, not written",
          "[console_fs]")
{
    console_up_once();

    type_line("write path=\"/nowhere/n.txt\" text=\"hi\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=3 msg=path"));
    TEST_ASSERT_FALSE(replied_line(">ok"));
}

/* Unlike mkdir's own missing-parent case, write has no dedicated word for
 * this - kdrv_storage_create_path()'s fopen() fails exactly the way a card
 * that refuses to mount fails, and path_answer() cannot tell those two
 * apart for any in-flight kind but MKDIR/RMDIR. `volume` is the honest word
 * left: the path named a real, mounted volume, and what refused this was the
 * open, not the path's syntax. */
TEST_CASE("write into a directory that does not exist is refused",
          "[console_fs]")
{
    console_up_once();

    type_line("write path=\"/storage/nodir/n.txt\" text=\"hi\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=volume"));
    TEST_ASSERT_FALSE(replied_line(">ok"));
}

/* The card warning is AO_Storage's own - see warn_if_card() in storage_ao.c -
 * and `write` is the fourth caller of it, alongside mkdir/rm/mv. One round
 * trip, one path, no STAT phase, the same shape "writing to the card is
 * warned about before it happens" already proves for mkdir. */
TEST_CASE("write warns about the card before it writes", "[console_fs]")
{
    console_up_once();

    type_line("write path=\"/sdcard/n.txt\" text=\"hi\"");
    settle();
    TEST_ASSERT_TRUE(replied_line("write on the card: /sdcard/n.txt"));
    TEST_ASSERT_TRUE(replied_line(">ok"));
}

/* fake_sdmmc_fail_file_writes() fails the real fopen() inside kdrv_storage.c
 * - see its own comment in fake_khal_sdmmc.c - which is exactly what a card
 * that refuses to be written to does to kdrv_storage_create_path(): the open
 * itself never succeeds, so this never reaches kdrv_storage_write() at all
 * and kdrv_storage_discard() is never called (nothing was opened to
 * discard). kdrv_storage_create_path() now reads errno on that failure the
 * same way kdrv_storage_mkdir() does, and the card leaves ENOTDIR behind,
 * not ENOENT: `/sdcard` mounted, the name was reachable, and it was the
 * open itself the medium refused. `volume` used to be the only word this
 * could reach, and it was wrong for the same reason `noparent` is wrong for
 * mkdir's own counterfeit-card case - `iofail` is what AO_Storage answers
 * with now. */
TEST_CASE("a write that cannot open its destination is refused",
          "[console_fs]")
{
    console_up_once();
    fake_sdmmc_fail_file_writes(true);

    type_line("write path=\"/sdcard/n.txt\" text=\"hi\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=iofail"));
    TEST_ASSERT_FALSE(replied_line(">ok"));
}

/* ---- write line= -----------------------------------------------------------
 * The read-modify-write: `line=` turns `write` from "put this text in the
 * file" into "replace exactly this line of it", holding the whole file in
 * one of AO_Storage's 32 KB buffers, rebuilding it in the other, and
 * publishing the result through a temporary and a rename rather than
 * touching the original directly. Every case here reads the result back
 * with `cat` rather than trusting `>ok` alone, the same discipline the
 * whole-file `write` cases above already follow and for the same reason: an
 * off-by-one in which line got replaced would still answer `>ok`.
 */

/* `append` and a nonzero `line=` pick incompatible shapes of the same verb -
 * add at the end, replace in the middle - so the combination is refused
 * outright, at h_write() itself, before AO_Storage is even asked. Refused
 * rather than silently acting on one of the two keys: the same lie a ninth
 * token on the line would be, one level up. */
TEST_CASE("write append=on with line= is refused", "[console_fs]")
{
    console_up_once();
    fake_media_put_fs_file("cfg.txt", "one\ntwo\n");

    type_line("write path=\"/storage/cfg.txt\" append=on line=1 text=\"ONE\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=appendline"));
    TEST_ASSERT_FALSE(replied_line(">ok"));

    /* Refused before either key ever reached AO_Storage - the file on disk
     * is exactly what fake_media_put_fs_file() wrote, untouched. */
    fake_klog_reset();
    type_line("cat path=\"/storage/cfg.txt\"");
    settle();
    settle();
    TEST_ASSERT_TRUE(replied_line(">|line=\"one\""));
    TEST_ASSERT_TRUE(replied_line(">|line=\"two\""));
    TEST_ASSERT_FALSE(replied_line(">|line=\"ONE\""));
}

TEST_CASE("write line= replaces exactly that line", "[console_fs]")
{
    console_up_once();
    fake_media_put_fs_file("cfg.txt", "one\ntwo\nthree\n");

    type_line("write path=\"/storage/cfg.txt\" line=2 text=\"TWO\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">ok"));

    fake_klog_reset();
    type_line("cat path=\"/storage/cfg.txt\"");
    settle();
    settle();
    TEST_ASSERT_TRUE(replied_line(">|line=\"one\""));
    TEST_ASSERT_TRUE(replied_line(">|line=\"TWO\""));
    TEST_ASSERT_TRUE(replied_line(">|line=\"three\""));
    TEST_ASSERT_FALSE(replied_line(">|line=\"two\""));
    TEST_ASSERT_TRUE(replied_line(">ok"));
}

/* The line editor publishes a rewrite through a temporary and a three-step
 * rename dance (path -> path.bak, path.tmp -> path, unlink path.bak), and the
 * dance is shaped that way for one reason: ESP-IDF's FatFs f_rename() REFUSES
 * to overwrite an existing target. The board failure this guards against was
 * the old code renaming the temporary straight ONTO the original - which
 * POSIX rename() overwrites, so the host fake never saw it, while on the card
 * every edit of an existing file came back `could not replace the original`.
 * The new dance never renames onto a name that already exists, so it passes on
 * both POSIX and FatFs; what this pins is exactly that property. Were any step
 * to target an existing name, FatFs would leave the edit half-done with a
 * leftover beside it - so the proof is that the content changed AND neither
 * the `.tmp` nor the `.bak` sibling survives a successful edit. */
TEST_CASE("write line= over an existing file leaves no .tmp or .bak behind",
          "[console_fs]")
{
    console_up_once();
    fake_media_put_fs_file("cfg.txt", "one\ntwo\nthree\n");

    type_line("write path=\"/storage/cfg.txt\" line=2 text=\"TWO\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">ok"));

    fake_klog_reset();
    type_line("ls path=\"/storage\"");
    settle();
    settle();
    /* The edited file is there under its own name, and the dance cleaned up
     * after itself: no temporary, no backup left over. */
    TEST_ASSERT_TRUE(replied("name=\"cfg.txt\" "));
    TEST_ASSERT_FALSE(replied("cfg.txt.tmp"));
    TEST_ASSERT_FALSE(replied("cfg.txt.bak"));

    fake_klog_reset();
    type_line("cat path=\"/storage/cfg.txt\"");
    settle();
    settle();
    TEST_ASSERT_TRUE(replied_line(">|line=\"TWO\""));
    TEST_ASSERT_FALSE(replied_line(">|line=\"two\""));
}

TEST_CASE("write line= past the end is refused", "[console_fs]")
{
    console_up_once();
    fake_media_put_fs_file("cfg.txt", "one\n");

    type_line("write path=\"/storage/cfg.txt\" line=9 text=\"x\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=3 msg=noline"));
    TEST_ASSERT_FALSE(replied_line(">ok"));
}

/* The refused edit above, followed by the read the task's own discipline
 * asks for: the rewrite never got as far as a temporary that could have
 * raced the rename, because there was no line 9 to substitute in the first
 * place, and the file at /storage/cfg.txt on disk is still the one
 * fake_media_put_fs_file() wrote before either command ran. */
TEST_CASE("a failed line edit leaves the original intact", "[console_fs]")
{
    console_up_once();
    fake_media_put_fs_file("cfg.txt", "one\ntwo\n");

    type_line("write path=\"/storage/cfg.txt\" line=9 text=\"x\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=3 msg=noline"));

    fake_klog_reset();
    type_line("cat path=\"/storage/cfg.txt\"");
    settle();
    settle();
    TEST_ASSERT_TRUE(replied_line(">|line=\"one\""));
    TEST_ASSERT_TRUE(replied_line(">|line=\"two\""));
    TEST_ASSERT_TRUE(replied_line(">ok"));
}

/* kdrv_storage_open_path()'s own zero-length special case answers
 * KODE_DENIED for a file that opened fine but has nothing in it - the same
 * status a path naming no volume gets - so this is the case that proves the
 * two are told apart. Semantically a zero-byte file has no line 1 to
 * replace, exactly what `noline` already means for a line past a non-empty
 * file's last one; `stat` afterwards proves the path itself is real, so
 * this cannot be passing by accident against a path that was simply bad. */
TEST_CASE("write line= against a zero-byte file answers noline, not path",
          "[console_fs]")
{
    console_up_once();
    fake_media_put_fs_file("empty.txt", "");

    type_line("write path=\"/storage/empty.txt\" line=1 text=\"x\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=3 msg=noline"));
    TEST_ASSERT_FALSE(replied_line(">ok"));

    fake_klog_reset();
    type_line("stat path=\"/storage/empty.txt\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(
        ">|path=\"/storage/empty.txt\" type=file size=0 medium=fs"));
}

/* A line replaced with something longer than the line it stood for can push
 * the REBUILT file over the ceiling even when the file this read started
 * from sat exactly AT it - answer_text_line_write()'s second STORAGE_CHUNK
 * check, distinct from the one in the case below that catches an oversized
 * SOURCE before a single byte is read.
 *
 * The body is built to land on exactly STORAGE_CHUNK (32768) bytes: a
 * one-byte first line, a newline, then padding out to the ceiling - so the
 * READ succeeds. `text` is built to KODE_FS_TEXT_MAX's own ceiling, 119
 * bytes, the longest a single `write ... text=` can ever carry - replacing
 * a one-byte line with a 119-byte one grows the file by 118 bytes, which is
 * what tips a file already sitting exactly on the line past it. */
TEST_CASE("write line= refuses when the replacement makes the file too big",
          "[console_fs]")
{
    enum { PAD_TOTAL = 32U * 1024U };   /* STORAGE_CHUNK, restated - see the
                                          * comment above for why this test
                                          * cannot import the real one. */
    static char body[PAD_TOTAL + 1U];
    static char text[120];              /* KODE_FS_TEXT_MAX itself, so the
                                          * 119 'q's below are the longest
                                          * text a `write` can ever carry,
                                          * not a value the console would
                                          * still have to clip on the way
                                          * in. */
    char        cmd[64U + sizeof(text)];

    console_up_once();
    body[0] = 'x';
    body[1] = '\n';
    (void)memset(&body[2], 'p', (size_t)PAD_TOTAL - 3U);
    body[PAD_TOTAL - 1U] = '\n';
    body[PAD_TOTAL]      = '\0';
    fake_media_put_fs_file("pad.txt", body);

    (void)memset(text, 'q', sizeof(text) - 1U);
    text[sizeof(text) - 1U] = '\0';
    (void)snprintf(cmd, sizeof(cmd),
                  "write path=\"/storage/pad.txt\" line=1 text=\"%s\"", text);

    type_line(cmd);
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=toolarge"));
    TEST_ASSERT_FALSE(replied_line(">ok"));

    /* Untouched: the refusal fired mid-rebuild, before a temporary was ever
     * created, so line 1 on disk is still the single byte this test put
     * there and not the 119 'q's it tried to become. */
    fake_klog_reset();
    type_line("cat path=\"/storage/pad.txt\" max=8");
    settle();
    settle();
    TEST_ASSERT_TRUE(replied_line(">|line=\"x\""));
}

/* The SOURCE alone is over the ceiling, so this refuses before a single byte
 * is read into either buffer - `stat` afterwards is what proves the file was
 * never even opened for writing, since a size that changed would mean some
 * version of the rewrite reached the card. */
TEST_CASE("write line= refuses a file over the 32 KB ceiling, unedited",
          "[console_fs]")
{
    static char big[(32U * 1024U) + 2U];

    console_up_once();
    (void)memset(big, 'a', sizeof(big) - 1U);
    big[sizeof(big) - 1U] = '\0';
    fake_media_put_fs_file("big.txt", big);

    type_line("write path=\"/storage/big.txt\" line=1 text=\"x\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=toolarge"));
    TEST_ASSERT_FALSE(replied_line(">ok"));

    fake_klog_reset();
    type_line("stat path=\"/storage/big.txt\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(
        ">|path=\"/storage/big.txt\" type=file size=32769 medium=fs"));
}

/* ---- file_put -------------------------------------------------------------
 * The one file verb whose payload does not fit on the line. `file_put` is
 * a terminal install's conversation with a path instead of an image - the
 * console hands its input stream over raw, AO_Storage stages what arrives,
 * acks each whole chunk so the host knows it may send the next, and checks
 * the CRC the host promised before it calls the file written:
 *
 *     host:  file_put path="/storage/x.bin" size=N crc32=C
 *     board: >|ready chunk=4096
 *     host:  <4096 raw bytes>
 *     board: >|ack
 *     ...                            (a final partial chunk earns no ack)
 *     board: >ok                     (or >err 5 / >err 6)
 *
 * The cases below drive both ends of that by hand, because neither exists
 * under direct dispatch: AO_Log is a stand-in with a queue and no task, so
 * raw_armed() plays the part of its reader task answering "the wire is
 * yours", and feed() plays the part of that same task posting what it read.
 * The console's own replies are read off the capture sink exactly as every
 * other case in this file reads them.
 *
 * One port, and the cable is the one driven here: KODE_CONSOLE_SRC_SERIAL on
 * every line below, which is what the spec calls cable-first. The BLE port is
 * out of scope for this slice and is untried rather than refused.
 *
 * What is deliberately NOT reachable from here: a write that fails PART WAY
 * through a transfer. The card fake refuses the open (fake_sdmmc_fail_file_
 * writes() hangs the mount point off a regular file), which is the "could not
 * open the destination" case below, and there is no fake that fails an
 * fwrite() once the file is already open. The shape that failure would test
 * is instead absent by construction here: AO_Storage counts what left the
 * WIRE and nothing else, so no failure downstream of the wire can rewind that
 * count and turn a write failure into a five-second stall - the exact defect
 * test_appmgr_ao.c's own Phase B was written to pin on the region side.
 */

/* The one place this file reaches for replied() instead of replied_line(),
 * and only ever to assert a record did NOT go out. The house rule exists
 * because a substring match lets `>|path="/storage/nope"` satisfy an
 * assertion about `>|path="/` - a prefix passing for the whole line. Turned
 * round, that same difference makes replied_line() the WEAKER matcher:
 * `>|ready` is never the end of its own line (`>|ready chunk=4096` is), so
 * TEST_ASSERT_FALSE(replied_line(">|ready")) holds whether or not the record
 * was sent, which is a case testing nothing. Asserting on the whole line
 * instead would only narrow it again - a mutant that answered
 * `>|ready chunk=8192` would slip through. So the negatives take the broadest
 * net there is, and every positive assertion in this file stays an
 * end-of-line one. */
#define NOT_READY_WHY \
    "no form of `>|ready` may reach the host here - it is the host's licence " \
    "to stream, and nothing is waiting to receive what it would send"

/** @brief AO_Storage's between-chunk deadline in QP ticks, as it arms it. */
#define RECV_IDLE_DEADLINE_TICKS  (KODE_RECV_IDLE_MS / 10U)

/**
 * @brief Drain AO_Log's queue, reporting whether @p sig was among what landed.
 *
 * AO_Log has a queue and no task here, so whatever AO_Storage posted to it
 * just sits there until a case asks. The same shape test_appmgr_ao.c reads
 * its own raw-mode traffic with.
 */
static bool log_saw(enum_t sig)
{
    const QEvt *e;
    bool        found = false;

    while ( xQueueReceive(AO_Log->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (enum_t)e->sig == sig )
        {
            found = true;
        }
        QF_gc(e);
    }
    return found;
}

/**
 * @brief Drain AO_Log's queue, copying out the raw-mode request it found.
 *
 * @param out Filled with the CONSOLE_RAW_REQ_SIG event's own fields when one
 *            was there. Untouched otherwise.
 * @return Whether a raw-mode request was among what landed.
 */
static bool log_took_raw(ConsoleRawReqEvt *out)
{
    const QEvt *e;
    bool        found = false;

    while ( xQueueReceive(AO_Log->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (enum_t)e->sig == CONSOLE_RAW_REQ_SIG )
        {
            *out  = *(ConsoleRawReqEvt const *)(const void *)e;
            found = true;
        }
        QF_gc(e);
    }
    return found;
}

/** @brief Answer the console-arm request the way AO_Log's reader task would. */
static void raw_armed(bool ok)
{
    ConsoleRawRspEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super  = (QEvt)QEVT_INITIALIZER(CONSOLE_RAW_RSP_SIG);
    e.status = (uint8_t)KODE_OK;
    if ( !ok )
    {
        e.status = (uint8_t)KODE_ERROR;
    }
    QASM_DISPATCH(AO_Storage, &e.super, 0U);
}

/**
 * @brief Post one chunk of the arriving payload, as the reader task does.
 *
 * Static, not a stack variable: RecvDataEvt carries a whole KODE_RECV_CHUNK
 * buffer and is the biggest event in the system. Dispatched directly rather
 * than posted, so its poolId stays 0 and no pool is touched at all.
 */
static void feed(const uint8_t *data, uint16_t len)
{
    static RecvDataEvt e;

    e.super = (QEvt)QEVT_INITIALIZER(RECV_DATA_SIG);
    e.len   = len;
    (void)memcpy(e.data, data, len);
    QASM_DISPATCH(AO_Storage, &e.super, 0U);
}

/** @brief Let the console dispatch whatever AO_Storage just posted it. */
static void console_step(void)
{
    (void)qf_support_tick(AO_Console, 1U);
}

/**
 * @brief Type the command, let AO_Storage arm the wire, and answer for it.
 *
 * The three steps every case below shares, up to and including `>|ready`.
 * The reply sink is deliberately NOT cleared here: the cases that are about
 * the handshake itself read it.
 */
static void put_begin(const char *cmd)
{
    ConsoleRawReqEvt raw;

    type_line(cmd);
    settle();
    (void)memset(&raw, 0, sizeof(raw));
    (void)log_took_raw(&raw);
    raw_armed(true);
    console_step();
}

/* The handshake, from the host's side: nothing may be asked of the host
 * before the wire has actually been handed over, because a `>|ready` the
 * console has not switched yet invites the payload straight into the line
 * editor. And the request that hands it over has to name this port, this
 * many bytes, and AO_Storage itself as the recipient - a transfer armed for
 * somebody else's queue is bytes nobody stages. */
TEST_CASE("file_put arms the wire before it asks the host for a byte",
          "[console_fs]")
{
    ConsoleRawReqEvt raw;

    console_up_once();

    type_line("file_put path=\"/storage/new.bin\" size=8 crc32=0");
    settle();
    TEST_ASSERT_FALSE_MESSAGE(replied(">|ready"), NOT_READY_WHY);

    (void)memset(&raw, 0, sizeof(raw));
    TEST_ASSERT_TRUE_MESSAGE(log_took_raw(&raw),
        "AO_Storage must ask AO_Log for the wire; nobody else may switch it");
    TEST_ASSERT_EQUAL_UINT32(8U, raw.size);
    TEST_ASSERT_EQUAL_PTR_MESSAGE(AO_Storage, raw.who,
        "the chunks have to reach the AO that opened the file");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)KODE_CONSOLE_SRC_SERIAL, raw.src,
        "the payload follows the command down the port it arrived on");

    raw_armed(true);
    console_step();
    TEST_ASSERT_TRUE(replied_line(">|ready chunk=4096"));
    TEST_ASSERT_FALSE(replied_line(">ok"));

    console_release();
}

/* The whole point of the verb: a file arrives over the cable and is on the
 * board afterwards. `>ok` alone would not say that - it is read back with
 * `cat`, the discipline every write case in this file already follows. */
TEST_CASE("file_put lands the payload and the file reads back",
          "[console_fs]")
{
    static const uint8_t body[6] = { 'h', 'e', 'l', 'l', 'o', '\n' };
    uint32_t const crc = khal_crc32(KHAL_CRC32_INIT, body, sizeof(body));
    char cmd[96];

    console_up_once();
    (void)snprintf(cmd, sizeof(cmd),
                   "file_put path=\"/storage/new.txt\" size=6 crc32=%lu",
                   (unsigned long)crc);

    put_begin(cmd);
    fake_klog_reset();
    feed(body, (uint16_t)sizeof(body));
    console_step();
    TEST_ASSERT_TRUE(replied_line(">ok"));

    fake_klog_reset();
    type_line("cat path=\"/storage/new.txt\"");
    settle();
    settle();
    TEST_ASSERT_TRUE(replied_line(">|line=\"hello\""));
    TEST_ASSERT_TRUE(replied_line(">ok"));

    console_release();
}

/* The ack ledger, the transfer's entire flow control: one `>|ack` per whole
 * KODE_RECV_CHUNK consumed and none for the trailing remainder, because the
 * host is not waiting for one. floor(8242 / 4096) is 2. A third ack would
 * leave the host reading a record it never asked for; a missing one leaves it
 * waiting forever with the payload half sent. */
TEST_CASE("file_put acks one whole chunk at a time, never the remainder",
          "[console_fs]")
{
    static uint8_t body[(2U * KODE_RECV_CHUNK) + 50U];
    uint32_t       crc;
    unsigned       acks = 0U;
    char           cmd[96];

    console_up_once();
    for ( unsigned i = 0U; sizeof(body) > i; ++i )
    {
        body[i] = (uint8_t)((i * 7U) + 3U);
    }
    crc = khal_crc32(KHAL_CRC32_INIT, body, sizeof(body));
    (void)snprintf(cmd, sizeof(cmd),
                   "file_put path=\"/storage/big.bin\" size=%u crc32=%lu",
                   (unsigned)sizeof(body), (unsigned long)crc);

    put_begin(cmd);
    fake_klog_reset();

    feed(&body[0], (uint16_t)KODE_RECV_CHUNK);
    console_step();
    feed(&body[KODE_RECV_CHUNK], (uint16_t)KODE_RECV_CHUNK);
    console_step();
    feed(&body[2U * KODE_RECV_CHUNK], 50U);
    console_step();

    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, ">|ack") != NULL) )
        {
            ++acks;
        }
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, acks,
        "floor(8242 / 4096) is 2 - the trailing 50 bytes must not earn a "
        "third ack the host is not waiting for");
    TEST_ASSERT_TRUE(replied_line(">ok"));

    /* Every byte, not just the last chunk's worth. */
    fake_klog_reset();
    type_line("stat path=\"/storage/big.bin\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(
        ">|path=\"/storage/big.bin\" type=file size=8242 medium=fs"));

    console_release();
}

/* The receive protocol is AO_AppMgr's, generalised - and generalising it is
 * exactly what made this case necessary. AO_Gui subscribes to four of the
 * five control signals and takes the screen over on RECV_READY_SIG, showing
 * "Installing..." because an image arriving on the console holds the flash
 * cache down and the launcher must not be repainting through it. A file_put
 * writes to FAT: it holds nothing down, needs no screen, and is not an
 * install - so a published RECV_READY_SIG here would put a false word in
 * front of a user for an operation that never needed the screen at all.
 *
 * AO_Storage therefore POSTS the four control events to whoever asked. The
 * console's handlers are keyed on its own in-flight command and cannot tell
 * a posted event from a published one, so nothing there changes; the bus,
 * which is where AO_Gui listens, never carries them. The spy is subscribed
 * to all five below and must stay empty across an entire successful
 * transfer that the console demonstrably answered in full. */
TEST_CASE("a file transfer answers the console alone, never the bus",
          "[console_fs]")
{
    static const uint8_t body[4] = { 'a', 'b', 'c', '\n' };
    uint32_t const crc = khal_crc32(KHAL_CRC32_INIT, body, sizeof(body));
    char cmd[96];

    console_up_once();
    ao_spy_subscribe(RECV_READY_SIG);
    ao_spy_subscribe(RECV_ACK_SIG);
    ao_spy_subscribe(RECV_DONE_SIG);
    ao_spy_subscribe(RECV_FAIL_SIG);
    ao_spy_subscribe(RECV_STALL_SIG);
    ao_spy_drain();

    (void)snprintf(cmd, sizeof(cmd),
                   "file_put path=\"/storage/quiet.txt\" size=4 crc32=%lu",
                   (unsigned long)crc);
    put_begin(cmd);
    feed(body, (uint16_t)sizeof(body));
    console_step();

    TEST_ASSERT_TRUE_MESSAGE(replied_line(">|ready chunk=4096"),
        "the console did hear the whole conversation");
    TEST_ASSERT_TRUE_MESSAGE(replied_line(">ok"), "...to its very end");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "not one of the five may reach the publish/subscribe bus, or AO_Gui "
        "says `Installing...` over a plain file copy");

    console_release();
}

/* The CRC is the host's promise about the bytes it is about to send, made
 * before any of them move. A payload that betrays it is refused - and the
 * half-written file goes with it, because a stub that promises bytes it does
 * not have is worse than no file at all: `app_run` would try to boot it. */
TEST_CASE("a payload that betrays its CRC leaves no file behind",
          "[console_fs]")
{
    static const uint8_t body[6] = { 'h', 'e', 'l', 'l', 'o', '\n' };
    char cmd[96];

    console_up_once();
    /* A CRC no payload of these six bytes could produce - the promise is
     * what the host committed to on the command line, and the wire is what
     * broke it. */
    (void)snprintf(cmd, sizeof(cmd),
                   "file_put path=\"/storage/bad.bin\" size=6 crc32=%lu",
                   (unsigned long)0xDEADBEEFUL);

    put_begin(cmd);
    fake_klog_reset();
    feed(body, (uint16_t)sizeof(body));
    console_step();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=file_put"));
    TEST_ASSERT_FALSE(replied_line(">ok"));

    fake_klog_reset();
    type_line("stat path=\"/storage/bad.bin\"");
    settle();
    TEST_ASSERT_TRUE_MESSAGE(replied_line(">err code=5 msg=missing"),
        "a failed transfer discards its destination; a stub that promises "
        "bytes it does not have is worse than no file");

    console_release();
}

/* A host that goes quiet mid-transfer is the failure this verb is most
 * likely to meet on a bench: an unplugged cable, a script that died. Three
 * things have to be true afterwards and a wedged console gets all three
 * wrong - the verdict is a timeout rather than silence, AO_Log is told to
 * abandon raw mode so the line editor comes back, and the half-written file
 * is gone. */
TEST_CASE("a host that goes quiet ends the transfer and gives the wire back",
          "[console_fs]")
{
    console_up_once();

    put_begin("file_put path=\"/storage/half.bin\" size=8192 crc32=0");
    fake_klog_reset();

    /* Half the payload, then nothing at all. */
    {
        static uint8_t half[KODE_RECV_CHUNK];
        feed(half, (uint16_t)sizeof(half));
        console_step();
    }

    /* settle() advances two ticks, so this comfortably outlasts the
     * between-chunk deadline AO_Storage armed when the first chunk landed. */
    for ( unsigned i = 0U; (RECV_IDLE_DEADLINE_TICKS + 50U) > i; ++i )
    {
        settle();
    }

    TEST_ASSERT_TRUE_MESSAGE(replied_line(">err code=6 msg=file_put"),
        "a host that goes silent is a timeout, not a console that stops "
        "answering");
    TEST_ASSERT_FALSE(replied_line(">ok"));
    TEST_ASSERT_TRUE_MESSAGE(log_saw(CONSOLE_RAW_END_SIG),
        "a transfer that died has to hand the wire back, or the console is "
        "left with its line editor off and no way to type");

    fake_klog_reset();
    type_line("stat path=\"/storage/half.bin\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=missing"));

    console_release();
}

/* The console allows one command at a time and says so before it looks at
 * anything else on the line. A transfer holds that slot for its whole
 * duration, which is the longest any command holds it. */
TEST_CASE("a second file_put while one is in flight is refused busy",
          "[console_fs]")
{
    console_up_once();

    put_begin("file_put path=\"/storage/new.bin\" size=8 crc32=0");
    fake_klog_reset();
    type_line("file_put path=\"/storage/other.bin\" size=8 crc32=0");
    TEST_ASSERT_TRUE(replied_line(">err code=4 msg=busy"));

    console_release();
}

TEST_CASE("a file verb is refused while a transfer is in flight",
          "[console_fs]")
{
    console_up_once();

    put_begin("file_put path=\"/storage/new.bin\" size=8 crc32=0");
    fake_klog_reset();
    type_line("ls path=\"/storage\"");
    TEST_ASSERT_TRUE(replied_line(">err code=4 msg=busy"));

    console_release();
}

/* Refused where every other file verb refuses a path that names no volume -
 * console_fs's own resolver, before AO_Storage is asked anything at all. The
 * console has to close the command itself there: nothing was posted, so
 * nothing is going to answer, and waiting five seconds to say `>err code=6`
 * about a path already known to be wrong is a worse answer arriving later. */
TEST_CASE("file_put to a path naming no volume is refused, and nothing is "
          "armed", "[console_fs]")
{
    console_up_once();

    type_line("file_put path=\"/nowhere/x.bin\" size=8 crc32=0");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=3 msg=path"));
    TEST_ASSERT_FALSE_MESSAGE(replied(">|ready"), NOT_READY_WHY);
    TEST_ASSERT_FALSE_MESSAGE(log_saw(CONSOLE_RAW_REQ_SIG),
        "a refused path must never hand the wire over - raw mode with no "
        "transfer behind it is a console nobody can type at");

    /* And the console is free again, rather than holding a slot for a
     * transfer that never started. */
    fake_klog_reset();
    type_line("pwd");
    TEST_ASSERT_TRUE(replied_line(">ok"));

    console_release();
}

/* A destination that will not open is refused after the wire was handed over
 * and before the host is told to send, which is the one ordering that leaves
 * neither side waiting: the host has heard nothing yet, and the wire goes
 * straight back. */
TEST_CASE("file_put onto a card that will not take it gives the wire back",
          "[console_fs]")
{
    console_up_once();
    fake_sdmmc_fail_file_writes(true);

    type_line("file_put path=\"/sdcard/x.bin\" size=8 crc32=0");
    settle();
    raw_armed(true);
    console_step();

    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=file_put"));
    TEST_ASSERT_FALSE_MESSAGE(replied(">|ready"), NOT_READY_WHY);
    TEST_ASSERT_TRUE_MESSAGE(log_saw(CONSOLE_RAW_END_SIG),
        "the wire was already raw when the open failed; it has to go back");

    console_release();
}

/* The card warning is AO_Storage's own - see warn_if_card() in storage_ao.c -
 * and `file_put` is the seventh caller of it, alongside mkdir/rm/rmdir/mv/
 * cp/write. It fires before the destination is even opened, for the reason
 * that function documents: on the counterfeit cards some units shipped with,
 * one failed write takes the whole session's reads down with it. */
TEST_CASE("file_put warns about the card before it opens the destination",
          "[console_fs]")
{
    static const uint8_t body[4] = { 'a', 'b', 'c', '\n' };
    uint32_t const crc = khal_crc32(KHAL_CRC32_INIT, body, sizeof(body));
    char cmd[96];

    console_up_once();
    (void)snprintf(cmd, sizeof(cmd),
                   "file_put path=\"/sdcard/x.bin\" size=4 crc32=%lu",
                   (unsigned long)crc);

    put_begin(cmd);
    TEST_ASSERT_TRUE(replied_line("file_put on the card: /sdcard/x.bin"));
    feed(body, (uint16_t)sizeof(body));
    console_step();
    TEST_ASSERT_TRUE(replied_line(">ok"));

    console_release();
}

/* The console refuses a second command before it looks at the line, so
 * nothing an operator types can reach AO_Storage's OWN handling of a transfer
 * that meets another - and that handling still has to exist and still has to
 * answer, because the console is not the only thing that can ask.
 * `kode_sys_file_*` will ask from an app's thread, and any Active Object may.
 * So this is posted directly, the way the stray close below is, and the asker
 * here is the spy rather than the console.
 *
 * The request waits for the transfer in flight, which is left completely
 * alone - it still takes its payload and still lands its file - and starts
 * when that one ends. The wait is bounded and its end is ANSWERED, because a
 * request that went silent would leave its asker waiting for a `>|ready` that
 * is never coming, with no watchdog of its own to fall back on. */
TEST_CASE("a transfer asked for while one is in flight waits for it, and the "
          "one already running is untouched", "[console_fs][turn]")
{
    static const uint8_t body[6] = { 'h', 'e', 'l', 'l', 'o', '\n' };
    uint32_t const crc = khal_crc32(KHAL_CRC32_INIT, body, sizeof(body));
    char cmd[96];

    console_up_once();
    (void)snprintf(cmd, sizeof(cmd),
                   "file_put path=\"/storage/first.txt\" size=6 crc32=%lu",
                   (unsigned long)crc);
    put_begin(cmd);

    ao_spy_drain();
    TEST_ASSERT_TRUE(storage_recv_request(ao_spy(), "/storage/second.txt", 6U,
                                          0U,
                                          (uint8_t)KODE_CONSOLE_SRC_SERIAL));
    settle();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)ao_spy_next_sig(),
        "the second transfer was answered while the first still ran");

    /* The first transfer never noticed. */
    fake_klog_reset();
    feed(body, (uint16_t)sizeof(body));
    console_step();
    TEST_ASSERT_TRUE(replied_line(">ok"));

    /* The second one began when the first ended: its wire is being armed. */
    settle();
    TEST_ASSERT_TRUE_MESSAGE(log_saw(CONSOLE_RAW_REQ_SIG),
        "the waiting transfer did not start when the one in flight ended");

    /* It is given up on, which leaves the AO free to read the first back. */
    console_release();
    fake_klog_reset();
    type_line("cat path=\"/storage/first.txt\"");
    settle();
    settle();
    TEST_ASSERT_TRUE(replied_line(">|line=\"hello\""));

    /* And the one that waited opened nothing. */
    fake_klog_reset();
    type_line("stat path=\"/storage/second.txt\"");
    settle();
    TEST_ASSERT_TRUE(replied_line(">err code=5 msg=missing"));

    console_release();
}

/** @brief Hold AO_Storage's reader slot, the way a transfer in flight does. */
static void hold_reader(void)
{
    static StorageOpenEvt e;

    fake_media_put_fs_file("held.bin", "0123456789abcdef");
    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(STORAGE_OPEN_REQ_SIG);
    (void)snprintf(e.name, sizeof(e.name), "%s", "held.bin");
    e.who = ao_spy();
    QASM_DISPATCH(AO_Storage, &e.super, 0U);
    ao_spy_drain();
}

/** @brief End that transfer and let what waited for it run. */
static void release_reader(void)
{
    storage_close_request();
    (void)qf_support_pump(AO_Storage, 8U);
}

TEST_CASE("file_put typed while a transfer runs waits for it and starts when it "
          "ends", "[console_fs][turn]")
{
    console_up_once();
    hold_reader();

    type_line("file_put path=\"/storage/new.bin\" size=8 crc32=0");
    settle();
    TEST_ASSERT_FALSE_MESSAGE(replied(">err"), "refused instead of waiting");
    TEST_ASSERT_FALSE_MESSAGE(replied(">|ready"), NOT_READY_WHY);
    TEST_ASSERT_FALSE_MESSAGE(log_saw(CONSOLE_RAW_REQ_SIG),
        "the wire was handed over with a transfer still running");

    release_reader();
    TEST_ASSERT_TRUE_MESSAGE(log_saw(CONSOLE_RAW_REQ_SIG),
        "the file_put did not start when the transfer ended");
    raw_armed(true);
    console_step();
    TEST_ASSERT_TRUE(replied(">|ready"));

    console_release();
}

TEST_CASE("file_put whose wait for the transfer runs out says busy, and hands "
          "nothing over", "[console_fs][turn]")
{
    console_up_once();
    hold_reader();

    type_line("file_put path=\"/storage/new.bin\" size=8 crc32=0");
    settle();
    qf_support_time_passes(KQP_TICKS_PER_MS(KODE_STORAGE_TURN_MS) + 10U);
    settle();
    settle();

    TEST_ASSERT_TRUE(replied_line(">err code=4 msg=busy"));
    TEST_ASSERT_FALSE_MESSAGE(replied(">|ready"), NOT_READY_WHY);
    TEST_ASSERT_FALSE_MESSAGE(log_saw(CONSOLE_RAW_REQ_SIG),
        "a refused file_put must never hand the wire over");

    /* The console is free again. */
    fake_klog_reset();
    type_line("pwd");
    TEST_ASSERT_TRUE(replied_line(">ok"));

    release_reader();
    console_release();
}

/* STORAGE_CLOSE_REQ_SIG carries no `who`, so any AO may post one, and its
 * handler's guard is the only thing stopping one AO from ending another's
 * transfer. A transfer holds the writer handle across many dispatch steps
 * exactly as a copy holds its two, so it needs naming in that guard for the
 * same reason: a close landing mid-transfer would shut the file while the
 * console still owed the host acks, and the bytes still arriving would go
 * into a closed handle. Posted directly here because nothing typed at a
 * console can reach it - the console is busy for the whole transfer. */
TEST_CASE("a stray close does not end a transfer in flight", "[console_fs]")
{
    static const uint8_t body[6] = { 'h', 'e', 'l', 'l', 'o', '\n' };
    uint32_t const crc = khal_crc32(KHAL_CRC32_INIT, body, sizeof(body));
    char cmd[96];

    console_up_once();
    (void)snprintf(cmd, sizeof(cmd),
                   "file_put path=\"/storage/kept.txt\" size=6 crc32=%lu",
                   (unsigned long)crc);

    put_begin(cmd);
    storage_close_request();
    settle();
    fake_klog_reset();
    feed(body, (uint16_t)sizeof(body));
    console_step();
    TEST_ASSERT_TRUE(replied_line(">ok"));

    fake_klog_reset();
    type_line("cat path=\"/storage/kept.txt\"");
    settle();
    settle();
    TEST_ASSERT_TRUE(replied_line(">|line=\"hello\""));

    console_release();
}
