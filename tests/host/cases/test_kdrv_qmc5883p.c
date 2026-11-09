/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_qmc5883p.c
 * @brief kdrv_qmc5883p on a mocked bus: identity, configuration, milligauss and self-test.
 */
#include <string.h>

#include "unity.h"

#include "Mockkhal_i2c.h"

#include "kdrv_qmc5883p.h"

#define REG_CHIP_ID  0x00U
#define REG_X_LSB    0x01U
#define REG_STATUS   0x09U
#define REG_CTRL1    0x0AU
#define REG_CTRL2    0x0BU
#define REG_SIGN     0x29U

static khal_i2c_dev_t s_handle = (khal_i2c_dev_t)(void *)&s_handle;

static uint8_t s_hold[16][8];
static size_t  s_hold_n;

static void expect_read(uint8_t reg, const uint8_t *bytes, size_t len)
{
    uint8_t * const hold = s_hold[s_hold_n % 16U];

    s_hold_n += 1U;
    (void)memcpy(hold, bytes, len);
    khal_i2c_read_reg_ExpectAndReturn(NULL, reg, NULL, len, KODE_OK);
    khal_i2c_read_reg_IgnoreArg_dev();
    khal_i2c_read_reg_IgnoreArg_buf();
    khal_i2c_read_reg_ReturnMemThruPtr_buf(hold, len);
}

static void expect_r8(uint8_t reg, uint8_t val)
{
    expect_read(reg, &val, 1U);
}

static void expect_w8(uint8_t reg, uint8_t val)
{
    khal_i2c_write_reg_ExpectAndReturn(NULL, reg, val, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
}

/**
 * @brief Bring the driver up once per binary, after the two things only a
 *        driver that is not up can show.
 *
 * Not static, for test_qmi8658_bring_up()'s reason exactly: the driver comes
 * up once, so "nothing is readable yet", "nothing ACKs at 0x2C", "something
 * else answers there" and every byte the first create() writes can be
 * asserted at one moment in the run, and the file that runs first is the one
 * that spends it. test_imu_ao.c calls this from there. Idempotent.
 */
void test_qmc5883p_bring_up(void)
{
    static bool s_up;
    uint8_t     id = 0U;
    int32_t     mg[3];

    if ( s_up )
    {
        return;
    }
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_qmc5883p_read(mg));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_qmc5883p_self_test(mg));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_qmc5883p_standby(false));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_qmc5883p_standby(true));

    /* Nothing at 0x2C. */
    khal_i2c_init_ExpectAndReturn(KODE_OK);
    khal_i2c_probe_ExpectAndReturn(0x2CU, 20U, false);
    TEST_ASSERT_EQUAL_INT(KODE_IO_ERROR, kdrv_qmc5883p_create(&id));

    /* Something that is not a QMC5883P: the QMC5883L's id register reads 0xFF. */
    khal_i2c_init_ExpectAndReturn(KODE_OK);
    khal_i2c_probe_ExpectAndReturn(0x2CU, 20U, true);
    khal_i2c_add_ExpectAnyArgsAndReturn(KODE_OK);
    khal_i2c_add_ReturnThruPtr_out(&s_handle);
    expect_r8(REG_CHIP_ID, 0xFFU);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_qmc5883p_create(&id));
    TEST_ASSERT_EQUAL_HEX8(0xFFU, id);

    /* The part: signs, range, then mode - the datasheet's own order. */
    khal_i2c_init_ExpectAndReturn(KODE_OK);
    khal_i2c_probe_ExpectAndReturn(0x2CU, 20U, true);
    expect_r8(REG_CHIP_ID, 0x80U);
    expect_w8(REG_SIGN, 0x06U);
    expect_w8(REG_CTRL2, 0x00U);   /* +-30 G, set and reset on          */
    expect_w8(REG_CTRL1, 0x00U);   /* suspended: the part is born at rest */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_qmc5883p_create(&id));
    TEST_ASSERT_EQUAL_HEX8(0x80U, id);
    s_up = true;
}

TEST_CASE("qmc5883p: absent, foreign, then the part and its configuration",
          "[kdrv_qmc5883p]")
{
    Mockkhal_i2c_Init();
    test_qmc5883p_bring_up();
    {
        uint8_t again = 0U;

        TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_qmc5883p_create(NULL));
        TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_qmc5883p_create(&again));
        TEST_ASSERT_EQUAL_HEX8(0x80U, again);   /* not zero once it is up */
    }
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("qmc5883p: the part found at 0x0C is told from anything else there",
          "[kdrv_qmc5883p]")
{
    uint8_t const other = 0x41U;

    Mockkhal_i2c_Init();

    /* Nothing at 0x0C. */
    khal_i2c_init_ExpectAndReturn(KODE_OK);
    khal_i2c_probe_ExpectAndReturn(0x0CU, 20U, false);
    TEST_ASSERT_FALSE(kdrv_qmc5883p_stray());

    /* Something at 0x0C that is not a QMC5883P. */
    khal_i2c_init_ExpectAndReturn(KODE_OK);
    khal_i2c_probe_ExpectAndReturn(0x0CU, 20U, true);
    khal_i2c_add_ExpectAnyArgsAndReturn(KODE_OK);
    khal_i2c_add_ReturnThruPtr_out(&s_handle);
    expect_r8(REG_CHIP_ID, other);
    khal_i2c_remove_ExpectAndReturn(s_handle, KODE_OK);
    TEST_ASSERT_FALSE(kdrv_qmc5883p_stray());

    /* The part, its chip id at the wrong address; the handle is given back. */
    khal_i2c_init_ExpectAndReturn(KODE_OK);
    khal_i2c_probe_ExpectAndReturn(0x0CU, 20U, true);
    khal_i2c_add_ExpectAnyArgsAndReturn(KODE_OK);
    khal_i2c_add_ReturnThruPtr_out(&s_handle);
    expect_r8(REG_CHIP_ID, 0x80U);
    khal_i2c_remove_ExpectAndReturn(s_handle, KODE_OK);
    TEST_ASSERT_TRUE(kdrv_qmc5883p_stray());

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("qmc5883p: leaving standby starts it converting, entering it "
          "suspends it", "[kdrv_qmc5883p]")
{
    Mockkhal_i2c_Init();
    test_qmc5883p_bring_up();

    expect_w8(REG_CTRL1, 0x07U);   /* continuous, 50 Hz, OSR1 8, OSR2 1 */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_qmc5883p_standby(false));
    expect_w8(REG_CTRL1, 0x00U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_qmc5883p_standby(true));

    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_CTRL1, 0x07U, KODE_IO_ERROR);
    khal_i2c_write_reg_IgnoreArg_dev();
    TEST_ASSERT_EQUAL_INT(KODE_IO_ERROR, kdrv_qmc5883p_standby(false));
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("qmc5883p: a ready conversion is three signed axes in milligauss",
          "[kdrv_qmc5883p]")
{
    int32_t mg[3] = { 0, 0, 0 };
    /* +1370, -250, +30000 LSB at 1000 LSB/G: one LSB is one milligauss. */
    uint8_t const raw[6] = { 0x5AU, 0x05U,  0x06U, 0xFFU,  0x30U, 0x75U };

    Mockkhal_i2c_Init();
    test_qmc5883p_bring_up();
    expect_r8(REG_STATUS, 0x01U);
    expect_read(REG_X_LSB, raw, sizeof(raw));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_qmc5883p_read(mg));
    TEST_ASSERT_EQUAL_INT32(1370, mg[0]);
    TEST_ASSERT_EQUAL_INT32(-250, mg[1]);
    TEST_ASSERT_EQUAL_INT32(30000, mg[2]);
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("qmc5883p: nothing ready is asked once more; an overflow is refused",
          "[kdrv_qmc5883p]")
{
    int32_t mg[3] = { 7, 7, 7 };
    uint8_t const raw[6] = { 0x01U, 0x00U,  0x02U, 0x00U,  0x03U, 0x00U };

    Mockkhal_i2c_Init();
    test_qmc5883p_bring_up();

    /* Early, not failed: a second look after one conversion period. */
    expect_r8(REG_STATUS, 0x00U);
    expect_r8(REG_STATUS, 0x01U);
    expect_read(REG_X_LSB, raw, sizeof(raw));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_qmc5883p_read(mg));
    TEST_ASSERT_EQUAL_INT32(1, mg[0]);

    /* Still nothing: said, and no data register is read. */
    expect_r8(REG_STATUS, 0x00U);
    expect_r8(REG_STATUS, 0x00U);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_qmc5883p_read(mg));

    /* Ready but overflowed: a clipped field is not a reading. */
    expect_r8(REG_STATUS, 0x03U);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_qmc5883p_read(mg));
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("qmc5883p: the self-test reports on minus off and leaves it converting",
          "[kdrv_qmc5883p]")
{
    int32_t delta[3] = { 0, 0, 0 };
    uint8_t const off[6] = { 0x64U, 0x00U,  0x9CU, 0xFFU,  0x00U, 0x00U };  /* 100 -100 0 */
    uint8_t const on[6]  = { 0x90U, 0x01U,  0x38U, 0xFFU,  0x2CU, 0x01U };  /* 400 -200 300 */

    Mockkhal_i2c_Init();
    test_qmc5883p_bring_up();
    expect_r8(REG_STATUS, 0x01U);
    expect_read(REG_X_LSB, off, sizeof(off));
    expect_w8(REG_CTRL2, 0x40U);           /* SELF_TEST, range kept at 30 G */
    expect_read(REG_X_LSB, on, sizeof(on));
    expect_w8(REG_CTRL1, 0x00U);           /* through suspend, as asked of  */
    expect_w8(REG_CTRL1, 0x07U);           /* every mode change             */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_qmc5883p_self_test(delta));
    TEST_ASSERT_EQUAL_INT32(-300, delta[0]);   /* x1 - x2: the datasheet's sign */
    TEST_ASSERT_EQUAL_INT32(-100, delta[1]);
    TEST_ASSERT_EQUAL_INT32(300, delta[2]);
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* ---- The calibration ------------------------------------------------------ */

#define CAL_AXES  3U     /**< Axes of the part.                               */
#define CAL_TOL   0.5F   /**< Float tolerance on a corrected field, mG.       */

/** @brief A calibration of the case's own numbers. */
static kdrv_qmc5883p_cal_t cal_make(int32_t ox, int32_t oy, int32_t oz,
                                    int32_t sx, int32_t sy, int32_t sz)
{
    kdrv_qmc5883p_cal_t c;

    c.offset_mg[0]   = ox;
    c.offset_mg[1]   = oy;
    c.offset_mg[2]   = oz;
    c.scale_milli[0] = sx;
    c.scale_milli[1] = sy;
    c.scale_milli[2] = sz;
    return c;
}

TEST_CASE("qmc5883p cal: the text round-trips and is the six numbers", "[kdrv_qmc5883p]")
{
    kdrv_qmc5883p_cal_t const in = cal_make(-1234, 0, 56, 1000, 1500, 250);
    kdrv_qmc5883p_cal_t       out;
    char                      text[KDRV_QMC5883P_CAL_TEXT_MAX + 1U];

    TEST_ASSERT_EQUAL_UINT32(24U, (uint32_t)kdrv_qmc5883p_cal_format(&in, text, sizeof(text)));
    TEST_ASSERT_EQUAL_STRING("-1234 0 56 1000 1500 250", text);
    TEST_ASSERT_TRUE(kdrv_qmc5883p_cal_parse(text, &out));
    TEST_ASSERT_EQUAL_INT32_ARRAY(in.offset_mg, out.offset_mg, CAL_AXES);
    TEST_ASSERT_EQUAL_INT32_ARRAY(in.scale_milli, out.scale_milli, CAL_AXES);
}

TEST_CASE("qmc5883p cal: the longest text fits the limit and no less room does", "[kdrv_qmc5883p]")
{
    kdrv_qmc5883p_cal_t const in = cal_make(-KDRV_QMC5883P_CAL_OFFSET_MAX_MG,
                                            -KDRV_QMC5883P_CAL_OFFSET_MAX_MG,
                                            -KDRV_QMC5883P_CAL_OFFSET_MAX_MG,
                                            KDRV_QMC5883P_CAL_SCALE_MAX,
                                            KDRV_QMC5883P_CAL_SCALE_MAX,
                                            KDRV_QMC5883P_CAL_SCALE_MAX);
    char text[KDRV_QMC5883P_CAL_TEXT_MAX + 1U];

    TEST_ASSERT_EQUAL_UINT32(KDRV_QMC5883P_CAL_TEXT_MAX,
                             (uint32_t)kdrv_qmc5883p_cal_format(&in, text, sizeof(text)));
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)kdrv_qmc5883p_cal_format(&in, text, sizeof(text) - 1U));
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)kdrv_qmc5883p_cal_format(NULL, text, sizeof(text)));
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)kdrv_qmc5883p_cal_format(&in, NULL, sizeof(text)));
}

TEST_CASE("qmc5883p cal: a number out of its limits is not formatted", "[kdrv_qmc5883p]")
{
    kdrv_qmc5883p_cal_t const big   = cal_make(KDRV_QMC5883P_CAL_OFFSET_MAX_MG + 1, 0, 0, 1000, 1000, 1000);
    kdrv_qmc5883p_cal_t const tiny = cal_make(0, 0, 0, 1000, KDRV_QMC5883P_CAL_SCALE_MIN - 1, 1000);
    char                      text[KDRV_QMC5883P_CAL_TEXT_MAX + 1U];

    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)kdrv_qmc5883p_cal_format(&big, text, sizeof(text)));
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)kdrv_qmc5883p_cal_format(&tiny, text, sizeof(text)));
}

TEST_CASE("qmc5883p cal: text that is not exactly six numbers is refused", "[kdrv_qmc5883p]")
{
    static const char *const bad[] = {
        "", "1 2 3", "1 2 3 1000 1000 1000 1000", "a b c d e f", "0 0 0 1000 1000  1000",
        " 0 0 0 1000 1000 1000", "0 0 0 1000 1000 1000 ", "0 0 0 1000 1000 1000\n",
        "+1 0 0 1000 1000 1000", "- 0 0 1000 1000 1000", "1-2 0 0 1000 1000 1000",
        "10000 0 0 1000 1000 1000", "-10000 0 0 1000 1000 1000",
        "0 0 0 249 1000 1000", "0 0 0 1000 4001 1000", "0 0 0 1000 1000 0",
        "0 0 0 1000 1000 -1000", "123456789012 0 0 1000 1000 1000",
    };
    kdrv_qmc5883p_cal_t corr = cal_make(7, 7, 7, 7, 7, 7);

    for ( uint32_t i = 0U; (sizeof(bad) / sizeof(bad[0])) > i; i++ )
    {
        TEST_ASSERT_FALSE_MESSAGE(kdrv_qmc5883p_cal_parse(bad[i], &corr), bad[i]);
    }
    TEST_ASSERT_FALSE(kdrv_qmc5883p_cal_parse(NULL, &corr));
    TEST_ASSERT_FALSE(kdrv_qmc5883p_cal_parse("0 0 0 1000 1000 1000", NULL));
    TEST_ASSERT_EQUAL_INT32_MESSAGE(7, corr.offset_mg[0], "a refusal leaves the calibration alone");
}

TEST_CASE("qmc5883p cal: apply takes the offset off and scales what is left", "[kdrv_qmc5883p]")
{
    kdrv_qmc5883p_cal_t const corr = cal_make(1000, -500, 100, 2000, 1000, 500);
    static const int32_t      raw[CAL_AXES] = { 1500, -200, 300 };
    float                     out[CAL_AXES];

    kdrv_qmc5883p_cal_apply(&corr, raw, out);
    TEST_ASSERT_FLOAT_WITHIN(CAL_TOL, 1000.0F, out[0]);
    TEST_ASSERT_FLOAT_WITHIN(CAL_TOL, 300.0F, out[1]);
    TEST_ASSERT_FLOAT_WITHIN(CAL_TOL, 100.0F, out[2]);
}

TEST_CASE("qmc5883p cal: a sweep gives its centre as the offset and evens the axes out",
          "[kdrv_qmc5883p]")
{
    static const int32_t lo[CAL_AXES] = { 2400, -2400, 200 };   /* centre 3000 -2000 500 */
    static const int32_t hi[CAL_AXES] = { 3600, -1600, 800 };   /* half 600 400 300      */
    kdrv_qmc5883p_cal_t  corr;
    float                out[CAL_AXES];

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_qmc5883p_cal_fit(lo, hi, &corr));
    TEST_ASSERT_EQUAL_INT32(3000, corr.offset_mg[0]);
    TEST_ASSERT_EQUAL_INT32(-2000, corr.offset_mg[1]);
    TEST_ASSERT_EQUAL_INT32(500, corr.offset_mg[2]);
    /* The mean half range is 433: each axis is scaled to it, rounded. */
    TEST_ASSERT_EQUAL_INT32(722, corr.scale_milli[0]);
    TEST_ASSERT_EQUAL_INT32(1083, corr.scale_milli[1]);
    TEST_ASSERT_EQUAL_INT32(1443, corr.scale_milli[2]);

    kdrv_qmc5883p_cal_apply(&corr, hi, out);
    for ( uint32_t i = 0U; CAL_AXES > i; i++ )
    {
        TEST_ASSERT_FLOAT_WITHIN(1.0F, 433.0F, out[i]);
    }
}

TEST_CASE("qmc5883p cal: a scale past its limits is clamped, an offset past its limit is refused",
          "[kdrv_qmc5883p]")
{
    static const int32_t lo[CAL_AXES] = { -100, -3000, -3000 };
    static const int32_t hi[CAL_AXES] = { 100, 3000, 3000 };
    static const int32_t far_lo[CAL_AXES] = { 19900, 0, 0 };
    static const int32_t far_hi[CAL_AXES] = { 20100, 400, 400 };
    kdrv_qmc5883p_cal_t  corr;

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_qmc5883p_cal_fit(lo, hi, &corr));
    TEST_ASSERT_EQUAL_INT32(KDRV_QMC5883P_CAL_SCALE_MAX, corr.scale_milli[0]);
    TEST_ASSERT_EQUAL_INT32(678, corr.scale_milli[1]);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_qmc5883p_cal_fit(far_lo, far_hi, &corr));
}

TEST_CASE("qmc5883p cal: an empty axis or a NULL is denied", "[kdrv_qmc5883p]")
{
    static const int32_t lo[CAL_AXES] = { 0, 0, 0 };
    static const int32_t flat[CAL_AXES] = { 100, 0, 100 };
    static const int32_t hi[CAL_AXES] = { 100, 100, 100 };
    kdrv_qmc5883p_cal_t  corr = cal_make(7, 7, 7, 7, 7, 7);

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_qmc5883p_cal_fit(lo, flat, &corr));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_qmc5883p_cal_fit(NULL, hi, &corr));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_qmc5883p_cal_fit(lo, NULL, &corr));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_qmc5883p_cal_fit(lo, hi, NULL));
    TEST_ASSERT_EQUAL_INT32_MESSAGE(7, corr.offset_mg[0], "a refusal leaves the calibration alone");
}
