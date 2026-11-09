/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_motion.c
 * @brief kode_block_imu_read_sample() and the kode_sys_haptic_* calls: the facade's motion doors.
 */
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "qpc.h"
#include "Mockkhal_i2c.h"

#include "ao_spy.h"
#include "fakes.h"
#include "kaos.h"
#include "kdrv_qmi8658.h"
#include "khal_sync.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kqp.h"
#include "motion_support.h"
#include "qf_support.h"

#define QMI_WHO_AM_I  0x00U
#define QMI_TEMP_L    0x33U
#define QMC_CHIP_ID   0x00U

/** @brief TEMP_L, TEMP_H, six bytes of acceleration, six of rotation. */
#define QMI_BURST_LEN 14U

/** @brief The sample's scale: 4096 LSB per g at +-8 g. */
#define LSB_PER_G     4096
/** @brief And 64 LSB per degree per second at +-512 dps. */
#define LSB_PER_DPS   64
/** @brief The die temperature is 8.8 fixed point degrees. */
#define LSB_PER_C     256

/** @brief What the staged sample reads on z, in milli-g: the board flat.
 *         A whole number of LSB in both directions (4096 per g), so it reads
 *         back exactly and a case is about the answer rather than about
 *         where two truncations landed. */
#define REST_AZ_MG    1000

/* ---- The chip, as a register file ---------------------------------------- */

/** @brief Registers of each chip a case stages. */
#define REGS_N 256U

static uint8_t s_regs[REGS_N];
static uint8_t s_sample[QMI_BURST_LEN];
static bool    s_acks;

/** @brief Write @p v into @p p, low byte first, as the part reports. */
static void le16_put(uint8_t *p, int16_t v)
{
    p[0] = (uint8_t)((uint16_t)v & 0xFFU);
    p[1] = (uint8_t)(((uint16_t)v >> 8) & 0xFFU);
}

/** @brief Stage the sample one read burst answers, in the caller's units. */
static void stage_sample(const int32_t a_mg[3], const int32_t w_mdps[3],
                         int32_t temp_dc)
{
    uint32_t i;

    le16_put(&s_sample[0], (int16_t)((temp_dc * LSB_PER_C) / 10));
    for ( i = 0U; 3U > i; ++i )
    {
        le16_put(&s_sample[2U + (2U * i)],
                 (int16_t)((a_mg[i] * LSB_PER_G) / 1000));
        le16_put(&s_sample[8U + (2U * i)],
                 (int16_t)((w_mdps[i] * LSB_PER_DPS) / 1000));
    }
}

/**
 * @brief One transfer of the two parts that share this mocked bus.
 *
 * test_imu_ao.c's rule, cut to what this file needs: only the magnetometer
 * reads one byte at 0x00 and only the IMU reads two, so the chip id and the
 * WHO_AM_I do not collide even though they live at the same address. Nothing
 * else here is read by both.
 */
static kode_status_t on_read(khal_i2c_dev_t dev, uint8_t reg, uint8_t *buf,
                             size_t len, int calls)
{
    (void)dev;
    (void)calls;
    if ( !s_acks )
    {
        return KODE_ERROR;
    }
    if ( (QMC_CHIP_ID == reg) && (1U == len) )
    {
        buf[0] = 0x80U;   /* the QMC5883P's, so a bring-up that runs succeeds */
    }
    else if ( (QMI_TEMP_L == reg) && (QMI_BURST_LEN == len) )
    {
        (void)memcpy(buf, s_sample, sizeof(s_sample));
    }
    else
    {
        (void)memcpy(buf, &s_regs[reg], len);
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
    s_regs[reg] = val;
    return KODE_OK;
}

static kode_status_t on_write_regs(khal_i2c_dev_t dev, uint8_t reg,
                                   const uint8_t *buf, size_t len, int calls)
{
    (void)dev;
    (void)calls;
    if ( !s_acks )
    {
        return KODE_ERROR;
    }
    (void)memcpy(&s_regs[reg], buf, len);
    return KODE_OK;
}

/** @brief A part that answers, lying flat: z reads about one g. */
static void bus_up(void)
{
    int32_t const a[3] = { 125, -250, REST_AZ_MG };
    int32_t const w[3] = { 0, 0, 0 };

    Mockkhal_i2c_Init();
    khal_i2c_init_IgnoreAndReturn(KODE_OK);
    khal_i2c_probe_IgnoreAndReturn(true);
    khal_i2c_add_IgnoreAndReturn(KODE_OK);
    khal_i2c_read_reg_StubWithCallback(&on_read);
    khal_i2c_write_reg_StubWithCallback(&on_write);
    khal_i2c_write_regs_StubWithCallback(&on_write_regs);

    (void)memset(s_regs, 0, sizeof(s_regs));
    s_regs[QMI_WHO_AM_I]      = (uint8_t)KDRV_QMI8658_WHOAMI;
    s_regs[QMI_WHO_AM_I + 1U] = 0x7CU;
    s_acks = true;
    stage_sample(a, w, 265);
}

/* ---- Letting the owner answer --------------------------------------------- */

/** @brief The ticks a read that woke the IMU is held for, and two more:
 *         imu_ao.c's GYRO_READY_MS, repeated here because it is private. */
#define IMU_WAKE_TICKS  (QF_SUPPORT_STEPS(200U) + 2U)

/**
 * @brief Let AO_Imu answer the wait this call is sitting in.
 *
 * Generously, per AO_Imu's own shape: a read of a part that is awake is
 * answered inside the step that receives it, but the queue it lands in may already hold an abandoned
 * request from whatever ran before, and a pump that stopped at one event
 * would answer that instead.
 *
 * The signal afterwards stands in for kode_init() and for nothing else: that
 * is what creates the semaphore the facade lends AO_Imu, no host case runs
 * it, and AO_Imu does not signal a semaphore it was handed as NULL.
 * Everything else here is the real thing - AO_Imu writes the sample through
 * the pointer the request actually carried, in its own step.
 */
static void imu_settle(void)
{
    (void)qf_support_pump(AO_Imu, 32U);
    /* A read that found the IMU resting is held until the gyroscope is on:
     * the ticks that turn-on takes, so the answer is in before the signal. */
    (void)qf_support_tick(AO_Imu, IMU_WAKE_TICKS);
    khal_sem_signal(NULL);
}

/** @brief Arm the settle hook for the next facade call this case makes. */
static void imu_will_settle(void)
{
    fake_sem_wait_hook(&imu_settle);
}

/* ---- Bringing the owners up ------------------------------------------------ */

/**
 * @brief Register AO_Imu once, and construct it afresh for every case.
 *
 * test_imu_ao.c's imu_up(), and for its reasons: the registration cannot be
 * repeated because QActive_register_() asserts on a priority slot already
 * taken, and the construction has to be, because QASM_INIT() insists the
 * machine is sitting at QHsm_top.
 *
 * The claim flag lives in qf_support because AO_Imu is brought up by more
 * than one file.
 */
static void imu_up(void)
{
    qf_support_init();

    /* The tick first, test_haptic_ao.c's rule: QP takes a DISARMED QTimeEvt
     * out of its list only inside the tick, so a case that ended on a disarm
     * leaves it linked, and QTimeEvt_ctorX() over a linked one rewrites the
     * link QF walks. */
    (void)qf_support_tick(AO_Imu, QF_SUPPORT_STEPS(KODE_SYS_IMU_REST_AFTER_MS) + 10U);
    (void)ao_spy();

    Imu_ctor();
    qf_support_give_queue(AO_Imu);
    AO_Imu->prio  = KODE_PRIO_IMU;
    AO_Imu->pthre = KODE_PRIO_IMU;
    if ( qf_support_claim_ao(AO_Imu) )
    {
        QActive_register_(AO_Imu);
    }
    motion_support_drain(AO_Imu);
    fake_klog_reset();
    QASM_INIT(AO_Imu, (void *)0, 0U);
    ao_spy_drain();
}

/**
 * @brief Prove the facade's claim is free before a case runs.
 *
 * A leaked claim is the one failure that spreads: it is taken by whichever
 * call is in flight and given back when it returns, and the way to lose it is
 * a longjmp out of a hook, which is why no hook in this file ever asserts.
 * An abandoned request is the other half - a case that let a read time out
 * and ended without letting AO_Imu answer it leaves a request holding
 * pointers into kode_sys_imu.c's own storage.
 *
 * So: one ordinary read, with AO_Imu free to answer it. Anything but
 * KODE_DENIED means the claim was there to take, which is the whole question.
 * Naming it here - at the top of the FIRST case after the damage - is worth
 * more than the cascade of unexplained refusals it would otherwise become.
 */
static void motion_barrier(void)
{
    kdrv_qmi8658_sample_t probe;

    imu_will_settle();
    TEST_ASSERT_NOT_EQUAL_INT_MESSAGE(
        KODE_DENIED, kode_block_imu_read_sample(&probe),
        "the facade was still dirty when this case began: an earlier case "
        "either left kode_block_imu_read_sample() without releasing its claim, or let one "
        "time out and ended without letting AO_Imu answer it");

    motion_support_drain(AO_Imu);
    fake_sem_reset();
    fake_time_reset();
}

/** @brief Bring the chip, AO_Imu and the facade up for one case. */
static void motion_fresh(void)
{
    bus_up();
    imu_up();
    fake_sem_reset();
    fake_time_reset();
    motion_barrier();
}

/* ---- Reading ---------------------------------------------------------------- */

TEST_CASE("kode_block_imu_read_sample fills the caller's sample from the chip", "[kode_motion]")
{
    kdrv_qmi8658_sample_t s;

    motion_fresh();

    (void)memset(&s, 0, sizeof(s));
    imu_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_imu_read_sample(&s));
    /* The staged burst, through the driver's own scales: this can only pass
     * if AO_Imu genuinely read the part and the facade genuinely copied the
     * answer out into the caller's struct. */
    TEST_ASSERT_EQUAL_INT32(REST_AZ_MG, s.a_mg[2]);
    TEST_ASSERT_EQUAL_INT32(265, s.temp_dc);

    /* The deadline the facade hands the fake: the part is left running, so a
     * read is one I2C burst inside the step that receives it, and this covers
     * that with room for a queue in front of it. */
    TEST_ASSERT_EQUAL_UINT32(KODE_SYS_IMU_READ_WAIT_MS, fake_sem_last_wait_ms());
}

/* A part that will not talk is not a reading of zero: the AO answers
 * KODE_IO_ERROR having written nothing, and the caller's struct is left
 * exactly as it was. (The QMI8658A cannot be made ABSENT here - the driver
 * comes up once per binary and test_imu_ao.c holds that moment - so what is
 * staged is the bus under it, which reaches the same answer by the same
 * door.) */
TEST_CASE("kode_block_imu_read_sample on a part that does not answer is KODE_IO_ERROR with "
          "the sample untouched", "[kode_motion]")
{
    kdrv_qmi8658_sample_t s;

    motion_fresh();

    (void)memset(&s, 0, sizeof(s));
    s.a_mg[2] = 1234;

    s_acks = false;
    imu_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_IO_ERROR, kode_block_imu_read_sample(&s));
    TEST_ASSERT_EQUAL_INT32(1234, s.a_mg[2]);
    s_acks = true;
}

TEST_CASE("kode_block_imu_read_sample refuses a NULL sample", "[kode_motion]")
{
    motion_fresh();

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_block_imu_read_sample(NULL));
}

/** @brief What a second, concurrent kode_block_imu_read_sample() was answered with. */
static kode_status_t s_second_rc;

/** @brief How many requests AO_Imu held while that second call ran. */
static uint32_t s_second_pending;

/**
 * @brief A second kode_block_imu_read_sample(), from inside the first one's own wait.
 *
 * The pump afterwards is what lets the FIRST call finish: the stand-in that
 * would normally do it has been displaced by this one for this wait.
 */
static void read_again_then_settle(void)
{
    static kdrv_qmi8658_sample_t other;

    s_second_rc      = kode_block_imu_read_sample(&other);
    s_second_pending = (uint32_t)uxQueueMessagesWaiting(AO_Imu->eQueue);
    imu_settle();
}

TEST_CASE("a second caller is refused while the first is asleep", "[kode_motion]")
{
    kdrv_qmi8658_sample_t s;

    motion_fresh();

    (void)memset(&s, 0, sizeof(s));
    s_second_rc      = KODE_OK;
    s_second_pending = 99U;
    fake_sem_wait_hook(&read_again_then_settle);

    /* Asserted, not discarded: a first call that quietly refused itself would
     * leave the second one's refusal proving nothing. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_imu_read_sample(&s));
    TEST_ASSERT_EQUAL_INT32(REST_AZ_MG, s.a_mg[2]);

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, s_second_rc,
                                  "the second caller is turned away");
    /* And turned away before AO_Imu heard of it: one request outstanding,
     * which is the first caller's own. A refusal that had already posted
     * would leave the owner answering into storage nobody holds. */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_second_pending,
                                   "the refused caller posted nothing");
}

TEST_CASE("an owner that never answers is a timeout, and the claim is released",
          "[kode_motion]")
{
    kdrv_qmi8658_sample_t s;

    motion_fresh();

    (void)memset(&s, 0, sizeof(s));
    fake_sem_wait_hook(NULL);
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_imu_read_sample(&s));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s.a_mg[2],
                                  "nothing is copied out of an answer that "
                                  "never arrived");

    /* The request is still in AO_Imu's queue, owed an answer. The claim is
     * not: the next caller gets in, and the abandoned answer landing first is
     * discarded by its generation rather than handed to them. */
    imu_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_imu_read_sample(&s));
    TEST_ASSERT_EQUAL_INT32(REST_AZ_MG, s.a_mg[2]);
}

/* A bounded wait can end before the request it made has been dispatched, and
 * the claim is given back the moment it does. Everything AO_Imu still holds a
 * pointer to at that instant has to be memory the facade owns: on the board
 * the caller's struct is a stack frame that has returned, and a sample
 * arriving in it a few milliseconds later would corrupt whatever took its
 * place, silently and nowhere near here. */
TEST_CASE("a late answer lands in the facade's own storage, never in the caller's",
          "[kode_motion]")
{
    kdrv_qmi8658_sample_t s;
    kdrv_qmi8658_sample_t sentinel;

    motion_fresh();

    /* A sentinel in every byte, so a write of any width through a stale
     * pointer shows up rather than happening to match. */
    (void)memset(&s, 0x5A, sizeof(s));
    (void)memset(&sentinel, 0x5A, sizeof(sentinel));

    fake_sem_wait_hook(NULL);
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_imu_read_sample(&s));
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&sentinel, &s, sizeof(s),
                                     "a timeout writes nothing at all");

    /* The request is still there, undispatched and owed an answer. This is
     * the state the case is about: on the board the caller's frame is gone by
     * now, and only the host lets anyone look at it afterwards. */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(
        1U, (uint32_t)uxQueueMessagesWaiting(AO_Imu->eQueue),
        "there was no abandoned request to be late with");

    imu_settle();

    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(
        &sentinel, &s, sizeof(s),
        "AO_Imu answered an abandoned request into the caller's sample: on "
        "the board that address is a stack frame that has returned");

    /* And the late answer is not handed to whoever comes next either - its
     * generation is not theirs. */
    imu_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_imu_read_sample(&s));
    TEST_ASSERT_EQUAL_INT32(REST_AZ_MG, s.a_mg[2]);
}

/* The abandoned request is still queued when the NEXT caller takes the claim,
 * and it comes out of the queue first - ahead of the newer one, into the same
 * sample, status and acknowledgement. So it has to be discarded on its
 * generation rather than answered: on two cores it would otherwise land
 * between the newer caller waking and copying its sample out. On this host,
 * where nothing interleaves, the overwrite would show as the newer caller's
 * own answer going missing under the abandoned read's acknowledgement. */
TEST_CASE("an abandoned read dispatched after a newer one never overwrites its "
          "answer", "[kode_motion]")
{
    kdrv_qmi8658_sample_t first;
    kdrv_qmi8658_sample_t second;

    motion_fresh();

    (void)memset(&first, 0x5A, sizeof(first));
    fake_sem_wait_hook(NULL);
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_imu_read_sample(&first));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(
        1U, (uint32_t)uxQueueMessagesWaiting(AO_Imu->eQueue),
        "the abandoned read left nothing behind to be late with");

    fake_klog_reset();
    (void)memset(&second, 0, sizeof(second));
    imu_will_settle();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        KODE_OK, kode_block_imu_read_sample(&second),
        "the abandoned read's late answer took the newer caller's place");
    TEST_ASSERT_EQUAL_INT32(REST_AZ_MG, second.a_mg[2]);
}

/* ---- Buzzing ---------------------------------------------------------------- */

TEST_CASE("kode_sys_haptic_play_buzz refuses out-of-range and posts otherwise",
          "[kode_motion]")
{
    const HapticPlayEvt *e = NULL;

    motion_support_haptic_up();

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, kode_sys_haptic_play_buzz(0U, 50U),
                                  "a buzz of no length is not a buzz");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        KODE_DENIED, kode_sys_haptic_play_buzz((uint16_t)(KODE_SYS_HAPTIC_MAX_MS + 1U), 50U),
        "past the ceiling the motor is held at, not truncated to it");
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, kode_sys_haptic_play_buzz(200U, 101U),
                                  "a gain over full scale is refused");

    /* Refused at the door, so AO_Haptic has heard nothing at all. */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(
        0U, (uint32_t)uxQueueMessagesWaiting(AO_Haptic->eQueue),
        "a refused buzz posted nothing");

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_haptic_play_buzz(200U, 50U));
    TEST_ASSERT_EQUAL_UINT(
        1U, (uint32_t)uxQueueMessagesWaiting(AO_Haptic->eQueue));

    TEST_ASSERT_TRUE(xQueueReceive(AO_Haptic->eQueue, &e, 0U) == pdTRUE);
    TEST_ASSERT_EQUAL_INT32((int32_t)HAPTIC_PLAY_REQ_SIG, (int32_t)e->super.sig);
    TEST_ASSERT_EQUAL_UINT(1U, e->n);
    TEST_ASSERT_EQUAL_UINT(200U, e->seg_ms[0]);
    TEST_ASSERT_FALSE(e->hold);
    TEST_ASSERT_EQUAL_UINT(50U, e->gain_pct);
    QF_gc(&e->super);

    motion_support_drain(AO_Haptic);
}

/* ---- Patterns, holds and stops ------------------------------------------ */

/** @brief Nothing is waiting in AO_Haptic's queue. */
static void haptic_queue_empty(const char *msg)
{
    TEST_ASSERT_EQUAL_UINT_MESSAGE(
        0U, (uint32_t)uxQueueMessagesWaiting(AO_Haptic->eQueue), msg);
}

TEST_CASE("kode_sys_haptic_play_pattern refuses a bad shape and posts otherwise",
          "[kode_motion]")
{
    static const uint16_t seg[]      = { 30U, 20U, 40U };
    static const uint16_t zero_seg[] = { 30U, 0U, 40U };
    uint16_t              many[KODE_SYS_HAPTIC_SEGMENTS_MAX + 1U];
    const HapticPlayEvt  *e = NULL;
    uint8_t               i;

    motion_support_haptic_up();
    for ( i = 0U; (uint8_t)(KODE_SYS_HAPTIC_SEGMENTS_MAX + 1U) > i; ++i )
    {
        many[i] = 10U;
    }

    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED,
                             (uint32_t)kode_sys_haptic_play_pattern(NULL, 3U, 50U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED,
                             (uint32_t)kode_sys_haptic_play_pattern(seg, 0U, 50U));
    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)KODE_DENIED,
        (uint32_t)kode_sys_haptic_play_pattern(many, (uint8_t)(KODE_SYS_HAPTIC_SEGMENTS_MAX + 1U), 50U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED,
                             (uint32_t)kode_sys_haptic_play_pattern(zero_seg, 3U, 50U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED,
                             (uint32_t)kode_sys_haptic_play_pattern(seg, 3U, 101U));
    haptic_queue_empty("a refused pattern posted something");

    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK,
                             (uint32_t)kode_sys_haptic_play_pattern(seg, 3U, 70U));
    TEST_ASSERT_EQUAL_UINT(1U, (uint32_t)uxQueueMessagesWaiting(AO_Haptic->eQueue));
    TEST_ASSERT_TRUE(xQueueReceive(AO_Haptic->eQueue, &e, 0U) == pdTRUE);
    TEST_ASSERT_EQUAL_INT((int32_t)HAPTIC_PLAY_REQ_SIG, (int32_t)e->super.sig);
    TEST_ASSERT_EQUAL_UINT(3U, e->n);
    TEST_ASSERT_EQUAL_UINT(30U, e->seg_ms[0]);
    TEST_ASSERT_EQUAL_UINT(20U, e->seg_ms[1]);
    TEST_ASSERT_EQUAL_UINT(40U, e->seg_ms[2]);
    TEST_ASSERT_EQUAL_UINT(70U, e->gain_pct);
    TEST_ASSERT_FALSE(e->hold);
    QF_gc(&e->super);

    motion_support_drain(AO_Haptic);
}

TEST_CASE("kode_sys_haptic_play_pattern takes the most segments there are",
          "[kode_motion]")
{
    uint16_t             full[KODE_SYS_HAPTIC_SEGMENTS_MAX];
    const HapticPlayEvt *e = NULL;
    uint8_t              i;

    motion_support_haptic_up();
    for ( i = 0U; (uint8_t)KODE_SYS_HAPTIC_SEGMENTS_MAX > i; ++i )
    {
        full[i] = (uint16_t)(10U + i);
    }
    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)KODE_OK,
        (uint32_t)kode_sys_haptic_play_pattern(full, (uint8_t)KODE_SYS_HAPTIC_SEGMENTS_MAX, 100U));
    TEST_ASSERT_TRUE(xQueueReceive(AO_Haptic->eQueue, &e, 0U) == pdTRUE);
    TEST_ASSERT_EQUAL_UINT(KODE_SYS_HAPTIC_SEGMENTS_MAX, e->n);
    TEST_ASSERT_EQUAL_UINT(10U, e->seg_ms[0]);
    TEST_ASSERT_EQUAL_UINT((uint32_t)(10U + KODE_SYS_HAPTIC_SEGMENTS_MAX - 1U),
                           e->seg_ms[KODE_SYS_HAPTIC_SEGMENTS_MAX - 1U]);
    QF_gc(&e->super);

    motion_support_drain(AO_Haptic);
}

TEST_CASE("kode_sys_haptic_hold refuses a gain over full scale and posts a hold",
          "[kode_motion]")
{
    const HapticPlayEvt *e = NULL;

    motion_support_haptic_up();

    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, (uint32_t)kode_sys_haptic_hold(101U));
    haptic_queue_empty("a refused hold posted something");

    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, (uint32_t)kode_sys_haptic_hold(40U));
    TEST_ASSERT_EQUAL_UINT(1U, (uint32_t)uxQueueMessagesWaiting(AO_Haptic->eQueue));
    TEST_ASSERT_TRUE(xQueueReceive(AO_Haptic->eQueue, &e, 0U) == pdTRUE);
    TEST_ASSERT_EQUAL_INT((int32_t)HAPTIC_PLAY_REQ_SIG, (int32_t)e->super.sig);
    TEST_ASSERT_TRUE(e->hold);
    TEST_ASSERT_EQUAL_UINT(40U, e->gain_pct);
    QF_gc(&e->super);

    motion_support_drain(AO_Haptic);
}

TEST_CASE("a second pattern or hold before the first ends is denied, nothing posted",
          "[kode_motion]")
{
    static const uint16_t seg[] = { 30U, 20U };

    motion_support_haptic_up();

    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, (uint32_t)kode_sys_haptic_hold(40U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED,
                             (uint32_t)kode_sys_haptic_play_pattern(seg, 2U, 40U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, (uint32_t)kode_sys_haptic_hold(40U));
    TEST_ASSERT_EQUAL_UINT(1U, (uint32_t)uxQueueMessagesWaiting(AO_Haptic->eQueue));

    motion_support_drain(AO_Haptic);
}

TEST_CASE("kode_sys_haptic_is_holding is true from a hold's door until the stop", "[kode_motion]")
{
    static const uint16_t seg[] = { 30U, 20U };

    motion_support_haptic_up();
    TEST_ASSERT_FALSE(kode_sys_haptic_is_holding());

    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, (uint32_t)kode_sys_haptic_hold(40U));
    TEST_ASSERT_TRUE(kode_sys_haptic_is_holding());
    kode_sys_haptic_stop();
    TEST_ASSERT_FALSE_MESSAGE(kode_sys_haptic_is_holding(), "a hold told to stop is still a hold");

    motion_support_haptic_up();
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK,
                             (uint32_t)kode_sys_haptic_play_pattern(seg, 2U, 40U));
    TEST_ASSERT_FALSE_MESSAGE(kode_sys_haptic_is_holding(), "a pattern is not a hold");
    motion_support_drain(AO_Haptic);
}

TEST_CASE("kode_sys_haptic_stop posts the stop event", "[kode_motion]")
{
    const QEvt *e = NULL;

    motion_support_haptic_up();

    kode_sys_haptic_stop();
    TEST_ASSERT_EQUAL_UINT(1U, (uint32_t)uxQueueMessagesWaiting(AO_Haptic->eQueue));
    TEST_ASSERT_TRUE(xQueueReceive(AO_Haptic->eQueue, &e, 0U) == pdTRUE);
    TEST_ASSERT_EQUAL_INT((int32_t)HAPTIC_STOP_REQ_SIG, (int32_t)e->sig);

    motion_support_drain(AO_Haptic);
}
