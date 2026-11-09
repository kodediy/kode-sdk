/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_qmc5883p.c
 * @brief QST QMC5883P over the shared I2C bus: identity, configuration, one sample.
 */
#include "kdrv_qmc5883p.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "kbsp.h"
#include "khal_i2c.h"
#include "khal_time.h"
#include "kode_log.h"

#define TAG "kdrv.qmc5883p"

/* ---- Registers ----------------------------------------------------------- */
/* Not the QMC5883L's map: the chip id is at 0x00, the data from 0x01, the status at 0x09. */
#define QMC_REG_CHIP_ID   0x00U
#define QMC_REG_X_LSB     0x01U  /* X, Y, Z: low byte then high byte each   */
#define QMC_REG_STATUS    0x09U
#define QMC_REG_CTRL1     0x0AU  /* mode, rate, oversampling                */
#define QMC_REG_CTRL2     0x0BU  /* set/reset mode, range, soft reset       */
#define QMC_REG_SIGN      0x29U  /* axis signs; every datasheet example
                                  * writes 0x06 here before anything else   */
#define QMC_SIGN_XYZ      0x06U

#define QMC_STATUS_DRDY   0x01U
#define QMC_STATUS_OVFL   0x02U

/* CTRL1: OSR2 = 00 (down-sampling 1), OSR1 = 00 (over-sampling 8),
 * ODR = 01 (50 Hz), MODE = 11 (continuous). */
#define QMC_CTRL1_CONT_50 0x07U
/* CTRL2: no reset, no self-test, RNG = 00 (+-30 G, 1000 LSB/G),
 * set/reset = 00 (both on, the datasheet's recommended default). */
#define QMC_CTRL2_30G     0x00U
#define QMC_CTRL2_SELF_TEST 0x40U  /* clears itself once the data is updated */
#define QMC_CTRL1_SUSPEND 0x00U
/** @brief What the datasheet's self-test example waits (5 ms), doubled. */
#define QMC_SELF_TEST_MS  10U

#define QMC_PROBE_MS      20U
/** @brief One conversion period at 50 Hz, and a little: what a read that
 *         found nothing ready waits before it asks once more. */
#define QMC_CONVERSION_MS 25U

static khal_i2c_dev_t s_dev;
static bool           s_up;
/** @brief The chip id read at bring-up, for a caller that asks again. */
static uint8_t        s_chip_id;

kode_status_t kdrv_qmc5883p_park(void)
{
    /* One quiet probe first: a part that is not fitted is a fact here, said
     * once by the caller, and no warning of the bring-up's. */
    if ( (khal_i2c_init() != KODE_OK) || !khal_i2c_probe(KBSP_I2C_ADDR_MAG, QMC_PROBE_MS) )
    {
        return KODE_ERROR;
    }
    return kdrv_qmc5883p_create(NULL);
}

bool kdrv_qmc5883p_stray(void)
{
    khal_i2c_dev_t dev   = NULL;
    uint8_t        id    = 0U;
    bool           stray = false;

    if ( (khal_i2c_init() != KODE_OK)
        || !khal_i2c_probe(KBSP_I2C_ADDR_MAG_STRAY, QMC_PROBE_MS) )
    {
        return false;
    }
    if ( khal_i2c_add(KBSP_I2C_ADDR_MAG_STRAY, &dev) != KODE_OK )
    {
        return false;
    }
    if ( (khal_i2c_read_reg(dev, QMC_REG_CHIP_ID, &id, 1U) == KODE_OK)
        && (KDRV_QMC5883P_CHIP_ID == id) )
    {
        stray = true;
    }
    (void)khal_i2c_remove(dev);
    return stray;
}

kode_status_t kdrv_qmc5883p_create(uint8_t *chip_id)
{
    uint8_t id = 0U;

    if ( s_up )
    {
        if ( NULL != chip_id )
        {
            *chip_id = s_chip_id;
        }
        return KODE_OK;
    }
    if ( khal_i2c_init() != KODE_OK )
    {
        return KODE_IO_ERROR;
    }
    if ( !khal_i2c_probe(KBSP_I2C_ADDR_MAG, QMC_PROBE_MS) )
    {
        KLOG_W(TAG, "no ACK at 0x%02X", (unsigned)KBSP_I2C_ADDR_MAG);
        return KODE_IO_ERROR;
    }
    if ( NULL == s_dev )
    {
        if ( khal_i2c_add(KBSP_I2C_ADDR_MAG, &s_dev) != KODE_OK )
        {
            s_dev = NULL;
            return KODE_IO_ERROR;
        }
    }
    if ( khal_i2c_read_reg(s_dev, QMC_REG_CHIP_ID, &id, 1U) != KODE_OK )
    {
        return KODE_IO_ERROR;
    }
    if ( NULL != chip_id )
    {
        *chip_id = id;
    }
    if ( KDRV_QMC5883P_CHIP_ID != id )
    {
        KLOG_E(TAG, "0x%02X answers chip id 0x%02X, not a QMC5883P's 0x%02X",
               (unsigned)KBSP_I2C_ADDR_MAG, (unsigned)id,
               (unsigned)KDRV_QMC5883P_CHIP_ID);
        return KODE_ERROR;
    }

    /* The signs, then the range, then the mode - the datasheet's own order,
     * so the first conversion is already what every later one will be. The
     * mode is suspend: the part rests until its owner has somebody to
     * convert for. */
    if ( (khal_i2c_write_reg(s_dev, QMC_REG_SIGN, QMC_SIGN_XYZ) != KODE_OK)
        || (khal_i2c_write_reg(s_dev, QMC_REG_CTRL2, QMC_CTRL2_30G) != KODE_OK)
        || (khal_i2c_write_reg(s_dev, QMC_REG_CTRL1, QMC_CTRL1_SUSPEND)
            != KODE_OK) )
    {
        KLOG_E(TAG, "the part would not take its configuration");
        return KODE_IO_ERROR;
    }

    s_chip_id = id;
    s_up = true;
    KLOG_D(TAG, "QMC5883P at 0x%02X: +-30 G, suspended until something asks",
           (unsigned)KBSP_I2C_ADDR_MAG);
    return KODE_OK;
}

kode_status_t kdrv_qmc5883p_standby(bool on)
{
    uint8_t ctrl1 = QMC_CTRL1_CONT_50;

    if ( !s_up )
    {
        return KODE_DENIED;
    }
    if ( on )
    {
        ctrl1 = QMC_CTRL1_SUSPEND;
    }
    if ( khal_i2c_write_reg(s_dev, QMC_REG_CTRL1, ctrl1) != KODE_OK )
    {
        return KODE_IO_ERROR;
    }
    return KODE_OK;
}

kode_status_t kdrv_qmc5883p_read(int32_t mg[KDRV_QMC5883P_AXES])
{
    uint8_t status = 0U;
    uint8_t raw[6];

    if ( (NULL == mg) || !s_up )
    {
        return KODE_DENIED;
    }
    if ( khal_i2c_read_reg(s_dev, QMC_REG_STATUS, &status, 1U) != KODE_OK )
    {
        return KODE_IO_ERROR;
    }
    if ( (status & QMC_STATUS_DRDY) == 0U )
    {
        /* A read that lands inside the part's first conversion - a row that
         * had to bring the part up itself - is early, not failed. Once. */
        khal_delay_ms(QMC_CONVERSION_MS);
        if ( khal_i2c_read_reg(s_dev, QMC_REG_STATUS, &status, 1U) != KODE_OK )
        {
            return KODE_IO_ERROR;
        }
    }
    if ( ((status & QMC_STATUS_DRDY) == 0U)
        || ((status & QMC_STATUS_OVFL) != 0U) )
    {
        KLOG_W(TAG, "status 0x%02X: no conversion ready, or one overflowed",
               (unsigned)status);
        return KODE_ERROR;
    }
    if ( khal_i2c_read_reg(s_dev, QMC_REG_X_LSB, raw, sizeof(raw)) != KODE_OK )
    {
        return KODE_IO_ERROR;
    }
    for ( size_t i = 0U; 3U > i; ++i )
    {
        uint16_t const u = (uint16_t)((uint16_t)raw[2U * i]
                                      | ((uint16_t)raw[(2U * i) + 1U] << 8U));

        /* +-30 G is 1000 LSB/G: one LSB is one milligauss. */
        mg[i] = (int32_t)(int16_t)u;
    }
    return KODE_OK;
}

kode_status_t kdrv_qmc5883p_self_test(int32_t delta_mg[KDRV_QMC5883P_AXES])
{
    int32_t       before[3] = { 0, 0, 0 };
    uint8_t       raw[6];
    kode_status_t rc;

    if ( (NULL == delta_mg) || !s_up )
    {
        return KODE_DENIED;
    }
    rc = kdrv_qmc5883p_read(before);
    if ( KODE_OK != rc )
    {
        return rc;
    }
    if ( khal_i2c_write_reg(s_dev, QMC_REG_CTRL2,
                           (uint8_t)(QMC_CTRL2_30G | QMC_CTRL2_SELF_TEST))
        != KODE_OK )
    {
        return KODE_IO_ERROR;
    }
    khal_delay_ms(QMC_SELF_TEST_MS);
    rc = khal_i2c_read_reg(s_dev, QMC_REG_X_LSB, raw, sizeof(raw));

    /* The part drops to suspend once the self-test's data is out. Back to
     * continuous whatever the read said, through suspend as the datasheet
     * asks of every mode change. */
    if ( (khal_i2c_write_reg(s_dev, QMC_REG_CTRL1, QMC_CTRL1_SUSPEND) != KODE_OK)
        || (khal_i2c_write_reg(s_dev, QMC_REG_CTRL1, QMC_CTRL1_CONT_50)
            != KODE_OK) )
    {
        s_up = false;   /* the next create() configures it afresh */
        return KODE_IO_ERROR;
    }
    if ( KODE_OK != rc )
    {
        return KODE_IO_ERROR;
    }
    for ( size_t i = 0U; 3U > i; ++i )
    {
        uint16_t const u = (uint16_t)((uint16_t)raw[2U * i]
                                      | ((uint16_t)raw[(2U * i) + 1U] << 8U));

        delta_mg[i] = (int32_t)(int16_t)u - before[i];
    }
    /* The datasheet's own arithmetic (7.3): x1 - x2, then y2 - y1 and
     * z2 - z1. X is the other way round from the other two, as written. */
    delta_mg[0] = -delta_mg[0];
    return KODE_OK;
}

/* ---- The calibration ------------------------------------------------------ */

#define CAL_NUMBERS     6U     /**< Numbers in a calibration's text.            */
#define CAL_DIGITS_MAX  5U     /**< Digits of one number in the text.           */
#define CAL_RADIX       10     /**< Base of the text's numbers.                 */
#define CAL_TO_UNIT     1000.0F /**< Thousandths in one.                        */

/**
 * @brief Whether every number of @p corr is within its limits.
 * @param corr The calibration.
 * @return true when it can be written and read back.
 */
static bool cal_in_limits(const kdrv_qmc5883p_cal_t *corr)
{
    bool ok = true;

    for ( uint32_t i = 0U; KDRV_QMC5883P_AXES > i; i++ )
    {
        ok = ok && (-KDRV_QMC5883P_CAL_OFFSET_MAX_MG <= corr->offset_mg[i])
             && (KDRV_QMC5883P_CAL_OFFSET_MAX_MG >= corr->offset_mg[i])
             && (KDRV_QMC5883P_CAL_SCALE_MIN <= corr->scale_milli[i])
             && (KDRV_QMC5883P_CAL_SCALE_MAX >= corr->scale_milli[i]);
    }
    return ok;
}

/**
 * @brief Read one number of the text.
 * @param p In: where it starts; out: just past it.
 * @param[out] v The number.
 * @return true when an optional '-' and one to CAL_DIGITS_MAX digits were there.
 */
static bool cal_number(const char **p, int32_t *v)
{
    const char *q = *p;
    bool        neg = false;
    uint32_t    digits = 0U;
    int32_t     n = 0;

    if ( '-' == *q )
    {
        neg = true;
        q++;
    }
    while ( ('0' <= *q) && ('9' >= *q) && (CAL_DIGITS_MAX >= digits) )
    {
        n = (n * CAL_RADIX) + (int32_t)(*q - '0');
        digits++;
        q++;
    }
    if ( neg )
    {
        n = -n;
    }
    *p = q;
    *v = n;
    return (1U <= digits) && (CAL_DIGITS_MAX >= digits);
}

bool kdrv_qmc5883p_cal_parse(const char *text, kdrv_qmc5883p_cal_t *corr)
{
    kdrv_qmc5883p_cal_t got;
    int32_t             v[CAL_NUMBERS];
    const char         *p = text;
    bool                ok = (NULL != text) && (NULL != corr);

    for ( uint32_t i = 0U; ok && (CAL_NUMBERS > i); i++ )
    {
        ok = cal_number(&p, &v[i]);
        if ( ok && ((CAL_NUMBERS - 1U) > i) )
        {
            ok = (' ' == *p);
            p++;
        }
    }
    if ( ok )
    {
        ok = ('\0' == *p);
    }
    if ( ok )
    {
        for ( uint32_t i = 0U; KDRV_QMC5883P_AXES > i; i++ )
        {
            got.offset_mg[i]   = v[i];
            got.scale_milli[i] = v[KDRV_QMC5883P_AXES + i];
        }
        ok = cal_in_limits(&got);
    }
    if ( ok )
    {
        *corr = got;
    }
    return ok;
}

size_t kdrv_qmc5883p_cal_format(const kdrv_qmc5883p_cal_t *corr, char *out, size_t cap)
{
    size_t n = 0U;

    if ( (NULL != corr) && (NULL != out) && (0U < cap) && cal_in_limits(corr) )
    {
        const int32_t w = (int32_t)snprintf(out, cap,
                                            "%" PRId32 " %" PRId32 " %" PRId32 " %" PRId32 " %" PRId32 " %" PRId32,
                                            corr->offset_mg[0], corr->offset_mg[1], corr->offset_mg[2],
                                            corr->scale_milli[0], corr->scale_milli[1], corr->scale_milli[2]);

        if ( (0 < w) && ((size_t)w < cap) )
        {
            n = (size_t)w;
        }
        else
        {
            out[0] = '\0';
        }
    }
    return n;
}

void kdrv_qmc5883p_cal_apply(const kdrv_qmc5883p_cal_t *corr,
                             const int32_t raw_mg[KDRV_QMC5883P_AXES],
                             float out_mg[KDRV_QMC5883P_AXES])
{
    for ( uint32_t i = 0U; KDRV_QMC5883P_AXES > i; i++ )
    {
        const int32_t moved = raw_mg[i] - corr->offset_mg[i];

        out_mg[i] = ((float)moved * (float)corr->scale_milli[i]) / CAL_TO_UNIT;
    }
}

kode_status_t kdrv_qmc5883p_cal_fit(const int32_t lo[KDRV_QMC5883P_AXES],
                                    const int32_t hi[KDRV_QMC5883P_AXES],
                                    kdrv_qmc5883p_cal_t *corr)
{
    kdrv_qmc5883p_cal_t got;
    int32_t             half[KDRV_QMC5883P_AXES];
    int32_t             mean = 0;
    kode_status_t       rc = KODE_OK;

    if ( (NULL == lo) || (NULL == hi) || (NULL == corr) )
    {
        return KODE_DENIED;
    }
    for ( uint32_t i = 0U; KDRV_QMC5883P_AXES > i; i++ )
    {
        half[i] = (hi[i] - lo[i]) / 2;
        if ( 0 >= half[i] )
        {
            return KODE_DENIED;
        }
        got.offset_mg[i] = lo[i] + half[i];
        mean += half[i];
    }
    mean /= (int32_t)KDRV_QMC5883P_AXES;
    for ( uint32_t i = 0U; KDRV_QMC5883P_AXES > i; i++ )
    {
        int32_t s = ((mean * KDRV_QMC5883P_CAL_SCALE_UNIT) + (half[i] / 2)) / half[i];

        if ( KDRV_QMC5883P_CAL_SCALE_MIN > s )
        {
            s = KDRV_QMC5883P_CAL_SCALE_MIN;
        }
        if ( KDRV_QMC5883P_CAL_SCALE_MAX < s )
        {
            s = KDRV_QMC5883P_CAL_SCALE_MAX;
        }
        got.scale_milli[i] = s;
    }
    if ( cal_in_limits(&got) )
    {
        *corr = got;
    }
    else
    {
        rc = KODE_ERROR;
    }
    return rc;
}
