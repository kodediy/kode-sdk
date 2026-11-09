/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_qmi8658.c
 * @brief QST QMI8658A over the shared I2C bus: identity, configuration, one sample.
 */
#include "kdrv_qmi8658.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "kbsp.h"
#include "khal_i2c.h"
#include "khal_time.h"
#include "kode_log.h"

#define TAG "kdrv.qmi8658"

/* ---- Registers ----------------------------------------------------------- */
#define QMI_REG_WHO_AM_I  0x00U
#define QMI_REG_REVISION  0x01U
#define QMI_REG_CTRL1     0x02U  /* interface: endianness, auto-increment   */
#define QMI_REG_CTRL2     0x03U  /* accelerometer: full scale, rate         */
#define QMI_REG_CTRL3     0x04U  /* gyroscope: full scale, rate             */
#define QMI_REG_CTRL5     0x06U  /* low-pass filters                        */
#define QMI_REG_CTRL7     0x08U  /* sensor enables                          */
#define QMI_REG_CTRL8     0x09U  /* bit 7: CTRL9's handshake on STATUSINT   */
#define QMI_REG_CTRL9     0x0AU  /* host commands                           */
#define QMI_REG_FIFO_CTRL 0x14U
#define QMI_REG_FIFO_CNT  0x15U  /* FIFO_SMPL_CNT, then FIFO_STATUS         */
#define QMI_REG_FIFO_DATA 0x17U
#define QMI_REG_STATUSINT 0x2DU  /* bit 0: self-test done (CTRL9's too)     */
#define QMI_REG_TEMP_L    0x33U  /* TEMP_L, TEMP_H, then AX_L .. GZ_H       */
#define QMI_REG_DVX_L     0x51U  /* dVX_L .. dVZ_H: the self-test's result  */

/* Auto-increment on (bit 6), little-endian (bit 5 clear), both interrupt pins
 * driven outputs (bits 4 and 3). Nothing is mapped to INT1, so it is driven
 * and idle, which is what the expander's P5 needs. INT2 is not connected on
 * this board, and is enabled because the datasheet describes STATUSINT bit 0
 * - what a self-test's end is read from - as showing "the same value of INT2
 * level": a pad left high-impedance is one reading of that sentence under
 * which the bit never rises. Driving a pin wired to nothing costs nothing. */
#define QMI_CTRL1_SETUP   0x58U
/* The same with auto-increment OFF, for the length of a FIFO drain. The
 * datasheet says both that FIFO_DATA is burst-read and that a burst walks on
 * to the next register; with the increment off both readings agree - every
 * byte of the burst is FIFO_DATA. */
#define QMI_CTRL1_FIFO    0x18U
#define QMI_CTRL2_8G_ODR3 0x23U  /* aFS = 010 (+-8 g), aODR = 0011 (896.8 Hz)      */
#define QMI_CTRL3_512_ODR3 0x53U /* gFS = 101 (+-512 dps), gODR = 0011 (896.8 Hz)  */
#define QMI_CTRL5_NO_LPF  0x00U
#define QMI_CTRL7_A_G_EN  0x03U  /* aEN | gEN                                       */
#define QMI_CTRL7_OFF     0x00U  /* neither: the part at rest                       */
#define QMI_CTRL7_A_EN    0x01U  /* aEN alone: the accelerometer at 1000 Hz         */
/* CTRL9's "command done" is reported on STATUSINT bit 7 and NOT on INT1: that
 * pin is the expander's P5, and every command would otherwise be an
 * interrupt to AO_IoExp about nothing. */
#define QMI_CTRL8_HANDSHAKE_STATUSINT 0x80U
#define QMI_CTRL9_ACK       0x00U
#define QMI_CTRL9_RST_FIFO  0x04U
#define QMI_CTRL9_REQ_FIFO  0x05U
#define QMI_STATUSINT_CMD_DONE 0x80U
/** @brief How long a CTRL9 command is given to report done, and then to
 *         acknowledge, in milliseconds - one STATUSINT read per millisecond.
 *         A working part answers a self-test's trigger at once but takes a
 *         few milliseconds over a FIFO reset (measured on the bench: sixteen
 *         reads back to back, about 2 ms, were not enough for
 *         CTRL_CMD_RST_FIFO), and QST's own reference driver polls the bit
 *         a millisecond at a time. This bounds a part that never answers. */
#define QMI_CTRL9_WAIT_MS   50U
#define QMI_FIFO_128_FIFO   0x0DU  /* FIFO_SIZE 11 (128 samples), FIFO_MODE 01  */
#define QMI_FIFO_BYPASS     0x00U  /* also clears FIFO_RD_MODE                   */
#define QMI_FIFO_READ_MAX   32U
/** @brief Records at the head of a window that may be the FIFO's own reset
 *         value rather than a sample, and are skipped. Measured on the
 *         bench, every window: the first record reads 0x8000 on all three
 *         axes - the part's full-scale negative, which no accelerometer at
 *         rest produces - and once in four the second is a torn one with a
 *         full-scale axis in it. The datasheet's data pattern (8.9) starts
 *         at AX_L[0]; what the FIFO memory holds before that is not in it. */
#define QMI_FIFO_JUNK_MAX   2U
#define QMI_CTRL_ST       0x80U  /* aST in CTRL2, gST in CTRL3                      */
#define QMI_STATUSINT_DONE 0x01U
/** @brief How many reads STATUSINT is given to fall after the trigger is
 *         cleared (procedure step 5). It falls at once; this bounds a part
 *         that does not. */
#define QMI_ST_CLEAR_READS 8U

/* The self-test result is fixed point whatever the scales are set to: the
 * accelerometer's has 11 fractional bits of a g, the gyroscope's 4 of a
 * degree per second. */
#define QMI_ST_XL_LSB_PER_G   2048
#define QMI_ST_GY_LSB_PER_DPS 16

/** @brief TEMP_L, TEMP_H, six bytes of acceleration, six of rotation. */
#define QMI_BURST_LEN     14U

/* +-8 g over 16 bits is 4096 LSB/g; +-512 dps is 64 LSB/dps; the temperature
 * is a signed 8.8 fixed-point number of degrees. */
#define QMI_LSB_PER_G     4096
#define QMI_LSB_PER_DPS   64
#define QMI_TEMP_LSB_PER_C 256

#define QMI_PROBE_MS      20U

static khal_i2c_dev_t    s_dev;
static bool              s_up;
/* The first record of a window that read as a sample, held back from the
 * skip in kdrv_qmi8658_fifo_open() for the first kdrv_qmi8658_fifo_read()
 * to hand out - see QMI_FIFO_JUNK_MAX. */
static uint8_t           s_stash[6];
static bool              s_stashed;
/** @brief What the identity registers read at bring-up: a caller that asks
 *         again once the part is up is told the same thing, not zeroes. */
static kdrv_qmi8658_id_t s_id;

/** @brief Two bytes, low first, as a signed sixteen-bit number. */
static int32_t le16(const uint8_t *p)
{
    uint16_t const u = (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8U));

    return (int32_t)(int16_t)u;
}

/** @brief Write one register and read it back; true when it holds @p val. */
static bool write_checked(uint8_t reg, uint8_t val)
{
    uint8_t got = 0U;

    if ( khal_i2c_write_reg(s_dev, reg, val) != KODE_OK )
    {
        return false;
    }
    if ( khal_i2c_read_reg(s_dev, reg, &got, 1U) != KODE_OK )
    {
        return false;
    }
    if ( got != val )
    {
        KLOG_E(TAG, "register 0x%02X reads 0x%02X after writing 0x%02X",
               (unsigned)reg, (unsigned)got, (unsigned)val);
        return false;
    }
    return true;
}

kode_status_t kdrv_qmi8658_park(void)
{
    /* One quiet probe first: a part that is not fitted is a fact here, said
     * once by the caller, and no warning of the bring-up's. */
    if ( (khal_i2c_init() != KODE_OK) || !khal_i2c_probe(KBSP_I2C_ADDR_IMU, QMI_PROBE_MS) )
    {
        return KODE_ERROR;
    }
    return kdrv_qmi8658_create(NULL);
}

kode_status_t kdrv_qmi8658_create(kdrv_qmi8658_id_t *id)
{
    uint8_t who[2] = { 0U, 0U };

    if ( s_up )
    {
        if ( NULL != id )
        {
            *id = s_id;
        }
        return KODE_OK;
    }
    if ( khal_i2c_init() != KODE_OK )
    {
        return KODE_IO_ERROR;
    }
    if ( !khal_i2c_probe(KBSP_I2C_ADDR_IMU, QMI_PROBE_MS) )
    {
        KLOG_W(TAG, "no ACK at 0x%02X", (unsigned)KBSP_I2C_ADDR_IMU);
        return KODE_IO_ERROR;
    }
    if ( NULL == s_dev )
    {
        if ( khal_i2c_add(KBSP_I2C_ADDR_IMU, &s_dev) != KODE_OK )
        {
            s_dev = NULL;
            return KODE_IO_ERROR;
        }
    }
    if ( khal_i2c_read_reg(s_dev, QMI_REG_WHO_AM_I, who, sizeof(who)) != KODE_OK )
    {
        return KODE_IO_ERROR;
    }
    if ( NULL != id )
    {
        id->whoami   = who[0];
        id->revision = who[1];
    }
    if ( KDRV_QMI8658_WHOAMI != who[0] )
    {
        KLOG_E(TAG, "0x%02X answers WHO_AM_I=0x%02X, not a QMI8658's 0x%02X",
               (unsigned)KBSP_I2C_ADDR_IMU, (unsigned)who[0],
               (unsigned)KDRV_QMI8658_WHOAMI);
        return KODE_ERROR;
    }

    /* Sensors off while the scales and rates change, and left off: the part
     * rests until its owner has somebody to convert for. */
    if ( (khal_i2c_write_reg(s_dev, QMI_REG_CTRL7, QMI_CTRL7_OFF) != KODE_OK)
        || !write_checked(QMI_REG_CTRL1, QMI_CTRL1_SETUP)
        || !write_checked(QMI_REG_CTRL2, QMI_CTRL2_8G_ODR3)
        || !write_checked(QMI_REG_CTRL3, QMI_CTRL3_512_ODR3)
        || !write_checked(QMI_REG_CTRL5, QMI_CTRL5_NO_LPF)
        || !write_checked(QMI_REG_CTRL8, QMI_CTRL8_HANDSHAKE_STATUSINT) )
    {
        KLOG_E(TAG, "the part would not take its configuration");
        return KODE_IO_ERROR;
    }

    s_id.whoami   = who[0];
    s_id.revision = who[1];
    s_up = true;
    KLOG_D(TAG, "QMI8658 at 0x%02X, revision 0x%02X: +-8 g, +-512 dps, both "
                "sensors off until something asks",
           (unsigned)KBSP_I2C_ADDR_IMU, (unsigned)who[1]);
    return KODE_OK;
}

kode_status_t kdrv_qmi8658_standby(bool on)
{
    uint8_t ctrl7 = QMI_CTRL7_A_G_EN;

    if ( !s_up )
    {
        return KODE_DENIED;
    }
    if ( on )
    {
        ctrl7 = QMI_CTRL7_OFF;
    }
    if ( !write_checked(QMI_REG_CTRL7, ctrl7) )
    {
        KLOG_E(TAG, "the part would not take its sensor enables");
        return KODE_IO_ERROR;
    }
    return KODE_OK;
}

kode_status_t kdrv_qmi8658_read(kdrv_qmi8658_sample_t *out)
{
    uint8_t raw[QMI_BURST_LEN];

    if ( (NULL == out) || !s_up )
    {
        return KODE_DENIED;
    }
    if ( khal_i2c_read_reg(s_dev, QMI_REG_TEMP_L, raw, sizeof(raw)) != KODE_OK )
    {
        return KODE_IO_ERROR;
    }

    out->temp_dc = (le16(&raw[0]) * 10) / QMI_TEMP_LSB_PER_C;
    for ( size_t i = 0U; 3U > i; ++i )
    {
        out->a_mg[i]   = (le16(&raw[2U + (2U * i)]) * 1000) / QMI_LSB_PER_G;
        out->w_mdps[i] = (le16(&raw[8U + (2U * i)]) * 1000) / QMI_LSB_PER_DPS;
    }
    return KODE_OK;
}

/** @brief The control register that carries @p which's trigger, and what it
 *         holds outside a self-test. */
static void st_reg(kdrv_qmi8658_sensor_t which, uint8_t *reg, uint8_t *rest)
{
    if ( KDRV_QMI8658_XL == which )
    {
        *reg  = QMI_REG_CTRL2;
        *rest = QMI_CTRL2_8G_ODR3;
    }
    else
    {
        *reg  = QMI_REG_CTRL3;
        *rest = QMI_CTRL3_512_ODR3;
    }
}

kode_status_t kdrv_qmi8658_st_begin(kdrv_qmi8658_sensor_t which)
{
    uint8_t reg;
    uint8_t rest;

    if ( !s_up )
    {
        return KODE_DENIED;
    }
    st_reg(which, &reg, &rest);
    /* Not write_checked(): the part rewrites the scale for the length of the
     * self-test, so what reads back is its business until it hands it back. */
    if ( (khal_i2c_write_reg(s_dev, QMI_REG_CTRL7, QMI_CTRL7_OFF) != KODE_OK)
        || (khal_i2c_write_reg(s_dev, reg, (uint8_t)(rest | QMI_CTRL_ST))
            != KODE_OK) )
    {
        return KODE_IO_ERROR;
    }
    return KODE_OK;
}

kode_status_t kdrv_qmi8658_st_poll(kdrv_qmi8658_sensor_t which, bool *done,
                                   int32_t shift[3])
{
    uint8_t reg;
    uint8_t rest;
    uint8_t status = 0U;
    uint8_t raw[6];
    int32_t num = 1000;
    int32_t den = QMI_ST_XL_LSB_PER_G;

    if ( (NULL == done) || (NULL == shift) || !s_up )
    {
        return KODE_DENIED;
    }
    *done = false;
    if ( khal_i2c_read_reg(s_dev, QMI_REG_STATUSINT, &status, 1U) != KODE_OK )
    {
        return KODE_IO_ERROR;
    }
    if ( (status & QMI_STATUSINT_DONE) == 0U )
    {
        return KODE_OK;
    }

    /* Done. Clear the trigger, see the flag fall, then read what moved. */
    st_reg(which, &reg, &rest);
    if ( khal_i2c_write_reg(s_dev, reg, rest) != KODE_OK )
    {
        return KODE_IO_ERROR;
    }
    for ( uint8_t i = 0U; QMI_ST_CLEAR_READS > i; ++i )
    {
        if ( khal_i2c_read_reg(s_dev, QMI_REG_STATUSINT, &status, 1U) != KODE_OK )
        {
            return KODE_IO_ERROR;
        }
        if ( (status & QMI_STATUSINT_DONE) == 0U )
        {
            break;
        }
    }
    if ( (status & QMI_STATUSINT_DONE) != 0U )
    {
        KLOG_W(TAG, "the self-test flag did not fall after its trigger was "
                    "cleared; reading the result anyway");
    }
    if ( khal_i2c_read_reg(s_dev, QMI_REG_DVX_L, raw, sizeof(raw)) != KODE_OK )
    {
        return KODE_IO_ERROR;
    }
    if ( KDRV_QMI8658_GY == which )
    {
        den = QMI_ST_GY_LSB_PER_DPS;
    }
    for ( size_t i = 0U; 3U > i; ++i )
    {
        shift[i] = (le16(&raw[2U * i]) * num) / den;
    }
    *done = true;
    return KODE_OK;
}

bool kdrv_qmi8658_st_judge(kdrv_qmi8658_sensor_t which, const int32_t shift[3])
{
    int32_t floor_v = KDRV_QMI8658_ST_XL_MIN_MG;
    bool    ok      = true;

    if ( NULL == shift )
    {
        return false;
    }
    if ( KDRV_QMI8658_GY == which )
    {
        floor_v = KDRV_QMI8658_ST_GY_MIN_MDPS;
    }
    for ( size_t i = 0U; 3U > i; ++i )
    {
        int32_t v = shift[i];

        if ( 0 > v )
        {
            v = -v;
        }
        if ( v < floor_v )
        {
            ok = false;
        }
    }
    return ok;
}

kode_status_t kdrv_qmi8658_st_end(void)
{
    if ( !s_up )
    {
        return KODE_DENIED;
    }
    if ( (khal_i2c_write_reg(s_dev, QMI_REG_CTRL7, QMI_CTRL7_OFF) != KODE_OK)
        || !write_checked(QMI_REG_CTRL2, QMI_CTRL2_8G_ODR3)
        || !write_checked(QMI_REG_CTRL3, QMI_CTRL3_512_ODR3)
        || !write_checked(QMI_REG_CTRL7, QMI_CTRL7_A_G_EN) )
    {
        KLOG_E(TAG, "the part would not take its configuration back after "
                    "the self-test");
        return KODE_IO_ERROR;
    }
    return KODE_OK;
}

/**
 * @brief Wait for STATUSINT's command-done bit to read @p want.
 *
 * One read, then a millisecond, up to QMI_CTRL9_WAIT_MS of them: the part
 * works on the command on its own clock, and a read every millisecond is
 * what its maker's reference driver does. The delay yields the thread, so a
 * command that takes three milliseconds costs the owner three milliseconds
 * of its step and nobody else anything.
 *
 * @param want       true to wait for the bit to rise, false for it to fall.
 * @param[out] last  What STATUSINT last read, for the log line.
 * @return KODE_OK, KODE_IO_ERROR for the bus, KODE_TIMEOUT when the bit
 *         never read @p want.
 */
static kode_status_t cmd_done_wait(bool want, uint8_t *last)
{
    uint8_t status = 0U;

    for ( uint32_t ms = 0U; QMI_CTRL9_WAIT_MS >= ms; ++ms )
    {
        if ( khal_i2c_read_reg(s_dev, QMI_REG_STATUSINT, &status, 1U) != KODE_OK )
        {
            return KODE_IO_ERROR;
        }
        *last = status;
        if ( ((status & QMI_STATUSINT_CMD_DONE) != 0U) == want )
        {
            return KODE_OK;
        }
        khal_delay_ms(1U);
    }
    return KODE_TIMEOUT;
}

/**
 * @brief Run one CTRL9 command through its handshake (datasheet 5.10.1).
 *
 * The command, the wait for STATUSINT bit 7, the acknowledge, the wait for
 * the bit to fall. Both waits are bounded by QMI_CTRL9_WAIT_MS; the first
 * one failing is the part not taking commands, and the byte it read goes in
 * the log, since that is the one register that says what the part thinks
 * it is doing.
 *
 * @param cmd The command.
 * @return KODE_OK, KODE_IO_ERROR for the bus, KODE_TIMEOUT when the part
 *         never reported the command done.
 */
static kode_status_t ctrl9(uint8_t cmd)
{
    uint8_t       status = 0U;
    kode_status_t rc;

    if ( khal_i2c_write_reg(s_dev, QMI_REG_CTRL9, cmd) != KODE_OK )
    {
        return KODE_IO_ERROR;
    }
    rc = cmd_done_wait(true, &status);
    if ( KODE_TIMEOUT == rc )
    {
        KLOG_E(TAG, "CTRL9 command 0x%02X was never reported done in %u ms "
                    "(STATUSINT reads 0x%02X)", (unsigned)cmd,
               (unsigned)QMI_CTRL9_WAIT_MS, (unsigned)status);
    }
    if ( KODE_OK != rc )
    {
        return rc;
    }
    if ( khal_i2c_write_reg(s_dev, QMI_REG_CTRL9, QMI_CTRL9_ACK) != KODE_OK )
    {
        return KODE_IO_ERROR;
    }
    /* A bit that does not fall after the acknowledge is logged and not
     * fatal: the command itself was done, and the next command's own wait
     * for the rise is what a stuck bit would break. */
    rc = cmd_done_wait(false, &status);
    if ( KODE_TIMEOUT == rc )
    {
        KLOG_W(TAG, "CTRL9 command 0x%02X: the done bit did not fall after the "
                    "acknowledge (STATUSINT reads 0x%02X)", (unsigned)cmd,
               (unsigned)status);
        rc = KODE_OK;
    }
    return rc;
}

kode_status_t kdrv_qmi8658_fifo_arm(void)
{
    kode_status_t rc;

    if ( !s_up )
    {
        return KODE_DENIED;
    }
    /* The gyroscope off and the accelerometer left running, the FIFO
     * emptied, then the FIFO switched on: a window begins with nothing in
     * it, and its first sample is one of a sensor that is already
     * converting rather than the start-up of one just enabled - which is
     * what a window judged at one frequency bin wants, since a sensor
     * turning on is a transient across every bin. */
    if ( (khal_i2c_write_reg(s_dev, QMI_REG_CTRL7, QMI_CTRL7_A_EN) != KODE_OK)
        || (khal_i2c_write_reg(s_dev, QMI_REG_FIFO_CTRL, QMI_FIFO_BYPASS)
            != KODE_OK) )
    {
        return KODE_IO_ERROR;
    }
    rc = ctrl9(QMI_CTRL9_RST_FIFO);
    if ( KODE_OK != rc )
    {
        return rc;
    }
    if ( khal_i2c_write_reg(s_dev, QMI_REG_FIFO_CTRL, QMI_FIFO_128_FIFO)
        != KODE_OK )
    {
        return KODE_IO_ERROR;
    }
    return KODE_OK;
}

/**
 * @brief Is @p rec the FIFO's own reset value rather than a sample?
 *
 * A full-scale reading on any axis: 0x8000 or 0x7FFF, which an accelerometer
 * set to +-8 g and lying on a bench never produces, and which is what the
 * torn records at the head of a window carry.
 *
 * @param rec Six bytes, little-endian, x y z.
 * @return true when the record is not a sample.
 */
static bool record_is_junk(const uint8_t rec[6])
{
    bool junk = false;

    for ( size_t a = 0U; (3U > a) && !junk; ++a )
    {
        int16_t const v = (int16_t)le16(&rec[2U * a]);

        if ( (INT16_MIN == v) || (INT16_MAX == v) )
        {
            junk = true;
        }
    }
    return junk;
}

kode_status_t kdrv_qmi8658_fifo_open(size_t *samples)
{
    uint8_t       cnt[2] = { 0U, 0U };
    size_t        words;
    kode_status_t rc;

    if ( (NULL == samples) || !s_up )
    {
        return KODE_DENIED;
    }
    *samples = 0U;
    if ( khal_i2c_read_reg(s_dev, QMI_REG_FIFO_CNT, cnt, sizeof(cnt)) != KODE_OK )
    {
        return KODE_IO_ERROR;
    }
    /* Ten bits, in words of two bytes; a sample is three of them. */
    words = (((size_t)cnt[1] & 0x03U) << 8U) | (size_t)cnt[0];
    rc = ctrl9(QMI_CTRL9_REQ_FIFO);
    if ( KODE_OK != rc )
    {
        return rc;
    }
    s_stashed = false;
    /* After the count, which is itself a two-register burst. */
    if ( !write_checked(QMI_REG_CTRL1, QMI_CTRL1_FIFO) )
    {
        return KODE_IO_ERROR;
    }
    *samples = words / 3U;
    if ( (size_t)KDRV_QMI8658_FIFO_DEPTH < *samples )
    {
        *samples = (size_t)KDRV_QMI8658_FIFO_DEPTH;
    }
    /* The head of the window: what is not a sample is skipped, and the first
     * record that is one is kept for the first read. */
    for ( uint32_t skipped = 0U; (0U < *samples) && !s_stashed; ++skipped )
    {
        if ( khal_i2c_read_reg(s_dev, QMI_REG_FIFO_DATA, s_stash, sizeof(s_stash))
            != KODE_OK )
        {
            return KODE_IO_ERROR;
        }
        if ( (QMI_FIFO_JUNK_MAX > skipped) && record_is_junk(s_stash) )
        {
            KLOG_D(TAG, "fifo: record %u at the head is not a sample "
                        "(%02X%02X %02X%02X %02X%02X); skipped",
                   (unsigned)skipped, s_stash[0], s_stash[1], s_stash[2],
                   s_stash[3], s_stash[4], s_stash[5]);
            *samples -= 1U;
        }
        else
        {
            s_stashed = true;
        }
    }
    return KODE_OK;
}

kode_status_t kdrv_qmi8658_fifo_read(int16_t *xyz, size_t n)
{
    uint8_t raw[QMI_FIFO_READ_MAX * 6U];

    size_t  from = 0U;

    if ( (NULL == xyz) || (0U == n) || ((size_t)QMI_FIFO_READ_MAX < n) || !s_up )
    {
        return KODE_DENIED;
    }
    if ( s_stashed )
    {
        /* The record kdrv_qmi8658_fifo_open() already took off the FIFO. */
        (void)memcpy(raw, s_stash, sizeof(s_stash));
        s_stashed = false;
        from      = 1U;
    }
    if ( (n > from)
        && (khal_i2c_read_reg(s_dev, QMI_REG_FIFO_DATA, &raw[6U * from],
                              (n - from) * 6U) != KODE_OK) )
    {
        return KODE_IO_ERROR;
    }
    for ( size_t i = 0U; (n * 3U) > i; ++i )
    {
        xyz[i] = (int16_t)le16(&raw[2U * i]);
    }
    return KODE_OK;
}

kode_status_t kdrv_qmi8658_fifo_disarm(void)
{
    if ( !s_up )
    {
        return KODE_DENIED;
    }
    if ( (khal_i2c_write_reg(s_dev, QMI_REG_FIFO_CTRL, QMI_FIFO_BYPASS) != KODE_OK)
        || !write_checked(QMI_REG_CTRL1, QMI_CTRL1_SETUP)
        || !write_checked(QMI_REG_CTRL7, QMI_CTRL7_A_G_EN) )
    {
        KLOG_E(TAG, "the part would not leave its FIFO window");
        return KODE_IO_ERROR;
    }
    return KODE_OK;
}
