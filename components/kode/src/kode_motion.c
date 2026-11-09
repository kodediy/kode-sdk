/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_motion.c
 * @brief The standard layer's IMU, magnetometer and compass, read by axis name.
 */
#include "kode.h"

#include <math.h>

#include "kcompass.h"
#include "kdrv_qmc5883p.h"
#include "kdrv_qmi8658.h"
#include "khal_once.h"
#include "khal_sync.h"
#include "kode_entry.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"

/** @brief Milli-g in one g. */
#define KODE_MOTION_MG_PER_G 1000.0F

/** @brief Milli-degrees a second in one degree a second. */
#define KODE_MOTION_MDPS_PER_DPS 1000.0F

/** @brief Tenths of a degree in one degree. */
#define KODE_MOTION_DC_PER_C 10.0F

/** @brief Milligauss in one microtesla (1 G = 100 uT). */
#define KODE_MOTION_MG_PER_UT 10.0F

/** @brief The IMU's readings in kode_imu_t's order: the index of a name in kode_imu_read_axis(). */
enum {
    IMU_AXIS_ACCEL_X = 0,  /**< accel_x */
    IMU_AXIS_ACCEL_Y,      /**< accel_y */
    IMU_AXIS_ACCEL_Z,      /**< accel_z */
    IMU_AXIS_GYRO_X,       /**< gyro_x  */
    IMU_AXIS_GYRO_Y,       /**< gyro_y  */
    IMU_AXIS_GYRO_Z,       /**< gyro_z  */
    IMU_AXIS_TEMP,         /**< temp    */
    IMU_AXIS_COUNT         /**< How many there are. */
};

/** @brief The field's axes in kode_mag_t's order: the index of a name in kode_mag_read_axis(). */
enum {
    MAG_AXIS_X = 0,  /**< x */
    MAG_AXIS_Y,      /**< y */
    MAG_AXIS_Z,      /**< z */
    MAG_AXIS_COUNT   /**< How many there are. */
};

_Static_assert((uint32_t)MAG_AXIS_COUNT == KDRV_QMC5883P_AXES, "the names are the part's axes");

#define KODE_MOTION_IMU_WHO  "imu"           /**< The IMU's prefix in an error.            */
#define KODE_MOTION_IMU_PART "IMU"           /**< The IMU's name in an error's sentence.   */
#define KODE_MOTION_MAG_WHO  "magnetometer"  /**< The magnetometer's prefix in an error.   */
#define KODE_MOTION_MAG_PART "magnetometer"  /**< Its name in an error's sentence.         */

/** @brief The lock every public motion call holds; made on the first call. */
static khal_mutex_t s_motion_lock;

/** @brief Make the motion lock. @return true when it exists. */
static bool motion_lock_make(void)
{
    s_motion_lock = khal_mutex_create();
    return NULL != s_motion_lock;
}

/**
 * @brief The prologue of a public motion call: kode_enter(), then its turn.
 *
 * The IMU and the magnetometer share one claim in the extended layer, so
 * another task's read, of either, finishes before this one starts. Released by
 * motion_leave(); no public call calls another while it holds it.
 *
 * @param name The public name of the call.
 * @return true when the call has its turn; false with the error set.
 */
static bool motion_enter(const char *name)
{
    static khal_once_t lock_once;  /* the one-shot the lock is made by */
    bool               ok = kode_enter(name);

    if ( ok )
    {
        ok = khal_once_try(&lock_once, &motion_lock_make);
        if ( ok )
        {
            khal_mutex_lock(s_motion_lock);
        }
        else
        {
            kode_err_set("%s: no memory for the motion lock", name);
        }
    }
    return ok;
}

/** @brief Give the turn back, at the end of a call motion_enter() let in. */
static void motion_leave(void)
{
    khal_mutex_unlock(s_motion_lock);
}

/** @brief What one IMU try carries. */
typedef struct {
    kdrv_qmi8658_sample_t sample;  /**< The last sample.      */
    kode_status_t         rc;      /**< The last answer.      */
} imu_try_t;

/** @brief What one magnetometer try carries. */
typedef struct {
    int32_t       mg[MAG_AXIS_COUNT];  /**< The last field, milligauss. */
    kode_status_t rc;                  /**< The last answer.            */
} mag_try_t;

/**
 * @brief The index of @p axis in @p names, or @p count with the error set.
 * @param axis  What the caller typed.
 * @param names The axis names.
 * @param count How many there are.
 * @return The index; @p count when there is none.
 */
static uint32_t axis_find(const char *axis, const char *const *names, uint32_t count)
{
    uint32_t found = count;

    if ( NULL == axis )
    {
        kode_err_set("axis name is NULL");
    }
    else
    {
        for ( uint32_t i = 0U; (i < count) && (found == count); i++ )
        {
            if ( kode_name_is(axis, names[i]) )
            {
                found = i;
            }
        }
        if ( found == count )
        {
            kode_err_set("unknown axis '%s'", axis);
        }
    }
    return found;
}

/**
 * @brief Keep why a read failed as the task's error.
 * @param who  The prefix: "imu" or "magnetometer".
 * @param part The part's name in the sentence.
 * @param rc   The sys answer.
 */
static void motion_fail(const char *who, const char *part, kode_status_t rc)
{
    const char *tail = "could not be read";

    switch ( rc )
    {
    case KODE_IO_ERROR:
        tail = "is not fitted";
        break;
    case KODE_TIMEOUT:
        tail = "did not answer";
        break;
    case KODE_DENIED:
        tail = "is busy";
        break;
    case KODE_ERROR:
        tail = "gave no reading";
        break;
    case KODE_NO_RESOURCES:
        tail = "could not be asked";
        break;
    default:
        /* No phrase of its own. */
        break;
    }
    kode_err_set("%s: the %s %s", who, part, tail);
}

/** @brief One try of the IMU: true unless another task holds the part. */
static bool imu_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the imu_try_t imu_sample() passed */
    imu_try_t *const t = (imu_try_t *)ctx;

    t->rc = kode_block_imu_read_sample(&t->sample);
    return KODE_DENIED != t->rc;
}

/** @brief One try of the field: true unless the part is held or had no conversion. */
static bool mag_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the mag_try_t mag_sample() passed */
    mag_try_t *const t = (mag_try_t *)ctx;

    t->rc = kode_block_mag_read_raw(t->mg);
    return (KODE_DENIED != t->rc) && (KODE_ERROR != t->rc);
}

/**
 * @brief Read the IMU, waiting its turn; the error is set on failure.
 * @param reading Out: the seven readings.
 * @return true when @p reading is filled.
 */
static bool imu_sample(kode_imu_t *reading)
{
    imu_try_t t = { { { 0, 0, 0 }, { 0, 0, 0 }, 0 }, KODE_ERROR };

    (void)kode_retry(&imu_attempt, &t, KODE_MOTION_BUSY_MS);
    if ( KODE_OK == t.rc )
    {
        reading->accel_x = (float)t.sample.a_mg[0] / KODE_MOTION_MG_PER_G;
        reading->accel_y = (float)t.sample.a_mg[1] / KODE_MOTION_MG_PER_G;
        reading->accel_z = (float)t.sample.a_mg[2] / KODE_MOTION_MG_PER_G;
        reading->gyro_x  = (float)t.sample.w_mdps[0] / KODE_MOTION_MDPS_PER_DPS;
        reading->gyro_y  = (float)t.sample.w_mdps[1] / KODE_MOTION_MDPS_PER_DPS;
        reading->gyro_z  = (float)t.sample.w_mdps[2] / KODE_MOTION_MDPS_PER_DPS;
        reading->temp    = (float)t.sample.temp_dc / KODE_MOTION_DC_PER_C;
    }
    else
    {
        motion_fail(KODE_MOTION_IMU_WHO, KODE_MOTION_IMU_PART, t.rc);
    }
    return KODE_OK == t.rc;
}

/**
 * @brief Read the field as the part measures it, waiting its turn.
 * @param mg Out: milligauss on the part's three axes.
 * @return true when @p mg is filled; false with the error set.
 */
static bool mag_raw(int32_t mg[MAG_AXIS_COUNT])
{
    mag_try_t t = { { 0, 0, 0 }, KODE_ERROR };

    (void)kode_retry(&mag_attempt, &t, KODE_MOTION_BUSY_MS);
    if ( KODE_OK == t.rc )
    {
        for ( uint32_t i = 0U; (uint32_t)MAG_AXIS_COUNT > i; i++ )
        {
            mg[i] = t.mg[i];
        }
    }
    else
    {
        motion_fail(KODE_MOTION_MAG_WHO, KODE_MOTION_MAG_PART, t.rc);
    }
    return KODE_OK == t.rc;
}

/** @brief What one try at the calibration carries. */
typedef struct {
    kdrv_qmc5883p_cal_t corr;     /**< The calibration, when there is one.  */
    bool                present;  /**< There is one.                        */
    kode_status_t       rc;       /**< The last answer.                     */
} cal_try_t;

/** @brief One try of the calibration: true unless another task holds the claim. */
static bool cal_get_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the cal_try_t cal_read() passed */
    cal_try_t *const t = (cal_try_t *)ctx;

    t->rc = kode_block_mag_cal_get(&t->corr, &t->present);
    return KODE_DENIED != t->rc;
}

/** @brief One try of storing @p ctx's calibration, or of clearing it when there is none. */
static bool cal_put_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the cal_try_t cal_write() passed */
    cal_try_t *const t = (cal_try_t *)ctx;

    if ( t->present )
    {
        t->rc = kode_block_mag_cal_set(&t->corr);
    }
    else
    {
        t->rc = kode_block_mag_cal_clear();
    }
    return KODE_DENIED != t->rc;
}

/**
 * @brief Ask AO_Imu for the calibration, waiting its turn.
 * @param corr    Out: the calibration, when @p present.
 * @param present Out: whether one is stored.
 * @return true when the answer is in; false with the error set.
 */
static bool cal_read(kdrv_qmc5883p_cal_t *corr, bool *present)
{
    cal_try_t t = { { { 0, 0, 0 }, { 0, 0, 0 } }, false, KODE_ERROR };

    (void)kode_retry(&cal_get_attempt, &t, KODE_MOTION_BUSY_MS);
    if ( KODE_OK == t.rc )
    {
        *corr    = t.corr;
        *present = t.present;
    }
    else
    {
        motion_fail(KODE_MOTION_MAG_WHO, KODE_MOTION_MAG_PART, t.rc);
    }
    return KODE_OK == t.rc;
}

/**
 * @brief Store the calibration, or clear it when @p corr is NULL, waiting its turn.
 * @param corr The calibration; NULL clears.
 * @return true when done; false with the error set.
 */
static bool cal_write(const kdrv_qmc5883p_cal_t *corr)
{
    cal_try_t t = { { { 0, 0, 0 }, { 0, 0, 0 } }, false, KODE_ERROR };

    if ( NULL != corr )
    {
        t.corr    = *corr;
        t.present = true;
    }
    (void)kode_retry(&cal_put_attempt, &t, KODE_MOTION_BUSY_MS);
    if ( KODE_ERROR == t.rc )
    {
        kode_err_set("%s: the calibration could not be stored", KODE_MOTION_MAG_WHO);
    }
    else if ( KODE_OK != t.rc )
    {
        motion_fail(KODE_MOTION_MAG_WHO, KODE_MOTION_MAG_PART, t.rc);
    }
    else
    {
        /* Stored. */
    }
    return KODE_OK == t.rc;
}

/**
 * @brief The corrected field, mG on the part's axes, from the calibration when there is one.
 * @param corr    The calibration.
 * @param present Whether it holds one.
 * @param raw     The part's reading, mG.
 * @param out     Out: the field, mG.
 */
static void mag_correct(const kdrv_qmc5883p_cal_t *corr, bool present, const int32_t raw[MAG_AXIS_COUNT],
                        float out[MAG_AXIS_COUNT])
{
    if ( present )
    {
        kdrv_qmc5883p_cal_apply(corr, raw, out);
    }
    else
    {
        for ( uint32_t i = 0U; (uint32_t)MAG_AXIS_COUNT > i; i++ )
        {
            out[i] = (float)raw[i];
        }
    }
}

/**
 * @brief Read the field, corrected when a calibration exists; the error is set on failure.
 * @param reading Out: the three axes, microtesla.
 * @return true when @p reading is filled.
 */
static bool mag_sample(kode_mag_t *reading)
{
    kdrv_qmc5883p_cal_t corr;
    bool                present = false;
    int32_t             raw[MAG_AXIS_COUNT];
    float               mg[MAG_AXIS_COUNT];
    bool                ok = cal_read(&corr, &present) && mag_raw(raw);

    if ( ok )
    {
        mag_correct(&corr, present, raw, mg);
        reading->x = mg[MAG_AXIS_X] / KODE_MOTION_MG_PER_UT;
        reading->y = mg[MAG_AXIS_Y] / KODE_MOTION_MG_PER_UT;
        reading->z = mg[MAG_AXIS_Z] / KODE_MOTION_MG_PER_UT;
    }
    return ok;
}

/** @brief Every field of an IMU reading set to NAN. */
static kode_imu_t imu_none(void)
{
    const kode_imu_t none = { NAN, NAN, NAN, NAN, NAN, NAN, NAN };

    return none;
}

/** @brief Every field of a field reading set to NAN. */
static kode_mag_t mag_none(void)
{
    const kode_mag_t none = { NAN, NAN, NAN };

    return none;
}

/**
 * @brief The reading of IMU axis @p idx.
 * @param reading The seven readings.
 * @param idx An index into kode_imu_read_axis()'s names.
 * @return The reading; NAN past the last.
 */
static float imu_pick(const kode_imu_t *reading, uint32_t idx)
{
    float value = NAN;

    switch ( idx )
    {
    case (uint32_t)IMU_AXIS_ACCEL_X:
        value = reading->accel_x;
        break;
    case (uint32_t)IMU_AXIS_ACCEL_Y:
        value = reading->accel_y;
        break;
    case (uint32_t)IMU_AXIS_ACCEL_Z:
        value = reading->accel_z;
        break;
    case (uint32_t)IMU_AXIS_GYRO_X:
        value = reading->gyro_x;
        break;
    case (uint32_t)IMU_AXIS_GYRO_Y:
        value = reading->gyro_y;
        break;
    case (uint32_t)IMU_AXIS_GYRO_Z:
        value = reading->gyro_z;
        break;
    case (uint32_t)IMU_AXIS_TEMP:
        value = reading->temp;
        break;
    default:
        /* Past the last: NAN. */
        break;
    }
    return value;
}

/**
 * @brief The reading of field axis @p idx.
 * @param reading The three axes.
 * @param idx An index into kode_mag_read_axis()'s names.
 * @return The reading; NAN past the last.
 */
static float mag_pick(const kode_mag_t *reading, uint32_t idx)
{
    float value = NAN;

    switch ( idx )
    {
    case (uint32_t)MAG_AXIS_X:
        value = reading->x;
        break;
    case (uint32_t)MAG_AXIS_Y:
        value = reading->y;
        break;
    case (uint32_t)MAG_AXIS_Z:
        value = reading->z;
        break;
    default:
        /* Past the last: NAN. */
        break;
    }
    return value;
}

float kode_imu_read_axis(const char *axis)
{
    float value = NAN;

    if ( motion_enter("kode_imu_read_axis") )
    {
        static const char *const names[IMU_AXIS_COUNT] = {
            [IMU_AXIS_ACCEL_X] = "accel_x", [IMU_AXIS_ACCEL_Y] = "accel_y",
            [IMU_AXIS_ACCEL_Z] = "accel_z", [IMU_AXIS_GYRO_X] = "gyro_x",
            [IMU_AXIS_GYRO_Y] = "gyro_y", [IMU_AXIS_GYRO_Z] = "gyro_z",
            [IMU_AXIS_TEMP] = "temp"
        };
        const uint32_t idx = axis_find(axis, names, (uint32_t)IMU_AXIS_COUNT);
        kode_imu_t     reading = imu_none();

        if ( ((uint32_t)IMU_AXIS_COUNT > idx) && imu_sample(&reading) )
        {
            value = imu_pick(&reading, idx);
        }
        motion_leave();
    }
    return value;
}

kode_imu_t kode_imu_read(void)
{
    kode_imu_t reading = imu_none();

    if ( motion_enter("kode_imu_read") )
    {
        (void)imu_sample(&reading);
        motion_leave();
    }
    return reading;
}

float kode_mag_read_axis(const char *axis)
{
    float value = NAN;

    if ( motion_enter("kode_mag_read_axis") )
    {
        static const char *const names[MAG_AXIS_COUNT] = {
            [MAG_AXIS_X] = "x", [MAG_AXIS_Y] = "y", [MAG_AXIS_Z] = "z"
        };
        const uint32_t idx = axis_find(axis, names, (uint32_t)MAG_AXIS_COUNT);
        kode_mag_t     reading = mag_none();

        if ( ((uint32_t)MAG_AXIS_COUNT > idx) && mag_sample(&reading) )
        {
            value = mag_pick(&reading, idx);
        }
        motion_leave();
    }
    return value;
}

kode_mag_t kode_mag_read(void)
{
    kode_mag_t reading = mag_none();

    if ( motion_enter("kode_mag_read") )
    {
        (void)mag_sample(&reading);
        motion_leave();
    }
    return reading;
}

/* ---- The compass ------------------------------------------------------------ */

/**
 * @brief Read both parts and work the heading out.
 * @param corr The calibration.
 * @return The heading, degrees; NAN with the error set.
 */
static float heading_read(const kdrv_qmc5883p_cal_t *corr)
{
    int32_t    raw[MAG_AXIS_COUNT];
    kode_imu_t motion;
    float      value = NAN;

    if ( mag_raw(raw) && imu_sample(&motion) )
    {
        float          field[MAG_AXIS_COUNT];
        const float    grav[KDRV_QMC5883P_AXES] = { motion.accel_x, motion.accel_y, motion.accel_z };
        float          turn = 0.0F;
        kcompass_rc_t  why;

        mag_correct(corr, true, raw, field);
        why = kcompass_heading(field, grav, &turn);
        if ( KCOMPASS_OK == why )
        {
            value = turn;
        }
        else if ( KCOMPASS_MOVING == why )
        {
            kode_err_set("the Dot is moving too much to tell which way is up");
        }
        else
        {
            kode_err_set("the field is too weak to give a heading");
        }
    }
    return value;
}

float kode_mag_heading(void)
{
    float value = NAN;

    if ( motion_enter("kode_mag_heading") )
    {
        kdrv_qmc5883p_cal_t corr;
        bool                present = false;

        if ( cal_read(&corr, &present) )
        {
            if ( present )
            {
                value = heading_read(&corr);
            }
            else
            {
                kode_err_set("call kode_mag_calibrate() first");
            }
        }
        motion_leave();
    }
    return value;
}

bool kode_mag_is_calibrated(void)
{
    bool yes = false;

    if ( motion_enter("kode_mag_is_calibrated") )
    {
        kdrv_qmc5883p_cal_t corr;

        if ( !cal_read(&corr, &yes) )
        {
            yes = false;
        }
        motion_leave();
    }
    return yes;
}

bool kode_mag_clear_calibration(void)
{
    bool done = false;

    if ( motion_enter("kode_mag_clear_calibration") )
    {
        done = cal_write(NULL);
        motion_leave();
    }
    return done;
}

bool kode_mag_raw_read(const char *name, int32_t mg[KDRV_QMC5883P_AXES])
{
    bool ok = false;

    if ( motion_enter(name) )
    {
        ok = mag_raw(mg);
        motion_leave();
    }
    return ok;
}

bool kode_mag_cal_store(const char *name, const kdrv_qmc5883p_cal_t *corr)
{
    bool ok = false;

    if ( motion_enter(name) )
    {
        ok = cal_write(corr);
        motion_leave();
    }
    return ok;
}
