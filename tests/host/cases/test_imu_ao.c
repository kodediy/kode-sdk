/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_imu_ao.c
 * @brief AO_Imu: the owner of the QMI8658A and the QMC5883P, over a register file.
 */
#include <math.h>
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "qpc.h"
#include "Mockkhal_i2c.h"

#include "ao_spy.h"
#include "ask_support.h"
#include "config_support.h"
#include "fakes.h"
#include "kaos.h"
#include "kbsp.h"
#include "kbsp_owners.h"
#include "kdrv_qmc5883p.h"
#include "kdrv_qmi8658.h"
#include "khal_nvs.h"
#include "khal_system.h"
#include "khal_sync.h"
#include "kimu_energy.h"
#include "kode_sys.h"
#include "lowpower_cases.h"
#include "kqp.h"
#include "kset.h"
#include "qf_support.h"

/* Cases run in file-name order: this file runs before test_kdrv_qmi8658.c, while both
 * drivers are still down. */
/**
 * @brief The QMI8658A's first bring-up, with every assertion the datasheet
 *        pins on it.
 *
 * Defined in test_kdrv_qmi8658.c and idempotent.
 */
void test_qmi8658_bring_up(void);

/** @brief The QMC5883P's, the same way. Defined in test_kdrv_qmc5883p.c. */
void test_qmc5883p_bring_up(void);

/* ---- The QMI8658A's registers ---------------------------------------------- */

#define QMI_WHO_AM_I    0x00U
#define QMI_CTRL1       0x02U
#define QMI_CTRL2       0x03U
#define QMI_CTRL3       0x04U
#define QMI_CTRL7       0x08U
#define QMI_CTRL9       0x0AU
#define QMI_FIFO_CTRL   0x14U
#define QMI_FIFO_CNT    0x15U
#define QMI_FIFO_DATA   0x17U
#define QMI_STATUSINT   0x2DU
#define QMI_TEMP_L      0x33U
#define QMI_DVX_L       0x51U

/** @brief TEMP_L, TEMP_H, six bytes of acceleration, six of rotation. */
#define QMI_BURST_LEN   14U

#define QMI_ST_TRIGGER  0x80U   /**< aST in CTRL2, gST in CTRL3.             */
#define QMI_CMD_DONE    0x80U   /**< STATUSINT bit 7: a CTRL9 command landed. */
#define QMI_ST_DONE     0x01U   /**< STATUSINT bit 0: the self-test finished. */
#define QMI_FIFO_FILL   0x0DU   /**< FIFO_CTRL: 128 samples, FIFO mode.      */

/* ---- The QMC5883P's ---------------------------------------------------------
 * A different part at a different address, and its map overlaps the IMU's:
 * bus_dispatch() is where the two are told apart.
 */
#define QMC_CHIP_ID     0x00U
#define QMC_X_LSB       0x01U
#define QMC_STATUS      0x09U
#define QMC_CTRL1       0x0AU   /**< The mode; the IMU's CTRL9 too.          */
#define QMC_CTRL2       0x0BU
#define QMC_SIGN        0x29U

#define QMC_DRDY        0x01U
#define QMC_CONTINUOUS  0x07U   /**< CTRL1: continuous at 50 Hz.             */
#define QMI_SENSORS_ON  0x03U   /**< CTRL7: aEN | gEN.                       */
#define QMC_SELF_TEST   0x40U

/* ---- What the numbers mean, so a case says mg rather than LSB -------------- */

/** @brief The sample's scale: 4096 LSB per g at +-8 g. */
#define LSB_PER_G       4096
/** @brief And 64 LSB per degree per second at +-512 dps. */
#define LSB_PER_DPS     64
/** @brief The die temperature is 8.8 fixed point degrees. */
#define LSB_PER_C       256
/** @brief The accelerometer's self-test result is U5.11 of a g. */
#define ST_LSB_PER_G    2048
/** @brief The gyroscope's is U12.4 of a degree per second. */
#define ST_LSB_PER_DPS  16

/* The self-test shifts a case stages, in raw LSB rather than in the unit the
 * record carries: both scales are powers of two over a thousand, so a round
 * number of milli-g is not a round number of LSB and a case written in mg
 * would be asserting where two truncations landed. Each is paired with what
 * the driver reports for it. */
/** @brief A shift that clears the datasheet's 200 mg floor: 500 mg. */
#define ST_XL_PASS_LSB  1024
#define ST_XL_PASS_MG   500
/** @brief One that does not: 125 mg. */
#define ST_XL_FAIL_LSB  256
#define ST_XL_FAIL_MG   125
/** @brief The same for the gyroscope's 300 dps floor: 400 dps. */
#define ST_GY_PASS_LSB  6400
#define ST_GY_PASS_MDPS 400000
/** @brief And 100 dps, which does not clear it. */
#define ST_GY_FAIL_LSB  1600
#define ST_GY_FAIL_MDPS 100000

/** @brief The witness sine the FIFO is filled with, in milli-g. */
#define WITNESS_MG      100
/** @brief At this frequency, which is a whole bin of a 128-sample window
 *         taken at 1000 Hz (7.8125 Hz apart). */
#define WITNESS_HZ      235U
/** @brief Which axis carries it. */
#define WITNESS_AXIS    1U

/**
 * @brief The gyroscope's turn-on, in milliseconds, as imu_ao.c holds it.
 *
 * Repeated here rather than imported, because it is private to the AO - the
 * console files repeat their own watchdog for the same reason. A FIFO reset
 * or a self-test trigger asked for inside this window is not acted on by the
 * part, so the AO holds the request until it is over, and a case that means
 * to see a request acted on at once has to be past it.
 */
#define GYRO_SETTLE_MS  300U
/** @brief What the row gives a part it has just put to rest, the same way. */
#define ROW_REST_MS     150U
/** @brief What a read that woke the part waits, the same way. */
#define GYRO_READY_MS   200U
/** @brief A part's whole rest time in QP ticks, and two more. */
#define REST_TICKS      (QF_SUPPORT_STEPS(KODE_SYS_IMU_REST_AFTER_MS) + 2U)

/* ---- The two chips, as register files --------------------------------------- */

static uint8_t  s_regs[256];       /* the IMU's control registers            */
static bool     s_imu_acks;        /* the part answers a probe at 0x6A       */
static bool     s_mag_acks;        /* and the magnetometer at 0x2C           */
static bool     s_mag_stray;       /* or at 0x0C, after a bad power-up       */
static bool     s_bus_dead;        /* every transfer fails                   */

static uint8_t  s_sample[QMI_BURST_LEN];  /* what one read burst answers     */
/* What each sensor's self-test moves, raw. Two of them rather than one,
 * because the result register is shared and the part answers with whichever
 * sensor is under test - so a case stages both up front and never has to get
 * between the row's own steps to change one. */
static int16_t  s_dv_xl[3];
static int16_t  s_dv_gy[3];
static bool     s_st_is_gy;        /* which trigger was set last            */

static bool     s_cmd_done;        /* a CTRL9 command is waiting to be acked */
static bool     s_st_armed;        /* a self-test trigger is set             */
static unsigned s_st_polls;        /* polls before the part reports it done  */
static unsigned s_st_seen;         /* polls so far                           */

static uint16_t s_fifo_fill;       /* samples a freshly armed window gets    */
static uint16_t s_fifo_left;       /* samples still in it                    */
static uint16_t s_fifo_idx;        /* how many have been read out            */

static uint8_t  s_mag_id;          /* the chip-id register                   */
static uint8_t  s_mag_status;      /* data ready, overflow                   */
static bool     s_mag_st_on;       /* the self-test stimulus is driven       */
static bool     s_mag_running;     /* continuous mode, as opposed to suspend */
static int16_t  s_mag_field[3];    /* the field, milligauss                  */
static int16_t  s_mag_st_add[3];   /* what the stimulus adds to it           */

/** @brief Write @p v into @p p, low byte first, as both parts report. */
static void le16_put(uint8_t *p, int16_t v)
{
    p[0] = (uint8_t)((uint16_t)v & 0xFFU);
    p[1] = (uint8_t)(((uint16_t)v >> 8) & 0xFFU);
}

/**
 * @brief Stage the sample one read burst answers.
 *
 * In the units a case thinks in; the scales are the part's own, so a case
 * that says 1000 mg is asserting the driver's arithmetic as well as the AO's
 * judgement of it.
 *
 * @param ax_mg   Acceleration per axis, milli-g.
 * @param w_mdps  Rotation per axis, milli-degrees per second.
 * @param temp_dc The die, in tenths of a degree.
 */
static void stage_sample(const int32_t ax_mg[3], const int32_t w_mdps[3],
                         int32_t temp_dc)
{
    unsigned i;

    le16_put(&s_sample[0], (int16_t)((temp_dc * LSB_PER_C) / 10));
    for ( i = 0U; 3U > i; ++i )
    {
        le16_put(&s_sample[2U + (2U * i)],
                 (int16_t)((ax_mg[i] * LSB_PER_G) / 1000));
        le16_put(&s_sample[8U + (2U * i)],
                 (int16_t)((w_mdps[i] * LSB_PER_DPS) / 1000));
    }
}

/** @brief The board lying flat and still, at room temperature. */
static void stage_sample_at_rest(void)
{
    int32_t const a[3] = { 0, 0, 1000 };
    int32_t const w[3] = { 0, 0, 0 };

    stage_sample(a, w, 265);
}

/** @brief Stage what the accelerometer's self-test will report, raw. */
static void stage_xl_shift(int16_t x, int16_t y, int16_t z)
{
    s_dv_xl[0] = x;
    s_dv_xl[1] = y;
    s_dv_xl[2] = z;
}

/** @brief And the gyroscope's. */
static void stage_gy_shift(int16_t x, int16_t y, int16_t z)
{
    s_dv_gy[0] = x;
    s_dv_gy[1] = y;
    s_dv_gy[2] = z;
}

/** @brief STATUSINT as the part would present it right now. */
static uint8_t statusint(void)
{
    uint8_t status = 0U;

    if ( s_cmd_done )
    {
        status |= QMI_CMD_DONE;
    }
    if ( s_st_armed )
    {
        if ( s_st_seen >= s_st_polls )
        {
            status |= QMI_ST_DONE;
        }
        else
        {
            s_st_seen++;
        }
    }
    return status;
}

/**
 * @brief One FIFO sample, generated rather than stored.
 *
 * A window is 128 samples of six bytes and holding them would be a copy of
 * the detector's own input; generating them is what lets a case say "a sine
 * of this many milli-g at this frequency" and then assert what came back out
 * of kimu_energy().
 *
 * @param i      Which sample.
 * @param[out] p Six bytes: x, y, z, little-endian.
 */
static void fifo_sample(uint16_t i, uint8_t *p)
{
    double const phase = (2.0 * M_PI * (double)WITNESS_HZ * (double)i)
                         / (double)KDRV_QMI8658_WITNESS_HZ;
    double const amp   = ((double)WITNESS_MG * 1000.0)
                         / (double)KDRV_QMI8658_UG_PER_LSB;
    unsigned     ax;

    for ( ax = 0U; 3U > ax; ++ax )
    {
        int16_t v = 0;

        if ( WITNESS_AXIS == ax )
        {
            v = (int16_t)lrint(amp * sin(phase));
        }
        else if ( 2U == ax )
        {
            /* Gravity, so the detector's mean removal is exercised rather
             * than handed three zeroes. */
            v = (int16_t)(1000000 / (int32_t)KDRV_QMI8658_UG_PER_LSB);
        }
        else
        {
            /* x reads nothing at all. */
        }
        le16_put(&p[2U * ax], v);
    }
}

/* A driver keeps the handle of whichever case first brought it up, so a handle names no chip. */
/**
 * @brief Which chip a transfer at @p reg of @p len bytes belongs to.
 *
 * The two share this bus and their maps overlap, and the device handle cannot
 * settle it. Every overlap is listed here rather than
 * left to a comment somewhere else:
 *
 *   0x00  the IMU reads two bytes (WHO_AM_I and the revision), the
 *         magnetometer one (its chip id).
 *   0x01  six bytes is the magnetometer's data; the IMU never reads there.
 *   0x09  the magnetometer's status. The IMU's CTRL8 lives there and is
 *         written once, inside a bring-up this file delegates, so the stub
 *         never sees it.
 *   0x0A  the IMU's CTRL9 and the magnetometer's CTRL1. Both are written and
 *         neither is read, and the command flag a CTRL9 write raises is only
 *         ever read by the IMU - so a mode change on the magnetometer setting
 *         it costs nothing.
 *   0x02..0x06  the IMU's control registers, one byte at a time; the
 *         magnetometer's data window, six at once.
 */
static bool reg_is_mag(uint8_t reg, size_t len)
{
    bool mag = false;

    if ( (QMC_CHIP_ID == reg) && (1U == len) )
    {
        mag = true;
    }
    else if ( (QMC_X_LSB == reg) && (6U == len) )
    {
        mag = true;
    }
    else if ( QMC_STATUS == reg )
    {
        mag = true;
    }
    else
    {
        /* the IMU's, or a register neither reads */
    }
    return mag;
}

static kode_status_t on_read(khal_i2c_dev_t dev, uint8_t reg, uint8_t *buf,
                             size_t len, int calls)
{
    (void)dev;
    (void)calls;
    if ( s_bus_dead )
    {
        return KODE_ERROR;
    }
    if ( reg_is_mag(reg, len) )
    {
        if ( QMC_CHIP_ID == reg )
        {
            buf[0] = s_mag_id;
        }
        else if ( QMC_STATUS == reg )
        {
            /* A suspended part converts nothing, so it has nothing ready. */
            buf[0] = 0U;
            if ( s_mag_running )
            {
                buf[0] = s_mag_status;
            }
        }
        else
        {
            unsigned ax;

            for ( ax = 0U; 3U > ax; ++ax )
            {
                int16_t v = s_mag_field[ax];

                if ( s_mag_st_on )
                {
                    v = (int16_t)(v + s_mag_st_add[ax]);
                }
                le16_put(&buf[2U * ax], v);
            }
        }
        return KODE_OK;
    }

    if ( (QMI_WHO_AM_I == reg) && (2U == len) )
    {
        buf[0] = s_regs[QMI_WHO_AM_I];
        buf[1] = s_regs[QMI_WHO_AM_I + 1U];
    }
    else if ( QMI_STATUSINT == reg )
    {
        buf[0] = statusint();
    }
    else if ( (QMI_TEMP_L == reg) && (QMI_BURST_LEN == len) )
    {
        (void)memcpy(buf, s_sample, sizeof(s_sample));
    }
    else if ( (QMI_DVX_L == reg) && (6U == len) )
    {
        const int16_t *dv = s_dv_xl;
        unsigned       ax;

        if ( s_st_is_gy )
        {
            dv = s_dv_gy;
        }
        for ( ax = 0U; 3U > ax; ++ax )
        {
            le16_put(&buf[2U * ax], dv[ax]);
        }
    }
    else if ( (QMI_FIFO_CNT == reg) && (2U == len) )
    {
        /* Ten bits, in words of two bytes; three of them are a sample. */
        uint16_t const words = (uint16_t)(3U * s_fifo_left);

        buf[0] = (uint8_t)(words & 0xFFU);
        buf[1] = (uint8_t)((words >> 8) & 0x03U);
    }
    else if ( QMI_FIFO_DATA == reg )
    {
        size_t i;

        for ( i = 0U; (len / 6U) > i; ++i )
        {
            fifo_sample(s_fifo_idx, &buf[6U * i]);
            s_fifo_idx++;
        }
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
    if ( s_bus_dead )
    {
        return KODE_ERROR;
    }

    if ( QMC_CTRL2 == reg )
    {
        s_mag_st_on = ((val & QMC_SELF_TEST) != 0U);
        return KODE_OK;
    }
    if ( QMC_SIGN == reg )
    {
        return KODE_OK;
    }
    if ( QMI_CTRL9 == reg )
    {
        /* The handshake, as the datasheet describes it: a command raises the
         * flag, the host's acknowledgement of zero lowers it again.
         *
         * This address is also the magnetometer's CTRL1, and a mode change
         * there ends the self-test stimulus - the bit clears itself once the
         * data is updated, which is why the driver never writes it back. Both
         * belong here because the address is one: the flag is read only by
         * the IMU, and the stimulus is driven only inside the magnetometer's
         * own self-test, so neither can see the other's write.
         *
         * Which part a write is for is told by its value: 0x07 is only ever
         * the magnetometer's continuous mode, and a zero is the IMU's
         * acknowledgement while a command of its is waiting for one and the
         * magnetometer's suspend otherwise. */
        s_mag_st_on = false;
        if ( QMC_CONTINUOUS == val )
        {
            s_mag_running = true;
        }
        else if ( (0U == val) && !s_cmd_done )
        {
            s_mag_running = false;
        }
        else
        {
            s_cmd_done = (0U != val);
        }
        return KODE_OK;
    }
    if ( (QMI_CTRL2 == reg) || (QMI_CTRL3 == reg) )
    {
        /* The self-test's stimulus: set, the part works on its own for a
         * while and then says so; cleared, the flag falls with it. The
         * trigger's register is also which sensor is under test, and the
         * result register answers for that one. */
        if ( (val & QMI_ST_TRIGGER) != 0U )
        {
            s_st_armed  = true;
            s_st_seen   = 0U;
            s_st_is_gy  = (QMI_CTRL3 == reg);
        }
        else
        {
            s_st_armed = false;
        }
    }
    if ( QMI_FIFO_CTRL == reg )
    {
        if ( QMI_FIFO_FILL == val )
        {
            /* The window opens empty and fills once: BYPASS emptied it, and
             * this is the moment the accelerometer starts putting samples in.
             * A window that did not refill here would hand the next drain the
             * last one's leftovers, which is exactly what the driver goes
             * through BYPASS to prevent. */
            s_fifo_left = s_fifo_fill;
            s_fifo_idx  = 0U;
        }
        else
        {
            s_fifo_left = 0U;
        }
    }
    s_regs[reg] = val;
    return KODE_OK;
}

static kode_status_t on_write_regs(khal_i2c_dev_t dev, uint8_t reg,
                                   const uint8_t *buf, size_t len, int calls)
{
    (void)dev;
    (void)calls;
    if ( s_bus_dead )
    {
        return KODE_ERROR;
    }
    (void)memcpy(&s_regs[reg], buf, len);
    return KODE_OK;
}

static bool on_probe(uint16_t addr, uint32_t timeout_ms, int calls)
{
    (void)timeout_ms;
    (void)calls;
    if ( (uint16_t)KBSP_I2C_ADDR_MAG == addr )
    {
        return s_mag_acks;
    }
    if ( (uint16_t)KBSP_I2C_ADDR_MAG_STRAY == addr )
    {
        return s_mag_stray;
    }
    return s_imu_acks;
}

/**
 * @brief Both chips answering, the board flat and still and the FIFO empty.
 *
 * @param imu Whether a QMI8658A answers at 0x6A.
 * @param mag Whether a QMC5883P answers at 0x2C.
 */
static void bus_up(bool imu, bool mag)
{
    Mockkhal_i2c_Init();
    khal_i2c_init_IgnoreAndReturn(KODE_OK);
    khal_i2c_probe_StubWithCallback(&on_probe);
    khal_i2c_add_IgnoreAndReturn(KODE_OK);
    khal_i2c_remove_IgnoreAndReturn(KODE_OK);
    khal_i2c_read_reg_StubWithCallback(&on_read);
    khal_i2c_write_reg_StubWithCallback(&on_write);
    khal_i2c_write_regs_StubWithCallback(&on_write_regs);

    (void)memset(s_regs, 0, sizeof(s_regs));
    s_regs[QMI_WHO_AM_I]      = (uint8_t)KDRV_QMI8658_WHOAMI;
    s_regs[QMI_WHO_AM_I + 1U] = 0x7CU;   /* the QMI8658A's revision */
    s_imu_acks  = imu;
    s_mag_acks  = mag;
    s_mag_stray = false;
    s_bus_dead  = false;
    s_cmd_done  = false;
    s_st_armed  = false;
    s_st_polls  = 0U;
    s_st_seen   = 0U;
    s_fifo_fill = (uint16_t)KDRV_QMI8658_FIFO_DEPTH;
    s_fifo_left = 0U;
    s_fifo_idx  = 0U;

    s_st_is_gy = false;
    stage_sample_at_rest();
    stage_xl_shift(ST_XL_PASS_LSB, -ST_XL_PASS_LSB, ST_XL_PASS_LSB);
    stage_gy_shift(ST_GY_PASS_LSB, -ST_GY_PASS_LSB, ST_GY_PASS_LSB);

    s_mag_id     = (uint8_t)KDRV_QMC5883P_CHIP_ID;
    s_mag_status = QMC_DRDY;
    s_mag_st_on  = false;
    s_mag_running = false;
    s_mag_field[0] = -914;
    s_mag_field[1] = 767;
    s_mag_field[2] = -679;
    s_mag_st_add[0] = 300;
    s_mag_st_add[1] = 300;
    s_mag_st_add[2] = 300;
}

/* ---- Bringing the AO up ------------------------------------------------------ */

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
 * @brief Register AO_Imu once, and construct it afresh for every case.
 *
 * The registration cannot be repeated - QActive_register_() asserts on a
 * priority slot already taken - and the construction has to be, because
 * QASM_INIT() insists the machine is sitting at QHsm_top. The claim flag
 * lives in qf_support because AO_Imu is brought up by more than one file.
 *
 * Leaves the AO where a board is the instant it has booted: both parts
 * configured and resting. Most cases want a board whose IMU something has
 * already asked for and use imu_up() instead.
 */
static void imu_up_raw(void)
{
    qf_support_init();

    /* The tick first, test_haptic_ao.c's rule: QP takes a DISARMED QTimeEvt
     * out of its list only inside the tick, so a case that ended on a disarm
     * leaves it linked, and QTimeEvt_ctorX() over a linked one rewrites the
     * link QF walks - after which the next tick goes round forever.
     *
     * Long enough for a rest one-shot the last case left armed to expire,
     * which is the only way an armed one leaves the list. */
    (void)qf_support_tick(AO_Imu, REST_TICKS + 8U);
    (void)ao_spy();
    ao_spy_subscribe(IMU_TEST_SIG);
    ao_spy_subscribe(MAG_TEST_SIG);
    ao_spy_subscribe(TEST_RESULT_SIG);

    Imu_ctor();
    qf_support_give_queue(AO_Imu);
    AO_Imu->prio  = KODE_PRIO_IMU;
    AO_Imu->pthre = KODE_PRIO_IMU;
    if ( qf_support_claim_ao(AO_Imu) )
    {
        QActive_register_(AO_Imu);
    }
    drain(AO_Imu);
    ao_spy_drain();
    fake_klog_reset();
    QASM_INIT(AO_Imu, (void *)0, 0U);
}

/**
 * @brief Let the gyroscope's turn-on run out.
 *
 * The millisecond clock, not QP's: the AO measures the turn-on against
 * khal_time_ms() and the two clocks in this harness are not joined up, so a
 * case that only ticks would leave the AO believing no time has passed since
 * the gyroscope came on.
 */
static void gyro_settled(void)
{
    fake_time_advance_ms(GYRO_SETTLE_MS);
}

/**
 * @brief imu_up_raw(), then the IMU woken and its gyroscope fully on.
 *
 * One read wakes it, and the read is answered when the turn-on has run out.
 * Its storage is static: the AO holds a copy of the request across those
 * ticks and answers into it long after this helper's frame has gone.
 */
static void imu_up(void)
{
    static kdrv_qmi8658_sample_t s_woke;
    static uint32_t              s_woke_rc;
    ImuReadEvt                   q;

    imu_up_raw();

    q.super = (QEvt)QEVT_INITIALIZER(IMU_READ_REQ_SIG);
    q.out   = &s_woke;
    q.rc    = &s_woke_rc;
    q.done  = NULL;
    q.gen   = 1U;
    q.cur   = NULL;
    q.ack   = NULL;
    q.who   = NULL;
    q.rsp   = NULL;
    QASM_DISPATCH(AO_Imu, &q.super, 0U);
    gyro_settled();
    (void)qf_support_tick(AO_Imu, QF_SUPPORT_STEPS(GYRO_READY_MS) + 2U);
}

/** @brief Was @p text in any line klog captured since the last reset? */
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

/** @brief Take the next event the spy holds and insist it is @p sig. */
static const QEvt *spy_expect(enum_t sig)
{
    const QEvt *e = ao_spy_take();

    TEST_ASSERT_NOT_NULL_MESSAGE(e, "every request is answered");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)sig, (int)e->sig, "the wrong answer");
    return e;
}

/* ---- Asking ------------------------------------------------------------------ */

/**
 * @brief Ask for one sample, and say what the AO wrote back.
 *
 * The status is static storage: a read that finds the gyroscope still
 * turning on is held as a copy and answered ticks later, through this
 * pointer, long after this frame has gone. It reads 0xFFFFFFFF until then.
 */
static uint32_t read_req(kdrv_qmi8658_sample_t *out)
{
    static uint32_t rc;
    ImuReadEvt      q;

    rc = 0xFFFFFFFFU;

    q.super = (QEvt)QEVT_INITIALIZER(IMU_READ_REQ_SIG);
    q.out   = out;
    q.rc    = &rc;
    q.done  = NULL;
    q.gen   = 1U;
    q.cur   = NULL;   /* nobody moves on inside one dispatch */
    q.ack   = NULL;
    q.who   = NULL;
    q.rsp   = NULL;
    QASM_DISPATCH(AO_Imu, &q.super, 0U);
    return rc;
}

/** @brief read_req()'s shape, for the magnetometer's own three axes. */
static uint32_t mag_read_req(int32_t out[3])
{
    MagReadEvt q;
    uint32_t   rc = 0xFFFFFFFFU;

    q.super = (QEvt)QEVT_INITIALIZER(MAG_READ_REQ_SIG);
    q.out   = out;
    q.rc    = &rc;
    q.done  = NULL;
    q.gen   = 1U;
    q.cur   = NULL;
    q.ack   = NULL;
    q.who   = NULL;
    q.rsp   = NULL;
    QASM_DISPATCH(AO_Imu, &q.super, 0U);
    return rc;
}

/** @brief Start the IMU row. */
static void row_req(void)
{
    static const QEvt req = QEVT_INITIALIZER(IMU_TEST_REQ_SIG);

    QASM_DISPATCH(AO_Imu, &req, 0U);
}

/** @brief Start the magnetometer row. */
static void mag_row_req(void)
{
    static const QEvt req = QEVT_INITIALIZER(MAG_TEST_REQ_SIG);

    QASM_DISPATCH(AO_Imu, &req, 0U);
}

/**
 * @brief Let the IMU row run to its verdict.
 *
 * Two self-tests polled every 20 ms and a 200 ms settle after them, so the
 * row is tens of ticks of its own. Generously past that: a row that finished
 * early simply leaves the extra ticks doing nothing.
 */
static void row_run(void)
{
    (void)qf_support_tick(AO_Imu, 200U);
}

/** @brief Ask for a witness capture, and take the answer. */
static const ImuArmedEvt *capture_req(void)
{
    ImuCaptureReqEvt q;

    q.super = (QEvt)QEVT_INITIALIZER(IMU_CAPTURE_REQ_SIG);
    q.who   = ao_spy();
    QASM_DISPATCH(AO_Imu, &q.super, 0U);
    return (const ImuArmedEvt *)spy_expect(IMU_ARMED_SIG);
}

/** @brief Ask for the drain, run the chunks out, and take the answer. */
static const ImuEnergyEvt *drain_req(uint16_t hz)
{
    ImuDrainReqEvt q;

    q.super = (QEvt)QEVT_INITIALIZER(IMU_DRAIN_REQ_SIG);
    q.who   = ao_spy();
    q.hz    = hz;
    QASM_DISPATCH(AO_Imu, &q.super, 0U);
    /* A full window is eight chunks and each one is a tick behind the last.
     * Well short of the capture deadline, which must not expire in the
     * middle of it. */
    (void)qf_support_tick(AO_Imu, 32U);
    return (const ImuEnergyEvt *)spy_expect(IMU_ENERGY_SIG);
}

/** @brief Take the next record of a row, and insist on its step and phase. */
static const ImuTestEvt *row_record(uint8_t step, uint8_t phase)
{
    const ImuTestEvt * const r = (const ImuTestEvt *)spy_expect(IMU_TEST_SIG);

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(step, r->step, "the wrong step");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(phase, r->phase, "the wrong phase");
    return r;
}

/** @brief The same, for the magnetometer row. */
static const MagTestEvt *mag_record(uint8_t step, uint8_t phase)
{
    const MagTestEvt * const r = (const MagTestEvt *)spy_expect(MAG_TEST_SIG);

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(step, r->step, "the wrong step");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(phase, r->phase, "the wrong phase");
    return r;
}

/** @brief Take a row's verdict and insist it is @p name's and @p ok. */
static void expect_verdict(const char *name, bool ok)
{
    const TestResultEvt * const v =
        (const TestResultEvt *)spy_expect(TEST_RESULT_SIG);

    TEST_ASSERT_EQUAL_STRING_MESSAGE(name, v->name, "the wrong row answered");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)ok, (int)v->ok, "the wrong verdict");
}

/**
 * @brief Give every event this case is still holding back to the pool.
 *
 * The harness has one pool of thirty-two blocks for the whole binary, and a
 * reference held past the last case in a file is a block no later file can
 * allocate - which shows up as a publish that silently did not happen,
 * hundreds of cases away.
 */
static void imu_down(void)
{
    /* Both parts back to rest while the bus is still there to take the
     * write, so no case hands the next one an armed one-shot. */
    (void)qf_support_tick(AO_Imu, REST_TICKS);
    ao_spy_drain();
    drain(AO_Imu);
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* ---- A board with neither part ------------------------------------------------
 * These run first in the binary on purpose. Nothing here
 * lets either create() succeed, so both drivers are still down when the case
 * after this one hands them their one bring-up.
 */

TEST_CASE("no QMI8658A: the AO says so at boot and refuses every read",
          "[imu_ao]")
{
    kdrv_qmi8658_sample_t s;

    bus_up(false, false);
    imu_up();

    TEST_ASSERT_TRUE_MESSAGE(logged("no QMI8658A at 0x6A"),
                             "a board with no IMU boots silently");
    TEST_ASSERT_TRUE_MESSAGE(logged("no QMC5883P at 0x2C"),
                             "a board with no magnetometer boots silently");
    TEST_ASSERT_FALSE_MESSAGE(logged("both resting"),
                              "the AO reported two parts it never found");

    /* Not a struct of zeroes: an absent chip has nothing to say about which
     * way up the board is, and a reading of zero is an opinion. */
    (void)memset(&s, 0x5A, sizeof(s));
    TEST_ASSERT_EQUAL_INT(KODE_IO_ERROR, read_req(&s));
    TEST_ASSERT_EQUAL_INT32(0x5A5A5A5A, s.a_mg[2]);

    {
        int32_t mg[3] = { 11, 22, 33 };

        TEST_ASSERT_EQUAL_INT(KODE_IO_ERROR, mag_read_req(mg));
        TEST_ASSERT_EQUAL_INT32(11, mg[0]);
    }
    imu_down();
}

TEST_CASE("no QMI8658A: the row stops at step 1 rather than measuring gravity "
          "through it", "[imu_ao]")
{
    bus_up(false, false);
    imu_up();

    row_req();
    {
        const ImuTestEvt * const r = row_record(1U, (uint8_t)IMU_TEST_WHOAMI);

        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0U, r->flags,
                                        "a part that never answered passed");
        TEST_ASSERT_EQUAL_INT32_MESSAGE(0, r->a,
                                        "a WHO_AM_I nobody read was reported");
    }
    expect_verdict("imu", false);
    TEST_ASSERT_TRUE(logged("WHO_AM_I did not answer"));
    TEST_ASSERT_TRUE(logged("does not answer at 0x6A"));

    /* And no step 2 or 3: the self-test is not run through a part that is
     * not there, and gravity through a dead accelerometer is a number. */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "the row carried on past an absent part");
    imu_down();
}

TEST_CASE("no QMC5883P: the row says absent and ends there", "[imu_ao]")
{
    bus_up(false, false);
    imu_up();

    mag_row_req();
    {
        const MagTestEvt * const r = mag_record(1U, (uint8_t)MAG_TEST_WHOAMI);

        TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)MAG_TF_ABSENT, r->flags,
                                        "an absent part is not told from one "
                                        "that named itself wrong");
    }
    /* The end marker before the verdict, so the console knows the records are
     * over before it prints one. */
    (void)mag_record(0U, (uint8_t)MAG_TEST_DONE);
    expect_verdict("mag", false);
    TEST_ASSERT_TRUE(logged("the chip id did not answer"));
    imu_down();
}

TEST_CASE("the QMC5883P at 0x0C: the row names the address and ends there",
          "[imu_ao]")
{
    bus_up(false, false);
    s_mag_stray = true;
    imu_up();

    mag_row_req();
    {
        const MagTestEvt * const r = mag_record(1U, (uint8_t)MAG_TEST_WHOAMI);

        TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)MAG_TF_MOVED, r->flags,
                                        "a part at the wrong address was "
                                        "reported absent");
        TEST_ASSERT_EQUAL_INT32((int32_t)KBSP_I2C_ADDR_MAG_STRAY, r->a);
    }
    (void)mag_record(0U, (uint8_t)MAG_TEST_DONE);
    expect_verdict("mag", false);
    TEST_ASSERT_TRUE(logged("answers at 0x0C, not 0x2C"));
    TEST_ASSERT_TRUE(logged("answers at another address"));
    imu_down();
}

/* ---- kode_sys's requests: answered to the asking AO --------------------------
 * kode_sys_imu_read(), kode_sys_mag_read() and the calibration requests post
 * and return; the answer is an event posted to the AO that asked, the spy
 * here, carrying the status and a copy. The no-part case sits in this group
 * because only here are both drivers still down.
 */

/** @brief Dispatch what kode_sys posted to AO_Imu, and take the one answer the spy got. */
static const void *sys_answer(enum_t sig)
{
    (void)qf_support_settle(AO_Imu);
    return (const void *)spy_expect(sig);
}

/** @brief AO_Imu up for a kode_sys case: its queue one a margin post can read. */
static void sys_up(void)
{
    qf_support_margin_queue(AO_Imu);
    ao_spy_drain();
}

TEST_CASE("no part fitted: kode_sys's motion reads are answered with the status",
          "[imu_ao]")
{
    const kode_sys_imu_answer_t *a;
    const kode_sys_mag_answer_t *m;

    bus_up(false, false);
    imu_up_raw();
    sys_up();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_imu_read(ao_spy()));
    a = (const kode_sys_imu_answer_t *)sys_answer(IMU_SAMPLE_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT_MESSAGE((uint32_t)KODE_IO_ERROR, a->rc,
                                   "an absent IMU still answers, with the status");
    TEST_ASSERT_EQUAL_INT32(0, a->sample.a_mg[0]);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_mag_read(ao_spy()));
    m = (const kode_sys_mag_answer_t *)sys_answer(MAG_SAMPLE_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_IO_ERROR, m->rc);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "one answer per request");
    ao_spy_release();
    imu_down();
}

/* ---- The one bring-up, handed to the drivers' own case files ----------------- */

TEST_CASE("both parts identify themselves, take their configuration, and the "
          "AO reports them resting", "[imu_ao]")
{
    /* The two helpers are test_kdrv_qmi8658.c's and test_kdrv_qmc5883p.c's
     * own, and every assertion they make is theirs: what answers before the
     * part does, what an absent one says, what a foreign one says, and every
     * byte the first create() writes. They are called from here because this
     * file runs first and would otherwise spend the one moment they can be
     * made. Idempotent, so the driver cases call them again for nothing. */
    Mockkhal_i2c_Init();
    test_qmi8658_bring_up();
    test_qmc5883p_bring_up();
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();

    /* And now the AO over them, both up. */
    bus_up(true, true);
    imu_up_raw();
    TEST_ASSERT_TRUE_MESSAGE(logged("both resting until something asks"),
                             "the AO did not report the two parts it found");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00U, s_regs[QMI_CTRL7],
                                   "the IMU converts on a board nobody has "
                                   "asked anything of");
    TEST_ASSERT_FALSE_MESSAGE(s_mag_running,
                              "the magnetometer converts on a board nobody "
                              "has asked anything of");
    TEST_ASSERT_FALSE(logged("no QMI8658A"));
    TEST_ASSERT_FALSE(logged("no QMC5883P"));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("AO_Imu",
                                     kbsp_owner_name(KBSP_RES_IMU),
                                     "the IMU has no owner, or the wrong one");
    imu_down();
}

/* ---- A sample ---------------------------------------------------------------- */

TEST_CASE("a read is answered inside the step that receives it, from the part",
          "[imu_ao]")
{
    kdrv_qmi8658_sample_t s;
    /* Whole numbers of LSB in both directions - 4096 per g, 64 per dps - so
     * these read back exactly and the case is about the answer rather than
     * about where two truncations landed. */
    int32_t const         a[3] = { 125, -250, 1000 };
    int32_t const         w[3] = { 1000, -2000, 0 };

    bus_up(true, true);
    imu_up();
    stage_sample(a, w, 265);

    (void)memset(&s, 0, sizeof(s));
    TEST_ASSERT_EQUAL_INT(KODE_OK, read_req(&s));
    /* The scales are the part's own, so these numbers are the driver's
     * arithmetic as much as the AO's. */
    TEST_ASSERT_EQUAL_INT32(125, s.a_mg[0]);
    TEST_ASSERT_EQUAL_INT32(-250, s.a_mg[1]);
    TEST_ASSERT_EQUAL_INT32(1000, s.a_mg[2]);
    TEST_ASSERT_EQUAL_INT32(1000, s.w_mdps[0]);
    TEST_ASSERT_EQUAL_INT32(-2000, s.w_mdps[1]);
    TEST_ASSERT_EQUAL_INT32(265, s.temp_dc);
    imu_down();
}

TEST_CASE("a field read is the magnetometer's own three axes, in milligauss",
          "[imu_ao]")
{
    int32_t mg[3] = { 0, 0, 0 };

    bus_up(true, true);
    imu_up();

    TEST_ASSERT_EQUAL_INT(KODE_OK, mag_read_req(mg));
    TEST_ASSERT_EQUAL_INT32(-914, mg[0]);
    TEST_ASSERT_EQUAL_INT32(767, mg[1]);
    TEST_ASSERT_EQUAL_INT32(-679, mg[2]);

    /* A conversion that never arrives is said rather than invented. */
    s_mag_status = 0U;
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, mag_read_req(mg));
    TEST_ASSERT_EQUAL_INT32_MESSAGE(-914, mg[0],
                                    "a refused read wrote the caller's axes");
    imu_down();
}

/* ---- Rest --------------------------------------------------------------------
 * A part converts for whoever asked and for KODE_SYS_IMU_REST_AFTER_MS after the
 * last time anybody did. These are the cases that say so; everything after
 * them starts from imu_up(), an IMU something has already woken.
 */

TEST_CASE("a read that finds the IMU resting wakes it and is answered once "
          "the gyroscope is on", "[imu_ao]")
{
    static kdrv_qmi8658_sample_t s;
    static uint32_t              rc;
    ImuReadEvt                   q;

    bus_up(true, true);
    imu_up_raw();
    (void)memset(&s, 0x5A, sizeof(s));
    rc = 0xFFFFFFFFU;

    q.super = (QEvt)QEVT_INITIALIZER(IMU_READ_REQ_SIG);
    q.out   = &s;
    q.rc    = &rc;
    q.done  = NULL;
    q.gen   = 1U;
    q.cur   = NULL;
    q.ack   = NULL;
    q.who   = NULL;
    q.rsp   = NULL;
    QASM_DISPATCH(AO_Imu, &q.super, 0U);

    TEST_ASSERT_EQUAL_HEX8_MESSAGE(QMI_SENSORS_ON, s_regs[QMI_CTRL7],
                                   "the read did not wake the part");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0xFFFFFFFFU, rc,
                                     "answered before the gyroscope's sample "
                                     "meant anything");

    /* One tick short of the turn-on: still held. */
    (void)qf_support_tick(AO_Imu, QF_SUPPORT_STEPS(GYRO_READY_MS) - 1U);
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFU, rc);

    (void)qf_support_tick(AO_Imu, 2U);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)KODE_OK, rc,
                                     "the held read was never answered");
    TEST_ASSERT_EQUAL_INT32(1000, s.a_mg[2]);
    imu_down();
}

TEST_CASE("one read is held at a time; a second with a caller is refused",
          "[imu_ao]")
{
    static kdrv_qmi8658_sample_t s;
    static uint32_t              rc;
    kdrv_qmi8658_sample_t        other;
    ImuReadEvt                   q;

    bus_up(true, true);
    imu_up_raw();
    rc = 0xFFFFFFFFU;

    q.super = (QEvt)QEVT_INITIALIZER(IMU_READ_REQ_SIG);
    q.out   = &s;
    q.rc    = &rc;
    q.done  = NULL;
    q.gen   = 1U;
    q.cur   = NULL;
    q.ack   = NULL;
    q.who   = NULL;
    q.rsp   = NULL;
    QASM_DISPATCH(AO_Imu, &q.super, 0U);

    (void)memset(&other, 0, sizeof(other));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, read_req(&other));

    (void)qf_support_tick(AO_Imu, QF_SUPPORT_STEPS(GYRO_READY_MS) + 2U);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)KODE_OK, rc,
                                     "the refusal took the held read with it");
    imu_down();
}

TEST_CASE("a held read whose caller left gives its place to the next one",
          "[imu_ao]")
{
    static kdrv_qmi8658_sample_t first;
    static kdrv_qmi8658_sample_t second;
    static uint32_t              rc1;
    static uint32_t              rc2;
    static uint32_t              cur;
    ImuReadEvt                   q;

    bus_up(true, true);
    imu_up_raw();
    rc1 = 0xFFFFFFFFU;
    rc2 = 0xFFFFFFFFU;
    cur = 1U;

    q.super = (QEvt)QEVT_INITIALIZER(IMU_READ_REQ_SIG);
    q.out   = &first;
    q.rc    = &rc1;
    q.done  = NULL;
    q.gen   = 1U;
    q.cur   = &cur;
    q.ack   = NULL;
    q.who   = NULL;
    q.rsp   = NULL;
    QASM_DISPATCH(AO_Imu, &q.super, 0U);

    /* The first caller's wait ran out and the next one took the claim. */
    cur   = 2U;
    q.out = &second;
    q.rc  = &rc2;
    q.gen = 2U;
    QASM_DISPATCH(AO_Imu, &q.super, 0U);

    (void)qf_support_tick(AO_Imu, QF_SUPPORT_STEPS(GYRO_READY_MS) + 2U);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0xFFFFFFFFU, rc1,
                                     "a read nobody waits for was answered");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)KODE_OK, rc2,
                                     "the caller that is waiting was not");
    imu_down();
}

TEST_CASE("an IMU nobody asks for a whole rest time goes back to standby",
          "[imu_ao]")
{
    kdrv_qmi8658_sample_t s;

    bus_up(true, true);
    imu_up();
    TEST_ASSERT_EQUAL_HEX8(QMI_SENSORS_ON, s_regs[QMI_CTRL7]);

    /* A read part way through starts the time again. Sixty ticks short of
     * the whole of it, because imu_up() spent some of this one already. */
    (void)qf_support_tick(AO_Imu, REST_TICKS - 60U);
    (void)memset(&s, 0, sizeof(s));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, read_req(&s),
                                  "a read inside the rest time found the "
                                  "part resting");
    (void)qf_support_tick(AO_Imu, REST_TICKS - 60U);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(QMI_SENSORS_ON, s_regs[QMI_CTRL7],
                                   "the part rested while it was being read");

    /* ...and silence ends it. */
    (void)qf_support_tick(AO_Imu, 64U);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00U, s_regs[QMI_CTRL7],
                                   "the part converts for nobody");
    imu_down();
}

TEST_CASE("the row on a resting IMU starts its self-test at once, without "
          "waking the part first", "[imu_ao]")
{
    bus_up(true, true);
    imu_up_raw();
    s_st_polls = 0U;

    row_req();
    /* Inside the request's own step: nothing was waited for, and nothing
     * was switched on. */
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00U, s_regs[QMI_CTRL7],
                                   "the part was switched on before a "
                                   "self-test that starts from sensors off");
    TEST_ASSERT_TRUE_MESSAGE(s_st_armed, "the self-test was not triggered");

    row_run();
    (void)row_record(1U, (uint8_t)IMU_TEST_WHOAMI);
    (void)row_record(2U, (uint8_t)IMU_TEST_ST);
    (void)row_record(2U, (uint8_t)IMU_TEST_ST);
    (void)row_record(3U, (uint8_t)IMU_TEST_GRAVITY);
    expect_verdict("imu", true);
    /* And the row hands the part back running, to rest on its own time. */
    TEST_ASSERT_EQUAL_HEX8(QMI_SENSORS_ON, s_regs[QMI_CTRL7]);
    imu_down();
}

TEST_CASE("the magnetometer wakes for a read and rests on its own time",
          "[imu_ao]")
{
    int32_t mg[3] = { 0, 0, 0 };

    bus_up(true, true);
    imu_up_raw();
    TEST_ASSERT_FALSE(s_mag_running);

    TEST_ASSERT_EQUAL_INT(KODE_OK, mag_read_req(mg));
    TEST_ASSERT_TRUE_MESSAGE(s_mag_running, "the read did not wake the part");
    TEST_ASSERT_EQUAL_INT32(-914, mg[0]);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00U, s_regs[QMI_CTRL7],
                                   "a field read woke the IMU as well");

    (void)qf_support_tick(AO_Imu, REST_TICKS - 20U);
    TEST_ASSERT_TRUE(s_mag_running);
    (void)qf_support_tick(AO_Imu, 24U);
    TEST_ASSERT_FALSE_MESSAGE(s_mag_running,
                              "the magnetometer converts for nobody");
    imu_down();
}

/**
 * @brief A read whose caller has already moved on, made the way the facade
 *        makes one: allocated, with the tag it is waiting on now.
 *
 * @param gen The request's own tag.
 * @param cur The tag the requester is waiting on now.
 * @param out Where a sample would land.
 * @param rc  Where a status would land.
 */
static void read_post(uint32_t gen, uint32_t *cur, kdrv_qmi8658_sample_t *out,
                      uint32_t *rc)
{
    imu_read_request(out, rc, khal_sem_create(), gen, cur, NULL);
    (void)qf_support_pump(AO_Imu, 8U);
}

/* The facade answers every read into ONE sample, one status and one
 * acknowledgement, whichever caller asked - so a request whose caller's
 * bounded wait already ran out must be dropped whole rather than answered
 * over the newer caller's own answer. */
TEST_CASE("a read its caller stopped waiting for is discarded, unanswered",
          "[imu_ao]")
{
    kdrv_qmi8658_sample_t s;
    kdrv_qmi8658_sample_t sentinel;
    uint32_t              rc  = 0xFFFFFFFFU;
    uint32_t              cur = 9U;

    bus_up(true, true);
    imu_up();
    fake_sem_reset();

    (void)memset(&s, 0x5A, sizeof(s));
    (void)memset(&sentinel, 0x5A, sizeof(sentinel));

    read_post(7U, &cur, &s, &rc);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&sentinel, &s, sizeof(s),
                                     "an abandoned read was answered into "
                                     "storage its caller had moved on from");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0xFFFFFFFFU, rc,
                                     "an abandoned read wrote a status");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_sem_signals(),
                                   "an abandoned read woke somebody");
    TEST_ASSERT_TRUE(logged("stopped waiting for is discarded"));

    /* The same request with its own tag current IS answered, so the case
     * above is about the tag rather than about a read that never worked. */
    cur = 7U;
    read_post(7U, &cur, &s, &rc);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, rc);
    TEST_ASSERT_EQUAL_INT32(1000, s.a_mg[2]);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_sem_signals(),
                                   "the caller was left asleep");
    imu_down();
}

/* ---- The IMU row --------------------------------------------------------------
 * Three steps, and the order is fault localisation: identity, then the
 * manufacturer's self-test - the one measurement on this board that does not
 * depend on how it is being held - and only then gravity, which an operator
 * can invalidate by moving.
 */

TEST_CASE("the row names the part, judges both self-tests and then measures "
          "the bench at rest", "[imu_ao]")
{
    bus_up(true, true);
    imu_up();
    s_st_polls = 1U;   /* the part takes one poll to finish each self-test */

    row_req();
    row_run();

    {
        const ImuTestEvt * const r = row_record(1U, (uint8_t)IMU_TEST_WHOAMI);

        TEST_ASSERT_EQUAL_UINT8((uint8_t)IMU_TF_PASS, r->flags);
        TEST_ASSERT_EQUAL_INT32((int32_t)KDRV_QMI8658_WHOAMI, r->a);
    }
    {
        const ImuTestEvt * const r = row_record(2U, (uint8_t)IMU_TEST_ST);

        TEST_ASSERT_EQUAL_UINT8((uint8_t)IMU_TF_PASS, r->flags);
        TEST_ASSERT_EQUAL_INT32_MESSAGE((int32_t)KDRV_QMI8658_XL, r->d,
                                        "the accelerometer is judged first");
        TEST_ASSERT_EQUAL_INT32(ST_XL_PASS_MG, r->a);
        TEST_ASSERT_EQUAL_INT32(-ST_XL_PASS_MG, r->b);
    }
    {
        const ImuTestEvt * const r = row_record(2U, (uint8_t)IMU_TEST_ST);

        TEST_ASSERT_EQUAL_UINT8((uint8_t)IMU_TF_PASS, r->flags);
        TEST_ASSERT_EQUAL_INT32_MESSAGE((int32_t)KDRV_QMI8658_GY, r->d,
                                        "the gyroscope is judged second");
        TEST_ASSERT_EQUAL_INT32(ST_GY_PASS_MDPS, r->a);
        TEST_ASSERT_EQUAL_INT32(-ST_GY_PASS_MDPS, r->b);
    }
    {
        const ImuTestEvt * const r = row_record(3U, (uint8_t)IMU_TEST_GRAVITY);

        TEST_ASSERT_EQUAL_UINT8((uint8_t)IMU_TF_PASS, r->flags);
        TEST_ASSERT_EQUAL_INT32_MESSAGE(1000, r->a, "|a| is not one g");
        TEST_ASSERT_EQUAL_INT32(0, r->b);
        TEST_ASSERT_EQUAL_INT32(265, r->c);
    }
    expect_verdict("imu", true);
    TEST_ASSERT_TRUE(logged("test imu: PASS"));

    /* The part is back as it runs: a sample after the row is a sample,
     * once the gyroscope the row switched back on has turned on. */
    gyro_settled();
    {
        kdrv_qmi8658_sample_t s;

        (void)memset(&s, 0, sizeof(s));
        TEST_ASSERT_EQUAL_INT(KODE_OK, read_req(&s));
    }
    imu_down();
}

/* A part that answers at 0x6A and is not a QMI8658A belongs to
 * test_kdrv_qmi8658.c's own bring-up: once a create() has succeeded the
 * driver answers from what it read then, so the row cannot be shown a
 * different WHO_AM_I from here. */

TEST_CASE("a sensor that does not move under its own stimulus fails the row",
          "[imu_ao]")
{
    bus_up(true, true);
    imu_up();
    s_st_polls = 0U;
    stage_xl_shift(ST_XL_PASS_LSB, ST_XL_FAIL_LSB, ST_XL_PASS_LSB);

    row_req();
    row_run();

    (void)row_record(1U, (uint8_t)IMU_TEST_WHOAMI);
    {
        const ImuTestEvt * const r = row_record(2U, (uint8_t)IMU_TEST_ST);

        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0U, r->flags,
                                        "125 mg on one axis passed a 200 mg "
                                        "floor");
        TEST_ASSERT_EQUAL_INT32(ST_XL_FAIL_MG, r->b);
    }
    expect_verdict("imu", false);
    TEST_ASSERT_TRUE(logged("did not move under its own self-test"));
    /* And the gyroscope is never asked: the row stops where it stopped. */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "the row carried on past a dead sensor");
    imu_down();
}

TEST_CASE("the gyroscope is judged by its own floor, not the accelerometer's",
          "[imu_ao]")
{
    bus_up(true, true);
    imu_up();
    s_st_polls = 0U;
    /* 100 dps: well past 200 mg read as a number, and well short of the
     * datasheet's own 300 dps. */
    stage_gy_shift(ST_GY_FAIL_LSB, ST_GY_FAIL_LSB, ST_GY_FAIL_LSB);

    row_req();
    row_run();

    (void)row_record(1U, (uint8_t)IMU_TEST_WHOAMI);
    (void)row_record(2U, (uint8_t)IMU_TEST_ST);
    {
        const ImuTestEvt * const r = row_record(2U, (uint8_t)IMU_TEST_ST);

        TEST_ASSERT_EQUAL_INT32((int32_t)KDRV_QMI8658_GY, r->d);
        TEST_ASSERT_EQUAL_UINT8(0U, r->flags);
        TEST_ASSERT_EQUAL_INT32(ST_GY_FAIL_MDPS, r->a);
    }
    expect_verdict("imu", false);
    TEST_ASSERT_TRUE(logged("gyroscope did not move"));
    imu_down();
}

TEST_CASE("a self-test that never reports finishing is a timeout, not a hang",
          "[imu_ao]")
{
    bus_up(true, true);
    imu_up();
    /* Further polls than the row is willing to make. */
    s_st_polls = 10000U;

    row_req();
    row_run();

    (void)row_record(1U, (uint8_t)IMU_TEST_WHOAMI);
    expect_verdict("imu", false);
    TEST_ASSERT_TRUE(logged("never reported finishing"));
    imu_down();
}

/** @brief Run the row to its verdict over a bench staged to fail step 3. */
static void rest_row_fails(const char *why)
{
    s_st_polls = 0U;
    row_req();
    row_run();

    (void)row_record(1U, (uint8_t)IMU_TEST_WHOAMI);
    (void)row_record(2U, (uint8_t)IMU_TEST_ST);
    (void)row_record(2U, (uint8_t)IMU_TEST_ST);
    {
        const ImuTestEvt * const r = row_record(3U, (uint8_t)IMU_TEST_GRAVITY);

        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0U, r->flags,
                                        "step 3 passed a bench it should not");
    }
    expect_verdict("imu", false);
    TEST_ASSERT_TRUE_MESSAGE(logged(why), "the operator is not told why");
}

TEST_CASE("gravity that is not one g fails step 3", "[imu_ao]")
{
    int32_t const a[3] = { 0, 0, 1000 + (int32_t)KODE_IMU_GRAVITY_TOL_MG + 10 };
    int32_t const w[3] = { 0, 0, 0 };

    bus_up(true, true);
    imu_up();
    stage_sample(a, w, 265);
    rest_row_fails("gravity is not one g");
    imu_down();
}

TEST_CASE("rotation on a bench that is not moving fails step 3", "[imu_ao]")
{
    int32_t const a[3] = { 0, 0, 1000 };
    int32_t const w[3] = { 0, 0, (int32_t)KODE_IMU_REST_MAX_MDPS + 1000 };

    bus_up(true, true);
    imu_up();
    stage_sample(a, w, 265);
    rest_row_fails("hold the board still");
    imu_down();
}

TEST_CASE("a die temperature that is not a room's fails step 3", "[imu_ao]")
{
    int32_t const a[3] = { 0, 0, 1000 };
    int32_t const w[3] = { 0, 0, 0 };

    bus_up(true, true);
    imu_up();
    stage_sample(a, w, (int32_t)KODE_IMU_TEMP_MAX_DC + 10);
    rest_row_fails("not a room's");
    imu_down();
}

/* ---- The magnetometer row ------------------------------------------------------
 * Identity, a self-test shift judged against a floor an axis that works clears
 * many times over - the datasheet gives the procedure and no threshold - and
 * the field at rest, which is a reading rather than a verdict. The whole row
 * runs inside one step.
 */

TEST_CASE("the mag row names the part, logs its shift and reports the field",
          "[imu_ao]")
{
    bus_up(true, true);
    imu_up();

    mag_row_req();
    {
        const MagTestEvt * const r = mag_record(1U, (uint8_t)MAG_TEST_WHOAMI);

        TEST_ASSERT_EQUAL_UINT8((uint8_t)MAG_TF_PASS, r->flags);
        TEST_ASSERT_EQUAL_INT32((int32_t)KDRV_QMC5883P_CHIP_ID, r->a);
    }
    {
        const MagTestEvt * const r = mag_record(2U, (uint8_t)MAG_TEST_ST);

        TEST_ASSERT_EQUAL_UINT8((uint8_t)MAG_TF_PASS, r->flags);
        TEST_ASSERT_INT32_WITHIN(1, 300, (0 > r->a) ? -r->a : r->a);
        TEST_ASSERT_INT32_WITHIN(1, 300, (0 > r->b) ? -r->b : r->b);
        TEST_ASSERT_INT32_WITHIN(1, 300, (0 > r->c) ? -r->c : r->c);
    }
    {
        const MagTestEvt * const r = mag_record(3U, (uint8_t)MAG_TEST_FIELD);

        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0U, r->flags,
                                        "a reading carries a verdict");
        TEST_ASSERT_EQUAL_INT32(-914, r->a);
        TEST_ASSERT_EQUAL_INT32(767, r->b);
        TEST_ASSERT_EQUAL_INT32(-679, r->c);
        TEST_ASSERT_EQUAL_INT32_MESSAGE(1372, r->d, "|B| is not the magnitude");
    }
    (void)mag_record(0U, (uint8_t)MAG_TEST_DONE);
    expect_verdict("mag", true);

    imu_down();
}

/* An axis the stimulus does not move is a dead one: the part still names
 * itself and still converts, which is all the row used to ask of it. */
TEST_CASE("an axis that does not move under the self-test fails the mag row",
          "[imu_ao]")
{
    bus_up(true, true);
    imu_up();
    s_mag_st_add[1] = 20;

    mag_row_req();
    (void)mag_record(1U, (uint8_t)MAG_TEST_WHOAMI);
    {
        const MagTestEvt * const r = mag_record(2U, (uint8_t)MAG_TEST_ST);

        TEST_ASSERT_EQUAL_UINT8(0U, r->flags);
        TEST_ASSERT_INT32_WITHIN(1, 20, (0 > r->b) ? -r->b : r->b);
    }
    (void)mag_record(0U, (uint8_t)MAG_TEST_DONE);
    expect_verdict("mag", false);
    imu_down();
}

/* A part that answers at 0x2C and is not a QMC5883P belongs to
 * test_kdrv_qmc5883p.c's own bring-up, for the reason the IMU row's own
 * absence of that case gives. */

TEST_CASE("a part that identifies itself and then gives no conversion fails",
          "[imu_ao]")
{
    bus_up(true, true);
    imu_up();
    s_mag_status = 0U;   /* nothing ever becomes ready */

    mag_row_req();
    (void)mag_record(1U, (uint8_t)MAG_TEST_WHOAMI);
    /* The self-test is the first thing to ask the part for a conversion. */
    (void)mag_record(2U, (uint8_t)MAG_TEST_ST);
    (void)mag_record(0U, (uint8_t)MAG_TEST_DONE);
    expect_verdict("mag", false);
    TEST_ASSERT_TRUE(logged("the self-test could not be run"));
    imu_down();
}

/* ---- The witness window --------------------------------------------------------
 * AO_Haptic drives the motor and asks for this: one FIFO window of the
 * accelerometer, measured at the frequency it drove. One at a time, because
 * the FIFO is one buffer and two readers would each get half of the other's
 * window without knowing it.
 */

TEST_CASE("a window is armed, drained a chunk at a time and measured at the "
          "bin it was asked about", "[imu_ao]")
{
    bus_up(true, true);
    imu_up();

    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, capture_req()->rc);
    {
        const ImuEnergyEvt * const e = drain_req((uint16_t)WITNESS_HZ);

        TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, e->rc);
        TEST_ASSERT_EQUAL_UINT16_MESSAGE((uint16_t)KDRV_QMI8658_FIFO_DEPTH,
                                         e->n,
                                         "the window was not drained whole");
        /* The detector's own numbers, in tenths of a milli-g: a 100 mg sine
         * on y is 1000 in the bin and nowhere else. */
        TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)WITNESS_AXIS, e->axis,
                                        "the bin was found on the wrong axis");
        TEST_ASSERT_INT_WITHIN_MESSAGE(150, 10 * WITNESS_MG, e->bin_x10,
                                       "the bin is not the sine that was in "
                                       "the window");
        TEST_ASSERT_TRUE_MESSAGE(0 < e->rms_x10,
                                 "a window with a sine in it reads no RMS");
    }

    /* And the part is back as it runs: a sample after a window is a sample,
     * once the gyroscope the window switched back on has turned on. */
    gyro_settled();
    {
        kdrv_qmi8658_sample_t s;

        (void)memset(&s, 0, sizeof(s));
        TEST_ASSERT_EQUAL_INT(KODE_OK, read_req(&s));
        TEST_ASSERT_EQUAL_INT32(1000, s.a_mg[2]);
    }
    imu_down();
}

TEST_CASE("a second requester is refused rather than queued behind the first",
          "[imu_ao]")
{
    ImuCaptureReqEvt q;

    bus_up(true, true);
    imu_up();

    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, capture_req()->rc);

    /* A capture is about a window of time that has already started, and a
     * refusal the caller can act on is worth more than a turn it cannot
     * date. */
    q.super = (QEvt)QEVT_INITIALIZER(IMU_CAPTURE_REQ_SIG);
    q.who   = ao_spy();
    QASM_DISPATCH(AO_Imu, &q.super, 0U);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_DENIED,
                            ((const ImuArmedEvt *)spy_expect(IMU_ARMED_SIG))->rc);

    (void)drain_req((uint16_t)WITNESS_HZ);
    imu_down();
}

TEST_CASE("a window nobody drains is taken back by its own deadline",
          "[imu_ao]")
{
    bus_up(true, true);
    imu_up();

    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, capture_req()->rc);

    /* Just short of it: still held, so the deadline is what ends it rather
     * than the first tick that comes along. */
    (void)qf_support_tick(AO_Imu,
                          QF_SUPPORT_STEPS(KODE_IMU_CAPTURE_MAX_MS) - 2U);
    TEST_ASSERT_FALSE_MESSAGE(logged("never drained"),
                              "the window was closed early");

    (void)qf_support_tick(AO_Imu, 4U);
    TEST_ASSERT_TRUE_MESSAGE(logged("armed and never drained"),
                             "a requester that died holds the part for the "
                             "rest of the boot");

    /* Given back whole: the next capture is granted, once the gyroscope the
     * closed window switched back on has finished turning on. */
    gyro_settled();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, capture_req()->rc);
    (void)drain_req((uint16_t)WITNESS_HZ);
    imu_down();
}

/* Measured on the bench: a FIFO reset asked for while the gyroscope is
 * turning back on is acknowledged and not acted on, and the window never
 * fills - which is exactly what the haptic row does, two windows a few
 * milliseconds apart. So the second one waits, and its requester is answered
 * when the part will act rather than told a lie it can only find out about
 * by measuring nothing. */
TEST_CASE("a capture asked for while the gyroscope is turning on waits for it",
          "[imu_ao]")
{
    ImuCaptureReqEvt q;

    bus_up(true, true);
    imu_up_raw();   /* a resting IMU: the capture is what switches it on */

    q.super = (QEvt)QEVT_INITIALIZER(IMU_CAPTURE_REQ_SIG);
    q.who   = ao_spy();
    QASM_DISPATCH(AO_Imu, &q.super, 0U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "the arm was answered before the part "
                                   "would have acted on it");

    /* A sample meanwhile is refused: the part is held for the capture that
     * is about to be armed, not free. */
    {
        kdrv_qmi8658_sample_t s;

        (void)memset(&s, 0, sizeof(s));
        TEST_ASSERT_EQUAL_INT(KODE_DENIED, read_req(&s));
    }

    (void)qf_support_tick(AO_Imu, QF_SUPPORT_STEPS(GYRO_SETTLE_MS) + 2U);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(
        (uint8_t)KODE_OK,
        ((const ImuArmedEvt *)spy_expect(IMU_ARMED_SIG))->rc,
        "the window was never armed once the wait was over");

    (void)drain_req((uint16_t)WITNESS_HZ);
    imu_down();
}

TEST_CASE("a drain nobody armed is refused rather than measured", "[imu_ao]")
{
    ImuDrainReqEvt q;

    bus_up(true, true);
    imu_up();

    q.super = (QEvt)QEVT_INITIALIZER(IMU_DRAIN_REQ_SIG);
    q.who   = ao_spy();
    q.hz    = (uint16_t)WITNESS_HZ;
    QASM_DISPATCH(AO_Imu, &q.super, 0U);
    {
        const ImuEnergyEvt * const e =
            (const ImuEnergyEvt *)spy_expect(IMU_ENERGY_SIG);

        TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_DENIED, e->rc);
        TEST_ASSERT_EQUAL_UINT16(0U, e->n);
    }
    imu_down();
}

/* ---- What a held part refuses --------------------------------------------------
 * The rates and the scales belong to whatever holds the IMU, and a number at
 * the wrong scale is worse than a refusal because it looks right.
 */

TEST_CASE("a sample is refused KODE_DENIED while a window holds the part",
          "[imu_ao]")
{
    kdrv_qmi8658_sample_t s;
    kdrv_qmi8658_sample_t sentinel;

    bus_up(true, true);
    imu_up();

    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, capture_req()->rc);
    (void)memset(&s, 0x5A, sizeof(s));
    (void)memset(&sentinel, 0x5A, sizeof(sentinel));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, read_req(&s));
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&sentinel, &s, sizeof(s),
                                     "a refused read wrote the caller's sample");

    /* The magnetometer is a part of its own and the window holds nothing of
     * it, so its read is served throughout. */
    {
        int32_t mg[3] = { 0, 0, 0 };

        TEST_ASSERT_EQUAL_INT(KODE_OK, mag_read_req(mg));
        TEST_ASSERT_EQUAL_INT32(-914, mg[0]);
    }

    (void)drain_req((uint16_t)WITNESS_HZ);
    /* And served again once the window is over and the gyroscope it
     * switched back on has turned on. */
    gyro_settled();
    TEST_ASSERT_EQUAL_INT(KODE_OK, read_req(&s));
    imu_down();
}

TEST_CASE("a row asked for while a window holds the part is answered with a "
          "failed verdict, not queued", "[imu_ao]")
{
    bus_up(true, true);
    imu_up();

    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, capture_req()->rc);

    row_req();
    expect_verdict("imu", false);
    TEST_ASSERT_TRUE(logged("held by a window or a row"));
    /* Refused, and the window it could not have is still running: the refusal
     * did not take the one-shot that is timing somebody else's capture. */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "the refused row published records");

    mag_row_req();
    expect_verdict("mag", false);

    (void)drain_req((uint16_t)WITNESS_HZ);
    imu_down();
}

TEST_CASE("a sample is refused while the row holds the rates", "[imu_ao]")
{
    kdrv_qmi8658_sample_t s;

    bus_up(true, true);
    imu_up();
    s_st_polls = 10000U;   /* the row stays in its self-test for a while */

    row_req();
    /* The part was awake, so the row has put it to rest and is giving it
     * time to get there: it is the row's already, and a sample is refused. */
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00U, s_regs[QMI_CTRL7],
                                   "the row did not put an awake part to "
                                   "rest before its self-test");
    (void)memset(&s, 0, sizeof(s));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, read_req(&s));
    (void)qf_support_tick(AO_Imu, QF_SUPPORT_STEPS(ROW_REST_MS) + 2U);
    (void)row_record(1U, (uint8_t)IMU_TEST_WHOAMI);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, read_req(&s));

    /* Run the row out, so the AO is left idle with its one-shot disarmed. */
    row_run();
    expect_verdict("imu", false);
    gyro_settled();
    TEST_ASSERT_EQUAL_INT(KODE_OK, read_req(&s));
    imu_down();
}

/* ---- The magnetometer's calibration ----------------------------------------- */

#define CAL_TEXT      "100 -200 300 1000 1100 900"   /**< A calibration, as stored.   */
#define CAL_OTHER     "-5 6 -7 2000 250 4000"        /**< Another, within the limits. */
#define CAL_UNSET     0xFFFFFFFFU                    /**< What a rc reads before an answer. */
#define CAL_NVS_CAP   64U                            /**< A buffer for the stored text. */

/** @brief The numbers CAL_TEXT stands for. */
static kdrv_qmc5883p_cal_t cal_text(void)
{
    kdrv_qmc5883p_cal_t c;

    TEST_ASSERT_TRUE(kdrv_qmc5883p_cal_parse(CAL_TEXT, &c));
    return c;
}

/**
 * @brief One MAG_CAL_REQ_SIG, dispatched with the tag @p gen against @p cur.
 * @return What the AO wrote to the status; CAL_UNSET when it answered nothing.
 */
static uint32_t cal_req(uint8_t op, const kdrv_qmc5883p_cal_t *corr, MagCalSlot *slot,
                        uint32_t gen, const uint32_t *cur)
{
    MagCalEvt q;
    uint32_t  rc = CAL_UNSET;

    (void)memset(&q, 0, sizeof(q));
    q.super = (QEvt)QEVT_INITIALIZER(MAG_CAL_REQ_SIG);
    q.op    = op;
    if ( NULL != corr )
    {
        (void)memcpy(q.offset_mg, corr->offset_mg, sizeof(q.offset_mg));
        (void)memcpy(q.scale_milli, corr->scale_milli, sizeof(q.scale_milli));
    }
    q.out = slot;
    q.rc  = &rc;
    q.gen = gen;
    q.cur = cur;
    QASM_DISPATCH(AO_Imu, &q.super, 0U);
    return rc;
}

TEST_CASE("mag_cal is absent at first, set to six numbers and read back", "[imu_ao]")
{
    const KodeConfigAnsEvt *a;
    char                    nvs[CAL_NVS_CAP];

    fake_nvs_reset();
    bus_up(true, true);
    imu_up_raw();

    a = config_ask(AO_Imu, &(config_ask_t){ .key = KSET_KEY_MAG_CAL });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_INT(KODE_OK, a->status);
    TEST_ASSERT_EQUAL_STRING("", a->value);
    TEST_ASSERT_FALSE(a->stored);

    a = config_ask(AO_Imu, &(config_ask_t){ .key = KSET_KEY_MAG_CAL, .set = true, .s = CAL_TEXT });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_INT(KODE_OK, a->status);
    TEST_ASSERT_EQUAL_STRING(CAL_TEXT, a->value);
    TEST_ASSERT_TRUE(a->stored);
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_get_str(KSET_NAMESPACE, KSET_KEY_MAG_CAL, nvs, sizeof(nvs)));
    TEST_ASSERT_EQUAL_STRING(CAL_TEXT, nvs);

    a = config_ask(AO_Imu, &(config_ask_t){ .key = KSET_KEY_MAG_CAL });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING(CAL_TEXT, a->value);
    TEST_ASSERT_TRUE(a->stored);
    imu_down();
    fake_nvs_reset();
}

TEST_CASE("mag_cal comes back from the store at the next boot", "[imu_ao]")
{
    const KodeConfigAnsEvt *a;

    fake_nvs_reset();
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_str(KSET_NAMESPACE, KSET_KEY_MAG_CAL, CAL_TEXT));
    bus_up(true, true);
    imu_up_raw();

    a = config_ask(AO_Imu, &(config_ask_t){ .key = KSET_KEY_MAG_CAL });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING(CAL_TEXT, a->value);
    TEST_ASSERT_TRUE(a->stored);
    imu_down();
    fake_nvs_reset();
}

TEST_CASE("an empty mag_cal forgets it and the store loses the key", "[imu_ao]")
{
    const KodeConfigAnsEvt *a;
    char                    nvs[CAL_NVS_CAP];

    fake_nvs_reset();
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_str(KSET_NAMESPACE, KSET_KEY_MAG_CAL, CAL_TEXT));
    bus_up(true, true);
    imu_up_raw();

    a = config_ask(AO_Imu, &(config_ask_t){ .key = KSET_KEY_MAG_CAL, .set = true, .s = "" });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_INT(KODE_OK, a->status);
    TEST_ASSERT_EQUAL_STRING("", a->value);
    TEST_ASSERT_FALSE(a->stored);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, khal_nvs_get_str(KSET_NAMESPACE, KSET_KEY_MAG_CAL, nvs, sizeof(nvs)));
    imu_down();
    fake_nvs_reset();
}

TEST_CASE("mag_cal refuses text that is not six numbers and keeps the old one", "[imu_ao]")
{
    const KodeConfigAnsEvt *a;

    fake_nvs_reset();
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_str(KSET_NAMESPACE, KSET_KEY_MAG_CAL, CAL_TEXT));
    bus_up(true, true);
    imu_up_raw();

    a = config_ask(AO_Imu, &(config_ask_t){ .key = KSET_KEY_MAG_CAL, .set = true, .s = "1 2 3" });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, a->status);
    TEST_ASSERT_EQUAL_STRING(CAL_TEXT, a->value);
    TEST_ASSERT_TRUE(a->stored);
    imu_down();
    fake_nvs_reset();
}

TEST_CASE("a mag_cal the store refuses is denied and the old one stays", "[imu_ao]")
{
    const KodeConfigAnsEvt *a;

    fake_nvs_reset();
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_str(KSET_NAMESPACE, KSET_KEY_MAG_CAL, CAL_TEXT));
    bus_up(true, true);
    imu_up_raw();

    fake_nvs_fail_next_set();
    a = config_ask(AO_Imu, &(config_ask_t){ .key = KSET_KEY_MAG_CAL, .set = true, .s = CAL_OTHER });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, a->status);
    TEST_ASSERT_EQUAL_STRING(CAL_TEXT, a->value);
    TEST_ASSERT_TRUE(a->stored);
    imu_down();
    fake_nvs_reset();
}

TEST_CASE("a stored mag_cal nobody can read is none, with a warning", "[imu_ao]")
{
    const KodeConfigAnsEvt *a;

    fake_nvs_reset();
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_str(KSET_NAMESPACE, KSET_KEY_MAG_CAL, "junk"));
    bus_up(true, true);
    imu_up_raw();

    TEST_ASSERT_TRUE(logged("calibration is unreadable"));
    a = config_ask(AO_Imu, &(config_ask_t){ .key = KSET_KEY_MAG_CAL });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("", a->value);
    TEST_ASSERT_FALSE(a->stored);
    imu_down();
    fake_nvs_reset();
}

TEST_CASE("another owner's key is not mag_cal's to answer", "[imu_ao]")
{
    bus_up(true, true);
    imu_up_raw();
    TEST_ASSERT_NULL(config_ask(AO_Imu, &(config_ask_t){ .key = KSET_KEY_HAPTIC }));
    imu_down();
}

TEST_CASE("the door gets, sets and clears the calibration for the facade", "[imu_ao]")
{
    kdrv_qmc5883p_cal_t const mine = cal_text();
    kdrv_qmc5883p_cal_t const over = { { KDRV_QMC5883P_CAL_OFFSET_MAX_MG + 1, 0, 0 }, { 1000, 1000, 1000 } };
    MagCalSlot                slot;
    char                      nvs[CAL_NVS_CAP];

    fake_nvs_reset();
    bus_up(true, true);
    imu_up_raw();

    TEST_ASSERT_EQUAL_UINT32(KODE_OK, cal_req(MAG_CAL_OP_GET, NULL, &slot, 1U, NULL));
    TEST_ASSERT_FALSE(slot.present);

    TEST_ASSERT_EQUAL_UINT32(KODE_OK, cal_req(MAG_CAL_OP_SET, &mine, NULL, 1U, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_get_str(KSET_NAMESPACE, KSET_KEY_MAG_CAL, nvs, sizeof(nvs)));
    TEST_ASSERT_EQUAL_STRING(CAL_TEXT, nvs);
    TEST_ASSERT_EQUAL_UINT32(KODE_OK, cal_req(MAG_CAL_OP_GET, NULL, &slot, 1U, NULL));
    TEST_ASSERT_TRUE(slot.present);
    TEST_ASSERT_EQUAL_INT32_ARRAY(mine.offset_mg, slot.corr.offset_mg, 3U);
    TEST_ASSERT_EQUAL_INT32_ARRAY(mine.scale_milli, slot.corr.scale_milli, 3U);

    TEST_ASSERT_EQUAL_UINT32(KODE_DENIED, cal_req(MAG_CAL_OP_SET, &over, NULL, 1U, NULL));
    TEST_ASSERT_EQUAL_UINT32(KODE_OK, cal_req(MAG_CAL_OP_GET, NULL, &slot, 1U, NULL));
    TEST_ASSERT_TRUE_MESSAGE(slot.present, "a refused set keeps the old calibration");

    TEST_ASSERT_EQUAL_UINT32(KODE_OK, cal_req(MAG_CAL_OP_CLEAR, NULL, NULL, 1U, NULL));
    TEST_ASSERT_EQUAL_UINT32(KODE_OK, cal_req(MAG_CAL_OP_GET, NULL, &slot, 1U, NULL));
    TEST_ASSERT_FALSE(slot.present);
    imu_down();
    fake_nvs_reset();
}

TEST_CASE("the door answers a set the store refuses with KODE_ERROR and keeps the old one",
          "[imu_ao]")
{
    kdrv_qmc5883p_cal_t const mine = cal_text();
    MagCalSlot                slot;

    fake_nvs_reset();
    bus_up(true, true);
    imu_up_raw();

    fake_nvs_fail_next_set();
    TEST_ASSERT_EQUAL_UINT32(KODE_ERROR, cal_req(MAG_CAL_OP_SET, &mine, NULL, 1U, NULL));
    TEST_ASSERT_EQUAL_UINT32(KODE_OK, cal_req(MAG_CAL_OP_GET, NULL, &slot, 1U, NULL));
    TEST_ASSERT_FALSE(slot.present);
    imu_down();
    fake_nvs_reset();
}

TEST_CASE("the door writes nothing for a request nobody waits on", "[imu_ao]")
{
    kdrv_qmc5883p_cal_t const mine = cal_text();
    static const uint32_t     current = 2U;
    char                      nvs[CAL_NVS_CAP];

    fake_nvs_reset();
    bus_up(true, true);
    imu_up_raw();

    TEST_ASSERT_EQUAL_UINT32(CAL_UNSET, cal_req(MAG_CAL_OP_SET, &mine, NULL, 1U, &current));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, khal_nvs_get_str(KSET_NAMESPACE, KSET_KEY_MAG_CAL, nvs, sizeof(nvs)));
    TEST_ASSERT_EQUAL_UINT32(KODE_OK, cal_req(MAG_CAL_OP_SET, &mine, NULL, 2U, &current));
    imu_down();
    fake_nvs_reset();
}

/* A setting's request is posted to its owner (kode_config_post()): AO_Imu
 * subscribes to none, so one published by mistake lands nowhere. */
TEST_CASE("a config request on the bus does not reach AO_Imu", "[imu_ao]")
{
    KodeConfigReqEvt *e;

    bus_up(true, true);
    imu_up_raw();
    drain(AO_Imu);

    e = Q_NEW(KodeConfigReqEvt, CONFIG_REQ_SIG);
    e->seq = 1U;
    e->set = false;
    (void)snprintf(e->key, sizeof(e->key), "%s", KSET_KEY_MAG_CAL);
    QACTIVE_PUBLISH(&e->super, NULL);
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Imu->eQueue));
    imu_down();
}

/** @brief A filler AO_Imu ignores. */
static const QEvt s_filler = QEVT_INITIALIZER(MAG_TEST_REQ_SIG);

TEST_CASE("a full queue refuses a magnetometer request and writes nothing", "[imu_ao]")
{
    static MagCalSlot slot;
    static int32_t    mg[3];
    uint32_t          rc = CAL_UNSET;

    bus_up(true, true);
    imu_up_raw();
    qf_support_margin_queue(AO_Imu);

    TEST_ASSERT_TRUE_MESSAGE(qf_support_queue_stuff(AO_Imu, &s_filler), "the queue never filled");
    TEST_ASSERT_FALSE(mag_cal_request(MAG_CAL_OP_GET, NULL, &slot, &rc, NULL, 1U, NULL, NULL));
    TEST_ASSERT_FALSE(mag_read_request(mg, &rc, NULL, 1U, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(CAL_UNSET, rc);

    drain(AO_Imu);
    TEST_ASSERT_TRUE(mag_cal_request(MAG_CAL_OP_GET, NULL, &slot, &rc, NULL, 1U, NULL, NULL));
    TEST_ASSERT_TRUE(mag_read_request(mg, &rc, NULL, 1U, NULL, NULL));
    drain(AO_Imu);
    imu_down();
}

TEST_CASE("a stored mag_cal too long to load is none, with a warning", "[imu_ao]")
{
    const KodeConfigAnsEvt *a;

    fake_nvs_reset();
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_str(KSET_NAMESPACE, KSET_KEY_MAG_CAL,
                                                    CAL_TEXT " 1000 1000 1000 1000"));
    bus_up(true, true);
    imu_up_raw();

    TEST_ASSERT_TRUE(logged("calibration is too long"));
    a = config_ask(AO_Imu, &(config_ask_t){ .key = KSET_KEY_MAG_CAL });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("", a->value);
    TEST_ASSERT_FALSE(a->stored);
    imu_down();
    fake_nvs_reset();
}

/* ---- kode_sys's requests on a board with both parts --------------------------- */

TEST_CASE("kode_sys_imu_read answers the asking AO once, with a copy of the sample",
          "[imu_ao]")
{
    kdrv_qmi8658_sample_t        want;
    const kode_sys_imu_answer_t *a;

    bus_up(true, true);
    imu_up();
    gyro_settled();
    sys_up();
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, read_req(&want));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_imu_read(ao_spy()));
    a = (const kode_sys_imu_answer_t *)sys_answer(IMU_SAMPLE_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_INT32_ARRAY(want.a_mg, a->sample.a_mg, KODE_SYS_IMU_AXES);
    TEST_ASSERT_EQUAL_INT32_ARRAY(want.w_mdps, a->sample.w_mdps, KODE_SYS_IMU_AXES);
    TEST_ASSERT_EQUAL_INT32(want.temp_dc, a->sample.temp_dc);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "posted once, not published as well");
    ao_spy_release();
    imu_down();
}

TEST_CASE("a second motion request of a kind before its answer is refused at once",
          "[imu_ao]")
{
    const kode_sys_imu_answer_t *a;
    const kode_sys_mag_answer_t *m;

    bus_up(true, true);
    imu_up();
    gyro_settled();
    sys_up();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_imu_read(ao_spy()));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_imu_read(ao_spy()));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_mag_read(ao_spy()),
                                  "another kind has a door of its own");
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_mag_read(ao_spy()));
    TEST_ASSERT_EQUAL_UINT32(2U, (uint32_t)uxQueueMessagesWaiting(AO_Imu->eQueue));

    (void)qf_support_settle(AO_Imu);
    a = (const kode_sys_imu_answer_t *)(const void *)spy_expect(IMU_SAMPLE_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    m = (const kode_sys_mag_answer_t *)(const void *)spy_expect(MAG_SAMPLE_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, m->rc);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the first of each answered once");

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_imu_read(ao_spy()),
                                  "the answer opens the door again");
    a = (const kode_sys_imu_answer_t *)sys_answer(IMU_SAMPLE_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    ao_spy_release();
    imu_down();
}

/** @brief Stack of the asking task, in bytes. */
#define TASK_ASK_STACK_BYTES 8192U

/** @brief Priority of the asking task. */
#define TASK_ASK_PRIO 1U

/** @brief The longest a case waits for the asking task, in ms. */
#define TASK_ASK_WAIT_MS 4000U

/** @brief What the asking task is given and gives back. */
typedef struct {
    QActive          *me;    /**< The AO it asks in the name of.        */
    SemaphoreHandle_t done;  /**< Given once the call has returned.     */
    kode_status_t     rc;    /**< What kode_sys_imu_read() returned.    */
} task_ask_t;

/** @brief A task that makes one IMU request in @c me's name and ends. */
static void task_asks(void *arg)
{
    task_ask_t * const t = (task_ask_t *)arg;

    t->rc = kode_sys_imu_read(t->me);
    (void)xSemaphoreGive(t->done);
}

/**
 * @brief Make one IMU request from a task that is not an AO's own thread.
 * @param me The started AO the task asks for.
 * @return What the call returned.
 */
static kode_status_t ask_from_task(QActive *me)
{
    task_ask_t t = { me, xSemaphoreCreateBinary(), KODE_ERROR };

    TEST_ASSERT_NOT_NULL(t.done);
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_task_start(&task_asks, "t_imu_ask", TASK_ASK_STACK_BYTES,
                                                    TASK_ASK_PRIO, &t));
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(t.done, pdMS_TO_TICKS(TASK_ASK_WAIT_MS)));
    vSemaphoreDelete(t.done);
    return t.rc;
}

TEST_CASE("a task asks in a started AO's name and the answer arrives at that AO",
          "[imu_ao]")
{
    const kode_sys_imu_answer_t *a;

    bus_up(true, true);
    imu_up();
    gyro_settled();
    sys_up();

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, ask_from_task(ao_spy()),
                                  "a task's request is taken as the AO's own is");
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, kode_sys_imu_read(ao_spy()),
                                  "the AO's own thread shares the door the task shut");
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, ask_from_task(ao_spy()),
                                  "a second task of the AO finds the door shut too");
    TEST_ASSERT_EQUAL_UINT32(1U, (uint32_t)uxQueueMessagesWaiting(AO_Imu->eQueue));

    a = (const kode_sys_imu_answer_t *)sys_answer(IMU_SAMPLE_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "answered once, to the AO");

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_imu_read(ao_spy()),
                                  "the answer opens the door for the AO's own thread");
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, ask_from_task(ao_spy()));
    a = (const kode_sys_imu_answer_t *)sys_answer(IMU_SAMPLE_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    ao_spy_release();
    imu_down();
}

TEST_CASE("a read that wakes the IMU is answered once its gyroscope is on",
          "[imu_ao]")
{
    const kode_sys_imu_answer_t *a;

    bus_up(true, true);
    imu_up_raw();
    sys_up();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_imu_read(ao_spy()));
    (void)qf_support_settle(AO_Imu);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "held until the rotation means something");
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_imu_read(ao_spy()));

    gyro_settled();
    (void)qf_support_tick(AO_Imu, QF_SUPPORT_STEPS(GYRO_READY_MS) + 2U);
    a = (const kode_sys_imu_answer_t *)(const void *)spy_expect(IMU_SAMPLE_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    ao_spy_release();
    imu_down();
}

TEST_CASE("kode_sys's calibration requests answer what is stored afterwards",
          "[imu_ao]")
{
    kdrv_qmc5883p_cal_t const        want = cal_text();
    kode_sys_mag_corr_t              corr;
    const kode_sys_mag_cal_answer_t *a;

    fake_nvs_reset();
    bus_up(true, true);
    imu_up_raw();
    sys_up();
    (void)memcpy(corr.offset_mg, want.offset_mg, sizeof(corr.offset_mg));
    (void)memcpy(corr.scale_milli, want.scale_milli, sizeof(corr.scale_milli));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_mag_cal_set(ao_spy(), &corr));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, kode_sys_mag_cal_get(ao_spy()),
                                  "one calibration request waits at a time");
    a = (const kode_sys_mag_cal_answer_t *)sys_answer(MAG_CAL_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_UINT(KODE_SYS_MAG_CAL_SET, a->op);
    TEST_ASSERT_TRUE(a->present);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_mag_cal_get(ao_spy()));
    a = (const kode_sys_mag_cal_answer_t *)sys_answer(MAG_CAL_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT(KODE_SYS_MAG_CAL_GET, a->op);
    TEST_ASSERT_TRUE(a->present);
    TEST_ASSERT_EQUAL_INT32_ARRAY(want.offset_mg, a->corr.offset_mg, KODE_SYS_MAG_AXES);
    TEST_ASSERT_EQUAL_INT32_ARRAY(want.scale_milli, a->corr.scale_milli, KODE_SYS_MAG_AXES);

    corr.scale_milli[0] = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_mag_cal_set(ao_spy(), &corr),
                                  "the limits are the owner's to judge");
    a = (const kode_sys_mag_cal_answer_t *)sys_answer(MAG_CAL_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_DENIED, a->rc);
    TEST_ASSERT_TRUE_MESSAGE(a->present, "the old one stays");

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_mag_cal_clear(ao_spy()));
    a = (const kode_sys_mag_cal_answer_t *)sys_answer(MAG_CAL_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_FALSE(a->present);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    ao_spy_release();
    imu_down();
    fake_nvs_reset();
}

TEST_CASE("a plain task's motion request is refused at once and posts nothing",
          "[imu_ao]")
{
    kode_sys_mag_corr_t corr;

    bus_up(true, true);
    imu_up();
    gyro_settled();
    sys_up();
    (void)memset(&corr, 0, sizeof(corr));

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_imu_read(NULL));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_mag_read(NULL));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_mag_cal_get(NULL));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_mag_cal_set(NULL, &corr));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_mag_cal_set(ao_spy(), NULL));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_mag_cal_clear(NULL));
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Imu->eQueue));

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_imu_read(ao_spy()),
                                  "a refusal takes no door");
    (void)sys_answer(IMU_SAMPLE_RSP_SIG);
    ao_spy_release();
    imu_down();
}

TEST_CASE("a full AO_Imu queue refuses kode_sys's requests with no resources",
          "[imu_ao]")
{
    bus_up(true, true);
    imu_up_raw();
    sys_up();

    TEST_ASSERT_TRUE_MESSAGE(qf_support_queue_stuff(AO_Imu, &s_filler), "the queue never filled");
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_imu_read(ao_spy()));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_mag_read(ao_spy()));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_mag_cal_get(ao_spy()));
    drain(AO_Imu);

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_mag_read(ao_spy()),
                                  "nothing was posted, so the door is open");
    (void)sys_answer(MAG_SAMPLE_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    ao_spy_release();
    imu_down();
}

TEST_CASE("two AOs asking the same kind are both accepted and both answered",
          "[imu_ao]")
{
    const kode_sys_imu_answer_t *a;
    const QEvt                  *o;

    bus_up(true, true);
    imu_up();
    gyro_settled();
    sys_up();
    ao_spy_other_drain();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_imu_read(ao_spy()));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_imu_read(ao_spy_other()),
                                  "the door is the asker's, not the kind's");
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_imu_read(ao_spy_other()));
    (void)qf_support_settle(AO_Imu);

    a = (const kode_sys_imu_answer_t *)(const void *)spy_expect(IMU_SAMPLE_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    o = ao_spy_other_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(o, "the second asker is answered too");
    TEST_ASSERT_EQUAL_INT((int)IMU_SAMPLE_RSP_SIG, (int)o->sig);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, ((const kode_sys_imu_answer_t *)(const void *)o)->rc);
    QF_gc(o);
    TEST_ASSERT_NULL(ao_spy_other_take());
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    ao_spy_release();
    imu_down();
}

/** @brief Most blocks a case takes out of a pool to leave it dry. */
#define DRY_MAX  128U

TEST_CASE("a dry pool refuses kode_sys's request with no resources and posts nothing",
          "[imu_ao]")
{
    static ImuSampleRspEvt *held[DRY_MAX];
    uint32_t                n = 0U;

    bus_up(true, true);
    imu_up();
    gyro_settled();
    sys_up();

    while ( DRY_MAX > n )
    {
        held[n] = Q_NEW_X(ImuSampleRspEvt, 0U, IMU_SAMPLE_RSP_SIG);
        if ( NULL == held[n] )
        {
            break;
        }
        n++;
    }
    TEST_ASSERT_LESS_THAN_UINT32_MESSAGE(DRY_MAX, n, "the pool never ran dry");
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_imu_read(ao_spy()));
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Imu->eQueue));
    for ( uint32_t i = 0U; i < n; i++ )
    {
        QF_gc(&held[i]->super);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_imu_read(ao_spy()),
                                  "nothing was posted, so the door is open");
    (void)sys_answer(IMU_SAMPLE_RSP_SIG);
    ao_spy_release();
    imu_down();
}

/** @brief The body of the full-queue case, run in a child: the asker's queue is full, then AO_Imu answers into it. */
static void full_asker_body(void *arg)
{
    (void)arg;
    (void)qf_support_queue_stuff(ao_spy_other(), &s_filler);
    (void)kode_sys_imu_read(ao_spy_other());
    (void)qf_support_settle(AO_Imu);
}

TEST_CASE("an answer into an asker's full queue is QP's assert, not a drop",
          "[imu_ao]")
{
    bus_up(true, true);
    imu_up();
    gyro_settled();
    sys_up();
    ao_spy_other_drain();

    /* QP asserts with Q_onError(), which aborts this binary, so the post runs
     * in a child and the parent reads how it died. */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)ASK_CHILD_ABORTED,
                                    (uint32_t)ask_support_fork_run(&full_asker_body, NULL),
                                  "the child was not stopped by QP's assert");
    imu_down();
}


/* ---- The board's rest -------------------------------------------------------- */

/* Both parts are woken, then a round puts them back; a second round (OFF
 * after ASLEEP) finds them there and writes nothing. */
TEST_CASE("a round puts the IMU and the magnetometer to rest and answers once",
          "[imu_ao][lowpower]")
{
    int32_t mg[3];

    bus_up(true, true);
    imu_up();
    TEST_ASSERT_EQUAL_INT(KODE_OK, (int)mag_read_req(mg));
    TEST_ASSERT_EQUAL_HEX8(QMI_SENSORS_ON, s_regs[QMI_CTRL7]);
    TEST_ASSERT_TRUE(s_mag_running);

    lp_round(AO_Imu, KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00U, s_regs[QMI_CTRL7], "the IMU still converts");
    TEST_ASSERT_FALSE_MESSAGE(s_mag_running, "the magnetometer still converts");
    lp_expect_answer(false, NULL);
    lp_expect_no_more();

    s_regs[QMI_CTRL7] = 0x55U;      /* anything a second rest would overwrite */
    lp_round(AO_Imu, KODE_LP_OFF);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x55U, s_regs[QMI_CTRL7], "OFF rested a part already at rest");
    lp_expect_answer(false, NULL);
    s_regs[QMI_CTRL7] = 0x00U;
    imu_down();
}

/* Nothing is restored on AWAKE: the parts wake for the next request. The rest
 * one-shots were silenced with the parts, so no expiry writes after it. */
TEST_CASE("after the rest AWAKE writes nothing and the next request wakes the part",
          "[imu_ao][lowpower]")
{
    kdrv_qmi8658_sample_t s;

    bus_up(true, true);
    imu_up();

    lp_round(AO_Imu, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
    fake_klog_reset();
    (void)qf_support_tick(AO_Imu, REST_TICKS);
    TEST_ASSERT_FALSE_MESSAGE(logged("the IMU rests"), "a one-shot outlived the rest");
    lp_awake(AO_Imu);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00U, s_regs[QMI_CTRL7], "AWAKE woke a part nobody asked for");

    (void)memset(&s, 0, sizeof(s));
    (void)read_req(&s);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(QMI_SENSORS_ON, s_regs[QMI_CTRL7],
                                   "the next request did not wake the part");
    gyro_settled();
    (void)qf_support_tick(AO_Imu, QF_SUPPORT_STEPS(GYRO_READY_MS) + 2U);
    imu_down();
}

/* A window is a stretch of time somebody has started to measure. */
TEST_CASE("a witness window refuses the round and is not cut", "[imu_ao][lowpower]")
{
    uint8_t ctrl7;

    bus_up(true, true);
    imu_up();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, capture_req()->rc);
    ctrl7 = s_regs[QMI_CTRL7];

    lp_round(AO_Imu, KODE_LP_ASLEEP);
    lp_expect_answer(true, "imu busy");
    lp_expect_no_more();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(ctrl7, s_regs[QMI_CTRL7],
                                   "the window's IMU was put to rest under it");
    (void)drain_req((uint16_t)WITNESS_HZ);
    imu_down();
}
