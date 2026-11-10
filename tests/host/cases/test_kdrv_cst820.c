/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_cst820.c
 * @brief The CST820 driver, checked by what it says on the bus.
 */
#include <string.h>

#include "unity.h"

#include "Mockkhal_i2c.h"

#include "fakes.h"
#include "kbsp.h"
#include "kdrv_cst820.h"

/* The family's map, as the public register list prints it. */
#define REG_GESTURE      0x01U
#define REG_FINGERS      0x02U
#define REG_XH           0x03U   /* [7:6] event, [3:0] x high nibble */
#define REG_XL           0x04U
#define REG_YH           0x05U
#define REG_YL           0x06U
#define REG_CHIP_ID      0xA7U
#define REG_PROJ_ID      0xA8U
#define REG_FW_VER       0xA9U
#define REG_MOTION_MASK  0xECU
#define REG_IRQ_PULSE    0xEDU
#define REG_NOR_SCAN     0xEEU
#define REG_SL_ANGLE     0xEFU
#define REG_LP_RAW1_H    0xF0U
#define REG_LP_RAW2_H    0xF2U
#define REG_LP_WAKE      0xF4U
#define REG_LP_TH        0xF5U
#define REG_LP_WIN       0xF6U
#define REG_LP_FREQ      0xF7U
#define REG_LP_IDAC      0xF8U
#define REG_AUTO_SLEEP   0xF9U
#define REG_IRQ_CTL      0xFAU
#define REG_AUTO_RESET   0xFBU
#define REG_LONG_PRESS   0xFCU
#define REG_IO_CTL       0xFDU
#define REG_DIS_SLEEP    0xFEU

#define PROBE_MS         50U
#define LOOK_MS   20U   /**< One look's probe (kdrv_cst820_answers()). */
#define FRAME_LEN        6U      /* REG_GESTURE..REG_YL, one burst */
#define IDENTITY_LEN     3U      /* REG_CHIP_ID..REG_FW_VER, one burst */

/** @brief Any non-NULL handle; every use of it is mocked. */
static khal_i2c_dev_t s_handle = (khal_i2c_dev_t)(void *)&s_handle;

/** @brief Queue one mocked burst read of @p len bytes from @p reg. */
static void expect_rn(uint8_t reg, const uint8_t *bytes, size_t len, uint8_t *hold)
{
    (void)memcpy(hold, bytes, len);
    khal_i2c_read_reg_ExpectAndReturn(NULL, reg, NULL, len, KODE_OK);
    khal_i2c_read_reg_IgnoreArg_dev();
    khal_i2c_read_reg_IgnoreArg_buf();
    khal_i2c_read_reg_ReturnMemThruPtr_buf(hold, len);
}

/** @brief Queue one mocked register read returning @p val. */
static void expect_r8(uint8_t reg, uint8_t val, uint8_t *hold)
{
    expect_rn(reg, &val, 1U, hold);
}

/** @brief Refuse the next read of @p len bytes at @p reg, as a NAK would. */
static void expect_rn_fail(uint8_t reg, size_t len)
{
    khal_i2c_read_reg_ExpectAndReturn(NULL, reg, NULL, len, KODE_ERROR);
    khal_i2c_read_reg_IgnoreArg_dev();
    khal_i2c_read_reg_IgnoreArg_buf();
}

/** @brief Expect one register write of @p val to @p reg. */
static void expect_w8(uint8_t reg, uint8_t val)
{
    khal_i2c_write_reg_ExpectAndReturn(NULL, reg, val, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
}

/**
 * @brief The bus side of a bring-up: probe, attach, read the identity once.
 *
 * The first probe NAKs on purpose: a CST816 that has gone to sleep does not
 * answer until it wakes, and the driver's retry is what finds it.
 */
static void expect_bring_up(uint8_t chip, uint8_t proj, uint8_t fw, uint8_t hold[3])
{
    uint8_t const id[IDENTITY_LEN] = { chip, proj, fw };

    khal_i2c_init_ExpectAndReturn(KODE_OK);
    khal_i2c_probe_ExpectAndReturn(KBSP_I2C_ADDR_TOUCH, PROBE_MS, false);
    khal_i2c_probe_ExpectAndReturn(KBSP_I2C_ADDR_TOUCH, PROBE_MS, true);
    khal_i2c_add_ExpectAnyArgsAndReturn(KODE_OK);
    khal_i2c_add_ReturnThruPtr_out(&s_handle);
    expect_rn(REG_CHIP_ID, id, IDENTITY_LEN, hold);
}

/** @brief Bring the driver up once per binary, as a cold power-up would. */
static void bring_up(void)
{
    static bool    s_up;
    static uint8_t hold[3];

    if ( s_up || kdrv_cst820_present() )
    {
        return;
    }
    expect_bring_up(0xB7U, 0x01U, 0x03U, hold);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cst820_init());
    s_up = true;
}

/* ---- Bring-up ------------------------------------------------------------ */

/* First in the file, and it has to be: the only case that can see the driver
 * before it is up. Three probes, because a sleeping CST816 NAKs until it
 * wakes; three NAKs is a controller that is not there. */
TEST_CASE("a bus with nothing at 0x15 is no touch controller", "[kdrv_cst820]")
{
    uint8_t chip = 0U;
    uint8_t proj = 0U;
    uint8_t fw   = 0U;

    Mockkhal_i2c_Init();
    khal_i2c_init_ExpectAndReturn(KODE_OK);
    khal_i2c_probe_ExpectAndReturn(KBSP_I2C_ADDR_TOUCH, PROBE_MS, false);
    khal_i2c_probe_ExpectAndReturn(KBSP_I2C_ADDR_TOUCH, PROBE_MS, false);
    khal_i2c_probe_ExpectAndReturn(KBSP_I2C_ADDR_TOUCH, PROBE_MS, false);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_cst820_init());
    TEST_ASSERT_FALSE(kdrv_cst820_present());
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_cst820_identity(&chip, &proj, &fw));
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* What the owner asks when it looks again for a controller that was silent at
 * the bring-up: one short probe, and nothing brought up by it. */
TEST_CASE("a look is one short probe and brings nothing up", "[kdrv_cst820]")
{
    Mockkhal_i2c_Init();
    khal_i2c_init_ExpectAndReturn(KODE_OK);
    khal_i2c_probe_ExpectAndReturn(KBSP_I2C_ADDR_TOUCH, LOOK_MS, false);
    TEST_ASSERT_FALSE(kdrv_cst820_answers());

    khal_i2c_init_ExpectAndReturn(KODE_OK);
    khal_i2c_probe_ExpectAndReturn(KBSP_I2C_ADDR_TOUCH, LOOK_MS, true);
    TEST_ASSERT_TRUE(kdrv_cst820_answers());
    TEST_ASSERT_FALSE(kdrv_cst820_present());
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* An ACK says something is at 0x15, not what. A part outside the family
 * would have its register 0x02 read as a finger count - so it is refused on
 * its chip_id, and the device it was attached as goes back. */
TEST_CASE("a part at 0x15 whose chip_id is not a CST8xx is refused", "[kdrv_cst820]")
{
    uint8_t hold[IDENTITY_LEN];

    uint8_t const id[IDENTITY_LEN] = { 0x5AU, 0x01U, 0x03U };

    Mockkhal_i2c_Init();
    expect_bring_up(0x5AU, 0x01U, 0x03U, hold);
    expect_rn(REG_CHIP_ID, id, IDENTITY_LEN, hold);
    expect_rn(REG_CHIP_ID, id, IDENTITY_LEN, hold);
    khal_i2c_remove_ExpectAndReturn(s_handle, KODE_OK);
    fake_klog_reset();
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_cst820_init());
    TEST_ASSERT_FALSE(kdrv_cst820_present());
    unsigned errors = 0U;   /* only the last of the three answers is an error */
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        if ( NULL != strstr(fake_klog_line(i), "not a CST8xx") )
        {
            ++errors;
        }
    }
    TEST_ASSERT_EQUAL_UINT(1U, errors);
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* The chip ACKed and went back to sleep before the identity read: nothing is
 * known about it, so it is not up, and the retry at the next display-up adds
 * one device rather than a second. */
TEST_CASE("an identity that would not read leaves the controller down and hands its device back",
          "[kdrv_cst820]")
{
    Mockkhal_i2c_Init();
    khal_i2c_init_ExpectAndReturn(KODE_OK);
    khal_i2c_probe_ExpectAndReturn(KBSP_I2C_ADDR_TOUCH, PROBE_MS, true);
    khal_i2c_add_ExpectAnyArgsAndReturn(KODE_OK);
    khal_i2c_add_ReturnThruPtr_out(&s_handle);
    expect_rn_fail(REG_CHIP_ID, IDENTITY_LEN);
    khal_i2c_remove_ExpectAndReturn(s_handle, KODE_OK);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_cst820_init());
    TEST_ASSERT_FALSE(kdrv_cst820_present());
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* A controller that ACKs while it wakes can answer 0x00 once. That read is
 * repeated, comes out as a CST8xx and logs no error. */
TEST_CASE("a chip_id that reads 0x00 once is read again and the part is found",
          "[kdrv_cst820]")
{
    uint8_t const zero[IDENTITY_LEN] = { 0x00U, 0x00U, 0x00U };
    uint8_t const good[IDENTITY_LEN] = { 0xB7U, 0x01U, 0x03U };
    uint8_t       hold[IDENTITY_LEN];
    uint8_t       hold2[IDENTITY_LEN];

    Mockkhal_i2c_Init();
    khal_i2c_init_ExpectAndReturn(KODE_OK);
    khal_i2c_probe_ExpectAndReturn(KBSP_I2C_ADDR_TOUCH, PROBE_MS, true);
    khal_i2c_add_ExpectAnyArgsAndReturn(KODE_OK);
    khal_i2c_add_ReturnThruPtr_out(&s_handle);
    expect_rn(REG_CHIP_ID, zero, IDENTITY_LEN, hold);
    expect_rn(REG_CHIP_ID, good, IDENTITY_LEN, hold2);
    fake_klog_reset();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cst820_init());
    TEST_ASSERT_TRUE(kdrv_cst820_present());
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        TEST_ASSERT_NULL_MESSAGE(strstr(fake_klog_line(i), "not a CST8xx"),
                                 "a read that was repeated must not be an error");
    }
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* The identity is read at bring-up and never again: right after the panel's
 * reset the chip is awake, and a minute later it may be asleep and NAK. So
 * the three bytes are cached, and asking for them costs no bus at all. */
TEST_CASE("a controller that NAKs once while waking is found, and its identity is read once",
          "[kdrv_cst820]")
{
    uint8_t chip = 0U;
    uint8_t proj = 0U;
    uint8_t fw   = 0U;

    Mockkhal_i2c_Init();
    bring_up();
    TEST_ASSERT_TRUE(kdrv_cst820_present());

    /* No bus traffic is expected for these. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cst820_identity(&chip, &proj, &fw));
    TEST_ASSERT_EQUAL_UINT8(0xB7U, chip);
    TEST_ASSERT_EQUAL_UINT8(0x01U, proj);
    TEST_ASSERT_EQUAL_UINT8(0x03U, fw);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_cst820_identity(NULL, &proj, &fw));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* ---- The polled frame ---------------------------------------------------- */

/* One burst of six bytes from 0x01 is the whole frame: gesture, the finger
 * count, and x and y as a high nibble and a low byte each. The top two bits
 * of the x high byte are the event flag and must not leak into x. */
TEST_CASE("a frame with one finger decodes pressed and the twelve-bit x and y",
          "[kdrv_cst820]")
{
    uint8_t const frame[FRAME_LEN] = {
        0x00U,          /* gesture: none */
        0x01U,          /* one finger */
        0x81U, 0x2CU,   /* event=2 (contact) in [7:6]; x = 0x12C = 300 */
        0x01U, 0x90U,   /* y = 0x190 = 400 */
    };
    uint8_t hold[FRAME_LEN];
    bool    pressed = false;
    int16_t x = -1;
    int16_t y = -1;

    Mockkhal_i2c_Init();
    bring_up();

    expect_rn(REG_GESTURE, frame, FRAME_LEN, hold);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cst820_read(&pressed, &x, &y));
    TEST_ASSERT_TRUE(pressed);
    TEST_ASSERT_EQUAL_INT16(300, x);
    TEST_ASSERT_EQUAL_INT16(400, y);

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* No finger is a release, and the coordinates the caller had are left alone:
 * the last position is the caller's business, and the frame's stale x/y are
 * not a place anybody touched. */
TEST_CASE("no finger reads as released and leaves the coordinates alone",
          "[kdrv_cst820]")
{
    uint8_t const frame[FRAME_LEN] = { 0x00U, 0x00U, 0x40U, 0x2CU, 0x01U, 0x90U };
    uint8_t hold[FRAME_LEN];
    bool    pressed = true;
    int16_t x = 7;
    int16_t y = 9;

    Mockkhal_i2c_Init();
    bring_up();

    expect_rn(REG_GESTURE, frame, FRAME_LEN, hold);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cst820_read(&pressed, &x, &y));
    TEST_ASSERT_FALSE(pressed);
    TEST_ASSERT_EQUAL_INT16(7, x);
    TEST_ASSERT_EQUAL_INT16(9, y);

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* A NAK is not a release. Decoding "the chip did not answer" as "the finger
 * lifted" would publish a lift nobody made; the caller decides what a failed
 * read means, and nothing it holds is touched. Said once on the way down and
 * once on the way back, at KODE_TOUCH_POLL_MS a line per failure would bury
 * the console it is trying to warn. */
TEST_CASE("a frame the controller did not answer is an error, said once",
          "[kdrv_cst820]")
{
    uint8_t const frame[FRAME_LEN] = { 0U, 0U, 0U, 0U, 0U, 0U };
    uint8_t hold[FRAME_LEN];
    bool    pressed = true;
    int16_t x = 7;
    int16_t y = 9;

    Mockkhal_i2c_Init();
    bring_up();

    unsigned const quiet = fake_klog_count();
    expect_rn_fail(REG_GESTURE, FRAME_LEN);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_cst820_read(&pressed, &x, &y));
    expect_rn_fail(REG_GESTURE, FRAME_LEN);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_cst820_read(&pressed, &x, &y));
    TEST_ASSERT_TRUE(pressed);
    TEST_ASSERT_EQUAL_INT16(7, x);
    TEST_ASSERT_EQUAL_INT16(9, y);
    TEST_ASSERT_EQUAL_UINT(quiet + 1U, fake_klog_count());

    expect_rn(REG_GESTURE, frame, FRAME_LEN, hold);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cst820_read(&pressed, &x, &y));
    TEST_ASSERT_EQUAL_UINT(quiet + 2U, fake_klog_count());

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* ---- The table: reading ---------------------------------------------------- */

/** @brief Read @p f after @p reg answers @p raw. */
static int32_t get_after(kdrv_cst820_field_t f, uint8_t reg, uint8_t raw)
{
    uint8_t hold;
    int32_t v = -1;

    expect_r8(reg, raw, &hold);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cst820_get(f, &v));
    return v;
}

/* The frame's registers are fields too, so the console can look at the
 * gesture engine and the event flag one at a time. x is a nibble and a
 * byte, read as one burst so the two halves belong to the same sample. */
TEST_CASE("the frame's fields decode whole, by nibble pair and by event flag",
          "[kdrv_cst820]")
{
    uint8_t const xy[2] = { 0x81U, 0x2CU };
    uint8_t hold[2];
    int32_t v = -1;

    Mockkhal_i2c_Init();
    bring_up();

    TEST_ASSERT_EQUAL_INT32(5, get_after(KDRV_CST_GESTURE, REG_GESTURE, 0x05U));
    TEST_ASSERT_EQUAL_INT32(1, get_after(KDRV_CST_FINGERS, REG_FINGERS, 0x01U));
    TEST_ASSERT_EQUAL_INT32(2, get_after(KDRV_CST_EVENT,   REG_XH,      0x81U));

    expect_rn(REG_XH, xy, 2U, hold);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cst820_get(KDRV_CST_X, &v));
    TEST_ASSERT_EQUAL_INT32(300, v);

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* The identity is the bring-up cache, so it answers with the chip asleep:
 * no bus transaction is expected for any of the three. */
TEST_CASE("the identity fields come from the cache, not the bus", "[kdrv_cst820]")
{
    int32_t v = -1;

    Mockkhal_i2c_Init();
    bring_up();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cst820_get(KDRV_CST_CHIP_ID, &v));
    TEST_ASSERT_EQUAL_INT32(0xB7, v);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cst820_get(KDRV_CST_PROJ_ID, &v));
    TEST_ASSERT_EQUAL_INT32(0x01, v);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cst820_get(KDRV_CST_FW_VER, &v));
    TEST_ASSERT_EQUAL_INT32(0x03, v);

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("a configuration register decodes whole, by bit and as sixteen bits",
          "[kdrv_cst820]")
{
    uint8_t const raw1[2] = { 0x12U, 0x34U };
    uint8_t hold[2];
    int32_t v = -1;

    Mockkhal_i2c_Init();
    bring_up();

    TEST_ASSERT_EQUAL_INT32(0x60, get_after(KDRV_CST_IRQ_CTL,   REG_IRQ_CTL, 0x60U));
    TEST_ASSERT_EQUAL_INT32(1,    get_after(KDRV_CST_EN_TOUCH,  REG_IRQ_CTL, 0x60U));
    TEST_ASSERT_EQUAL_INT32(0,    get_after(KDRV_CST_EN_MOTION, REG_IRQ_CTL, 0x60U));
    TEST_ASSERT_EQUAL_INT32(2,    get_after(KDRV_CST_AUTO_SLEEP_TIME, REG_AUTO_SLEEP, 0x02U));
    TEST_ASSERT_EQUAL_INT32(1,    get_after(KDRV_CST_IIC_OD,    REG_IO_CTL, 0x02U));

    expect_rn(REG_LP_RAW1_H, raw1, 2U, hold);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cst820_get(KDRV_CST_LP_SCAN_RAW1, &v));
    TEST_ASSERT_EQUAL_INT32(0x1234, v);

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* A NAK leaves the caller's variable alone: a sleeping chip must not be
 * able to impersonate a setting. */
TEST_CASE("a field the chip did not answer is an error and leaves the value alone",
          "[kdrv_cst820]")
{
    int32_t v = -1;

    Mockkhal_i2c_Init();
    bring_up();

    expect_rn_fail(REG_AUTO_SLEEP, 1U);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_cst820_get(KDRV_CST_AUTO_SLEEP_TIME, &v));
    TEST_ASSERT_EQUAL_INT32(-1, v);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_cst820_get(KDRV_CST_AUTO_SLEEP_TIME, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_cst820_get(KDRV_CST_FIELD_MAX, &v));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* ---- The table: writing ---------------------------------------------------- */

/* Enabling one interrupt source must not touch the others: a masked
 * read-modify-write, checked on this field's bits alone. */
TEST_CASE("writing one bit is a masked read-modify-write checked on that bit alone",
          "[kdrv_cst820]")
{
    uint8_t before;
    uint8_t after;

    Mockkhal_i2c_Init();
    bring_up();

    expect_r8(REG_IRQ_CTL, 0x60U, &before);
    expect_w8(REG_IRQ_CTL, 0x70U);
    expect_r8(REG_IRQ_CTL, 0x71U, &after);   /* bit 0 moved: not our business */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cst820_set(KDRV_CST_EN_MOTION, 1));

    expect_r8(REG_AUTO_SLEEP, 0x02U, &before);
    expect_w8(REG_AUTO_SLEEP, 0x05U);
    expect_r8(REG_AUTO_SLEEP, 0x05U, &after);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cst820_set(KDRV_CST_AUTO_SLEEP_TIME, 5));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("a write the chip did not keep is an error, not a setting",
          "[kdrv_cst820]")
{
    uint8_t before;
    uint8_t after;

    Mockkhal_i2c_Init();
    bring_up();

    expect_r8(REG_DIS_SLEEP, 0x00U, &before);
    expect_w8(REG_DIS_SLEEP, 0x01U);
    expect_r8(REG_DIS_SLEEP, 0x00U, &after);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_cst820_set(KDRV_CST_DIS_AUTO_SLEEP, 1));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* Refused before the bus is touched: the mock would fail on any transaction. */
TEST_CASE("the frame and the identity are read-only, and a value the field cannot hold is refused",
          "[kdrv_cst820]")
{
    Mockkhal_i2c_Init();
    bring_up();

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_cst820_set(KDRV_CST_GESTURE, 0));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_cst820_set(KDRV_CST_X, 0));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_cst820_set(KDRV_CST_EVENT, 0));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_cst820_set(KDRV_CST_CHIP_ID, 0));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_cst820_set(KDRV_CST_LP_SCAN_RAW1, 0));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_cst820_set(KDRV_CST_EN_TEST, 2));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_cst820_set(KDRV_CST_AUTO_SLEEP_TIME, 256));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_cst820_set(KDRV_CST_AUTO_SLEEP_TIME, -1));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_cst820_set(KDRV_CST_FIELD_MAX, 0));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* ---- The table itself ------------------------------------------------------ */

TEST_CASE("the table names the frame, the identity and every configuration register",
          "[kdrv_cst820]")
{
    TEST_ASSERT_EQUAL_INT(36, (int)KDRV_CST_FIELD_MAX);

    const kdrv_cst820_field_info_t *x = kdrv_cst820_field_info(KDRV_CST_X);
    TEST_ASSERT_NOT_NULL(x);
    TEST_ASSERT_EQUAL_STRING("x", x->name);
    TEST_ASSERT_EQUAL_STRING("px", x->unit);
    TEST_ASSERT_FALSE(x->writable);
    TEST_ASSERT_EQUAL_INT32(0,    x->min);
    TEST_ASSERT_EQUAL_INT32(4095, x->max);

    const kdrv_cst820_field_info_t *slp = kdrv_cst820_field_info(KDRV_CST_AUTO_SLEEP_TIME);
    TEST_ASSERT_EQUAL_STRING("auto_sleep_time", slp->name);
    TEST_ASSERT_EQUAL_STRING("s", slp->unit);
    TEST_ASSERT_TRUE(slp->writable);
    TEST_ASSERT_EQUAL_INT32(255, slp->max);

    const kdrv_cst820_field_info_t *bit = kdrv_cst820_field_info(KDRV_CST_EN_TOUCH);
    TEST_ASSERT_EQUAL_STRING("en_touch", bit->name);
    TEST_ASSERT_EQUAL_STRING("bool", bit->unit);
    TEST_ASSERT_EQUAL_INT32(1, bit->max);

    TEST_ASSERT_EQUAL_STRING("chip_id", kdrv_cst820_field_info(KDRV_CST_CHIP_ID)->name);
    TEST_ASSERT_EQUAL_STRING("lp_scan_raw2", kdrv_cst820_field_info(KDRV_CST_LP_SCAN_RAW2)->name);
    TEST_ASSERT_EQUAL_INT32(65535, kdrv_cst820_field_info(KDRV_CST_LP_SCAN_RAW2)->max);
    TEST_ASSERT_EQUAL_STRING("dis_auto_sleep", kdrv_cst820_field_info(KDRV_CST_DIS_AUTO_SLEEP)->name);
    TEST_ASSERT_NULL(kdrv_cst820_field_info(KDRV_CST_FIELD_MAX));
}

TEST_CASE("every field is found by its name, and an unknown name is refused",
          "[kdrv_cst820]")
{
    for ( int f = 0; (int)KDRV_CST_FIELD_MAX > f; ++f )
    {
        kdrv_cst820_field_t got = KDRV_CST_FIELD_MAX;
        const char *name = kdrv_cst820_field_info((kdrv_cst820_field_t)f)->name;
        TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cst820_field_by_name(name, &got));
        TEST_ASSERT_EQUAL_INT(f, (int)got);
    }
    kdrv_cst820_field_t got;
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_cst820_field_by_name("ichg_ma", &got));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_cst820_field_by_name(NULL, &got));
}

/* ---- Dump ------------------------------------------------------------------ */

/* Two bursts: the frame, and the whole configuration block 0xEC..0xFE in
 * one transaction. The identity comes from the cache. */
TEST_CASE("a dump logs the identity, the frame and the configuration block",
          "[kdrv_cst820]")
{
    uint8_t const frame[FRAME_LEN] = { 0U, 0U, 0U, 0U, 0U, 0U };
    uint8_t const cfg[19] = { 0 };
    uint8_t hold_frame[FRAME_LEN];
    uint8_t hold_cfg[19];

    Mockkhal_i2c_Init();
    bring_up();

    unsigned const before = fake_klog_count();
    expect_rn(REG_GESTURE, frame, FRAME_LEN, hold_frame);
    expect_rn(REG_MOTION_MASK, cfg, 19U, hold_cfg);
    kdrv_cst820_dump();
    TEST_ASSERT_TRUE(fake_klog_count() > before + 1U);

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* A sleeping chip is one line, not a line per register - and the identity
 * still prints, because it never needed the bus. */
TEST_CASE("a dump of a sleeping chip says so once", "[kdrv_cst820]")
{
    Mockkhal_i2c_Init();
    bring_up();

    unsigned const before = fake_klog_count();
    expect_rn_fail(REG_GESTURE, FRAME_LEN);
    kdrv_cst820_dump();
    TEST_ASSERT_TRUE(fake_klog_count() > before);
    TEST_ASSERT_TRUE(fake_klog_count() <= before + 3U);

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}
