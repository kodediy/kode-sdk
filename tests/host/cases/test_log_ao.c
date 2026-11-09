/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_log_ao.c
 * @brief AO_Log's line editor, which is the console's whole input half.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "qpc.h"

#include "ao_spy.h"
#include "config_support.h"
#include "fakes.h"
#include "kaos.h"
#include "kbsp_owners.h"
#include "khal_nvs.h"
#include "khal_time.h"
#include "kqp.h"
#include "kode_log.h"
#include "kode_sys.h"
#include "kset.h"
#include "net_channel.h"
#include "qf_support.h"
#if !CONFIG_KODEOS_CHANNEL
#error "test_log_ao.c needs CONFIG_KODEOS_CHANNEL=1 (main/CMakeLists.txt)"
#endif

/**
 * @brief Construct AO_Log and run its initial transition, and nothing more.
 *
 * What the bring-up said is still in AO_Log's queue and the level it applied
 * is still klog's, so a case about the bring-up itself can read both before
 * log_settle() takes them away.
 */
static void log_bring_up(void)
{
    static bool s_registered;

    qf_support_init();
    (void)ao_spy();
    ao_spy_drain();
    ao_spy_subscribe(CONSOLE_LINE_SIG);

    fake_console_reset();

    Log_ctor();
    qf_support_give_queue(AO_Log);
    AO_Log->prio  = KODE_PRIO_LOG;
    AO_Log->pthre = KODE_PRIO_LOG;

    if ( !s_registered )
    {
        QActive_register_(AO_Log);
        s_registered = true;
    }
    QASM_INIT(AO_Log, (void *)0, 0U);
}

/** @brief Undo what the bring-up left for the rest of the binary. */
static void log_settle(void)
{
    /* The initial transition installed AO_Log's own sink. Take the capture
     * sink back, or every KLOG_ line in every later case becomes an event on
     * a queue nothing drains. */
    fake_klog_reset();

    /* Whatever the bring-up announced, and whatever it queued for itself. */
    for ( ;; )
    {
        const QEvt *e = NULL;
        if ( xQueueReceive(AO_Log->eQueue, &e, 0U) != pdTRUE )
        {
            break;
        }
        QF_gc(e);
    }
    ao_spy_drain();

    /* The screen, not the counters: a case asserting that the transport was
     * opened would otherwise be reading a figure this line had just zeroed. */
    fake_console_clear_out();
}

/** @brief Put AO_Log back to a freshly started state with an empty screen. */
static void log_fresh(void)
{
    log_bring_up();
    log_settle();
}

/**
 * @brief Register log_on_byte() with the fake reader seam.
 *
 * Not part of log_fresh() itself: log_start_reader() also draws a prompt
 * (prompt_draw()), which sets prompt_drawn true as a side effect - every
 * existing case in this file relies on prompt_drawn starting false, so
 * folding this into the shared fixture broke six of them by making the
 * first keystroke think a prompt it never actually showed had already been
 * drawn. Only a case that calls fake_console_feed_byte() needs this, and
 * only those call it, right after log_fresh(), clearing the prompt's own
 * output the same way log_fresh() clears everything else.
 */
static void reader_ready(void)
{
    log_start_reader();
    fake_console_clear_out();
}

/** @brief Feed one byte in, the way the reader task would. */
static void type(uint8_t c)
{
    ConsoleByteEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(CONSOLE_BYTE_SIG);
    e.byte  = c;
    QASM_DISPATCH(AO_Log, &e.super, 0U);
}

/** @brief Feed a whole string in, byte by byte. */
static void type_str(const char *s)
{
    for ( const char *p = s; '\0' != *p; ++p )
    {
        type((uint8_t)*p);
    }
}

/** @brief The line the spy received, or NULL if none was published. */
static const char *published(void)
{
    const QEvt * const e = ao_spy_take();

    if ( (NULL == e) || (CONSOLE_LINE_SIG != e->sig) )
    {
        return NULL;
    }
    return ((ConsoleLineEvt const *)e)->line;
}

/** @brief Hand AO_Log every event waiting in its queue, as its task would. */
static void log_run(void)
{
    const QEvt *e = NULL;

    while ( xQueueReceive(AO_Log->eQueue, &e, 0U) == pdTRUE )
    {
        QASM_DISPATCH(AO_Log, e, 0U);
        QF_gc(e);
    }
}

/* ---- Bring-up -------------------------------------------------------------- */

TEST_CASE("AO_Log opens the transport and claims the console", "[log_ao]")
{
    log_fresh();

    TEST_ASSERT_EQUAL_STRING("AO_Log", kbsp_owner_name(KBSP_RES_CONSOLE));
    TEST_ASSERT_GREATER_THAN_UINT(0U, fake_console_inits());
}

/* ---- Typing ---------------------------------------------------------------- */

/* The prompt is drawn on the first byte of a line, not after every output
 * line. That is what keeps a record's '>' or '!' in column zero while a host
 * is driving the console. */
TEST_CASE("the prompt appears at the first keystroke, not before", "[log_ao]")
{
    log_fresh();

    TEST_ASSERT_EQUAL_STRING_MESSAGE("", fake_console_out(),
        "an idle console must not have a prompt waiting on the wire");

    type('a');
    TEST_ASSERT_EQUAL_STRING("kode> a", fake_console_out());

    type('b');
    TEST_ASSERT_EQUAL_STRING("kode> ab", fake_console_out());
}

TEST_CASE("a finished line is published, and the buffer starts again",
          "[log_ao]")
{
    log_fresh();

    type_str("ps\r");

    const char * const line = published();
    TEST_ASSERT_NOT_NULL(line);
    TEST_ASSERT_EQUAL_STRING("ps", line);

    /* And the next line is a new one rather than a continuation. */
    ao_spy_drain();
    type_str("bat_info\n");
    TEST_ASSERT_EQUAL_STRING("bat_info", published());
}

/* Both terminators, because a terminal sends CR and a host script often sends
 * LF, and a console that only understood one would look broken to half its
 * callers. */
TEST_CASE("carriage return and line feed both end a line", "[log_ao]")
{
    log_fresh();
    type_str("a\r");
    TEST_ASSERT_EQUAL_STRING("a", published());

    ao_spy_drain();
    log_fresh();
    type_str("b\n");
    TEST_ASSERT_EQUAL_STRING("b", published());
}

/* An empty line publishes nothing. Pressing return at an idle prompt is not a
 * command, and answering it with '>err' would make a stray keystroke look
 * like a failure. */
TEST_CASE("an empty line publishes nothing", "[log_ao]")
{
    log_fresh();

    type('\r');

    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
}

/* No prompt is drawn after a line ends, on purpose: the response to that line
 * is about to go out, and a prompt drawn now would sit in front of its '>' on
 * the wire. */
TEST_CASE("no prompt is drawn after a line, so a reply starts in column zero",
          "[log_ao]")
{
    log_fresh();

    type_str("ps\r");

    TEST_ASSERT_EQUAL_STRING_MESSAGE("kode> ps\r\n", fake_console_out(),
        "a prompt here would go out ahead of the reply's marker byte");
}

/* ---- Editing --------------------------------------------------------------- */

TEST_CASE("backspace removes the character and rubs it off the screen",
          "[log_ao]")
{
    log_fresh();

    type_str("ab");
    type(0x7fU);          /* DEL */
    type('\r');

    TEST_ASSERT_EQUAL_STRING("a", published());
    TEST_ASSERT_EQUAL_STRING("kode> ab\b \b\r\n", fake_console_out());
}

TEST_CASE("the other backspace works too", "[log_ao]")
{
    log_fresh();

    type_str("xy");
    type(0x08U);          /* ^H */
    type('\r');

    TEST_ASSERT_EQUAL_STRING("x", published());
}

/* Backspacing at an empty prompt writes nothing: erasing what is not there
 * would walk the cursor back over the prompt itself. */
TEST_CASE("backspace on an empty line touches neither buffer nor screen",
          "[log_ao]")
{
    log_fresh();

    type(0x7fU);
    type(0x7fU);

    TEST_ASSERT_EQUAL_STRING("", fake_console_out());
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
}

/* A control byte nobody asked about is dropped rather than stored: an arrow
 * key is three bytes, and putting its escape sequence in the buffer would
 * hand the parser something no human typed. */
TEST_CASE("a control byte is ignored, not buffered", "[log_ao]")
{
    log_fresh();

    type('a');
    type(0x1bU);          /* ESC */
    type(0x09U);          /* TAB */
    type(0x00U);
    type('b');
    type('\r');

    TEST_ASSERT_EQUAL_STRING("ab", published());
}

/* ---- The line that does not fit -------------------------------------------- */

/* A line silently cut in half is a protocol lie. Over the cable a human sees
 * their echo stop and can guess; a machine on the other transport sees
 * nothing at all and would get a truncated command executed and answered
 * '>ok'. So it is refused, with a code, and nothing is published. */
TEST_CASE("a line too long is refused with a code, never truncated",
          "[log_ao]")
{
    bool said_so = false;

    log_fresh();

    for ( unsigned i = 0U; (KODE_CONSOLE_LINE_MAX + 16U) > i; ++i )
    {
        type('x');
    }
    type('\r');

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "a truncated command must not reach the parser looking complete");

    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, "line_too_long") != NULL) )
        {
            said_so = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(said_so,
        "the caller has to be told, or a dropped line looks like a hang");
}

/* Nothing past the limit is echoed either: the echo stopping is the only
 * warning a human gets that they have run out of line. */
TEST_CASE("the echo stops at the limit", "[log_ao]")
{
    log_fresh();

    for ( unsigned i = 0U; (KODE_CONSOLE_LINE_MAX + 16U) > i; ++i )
    {
        type('x');
    }

    /* The prompt plus exactly as many characters as fit, and no more. */
    TEST_ASSERT_EQUAL_UINT(strlen("kode> ") + (KODE_CONSOLE_LINE_MAX - 1U),
                           fake_console_out_len());
}

/* Somebody who overshot, backed up and retyped has a line that fits, and must
 * not be refused for a state they have already corrected. */
TEST_CASE("backing up under the limit un-refuses the line", "[log_ao]")
{
    log_fresh();

    for ( unsigned i = 0U; (KODE_CONSOLE_LINE_MAX + 4U) > i; ++i )
    {
        type('x');
    }
    type(0x7fU);          /* back under the limit */
    type('\r');

    const char * const line = published();
    TEST_ASSERT_NOT_NULL_MESSAGE(line,
        "the overflow flag outlived the overflow");
    TEST_ASSERT_EQUAL_UINT(KODE_CONSOLE_LINE_MAX - 2U, strlen(line));
}

/* ---- A log line arriving while somebody is typing -------------------------- */

/** @brief Push one formatted log line through AO_Log, as klog's sink does. */
static void log_line_src(const char *text, uint8_t src)
{
    LogEvt *e = Q_NEW_X(LogEvt, 0U, LOG_LINE_SIG);

    TEST_ASSERT_NOT_NULL(e);
    e->src = src;
    e->len = (uint16_t)snprintf(e->line, sizeof(e->line), "%s", text);
    QASM_DISPATCH(AO_Log, &e->super, 0U);
    QF_gc(&e->super);
}

/** @brief A log line: answers no command, so it carries no source. */
static void log_line(const char *text)
{
    log_line_src(text, (uint8_t)KODE_LOG_SRC_NONE);
}

/* The whole reason the console became one Active Object. A log line arriving
 * mid-command wipes the prompt, prints on a clean line, and puts the prompt
 * and the half-typed command back - with no lock, because there is only one
 * writer. */
TEST_CASE("a log line steps around a half-typed command and restores it",
          "[log_ao]")
{
    log_fresh();

    type_str("app li");
    fake_console_reset();

    log_line("I hello\n");

    TEST_ASSERT_EQUAL_STRING_MESSAGE("\r\033[KI hello\nkode> app li",
                                     fake_console_out(),
        "erase, print, redraw - and the redraw carries what was typed");
}

/* With nothing typed there is no prompt to restore, and restoring one would
 * put a prompt fragment in front of the next record's marker byte. */
TEST_CASE("a log line at an idle console leaves no prompt behind", "[log_ao]")
{
    log_fresh();

    log_line("I quiet\n");

    TEST_ASSERT_EQUAL_STRING("I quiet\n", fake_console_out());
}

/* Nothing to erase before the first prompt is ever drawn, so the first log
 * line of the boot must not open with an erase sequence. */
TEST_CASE("the first log line does not erase a prompt that was never drawn",
          "[log_ao]")
{
    log_fresh();

    log_line("I first\n");

    TEST_ASSERT_EQUAL_STRING("I first\n", fake_console_out());
}

/* ---- Raw mode -------------------------------------------------------------- */

/** @brief Ask AO_Log to hand the input stream over. */
static void ask_raw(QActive *who, uint32_t size)
{
    ConsoleRawReqEvt req;

    (void)memset(&req, 0, sizeof(req));
    req.super = (QEvt)QEVT_INITIALIZER(CONSOLE_RAW_REQ_SIG);
    req.who   = who;
    req.size  = size;
    req.src   = (uint8_t)KODE_CONSOLE_SRC_SERIAL;
    QASM_DISPATCH(AO_Log, &req.super, 0U);
}

/* Raw mode hands the transport to a bulk reader - a file upload over the
 * cable - and the line editor stops interpreting bytes for its duration. */
TEST_CASE("a raw request opens raw mode, and ending it closes it", "[log_ao]")
{
    static const QEvt end = QEVT_INITIALIZER(CONSOLE_RAW_END_SIG);

    log_fresh();
    fake_console_raw_refuse(false);

    ask_raw(ao_spy(), 64U);
    TEST_ASSERT_TRUE(fake_console_raw_open());

    QASM_DISPATCH(AO_Log, &end, 0U);
    TEST_ASSERT_FALSE(fake_console_raw_open());
}

/* Whatever was half-typed is abandoned rather than carried over: what follows
 * on this wire is not text, and the editor's contents were never terminated. */
TEST_CASE("arming raw mode throws away the half-typed line", "[log_ao]")
{
    static const QEvt end = QEVT_INITIALIZER(CONSOLE_RAW_END_SIG);

    log_fresh();
    fake_console_raw_refuse(false);

    type_str("app inst");
    ask_raw(ao_spy(), 64U);
    QASM_DISPATCH(AO_Log, &end, 0U);

    /* Back in the editor, and the abandoned text is not a prefix of the next
     * command. */
    ao_spy_drain();
    type_str("ps\r");
    TEST_ASSERT_EQUAL_STRING("ps", published());
}

/* A refusal is still an answer. The requester has gone into its receiving
 * state on the strength of this request, so silence would leave it there
 * until its own watchdog fired. */
TEST_CASE("a raw request the transport refuses is answered, not dropped",
          "[log_ao]")
{
    log_fresh();
    fake_console_raw_refuse(true);

    ask_raw(ao_spy(), 64U);

    TEST_ASSERT_FALSE(fake_console_raw_open());

    const QEvt * const rsp = ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(rsp, "a refused request must still answer");
    TEST_ASSERT_EQUAL_INT(CONSOLE_RAW_RSP_SIG, rsp->sig);
    TEST_ASSERT_NOT_EQUAL_INT(KODE_OK, ((ConsoleRawRspEvt const *)rsp)->status);

    fake_console_raw_refuse(false);
}

/* A transfer of nothing is a bad request, not an empty one: raw mode ends
 * when the byte count runs out, and a count of zero has already run out. */
TEST_CASE("a raw request for no bytes is refused", "[log_ao]")
{
    log_fresh();
    fake_console_raw_refuse(false);

    ask_raw(ao_spy(), 0U);

    TEST_ASSERT_FALSE(fake_console_raw_open());

    const QEvt * const rsp = ao_spy_take();
    TEST_ASSERT_NOT_NULL(rsp);
    TEST_ASSERT_EQUAL_INT(CONSOLE_RAW_RSP_SIG, rsp->sig);
    TEST_ASSERT_NOT_EQUAL_INT(KODE_OK, ((ConsoleRawRspEvt const *)rsp)->status);
}

/* A request naming nobody. The accept branch has always treated it as a bad
 * request; the refusal branch used to answer it by posting to NULL, which is
 * a dereference rather than a refusal. No caller does this today - which is
 * exactly why it would have been found by the next one, at runtime.
 *
 * Note how this one fails without the fix: it does not fail, it HANGS, deep
 * inside QP's post. That is worth knowing before assuming a green run means
 * this case did anything. */
TEST_CASE("a raw request naming no requester is refused, not dereferenced",
          "[log_ao]")
{
    log_fresh();
    fake_console_raw_refuse(false);

    ask_raw(NULL, 64U);

    TEST_ASSERT_FALSE(fake_console_raw_open());
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
}

/* ---- The reader's own retry (log_on_byte, not handle_byte) --------------- */

/* Every case above dispatches a ConsoleByteEvt straight into AO_Log, which
 * exercises handle_byte() but never log_on_byte() itself - the allocation a
 * burst wider than the small pool can fail, and the retry that is supposed
 * to cover it. fake_console_feed_byte() is the seam that reaches it: the
 * captured on_byte() pointer, called synchronously, no task involved. */

#define RX_FILL_MAX  40U   /* headroom above the host harness's 32-block pool */

/** @brief Hold as many small-pool blocks as it will give up. */
static size_t fill_pool(const QEvt *out[RX_FILL_MAX])
{
    size_t n = 0U;
    while ( RX_FILL_MAX > n )
    {
        ConsoleByteEvt *e = Q_NEW_X(ConsoleByteEvt, 0U, CONSOLE_BYTE_SIG);
        if ( NULL == e )
        {
            break;
        }
        out[n] = &e->super;
        ++n;
    }
    return n;
}

/** @brief Give every block fill_pool() took back to the pool. */
static void drain_pool(const QEvt *held[], size_t n)
{
    for ( size_t i = 0U; i < n; ++i )
    {
        QF_gc(held[i]);
    }
}

/* With room in the pool, the byte goes straight through: no retry, no delay,
 * one event lands on AO_Log's own queue. This is the branch every other case
 * in this file exercises indirectly by dispatching the equivalent event by
 * hand; this one goes through log_on_byte() itself to say so explicitly. */
TEST_CASE("the reader posts immediately when the pool has room", "[log_ao]")
{
    log_fresh();
    reader_ready();

    uint32_t const before_ms   = khal_time_ms();
    uint32_t const before_drop = console_rx_dropped();

    fake_console_feed_byte((int)'p');

    TEST_ASSERT_EQUAL_UINT32(before_ms, khal_time_ms());
    TEST_ASSERT_EQUAL_UINT32(before_drop, console_rx_dropped());

    const QEvt *e = NULL;
    TEST_ASSERT_EQUAL_INT(pdTRUE, xQueueReceive(AO_Log->eQueue, &e, 0U));
    TEST_ASSERT_EQUAL_INT(CONSOLE_BYTE_SIG, e->sig);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)'p', ((ConsoleByteEvt const *)e)->byte);
    QF_gc(e);
}

/* A pool that stays empty for the whole retry window is what a byte lost to
 * something genuinely wedged looks like, as opposed to one that lost the
 * race by a millisecond. log_on_byte() must not hang the reader task over
 * it - it retries, gives up, and counts the loss instead of pretending
 * nothing happened. The elapsed time proves it actually waited rather than
 * giving up on the first miss. */
TEST_CASE("the reader retries a full pool, then gives the byte up and counts it",
          "[log_ao]")
{
    log_fresh();
    reader_ready();

    const QEvt *held[RX_FILL_MAX];
    size_t const n = fill_pool(held);
    TEST_ASSERT_GREATER_THAN_UINT(0U, n);

    uint32_t const before_ms   = khal_time_ms();
    uint32_t const before_drop = console_rx_dropped();

    fake_console_feed_byte((int)'x');

    TEST_ASSERT_EQUAL_UINT32(before_drop + 1U, console_rx_dropped());
    TEST_ASSERT_GREATER_THAN_UINT32(before_ms, khal_time_ms());

    /* Not merely counted: nothing was posted either. A queue that somehow
     * got an event here would mean the byte was both dropped and kept. */
    const QEvt *stray = NULL;
    TEST_ASSERT_NOT_EQUAL_INT(pdTRUE, xQueueReceive(AO_Log->eQueue, &stray, 0U));

    drain_pool(held, n);
}

/* The byte path leaves KQP_BYTE_POOL_MARGIN blocks to everybody else: with
 * the pool down to the margin the byte is dropped and counted, and the
 * blocks left are still there for an allocation that takes no margin, as the
 * storage and expander answers do. */
TEST_CASE("a byte flood leaves the small pool its margin and is counted",
          "[log_ao]")
{
    log_fresh();
    reader_ready();

    const QEvt *held[RX_FILL_MAX];
    size_t n = 0U;
    while ( RX_FILL_MAX > n )
    {
        ConsoleByteEvt *e = Q_NEW_X(ConsoleByteEvt, KQP_BYTE_POOL_MARGIN, CONSOLE_BYTE_SIG);
        if ( NULL == e )
        {
            break;
        }
        held[n] = &e->super;
        ++n;
    }
    TEST_ASSERT_GREATER_THAN_UINT(0U, n);

    uint32_t const before_drop = console_rx_dropped();

    fake_console_feed_byte((int)'x');

    TEST_ASSERT_EQUAL_UINT32(before_drop + 1U, console_rx_dropped());

    /* Another AO's allocation, with no margin of its own, still gets all of it. */
    const QEvt *others[RX_FILL_MAX];
    size_t const m = fill_pool(others);
    TEST_ASSERT_EQUAL_UINT(KQP_BYTE_POOL_MARGIN, m);

    drain_pool(others, m);
    drain_pool(held, n);
}

/* handle_byte()'s own allocation - the finished LINE, not the byte - runs
 * inside AO_Log's dispatch and cannot retry (rule 5: an AO step never
 * blocks). A pool this empty loses the whole line, not one byte of it, and
 * that loss is worth counting on the spot rather than swallowing silently. */
TEST_CASE("a finished line lost to a full pool is counted, not swallowed",
          "[log_ao]")
{
    log_fresh();

    const QEvt *held[RX_FILL_MAX];
    size_t const n = fill_pool(held);
    TEST_ASSERT_GREATER_THAN_UINT(0U, n);

    uint32_t const before_drop = console_rx_dropped();

    type_str("ps\r");

    TEST_ASSERT_EQUAL_UINT32(before_drop + 1U, console_rx_dropped());
    TEST_ASSERT_NULL_MESSAGE(published(),
                              "a line the pool could not carry must not "
                              "reach the console dispatcher either");

    drain_pool(held, n);
}

/* ---- The console's log level ------------------------------------------------ */

/** @brief Hand AO_Log a level the way log_level_request() would. */
static void set_level(const char *tag, kode_log_lvl_t lvl)
{
    LogLevelEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(LOG_LEVEL_SET_SIG);
    if ( NULL != tag )
    {
        (void)snprintf(e.tag, sizeof(e.tag), "%s", tag);
    }
    e.level = (uint8_t)lvl;
    QASM_DISPATCH(AO_Log, &e.super, 0U);
}

/** @brief The level under the `log` key, or -1 when there is none. */
static int stored_level(void)
{
    uint8_t v = 0U;

    if ( khal_nvs_get_u8(KSET_NAMESPACE, KSET_KEY_LOG, &v) != KODE_OK )
    {
        return -1;
    }
    return (int)v;
}

/**
 * @brief Whether a line the capture sink holds contains @p needle.
 *
 * The stored level is applied before AO_Log installs its own sink, so what
 * it says goes to whichever sink was there - on the board klog's rescue
 * path, here the suite's capture.
 */
static bool captured_has(const char *needle)
{
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const line = fake_klog_line(i);
        if ( (NULL != line) && (strstr(line, needle) != NULL) )
        {
            return true;
        }
    }
    return false;
}

/* The global level is the one the console runs at, so it is the one that
 * must survive the reset opening the port causes. And only what differs
 * from the build is kept: set back to the build's own level, the key goes,
 * so "absent" and "default" are one state. */
TEST_CASE("the global level is applied and stored, and the build's own "
          "level forgets it", "[log_ao]")
{
    fake_nvs_reset();
    log_fresh();

    set_level(NULL, KODE_LOG_NOTICE);
    TEST_ASSERT_EQUAL_INT(KODE_LOG_NOTICE, kode_log_get_level());
    TEST_ASSERT_EQUAL_INT(KODE_LOG_NOTICE, stored_level());

    KLOG_I("t.lvl", "chatter");
    KLOG_N("t.lvl", "outcome");
    TEST_ASSERT_EQUAL_UINT(1U, fake_klog_count());
    TEST_ASSERT_EQUAL_STRING("!N 0 t.lvl outcome\n", fake_klog_last());

    set_level(NULL, kode_log_build_level());
    TEST_ASSERT_EQUAL_INT(kode_log_build_level(), kode_log_get_level());
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, stored_level(),
        "the build's own level must erase the key, not store a copy of it");

    fake_klog_reset();
    fake_nvs_reset();
}

/* A per-tag level is a bench tool for one boot. Stored, sixteen of them
 * nobody remembers would outlive the session that set them. */
TEST_CASE("a per-tag level is applied and never stored", "[log_ao]")
{
    fake_nvs_reset();
    log_fresh();

    set_level("t.tag", KODE_LOG_WARN);
    TEST_ASSERT_EQUAL_INT(-1, stored_level());

    KLOG_N("t.tag", "hidden");
    KLOG_W("t.tag", "shown");
    TEST_ASSERT_EQUAL_UINT(1U, fake_klog_count());
    TEST_ASSERT_EQUAL_STRING("!W 0 t.tag shown\n", fake_klog_last());

    /* klog's table outlives the case; open the tag again for the rest. */
    kode_log_set_level("t.tag", KODE_LOG_DEBUG);
    fake_klog_reset();
}

/* The stored level is applied before any other AO starts, and a board that
 * hides lines says so in one that the chosen level still lets through. */
TEST_CASE("a stored level is applied at bring-up, and said", "[log_ao]")
{
    uint8_t const notice = (uint8_t)KODE_LOG_NOTICE;

    fake_nvs_reset();
    fake_nvs_seed(KSET_NAMESPACE, KSET_KEY_LOG, &notice, 1U);
    fake_klog_reset();

    log_bring_up();
    kode_log_lvl_t const applied = kode_log_get_level();
    bool const said = captured_has("log: level n (stored)");
    log_settle();

    TEST_ASSERT_EQUAL_INT(KODE_LOG_NOTICE, applied);
    TEST_ASSERT_TRUE_MESSAGE(said, "a stored level must announce itself");

    fake_nvs_reset();
}

/* A byte no level of this build answers to is not trusted: the level stays
 * where it was, and the reason is a warning rather than silence. */
TEST_CASE("a stored byte that is no level is ignored, with a warning",
          "[log_ao]")
{
    uint8_t const junk = 9U;

    fake_nvs_reset();
    fake_nvs_seed(KSET_NAMESPACE, KSET_KEY_LOG, &junk, 1U);
    fake_klog_reset();
    kode_log_lvl_t const before = kode_log_get_level();

    log_bring_up();
    kode_log_lvl_t const after = kode_log_get_level();
    bool const said = captured_has("is not one this build has");
    log_settle();

    TEST_ASSERT_EQUAL_INT(before, after);
    TEST_ASSERT_TRUE(said);

    fake_nvs_reset();
}

/* ---- config key="log" ------------------------------------------------------ */

/** @brief The words `config` takes for the level, in kode_log_lvl_t order. */
static const char *const CONFIG_LEVEL_WORDS[] = KODE_LOG_LEVEL_WORDS;

/** @brief Put the level and the store back as the suite's sink leaves them. */
static void config_level_restored(void)
{
    fake_klog_reset();
    fake_nvs_reset();
    ao_spy_drain();
}

/* A read must say the level in force without touching anything: a read that
 * stored would turn every `config` listing into a flash write. */
TEST_CASE("config log reads the level in force and stores nothing",
          "[log_ao]")
{
    const KodeConfigAnsEvt *a;

    fake_nvs_reset();
    log_fresh();
    /* Off the suite's own level, so a reply that ignores the level in force
     * cannot match it by luck. */
    kode_log_set_level(NULL, KODE_LOG_WARN);

    a = config_ask(AO_Log, &(config_ask_t){ .key = KSET_KEY_LOG });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->status);
    TEST_ASSERT_EQUAL_STRING(CONFIG_LEVEL_WORDS[KODE_LOG_WARN], a->value);
    TEST_ASSERT_FALSE_MESSAGE(a->stored,
        "a level nobody stored must not claim to be a stored one");
    TEST_ASSERT_EQUAL_INT(-1, stored_level());

    config_level_restored();
}

/* The enum word's index is the level: if the table and the levels drift
 * apart, `config key="log" value=w` would silence the wrong lines. Every word
 * is walked, and the answer is the word the level reads back as. */
TEST_CASE("config log maps every word to its level, stores it and answers it",
          "[log_ao]")
{
    const KodeConfigAnsEvt *a;

    fake_nvs_reset();
    log_fresh();

    for ( uint8_t i = 0U; (uint8_t)KODE_LOG_OFF >= i; ++i )
    {
        a = config_ask(AO_Log, &(config_ask_t){ .key = KSET_KEY_LOG, .set = true,
                                                .e = i });
        TEST_ASSERT_NOT_NULL(a);
        TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->status);
        TEST_ASSERT_EQUAL_STRING(CONFIG_LEVEL_WORDS[i], a->value);
        TEST_ASSERT_EQUAL_INT((int)i, (int)kode_log_get_level());
        /* The build's own level is the one value that leaves no key. */
        if ( (uint8_t)kode_log_build_level() == i )
        {
            TEST_ASSERT_FALSE(a->stored);
            TEST_ASSERT_EQUAL_INT(-1, stored_level());
        }
        else
        {
            TEST_ASSERT_TRUE(a->stored);
            TEST_ASSERT_EQUAL_INT((int)i, stored_level());
        }
        ao_spy_drain();
    }

    config_level_restored();
}

/* The console refuses a word that is no level before it asks anyone; an owner
 * handed an index past `off` anyway keeps the level it has and says so, instead
 * of storing a byte the next boot would warn about. */
TEST_CASE("config log with an index past off keeps the level and stores nothing",
          "[log_ao]")
{
    const KodeConfigAnsEvt *a;

    fake_nvs_reset();
    log_fresh();
    kode_log_set_level(NULL, KODE_LOG_WARN);

    a = config_ask(AO_Log, &(config_ask_t){ .key = KSET_KEY_LOG, .set = true,
                                            .e = (uint8_t)((uint8_t)KODE_LOG_OFF + 1U) });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING(CONFIG_LEVEL_WORDS[KODE_LOG_WARN], a->value);
    TEST_ASSERT_FALSE(a->stored);
    TEST_ASSERT_EQUAL_INT(KODE_LOG_WARN, kode_log_get_level());
    TEST_ASSERT_EQUAL_INT(-1, stored_level());

    config_level_restored();
}

/* One key, one owner: a request for another AO's setting must go unanswered,
 * or the console would read two answers for one question. */
TEST_CASE("another owner's key is not AO_Log's to answer", "[log_ao]")
{
    fake_nvs_reset();
    log_fresh();

    TEST_ASSERT_NULL(config_ask(AO_Log, &(config_ask_t){ .key = KSET_KEY_BRIGHT }));

    config_level_restored();
}

/* ---- Replies to the Kode channel's commands --------------------------------
 * AO_Log writes every line to the cable and to BLE as it always did. A reply
 * tagged NET also goes, without its newline, into the ring AO_Net drains onto
 * cmd/out. Nothing else does: not the replies to the cable or to BLE, and not
 * a '!' line.
 */

/** @brief The ring, empty and with nobody to wake. */
static void ring_emptied(void)
{
    char scratch[KODE_LOG_LINE_MAX + 1U];

    TEST_ASSERT_TRUE(kchan_out_init());
    kchan_out_set_wake(NULL);
    while ( kchan_out_pop(scratch, sizeof(scratch)) != 0U )
    {
        /* emptied */
    }
}

TEST_CASE("a reply to a NET command is written to the cable and kept for cmd/out",
          "[log_ao]")
{
    char got[KODE_LOG_LINE_MAX + 1U];
    size_t n;

    log_fresh();
    ring_emptied();

    log_line_src(">|pong\n", (uint8_t)KODE_CONSOLE_SRC_NET);

    n = kchan_out_pop(got, sizeof(got));
    TEST_ASSERT_EQUAL_STRING(">|pong\n", fake_console_out());
    TEST_ASSERT_EQUAL_UINT(6U, n);
    TEST_ASSERT_EQUAL_STRING(">|pong", got);
    TEST_ASSERT_FALSE(kchan_out_pending());
}

TEST_CASE("replies to the cable or BLE, and log lines, never reach cmd/out",
          "[log_ao]")
{
    log_fresh();
    ring_emptied();

    log_line_src(">ok\n", (uint8_t)KODE_CONSOLE_SRC_SERIAL);
    log_line_src(">ok\n", (uint8_t)KODE_CONSOLE_SRC_BLE);
    log_line_src(">ok\n", (uint8_t)KODE_LOG_SRC_NONE);
    log_line_src("!N 12 kaos.net channel: up as kode-X\n",
                 (uint8_t)KODE_LOG_SRC_NONE);

    TEST_ASSERT_FALSE(kchan_out_pending());
}

/** @brief Most LogEvt blocks the harness's pool holds, with headroom. */
#define LOG_FILL_MAX  80U

/* A reply the owner waits for on the channel that the event pool could not
 * carry is lost on its way to AO_Log, before it can reach the ring. It is
 * counted as lost for the channel, so `channel_info` says so; a reply to the
 * cable or a log line the pool could not carry is only counted by `ps`. */
TEST_CASE("a NET reply lost to a dry event pool is counted as lost for the "
          "channel; a cable reply is not", "[log_ao]")
{
    const QEvt *held[LOG_FILL_MAX];
    size_t      n = 0U;
    uint32_t    lost0;
    uint32_t    lost_net;
    uint32_t    lost_cable;

    log_bring_up();                 /* AO_Log's own sink is the klog sink now */
    ring_emptied();
    lost0 = kchan_out_lost();

    while ( LOG_FILL_MAX > n )
    {
        LogEvt *e = Q_NEW_X(LogEvt, 0U, LOG_LINE_SIG);
        if ( NULL == e )
        {
            break;
        }
        held[n] = &e->super;
        ++n;
    }
    TEST_ASSERT_GREATER_THAN_UINT(0U, n);

    kode_log_reply_src(">|pong", (uint8_t)KODE_CONSOLE_SRC_NET);
    lost_net = kchan_out_lost();
    kode_log_reply_src(">|pong", (uint8_t)KODE_CONSOLE_SRC_SERIAL);
    kode_log_reply_src(">|pong", (uint8_t)KODE_CONSOLE_SRC_BLE);
    lost_cable = kchan_out_lost();

    for ( size_t i = 0U; i < n; ++i )
    {
        QF_gc(held[i]);
    }
    log_settle();

    TEST_ASSERT_EQUAL_UINT32(lost0 + 1U, lost_net);
    TEST_ASSERT_EQUAL_UINT32(lost0 + 1U, lost_cable);
}

/* ---- The console's congestion, through kode_sys -------------------------- */

/* The cases before this one throw queued lines away unwritten, and each still
 * counts as waiting. A line AO_Log writes is one off the count, so empty lines
 * handed straight to it bring the count back under the limit first. */
TEST_CASE("kode_sys_console_congested() follows the lines waiting for the "
          "console", "[log_ao]")
{
    uint32_t i = 0U;

    log_bring_up();
    log_run();
    while ( console_congested() && (KQP_LARGE_POOL_COUNT > i) )
    {
        log_line_src("", (uint8_t)KODE_LOG_SRC_NONE);
        ++i;
    }
    TEST_ASSERT_FALSE(kode_sys_console_congested());

    for ( i = 0U; !console_congested() && (KQP_LARGE_POOL_COUNT > i); ++i )
    {
        KLOG_W("t.cong", "line %u", (unsigned)i);
    }
    TEST_ASSERT_TRUE_MESSAGE(kode_sys_console_congested(),
        "a backlog the console's own test sees is not seen through kode_sys");

    log_run();
    TEST_ASSERT_FALSE(kode_sys_console_congested());
    log_settle();
}

/* ---- The dock console's pins, lent to the station (log_dock_lend_request) --- */

/** @brief The dock's answer the spy received, or NULL if none came. */
static const LogDockEvt *dock_answer(void)
{
    const LogDockEvt *found = NULL;

    for ( const QEvt *e = ao_spy_take(); NULL != e; e = ao_spy_take() )
    {
        if ( LOG_DOCK_SIG == e->sig )
        {
            found = (const LogDockEvt *)e;
        }
    }
    return found;
}

TEST_CASE("test_dock_lend_and_take_back", "[log_ao][test_dot]")
{
    log_fresh();
    qf_support_margin_queue(AO_Log);   /* the request posts with a margin */

    TEST_ASSERT_TRUE(log_dock_lend_request(ao_spy(), true));
    log_run();
    const LogDockEvt *a = dock_answer();
    TEST_ASSERT_NOT_NULL_MESSAGE(a, "the lend is answered");
    TEST_ASSERT_TRUE(a->lent);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->status);
    TEST_ASSERT_TRUE_MESSAGE(fake_console_dock_lent(), "TinyUSB let the pads go");
    TEST_ASSERT_EQUAL_UINT(1U, fake_console_dock_releases());

    TEST_ASSERT_TRUE(log_dock_lend_request(ao_spy(), false));
    log_run();
    a = dock_answer();
    TEST_ASSERT_NOT_NULL_MESSAGE(a, "the take back is answered");
    TEST_ASSERT_FALSE(a->lent);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->status);
    TEST_ASSERT_FALSE_MESSAGE(fake_console_dock_lent(), "the dock is back on its pins");
    TEST_ASSERT_EQUAL_UINT(1U, fake_console_dock_takes());
    log_settle();
}

TEST_CASE("test_dock_take_back_refused_says_so_and_leaves_the_pins_free",
          "[log_ao][test_dot]")
{
    log_fresh();
    qf_support_margin_queue(AO_Log);   /* the request posts with a margin */
    TEST_ASSERT_TRUE(log_dock_lend_request(ao_spy(), true));
    log_run();
    (void)dock_answer();

    fake_console_dock_take_rc(KODE_ERROR);
    TEST_ASSERT_TRUE(log_dock_lend_request(ao_spy(), false));
    log_run();
    const LogDockEvt * const a = dock_answer();
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT8(KODE_ERROR, a->status);
    TEST_ASSERT_TRUE(a->lent);
    TEST_ASSERT_FALSE_MESSAGE(log_dock_lend_request(NULL, true), "nobody to tell");
    fake_console_dock_take_rc(KODE_OK);
    log_settle();
}
