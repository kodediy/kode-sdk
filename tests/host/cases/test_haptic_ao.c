/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_haptic_ao.c
 * @brief AO_Haptic: the owner of the AW86233, over a register file.
 */
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "qpc.h"
#include "Mockkhal_i2c.h"

#include "ao_spy.h"
#include "config_support.h"
#include "fakes.h"
#include "kaos.h"
#include "kbsp.h"
#include "kbsp_owners.h"
#include "kdrv_aw86233.h"
#include "khal_nvs.h"
#include "khal_time.h"
#include "kode_log.h"
#include "kode_sys.h"
#include "kqp.h"
#include "kset.h"
#include "lowpower_cases.h"
#include "qf_support.h"

#define REG_RSTCFG   0x00U
#define REG_SYSST    0x01U
#define REG_PLAYCFG2 0x07U
#define REG_PLAYCFG3 0x08U
#define REG_PLAYCFG4 0x09U
#define REG_RTPDATA  0x32U
#define REG_GLBRD5   0x3FU
#define REG_SYSCTRL2 0x46U
#define REG_CHIPID_H 0x57U
#define REG_CHIPID_L 0x69U

#define AW_GO        0x01U
#define AW_STOP      0x02U
#define AW_FF_AFS    0x08U

/** @brief The buzz every case that is not the row asks for. */
#define BUZZ_MS      200U
/** @brief What BUZZ_MS costs in samples, and so in bytes on RTPDATA. */
#define BUZZ_SAMPLES (KDRV_AW_SAMPLES(BUZZ_MS))
/** @brief What one tick of the feed offers: two bursts of 64. */
#define TICK_BYTES   (KDRV_AW_BURST * KODE_HAPTIC_BURSTS_PER_TICK)

/** @brief How much of the RTPDATA stream a case can read back, in bytes. */
#define FIFO_LOG_MAX 1024U

/** @brief The still window's energy every row case starts from, in 0.1 mg. */
#define FLOOR_X10    10
/** @brief Samples a window the detector is happy with holds. */
#define WINDOW_N     256U

/* ---- The chip, as a register file ---------------------------------------- */

static uint8_t  s_regs[256];
static bool     s_acks;
static size_t   s_fifo_bytes;   /* everything that ever landed on RTPDATA */
static int8_t   s_fifo_data[FIFO_LOG_MAX];   /* and the first of it, byte for byte */
static unsigned s_bursts;       /* how many writes it took                */
static bool     s_burst_fail;   /* the chip stops taking bursts           */
static bool     s_afs_hold;     /* the FIFO never drains again            */
static bool     s_afs_throttle; /* it drains just fast enough for one burst
                                 * between one status read and the next    */
static bool     s_afs_full;     /* and right now it is holding that burst  */
static uint8_t  s_kill_read;    /* the register whose write is the last thing
                                 * the bus ever does; 0 for a live bus     */

static kode_status_t on_read(khal_i2c_dev_t dev, uint8_t reg, uint8_t *buf,
                             size_t len, int calls)
{
    (void)dev;
    (void)calls;
    if ( !s_acks )
    {
        return KODE_ERROR;
    }
    (void)memcpy(buf, &s_regs[reg], len);
    if ( REG_SYSST == reg )
    {
        /* The one register this file drives rather than stores: the almost-
         * full flag is what the feed is paced by, so staging it is how a case
         * says how fast the chip is draining. */
        if ( s_afs_hold )
        {
            buf[0] |= AW_FF_AFS;
        }
        else if ( s_afs_full )
        {
            /* Full of the burst it was just given, and drained by the time
             * the next read comes round. Self-synchronising rather than
             * counted, so a case that starts mid-sequence still gets exactly
             * one burst accepted between two reads. */
            buf[0]     |= AW_FF_AFS;
            s_afs_full  = false;
        }
        else
        {
            /* Room, and the driver takes it. */
        }
    }
    return KODE_OK;
}

static kode_status_t on_write(khal_i2c_dev_t dev, uint8_t reg, uint8_t val,
                              int calls)
{
    (void)dev;
    (void)calls;
    if ( !s_acks )
    {
        return KODE_ERROR;
    }
    if ( (REG_RSTCFG == reg) && (0xAAU == val) )
    {
        return KODE_OK;   /* the soft reset does not land in a register */
    }
    s_regs[reg] = val;
    /* The chip's own state machine, as far as the driver reads it: GO puts
     * it in RTP and STOP back in standby, which is what rtp_begin() checks
     * before the first sample. */
    if ( REG_PLAYCFG4 == reg )
    {
        if ( (val & AW_GO) != 0U )
        {
            s_regs[REG_GLBRD5] = 0x08U;
        }
        if ( (val & AW_STOP) != 0U )
        {
            s_regs[REG_GLBRD5] = 0x00U;
        }
    }
    /* A bus that takes one more write and then stops answering. There is no
     * other way to stage it: a read-back happens inside the same AO step as
     * the write it follows, so a case cannot get between them. */
    if ( (0U != s_kill_read) && (reg == s_kill_read) )
    {
        s_acks = false;
    }
    return KODE_OK;
}

static kode_status_t on_write_regs(khal_i2c_dev_t dev, uint8_t reg,
                                   const uint8_t *buf, size_t len, int calls)
{
    (void)dev;
    (void)calls;
    if ( !s_acks || s_burst_fail )
    {
        return KODE_ERROR;
    }
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(REG_RTPDATA, reg,
                                   "a burst went somewhere other than the FIFO");
    TEST_ASSERT_TRUE_MESSAGE((size_t)KDRV_AW_BURST >= len,
                             "a burst was longer than the chip's own limit");
    for ( size_t i = 0U; i < len; ++i )
    {
        if ( (s_fifo_bytes + i) < (size_t)FIFO_LOG_MAX )
        {
            s_fifo_data[s_fifo_bytes + i] = (int8_t)buf[i];
        }
    }
    s_fifo_bytes += len;
    s_bursts++;
    if ( s_afs_throttle )
    {
        s_afs_full = true;
    }
    return KODE_OK;
}

/** @brief A chip that answers, or a board whose Antenna PCB is not fitted. */
static void bus_up(bool present)
{
    Mockkhal_i2c_Init();
    khal_i2c_init_IgnoreAndReturn(KODE_OK);
    khal_i2c_probe_IgnoreAndReturn(present);
    khal_i2c_add_IgnoreAndReturn(KODE_OK);
    khal_i2c_read_reg_StubWithCallback(&on_read);
    khal_i2c_write_reg_StubWithCallback(&on_write);
    khal_i2c_write_regs_StubWithCallback(&on_write_regs);

    (void)memset(s_regs, 0, sizeof(s_regs));
    s_regs[REG_CHIPID_H] = 0x23U;   /* an AW86233: 0x2330 */
    s_regs[REG_CHIPID_L] = 0x30U;
    s_acks        = present;
    s_fifo_bytes  = 0U;
    (void)memset(s_fifo_data, 0, sizeof(s_fifo_data));
    s_bursts      = 0U;
    s_burst_fail   = false;
    s_afs_hold     = false;
    s_afs_throttle = false;
    s_afs_full     = false;
    s_kill_read    = 0U;
}

/* ---- Bringing the AO up --------------------------------------------------- */

/** @brief The event last taken out of AO_Imu's queue, held until the next. */
static const QEvt *s_imu_held;

/** @brief Drop whatever the row left in AO_Imu's mailbox. */
static void imu_mailbox_clear(void)
{
    const QEvt *e = NULL;

    if ( NULL != s_imu_held )
    {
        QF_gc(s_imu_held);
        s_imu_held = NULL;
    }
    while ( (NULL != AO_Imu->eQueue)
           && (xQueueReceive(AO_Imu->eQueue, &e, 0U) == pdTRUE) )
    {
        QF_gc(e);
    }
}

/**
 * @brief Register AO_Haptic once, and construct it afresh for every case.
 *
 * test_imu_ao.c's arrangement and for its reasons. AO_Imu gets a queue and
 * nothing else: this file never dispatches into it, it only reads what the
 * row posted there.
 */
static void haptic_up(void)
{
    const QEvt *stray = NULL;

    qf_support_init();

    /* Before anything is constructed: QP takes a DISARMED QTimeEvt out of its
     * list only inside the tick, so a case that ended on a disarm leaves
     * three of them still linked - and QTimeEvt_ctorX() on a linked one
     * rewrites the link QF walks, after which the next tick goes round
     * forever. These ticks are what empty the list first. Enough of them to
     * expire whatever a case that failed halfway left armed, too, so a broken
     * assertion is reported as a failure rather than as a hung suite. */
    (void)qf_support_tick(AO_Haptic, QF_SUPPORT_STEPS(KODE_HAPTIC_RETRY_MS) + 64U);
    (void)ao_spy();
    ao_spy_subscribe(HAPTIC_DONE_SIG);
    ao_spy_subscribe(HAPTIC_TEST_SIG);
    ao_spy_subscribe(HAPTIC_FIELD_SIG);
    ao_spy_subscribe(TEST_RESULT_SIG);

    Haptic_ctor();
    qf_support_give_queue(AO_Haptic);
    qf_support_give_queue(AO_Imu);
    AO_Haptic->prio  = KODE_PRIO_HAPTIC;
    AO_Haptic->pthre = KODE_PRIO_HAPTIC;
    if ( qf_support_claim_ao(AO_Haptic) )
    {
        QActive_register_(AO_Haptic);
    }
    while ( xQueueReceive(AO_Haptic->eQueue, &stray, 0U) == pdTRUE )
    {
        QF_gc(stray);
    }
    imu_mailbox_clear();
    fake_klog_reset();
    QASM_INIT(AO_Haptic, (void *)0, 0U);
    ao_spy_drain();
}

/**
 * @brief Give every event this case is still holding back to the pool.
 *
 * The harness has one pool of thirty-two blocks for the whole binary, and a
 * reference held past the last case in a file is a block no later file can
 * allocate - which shows up as a publish that silently did not happen,
 * hundreds of cases away. This file holds two kinds: what the spy took, and
 * what was read out of the IMU's mailbox.
 */
static void haptic_down(void)
{
    ao_spy_drain();
    imu_mailbox_clear();
}

/** @brief Take the next event the spy holds and insist it is @p sig. */
static const QEvt *spy_expect(enum_t sig)
{
    const QEvt *e = ao_spy_take();

    TEST_ASSERT_NOT_NULL_MESSAGE(e, "the AO said nothing at all");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)sig, (int)e->sig, "the wrong answer");
    return e;
}

/** @brief Take the next request the row posted to AO_Imu, and insist on @p sig. */
static const QEvt *imu_expect(enum_t sig)
{
    const QEvt *e = NULL;

    if ( NULL != s_imu_held )
    {
        QF_gc(s_imu_held);
        s_imu_held = NULL;
    }
    TEST_ASSERT_TRUE_MESSAGE(xQueueReceive(AO_Imu->eQueue, &e, 0U) == pdTRUE,
                             "the row asked the IMU for nothing");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)sig, (int)e->sig,
                                  "the row asked the IMU for the wrong thing");
    s_imu_held = e;
    return e;
}

/** @brief How many requests are still waiting in AO_Imu's mailbox. */
static unsigned imu_pending(void)
{
    if ( NULL == AO_Imu->eQueue )
    {
        return 0U;
    }
    return (unsigned)uxQueueMessagesWaiting(AO_Imu->eQueue);
}

/** @brief Answer a capture request, as AO_Imu would. */
static void imu_armed(kode_status_t rc)
{
    ImuArmedEvt e;

    e.super = (QEvt)QEVT_INITIALIZER(IMU_ARMED_SIG);
    e.rc    = (uint8_t)rc;
    QASM_DISPATCH(AO_Haptic, &e.super, 0U);
}

/** @brief Answer a drain request with a measured window. */
static void imu_energy(kode_status_t rc, uint16_t n, int32_t bin_x10,
                       int32_t rms_x10)
{
    ImuEnergyEvt e;

    e.super   = (QEvt)QEVT_INITIALIZER(IMU_ENERGY_SIG);
    e.rc      = (uint8_t)rc;
    e.n       = n;
    e.bin_x10 = bin_x10;
    e.rms_x10 = rms_x10;
    e.axis    = 1U;
    QASM_DISPATCH(AO_Haptic, &e.super, 0U);
}

/** @brief Hand AO_Haptic a buzz request that never went through the door. */
static void play_evt_raw(uint16_t ms, uint8_t gain_pct)
{
    HapticPlayEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super    = (QEvt)QEVT_INITIALIZER(HAPTIC_PLAY_REQ_SIG);
    e.seg_ms[0] = ms;
    e.n        = 1U;
    e.gain_pct = gain_pct;
    QASM_DISPATCH(AO_Haptic, &e.super, 0U);
}

/** @brief Ask for a buzz through the door and run what it posted. */
static void play_evt(uint16_t ms, uint8_t gain_pct)
{
    const QEvt *e = NULL;

    TEST_ASSERT_EQUAL_INT32((int32_t)KODE_OK, (int32_t)haptic_play_request(ms, gain_pct));
    TEST_ASSERT_TRUE(xQueueReceive(AO_Haptic->eQueue, &e, 0U) == pdTRUE);
    QASM_DISPATCH(AO_Haptic, e, 0U);
    QF_gc(e);
}

/** @brief Ask for one field, the way a posted request arrives. */
static void field_req(uint8_t op, uint16_t field, int32_t value)
{
    HapticFieldReqEvt q;

    q.super = (QEvt)QEVT_INITIALIZER(HAPTIC_FIELD_REQ_SIG);
    q.op    = op;
    q.field = field;
    q.value = value;
    QASM_DISPATCH(AO_Haptic, &q.super, 0U);
}

/** @brief Start the production row. */
static void test_req(void)
{
    static const QEvt req = QEVT_INITIALIZER(HAPTIC_TEST_REQ_SIG);

    QASM_DISPATCH(AO_Haptic, &req, 0U);
}

/* ---- Turning the crank ------------------------------------------------------
 * The harness has two clocks and nothing joins them up: QF's tick, which a
 * case advances with qf_support_tick(), and the millisecond clock in
 * fake_khal_time.c, which a case advances by hand. AO_Haptic reads BOTH - its
 * timers off the first and khal_time_ms() off the second, to work out how much
 * of a buzz the chip has already played - so every tick here moves them
 * together. A tick that did not would leave the AO believing no time passes
 * between the first sample and the last.
 */

/** @brief One QP tick is this many milliseconds. */
#define TICK_MS 10U   /* one harness step */

/** @brief Advance both clocks by @p ticks, letting the AO see what lands. */
static void tick_ms(unsigned ticks)
{
    unsigned i;

    for ( i = 0U; i < ticks; ++i )
    {
        fake_time_advance_ms(TICK_MS);
        (void)qf_support_tick(AO_Haptic, 1U);
    }
}

/**
 * @brief Advance both clocks WITHOUT letting the AO dispatch what lands.
 *
 * The one thing a case cannot otherwise stage: a timer that has expired and
 * whose event is sitting in the AO's queue, behind whatever the case dispatches
 * next. That is the race the stale counters exist for, and it is real - on the
 * board the tick runs in its own thread and an AO at priority 22 can easily be
 * handed a reply before it gets to its own expiry.
 *
 * @param ticks How many ticks to let pass unseen.
 */
static void tick_quiet(unsigned ticks)
{
    unsigned i;

    for ( i = 0U; i < ticks; ++i )
    {
        fake_time_advance_ms(TICK_MS);
        (void)qf_support_pump(AO_Haptic, 0U);   /* ticks, dispatches nothing */
    }
}

/** @brief Let the AO dispatch up to @p max of what is waiting for it. */
static void pump(unsigned max)
{
    fake_time_advance_ms(TICK_MS);
    (void)qf_support_pump(AO_Haptic, max);
}

/**
 * @brief Turn the crank until the chip has been told to stop.
 *
 * Bounded well above what a buzz costs - BUZZ_MS is eighteen ticks of feeding
 * and two of playing out what is left - so a state machine that never ends
 * fails the case rather than hanging the suite.
 *
 * @return The buzz's own record.
 */
static const HapticDoneEvt *play_finish(void)
{
    unsigned i;

    for ( i = 0U; 200U > i; ++i )
    {
        if ( AW_STOP == s_regs[REG_PLAYCFG4] )
        {
            break;
        }
        tick_ms(1U);
    }
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(AW_STOP, s_regs[REG_PLAYCFG4],
                                   "the buzz never ended");
    return (const HapticDoneEvt *)spy_expect(HAPTIC_DONE_SIG);
}

/** @brief Let the row's still window, or one of its reply deadlines, run out. */
static void row_wait(uint32_t ms)
{
    tick_ms(QF_SUPPORT_STEPS(ms) + 1U);
}

/**
 * @brief Take the row through its floor: arm, still window, drain, answer.
 *
 * @param n       Samples the window held.
 * @param bin_x10 What the bench carries at 235 Hz, in 0.1 mg.
 */
static void row_floor(uint16_t n, int32_t bin_x10)
{
    const ImuCaptureReqEvt *cap;
    const ImuDrainReqEvt   *drn;

    cap = (const ImuCaptureReqEvt *)imu_expect(IMU_CAPTURE_REQ_SIG);
    TEST_ASSERT_EQUAL_PTR_MESSAGE(AO_Haptic, cap->who,
                                  "the answer would go to the wrong AO");
    imu_armed(KODE_OK);

    row_wait(KODE_HAPTIC_STILL_MS);

    drn = (const ImuDrainReqEvt *)imu_expect(IMU_DRAIN_REQ_SIG);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(KDRV_AW_F0_HZ, drn->hz,
                                   "the floor was measured at the wrong bin");
    imu_energy(KODE_OK, n, bin_x10, 300);
}

/**
 * @brief Take the row through one gain: arm, buzz, drain, answer.
 *
 * @param n       Samples the driven window held.
 * @param bin_x10 What the motor put at 235 Hz, in 0.1 mg.
 */
static void row_gain(uint16_t n, int32_t bin_x10)
{
    (void)imu_expect(IMU_CAPTURE_REQ_SIG);
    imu_armed(KODE_OK);
    (void)play_finish();
    (void)imu_expect(IMU_DRAIN_REQ_SIG);
    imu_energy(KODE_OK, n, bin_x10, 400);
}

/** @brief An energy at 235 Hz whose ratio over FLOOR_X10 clears the bar. */
static int32_t gain_bin_pass(void)
{
    return ((FLOOR_X10 * (int32_t)KODE_HAPTIC_TEST_RATIO_X10) / 10) + 100;
}

/** @brief One tenth-milli-g short of clearing it. */
static int32_t gain_bin_fail(void)
{
    return ((FLOOR_X10 * (int32_t)KODE_HAPTIC_TEST_RATIO_X10) / 10) - 1;
}

/** @brief Does any captured log line contain @p text? */
static bool logged(const char *text)
{
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, text) != NULL) )
        {
            return true;
        }
    }
    return false;
}

/* ---- A board with no AW86233 ----------------------------------------------- */

/* Absence is not busy. The flag haptic_play_request() reads is about another
 * buzz being in flight, and a board with no driver has none - so the request
 * is taken, and the AO is what says there is nothing to play. The row is the
 * other half: an operator needs to tell a missing Antenna PCB from a motor
 * that will not move, and the step-0 record is what does it. */
TEST_CASE("no chip: a play is answered inside the AO, and the row reports absent "
          "at step 0", "[haptic_ao]")
{
    const HapticTestEvt *rec;
    const TestResultEvt *verdict;

    bus_up(false);
    haptic_up();

    TEST_ASSERT_EQUAL_STRING("AO_Haptic", kbsp_owner_name(KBSP_RES_HAPTIC));

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, (int)haptic_play_request(100U, 50U),
                                  "absence is not a reason to refuse at the door");
    pump(4U);
    TEST_ASSERT_TRUE_MESSAGE(logged("no AW86233"),
                             "nothing said why the board is silent");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00U, s_regs[REG_PLAYCFG4],
                                   "a board with no chip was told to play");

    ao_spy_drain();
    test_req();

    rec = (const HapticTestEvt *)spy_expect(HAPTIC_TEST_SIG);
    TEST_ASSERT_EQUAL_UINT(0U, rec->step);
    TEST_ASSERT_EQUAL_UINT(HAPTIC_TEST_ABSENT, rec->phase);
    TEST_ASSERT_EQUAL_UINT(0U, rec->flags);

    verdict = (const TestResultEvt *)spy_expect(TEST_RESULT_SIG);
    TEST_ASSERT_EQUAL_STRING("haptic", verdict->name);
    TEST_ASSERT_FALSE(verdict->ok);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, imu_pending(),
                                   "a board with no motor asked the IMU to witness it");
    haptic_down();
}

TEST_CASE("no chip: absence is said at warning level once, then only at debug",
          "[haptic_ao]")
{
    unsigned said = 0U;

    bus_up(false);
    haptic_up();
    /* At INFO the debug repeats are dropped before the sink, so the capture
     * holds exactly what a board's console would print. haptic_up() reset
     * the sink and the default level; the per-tag override is set after it
     * and put back at the end, because the tag table outlives this case. */
    kode_log_set_level("kaos.haptic", KODE_LOG_INFO);

    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)haptic_play_request(100U, 50U));
    pump(4U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)haptic_play_request(100U, 50U));
    pump(4U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)haptic_play_request(100U, 50U));
    pump(4U);

    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, "nothing to play") != NULL) )
        {
            ++said;
        }
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, said,
        "an absent motor is reported once per boot, not once per press");

    kode_log_set_level("kaos.haptic", KODE_LOG_DEBUG);
}

/* ---- One buzz ---------------------------------------------------------------- */

/* The chip swallows samples far faster than it plays them, so writing the last
 * sample is not the end of anything: STOP issued there cuts the motor off
 * mid-waveform. The buzz ends on the tail timer, and this case is the one that
 * would catch a STOP moved into the last feed. */
TEST_CASE("a buzz feeds the FIFO in bursts of at most 64 and stops only after the "
          "chip has played it", "[haptic_ao]")
{
    const HapticDoneEvt *done;
    uint32_t             t_go;
    unsigned             i;

    bus_up(true);
    haptic_up();

    play_evt((uint16_t)BUZZ_MS, 50U);
    t_go = khal_time_ms();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(AW_GO, s_regs[REG_PLAYCFG4],
                                   "the chip was never told to play");
    TEST_ASSERT_EQUAL_HEX8((uint8_t)((50U * 255U) / 100U), s_regs[REG_PLAYCFG2]);
    TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)TICK_BYTES, (unsigned)s_fifo_bytes,
                                   "the chip was left with nothing to play until "
                                   "the first tick");

    for ( i = 0U; 100U > i; ++i )
    {
        if ( (size_t)BUZZ_SAMPLES <= s_fifo_bytes )
        {
            break;
        }
        tick_ms(1U);
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)BUZZ_SAMPLES, (unsigned)s_fifo_bytes,
                                   "the buzz was not fed to the end");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(AW_GO, s_regs[REG_PLAYCFG4],
                                   "STOP came with the last sample, not after it");

    for ( i = 0U; 100U > i; ++i )
    {
        if ( AW_STOP == s_regs[REG_PLAYCFG4] )
        {
            break;
        }
        tick_ms(1U);
    }
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(AW_STOP, s_regs[REG_PLAYCFG4],
                                   "the motor was left ringing");

    /* And STOP lands when the chip has played the buzz, not a whole buzz after
     * it was fed. Two bursts a tick is 10.7 ms of playback per 10 ms of feed,
     * so the feed takes almost exactly as long as the playback: the tail has a
     * couple of ticks left to wait, not two hundred milliseconds. */
    TEST_ASSERT_UINT_WITHIN_MESSAGE(
        2U * TICK_MS, BUZZ_MS, khal_time_ms() - t_go,
        "the buzz was held for the feed AND the playback, one after the other");

    done = (const HapticDoneEvt *)spy_expect(HAPTIC_DONE_SIG);
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)done->rc);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)BUZZ_SAMPLES, done->samples);
    TEST_ASSERT_FALSE_MESSAGE(done->starved,
                              "a feed the FIFO never refused reported "
                              "back-pressure");

    /* 2400 bytes in writes of at most 64 is 37 full bursts and a remainder,
     * and the remainder is the part that says the AO offers what is left
     * rather than a fixed block past the end of the buzz. */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(
        ((unsigned)BUZZ_SAMPLES / KDRV_AW_BURST) + 1U, s_bursts,
        "the FIFO was fed in the wrong number of bursts");
    haptic_down();
}

/* The chip rests with its driver stage parked: I2C is alive and every register
 * keeps its value, but a GO issued into standby is acknowledged and never
 * heard. A buzz is what wakes it, and the tail that ends the buzz - the one
 * door every ending goes through - is what puts it back. */
TEST_CASE("a buzz wakes the chip out of standby and the tail rests it again",
          "[haptic_ao]")
{
    bus_up(true);
    haptic_up();

    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x40U, s_regs[REG_SYSCTRL2] & 0x40U,
                                   "the driver stage is live on a board nobody "
                                   "asked to buzz");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x02U, s_regs[REG_SYSCTRL2] & 0x03U,
                                   "the rate the run write proved is not under it");

    play_evt((uint16_t)BUZZ_MS, 50U);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00U, s_regs[REG_SYSCTRL2] & 0x40U,
                                   "the chip was told to play while still parked");
    TEST_ASSERT_EQUAL_HEX8(AW_GO, s_regs[REG_PLAYCFG4]);

    (void)play_finish();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x40U, s_regs[REG_SYSCTRL2] & 0x40U,
                                   "the chip was left out of standby after the buzz");
    haptic_down();
}

/* A buzz that never started still ends through the tail, so it is still put
 * back to rest: the failure path and the good one share the one site. */
TEST_CASE("a buzz that could not be started still leaves the chip resting",
          "[haptic_ao]")
{
    const HapticDoneEvt *done;
    const QEvt          *stray = NULL;

    bus_up(true);
    haptic_up();

    s_burst_fail = true;
    play_evt((uint16_t)BUZZ_MS, 50U);
    done = play_finish();
    TEST_ASSERT_NOT_EQUAL_INT(KODE_OK, (int)done->rc);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x40U, s_regs[REG_SYSCTRL2] & 0x40U,
                                   "a buzz that failed left the driver stage live");

    s_burst_fail = false;
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)haptic_play_request(10U, 50U));
    while ( xQueueReceive(AO_Haptic->eQueue, &stray, 0U) == pdTRUE )
    {
        QF_gc(stray);
    }
    haptic_down();
}

/* Bit 6 is the owner's and moves on every buzz. What is under it is not: a
 * sample rate set through the field table has to survive both halves. */
TEST_CASE("a rate a user set survives a buzz", "[haptic_ao]")
{
    const HapticFieldEvt *f;

    bus_up(true);
    haptic_up();

    field_req(FIELD_OP_SET, (uint16_t)KDRV_AW_SYSCTRL2, 0x43);
    f = (const HapticFieldEvt *)spy_expect(HAPTIC_FIELD_SIG);
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)f->rc);

    play_evt((uint16_t)BUZZ_MS, 50U);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x03U, s_regs[REG_SYSCTRL2],
                                   "waking the chip rewrote the rate");

    (void)play_finish();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x43U, s_regs[REG_SYSCTRL2],
                                   "resting the chip rewrote the rate");
    haptic_down();
}

/* A write that lands and a read-back that does not is not a success. Every
 * other owner reports the read-back's verdict, and `>ok value=0` for a chip
 * that stopped answering sends an operator looking in the wrong place. */
TEST_CASE("a field write whose read-back fails is reported as the failure it is",
          "[haptic_ao]")
{
    const HapticFieldEvt *f;

    bus_up(true);
    haptic_up();

    s_kill_read = REG_PLAYCFG2;
    field_req(FIELD_OP_SET, (uint16_t)KDRV_AW_GAIN, 0x44);
    f = (const HapticFieldEvt *)spy_expect(HAPTIC_FIELD_SIG);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_IO_ERROR, (int)f->rc,
                                  "a read-back that never happened was published as "
                                  "a value");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x44U, s_regs[REG_PLAYCFG2],
                                   "the write itself never reached the chip, so the "
                                   "case proves nothing");

    s_kill_read = 0U;
    s_acks      = true;
    haptic_down();
}

/* The almost-full flag is back-pressure, not a failure: the FIFO is full of
 * this AO's own samples and the right answer is to come back next tick. */
TEST_CASE("FF_AFS holds a tick back without ending the buzz", "[haptic_ao]")
{
    size_t held;

    bus_up(true);
    haptic_up();

    play_evt((uint16_t)BUZZ_MS, 50U);
    held = s_fifo_bytes;

    s_regs[REG_SYSST] = AW_FF_AFS;
    tick_ms(1U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)held, (unsigned)s_fifo_bytes,
                                   "a full FIFO was written to anyway");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(AW_GO, s_regs[REG_PLAYCFG4],
                                   "back-pressure ended the buzz");

    s_regs[REG_SYSST] = 0x00U;
    tick_ms(1U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)(held + TICK_BYTES),
                                   (unsigned)s_fifo_bytes,
                                   "the tick after did not pick the feed back up");

    (void)play_finish();
    haptic_down();
}

/* The refusal happens at the door rather than inside the AO, because a caller
 * that gets KODE_OK has been promised a buzz, and a second request that only
 * learns it was dropped from a log line is a promise broken quietly. */
TEST_CASE("a second play while one plays is refused before it is posted",
          "[haptic_ao]")
{
    bus_up(true);
    haptic_up();

    play_evt((uint16_t)BUZZ_MS, 50U);

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)haptic_play_request(50U, 50U));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(
        0U, (unsigned)uxQueueMessagesWaiting(AO_Haptic->eQueue),
        "the refused request was posted anyway");

    (void)play_finish();

    /* And the door opens again the moment the buzz is over. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)haptic_play_request(10U, 50U));
    pump(4U);
    (void)play_finish();
    haptic_down();
}

/* ---- The production row -------------------------------------------------------- */

/* The row is a comparison, not a measurement: what the bench carries at
 * 235 Hz with the motor off is the number every driven window is divided by.
 * It stops at the first gain that clears the bar, because the question is
 * whether the motor moves at all and a louder answer to it is not a better
 * one. */
TEST_CASE("the row: floor, then gains until one clears the ratio", "[haptic_ao]")
{
    const HapticTestEvt *rec;
    const TestResultEvt *verdict;

    bus_up(true);
    haptic_up();

    test_req();
    row_floor((uint16_t)WINDOW_N, FLOOR_X10);

    rec = (const HapticTestEvt *)spy_expect(HAPTIC_TEST_SIG);
    TEST_ASSERT_EQUAL_UINT(1U, rec->step);
    TEST_ASSERT_EQUAL_UINT(HAPTIC_TEST_FLOOR, rec->phase);
    TEST_ASSERT_EQUAL_INT(FLOOR_X10, rec->floor_x10);
    TEST_ASSERT_EQUAL_UINT((uint16_t)WINDOW_N, rec->n);
    TEST_ASSERT_EQUAL_UINT((uint8_t)HAPTIC_TF_PASS,
                           rec->flags & (uint8_t)HAPTIC_TF_PASS);

    row_gain((uint16_t)WINDOW_N, gain_bin_fail());

    rec = (const HapticTestEvt *)spy_expect(HAPTIC_TEST_SIG);
    TEST_ASSERT_EQUAL_UINT(2U, rec->step);
    TEST_ASSERT_EQUAL_UINT(HAPTIC_TEST_GAIN, rec->phase);
    /* The PLAYCFG2 byte the chip was actually given for the row's first,
     * weakest gain - 25 per cent - and not the percentage it was asked for. */
    TEST_ASSERT_EQUAL_HEX8(0x3FU, rec->gain);
    TEST_ASSERT_EQUAL_INT(gain_bin_fail(), rec->on_x10);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_HAPTIC_TEST_RATIO_X10 - 1U,
                             rec->ratio_x10);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, rec->flags & (uint8_t)HAPTIC_TF_PASS,
                                   "a gain one tenth short of the bar passed");

    row_gain((uint16_t)WINDOW_N, gain_bin_pass());

    rec = (const HapticTestEvt *)spy_expect(HAPTIC_TEST_SIG);
    TEST_ASSERT_EQUAL_UINT(2U, rec->step);
    TEST_ASSERT_EQUAL_UINT(HAPTIC_TEST_GAIN, rec->phase);
    TEST_ASSERT_EQUAL_HEX8(0x7FU, rec->gain);   /* 50 per cent */
    TEST_ASSERT_EQUAL_UINT((uint8_t)HAPTIC_TF_PASS,
                           rec->flags & (uint8_t)HAPTIC_TF_PASS);

    verdict = (const TestResultEvt *)spy_expect(TEST_RESULT_SIG);
    TEST_ASSERT_EQUAL_STRING("haptic", verdict->name);
    TEST_ASSERT_TRUE(verdict->ok);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, imu_pending(),
                                   "the row went on measuring after it had its answer");
    /* And the door is open again: the row released the flag it took. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)haptic_play_request(10U, 50U));
    pump(4U);
    (void)play_finish();
    haptic_down();
}

/* The chip answering is not the motor moving, and the verdict has to say
 * which of the two failed or an operator bins the wrong board. */
TEST_CASE("no gain clears: the verdict names the motor, not the chip", "[haptic_ao]")
{
    const HapticTestEvt *rec;
    const TestResultEvt *verdict;
    unsigned             i;

    bus_up(true);
    haptic_up();

    test_req();
    row_floor((uint16_t)WINDOW_N, FLOOR_X10);
    (void)spy_expect(HAPTIC_TEST_SIG);   /* the floor */

    for ( i = 0U; (unsigned)KODE_HAPTIC_TEST_GAINS_N > i; ++i )
    {
        row_gain((uint16_t)WINDOW_N, gain_bin_fail());
        rec = (const HapticTestEvt *)spy_expect(HAPTIC_TEST_SIG);
        TEST_ASSERT_EQUAL_UINT(HAPTIC_TEST_GAIN, rec->phase);
        TEST_ASSERT_EQUAL_UINT(0U, rec->flags & (uint8_t)HAPTIC_TF_PASS);
    }

    verdict = (const TestResultEvt *)spy_expect(TEST_RESULT_SIG);
    TEST_ASSERT_EQUAL_STRING("haptic", verdict->name);
    TEST_ASSERT_FALSE(verdict->ok);
    TEST_ASSERT_TRUE_MESSAGE(logged("does not move"),
                             "the verdict does not name the motor");
    TEST_ASSERT_EQUAL_UINT(0U, imu_pending());
    haptic_down();
}

/* The failure the row must not turn into a verdict about the motor. A buzz
 * the chip stopped taking ends like any other - STOP, a done record, the
 * flag given back - so the row would measure a window nothing was driven
 * into, find it as quiet as the floor, and after three gains of that print
 * "the LRA does not move" about a fault on the wire. */
TEST_CASE("a buzz the chip stopped taking is the wire's fault, not the motor's",
          "[haptic_ao]")
{
    const HapticDoneEvt *done;
    const HapticTestEvt *rec;
    const TestResultEvt *verdict;

    bus_up(true);
    haptic_up();

    test_req();
    row_floor((uint16_t)WINDOW_N, FLOOR_X10);
    (void)spy_expect(HAPTIC_TEST_SIG);   /* the floor */

    /* The first gain's window is armed, and then the FIFO stops draining for
     * good: the buzz runs out of patience instead of out of samples. */
    (void)imu_expect(IMU_CAPTURE_REQ_SIG);
    imu_armed(KODE_OK);
    s_afs_hold = true;

    done = play_finish();
    TEST_ASSERT_NOT_EQUAL_INT_MESSAGE(KODE_OK, (int)done->rc,
                                      "the buzz reported itself as having played");

    rec = (const HapticTestEvt *)spy_expect(HAPTIC_TEST_SIG);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(HAPTIC_TEST_NOVERDICT, rec->phase,
                                   "the row graded a window nothing was driven "
                                   "into");
    TEST_ASSERT_EQUAL_UINT((uint8_t)HAPTIC_TF_MUTE, rec->flags);

    verdict = (const TestResultEvt *)spy_expect(TEST_RESULT_SIG);
    TEST_ASSERT_EQUAL_STRING("haptic", verdict->name);
    TEST_ASSERT_FALSE(verdict->ok);
    TEST_ASSERT_FALSE_MESSAGE(logged("does not move"),
                              "a fault on the bus was blamed on the motor");

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, imu_pending(),
                                   "the row asked the witness for another gain");
    /* And the motor is given back: the row ended, whichever way. */
    s_afs_hold = false;
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)haptic_play_request(10U, 50U));
    pump(4U);
    (void)play_finish();
    haptic_down();
}

/* A bench that already carries 235 Hz - a vibrating table, a neighbour's
 * motor - would pass any board put on it. Refusing to judge is the honest
 * answer, and it is about the bench rather than the board. */
TEST_CASE("a floor already vibrating at 235 Hz is the bench's fault, not the "
          "board's", "[haptic_ao]")
{
    const HapticTestEvt *rec;
    const TestResultEvt *verdict;

    bus_up(true);
    haptic_up();

    test_req();
    row_floor((uint16_t)WINDOW_N, (int32_t)KODE_HAPTIC_FLOOR_MAX_X10 + 1);

    rec = (const HapticTestEvt *)spy_expect(HAPTIC_TEST_SIG);
    TEST_ASSERT_EQUAL_UINT(1U, rec->step);
    TEST_ASSERT_EQUAL_UINT(HAPTIC_TEST_FLOOR, rec->phase);
    TEST_ASSERT_EQUAL_UINT((uint8_t)HAPTIC_TF_LOUD,
                           rec->flags & (uint8_t)HAPTIC_TF_LOUD);
    TEST_ASSERT_EQUAL_UINT(0U, rec->flags & (uint8_t)HAPTIC_TF_PASS);

    rec = (const HapticTestEvt *)spy_expect(HAPTIC_TEST_SIG);
    TEST_ASSERT_EQUAL_UINT(HAPTIC_TEST_NOVERDICT, rec->phase);
    TEST_ASSERT_EQUAL_UINT((uint8_t)HAPTIC_TF_LOUD, rec->flags);

    verdict = (const TestResultEvt *)spy_expect(TEST_RESULT_SIG);
    TEST_ASSERT_FALSE(verdict->ok);

    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00U, s_regs[REG_PLAYCFG4],
                                   "the motor was driven into a bench already "
                                   "vibrating");
    TEST_ASSERT_EQUAL_UINT(0U, imu_pending());
    haptic_down();
}

/* A ratio computed over eleven samples is a number, not a measurement. The
 * row says so rather than producing a verdict nobody should act on. */
TEST_CASE("a short capture is no verdict", "[haptic_ao]")
{
    const HapticTestEvt *rec;
    const TestResultEvt *verdict;

    bus_up(true);
    haptic_up();

    test_req();
    row_floor((uint16_t)WINDOW_N, FLOOR_X10);
    (void)spy_expect(HAPTIC_TEST_SIG);   /* the floor */

    row_gain((uint16_t)(KODE_HAPTIC_MIN_SAMPLES - 1U), gain_bin_pass());

    rec = (const HapticTestEvt *)spy_expect(HAPTIC_TEST_SIG);
    TEST_ASSERT_EQUAL_UINT(HAPTIC_TEST_GAIN, rec->phase);
    TEST_ASSERT_EQUAL_UINT((uint8_t)HAPTIC_TF_SHORT,
                           rec->flags & (uint8_t)HAPTIC_TF_SHORT);

    rec = (const HapticTestEvt *)spy_expect(HAPTIC_TEST_SIG);
    TEST_ASSERT_EQUAL_UINT(HAPTIC_TEST_NOVERDICT, rec->phase);
    TEST_ASSERT_EQUAL_UINT((uint8_t)HAPTIC_TF_SHORT, rec->flags);

    verdict = (const TestResultEvt *)spy_expect(TEST_RESULT_SIG);
    TEST_ASSERT_FALSE(verdict->ok);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, imu_pending(),
                                   "the row carried on after it stopped measuring");
    haptic_down();
}

/* The witness is half the row. Without it there is no verdict to give, and
 * saying FAIL would condemn a motor nobody listened to. */
TEST_CASE("an IMU that does not answer is no verdict", "[haptic_ao]")
{
    const HapticTestEvt *rec;
    const TestResultEvt *verdict;
    const QEvt          *stray = NULL;

    bus_up(true);
    haptic_up();

    test_req();
    (void)imu_expect(IMU_CAPTURE_REQ_SIG);   /* asked, and never answered */

    row_wait(KODE_HAPTIC_REPLY_MS);

    rec = (const HapticTestEvt *)spy_expect(HAPTIC_TEST_SIG);
    TEST_ASSERT_EQUAL_UINT(HAPTIC_TEST_NOVERDICT, rec->phase);
    TEST_ASSERT_EQUAL_UINT((uint8_t)HAPTIC_TF_MUTE, rec->flags);

    verdict = (const TestResultEvt *)spy_expect(TEST_RESULT_SIG);
    TEST_ASSERT_EQUAL_STRING("haptic", verdict->name);
    TEST_ASSERT_FALSE(verdict->ok);

    /* The row that could not finish still gave the flag back. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, (int)haptic_play_request(10U, 50U),
                                  "a row that gave up left the board busy forever");
    while ( xQueueReceive(AO_Haptic->eQueue, &stray, 0U) == pdTRUE )
    {
        QF_gc(stray);
    }
    haptic_down();
}

/* The flag is taken for the whole row, not for each of its buzzes: a request
 * landing between two gains would measure the wrong thing. */
TEST_CASE("a play during the row is busy", "[haptic_ao]")
{
    const TestResultEvt *verdict;

    bus_up(true);
    haptic_up();

    test_req();
    (void)imu_expect(IMU_CAPTURE_REQ_SIG);

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)haptic_play_request(50U, 50U));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(
        0U, (unsigned)uxQueueMessagesWaiting(AO_Haptic->eQueue),
        "the refused request was posted into the row");

    /* And one that reached the AO some other way is dropped rather than
     * played over the window being measured. */
    play_evt_raw(50U, 50U);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00U, s_regs[REG_PLAYCFG4],
                                   "a buzz played on top of the row's own window");

    row_wait(KODE_HAPTIC_REPLY_MS);
    (void)spy_expect(HAPTIC_TEST_SIG);   /* the no-verdict record */
    verdict = (const TestResultEvt *)spy_expect(TEST_RESULT_SIG);
    TEST_ASSERT_FALSE(verdict->ok);
    haptic_down();
}

/* ---- A buzz that cannot be fed ------------------------------------------------
 * Both of these are about the same defect from two directions: the feed's only
 * way out used to be reaching the last sample. A chip that stops taking
 * samples therefore never reached it, and the AO fed it forever - a log line
 * every ten milliseconds, no HAPTIC_DONE_SIG for the caller waiting on one, the
 * busy flag never given back, and a row parked in GAIN_PLAY behind a window
 * nobody would ever measure.
 */

/* The bus half: the chip stops acknowledging bursts mid-buzz. */
TEST_CASE("a FIFO that refuses a burst ends the buzz instead of retrying forever",
          "[haptic_ao]")
{
    const HapticDoneEvt *done;
    const QEvt          *stray = NULL;

    bus_up(true);
    haptic_up();

    play_evt((uint16_t)BUZZ_MS, 50U);
    TEST_ASSERT_EQUAL_HEX8(AW_GO, s_regs[REG_PLAYCFG4]);

    s_burst_fail = true;
    done = play_finish();   /* bounded: a buzz that never ends fails here */

    TEST_ASSERT_NOT_EQUAL_INT_MESSAGE(KODE_OK, (int)done->rc,
                                      "a buzz the chip refused was reported as fine");
    TEST_ASSERT_TRUE_MESSAGE((uint32_t)BUZZ_SAMPLES > done->samples,
                             "it claims to have fed a buzz the chip would not take");
    TEST_ASSERT_TRUE(logged("stopped taking samples"));

    /* And the door is open again: a failed buzz still gives the flag back. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, (int)haptic_play_request(10U, 50U),
                                  "a buzz that failed left the board busy forever");
    while ( xQueueReceive(AO_Haptic->eQueue, &stray, 0U) == pdTRUE )
    {
        QF_gc(stray);
    }
    haptic_down();
}

/* The chip half: every write is acknowledged and the FIFO never drains, so
 * every burst is refused with FF_AFS and nothing is ever an error. Patience
 * here has to be bounded by a clock rather than by a status code. */
TEST_CASE("a FIFO that never drains ends the buzz once it has waited long enough",
          "[haptic_ao]")
{
    const HapticDoneEvt *done;
    const QEvt          *stray = NULL;
    unsigned             i;

    bus_up(true);
    haptic_up();

    play_evt((uint16_t)BUZZ_MS, 50U);
    s_afs_hold = true;

    /* Twenty ticks of patience, and one more for the tail. */
    for ( i = 0U; 21U > i; ++i )
    {
        if ( AW_STOP == s_regs[REG_PLAYCFG4] )
        {
            break;
        }
        tick_ms(1U);
    }
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(AW_STOP, s_regs[REG_PLAYCFG4],
                                   "the AO is still offering bursts to a dead FIFO");

    done = (const HapticDoneEvt *)spy_expect(HAPTIC_DONE_SIG);
    TEST_ASSERT_NOT_EQUAL_INT(KODE_OK, (int)done->rc);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)TICK_BYTES, done->samples,
                                     "only the two bursts fed before GO went in");
    TEST_ASSERT_TRUE(logged("has not taken a sample"));

    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)haptic_play_request(10U, 50U));
    while ( xQueueReceive(AO_Haptic->eQueue, &stray, 0U) == pdTRUE )
    {
        QF_gc(stray);
    }
    haptic_down();
}

/* ---- The tail is what the chip STILL HOLDS ------------------------------------ */

/* Feeding is not free of time: the FIFO is offered bursts against its own
 * almost-full flag, so a chip that drains slowly is fed slowly. Waiting out the
 * whole of the playback afterwards would hold GO and the busy flag for the feed
 * AND the buzz, one after the other. */
TEST_CASE("a slowly draining FIFO does not add the feed's time to the tail",
          "[haptic_ao]")
{
    const HapticDoneEvt *done;
    uint32_t             t_go;
    uint32_t             fed_at;
    unsigned             i;

    bus_up(true);
    haptic_up();

    s_afs_throttle = true;   /* one burst accepted per tick, not two */
    play_evt((uint16_t)BUZZ_MS, 50U);
    t_go = khal_time_ms();

    for ( i = 0U; 200U > i; ++i )
    {
        if ( (size_t)BUZZ_SAMPLES <= s_fifo_bytes )
        {
            break;
        }
        tick_ms(1U);
    }
    fed_at = khal_time_ms();
    TEST_ASSERT_EQUAL_UINT((unsigned)BUZZ_SAMPLES, (unsigned)s_fifo_bytes);
    TEST_ASSERT_TRUE_MESSAGE((fed_at - t_go) > BUZZ_MS,
                             "the throttle did not slow the feed down at all");

    for ( i = 0U; 200U > i; ++i )
    {
        if ( AW_STOP == s_regs[REG_PLAYCFG4] )
        {
            break;
        }
        tick_ms(1U);
    }
    TEST_ASSERT_EQUAL_HEX8(AW_STOP, s_regs[REG_PLAYCFG4]);

    /* The chip finished playing long before the feed finished handing it
     * samples, so there is nothing left to wait out: STOP follows the last
     * sample by a tick, not by a whole buzz. */
    TEST_ASSERT_TRUE_MESSAGE((khal_time_ms() - fed_at) <= (2U * TICK_MS),
                             "the tail waited out playback the chip had already "
                             "done");

    /* And the record says the feed met the FIFO's back-pressure, which is the
     * one thing about this buzz the status code cannot carry: it played out
     * whole, and it spent the whole of itself waiting for room. */
    done = (const HapticDoneEvt *)spy_expect(HAPTIC_DONE_SIG);
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)done->rc);
    TEST_ASSERT_TRUE_MESSAGE(done->starved,
                             "a buzz fed against FF_AFS reported a clear run");
    haptic_down();
}

/* ---- Deadlines that have already expired -------------------------------------- */

/* The race the stale counters exist for. On the board the tick runs in its own
 * thread, so an AO at priority 22 is easily handed a reply before it gets round
 * to its own expiry - and that expiry is then a generation behind. Counting it
 * is only half the job: the count has to be CONSUMED wherever the transition
 * the reply caused happens to land, or the next genuine deadline is eaten by
 * it and the row waits forever on an IMU that has already gone quiet. */
TEST_CASE("an expiry queued behind a reply does not swallow the next deadline",
          "[haptic_ao]")
{
    const HapticTestEvt *rec;
    const TestResultEvt *verdict;

    bus_up(true);
    haptic_up();

    test_req();
    row_floor((uint16_t)WINDOW_N, FLOOR_X10);
    (void)spy_expect(HAPTIC_TEST_SIG);   /* the floor, judged normally */

    /* The gain's arm deadline runs out and its expiry sits in the queue; the
     * IMU's answer is dispatched first, and the row follows the answer. */
    (void)imu_expect(IMU_CAPTURE_REQ_SIG);
    tick_quiet(QF_SUPPORT_STEPS(KODE_HAPTIC_REPLY_MS) + 1U);
    imu_armed(KODE_OK);       /* -> Haptic_playing, with the expiry still queued */
    (void)play_finish();      /* which is where it is finally dispatched */

    /* Now the drain deadline, and this time nobody answers at all. It has to
     * fire: the count the stale one left behind would otherwise eat it. */
    (void)imu_expect(IMU_DRAIN_REQ_SIG);
    row_wait(KODE_HAPTIC_REPLY_MS);

    rec = (const HapticTestEvt *)spy_expect(HAPTIC_TEST_SIG);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(HAPTIC_TEST_NOVERDICT, rec->phase,
                                   "the genuine deadline was swallowed by a stale "
                                   "one");
    TEST_ASSERT_EQUAL_UINT((uint8_t)HAPTIC_TF_MUTE, rec->flags);

    verdict = (const TestResultEvt *)spy_expect(TEST_RESULT_SIG);
    TEST_ASSERT_FALSE(verdict->ok);
    haptic_down();
}

/* The other half of the same bookkeeping. Dropping a stale expiry must not
 * touch the live flag, because that flag now belongs to the timer armed in its
 * place: marking THAT one dead means the next reply-before-expiry is not
 * counted, and the expiry it leaves behind is read as a genuine deadline - a
 * no-verdict on a board that answered everything it was asked. */
TEST_CASE("a reply that beats its own deadline leaves the next window armed",
          "[haptic_ao]")
{
    const HapticTestEvt *rec;
    const TestResultEvt *verdict;

    bus_up(true);
    haptic_up();

    test_req();
    (void)imu_expect(IMU_CAPTURE_REQ_SIG);
    imu_armed(KODE_OK);
    row_wait(KODE_HAPTIC_STILL_MS);
    (void)imu_expect(IMU_DRAIN_REQ_SIG);

    /* First race: the drain deadline expires, the energy arrives first. */
    tick_quiet(QF_SUPPORT_STEPS(KODE_HAPTIC_REPLY_MS) + 1U);
    imu_energy(KODE_OK, (uint16_t)WINDOW_N, FLOOR_X10, 300);
    rec = (const HapticTestEvt *)spy_expect(HAPTIC_TEST_SIG);
    TEST_ASSERT_EQUAL_UINT(HAPTIC_TEST_FLOOR, rec->phase);
    TEST_ASSERT_EQUAL_UINT((uint8_t)HAPTIC_TF_PASS,
                           rec->flags & (uint8_t)HAPTIC_TF_PASS);
    pump(1U);   /* the stale expiry, dispatched while the row is still the row */

    /* Second race, on the window armed by the first one's reply. */
    (void)imu_expect(IMU_CAPTURE_REQ_SIG);
    tick_quiet(QF_SUPPORT_STEPS(KODE_HAPTIC_REPLY_MS) + 1U);
    imu_armed(KODE_OK);
    (void)play_finish();

    (void)imu_expect(IMU_DRAIN_REQ_SIG);
    imu_energy(KODE_OK, (uint16_t)WINDOW_N, gain_bin_pass(), 400);

    rec = (const HapticTestEvt *)spy_expect(HAPTIC_TEST_SIG);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(HAPTIC_TEST_GAIN, rec->phase,
                                   "a reply that arrived was reported as silence");
    verdict = (const TestResultEvt *)spy_expect(TEST_RESULT_SIG);
    TEST_ASSERT_TRUE_MESSAGE(verdict->ok,
                             "a board that answered everything failed the row");
    TEST_ASSERT_FALSE_MESSAGE(logged("nothing waiting on it"),
                              "a deadline was dropped that nothing had counted");
    haptic_down();
}

/* ---- Replies for a phase the row is not in ------------------------------------- */

/* A row acts only on the reply it is waiting for. The energy half is the
 * expensive one: a window measured before the motor was ever driven would be
 * judged against a floor of zero, which the ratio floors at one, so ANY reading
 * at all would clear the bar and pass a board whose motor never moved. */
TEST_CASE("a reply for a phase the row is not in changes nothing", "[haptic_ao]")
{
    const HapticTestEvt *rec;
    const TestResultEvt *verdict;

    bus_up(true);
    haptic_up();

    test_req();
    (void)imu_expect(IMU_CAPTURE_REQ_SIG);
    imu_armed(KODE_OK);   /* the still window is filling: nothing is expected */

    imu_energy(KODE_OK, (uint16_t)WINDOW_N, 5000, 5000);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "a window nobody asked for was judged as a gain");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00U, s_regs[REG_PLAYCFG4],
                                   "it drove the motor off an unasked-for reply");

    imu_armed(KODE_OK);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, imu_pending(),
                                   "the row asked the IMU for something new");
    TEST_ASSERT_TRUE(logged("a phase this row is not in"));

    /* And neither of them touched the deadline the row IS waiting on: the
     * still window runs out on its own clock and the row carries on. */
    row_wait(KODE_HAPTIC_STILL_MS);
    (void)imu_expect(IMU_DRAIN_REQ_SIG);
    imu_energy(KODE_OK, (uint16_t)WINDOW_N, FLOOR_X10, 300);

    rec = (const HapticTestEvt *)spy_expect(HAPTIC_TEST_SIG);
    TEST_ASSERT_EQUAL_UINT(HAPTIC_TEST_FLOOR, rec->phase);
    TEST_ASSERT_EQUAL_INT(FLOOR_X10, rec->floor_x10);

    /* End it: nobody answers the gain's arm, so the row gives up cleanly. */
    (void)imu_expect(IMU_CAPTURE_REQ_SIG);
    row_wait(KODE_HAPTIC_REPLY_MS);
    (void)spy_expect(HAPTIC_TEST_SIG);
    verdict = (const TestResultEvt *)spy_expect(TEST_RESULT_SIG);
    TEST_ASSERT_FALSE(verdict->ok);
    haptic_down();
}

/* ---- The field table ----------------------------------------------------------- */

/* Answered in every state the chip is in, a buzz included: the driver reads
 * registers, which needs neither an idle motor nor an idle owner - and a
 * console asking `haptic` mid-buzz is asking what the chip says right now. */
TEST_CASE("a field is read, written and dumped, mid-buzz as well as idle",
          "[haptic_ao]")
{
    const HapticFieldEvt *f;

    bus_up(true);
    haptic_up();

    s_regs[REG_PLAYCFG2] = 0x33U;
    field_req(FIELD_OP_GET, (uint16_t)KDRV_AW_GAIN, 0);
    f = (const HapticFieldEvt *)spy_expect(HAPTIC_FIELD_SIG);
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)f->rc);
    TEST_ASSERT_EQUAL_INT(0x33, f->value);

    field_req(FIELD_OP_SET, (uint16_t)KDRV_AW_GAIN, 0x44);
    f = (const HapticFieldEvt *)spy_expect(HAPTIC_FIELD_SIG);
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)f->rc);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0x44, f->value,
                                  "a write reports what the chip holds, not what "
                                  "was asked for");

    /* A read-only field is refused rather than silently ignored. */
    field_req(FIELD_OP_SET, (uint16_t)KDRV_AW_CHIPID_FIELD, 1);
    f = (const HapticFieldEvt *)spy_expect(HAPTIC_FIELD_SIG);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, (int)f->rc);

    /* And the same answer arrives while the motor is running. */
    play_evt((uint16_t)BUZZ_MS, 50U);
    field_req(FIELD_OP_DUMP, 0U, 0);
    f = (const HapticFieldEvt *)spy_expect(HAPTIC_FIELD_SIG);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, (int)f->rc,
                                  "a buzz is not a reason to stop answering");
    (void)play_finish();
    haptic_down();
}

/* ---- The setting ------------------------------------------------------------ */

/* `haptic` is a scale on the gain an app asks for: 60 % of a board set to 50
 * reaches the chip as 30 % would at full scale. */
TEST_CASE("haptic scales an app's buzz, and 0 answers it without playing",
          "[haptic_ao]")
{
    const KodeConfigAnsEvt *a;
    const HapticDoneEvt    *done;
    uint8_t                 at_30;

    fake_nvs_reset();
    bus_up(true);
    haptic_up();

    play_evt((uint16_t)BUZZ_MS, 30U);
    (void)play_finish();
    at_30 = s_regs[REG_PLAYCFG2];

    a = config_ask(AO_Haptic, &(config_ask_t){ .key = KSET_KEY_HAPTIC, .set = true,
                                               .i = 50 });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("50", a->value);
    TEST_ASSERT_TRUE(a->stored);

    s_regs[REG_PLAYCFG2] = 0U;
    play_evt((uint16_t)BUZZ_MS, 60U);
    (void)play_finish();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(at_30, s_regs[REG_PLAYCFG2],
                                   "the stored scale did not reach the chip");

    a = config_ask(AO_Haptic, &(config_ask_t){ .key = KSET_KEY_HAPTIC, .set = true });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("0", a->value);
    s_regs[REG_PLAYCFG4] = 0U;
    play_evt((uint16_t)BUZZ_MS, 60U);
    done = (const HapticDoneEvt *)spy_expect(HAPTIC_DONE_SIG);
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)done->rc);
    TEST_ASSERT_EQUAL_UINT32(0U, done->samples);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0U, s_regs[REG_PLAYCFG4],
                                   "a silenced buzz was played");

    fake_nvs_reset();
    haptic_down();
}

TEST_CASE("haptic is read from the store at boot, full scale when absent",
          "[haptic_ao]")
{
    const KodeConfigAnsEvt *a;

    fake_nvs_reset();
    bus_up(true);
    haptic_up();
    a = config_ask(AO_Haptic, &(config_ask_t){ .key = KSET_KEY_HAPTIC });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("100", a->value);
    TEST_ASSERT_FALSE(a->stored);
    haptic_down();

    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_u8(KSET_NAMESPACE, KSET_KEY_HAPTIC, 20U));
    bus_up(true);
    haptic_up();
    a = config_ask(AO_Haptic, &(config_ask_t){ .key = KSET_KEY_HAPTIC });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("20", a->value);
    TEST_ASSERT_TRUE(a->stored);
    fake_nvs_reset();
    haptic_down();
}

/* ---- Patterns and holds ---------------------------------------------------- */

/** @brief Samples in @p ms of the stream. */
#define SAMPLES_OF(ms_)  ((size_t)KDRV_AW_SAMPLES(ms_))

/** @brief Is every sample of [@p from, @p to) in the fed stream zero? */
static bool fifo_silent(size_t from, size_t to)
{
    for ( size_t i = from; i < to; ++i )
    {
        if ( 0 != s_fifo_data[i] )
        {
            return false;
        }
    }
    return true;
}

/** @brief Does any sample of [@p from, @p to) in the fed stream move? */
static bool fifo_sounds(size_t from, size_t to)
{
    return !fifo_silent(from, to);
}

/** @brief Nothing is waiting in AO_Haptic's queue. */
static void queue_empty(const char *why)
{
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0U, (uint32_t)uxQueueMessagesWaiting(AO_Haptic->eQueue), why);
}

TEST_CASE("a pattern feeds its on segments as the sine and its off segments as "
          "zeros, in one stream", "[haptic_ao]")
{
    static const uint16_t seg[] = { 30U, 20U, 30U };
    const HapticDoneEvt  *done;
    size_t const          a = SAMPLES_OF(30U);
    size_t const          b = SAMPLES_OF(20U);
    size_t const          total = (2U * a) + b;

    bus_up(true);
    haptic_up();

    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, (uint32_t)haptic_pattern_request(seg, 3U, 50U));
    pump(4U);
    done = play_finish();

    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)total, done->samples,
                                     "the pattern is not 80 ms of samples");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)total, (uint32_t)s_fifo_bytes);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, (uint32_t)done->rc);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)((50U * 255U) / 100U), s_regs[REG_PLAYCFG2]);

    TEST_ASSERT_TRUE_MESSAGE(fifo_sounds(0U, a), "the first on segment is silent");
    TEST_ASSERT_TRUE_MESSAGE(fifo_silent(a, a + b),
                             "the off segment is not zero samples");
    TEST_ASSERT_TRUE_MESSAGE(fifo_sounds(a + b, total),
                             "the second on segment is silent");
    /* The sine starts over at each on segment, so the second one is the first
     * one again and begins from a zero crossing rather than from a step. */
    TEST_ASSERT_EQUAL_INT8_ARRAY((const int8_t *)&s_fifo_data[0],
                                 (const int8_t *)&s_fifo_data[a + b], a);
    haptic_down();
}

TEST_CASE("a hold keeps feeding past the buzz limit and ends only on a stop, "
          "with one DONE", "[haptic_ao]")
{
    const HapticDoneEvt *done;
    size_t const         past = SAMPLES_OF(KODE_SYS_HAPTIC_MAX_MS) + SAMPLES_OF(500U);
    size_t               at_stop;
    uint32_t             i;

    bus_up(true);
    haptic_up();

    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, (uint32_t)haptic_hold_request(50U));
    pump(4U);
    TEST_ASSERT_EQUAL_HEX8(AW_GO, s_regs[REG_PLAYCFG4]);
    for ( i = 0U; 400U > i; ++i )
    {
        if ( s_fifo_bytes >= past )
        {
            break;
        }
        tick_ms(1U);
    }
    TEST_ASSERT_TRUE_MESSAGE(s_fifo_bytes >= past,
                             "the hold stopped feeding on its own");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(AW_GO, s_regs[REG_PLAYCFG4],
                                   "the hold was stopped by itself");
    TEST_ASSERT_NULL_MESSAGE(ao_spy_take(), "the hold reported an end");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)KODE_DENIED, (uint32_t)haptic_hold_request(50U),
                                  "a hold is a buzz in flight");

    haptic_stop_request();
    pump(4U);
    at_stop = s_fifo_bytes;
    done = play_finish();
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, (uint32_t)done->rc);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)at_stop, done->samples);
    tick_ms(10U);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)at_stop, (uint32_t)s_fifo_bytes,
                                   "the feed went on after the stop");
    TEST_ASSERT_NULL_MESSAGE(ao_spy_take(), "the stop ended the hold twice");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x40U, s_regs[REG_SYSCTRL2] & 0x40U,
                                   "the chip was left out of standby");

    /* And the door is open again. */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, (uint32_t)haptic_hold_request(50U));
    pump(4U);
    haptic_stop_request();
    pump(4U);
    (void)play_finish();
    haptic_down();
}

TEST_CASE("a stop ends a pattern early, once", "[haptic_ao]")
{
    static const uint16_t seg[] = { 400U, 100U, 400U };
    const HapticDoneEvt  *done;

    bus_up(true);
    haptic_up();

    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, (uint32_t)haptic_pattern_request(seg, 3U, 50U));
    pump(4U);
    tick_ms(5U);
    haptic_stop_request();
    pump(4U);
    done = play_finish();
    TEST_ASSERT_TRUE_MESSAGE(SAMPLES_OF(900U) > done->samples,
                             "the pattern played to its end");
    TEST_ASSERT_TRUE(0U < done->samples);
    TEST_ASSERT_NULL(ao_spy_take());
    haptic_down();
}

TEST_CASE("a stop while idle does nothing", "[haptic_ao]")
{
    bus_up(true);
    haptic_up();

    haptic_stop_request();
    pump(4U);
    tick_ms(3U);
    TEST_ASSERT_NULL_MESSAGE(ao_spy_take(), "an idle stop produced an end");
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_regs[REG_PLAYCFG4]);
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)s_fifo_bytes);

    /* A stop that arrives after the buzz is over changes nothing either. */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, (uint32_t)haptic_play_request(50U, 50U));
    pump(4U);
    (void)play_finish();
    haptic_stop_request();
    pump(4U);
    TEST_ASSERT_NULL(ao_spy_take());
    haptic_down();
}

TEST_CASE("a stop during the production row does not end the row's buzz",
          "[haptic_ao]")
{
    const HapticTestEvt *rec;
    const HapticDoneEvt *done;
    const TestResultEvt *verdict;

    bus_up(true);
    haptic_up();

    test_req();
    row_floor((uint16_t)WINDOW_N, FLOOR_X10);
    (void)spy_expect(HAPTIC_TEST_SIG);
    (void)imu_expect(IMU_CAPTURE_REQ_SIG);
    imu_armed(KODE_OK);
    TEST_ASSERT_EQUAL_HEX8(AW_GO, s_regs[REG_PLAYCFG4]);

    haptic_stop_request();
    pump(4U);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(AW_GO, s_regs[REG_PLAYCFG4],
                                   "a stop cut the row's window short");
    done = play_finish();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)BUZZ_SAMPLES, done->samples,
                                     "the row's buzz did not play whole");

    (void)imu_expect(IMU_DRAIN_REQ_SIG);
    imu_energy(KODE_OK, (uint16_t)WINDOW_N, gain_bin_pass(), 400);
    rec = (const HapticTestEvt *)spy_expect(HAPTIC_TEST_SIG);
    TEST_ASSERT_EQUAL_UINT32(HAPTIC_TEST_GAIN, rec->phase);
    verdict = (const TestResultEvt *)spy_expect(TEST_RESULT_SIG);
    TEST_ASSERT_TRUE(verdict->ok);
    haptic_down();
}

TEST_CASE("a pattern or a hold with a bad shape is denied and posts nothing",
          "[haptic_ao]")
{
    static const uint16_t ok_seg[KODE_HAPTIC_SEGMENTS_MAX + 1U] = {
        10U, 10U, 10U, 10U, 10U, 10U, 10U, 10U, 10U, 10U, 10U, 10U, 10U, 10U,
        10U, 10U, 10U
    };
    static const uint16_t zero_seg[] = { 10U, 0U, 10U };

    bus_up(true);
    haptic_up();

    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, (uint32_t)haptic_pattern_request(NULL, 3U, 50U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, (uint32_t)haptic_pattern_request(ok_seg, 0U, 50U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED,
                          (uint32_t)haptic_pattern_request(ok_seg,
                              (uint8_t)(KODE_HAPTIC_SEGMENTS_MAX + 1U), 50U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, (uint32_t)haptic_pattern_request(zero_seg, 3U, 50U));
    queue_empty("a refused pattern was posted");

    /* The most it can hold is not too much. */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK,
                          (uint32_t)haptic_pattern_request(ok_seg,
                              (uint8_t)KODE_HAPTIC_SEGMENTS_MAX, 50U));
    pump(4U);
    (void)play_finish();
    haptic_down();
}

TEST_CASE("a pattern or a hold while a buzz is in flight is denied, nothing posted",
          "[haptic_ao]")
{
    static const uint16_t seg[] = { 10U, 10U };

    bus_up(true);
    haptic_up();

    play_evt((uint16_t)BUZZ_MS, 50U);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, (uint32_t)haptic_pattern_request(seg, 2U, 50U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, (uint32_t)haptic_hold_request(50U));
    queue_empty("a refused request was posted while busy");
    (void)play_finish();

    /* And one while the row has the motor. */
    test_req();
    (void)imu_expect(IMU_CAPTURE_REQ_SIG);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, (uint32_t)haptic_pattern_request(seg, 2U, 50U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, (uint32_t)haptic_hold_request(50U));
    queue_empty("a refused request was posted into the row");
    row_wait(KODE_HAPTIC_REPLY_MS);
    (void)spy_expect(HAPTIC_TEST_SIG);
    (void)spy_expect(TEST_RESULT_SIG);
    haptic_down();
}

TEST_CASE("haptic 0 answers a pattern and a hold once without feeding anything",
          "[haptic_ao]")
{
    static const uint16_t seg[] = { 30U, 20U, 30U };
    const HapticDoneEvt  *done;
    const KodeConfigAnsEvt *a;

    fake_nvs_reset();
    bus_up(true);
    haptic_up();
    a = config_ask(AO_Haptic, &(config_ask_t){ .key = KSET_KEY_HAPTIC, .set = true });
    TEST_ASSERT_NOT_NULL(a);

    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, (uint32_t)haptic_pattern_request(seg, 3U, 60U));
    pump(4U);
    done = (const HapticDoneEvt *)spy_expect(HAPTIC_DONE_SIG);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, (uint32_t)done->rc);
    TEST_ASSERT_EQUAL_UINT32(0U, done->samples);

    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, (uint32_t)haptic_hold_request(60U));
    pump(4U);
    done = (const HapticDoneEvt *)spy_expect(HAPTIC_DONE_SIG);
    TEST_ASSERT_EQUAL_UINT32(0U, done->samples);

    haptic_stop_request();
    pump(4U);
    TEST_ASSERT_NULL(ao_spy_take());
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)s_fifo_bytes);
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_regs[REG_PLAYCFG4]);
    fake_nvs_reset();
    haptic_down();
}

TEST_CASE("two requests posted before the AO runs: the first is taken, the second "
          "denied, and the door reopens after DONE", "[haptic_ao]")
{
    static const uint16_t seg[] = { 30U, 20U, 30U };

    bus_up(true);
    haptic_up();

    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK,
                             (uint32_t)haptic_pattern_request(seg, 3U, 50U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED,
                             (uint32_t)haptic_pattern_request(seg, 3U, 50U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, (uint32_t)haptic_hold_request(50U));
    TEST_ASSERT_EQUAL_UINT32(1U, (uint32_t)uxQueueMessagesWaiting(AO_Haptic->eQueue));

    pump(4U);
    (void)play_finish();
    TEST_ASSERT_NULL(ao_spy_take());
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK,
                             (uint32_t)haptic_pattern_request(seg, 3U, 50U));
    pump(4U);
    (void)play_finish();
    haptic_down();
}

TEST_CASE("an absent board gives the request door back", "[haptic_ao]")
{
    static const uint16_t seg[] = { 30U, 20U, 30U };

    bus_up(false);
    haptic_up();

    /* The AO releases the claim when it has said there is nothing to play. */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK,
                             (uint32_t)haptic_pattern_request(seg, 3U, 50U));
    pump(4U);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, (uint32_t)haptic_hold_request(50U));
    pump(4U);
    haptic_down();
}

TEST_CASE("a second stop right after the first gives one DONE", "[haptic_ao]")
{
    bus_up(true);
    haptic_up();

    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, (uint32_t)haptic_hold_request(50U));
    pump(4U);
    tick_ms(3U);
    haptic_stop_request();
    haptic_stop_request();
    pump(4U);
    (void)play_finish();
    tick_ms(5U);
    TEST_ASSERT_NULL_MESSAGE(ao_spy_take(), "the second stop ended it again");
    haptic_down();
}

TEST_CASE("a stop once the pattern's tail is armed changes nothing and gives one "
          "DONE", "[haptic_ao]")
{
    static const uint16_t seg[] = { 30U, 20U, 30U };
    const HapticDoneEvt  *done;
    uint32_t              i;

    bus_up(true);
    haptic_up();

    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK,
                             (uint32_t)haptic_pattern_request(seg, 3U, 50U));
    pump(4U);
    for ( i = 0U; 100U > i; ++i )
    {
        if ( (SAMPLES_OF(30U) * 2U) + SAMPLES_OF(20U) <= s_fifo_bytes )
        {
            break;
        }
        tick_ms(1U);
    }
    TEST_ASSERT_EQUAL_UINT32((uint32_t)((SAMPLES_OF(30U) * 2U) + SAMPLES_OF(20U)),
                             (uint32_t)s_fifo_bytes);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(AW_GO, s_regs[REG_PLAYCFG4],
                                   "the tail was not armed yet");

    haptic_stop_request();
    pump(1U);
    done = play_finish();
    TEST_ASSERT_EQUAL_UINT32((uint32_t)((SAMPLES_OF(30U) * 2U) + SAMPLES_OF(20U)),
                             done->samples);
    tick_ms(5U);
    TEST_ASSERT_NULL(ao_spy_take());
    haptic_down();
}

/* ---- One stop pending at a time ------------------------------------------- */

/** @brief More stops than any AO queue holds (the host's is 32 deep). */
#define STOPS_MANY 64U

/** @brief Events waiting in AO_Haptic's queue. */
static uint32_t haptic_queued(void)
{
    return (uint32_t)uxQueueMessagesWaiting(AO_Haptic->eQueue);
}

/** @brief Post STOPS_MANY stops unrun: one waits; run it; a new one posts. */
static void stop_cycle(void)
{
    for ( uint32_t i = 0U; STOPS_MANY > i; ++i )
    {
        haptic_stop_request();
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, haptic_queued(), "more than one stop is pending");
    pump(4U);
    TEST_ASSERT_EQUAL_UINT32(0U, haptic_queued());
    haptic_stop_request();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, haptic_queued(), "the dispatched stop left the door shut");
    pump(4U);
}

TEST_CASE("stops past the queue's depth keep one pending while idle", "[haptic_ao]")
{
    bus_up(true);
    haptic_up();
    stop_cycle();
    haptic_down();
}

TEST_CASE("stops past the queue's depth keep one pending on an absent board", "[haptic_ao]")
{
    bus_up(false);
    haptic_up();
    stop_cycle();
    haptic_down();
}

TEST_CASE("stops past the queue's depth keep one pending while a hold plays", "[haptic_ao]")
{
    bus_up(true);
    haptic_up();
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, (uint32_t)haptic_hold_request(50U));
    pump(4U);
    stop_cycle();
    (void)play_finish();
    haptic_down();
}

TEST_CASE("stops past the queue's depth keep one pending during the row", "[haptic_ao]")
{
    const TestResultEvt *verdict;

    bus_up(true);
    haptic_up();
    test_req();
    (void)imu_expect(IMU_CAPTURE_REQ_SIG);
    stop_cycle();
    row_wait(KODE_HAPTIC_REPLY_MS);
    (void)spy_expect(HAPTIC_TEST_SIG);
    verdict = (const TestResultEvt *)spy_expect(TEST_RESULT_SIG);
    TEST_ASSERT_FALSE(verdict->ok);
    haptic_down();
}

/* ---- The motor's state ------------------------------------------------------ */

/** @brief The motor is in @p want. */
static void state_is(haptic_state_t want, const char *why)
{
    TEST_ASSERT_EQUAL_INT32_MESSAGE((int32_t)want, (int32_t)haptic_state(), why);
}

TEST_CASE("the motor's state follows a pattern, a hold and the row from the door", "[haptic_ao]")
{
    const TestResultEvt *verdict;

    bus_up(true);
    haptic_up();
    state_is(HAPTIC_STATE_IDLE, "a fresh AO is not busy");

    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, (uint32_t)haptic_play_request(BUZZ_MS, 50U));
    state_is(HAPTIC_STATE_PLAYING, "the door did not claim a buzz");
    pump(4U);
    state_is(HAPTIC_STATE_PLAYING, "the claim moved when the buzz started");
    (void)play_finish();
    state_is(HAPTIC_STATE_IDLE, "a finished buzz kept the claim");

    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, (uint32_t)haptic_hold_request(50U));
    state_is(HAPTIC_STATE_HOLDING, "the door did not claim a hold");
    pump(4U);
    state_is(HAPTIC_STATE_HOLDING, "the hold is not a hold once it plays");
    haptic_stop_request();
    state_is(HAPTIC_STATE_PLAYING, "a hold told to stop is still called a hold");
    pump(4U);
    (void)play_finish();
    state_is(HAPTIC_STATE_IDLE, "a stopped hold kept the claim");

    test_req();
    state_is(HAPTIC_STATE_PLAYING, "the row did not claim the motor");
    (void)imu_expect(IMU_CAPTURE_REQ_SIG);
    row_wait(KODE_HAPTIC_REPLY_MS);
    (void)spy_expect(HAPTIC_TEST_SIG);
    verdict = (const TestResultEvt *)spy_expect(TEST_RESULT_SIG);
    TEST_ASSERT_FALSE(verdict->ok);
    state_is(HAPTIC_STATE_IDLE, "the row kept the claim");
    haptic_down();
}

TEST_CASE("a hold silenced by haptic 0 is over and the motor is free", "[haptic_ao]")
{
    const KodeConfigAnsEvt *a;

    fake_nvs_reset();
    bus_up(true);
    haptic_up();
    a = config_ask(AO_Haptic, &(config_ask_t){ .key = KSET_KEY_HAPTIC, .set = true });
    TEST_ASSERT_NOT_NULL(a);

    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, (uint32_t)haptic_hold_request(60U));
    state_is(HAPTIC_STATE_HOLDING, "the door did not claim the hold");
    pump(4U);
    (void)spy_expect(HAPTIC_DONE_SIG);
    state_is(HAPTIC_STATE_IDLE, "a hold that never played still counts as held");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, (uint32_t)haptic_hold_request(60U));
    pump(4U);
    fake_nvs_reset();
    haptic_down();
}

TEST_CASE("a hold on a board with no chip is over and the motor is free", "[haptic_ao]")
{
    bus_up(false);
    haptic_up();

    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, (uint32_t)haptic_hold_request(60U));
    state_is(HAPTIC_STATE_HOLDING, "the door did not claim the hold");
    pump(4U);
    state_is(HAPTIC_STATE_IDLE, "an absent board kept the hold");
    haptic_down();
}

TEST_CASE("a hold that cannot start gives the motor back once", "[haptic_ao]")
{
    bus_up(true);
    haptic_up();
    s_acks = false;

    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, (uint32_t)haptic_hold_request(60U));
    pump(4U);
    tick_ms(3U);
    (void)spy_expect(HAPTIC_DONE_SIG);
    state_is(HAPTIC_STATE_IDLE, "a hold that could not start kept the claim");
    s_acks = true;
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, (uint32_t)haptic_hold_request(60U));
    pump(4U);
    state_is(HAPTIC_STATE_HOLDING, "a second release freed the next hold's claim");
    haptic_stop_request();
    pump(4U);
    (void)play_finish();
    haptic_down();
}

TEST_CASE("a row that fails in the tail gives the motor back and takes no more", "[haptic_ao]")
{
    bus_up(true);
    haptic_up();

    test_req();
    row_floor((uint16_t)WINDOW_N, FLOOR_X10);
    (void)spy_expect(HAPTIC_TEST_SIG);
    (void)imu_expect(IMU_CAPTURE_REQ_SIG);
    imu_armed(KODE_OK);
    s_afs_hold = true;
    (void)play_finish();
    (void)spy_expect(HAPTIC_TEST_SIG);
    (void)spy_expect(TEST_RESULT_SIG);
    state_is(HAPTIC_STATE_IDLE, "a failed row kept the motor");

    s_afs_hold = false;
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, (uint32_t)haptic_hold_request(60U));
    pump(4U);
    tick_ms(5U);
    state_is(HAPTIC_STATE_HOLDING, "the failed row freed the next claim as well");
    haptic_stop_request();
    pump(4U);
    (void)play_finish();
    haptic_down();
}

TEST_CASE("a test request behind a claimed play waits for it, then runs", "[haptic_ao]")
{
    const TestResultEvt *verdict;

    bus_up(true);
    haptic_up();

    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, (uint32_t)haptic_play_request(BUZZ_MS, 50U));
    test_req();
    TEST_ASSERT_TRUE_MESSAGE(logged("a buzz is in flight"), "the row did not wait");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, (uint32_t)imu_pending(), "the row started over the play");
    state_is(HAPTIC_STATE_PLAYING, "the waiting row released the play's claim");

    pump(4U);
    (void)play_finish();
    pump(4U);
    /* The play ended and the row it waited for started: it asks for motion. */
    (void)imu_expect(IMU_CAPTURE_REQ_SIG);
    row_wait(KODE_HAPTIC_REPLY_MS);
    (void)spy_expect(HAPTIC_TEST_SIG);
    verdict = (const TestResultEvt *)spy_expect(TEST_RESULT_SIG);
    TEST_ASSERT_FALSE(verdict->ok);
    state_is(HAPTIC_STATE_IDLE, "the row did not end");
    haptic_down();
}

/** @brief Pool blocks the dry-pool case holds. */
#define POOL_HELD_MAX 64U

TEST_CASE("a door that finds the pool dry gives the claim back", "[haptic_ao]")
{
    HapticPlayEvt *held[POOL_HELD_MAX];
    uint32_t       n = 0U;

    bus_up(true);
    haptic_up();

    held[n] = Q_NEW_X(HapticPlayEvt, 0U, HAPTIC_PLAY_REQ_SIG);
    while ( (NULL != held[n]) && ((POOL_HELD_MAX - 1U) > n) )
    {
        n++;
        held[n] = Q_NEW_X(HapticPlayEvt, 0U, HAPTIC_PLAY_REQ_SIG);
    }
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_NO_RESOURCES, (uint32_t)haptic_hold_request(50U));
    state_is(HAPTIC_STATE_IDLE, "a refused request kept the claim");
    while ( 0U < n )
    {
        n--;
        QF_gc(&held[n]->super);
    }
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, (uint32_t)haptic_hold_request(50U));
    haptic_stop_request();
    pump(4U);
    (void)play_finish();
    haptic_down();
}

/* ---- The board's rest ------------------------------------------------------- */

/* Parked is the state between buzzes, so the staged case is a chip something
 * left out of standby; the round parks it and a second one writes nothing. */
TEST_CASE("a round parks the chip in standby and answers once", "[haptic_ao][lowpower]")
{
    bus_up(true);
    haptic_up();
    s_regs[REG_SYSCTRL2] &= (uint8_t)~0x40U;    /* left running */

    lp_round(AO_Haptic, KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x40U, s_regs[REG_SYSCTRL2] & 0x40U,
                                   "the driver stage is live in the rest");
    lp_expect_answer(false, NULL);
    lp_expect_no_more();

    s_regs[REG_SYSCTRL2] &= (uint8_t)~0x40U;
    lp_round(AO_Haptic, KODE_LP_OFF);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x40U, s_regs[REG_SYSCTRL2] & 0x40U, "OFF did not park it");
    lp_expect_answer(false, NULL);
    haptic_down();
}

/* Nothing to restore: the next buzz takes the chip out of standby itself. */
TEST_CASE("after the rest AWAKE writes nothing and the next buzz plays", "[haptic_ao][lowpower]")
{
    bus_up(true);
    haptic_up();

    lp_round(AO_Haptic, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
    s_regs[REG_SYSCTRL2] &= (uint8_t)~0x40U;
    lp_awake(AO_Haptic);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00U, s_regs[REG_SYSCTRL2] & 0x40U, "AWAKE wrote the chip");

    play_evt((uint16_t)BUZZ_MS, 50U);
    TEST_ASSERT_EQUAL_HEX8(AW_GO, s_regs[REG_PLAYCFG4]);
    (void)play_finish();
    TEST_ASSERT_EQUAL_HEX8(0x40U, s_regs[REG_SYSCTRL2] & 0x40U);
    haptic_down();
}

/* A buzz is a promise the caller is waiting on, and the row a measurement. */
TEST_CASE("a buzz in the air or the row refuses the round", "[haptic_ao][lowpower]")
{
    bus_up(true);
    haptic_up();

    play_evt((uint16_t)BUZZ_MS, 50U);
    lp_round(AO_Haptic, KODE_LP_ASLEEP);
    lp_expect_answer(true, "haptic busy");
    lp_expect_no_more();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(AW_GO, s_regs[REG_PLAYCFG4], "the buzz was cut short");
    (void)play_finish();

    test_req();
    (void)imu_expect(IMU_CAPTURE_REQ_SIG);
    lp_round(AO_Haptic, KODE_LP_ASLEEP);
    lp_expect_answer(true, "haptic busy");
    row_wait(KODE_HAPTIC_REPLY_MS);
    (void)spy_expect(HAPTIC_TEST_SIG);
    (void)spy_expect(TEST_RESULT_SIG);
    haptic_down();
}

TEST_CASE("no chip: the round is answered ready at once", "[haptic_ao][lowpower]")
{
    bus_up(false);
    haptic_up();

    lp_round(AO_Haptic, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
    lp_expect_no_more();
    haptic_down();
}

/* ---- A chip that did not answer at boot ----------------------------------- */

/** @brief How many times the bus was asked for the chip. */
static unsigned s_probes;
/** @brief What it answers. */
static bool s_probe_acks;

static bool on_probe(uint16_t addr, uint32_t ms, int calls)
{
    (void)addr;
    (void)ms;
    (void)calls;
    ++s_probes;
    return s_probe_acks;
}

/** @brief Up with no chip on the bus, counting every look. */
static void absent_up(void)
{
    bus_up(false);
    /* The look a case before this one left on the timer goes off here, not in
     * this case's count. */
    (void)qf_support_tick(AO_Haptic, QF_SUPPORT_STEPS(KODE_HAPTIC_RETRY_MS) + 2U);
    s_probes     = 0U;
    s_probe_acks = false;
    khal_i2c_probe_StubWithCallback(&on_probe);
    haptic_up();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_probes, "boot looks once");
}

/** @brief The chip starts answering. */
static void chip_arrives(void)
{
    s_probe_acks = true;
    s_acks       = true;
}

/** @brief Turn the crank past the moment boot said it would look again. */
static void past_the_retry(void)
{
    for ( unsigned i = 0U; ((KODE_HAPTIC_RETRY_MS / TICK_MS) + 2U) > i; ++i )
    {
        pump(4U);
    }
}

TEST_CASE("a chip that missed the boot is found by the look that follows it",
          "[haptic_ao]")
{
    absent_up();
    chip_arrives();
    past_the_retry();
    TEST_ASSERT_EQUAL_UINT(2U, s_probes);
    TEST_ASSERT_TRUE_MESSAGE(logged("answers now"), "the board did not say it buzzes");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00U, s_regs[REG_PLAYCFG4],
                                   "looking for the chip told it to play");
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)haptic_play_request(100U, 50U));
    pump(4U);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0x00U, s_regs[REG_PLAYCFG4], "the buzz was not played");
    (void)play_finish();
    haptic_down();
}

TEST_CASE("a chip that missed both looks before it is found by the first buzz, "
          "which plays", "[haptic_ao]")
{
    absent_up();
    past_the_retry();
    TEST_ASSERT_EQUAL_UINT(2U, s_probes);

    chip_arrives();
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)haptic_play_request(100U, 50U));
    pump(4U);
    TEST_ASSERT_EQUAL_UINT(3U, s_probes);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0x00U, s_regs[REG_PLAYCFG4],
                                  "the buzz that found the chip was not played");
    (void)play_finish();
    state_is(HAPTIC_STATE_IDLE, "the buzz did not give the motor back");
    haptic_down();
}

TEST_CASE("the first buzz finding the chip stops the look still on the timer",
          "[haptic_ao]")
{
    absent_up();
    chip_arrives();
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)haptic_play_request(100U, 50U));
    pump(4U);
    TEST_ASSERT_EQUAL_UINT(2U, s_probes);
    (void)play_finish();
    past_the_retry();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_probes, "a board with its chip was probed again");
    haptic_down();
}

TEST_CASE("a board with no chip looks twice more and then never again",
          "[haptic_ao]")
{
    absent_up();
    past_the_retry();
    for ( unsigned i = 0U; 5U > i; ++i )
    {
        TEST_ASSERT_EQUAL_INT(KODE_OK, (int)haptic_play_request(100U, 50U));
        pump(4U);
    }
    past_the_retry();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(3U, s_probes, "an unfitted board keeps paying for looks");
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_regs[REG_PLAYCFG4]);
    state_is(HAPTIC_STATE_IDLE, "an absent board kept the claim");
    haptic_down();
}

TEST_CASE("a first buzz at haptic=0 that finds the chip ends without playing",
          "[haptic_ao]")
{
    fake_nvs_reset();
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_u8(KSET_NAMESPACE, KSET_KEY_HAPTIC, 0U));
    absent_up();
    chip_arrives();
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)haptic_play_request(100U, 50U));
    pump(4U);
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_regs[REG_PLAYCFG4]);
    state_is(HAPTIC_STATE_IDLE, "a silenced buzz kept the claim");
    fake_nvs_reset();
    haptic_down();
}
