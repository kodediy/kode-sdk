/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_mag.c
 * @brief kode_block_mag_read_raw(): the facade's door onto the magnetometer.
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
#include "kdrv_qmc5883p.h"
#include "kdrv_qmi8658.h"
#include "khal_sync.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kqp.h"
#include "qf_support.h"

/* The magnetometer answers inside the step that receives the request, so this
 * wait covers one conversion period if none is ready when it lands, plus the
 * room a motion read gets. Caught at compile time rather than by a case that
 * could be skipped or forgotten. */
_Static_assert(KODE_SYS_MAG_READ_WAIT_MS >= KODE_SYS_IMU_READ_WAIT_MS,
               "kode_block_mag_read_raw() waits less than a plain motion read, for a "
               "part that may have to be asked twice");

#define REGS_N       256U   /**< Registers of each chip a case stages.        */

#define QMI_WHO_AM_I  0x00U
#define QMI_TEMP_L    0x33U
#define QMI_BURST_LEN 14U

#define QMC_CHIP_ID   0x00U
#define QMC_X_LSB     0x01U
#define QMC_STATUS    0x09U

#define QMC_DRDY      0x01U

/* ---- The two chips, as register files -------------------------------------- */

static uint8_t s_regs[REGS_N];        /* the IMU's control registers        */
static uint8_t s_sample[QMI_BURST_LEN];
static uint8_t s_mag_status;
static int16_t s_mag_field[3];
static bool    s_acks;

/** @brief Write @p v into @p p, low byte first, as both parts report. */
static void le16_put(uint8_t *p, int16_t v)
{
    p[0] = (uint8_t)((uint16_t)v & 0xFFU);
    p[1] = (uint8_t)(((uint16_t)v >> 8) & 0xFFU);
}

/* The two parts share the bus and their maps overlap: a transfer is told by register AND length. */
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
        buf[0] = (uint8_t)KDRV_QMC5883P_CHIP_ID;
    }
    else if ( (QMC_X_LSB == reg) && (6U == len) )
    {
        unsigned ax;

        for ( ax = 0U; 3U > ax; ++ax )
        {
            le16_put(&buf[2U * ax], s_mag_field[ax]);
        }
    }
    else if ( QMC_STATUS == reg )
    {
        buf[0] = s_mag_status;
    }
    else if ( (QMI_WHO_AM_I == reg) && (2U == len) )
    {
        buf[0] = (uint8_t)KDRV_QMI8658_WHOAMI;
        buf[1] = 0x7CU;
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

/** @brief Both parts answering, with a conversion already waiting. */
static void bus_up(void)
{
    Mockkhal_i2c_Init();
    khal_i2c_init_IgnoreAndReturn(KODE_OK);
    khal_i2c_probe_IgnoreAndReturn(true);
    khal_i2c_add_IgnoreAndReturn(KODE_OK);
    khal_i2c_read_reg_StubWithCallback(&on_read);
    khal_i2c_write_reg_StubWithCallback(&on_write);
    khal_i2c_write_regs_StubWithCallback(&on_write_regs);

    (void)memset(s_regs, 0, sizeof(s_regs));
    (void)memset(s_sample, 0, sizeof(s_sample));
    /* One g on z at 4096 LSB per g: the IMU read that shares this claim. */
    le16_put(&s_sample[6], 4096);
    s_acks         = true;
    s_mag_status   = QMC_DRDY;
    s_mag_field[0] = 1500;
    s_mag_field[1] = -1500;
    s_mag_field[2] = 300;
}

/* ---- Letting the owner answer --------------------------------------------- */

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

/**
 * @brief Let AO_Imu answer the wait this call is sitting in.
 *
 * test_kode_motion.c's imu_settle(), and for its reasons: the pump is where
 * the owner runs on a single-threaded host, and the signal afterwards stands
 * in for kode_init(), which is what creates the semaphore the facade lends
 * AO_Imu and which no host case runs.
 */
static void mag_settle(void)
{
    (void)qf_support_pump(AO_Imu, 32U);
    /* An IMU read that found the part resting is held until the gyroscope
     * is on (imu_ao.c's GYRO_READY_MS, 200 ms): the ticks that takes, so
     * the answer is in before the signal. A field read needs none of them. */
    (void)qf_support_tick(AO_Imu, QF_SUPPORT_STEPS(200U) + 2U);
    khal_sem_signal(NULL);
}

/** @brief Arm the settle hook for the next facade call this case makes. */
static void mag_will_settle(void)
{
    fake_sem_wait_hook(&mag_settle);
}

/* ---- Bringing AO_Imu up ---------------------------------------------------- */

/**
 * @brief Register AO_Imu once, and construct it afresh for every case.
 *
 * test_kode_motion.c's imu_up(), and for its reasons.
 */
static void imu_up(void)
{
    qf_support_init();

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
    drain(AO_Imu);
    fake_klog_reset();
    QASM_INIT(AO_Imu, (void *)0, 0U);
    ao_spy_drain();
}

/**
 * @brief Prove the facade's shared claim is free before a case runs.
 *
 * motion_barrier()'s own reason, word for word: a leaked claim is taken by
 * whichever call is in flight and given back when it returns, and the way to
 * lose it is a longjmp out of a hook - which is why no hook in this file ever
 * asserts. One ordinary read, with AO_Imu free to answer it, proves it.
 */
static void mag_barrier(void)
{
    int32_t probe[3] = { 0, 0, 0 };

    mag_will_settle();
    TEST_ASSERT_NOT_EQUAL_INT_MESSAGE(
        KODE_DENIED, kode_block_mag_read_raw(probe),
        "the facade was still dirty when this case began: an earlier case "
        "either left kode_block_mag_read_raw() without releasing its claim, or let one "
        "time out and ended without letting AO_Imu answer it");

    drain(AO_Imu);
    fake_sem_reset();
    fake_time_reset();
}

/** @brief Bring the two chips, AO_Imu and the facade up for one case. */
static void mag_fresh(void)
{
    bus_up();
    imu_up();
    fake_sem_reset();
    fake_time_reset();
    mag_barrier();
}

/* ---- Reading ---------------------------------------------------------------- */

TEST_CASE("kode_block_mag_read_raw fills the caller's axes with the converted field",
          "[kode_mag]")
{
    int32_t mg[3] = { 0, 0, 0 };

    mag_fresh();

    mag_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_mag_read_raw(mg));
    /* The chip's own three axes, one LSB per milligauss at +-30 G. No
     * heading and no calibration: exactly what the part measured. */
    TEST_ASSERT_EQUAL_INT32(1500, mg[0]);
    TEST_ASSERT_EQUAL_INT32(-1500, mg[1]);
    TEST_ASSERT_EQUAL_INT32(300, mg[2]);

    TEST_ASSERT_EQUAL_UINT32(KODE_SYS_MAG_READ_WAIT_MS, fake_sem_last_wait_ms());
}

/* A part that will not convert is not a field of zeroes, and it is not the
 * last field either: the AO answers having written nothing, and the caller's
 * three axes are left exactly as they were. */
TEST_CASE("a part that gives no conversion is answered, and the axes are left "
          "alone", "[kode_mag]")
{
    int32_t mg[3] = { 11, 22, 33 };

    mag_fresh();

    s_mag_status = 0U;
    mag_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kode_block_mag_read_raw(mg));
    TEST_ASSERT_EQUAL_INT32(11, mg[0]);
    TEST_ASSERT_EQUAL_INT32(22, mg[1]);
    TEST_ASSERT_EQUAL_INT32(33, mg[2]);

    /* A bus that says nothing at all is the other answer, and the same
     * silence about the field. */
    s_acks = false;
    mag_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_IO_ERROR, kode_block_mag_read_raw(mg));
    TEST_ASSERT_EQUAL_INT32(11, mg[0]);
    s_acks = true;
}

TEST_CASE("kode_block_mag_read_raw refuses a NULL sample", "[kode_mag]")
{
    mag_fresh();

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_block_mag_read_raw(NULL));
}

/** @brief What a second, concurrent kode_block_mag_read_raw() was answered with. */
static kode_status_t s_second_rc;

/** @brief How many requests AO_Imu held while that second call ran. */
static unsigned s_second_pending;

/**
 * @brief A second kode_block_mag_read_raw(), from inside the first one's own wait.
 *
 * The settle afterwards is what lets the FIRST call finish: the stand-in
 * that would normally do it has been displaced by this one for this wait.
 */
static void mag_read_again_then_settle(void)
{
    static int32_t other[3];

    s_second_rc      = kode_block_mag_read_raw(other);
    s_second_pending = (unsigned)uxQueueMessagesWaiting(AO_Imu->eQueue);
    mag_settle();
}

TEST_CASE("a second concurrent caller is refused KODE_DENIED", "[kode_mag]")
{
    int32_t mg[3] = { 0, 0, 0 };

    mag_fresh();

    s_second_rc      = KODE_OK;
    s_second_pending = 99U;
    fake_sem_wait_hook(&mag_read_again_then_settle);

    /* Asserted, not discarded: a first call that quietly refused itself
     * would leave the second one's refusal proving nothing. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_mag_read_raw(mg));
    TEST_ASSERT_EQUAL_INT32(1500, mg[0]);

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, s_second_rc,
                                  "the second caller is turned away");
    /* And turned away before AO_Imu heard of it: one request outstanding,
     * which is the first caller's own. */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_second_pending,
                                   "the refused caller posted nothing");
}

/* A bounded wait can end before the request it made has been dispatched, and
 * the claim is given back the moment it does. The abandoned request is still
 * owed an answer and the facade's own storage is where it has to land - never
 * a caller's stack frame, kode_block_imu_read_sample()'s own reason. */
TEST_CASE("a wait that times out leaves the caller's axes untouched",
          "[kode_mag]")
{
    int32_t mg[3] = { 11, 22, 33 };

    mag_fresh();

    fake_sem_wait_hook(NULL);
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_mag_read_raw(mg));
    TEST_ASSERT_EQUAL_INT32_MESSAGE(11, mg[0], "a timeout wrote an axis");
    TEST_ASSERT_EQUAL_INT32(22, mg[1]);
    TEST_ASSERT_EQUAL_INT32(33, mg[2]);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(
        1U, (unsigned)uxQueueMessagesWaiting(AO_Imu->eQueue),
        "there was no abandoned request to be late with");

    /* The abandoned request comes out of the queue ahead of the next
     * caller's, into the same storage, status and acknowledgement - so it has
     * to be discarded on its generation rather than answered over the newer
     * one. Here that shows as the newer caller getting its own answer. */
    mag_will_settle();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        KODE_OK, kode_block_mag_read_raw(mg),
        "the abandoned read's late answer took the newer caller's place");
    TEST_ASSERT_EQUAL_INT32(1500, mg[0]);
}

/* ---- One claim across both motion reads ----------------------------------- */

/** @brief What the other motion read, made from inside a wait, answered. */
static kode_status_t s_cross_rc;

/** @brief How many requests AO_Imu held while that other read ran. */
static unsigned s_cross_pending;

/** @brief Whether the refused mag read left its axes exactly as given. */
static bool s_cross_untouched;

/**
 * @brief A kode_block_mag_read_raw() from inside a kode_block_imu_read_sample()'s own wait, then
 *        let AO_Imu answer the IMU read. Records; never asserts.
 */
static void mag_read_inside_imu_read(void)
{
    static int32_t other[3];

    other[0] = 11;
    other[1] = 22;
    other[2] = 33;
    s_cross_rc        = kode_block_mag_read_raw(other);
    s_cross_pending   = (unsigned)uxQueueMessagesWaiting(AO_Imu->eQueue);
    s_cross_untouched = (11 == other[0]) && (22 == other[1]) && (33 == other[2]);
    mag_settle();
}

/**
 * @brief A kode_block_imu_read_sample() from inside a kode_block_mag_read_raw()'s own wait, then let
 *        AO_Imu answer the mag read. Records; never asserts.
 */
static void imu_read_inside_mag_read(void)
{
    static kdrv_qmi8658_sample_t other;

    s_cross_rc      = kode_block_imu_read_sample(&other);
    s_cross_pending = (unsigned)uxQueueMessagesWaiting(AO_Imu->eQueue);
    mag_settle();
}

TEST_CASE("a mag read is refused KODE_DENIED while an IMU read holds the claim",
          "[kode_mag]")
{
    kdrv_qmi8658_sample_t imu;

    mag_fresh();
    s_cross_rc        = KODE_OK;
    s_cross_pending   = 99U;
    s_cross_untouched = false;
    fake_sem_wait_hook(&mag_read_inside_imu_read);

    /* Asserted, not discarded: an IMU read that quietly refused itself would
     * leave the mag read's refusal proving nothing. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_imu_read_sample(&imu));
    TEST_ASSERT_EQUAL_INT32(1000, imu.a_mg[2]);

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, s_cross_rc,
                                  "the mag read was let past the IMU read's claim");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_cross_pending,
                                   "the refused mag read posted something");
    TEST_ASSERT_TRUE_MESSAGE(s_cross_untouched,
                             "the refused mag read wrote its caller's axes");
}

TEST_CASE("an IMU read is refused KODE_DENIED while a mag read holds the claim",
          "[kode_mag]")
{
    int32_t mg[3] = { 0, 0, 0 };

    mag_fresh();
    s_cross_rc      = KODE_OK;
    s_cross_pending = 99U;
    fake_sem_wait_hook(&imu_read_inside_mag_read);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_mag_read_raw(mg));
    TEST_ASSERT_EQUAL_INT32(1500, mg[0]);

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, s_cross_rc,
                                  "the IMU read was let past the mag read's claim");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_cross_pending,
                                   "the refused IMU read posted something");
}

/* ---- What a row does and does not hold ------------------------------------- */

/**
 * @brief One pump of AO_Imu from inside the caller's wait, and nothing more.
 *
 * Whatever is queued, and no ticks: enough for AO_Imu to dispatch the
 * request, far too little for the IMU row to reach its verdict. The semaphore
 * is signalled by hand afterwards, mag_settle()'s own reason, but the facade
 * only takes that for its answer when AO_Imu really acknowledged the request
 * inside this one step.
 */
static void mag_one_step(void)
{
    (void)qf_support_pump(AO_Imu, 4U);
    khal_sem_signal(NULL);
}

/* The IMU row holds the accelerometer's rates and scales for about two
 * seconds. It holds nothing of the magnetometer, which is a chip of its own
 * on the same bus - so a field read during the row is served rather than made
 * to wait for it, while a motion read is refused. Both halves here, because
 * the pair is the point. */
TEST_CASE("a field read during the IMU row is served, and a motion read is "
          "refused", "[kode_mag]")
{
    int32_t               mg[3] = { 0, 0, 0 };
    kdrv_qmi8658_sample_t s;

    mag_fresh();

    imu_test_request();
    (void)qf_support_pump(AO_Imu, 4U);

    fake_sem_wait_hook(&mag_one_step);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        KODE_OK, kode_block_mag_read_raw(mg),
        "the magnetometer was held hostage by the IMU's own row");
    TEST_ASSERT_EQUAL_INT32(1500, mg[0]);

    (void)memset(&s, 0x5A, sizeof(s));
    fake_sem_wait_hook(&mag_one_step);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        KODE_DENIED, kode_block_imu_read_sample(&s),
        "a sample was taken at whatever scale the row had left behind");
    TEST_ASSERT_EQUAL_INT32_MESSAGE(0x5A5A5A5A, s.a_mg[2],
                                    "a refused read wrote the caller's sample");

    /* Run the row out, so the AO is left idle with its one-shot disarmed and
     * the claim comes back clean for whichever case runs next. */
    (void)qf_support_tick(AO_Imu,
                          QF_SUPPORT_STEPS(KODE_IMU_TEST_BUDGET_MS) + 50U);
    fake_sem_reset();
    mag_will_settle();
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_block_mag_read_raw(mg),
                                  "a read after the IMU row was not served");
}

/* ---- The calibration door ---------------------------------------------------- */

#define DOOR_AXES  3U  /**< Axes of the part. */

/** @brief A calibration of the case's own numbers. */
static kdrv_qmc5883p_cal_t door_cal(int32_t offset_x)
{
    kdrv_qmc5883p_cal_t c = { { offset_x, -200, 300 }, { 1000, 1100, 900 } };

    return c;
}

TEST_CASE("the calibration door gets nothing, sets, gets it back and clears", "[kode_mag]")
{
    kdrv_qmc5883p_cal_t const mine = door_cal(100);
    kdrv_qmc5883p_cal_t       got  = door_cal(7);
    bool                      present = true;

    fake_nvs_reset();
    mag_fresh();

    mag_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_mag_cal_get(&got, &present));
    TEST_ASSERT_FALSE(present);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(7, got.offset_mg[0], "nothing stored leaves the calibration alone");

    mag_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_mag_cal_set(&mine));
    TEST_ASSERT_EQUAL_UINT32(KODE_SYS_MAG_CAL_WAIT_MS, fake_sem_last_wait_ms());

    mag_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_mag_cal_get(&got, &present));
    TEST_ASSERT_TRUE(present);
    TEST_ASSERT_EQUAL_INT32_ARRAY(mine.offset_mg, got.offset_mg, DOOR_AXES);
    TEST_ASSERT_EQUAL_INT32_ARRAY(mine.scale_milli, got.scale_milli, DOOR_AXES);

    mag_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_mag_cal_clear());
    mag_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_mag_cal_get(&got, &present));
    TEST_ASSERT_FALSE(present);
    fake_nvs_reset();
}

TEST_CASE("the calibration door refuses NULL and numbers past their limits", "[kode_mag]")
{
    kdrv_qmc5883p_cal_t const over = door_cal(KDRV_QMC5883P_CAL_OFFSET_MAX_MG + 1);
    kdrv_qmc5883p_cal_t       got;
    bool                      present;

    fake_nvs_reset();
    mag_fresh();

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_block_mag_cal_get(NULL, &present));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_block_mag_cal_get(&got, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_block_mag_cal_set(NULL));
    mag_will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_block_mag_cal_set(&over));
    fake_nvs_reset();
}

TEST_CASE("a calibration wait that times out is KODE_TIMEOUT and the next call is answered",
          "[kode_mag]")
{
    kdrv_qmc5883p_cal_t got;
    bool                present = true;

    fake_nvs_reset();
    mag_fresh();

    fake_sem_wait_hook(NULL);
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_mag_cal_get(&got, &present));

    mag_will_settle();
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_block_mag_cal_get(&got, &present),
                                  "the abandoned request took the newer caller's place");
    TEST_ASSERT_FALSE(present);
    fake_nvs_reset();
}

/** @brief What a calibration get made from inside a field read's wait answered. */
static kode_status_t s_door_rc;

/** @brief A calibration get from inside a kode_block_mag_read_raw()'s wait, then let AO_Imu answer. */
static void cal_inside_mag_read(void)
{
    static kdrv_qmc5883p_cal_t other;
    bool                       present = false;

    s_door_rc = kode_block_mag_cal_get(&other, &present);
    mag_settle();
}

TEST_CASE("a calibration get is refused KODE_DENIED while a field read holds the claim",
          "[kode_mag]")
{
    int32_t mg[3] = { 0, 0, 0 };

    mag_fresh();

    s_door_rc = KODE_OK;
    fake_sem_wait_hook(&cal_inside_mag_read);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_mag_read_raw(mg));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, s_door_rc);
}

/* ---- A full queue ------------------------------------------------------------ */

/** @brief A filler AO_Imu ignores. */
static const QEvt s_filler = QEVT_INITIALIZER(MAG_TEST_REQ_SIG);

TEST_CASE("a field read or a calibration call is refused KODE_DENIED by a full queue and frees the claim",
          "[kode_mag]")
{
    kdrv_qmc5883p_cal_t corr;
    bool                present;
    int32_t             mg[3] = { 11, 22, 33 };

    fake_nvs_reset();
    mag_fresh();

    qf_support_margin_queue(AO_Imu);
    TEST_ASSERT_TRUE_MESSAGE(qf_support_queue_stuff(AO_Imu, &s_filler), "the queue never filled");
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_block_mag_read_raw(mg));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_block_mag_cal_get(&corr, &present));
    TEST_ASSERT_EQUAL_INT32(11, mg[0]);

    drain(AO_Imu);
    mag_will_settle();
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_block_mag_cal_get(&corr, &present),
                                  "the refused call kept the claim");
    fake_nvs_reset();
}
