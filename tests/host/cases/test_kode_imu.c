/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_imu.c
 * @brief kode_imu_read_axis(), kode_imu_read(), kode_mag_read_axis() and kode_mag_read().
 */
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "qpc.h"
#include "Mockkhal_i2c.h"

#include "ao_spy.h"
#include "fakes.h"
#include "kaos.h"
#include "kdrv_qmc5883p.h"
#include "kdrv_qmi8658.h"
#include "khal_sync.h"
#include "khal_system.h"
#include "kode.h"
#include "kode_entry.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kode_sys_priv.h"
#include "kqp.h"
#include "qf_support.h"

#define REGS_N       256U   /**< Registers of each chip a case stages.        */

#define QMI_WHO_AM_I  0x00U  /**< The IMU's id register, read two bytes.      */
#define QMI_TEMP_L    0x33U  /**< The IMU's sample burst.                      */
#define QMI_BURST_LEN 14U    /**< Temperature, acceleration, rotation.         */
#define QMI_REVISION  0x7CU  /**< What the part reports as its revision.       */

#define QMC_CHIP_ID   0x00U  /**< The magnetometer's id register, one byte.    */
#define QMC_X_LSB     0x01U  /**< Its field burst, six bytes.                  */
#define QMC_STATUS    0x09U  /**< Its status register.                         */
#define QMC_DRDY      0x01U  /**< A conversion is ready.                       */

#define LSB_PER_G     4096   /**< The sample's acceleration scale.             */
#define LSB_PER_DPS   64     /**< Its rotation scale.                          */
#define LSB_PER_C     256    /**< Its temperature scale, 8.8 fixed point.      */
#define DECI_PER_C    10     /**< Tenths of a degree in a degree.              */
#define MILLI_PER_UNIT 1000  /**< Milli-units in a unit.                       */

#define OFFSET_ACCEL  2U     /**< Where acceleration starts in the burst.      */
#define OFFSET_GYRO   8U     /**< Where rotation starts in the burst.          */
#define BYTES_PER_AXIS 2U    /**< Bytes of one axis.                           */
#define AXES          3U     /**< Axes of each sensor.                         */

#define TOL           0.001F /**< Tolerance of every float comparison.         */
#define SEVEN         7U     /**< The IMU's readings.                          */
#define ERROR_COPY    KODE_ERROR_MAX /**< Bytes of a copied error text.        */

/** @brief The tick the IMU read waits for, from AO_Imu's own gyroscope turn-on. */
#define IMU_WAKE_TICKS (QF_SUPPORT_STEPS(200U) + 2U)

/** @brief Ticks that run a witness window past its deadline. */
#define WINDOW_END_TICKS (QF_SUPPORT_STEPS(KODE_IMU_CAPTURE_MAX_MS) + 4U)

/* ---- The two chips, as register files -------------------------------------- */

static uint8_t s_regs[REGS_N];
static uint8_t s_sample[QMI_BURST_LEN];
static uint8_t s_mag_status;
static int16_t s_mag_field[AXES];
static bool    s_acks;

/** @brief Write @p v into @p p, low byte first, as both parts report. */
static void le16_put(uint8_t *p, int16_t v)
{
    p[0] = (uint8_t)((uint16_t)v & 0xFFU);
    p[1] = (uint8_t)(((uint16_t)v >> 8) & 0xFFU);
}

/** @brief Stage the sample the next IMU burst answers, in the layer's units. */
static void stage_sample(const int32_t a_mg[AXES], const int32_t w_mdps[AXES], int32_t temp_dc)
{
    le16_put(&s_sample[0], (int16_t)((temp_dc * LSB_PER_C) / DECI_PER_C));
    for ( uint32_t i = 0U; AXES > i; i++ )
    {
        le16_put(&s_sample[OFFSET_ACCEL + (BYTES_PER_AXIS * i)],
                 (int16_t)((a_mg[i] * LSB_PER_G) / MILLI_PER_UNIT));
        le16_put(&s_sample[OFFSET_GYRO + (BYTES_PER_AXIS * i)],
                 (int16_t)((w_mdps[i] * LSB_PER_DPS) / MILLI_PER_UNIT));
    }
}

static kode_status_t on_read(khal_i2c_dev_t dev, uint8_t reg, uint8_t *buf, size_t len, int calls)
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
    else if ( (QMC_X_LSB == reg) && ((BYTES_PER_AXIS * AXES) == len) )
    {
        for ( uint32_t ax = 0U; AXES > ax; ax++ )
        {
            le16_put(&buf[BYTES_PER_AXIS * ax], s_mag_field[ax]);
        }
    }
    else if ( QMC_STATUS == reg )
    {
        buf[0] = s_mag_status;
    }
    else if ( (QMI_WHO_AM_I == reg) && (BYTES_PER_AXIS == len) )
    {
        buf[0] = (uint8_t)KDRV_QMI8658_WHOAMI;
        buf[1] = QMI_REVISION;
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

static kode_status_t on_write(khal_i2c_dev_t dev, uint8_t reg, uint8_t val, int calls)
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

static kode_status_t on_write_regs(khal_i2c_dev_t dev, uint8_t reg, const uint8_t *buf,
                                   size_t len, int calls)
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

/** @brief Both parts answering; the IMU sample and the field staged by the case. */
static void bus_up(void)
{
    static const int32_t a_mg[AXES]    = { 1000, -500, 250 };
    static const int32_t w_mdps[AXES]  = { -2500, 1000, 500 };

    Mockkhal_i2c_Init();
    khal_i2c_init_IgnoreAndReturn(KODE_OK);
    khal_i2c_probe_IgnoreAndReturn(true);
    khal_i2c_add_IgnoreAndReturn(KODE_OK);
    khal_i2c_read_reg_StubWithCallback(&on_read);
    khal_i2c_write_reg_StubWithCallback(&on_write);
    khal_i2c_write_regs_StubWithCallback(&on_write_regs);

    (void)memset(s_regs, 0, sizeof(s_regs));
    (void)memset(s_sample, 0, sizeof(s_sample));
    stage_sample(a_mg, w_mdps, 265);
    s_acks         = true;
    s_mag_status   = QMC_DRDY;
    s_mag_field[0] = 450;
    s_mag_field[1] = -1500;
    s_mag_field[2] = 300;
}

/* ---- Letting the owner answer --------------------------------------------- */

/** @brief Drop everything sitting, undispatched, in @p ao's own queue. */
static void drain(QActive *ao)
{
    const QEvt *e;

    while ( (NULL != ao->eQueue) && (xQueueReceive(ao->eQueue, &e, 0U) == pdTRUE) )
    {
        QF_gc(e);
    }
}

/** @brief Let AO_Imu answer the wait this call is sitting in. */
static void settle(void)
{
    (void)qf_support_pump(AO_Imu, 32U);
    (void)qf_support_tick(AO_Imu, IMU_WAKE_TICKS);
    khal_sem_signal(NULL);
}

/** @brief Waits left for settle_again() to answer. */
static uint32_t s_settles_left;

/** @brief settle(), and again for the next wait while some are left. */
static void settle_again(void)
{
    settle();
    s_settles_left--;
    if ( 0U < s_settles_left )
    {
        fake_sem_wait_hook(&settle_again);
    }
}

/** @brief Arm the settle hook for the next sys read this case makes. */
static void will_settle(void)
{
    fake_sem_wait_hook(&settle);
}

/** @brief Arm the hook for the next @p n waits: a field read asks for the calibration, then the field. */
static void will_settle_n(uint32_t n)
{
    s_settles_left = n;
    fake_sem_wait_hook(&settle_again);
}

/** @brief Waits one field read makes. */
#define MAG_WAITS 2U

/** @brief Register AO_Imu once, and construct it afresh for every case. */
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

/** @brief Prove the facade's claim is free before a case runs. */
static void barrier(void)
{
    kdrv_qmi8658_sample_t probe;

    will_settle();
    TEST_ASSERT_NOT_EQUAL_INT_MESSAGE(KODE_DENIED, kode_block_imu_read_sample(&probe),
                                      "the facade was still dirty when this case began");
    drain(AO_Imu);
    fake_sem_reset();
    fake_time_reset();
}

/** @brief The chips, AO_Imu and the facade up for one case; the system marked up. */
static void fresh(void)
{
    /* The motion lock is made once, on the first kode.h call of the binary: a real one. */
    fake_mutex_real(true);
    bus_up();
    imu_up();
    fake_sem_reset();
    fake_time_reset();
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
    barrier();
}

/** @brief Expect @p got to be NAN and the task's error to read @p why. */
static void expect_failed(float got, const char *why)
{
    TEST_ASSERT_TRUE_MESSAGE(isnan(got), "a failed read must answer NAN");
    TEST_ASSERT_EQUAL_STRING(why, kode_error());
}

/* ---- The IMU by name -------------------------------------------------------- */

/** @brief One axis name and what the staged sample converts to. */
typedef struct {
    const char *name;  /**< As the caller types it.          */
    float       want;  /**< The staged value, in the unit.   */
} axis_row_t;

TEST_CASE("each IMU axis converts to its unit", "[kode_imu]")
{
    static const axis_row_t rows[SEVEN] = {
        { "accel_x", 1.0F }, { "accel_y", -0.5F }, { "accel_z", 0.25F },
        { "gyro_x", -2.5F }, { "gyro_y", 1.0F },   { "gyro_z", 0.5F },
        { "temp", 26.5F },
    };

    fresh();
    for ( uint32_t i = 0U; SEVEN > i; i++ )
    {
        will_settle();
        TEST_ASSERT_FLOAT_WITHIN(TOL, rows[i].want, kode_imu_read_axis(rows[i].name));
        TEST_ASSERT_EQUAL_STRING("", kode_error());
    }
}

TEST_CASE("the axis names take any case", "[kode_imu]")
{
    fresh();
    will_settle();
    TEST_ASSERT_FLOAT_WITHIN(TOL, 1.0F, kode_imu_read_axis("ACCEL_X"));
    will_settle();
    TEST_ASSERT_FLOAT_WITHIN(TOL, 0.5F, kode_imu_read_axis("Gyro_Z"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
}

TEST_CASE("a bad IMU axis is NAN with the name in the error and no read made", "[kode_imu]")
{
    fresh();

    /* No settle is armed: a bad name that reached the part would time out. */
    expect_failed(kode_imu_read_axis("accel x"), "unknown axis 'accel x'");
    expect_failed(kode_imu_read_axis("gyro_w"), "unknown axis 'gyro_w'");
    expect_failed(kode_imu_read_axis(""), "unknown axis ''");
    expect_failed(kode_imu_read_axis(NULL), "axis name is NULL");
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Imu->eQueue));
}

TEST_CASE("read_all fills the seven readings from one sample", "[kode_imu]")
{
    kode_imu_t all;

    fresh();
    will_settle();
    all = kode_imu_read();

    TEST_ASSERT_FLOAT_WITHIN(TOL, 1.0F, all.accel_x);
    TEST_ASSERT_FLOAT_WITHIN(TOL, -0.5F, all.accel_y);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 0.25F, all.accel_z);
    TEST_ASSERT_FLOAT_WITHIN(TOL, -2.5F, all.gyro_x);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 1.0F, all.gyro_y);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 0.5F, all.gyro_z);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 26.5F, all.temp);
    /* One settle was armed: a second request would have timed out. */
    TEST_ASSERT_EQUAL_STRING("", kode_error());
}

TEST_CASE("an IMU that does not answer is NAN and not fitted", "[kode_imu]")
{
    kode_imu_t all;

    fresh();
    s_acks = false;

    will_settle();
    expect_failed(kode_imu_read_axis("temp"), "imu: the IMU is not fitted");

    will_settle();
    all = kode_imu_read();
    TEST_ASSERT_TRUE(isnan(all.accel_x));
    TEST_ASSERT_TRUE(isnan(all.accel_y));
    TEST_ASSERT_TRUE(isnan(all.accel_z));
    TEST_ASSERT_TRUE(isnan(all.gyro_x));
    TEST_ASSERT_TRUE(isnan(all.gyro_y));
    TEST_ASSERT_TRUE(isnan(all.gyro_z));
    TEST_ASSERT_TRUE(isnan(all.temp));
    TEST_ASSERT_EQUAL_STRING("imu: the IMU is not fitted", kode_error());
    s_acks = true;
}

TEST_CASE("an IMU owner that never answers is NAN and did not answer", "[kode_imu]")
{
    fresh();

    fake_sem_wait_hook(NULL);
    expect_failed(kode_imu_read_axis("temp"), "imu: the IMU did not answer");

    /* Let the abandoned request land, so the next case finds a clean facade. */
    will_settle();
    TEST_ASSERT_FLOAT_WITHIN(TOL, 26.5F, kode_imu_read_axis("temp"));
}

/* ---- The magnetometer by name ----------------------------------------------- */

TEST_CASE("each field axis converts to microtesla", "[kode_mag]")
{
    fresh();

    will_settle_n(MAG_WAITS);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 45.0F, kode_mag_read_axis("x"));
    will_settle_n(MAG_WAITS);
    TEST_ASSERT_FLOAT_WITHIN(TOL, -150.0F, kode_mag_read_axis("Y"));
    will_settle_n(MAG_WAITS);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 30.0F, kode_mag_read_axis("z"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
}

TEST_CASE("mag read_all fills the three axes from one reading", "[kode_mag]")
{
    kode_mag_t all;

    fresh();
    will_settle_n(MAG_WAITS);
    all = kode_mag_read();

    TEST_ASSERT_FLOAT_WITHIN(TOL, 45.0F, all.x);
    TEST_ASSERT_FLOAT_WITHIN(TOL, -150.0F, all.y);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 30.0F, all.z);
    TEST_ASSERT_EQUAL_STRING("", kode_error());
}

TEST_CASE("a bad field axis is NAN with the name in the error and no read made", "[kode_mag]")
{
    fresh();

    expect_failed(kode_mag_read_axis("w"), "unknown axis 'w'");
    expect_failed(kode_mag_read_axis("xx"), "unknown axis 'xx'");
    expect_failed(kode_mag_read_axis(NULL), "axis name is NULL");
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Imu->eQueue));
}

TEST_CASE("a magnetometer that is not fitted is NAN for all three axes", "[kode_mag]")
{
    kode_mag_t all;

    fresh();
    s_acks = false;

    will_settle_n(MAG_WAITS);
    expect_failed(kode_mag_read_axis("x"), "magnetometer: the magnetometer is not fitted");

    will_settle_n(MAG_WAITS);
    all = kode_mag_read();
    TEST_ASSERT_TRUE(isnan(all.x));
    TEST_ASSERT_TRUE(isnan(all.y));
    TEST_ASSERT_TRUE(isnan(all.z));
    s_acks = true;
}

/** @brief The part has a conversion by the time the caller asks again. */
static void conversion_ready(void)
{
    s_mag_status = QMC_DRDY;
    will_settle();
}

TEST_CASE("a field read with no conversion ready is asked again", "[kode_mag]")
{
    fresh();
    s_mag_status = 0U;

    will_settle_n(MAG_WAITS);
    fake_time_delay_hook(&conversion_ready);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 45.0F, kode_mag_read_axis("x"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_RETRY_STEP_MS, kode_sys_time_ms());
    TEST_ASSERT_LESS_THAN_UINT32(KODE_MOTION_BUSY_MS, kode_sys_time_ms());
}

/** @brief Still no conversion by the next ask; the owner answers it again. */
static void conversion_missing(void)
{
    will_settle();
    fake_time_delay_hook(&conversion_missing);
}

TEST_CASE("a field that never converts is NAN after the bound", "[kode_mag]")
{
    fresh();
    s_mag_status = 0U;

    will_settle_n(MAG_WAITS);
    fake_time_delay_hook(&conversion_missing);
    expect_failed(kode_mag_read_axis("x"), "magnetometer: the magnetometer gave no reading");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_MOTION_BUSY_MS, kode_sys_time_ms());
}

/* ---- Another task reading --------------------------------------------------- */

/** @brief The error text a read made from inside a wait left behind. */
static char s_inner_error[ERROR_COPY];

/** @brief What that read answered. */
static float s_inner_value;

/** @brief How long the fake clock had run when it came back. */
static uint32_t s_inner_ms;

/** @brief Which read the hook makes: true for the magnetometer. */
static bool s_inner_mag;

/** @brief A second read from inside the first one's wait, then the first's answer. */
static void read_while_held(void)
{
    if ( s_inner_mag )
    {
        s_inner_value = kode_mag_read_axis("x");
    }
    else
    {
        s_inner_value = kode_imu_read_axis("temp");
    }
    s_inner_ms = kode_sys_time_ms();
    (void)strncpy(s_inner_error, kode_error(), sizeof(s_inner_error) - 1U);
    s_inner_error[sizeof(s_inner_error) - 1U] = '\0';
    settle();
}

TEST_CASE("an IMU read held by another reader gives up after the bound", "[kode_imu]")
{
    kdrv_qmi8658_sample_t s;

    fresh();
    s_inner_mag = false;
    fake_sem_wait_hook(&read_while_held);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_imu_read_sample(&s));

    TEST_ASSERT_TRUE(isnan(s_inner_value));
    TEST_ASSERT_EQUAL_STRING("imu: the IMU is busy", s_inner_error);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_MOTION_BUSY_MS, s_inner_ms);
}

TEST_CASE("a field read held by another reader gives up after the bound", "[kode_mag]")
{
    int32_t mg[AXES];

    fresh();
    s_inner_mag = true;
    fake_sem_wait_hook(&read_while_held);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_mag_read_raw(mg));

    TEST_ASSERT_TRUE(isnan(s_inner_value));
    TEST_ASSERT_EQUAL_STRING("magnetometer: the magnetometer is busy", s_inner_error);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_MOTION_BUSY_MS, s_inner_ms);
}

/** @brief Open a witness window on AO_Imu: the part is then held. */
static void window_open(void)
{
    ImuCaptureReqEvt q;

    q.super = (QEvt)QEVT_INITIALIZER(IMU_CAPTURE_REQ_SIG);
    q.who   = ao_spy();
    QASM_DISPATCH(AO_Imu, &q.super, 0U);
    ao_spy_drain();
}

/** @brief The window's deadline passes while the caller sleeps; the next try finds the part free. */
static void window_ends(void)
{
    (void)qf_support_tick(AO_Imu, WINDOW_END_TICKS);
    will_settle();
}

TEST_CASE("an IMU read refused by a window waits its turn and gets the value", "[kode_imu]")
{
    fresh();
    window_open();

    will_settle();
    fake_time_delay_hook(&window_ends);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 26.5F, kode_imu_read_axis("temp"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_RETRY_STEP_MS, kode_sys_time_ms());
    TEST_ASSERT_LESS_THAN_UINT32(KODE_MOTION_BUSY_MS, kode_sys_time_ms());
}

/* ---- Two tasks, one motion layer ------------------------------------------- */

/** @brief Stack of the second task, in bytes. */
#define TEST_STACK_BYTES 8192U

/** @brief Priority of the second task. */
#define TEST_PRIO 1U

/** @brief The longest the case waits for the second task, in ms. */
#define TEST_WAIT_MS 4000U

/** @brief Ticks the first task sleeps between two looks at the second. */
#define TEST_POLL_TICKS 1U

/** @brief What the staged field reads on x, in microtesla. */
#define TEST_FIELD_X_UT 45.0F

static SemaphoreHandle_t s_two_go;       /**< Given to let the second task begin.   */
static SemaphoreHandle_t s_two_at_door;  /**< Given by it just before its call.     */
static SemaphoreHandle_t s_two_done;     /**< Given by it when its call returned.   */
static TaskHandle_t      s_two_task;     /**< The second task.                      */
static atomic_bool       s_two_returned; /**< Its call returned.                    */
static kode_mag_t        s_two_field;    /**< What its read answered.               */
static char              s_two_error[ERROR_COPY]; /**< Its error after the call.    */

/** @brief The second task: one field read at the moment it is let go. */
static void two_second(void *arg)
{
    (void)arg;
    (void)xSemaphoreTake(s_two_go, pdMS_TO_TICKS(TEST_WAIT_MS));
    s_two_task = xTaskGetCurrentTaskHandle();
    will_settle_n(MAG_WAITS);
    (void)xSemaphoreGive(s_two_at_door);
    s_two_field = kode_mag_read();
    (void)strncpy(s_two_error, kode_error(), sizeof(s_two_error) - 1U);
    s_two_error[sizeof(s_two_error) - 1U] = '\0';
    atomic_store(&s_two_returned, true);
    (void)xSemaphoreGive(s_two_done);
}

/** @brief From inside the first task's wait: let the second call in, then answer the first. */
static void two_let_in(void)
{
    bool settled = (xSemaphoreGive(s_two_go) == pdTRUE)
                   && (xSemaphoreTake(s_two_at_door, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);

    for ( uint32_t i = 0U; settled && (pdMS_TO_TICKS(TEST_WAIT_MS) > i) && !atomic_load(&s_two_returned)
                          && (eTaskGetState(s_two_task) != eBlocked); i++ )
    {
        vTaskDelay(TEST_POLL_TICKS);
    }
    settle();
}

TEST_CASE("a field read and an IMU read at once are served one after the other", "[kode_imu]")
{
    kode_imu_t first;

    fresh();
    s_two_go      = xSemaphoreCreateBinary();
    s_two_at_door = xSemaphoreCreateBinary();
    s_two_done    = xSemaphoreCreateBinary();
    s_two_task    = NULL;
    s_two_error[0] = '\0';
    atomic_store(&s_two_returned, false);
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_task_start(&two_second, "t_motion", TEST_STACK_BYTES,
                                                    TEST_PRIO, NULL));

    fake_sem_wait_hook(&two_let_in);
    first = kode_imu_read();
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_two_done, pdMS_TO_TICKS(TEST_WAIT_MS)));
    vSemaphoreDelete(s_two_go);
    vSemaphoreDelete(s_two_at_door);
    vSemaphoreDelete(s_two_done);

    TEST_ASSERT_FLOAT_WITHIN(TOL, 26.5F, first.temp);
    TEST_ASSERT_EQUAL_STRING("", s_two_error);
    TEST_ASSERT_FLOAT_WITHIN(TOL, TEST_FIELD_X_UT, s_two_field.x);
    fake_sem_reset();
}

/* ---- The compass ------------------------------------------------------------- */

#define CAL_OFFSET_X   50    /**< The case's calibration: offset on x, mG.        */
#define CAL_OFFSET_Y   (-100) /**< Offset on y.                                   */
#define CAL_OFFSET_Z   30    /**< Offset on z.                                    */
#define CAL_SCALE_X    2000  /**< Scale on x, thousandths.                        */
#define CAL_SCALE_UNIT 1000  /**< Scale 1.0.                                      */
#define HEADING_TOL    2.0F  /**< The heading's tolerance, degrees.               */
#define EAST_FIELD_X   (-200) /**< Field on x with the Dot's top pointing east, mG. */
#define DOWN_FIELD_Z   (-400) /**< The downward part of the field, mG.            */
#define ONE_G_MG       1000  /**< One g, mG.                                      */
#define SHAKEN_MG      3000  /**< An acceleration that says nothing about up.     */
#define UT_PER_MG      0.1F  /**< Microtesla in a milligauss.                     */

/** @brief The case's calibration, stored through the facade's door. */
static void store_cal(int32_t scale_x)
{
    const kdrv_qmc5883p_cal_t corr = {
        { CAL_OFFSET_X, CAL_OFFSET_Y, CAL_OFFSET_Z }, { scale_x, CAL_SCALE_UNIT, CAL_SCALE_UNIT }
    };

    will_settle();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_mag_cal_set(&corr));
}

/** @brief Stage the Dot flat, its top east: the field as the chip reads it with the case's offsets in. */
static void stage_flat_east(int32_t accel_z_mg)
{
    const int32_t a_mg[AXES]   = { 0, 0, accel_z_mg };
    const int32_t w_mdps[AXES] = { 0, 0, 0 };

    stage_sample(a_mg, w_mdps, 0);
    s_mag_field[0] = (int16_t)(EAST_FIELD_X + CAL_OFFSET_X);
    s_mag_field[1] = (int16_t)(0 + CAL_OFFSET_Y);
    s_mag_field[2] = (int16_t)(DOWN_FIELD_Z + CAL_OFFSET_Z);
}

TEST_CASE("a stored calibration corrects what kode_mag_read_axis answers", "[kode_compass]")
{
    fake_nvs_reset();
    fresh();
    store_cal(CAL_SCALE_X);

    /* x: (450 - 50) mG at 2.0 is 800 mG; y: (-1500 + 100) at 1.0; z: 300 - 30. */
    will_settle_n(MAG_WAITS);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 800.0F * UT_PER_MG, kode_mag_read_axis("x"));
    will_settle_n(MAG_WAITS);
    TEST_ASSERT_FLOAT_WITHIN(TOL, -1400.0F * UT_PER_MG, kode_mag_read_axis("y"));
    will_settle_n(MAG_WAITS);
    TEST_ASSERT_FLOAT_WITHIN(TOL, 270.0F * UT_PER_MG, kode_mag_read_axis("z"));
    fake_nvs_reset();
}

TEST_CASE("the calibration is stored, reported and cleared", "[kode_compass]")
{
    fake_nvs_reset();
    fresh();

    will_settle();
    TEST_ASSERT_FALSE(kode_mag_is_calibrated());
    TEST_ASSERT_EQUAL_STRING("", kode_error());

    store_cal(CAL_SCALE_X);
    will_settle();
    TEST_ASSERT_TRUE(kode_mag_is_calibrated());

    will_settle();
    TEST_ASSERT_TRUE(kode_mag_clear_calibration());
    will_settle();
    TEST_ASSERT_FALSE(kode_mag_is_calibrated());

    /* Clearing nothing is done all the same. */
    will_settle();
    TEST_ASSERT_TRUE(kode_mag_clear_calibration());
    fake_nvs_reset();
}

TEST_CASE("an owner that does not answer is not a missing calibration", "[kode_compass]")
{
    fresh();

    fake_sem_wait_hook(NULL);
    TEST_ASSERT_FALSE(kode_mag_is_calibrated());
    TEST_ASSERT_EQUAL_STRING("magnetometer: the magnetometer did not answer", kode_error());

    will_settle();
    TEST_ASSERT_TRUE_MESSAGE(kode_mag_clear_calibration(),   /* the abandoned request lands first */
                             "the abandoned request took the newer caller's place");
    fake_nvs_reset();
}

TEST_CASE("heading without a calibration is NAN and says what to call", "[kode_compass]")
{
    fake_nvs_reset();
    fresh();
    stage_flat_east(ONE_G_MG);

    will_settle();
    expect_failed(kode_mag_heading(), "call kode_mag_calibrate() first");
    fake_nvs_reset();
}

TEST_CASE("heading with a calibration follows the Dot's top", "[kode_compass]")
{
    fake_nvs_reset();
    fresh();
    store_cal(CAL_SCALE_UNIT);
    stage_flat_east(ONE_G_MG);

    will_settle_n(MAG_WAITS + 1U);
    TEST_ASSERT_FLOAT_WITHIN(HEADING_TOL, 90.0F, kode_mag_heading());
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    fake_nvs_reset();
}

TEST_CASE("heading of a shaken Dot is NAN and says so", "[kode_compass]")
{
    fake_nvs_reset();
    fresh();
    store_cal(CAL_SCALE_UNIT);
    stage_flat_east(SHAKEN_MG);

    will_settle_n(MAG_WAITS + 1U);
    expect_failed(kode_mag_heading(), "the Dot is moving too much to tell which way is up");
    fake_nvs_reset();
}

/** @brief A filler AO_Imu ignores. */
static const QEvt s_filler = QEVT_INITIALIZER(MAG_TEST_REQ_SIG);

/** @brief The queue empties while the caller sleeps, and its next try is answered. */
static void queue_emptied(void)
{
    drain(AO_Imu);
    will_settle();
}

TEST_CASE("a calibration call waits its turn through a full AO_Imu queue", "[kode_compass]")
{
    fake_nvs_reset();
    fresh();

    qf_support_margin_queue(AO_Imu);
    TEST_ASSERT_TRUE_MESSAGE(qf_support_queue_stuff(AO_Imu, &s_filler), "the queue never filled");
    fake_time_delay_hook(&queue_emptied);
    TEST_ASSERT_FALSE(kode_mag_is_calibrated());
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", kode_error(), "it gave up instead of waiting");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_RETRY_STEP_MS, kode_sys_time_ms());
    fake_nvs_reset();
}
