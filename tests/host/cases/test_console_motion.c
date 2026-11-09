/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_motion.c
 * @brief `haptic`, `haptic_play` and the three motion rows, from the wire.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "Mockkhal_clk.h"

#include "kdrv_clocks.h"   /* KDRV_CLOCKS_CYCLES_FAST: what the row asks for */

#include "qpc.h"

#include "ao_spy.h"
#include "autotest_support.h"
#include "fakes.h"
#include "kaos.h"
#include "kaos_wire.h"
#include "kcli_proto.h"
#include "kdrv_aw86233.h"
#include "kqp.h"
#include "qf_support.h"
#include "owners_support.h"

/* The command watchdog, in QP ticks - repeated rather than imported for the
 * same reason the other console files repeat it. The third one is what
 * expires a command an earlier file left in flight; it has to outlast the
 * longest budget any row carries, which is the card's. */
#define CMD_TIMEOUT_TICKS    ((5000U * 100U) / 1000U)
#define ANY_BUDGET_TICKS     ((200000U * 100U) / 1000U)

/** @brief The IMU row's own budget, in whole QP ticks. */
#define IMU_BUDGET_TICKS     ((KODE_IMU_TEST_BUDGET_MS * 100U) / 1000U)
/** @brief The magnetometer row's own budget, in whole QP ticks. */
#define MAG_BUDGET_TICKS     ((KODE_MAG_TEST_BUDGET_MS * 100U) / 1000U)
/** @brief The haptic row's own budget, in whole QP ticks. */
#define HAPTIC_BUDGET_TICKS  ((KODE_HAPTIC_TEST_BUDGET_MS * 100U) / 1000U)

/** @brief Empty one Active Object's mailbox. */
static void drain(QActive *ao)
{
    const QEvt *e;

    qf_support_give_queue(ao);
    while ( xQueueReceive(ao->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

static void console_up(void)
{
    qf_support_init();
    autotest_up();
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
    /* Every owner `all` reaches gets its queue BEFORE the console is ticked,
     * because the tick below can walk a run an earlier case left half done,
     * and a post to an Active Object with no queue aborts inside FreeRTOS
     * rather than failing a case. */
    qf_support_give_queue(AO_Power);
    qf_support_give_queue(AO_IoExp);
    qf_support_give_queue(AO_Imu);
    qf_support_give_queue(AO_Ir);
    Haptic_ctor();   /* gives back the request door a case left claimed */
    qf_support_give_queue(AO_Haptic);
    qf_support_give_queue(AO_Storage);
    qf_support_give_queue(AO_Radio);
    qf_support_give_queue(AO_Nfc);
    qf_support_give_queue(AO_Rfid);
    qf_support_give_queue(AO_Audio);
    qf_support_give_queue(AO_Display);
    qf_support_give_queue(AO_LedMgr);
    qf_support_give_queue(AO_Buttons);
    qf_support_give_queue(AO_Touch);

    /* Whatever an earlier file left in flight is timed out here, so every
     * case starts at an idle prompt. */
    (void)qf_support_tick(AO_Console, ANY_BUDGET_TICKS);

    drain(AO_Power);
    drain(AO_IoExp);
    drain(AO_Imu);
    drain(AO_Ir);
    drain(AO_Haptic);
    drain(AO_Storage);
    drain(AO_Radio);
    drain(AO_Nfc);
    drain(AO_Rfid);
    drain(AO_Audio);
    drain(AO_Display);
    drain(AO_LedMgr);
    drain(AO_Buttons);
    drain(AO_Touch);

    fake_klog_reset();
    fake_neighbours_reset();
}

static void type_line(const char *text)
{
    ConsoleLineEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(CONSOLE_LINE_SIG);
    e.src   = (uint8_t)KODE_CONSOLE_SRC_SERIAL;
    (void)snprintf(e.line, sizeof(e.line), "%s", text);
    QASM_DISPATCH(AO_Console, &e.super, 0U);
    autotest_typed(text);
}

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
 * @brief Take the field request the console posted to AO_Haptic, if any.
 *
 * @param out A copy of it.
 * @return false when nothing was posted.
 */
static bool posted_haptic_field(HapticFieldReqEvt *out)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Haptic->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(HAPTIC_FIELD_REQ_SIG, e->sig);
    *out = *(const HapticFieldReqEvt *)e;
    QF_gc(e);
    return true;
}

/** @brief Was one buzz asked of AO_Haptic? Consumes the request. */
static bool posted_play(HapticPlayEvt *out)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Haptic->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(HAPTIC_PLAY_REQ_SIG, e->sig);
    *out = *(const HapticPlayEvt *)e;
    QF_gc(e);
    return true;
}

/** @brief Was the IMU row asked of AO_Imu? Consumes the request. */
static bool posted_imu_row(void)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Imu->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(IMU_TEST_REQ_SIG, e->sig);
    QF_gc(e);
    return true;
}

/** @brief Was the magnetometer row asked of AO_Imu? Consumes the request. */
static bool posted_mag_row(void)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Imu->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(MAG_TEST_REQ_SIG, e->sig);
    QF_gc(e);
    return true;
}

/** @brief Was the haptic row asked of AO_Haptic? Consumes the request. */
static bool posted_haptic_row(void)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Haptic->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(HAPTIC_TEST_REQ_SIG, e->sig);
    QF_gc(e);
    return true;
}

/** @brief Publish AO_Haptic's answer to a field request. */
static void answer_haptic_field(uint8_t op, uint16_t field, int32_t value,
                                kode_status_t rc)
{
    HapticFieldEvt a;

    (void)memset(&a, 0, sizeof(a));
    a.super = (QEvt)QEVT_INITIALIZER(HAPTIC_FIELD_SIG);
    a.op    = op;
    a.field = field;
    a.value = value;
    a.rc    = (uint8_t)rc;
    QASM_DISPATCH(AO_Console, &a.super, 0U);
}

/** @brief Publish a row's verdict, the way its owner ends one. */
static void verdict(const char *name, bool ok)
{
    TestResultEvt r;

    (void)memset(&r, 0, sizeof(r));
    r.super = (QEvt)QEVT_INITIALIZER(TEST_RESULT_SIG);
    (void)snprintf(r.name, sizeof(r.name), "%s", name);
    r.ok = ok;
    r.origin = TEST_ORIGIN_AUTOTEST;
    QASM_DISPATCH(AO_Autotest, &r.super, 0U);
    QASM_DISPATCH(AO_Console, &r.super, 0U);
    autotest_settle();
}

/** @brief Publish one of the IMU row's step records. */
static void record_imu(uint8_t step, uint8_t phase, uint8_t flags,
                       int32_t a, int32_t b, int32_t c, int32_t d)
{
    ImuTestEvt t;

    (void)memset(&t, 0, sizeof(t));
    t.super = (QEvt)QEVT_INITIALIZER(IMU_TEST_SIG);
    t.step  = step;
    t.phase = phase;
    t.flags = flags;
    t.a     = a;
    t.b     = b;
    t.c     = c;
    t.d     = d;
    QASM_DISPATCH(AO_Console, &t.super, 0U);
}

/** @brief Publish one of the magnetometer row's step records. */
static void record_mag(uint8_t step, uint8_t phase, uint8_t flags,
                       int32_t a, int32_t b, int32_t c, int32_t d)
{
    MagTestEvt t;

    (void)memset(&t, 0, sizeof(t));
    t.super = (QEvt)QEVT_INITIALIZER(MAG_TEST_SIG);
    t.step  = step;
    t.phase = phase;
    t.flags = flags;
    t.a     = a;
    t.b     = b;
    t.c     = c;
    t.d     = d;
    QASM_DISPATCH(AO_Console, &t.super, 0U);
}

/**
 * @brief The one captured line holding @p needle, or NULL.
 *
 * For the assertions a substring cannot make: that a record is exactly the
 * documented one, and that it carries no key it must not.
 */
static const char *line_with(const char *needle)
{
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, needle) != NULL) )
        {
            return l;
        }
    }
    return NULL;
}

/** @brief Publish one of the haptic row's step records. */
static void record_haptic(uint8_t step, uint8_t phase, uint8_t flags,
                          uint8_t gain, int32_t floor_x10, int32_t on_x10,
                          uint32_t ratio_x10, uint16_t n)
{
    HapticTestEvt t;

    (void)memset(&t, 0, sizeof(t));
    t.super     = (QEvt)QEVT_INITIALIZER(HAPTIC_TEST_SIG);
    t.step      = step;
    t.phase     = phase;
    t.flags     = flags;
    t.gain      = gain;
    t.floor_x10 = floor_x10;
    t.on_x10    = on_x10;
    t.ratio_x10 = ratio_x10;
    t.n         = n;
    QASM_DISPATCH(AO_Autotest, &t.super, 0U);
    QASM_DISPATCH(AO_Console, &t.super, 0U);
}

/* ---- The haptic field table -------------------------------------------------
 * The only one of the three motion chips that still has one. The IMU and the
 * magnetometer are production rows and two facade reads now, so `imu`, `mag`
 * and their two help verbs are gone from the command table - which
 * test_console_parse.c is where anybody notices.
 */

TEST_CASE("haptic with no keys asks for the whole chip and closes on the answer",
          "[console_motion]")
{
    HapticFieldReqEvt q;

    console_up();
    type_line("haptic");
    TEST_ASSERT_TRUE(posted_haptic_field(&q));
    TEST_ASSERT_EQUAL_UINT(FIELD_OP_DUMP, q.op);
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "closed before AO_Haptic answered");

    answer_haptic_field(FIELD_OP_DUMP, 0U, 0, KODE_OK);
    TEST_ASSERT_FALSE_MESSAGE(replied(">|chip=haptic"),
                              "a dump prints through the log, not as a record");
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("a field nobody printed is refused before AO_Haptic is asked",
          "[console_motion]")
{
    HapticFieldReqEvt q;

    console_up();
    type_line("haptic field=\"nope\"");
    TEST_ASSERT_TRUE_MESSAGE(replied(">err code=3 msg=field"),
                             "the key is named, because the name is what was wrong");
    TEST_ASSERT_FALSE_MESSAGE(posted_haptic_field(&q),
                              "nothing was asked of the owner");
}

TEST_CASE("haptic field=gain value=40 asks for a write and answers the readback",
          "[console_motion]")
{
    HapticFieldReqEvt q;

    console_up();
    type_line("haptic field=\"gain\" value=40");
    TEST_ASSERT_TRUE(posted_haptic_field(&q));
    TEST_ASSERT_EQUAL_UINT(FIELD_OP_SET, q.op);
    TEST_ASSERT_EQUAL_UINT(KDRV_AW_GAIN, q.field);
    TEST_ASSERT_EQUAL_INT(40, q.value);

    /* The readback, not the echo. */
    answer_haptic_field(FIELD_OP_SET, (uint16_t)KDRV_AW_GAIN, 40, KODE_OK);
    TEST_ASSERT_TRUE(replied(">|chip=haptic field=\"gain\" value=40"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("a refusal from an owner names the field, a failure names the verb",
          "[console_motion]")
{
    HapticFieldReqEvt h;

    /* The whole line, not a substring of it: `chipid` and `haptic` both occur
     * in the records a SUCCESS would print, so a loose match would pass over
     * either mapping being wrong. */
    console_up();
    type_line("haptic field=\"chipid\" value=1");
    TEST_ASSERT_TRUE(posted_haptic_field(&h));
    answer_haptic_field(FIELD_OP_SET, (uint16_t)KDRV_AW_CHIPID_FIELD, 0, KODE_DENIED);
    TEST_ASSERT_TRUE_MESSAGE(replied(">err code=3 msg=chipid"),
                             "an identity register is not written, and the field is named");
    TEST_ASSERT_FALSE(replied(">|chip=haptic"));

    fake_klog_reset();
    type_line("haptic field=\"gain\"");
    TEST_ASSERT_TRUE(posted_haptic_field(&h));
    answer_haptic_field(FIELD_OP_GET, (uint16_t)KDRV_AW_GAIN, 0, KODE_IO_ERROR);
    TEST_ASSERT_TRUE_MESSAGE(replied(">err code=5 msg=haptic"),
                             "a bus that said no names the verb, not the field");
    TEST_ASSERT_FALSE(replied(">|chip=haptic"));
}

TEST_CASE("haptic_help lists every field and closes with ok", "[console_motion]")
{
    /* The AW86233's table fits in one slice, so it closes without a tick. */
    console_up();
    type_line("haptic_help");
    TEST_ASSERT_TRUE(replied(">|field=\"present\" unit=bool min=0 max=1 rw=ro"));
    TEST_ASSERT_TRUE(replied("desc="));
    TEST_ASSERT_TRUE(replied(">|field=\"gain\""));
    TEST_ASSERT_TRUE(replied(">|field=\"glb_state\""));
    TEST_ASSERT_TRUE(replied(">|field=\"standby\""));
    TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)KDRV_AW_FIELD_MAX + 1U,
                                   fake_klog_count(),
                                   "one line per field, and then >ok");
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("every field of the haptic chip has a name and a description",
          "[console_motion]")
{
    /* The count is a _Static_assert in console_ao.c; this is the other half -
     * a hole left as a NULL by a designated initialiser would pass the count
     * and print nothing on the wire. */
    for ( uint32_t f = 0U; (uint32_t)KDRV_AW_FIELD_MAX > f; ++f )
    {
        const kdrv_aw86233_field_info_t * const info =
            kdrv_aw86233_field_info((kdrv_aw86233_field_t)f);
        kdrv_aw86233_field_t back = KDRV_AW_FIELD_MAX;

        TEST_ASSERT_NOT_NULL(info);
        TEST_ASSERT_NOT_NULL(info->name);
        TEST_ASSERT_TRUE_MESSAGE('\0' != info->name[0], "a field has no name");
        TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK,
            kdrv_aw86233_field_by_name(info->name, &back),
            "a field the table prints cannot be typed back at it");
        TEST_ASSERT_EQUAL_UINT(f, (uint32_t)back);
    }
}

/**
 * @brief Did the console print exactly @p expected as one whole line?
 *
 * A substring would pass a record carrying an extra key, which is exactly
 * the mistake step 3's missing `result=` is there to catch.
 */
static bool replied_exactly(const char *expected)
{
    size_t const n = strlen(expected);

    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strncmp(l, expected, n) == 0)
            && (('\n' == l[n]) || ('\r' == l[n]) || ('\0' == l[n])) )
        {
            return true;
        }
    }
    return false;
}

/* ---- haptic_play ----------------------------------------------------------- */

TEST_CASE("haptic_play posts ms and gain and answers the buzz it asked for",
          "[console_motion]")
{
    HapticPlayEvt p;

    console_up();
    type_line("haptic_play ms=200 gain=50");
    TEST_ASSERT_TRUE(posted_play(&p));
    TEST_ASSERT_EQUAL_UINT8(1U, p.n);
    TEST_ASSERT_EQUAL_UINT16(200U, p.seg_ms[0]);
    TEST_ASSERT_FALSE(p.hold);
    TEST_ASSERT_EQUAL_UINT8(50U, p.gain_pct);
    TEST_ASSERT_TRUE(replied(">|play=1 ms=200 gain=50"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("a bare haptic_play takes the row's own buzz", "[console_motion]")
{
    HapticPlayEvt p;

    console_up();
    type_line("haptic_play");
    TEST_ASSERT_TRUE(posted_play(&p));
    TEST_ASSERT_EQUAL_UINT8(1U, p.n);
    TEST_ASSERT_EQUAL_UINT16(200U, p.seg_ms[0]);
    TEST_ASSERT_FALSE(p.hold);
    TEST_ASSERT_EQUAL_UINT8(50U, p.gain_pct);
    TEST_ASSERT_TRUE(replied(">|play=1 ms=200 gain=50"));
}

TEST_CASE("haptic_play out of range is refused by the parser",
          "[console_motion]")
{
    HapticPlayEvt p;

    console_up();
    type_line("haptic_play ms=0");
    TEST_ASSERT_TRUE(replied(">err code=3"));
    TEST_ASSERT_FALSE_MESSAGE(posted_play(&p), "nothing was asked of the owner");

    fake_klog_reset();
    type_line("haptic_play ms=2001");
    TEST_ASSERT_TRUE(replied(">err code=3"));
    TEST_ASSERT_FALSE(posted_play(&p));

    fake_klog_reset();
    type_line("haptic_play gain=101");
    TEST_ASSERT_TRUE(replied(">err code=3"));
    TEST_ASSERT_FALSE(posted_play(&p));
}

/* ---- The two rows ---------------------------------------------------------- */

TEST_CASE("test component=imu and =haptic ask their owners and close on their verdicts",
          "[console_motion]")
{
    console_up();
    type_line("test component=imu");
    TEST_ASSERT_TRUE(posted_imu_row());
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "closed before AO_Imu answered");
    verdict("imu", true);
    TEST_ASSERT_TRUE(replied(">|test=imu result=pass"));
    TEST_ASSERT_TRUE(replied(">ok"));

    fake_klog_reset();
    type_line("test component=haptic");
    TEST_ASSERT_TRUE(posted_haptic_row());
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "closed before AO_Haptic answered");
    verdict("haptic", false);
    TEST_ASSERT_TRUE(replied(">|test=haptic result=fail"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("the imu row waits its own budget; so does the haptic row",
          "[console_motion]")
{
    /* Both budgets are SHORTER than a chip's five seconds, which is the
     * whole point of a row carrying its own: a motionless IMU is known to
     * have failed long before the generic watchdog would say so. */
    console_up();
    type_line("test component=imu");
    TEST_ASSERT_TRUE(posted_imu_row());

    (void)qf_support_tick(AO_Console, IMU_BUDGET_TICKS - 1U);
    TEST_ASSERT_FALSE_MESSAGE(replied(">|test=imu"),
                              "not yet: the row has its own budget");
    (void)qf_support_tick(AO_Console, 2U);
    TEST_ASSERT_TRUE(replied(">|test=imu result=timeout"));
    TEST_ASSERT_TRUE_MESSAGE(replied(">ok"),
                             "a row's silence is a record, not the end of it");
    TEST_ASSERT_FALSE(replied(">err code=6"));

    console_up();
    type_line("test component=haptic");
    TEST_ASSERT_TRUE(posted_haptic_row());

    (void)qf_support_tick(AO_Console, HAPTIC_BUDGET_TICKS - 1U);
    TEST_ASSERT_FALSE_MESSAGE(replied(">|test=haptic"),
                              "not yet: the row has its own budget");
    (void)qf_support_tick(AO_Console, 2U);
    TEST_ASSERT_TRUE(replied(">|test=haptic result=timeout"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("every record shape the imu row prints is the documented one",
          "[console_motion]")
{
    console_up();
    type_line("test component=imu");
    TEST_ASSERT_TRUE(posted_imu_row());

    record_imu(1U, IMU_TEST_WHOAMI, IMU_TF_PASS, 0x70, 0, 0, 0);
    TEST_ASSERT_TRUE(replied(">|step=1 of=3 whoami=0x70 result=pass"));

    record_imu(2U, IMU_TEST_ST, IMU_TF_PASS, 300, 300, 300, 0);
    TEST_ASSERT_TRUE(replied(
        ">|step=2 of=3 sensor=xl dx=300 dy=300 dz=300 result=pass"));

    record_imu(2U, IMU_TEST_ST, 0U, -10, 5, 7, 1);
    TEST_ASSERT_TRUE(replied(
        ">|step=2 of=3 sensor=gy dx=-10 dy=5 dz=7 result=fail"));

    record_imu(3U, IMU_TEST_GRAVITY, IMU_TF_PASS, 1004, 310, 284, 0);
    TEST_ASSERT_TRUE(replied(
        ">|step=3 of=3 a_mg=1004 w_mdps=310 temp_dc=284 result=pass"));

    verdict("imu", true);
    TEST_ASSERT_TRUE(replied(">|test=imu result=pass"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* The spec's own example run, line for line. Step 3 is a reading and carries
 * no `result=` at all: inventing a verdict for it would teach whoever reads a
 * run to read past one. */
TEST_CASE("every record shape the mag row prints is the documented one",
          "[console_motion]")
{
    const char *l;

    console_up();
    type_line("test component=mag");
    TEST_ASSERT_TRUE_MESSAGE(posted_mag_row(), "the row was not asked of AO_Imu");
    TEST_ASSERT_FALSE(replied(">ok"));

    record_mag(1U, MAG_TEST_WHOAMI, MAG_TF_PASS, 0x40, 0, 0, 0);
    TEST_ASSERT_TRUE(replied_exactly(">|step=1 whoami=0x40 result=pass"));

    record_mag(2U, MAG_TEST_ST, MAG_TF_PASS, 123, 98, 145, 0);
    TEST_ASSERT_TRUE(replied_exactly(">|step=2 st_x=123 st_y=98 st_z=145 result=pass"));

    record_mag(3U, MAG_TEST_FIELD, 0U, -914, 767, -679, 1370);
    TEST_ASSERT_TRUE(replied_exactly(">|step=3 bx=-914 by=767 bz=-679 babs=1370"));
    l = line_with(">|step=3");
    TEST_ASSERT_NOT_NULL(l);
    TEST_ASSERT_NULL_MESSAGE(strstr(l, "result="), "a reading carries a verdict");

    fake_klog_reset();
    record_mag(3U, MAG_TEST_DONE, MAG_TF_PASS, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_klog_count(),
                                   "the end marker printed a record of its own");

    verdict("mag", true);
    TEST_ASSERT_TRUE(replied_exactly(">|test=mag result=pass"));
    TEST_ASSERT_TRUE(replied_exactly(">ok"));

    /* The two failing shapes: a part that is not there, and a shift that is
     * not in the window. */
    console_up();
    type_line("test component=mag");
    TEST_ASSERT_TRUE(posted_mag_row());
    record_mag(1U, MAG_TEST_WHOAMI, MAG_TF_ABSENT, 0, 0, 0, 0);
    TEST_ASSERT_TRUE(replied_exactly(">|step=1 mag=absent result=absent"));
    record_mag(1U, MAG_TEST_WHOAMI, MAG_TF_MOVED, 0x0C, 0, 0, 0);
    TEST_ASSERT_TRUE(replied_exactly(">|step=1 mag=moved addr=0x0C result=fail"));
    record_mag(1U, MAG_TEST_WHOAMI, 0U, 0x41, 0, 0, 0);
    TEST_ASSERT_TRUE(replied_exactly(">|step=1 whoami=0x41 result=fail"));
    record_mag(2U, MAG_TEST_ST, 0U, 123, 8, -145, 0);
    TEST_ASSERT_TRUE(replied_exactly(">|step=2 st_x=123 st_y=8 st_z=-145 result=fail"));
    verdict("mag", false);
    TEST_ASSERT_TRUE(replied_exactly(">|test=mag result=fail"));
}

TEST_CASE("the mag row waits its own budget", "[console_motion]")
{
    console_up();
    type_line("test component=mag");
    TEST_ASSERT_TRUE(posted_mag_row());

    (void)qf_support_tick(AO_Console, MAG_BUDGET_TICKS - 1U);
    TEST_ASSERT_FALSE_MESSAGE(replied(">|test=mag"),
                              "not yet: the row has its own budget");
    (void)qf_support_tick(AO_Console, 2U);
    TEST_ASSERT_TRUE(replied_exactly(">|test=mag result=timeout"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("every record shape the haptic row prints is the documented one",
          "[console_motion]")
{
    console_up();
    type_line("test component=haptic");
    TEST_ASSERT_TRUE(posted_haptic_row());

    record_haptic(0U, HAPTIC_TEST_ABSENT, 0U, 0U, 0, 0, 0U, 0U);
    TEST_ASSERT_TRUE(replied(">|step=0 haptic=absent result=absent"));

    record_haptic(1U, HAPTIC_TEST_FLOOR, HAPTIC_TF_PASS, 0U, 4, 0, 0U, 256U);
    TEST_ASSERT_TRUE(replied(">|step=1 floor_235=4 n=256 result=pass"));

    /* A bench that already carries 235 Hz is judged, not the board. */
    record_haptic(1U, HAPTIC_TEST_FLOOR, HAPTIC_TF_LOUD, 0U, 5000, 0, 0U, 256U);
    TEST_ASSERT_TRUE(replied(">|step=1 floor_235=5000 n=256 result=loud"));

    record_haptic(2U, HAPTIC_TEST_GAIN, HAPTIC_TF_PASS, 0x40U, 4, 1460, 730U,
                  256U);
    TEST_ASSERT_TRUE(replied(
        ">|step=2 gain=0x40 on_235=1460 ratio=73.0 n=256 result=pass"));

    record_haptic(2U, HAPTIC_TEST_GAIN, 0U, 0x80U, 4, 10, 25U, 256U);
    TEST_ASSERT_TRUE(replied(">|step=2 gain=0x80 on_235=10 ratio=2.5 n=256 result=fail"));

    fake_klog_reset();
    record_haptic(2U, HAPTIC_TEST_NOVERDICT, HAPTIC_TF_MUTE, 0U, 0, 0, 0U, 0U);
    TEST_ASSERT_TRUE(replied(">|step=2 result=noverdict why=mute"));
    record_haptic(1U, HAPTIC_TEST_NOVERDICT, HAPTIC_TF_SHORT, 0U, 0, 0, 0U, 0U);
    TEST_ASSERT_TRUE(replied(">|step=1 result=noverdict why=short"));
    record_haptic(1U, HAPTIC_TEST_NOVERDICT, HAPTIC_TF_LOUD, 0U, 0, 0, 0U, 0U);
    TEST_ASSERT_TRUE(replied(">|step=1 result=noverdict why=loud"));

    verdict("haptic", false);
    TEST_ASSERT_TRUE(replied(">|test=haptic result=fail"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* The IMU could not arm a window, so the motor was never judged. That is
 * the unattended outcome, not a faulty motor, and it prints as a timeout. */
TEST_CASE("a haptic row with no witness is a timeout, not a fail",
          "[console_motion]")
{
    console_up();
    type_line("test component=haptic");
    TEST_ASSERT_TRUE(posted_haptic_row());

    record_haptic(1U, HAPTIC_TEST_FLOOR, HAPTIC_TF_PASS, 0U, 8, 0, 0U, 128U);
    record_haptic(2U, HAPTIC_TEST_NOVERDICT, HAPTIC_TF_MUTE, 0U, 0, 0, 0U, 0U);
    verdict("haptic", false);

    TEST_ASSERT_TRUE(replied(">|test=haptic result=timeout"));
    TEST_ASSERT_TRUE(replied(">|summary pass=0 fail=1 timeout=1"));
}

/* AO_Haptic publishes the window it is about to give up on BEFORE the
 * noverdict record that says why. So the measured records carry the giving-up
 * reason in their own flags, and printing those as `fail` would condemn a
 * board over a bench the very next line excuses. */
TEST_CASE("a measurement the row could not judge says so rather than failing",
          "[console_motion]")
{
    console_up();
    type_line("test component=haptic");
    TEST_ASSERT_TRUE(posted_haptic_row());

    record_haptic(1U, HAPTIC_TEST_FLOOR, HAPTIC_TF_SHORT, 0U, 4, 0, 0U, 3U);
    TEST_ASSERT_TRUE(replied(">|step=1 floor_235=4 n=3 result=short"));
    TEST_ASSERT_FALSE_MESSAGE(replied("result=fail"),
                              "a window too short to judge is not a verdict");

    fake_klog_reset();
    record_haptic(2U, HAPTIC_TEST_GAIN, HAPTIC_TF_MUTE, 0x40U, 4, 0, 0U, 0U);
    TEST_ASSERT_TRUE(replied(
        ">|step=2 gain=0x40 on_235=0 ratio=0.0 n=0 result=mute"));
    TEST_ASSERT_FALSE_MESSAGE(replied("result=fail"),
                              "no witness is not the motor's fault either");

    /* And a loud bench still reads as loud on the floor record, whichever
     * other flag is set with it. */
    fake_klog_reset();
    record_haptic(1U, HAPTIC_TEST_FLOOR,
                  (uint8_t)(HAPTIC_TF_LOUD | HAPTIC_TF_SHORT), 0U, 5000, 0,
                  0U, 3U);
    TEST_ASSERT_TRUE(replied(">|step=1 floor_235=5000 n=3 result=loud"));

    verdict("haptic", false);
    TEST_ASSERT_TRUE(replied(">|test=haptic result=fail"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("a record arriving with no row in flight prints nothing",
          "[console_motion]")
{
    console_up();
    record_imu(1U, IMU_TEST_WHOAMI, IMU_TF_PASS, 0x70, 0, 0, 0);
    record_haptic(1U, HAPTIC_TEST_FLOOR, HAPTIC_TF_PASS, 0U, 4, 0, 0U, 256U);
    TEST_ASSERT_FALSE_MESSAGE(replied(">|step="), "nobody asked for those rows");
}

/* ---- The place both rows hold in `all` ------------------------------------- */

/** @brief What the clocks row asks of the mocked clock block, once. */
static void expect_clocks_row(void)
{
    static uint32_t x32k;
    static uint32_t rc_slow;
    static uint32_t tree;

    x32k    = 32768000U;
    rc_slow = 121300000U;
    tree    = 400000000U;

    khal_clk_main_hz_ExpectAndReturn(40000000U);
    khal_clk_slow_src_ExpectAndReturn(KHAL_CLK_SLOW_XTAL32K);

    khal_clk_measure_ExpectAndReturn(KHAL_CLK_SRC_XTAL32K,
                                     KDRV_CLOCKS_CYCLES_FAST, NULL, KODE_OK);
    khal_clk_measure_IgnoreArg_milli_hz();
    khal_clk_measure_ReturnThruPtr_milli_hz(&x32k);

    khal_clk_measure_ExpectAndReturn(KHAL_CLK_SRC_RC_SLOW,
                                     KDRV_CLOCKS_CYCLES_FAST, NULL, KODE_OK);
    khal_clk_measure_IgnoreArg_milli_hz();
    khal_clk_measure_ReturnThruPtr_milli_hz(&rc_slow);

    for ( int i = 0; 4 > i; ++i )
    {
        khal_clk_tree_hz_ExpectAndReturn(KHAL_CLK_TREE_CPU, NULL, KODE_OK);
        khal_clk_tree_hz_IgnoreArg_clk();
        khal_clk_tree_hz_IgnoreArg_hz();
        khal_clk_tree_hz_ReturnThruPtr_hz(&tree);
    }
}

/** @brief Consume one request of any signal from @p ao. */
static bool posted_any(QActive *ao)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(ao->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    QF_gc(e);
    return true;
}

/* `main` is the main PCB's I2C chips and its crystals, and nothing else:
 * nothing that needs a panel, the antenna PCB, a card, the coprocessor or
 * somebody at the glass. The IMU comes right after the crystals and the
 * magnetometer right after it, the part beside it on the same bus. */
TEST_CASE("main runs the six rows of the main PCB and nothing else",
          "[console_motion]")
{
    console_up();
    Mockkhal_clk_Init();
    expect_clocks_row();

    type_line("test component=main");
    TEST_ASSERT_TRUE_MESSAGE(posted_any(AO_Power), "the PMIC is first");
    verdict("pmic", true);
    TEST_ASSERT_TRUE(posted_any(AO_Power));       /* the gauge */
    verdict("gauge", true);
    TEST_ASSERT_TRUE(posted_any(AO_IoExp));
    verdict("ioexp", true);
    TEST_ASSERT_TRUE_MESSAGE(replied(">|test=clocks result=pass"),
                             "the crystals answer on this thread, in their turn");
    TEST_ASSERT_TRUE_MESSAGE(posted_imu_row(), "the IMU, right after the crystals");
    verdict("imu", false);
    TEST_ASSERT_TRUE(replied(">|test=imu result=fail"));
    TEST_ASSERT_TRUE_MESSAGE(posted_mag_row(), "a failed row does not end the run");
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "closed before the magnetometer answered");

    verdict("mag", true);
    TEST_ASSERT_TRUE(replied(">|test=mag result=pass"));
    TEST_ASSERT_TRUE_MESSAGE(replied(">ok"), "the magnetometer is the last row");
    TEST_ASSERT_FALSE(posted_any(AO_Ir));
    TEST_ASSERT_FALSE(posted_haptic_row());
    TEST_ASSERT_FALSE(posted_any(AO_Storage));

    Mockkhal_clk_Verify();
    Mockkhal_clk_Destroy();
}
