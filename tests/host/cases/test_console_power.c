/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_power.c
 * @brief `pmic` and `gauge`: every field of both chips, by name, from the wire.
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
#include "kcli_proto.h"
#include "kaos_wire.h"
#include "kdrv_bq25896.h"
#include "kdrv_cw2217.h"
#include "kdrv_storage.h"
#include "kqp.h"
#include "kqp_config.h"
#include "qf_support.h"
#include "owners_support.h"

/* The command watchdog, in QP ticks. Repeated rather than imported for the
 * same reason test_console_flow.c repeats it.
 *
 * The second one is what expires a command left in flight by an earlier case,
 * and it is not the watchdog: a `test` row carries its OWN budget, and the
 * card's is the better part of a minute. Ticking five seconds' worth would
 * leave the console busy and every case below answering `busy`. */
#define CMD_TIMEOUT_TICKS  ((5000U * 100U) / 1000U)
#define ANY_BUDGET_TICKS   ((200000U * 100U) / 1000U)

#define POWER_FILL_MAX     64U   /**< Posts that surely fill AO_Power's queue. */

static void console_up(void)
{
    const QEvt *e;

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
    /* Whatever an earlier file left in flight - test_console_parse.c ends
     * with a command deliberately waiting - is timed out here, so every case
     * starts at an idle prompt. */
    (void)qf_support_tick(AO_Console, ANY_BUDGET_TICKS);
    /* AO_Power's queue is the mailbox the verbs post into; emptied here so a
     * request left by an earlier case cannot be mistaken for this one's.
     * AO_Buttons' too: `test` with no component asks it for the guided
     * button test. */
    qf_support_give_queue(AO_Power);
    while ( xQueueReceive(AO_Power->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
    qf_support_give_queue(AO_Buttons);
    while ( xQueueReceive(AO_Buttons->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
    /* AO_Storage's too: the card is a hardware row now, so a bare `test`
     * asks it as well. */
    qf_support_give_queue(AO_Storage);
    while ( xQueueReceive(AO_Storage->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
    /* And AO_Nfc's, for the same reason: `all` reaches the reader too, and a
     * post to an Active Object with no queue aborts inside FreeRTOS rather
     * than failing a case. */
    qf_support_give_queue(AO_Nfc);
    while ( xQueueReceive(AO_Nfc->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
    /* And AO_Rfid's, for the same reason: `all` reaches the LF front end
     * right after the reader it shares the Antenna PCB with. */
    qf_support_give_queue(AO_Rfid);
    while ( xQueueReceive(AO_Rfid->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
    /* And AO_Audio's, again for the same reason: `all` reaches the codec
     * between the reader and the operator's two rows. */
    qf_support_give_queue(AO_Audio);
    while ( xQueueReceive(AO_Audio->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
    /* And the two motion owners': `all` reaches the IMU right after the
     * crystals and the motor right after the infrared block. */
    qf_support_give_queue(AO_Imu);
    while ( xQueueReceive(AO_Imu->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
    qf_support_give_queue(AO_Haptic);
    while ( xQueueReceive(AO_Haptic->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
    /* And the owners of the two rows the operator answers on the pad: `all`
     * reaches them between the button test and the touch test. */
    qf_support_give_queue(AO_Display);
    while ( xQueueReceive(AO_Display->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
    qf_support_give_queue(AO_LedMgr);
    while ( xQueueReceive(AO_LedMgr->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
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
 * @brief Take the request the console posted to AO_Power, if any.
 * @param[out] out A copy of it.
 * @return false when nothing was posted.
 */
static bool posted(PowerFieldReqEvt *out)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Power->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(POWER_FIELD_REQ_SIG, e->sig);
    *out = *(const PowerFieldReqEvt *)e;
    QF_gc(e);
    return true;
}

/** @brief Deliver AO_Power's answer to the console. */
static void answer(uint8_t chip, uint8_t op, uint16_t field, kode_status_t rc,
                   int32_t value)
{
    PowerFieldEvt a;

    a.super = (QEvt)QEVT_INITIALIZER(POWER_FIELD_SIG);
    a.chip  = chip;
    a.op    = op;
    a.field = field;
    a.rc    = (uint8_t)rc;
    a.value = value;
    QASM_DISPATCH(AO_Console, &a.super, 0U);
}

/** @brief Take the control request the console posted to AO_Power, if any. */
static bool posted_ctrl(PowerCtrlReqEvt *out)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Power->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(POWER_CTRL_REQ_SIG, e->sig);
    *out = *(const PowerCtrlReqEvt *)e;
    QF_gc(e);
    return true;
}

/** @brief Deliver AO_Power's answer to a control request. */
static void answer_ctrl(uint8_t op, kode_status_t rc, int32_t value)
{
    PowerCtrlEvt a;

    a.super = (QEvt)QEVT_INITIALIZER(POWER_CTRL_SIG);
    a.op    = op;
    a.rc    = (uint8_t)rc;
    a.value = value;
    QASM_DISPATCH(AO_Console, &a.super, 0U);
}

/* ---- Dumps ----------------------------------------------------------------- */

TEST_CASE("pmic with no keys asks for the whole chip and closes on the answer",
          "[console_power]")
{
    PowerFieldReqEvt q;

    console_up();
    type_line("pmic");

    TEST_ASSERT_TRUE(posted(&q));
    TEST_ASSERT_EQUAL_UINT8(POWER_CHIP_PMIC, q.chip);
    TEST_ASSERT_EQUAL_UINT8(POWER_OP_DUMP, q.op);
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "closed before the owner answered");

    answer(POWER_CHIP_PMIC, POWER_OP_DUMP, 0U, KODE_OK, 0);
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* ---- Reading and writing a field ------------------------------------------- */

TEST_CASE("pmic field= asks the owner for that field and answers a record",
          "[console_power]")
{
    PowerFieldReqEvt q;

    console_up();
    type_line("pmic field=\"ichg_ma\"");

    TEST_ASSERT_TRUE(posted(&q));
    TEST_ASSERT_EQUAL_UINT8(POWER_CHIP_PMIC, q.chip);
    TEST_ASSERT_EQUAL_UINT8(POWER_OP_GET, q.op);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)KDRV_BQ_ICHG_MA, q.field);

    answer(POWER_CHIP_PMIC, POWER_OP_GET, (uint16_t)KDRV_BQ_ICHG_MA, KODE_OK, 2048);
    TEST_ASSERT_TRUE(replied(">|chip=pmic field=\"ichg_ma\" value=2048"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("pmic field= value= asks for a write and answers the readback",
          "[console_power]")
{
    PowerFieldReqEvt q;

    console_up();
    type_line("pmic field=\"ichg_ma\" value=500");

    TEST_ASSERT_TRUE(posted(&q));
    TEST_ASSERT_EQUAL_UINT8(POWER_OP_SET, q.op);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)KDRV_BQ_ICHG_MA, q.field);
    TEST_ASSERT_EQUAL_INT32(500, q.value);

    /* The chip only knows multiples of 64: the record says what it kept. */
    answer(POWER_CHIP_PMIC, POWER_OP_SET, (uint16_t)KDRV_BQ_ICHG_MA, KODE_OK, 512);
    TEST_ASSERT_TRUE(replied(">|chip=pmic field=\"ichg_ma\" value=512"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* ---- pmic_help / gauge_help ------------------------------------------------ */

TEST_CASE("pmic_help lists every field, paced, and closes with ok",
          "[console_power]")
{
    unsigned remaining;
    unsigned ticks_to_last_slice;

    console_up();
    type_line("pmic_help");

    /* The first field is in the first slice, emitted before any tick. */
    TEST_ASSERT_TRUE(replied(">|field=\"en_hiz\""));
    TEST_ASSERT_TRUE(replied("desc="));
    /* KDRV_BQ_FIELD_MAX is well past one KAOS_WIRE_SLICE and past the fake
     * klog sink's capture window: the closing `>ok` cannot be read back as
     * text once the whole table is captured from the start, but the sink
     * keeps counting past its cap, so completion is checked by count here. */
    TEST_ASSERT_EQUAL_UINT((unsigned)KAOS_WIRE_SLICE, fake_klog_count());

    for ( unsigned i = 0U; (unsigned)KDRV_BQ_FIELD_MAX > i; ++i )
    {
        (void)qf_support_tick(AO_Console, 1U);
    }
    TEST_ASSERT_EQUAL_UINT((unsigned)KDRV_BQ_FIELD_MAX + 1U, fake_klog_count());

    /* The count above proves the listing reached KDRV_BQ_FIELD_MAX lines,
     * but a count is what a truncated table would also produce if the last
     * entries were merely blank. Replay the run once more, resetting the
     * capture window just before the final slice, so the actual text of the
     * chip's last field lands inside the window instead of past it - a
     * table missing its tail fails this even though the count above would
     * not. */
    console_up();
    type_line("pmic_help");
    remaining = (unsigned)KDRV_BQ_FIELD_MAX - (unsigned)KAOS_WIRE_SLICE;
    ticks_to_last_slice =
        (remaining + (unsigned)KAOS_WIRE_SLICE - 1U) / (unsigned)KAOS_WIRE_SLICE;
    for ( unsigned i = 0U; (ticks_to_last_slice - 1U) > i; ++i )
    {
        (void)qf_support_tick(AO_Console, 1U);
    }
    fake_klog_reset();
    (void)qf_support_tick(AO_Console, 1U);
    TEST_ASSERT_TRUE(replied(">|field=\"dev_rev\""));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("gauge_help lists every field, paced, and closes with ok",
          "[console_power]")
{
    console_up();
    type_line("gauge_help");

    TEST_ASSERT_TRUE(replied(">|field=\"version\""));

    for ( unsigned i = 0U; (unsigned)KDRV_CW_FIELD_MAX > i; ++i )
    {
        (void)qf_support_tick(AO_Console, 1U);
    }
    TEST_ASSERT_TRUE(replied(">|field=\"fw_version\""));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("gauge reaches the gauge's fields by their own names",
          "[console_power]")
{
    PowerFieldReqEvt q;

    console_up();
    type_line("gauge field=\"vcell_mv\"");

    TEST_ASSERT_TRUE(posted(&q));
    TEST_ASSERT_EQUAL_UINT8(POWER_CHIP_GAUGE, q.chip);
    TEST_ASSERT_EQUAL_UINT8(POWER_OP_GET, q.op);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)KDRV_CW_VCELL_MV, q.field);

    answer(POWER_CHIP_GAUGE, POWER_OP_GET, (uint16_t)KDRV_CW_VCELL_MV, KODE_OK, 3720);
    TEST_ASSERT_TRUE(replied(">|chip=gauge field=\"vcell_mv\" value=3720"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* ---- Refusals -------------------------------------------------------------- */

TEST_CASE("value without field is a missing key", "[console_power]")
{
    PowerFieldReqEvt q;

    console_up();
    type_line("pmic value=5");
    TEST_ASSERT_TRUE(replied(">err code=2 msg=field"));
    TEST_ASSERT_FALSE(posted(&q));
}

/* A name the driver never printed is refused here: nothing is asked of the
 * owner, and the answer names the key rather than the verb. */
TEST_CASE("a field nobody printed is refused before the owner is asked",
          "[console_power]")
{
    PowerFieldReqEvt q;

    console_up();
    type_line("pmic field=\"ichg\"");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=field"));
    TEST_ASSERT_FALSE(posted(&q));

    fake_klog_reset();
    type_line("gauge field=\"batv_mv\"");     /* the PMIC's, not the gauge's */
    TEST_ASSERT_TRUE(replied(">err code=3 msg=field"));
    TEST_ASSERT_FALSE(posted(&q));
}

/* The owner's verdicts, on the wire: a value the chip cannot hold is a bad
 * value naming the field; a chip that did not answer is a rejection. */
TEST_CASE("a refusal from the owner names the field, a failure names the verb",
          "[console_power]")
{
    PowerFieldReqEvt q;

    console_up();
    type_line("pmic field=\"batfet_dis\" value=1");
    TEST_ASSERT_TRUE(posted(&q));
    answer(POWER_CHIP_PMIC, POWER_OP_SET, (uint16_t)KDRV_BQ_BATFET_DIS, KODE_DENIED, 1);
    TEST_ASSERT_TRUE(replied(">err code=3 msg=batfet_dis"));

    fake_klog_reset();
    type_line("gauge field=\"soh_pct\"");
    TEST_ASSERT_TRUE(posted(&q));
    answer(POWER_CHIP_GAUGE, POWER_OP_GET, (uint16_t)KDRV_CW_SOH_PCT, KODE_ERROR, 0);
    TEST_ASSERT_TRUE(replied(">err code=5 msg=gauge"));
}

/* ---- Self-tests ------------------------------------------------------------ */

/** @brief Take the self-test request the console posted to AO_Power, if any. */
static bool posted_test(uint8_t *chip)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Power->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(POWER_TEST_REQ_SIG, e->sig);
    *chip = ((const PowerTestReqEvt *)e)->chip;
    QF_gc(e);
    return true;
}

/** @brief Was the expander's self-test asked of AO_IoExp? The third hardware row. */
static bool posted_ioexp_test(void)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_IoExp->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(IOEXP_TEST_REQ_SIG, e->sig);
    QF_gc(e);
    return true;
}

/** @brief Was the card's row asked of AO_Storage? The longest automatic one. */
static bool posted_sd_test(void)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Storage->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(STORAGE_CMD_SIG, e->sig);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)STORAGE_OP_SELFTEST,
                            ((const StorageCmdEvt *)e)->op);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_MEDIUM_SD,
                            ((const StorageCmdEvt *)e)->medium);
    QF_gc(e);
    return true;
}

/** @brief Was `test component=imu` asked of AO_Imu? */
static bool posted_imu_test(void)
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

/** @brief Was `test component=mag` asked of AO_Imu? */
static bool posted_mag_test(void)
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

/** @brief Was `test component=haptic` asked of AO_Haptic? */
static bool posted_haptic_test(void)
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

/** @brief Was `test component=ir` asked of AO_Ir? */
static bool posted_ir_test(void)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Ir->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(IR_TEST_REQ_SIG, e->sig);
    QF_gc(e);
    return true;
}

/** @brief Was `test component=c5` asked of AO_Radio? */
static bool posted_c5_test(void)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Radio->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(RADIO_TEST_REQ_SIG, e->sig);
    QF_gc(e);
    return true;
}

/**
 * @brief Did the console ask AO_Nfc to run its row?
 *
 * @return true when NFC_TEST_REQ_SIG was waiting, which consumes it.
 */
static bool posted_nfc_test(void)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Nfc->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(NFC_TEST_REQ_SIG, e->sig);
    QF_gc(e);
    return true;
}

/**
 * @brief Did the console ask AO_Rfid to run its row?
 *
 * @return true when RFID_TEST_REQ_SIG was waiting, which consumes it.
 */
static bool posted_rfid_test(void)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Rfid->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(RFID_TEST_REQ_SIG, e->sig);
    QF_gc(e);
    return true;
}

/**
 * @brief Did the console ask AO_Audio to run its row?
 *
 * @return true when AUDIO_TEST_REQ_SIG was waiting, which consumes it.
 */
static bool posted_audio_test(void)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Audio->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(AUDIO_TEST_REQ_SIG, e->sig);
    QF_gc(e);
    return true;
}

/** @brief Was the panel's row asked of AO_Display? */
static bool posted_display_test(void)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Display->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(DISPLAY_TEST_REQ_SIG, e->sig);
    QF_gc(e);
    return true;
}

/** @brief Was the LED's row asked of AO_LedMgr? */
static bool posted_led_test(void)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_LedMgr->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(LED_TEST_REQ_SIG, e->sig);
    QF_gc(e);
    return true;
}

/** @brief Was the guided button test asked of AO_Buttons? The operator's row. */
static bool posted_buttons_test(void)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Buttons->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(BUTTONS_TEST_REQ_SIG, e->sig);
    QF_gc(e);
    return true;
}

/** @brief Was the guided touch test asked of AO_Touch? The operator's other row. */
static bool posted_touch_test(void)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Touch->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(TOUCH_TEST_REQ_SIG, e->sig);
    QF_gc(e);
    return true;
}

/** @brief Deliver a self-test verdict to the sequencer and the console. */
static void verdict(const char *name, bool ok)
{
    TestResultEvt r;

    r.super = (QEvt)QEVT_INITIALIZER(TEST_RESULT_SIG);
    (void)snprintf(r.name, sizeof(r.name), "%s", name);
    r.ok = ok;
    r.origin = TEST_ORIGIN_AUTOTEST;
    QASM_DISPATCH(AO_Autotest, &r.super, 0U);
    QASM_DISPATCH(AO_Console, &r.super, 0U);
    autotest_settle();
}

/* The chips are AO_Power's, so the console asks and the tally waits. */
TEST_CASE("test component=pmic and =gauge ask AO_Power and close on its verdict",
          "[console_power]")
{
    uint8_t chip = 0xFFU;

    console_up();
    type_line("test component=pmic");
    TEST_ASSERT_TRUE(posted_test(&chip));
    TEST_ASSERT_EQUAL_UINT8(POWER_CHIP_PMIC, chip);
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "closed before AO_Power answered");
    verdict("pmic", true);
    TEST_ASSERT_TRUE(replied(">|test=pmic result=pass"));
    TEST_ASSERT_TRUE(replied(">ok"));

    fake_klog_reset();
    type_line("test component=gauge");
    TEST_ASSERT_TRUE(posted_test(&chip));
    TEST_ASSERT_EQUAL_UINT8(POWER_CHIP_GAUGE, chip);
    verdict("gauge", false);
    TEST_ASSERT_TRUE(replied(">|test=gauge result=fail"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/**
 * @brief Arrange one pass of the clocks row.
 *
 * `all` now includes a hardware row that belongs to no Active Object - the
 * two crystals, which nothing contends for - so it is judged on this thread
 * and needs its seam standing by. The numbers are a healthy board's; what
 * this file cares about is that the row does not disturb the four rows that
 * do have owners. test_console_clocks.c is where the row itself is judged.
 */
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

/* `main` walks the main PCB's rows one at a time in the table's order: two
 * rows at once would be two owners judging one bus. `all` and a bare `test`
 * are the E5 station's final test, which has cases of its own; a software
 * test answering inside `main` would be a pass nobody asked for, so the case
 * checks what was posted and what printed, and when. */
TEST_CASE("test component=main runs the main PCB's rows, one at a time",
          "[console_power]")
{
    uint8_t chip = 0xFFU;

    console_up();
    Mockkhal_clk_Init();
    expect_clocks_row();
    type_line("test component=main");
    TEST_ASSERT_TRUE(posted_test(&chip));
    TEST_ASSERT_EQUAL_UINT8(POWER_CHIP_PMIC, chip);
    TEST_ASSERT_FALSE_MESSAGE(posted_test(&chip), "the gauge waits for the PMIC's verdict");
    TEST_ASSERT_FALSE(posted_ioexp_test());
    TEST_ASSERT_FALSE_MESSAGE(replied(">|test=clocks"), "the crystals wait their turn too");
    TEST_ASSERT_FALSE(replied(">ok"));

    verdict("pmic", true);
    TEST_ASSERT_TRUE(replied(">|test=pmic result=pass"));
    TEST_ASSERT_TRUE_MESSAGE(posted_test(&chip), "the verdict starts the next row");
    TEST_ASSERT_EQUAL_UINT8(POWER_CHIP_GAUGE, chip);
    verdict("gauge", false);
    TEST_ASSERT_TRUE(replied(">|test=gauge result=fail"));
    TEST_ASSERT_TRUE_MESSAGE(posted_ioexp_test(), "a failed row does not end the run");
    verdict("ioexp", true);
    TEST_ASSERT_TRUE(replied(">|test=ioexp result=pass"));
    /* The row with no owner is judged on this thread the moment its turn
     * comes. */
    TEST_ASSERT_TRUE_MESSAGE(replied(">|test=clocks result=pass"),
                             "the crystals answer on this thread, in their turn");
    TEST_ASSERT_TRUE_MESSAGE(posted_imu_test(), "the IMU, right after the crystals");
    TEST_ASSERT_FALSE_MESSAGE(posted_mag_test(),
                              "the magnetometer waits for the IMU's verdict");
    verdict("imu", true);
    TEST_ASSERT_TRUE(replied(">|test=imu result=pass"));
    TEST_ASSERT_TRUE(posted_mag_test());
    TEST_ASSERT_FALSE(replied(">ok"));
    verdict("mag", true);
    TEST_ASSERT_TRUE(replied(">|test=mag result=pass"));
    TEST_ASSERT_TRUE_MESSAGE(replied(">ok"), "the magnetometer is the last row");

    TEST_ASSERT_FALSE(posted_haptic_test());
    TEST_ASSERT_FALSE(posted_sd_test());
    TEST_ASSERT_FALSE(posted_c5_test());
    TEST_ASSERT_FALSE(posted_ir_test());
    TEST_ASSERT_FALSE(posted_nfc_test());
    TEST_ASSERT_FALSE(posted_rfid_test());
    TEST_ASSERT_FALSE(posted_audio_test());
    TEST_ASSERT_FALSE(posted_buttons_test());
    TEST_ASSERT_FALSE(posted_led_test());
    TEST_ASSERT_FALSE(posted_display_test());
    TEST_ASSERT_FALSE(posted_touch_test());
    TEST_ASSERT_FALSE_MESSAGE(replied(">|test=boot"), "no software test ran under main");
    TEST_ASSERT_FALSE_MESSAGE(replied(">|test=mem"), "no row outside main ran");

    Mockkhal_clk_Verify();
    Mockkhal_clk_Destroy();
}

/* Every row has a budget, and a row that does not answer inside it is a
 * timeout: one record, and the run goes on to the next row rather than
 * dying with the tally of every other row unprinted. A verdict that turns
 * up after its row was given up on belongs to nobody. */
TEST_CASE("a row that never answers is a timeout, the run goes on, and its late verdict is ignored",
          "[console_power]")
{
    uint8_t chip = 0xFFU;

    console_up();
    Mockkhal_clk_Init();
    expect_clocks_row();
    type_line("test component=main");
    TEST_ASSERT_TRUE(posted_test(&chip));
    TEST_ASSERT_EQUAL_UINT8(POWER_CHIP_PMIC, chip);

    (void)qf_support_tick(AO_Console, CMD_TIMEOUT_TICKS - 1U);
    TEST_ASSERT_FALSE_MESSAGE(replied(">|test=pmic"), "not yet: the row has its five seconds");
    (void)qf_support_tick(AO_Console, 2U);
    TEST_ASSERT_TRUE(replied(">|test=pmic result=timeout"));
    TEST_ASSERT_FALSE_MESSAGE(replied(">err code=6"), "a row's silence is a record, not the end of the run");
    TEST_ASSERT_TRUE_MESSAGE(posted_test(&chip), "the next row starts");
    TEST_ASSERT_EQUAL_UINT8(POWER_CHIP_GAUGE, chip);

    verdict("pmic", true);   /* late, and nobody's */
    TEST_ASSERT_FALSE(replied(">|test=pmic result=pass"));
    TEST_ASSERT_FALSE_MESSAGE(posted_ioexp_test(), "a stranger's verdict does not advance the run");

    verdict("gauge", true);
    TEST_ASSERT_TRUE(posted_ioexp_test());
    verdict("ioexp", true);
    TEST_ASSERT_TRUE(replied(">|test=clocks result=pass"));
    TEST_ASSERT_TRUE(posted_imu_test());
    verdict("imu", true);
    TEST_ASSERT_TRUE(posted_mag_test());
    verdict("mag", true);
    TEST_ASSERT_TRUE(replied(">ok"));

    Mockkhal_clk_Verify();
    Mockkhal_clk_Destroy();
}

/* ---- The two rows the operator answers on the pad ---------------------------- */

/** @brief One record of the LED row, as AO_LedMgr publishes it. */
static void led_record(uint8_t colour, uint8_t phase)
{
    LedTestEvt r;

    (void)memset(&r, 0, sizeof(r));
    r.super  = (QEvt)QEVT_INITIALIZER(LED_TEST_SIG);
    r.step   = (uint8_t)(colour + 1U);
    r.total  = (uint8_t)LED_COLOUR_COUNT;
    r.phase  = phase;
    r.colour = colour;
    QASM_DISPATCH(AO_Autotest, &r.super, 0U);
    QASM_DISPATCH(AO_Console, &r.super, 0U);
}

/** @brief One record of the display row, as AO_Display publishes it. */
static void display_record(uint8_t field, uint8_t phase)
{
    DisplayTestEvt r;

    (void)memset(&r, 0, sizeof(r));
    r.super = (QEvt)QEVT_INITIALIZER(DISPLAY_TEST_SIG);
    r.step  = (uint8_t)(field + 1U);
    r.total = (uint8_t)DISPLAY_FIELD_COUNT;
    r.phase = phase;
    r.field = field;
    QASM_DISPATCH(AO_Autotest, &r.super, 0U);
    QASM_DISPATCH(AO_Console, &r.super, 0U);
}

/* The guided button test's grammar: a question, then how it was answered,
 * under one step number. The question names what to look at and which button
 * means what, because the station's script prints it to somebody who has
 * never read the protocol. */
TEST_CASE("test component=led prints each question and how it was answered",
          "[console_power]")
{
    console_up();
    type_line("test component=led");
    TEST_ASSERT_TRUE(posted_led_test());

    led_record((uint8_t)LED_COLOUR_RED, (uint8_t)LED_TEST_ASK);
    TEST_ASSERT_TRUE(replied(">|step=1 of=3 look=red yes=top no=bot"));
    led_record((uint8_t)LED_COLOUR_RED, (uint8_t)LED_TEST_PASS);
    TEST_ASSERT_TRUE(replied(">|step=1 colour=red result=pass"));
    led_record((uint8_t)LED_COLOUR_GREEN, (uint8_t)LED_TEST_ASK);
    TEST_ASSERT_TRUE(replied(">|step=2 of=3 look=green yes=top no=bot"));
    led_record((uint8_t)LED_COLOUR_GREEN, (uint8_t)LED_TEST_FAIL);
    TEST_ASSERT_TRUE(replied(">|step=2 colour=green result=fail"));
    led_record((uint8_t)LED_COLOUR_BLUE, (uint8_t)LED_TEST_TIMEOUT);
    TEST_ASSERT_TRUE(replied(">|step=3 colour=blue result=timeout"));

    fake_klog_reset();
    led_record((uint8_t)LED_COLOUR_BLUE, (uint8_t)LED_TEST_DONE);
    TEST_ASSERT_FALSE_MESSAGE(replied(">|step="), "the closing record is not a line");
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "closed before the verdict");
    verdict("led", false);
    TEST_ASSERT_TRUE(replied(">|test=led result=fail"));
    TEST_ASSERT_TRUE(replied(">ok"));

    /* And with nothing in flight a record is nobody's: it prints nothing. */
    fake_klog_reset();
    led_record((uint8_t)LED_COLOUR_RED, (uint8_t)LED_TEST_ASK);
    TEST_ASSERT_FALSE(replied(">|step="));
}

TEST_CASE("test component=display prints each question, its answer, and a board with no panel",
          "[console_power]")
{
    console_up();
    type_line("test component=display");
    TEST_ASSERT_TRUE(posted_display_test());

    display_record((uint8_t)DISPLAY_FIELD_WHITE, (uint8_t)DISPLAY_TEST_ASK);
    TEST_ASSERT_TRUE(replied(">|step=1 of=5 look=white yes=top no=bot"));
    display_record((uint8_t)DISPLAY_FIELD_WHITE, (uint8_t)DISPLAY_TEST_PASS);
    TEST_ASSERT_TRUE(replied(">|step=1 field=white result=pass"));
    display_record((uint8_t)DISPLAY_FIELD_RED, (uint8_t)DISPLAY_TEST_FAIL);
    TEST_ASSERT_TRUE(replied(">|step=2 field=red result=fail"));
    display_record((uint8_t)DISPLAY_FIELD_BLACK, (uint8_t)DISPLAY_TEST_TIMEOUT);
    TEST_ASSERT_TRUE(replied(">|step=5 field=black result=timeout"));
    display_record((uint8_t)DISPLAY_FIELD_BLACK, (uint8_t)DISPLAY_TEST_DONE);
    verdict("display", false);
    TEST_ASSERT_TRUE(replied(">|test=display result=fail"));
    TEST_ASSERT_TRUE(replied(">ok"));

    /* A main PCB with no glass: one record, no question, a failed verdict. */
    fake_klog_reset();
    type_line("test component=display");
    TEST_ASSERT_TRUE(posted_display_test());
    display_record((uint8_t)DISPLAY_FIELD_WHITE, (uint8_t)DISPLAY_TEST_ABSENT);
    TEST_ASSERT_TRUE(replied(">|step=1 panel=absent result=absent"));
    TEST_ASSERT_FALSE(replied("look="));
    display_record((uint8_t)DISPLAY_FIELD_WHITE, (uint8_t)DISPLAY_TEST_DONE);
    verdict("display", false);
    TEST_ASSERT_TRUE(replied(">|test=display result=fail"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* ---- The reading (bat_info) ------------------------------------------------ */

/** @brief Deliver a reading to the console. */
static void reading(bool boost)
{
    PowerEvt p;

    (void)memset(&p, 0, sizeof(p));
    p.super    = (QEvt)QEVT_INITIALIZER(POWER_EVT_SIG);
    p.vbat_mv  = 3720U;
    p.vbus_mv  = 5000U;
    p.soc_pct  = 42U;
    p.boost    = boost;
    QASM_DISPATCH(AO_Console, &p.super, 0U);
}

TEST_CASE("bat_info's record says whether the chip is boosting", "[console_power]")
{
    console_up();
    type_line("bat_info");
    reading(true);
    TEST_ASSERT_TRUE(replied("vbus=off boost=on"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* ---- The owner's operations ------------------------------------------------ */

TEST_CASE("boost state=on asks the owner and answers its record", "[console_power]")
{
    PowerCtrlReqEvt q;

    console_up();
    type_line("boost state=on");
    TEST_ASSERT_TRUE(posted_ctrl(&q));
    TEST_ASSERT_EQUAL_UINT8(POWER_CTRL_BOOST, q.op);
    TEST_ASSERT_EQUAL_INT32(1, q.value);
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "closed before the owner answered");

    answer_ctrl(POWER_CTRL_BOOST, KODE_OK, 1);
    TEST_ASSERT_TRUE(replied(">|op=boost boost=on"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* Charging is a stored setting now, `config key="charge"` and
 * `config key="charge_ma"`, which AO_Power answers itself: the two verbs that
 * did it unstored are gone, not kept as a second door. */
TEST_CASE("charge and charge_limit are not verbs: config owns charging",
          "[console_power]")
{
    PowerCtrlReqEvt q;

    console_up();
    type_line("charge state=off");
    TEST_ASSERT_TRUE(replied(">err code=1 msg=charge"));
    TEST_ASSERT_FALSE(posted_ctrl(&q));

    fake_klog_reset();
    type_line("charge_limit ma=500");
    TEST_ASSERT_TRUE(replied(">err code=1 msg=charge_limit"));
    TEST_ASSERT_FALSE(posted_ctrl(&q));
}

TEST_CASE("pmic_reset closes on the owner's ok with no record", "[console_power]")
{
    PowerCtrlReqEvt q;

    console_up();
    type_line("pmic_reset");
    TEST_ASSERT_TRUE(posted_ctrl(&q));
    TEST_ASSERT_EQUAL_UINT8(POWER_CTRL_RESET, q.op);
    answer_ctrl(POWER_CTRL_RESET, KODE_OK, 0);
    TEST_ASSERT_FALSE(replied(">|op="));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* Four verdicts, three codes the protocol already has: a refusal names the
 * verb, busy is busy, and a chip that did not do it is a rejection. */
TEST_CASE("a control answer's rc maps onto the protocol's codes", "[console_power]")
{
    PowerCtrlReqEvt q;

    console_up();
    type_line("boost state=on");
    TEST_ASSERT_TRUE(posted_ctrl(&q));
    answer_ctrl(POWER_CTRL_BOOST, KODE_DENIED, 0);
    TEST_ASSERT_TRUE(replied(">err code=3 msg=boost"));

    fake_klog_reset();
    type_line("boost state=off");
    TEST_ASSERT_TRUE(posted_ctrl(&q));
    answer_ctrl(POWER_CTRL_BOOST, KODE_NO_RESOURCES, 0);
    TEST_ASSERT_TRUE(replied(">err code=4 msg=busy"));

    fake_klog_reset();
    type_line("pmic_reset");
    TEST_ASSERT_TRUE(posted_ctrl(&q));
    answer_ctrl(POWER_CTRL_RESET, KODE_TIMEOUT, 0);
    TEST_ASSERT_TRUE(replied(">err code=5 msg=pmic_reset"));
}

/* Every answer is published, so a verb in flight sees the answers to
 * requests other subscribers made. An app calling kode_sys_power_charge() during
 * a `pmic_reset`'s wait must not close the reset with somebody else's
 * verdict: the op is what says whose answer it is. */
TEST_CASE("a verb in flight ignores an answer for another op and closes on its own",
          "[console_power]")
{
    PowerCtrlReqEvt q;

    console_up();
    type_line("pmic_reset");
    TEST_ASSERT_TRUE(posted_ctrl(&q));
    TEST_ASSERT_EQUAL_UINT8(POWER_CTRL_RESET, q.op);

    answer_ctrl(POWER_CTRL_CHARGE, KODE_NO_RESOURCES, 0);
    TEST_ASSERT_FALSE_MESSAGE(replied(">err"),
        "somebody else's answer closed the verb in flight");
    TEST_ASSERT_FALSE(replied(">ok"));

    answer_ctrl(POWER_CTRL_RESET, KODE_OK, 0);
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("idle is not a verb", "[console_power]")
{
    console_up();
    type_line("idle s=300");
    TEST_ASSERT_TRUE(replied(">err code=1 msg=idle"));
}

/* The ship door posts with a margin, so a queue within it is the same refusal
 * as a dry pool: `>ok` would promise a switch-off nobody will do. `sleep` is
 * AO_LowPower's now (test_console_lowpower.c); ship mode is `ship_mode`. */
TEST_CASE("ship_mode posts the request and answers ok", "[console_power]")
{
    const QEvt *e = NULL;

    console_up();
    type_line("ship_mode");
    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_TRUE(xQueueReceive(AO_Power->eQueue, &e, 0U) == pdTRUE);
    TEST_ASSERT_EQUAL_INT(POWER_SHIP_REQ_SIG, e->sig);
    QF_gc(e);
}

TEST_CASE("ship_mode on a power queue within the margin is refused, not promised", "[console_power]")
{
    static const QEvt filler = QEVT_INITIALIZER(POWER_DUMP_REQ_SIG);
    const QEvt *e = NULL;

    console_up();
    for ( uint32_t guard = 0U;
         (POWER_FILL_MAX > guard)
         && (uxQueueSpacesAvailable(AO_Power->eQueue) > KQP_POWER_POST_MARGIN);
         ++guard )
    {
        QACTIVE_POST(AO_Power, &filler, NULL);
    }
    fake_klog_reset();
    type_line("ship_mode");
    TEST_ASSERT_TRUE(replied(">err code=5 msg=ship_mode"));
    TEST_ASSERT_FALSE(replied(">ok"));
    TEST_ASSERT_EQUAL_UINT(KQP_POWER_POST_MARGIN, uxQueueSpacesAvailable(AO_Power->eQueue));
    while ( xQueueReceive(AO_Power->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}
