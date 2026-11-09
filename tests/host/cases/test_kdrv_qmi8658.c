/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_qmi8658.c
 * @brief kdrv_qmi8658 on a mocked bus: identity, configuration, sample scales and self-test.
 */
#include <string.h>

#include "unity.h"

#include "Mockkhal_i2c.h"

#include "kbsp.h"
#include "kdrv_qmi8658.h"

#define REG_WHO_AM_I   0x00U
#define REG_CTRL1      0x02U
#define REG_CTRL2      0x03U
#define REG_CTRL3      0x04U
#define REG_CTRL5      0x06U
#define REG_CTRL7      0x08U
#define REG_CTRL8      0x09U
#define REG_CTRL9      0x0AU
#define REG_FIFO_CTRL  0x14U
#define REG_FIFO_CNT   0x15U
#define REG_FIFO_DATA  0x17U
#define REG_STATUSINT  0x2DU
#define REG_TEMP_L     0x33U
#define REG_DVX_L      0x51U

/** @brief Reads STATUSINT is given before a CTRL9 command is called lost:
 *         one, then one a millisecond for the driver's whole 50 ms window. */
#define CTRL9_READS    51U

/** @brief Any non-NULL handle; every use of it is mocked. */
static khal_i2c_dev_t s_handle = (khal_i2c_dev_t)(void *)&s_handle;

/** @brief Read buffers the mock copies out of; they must outlive the call. */
static uint8_t s_hold[16][16];
static size_t  s_hold_n;

/** @brief STATUSINT as a CTRL9 wait reads it, bit 7 low and high. Storage of
 *         their own rather than the ring: a whole wait queues more reads
 *         than the ring has slots, and a later one would overwrite them. */
static uint8_t s_cmd_busy = 0x00U;
static uint8_t s_cmd_done = 0x80U;

/**
 * @brief Queue one mocked read of @p len bytes at @p reg, copied out of @p src
 *        when the driver makes the call - so @p src must outlive it.
 */
static void expect_read_at(uint8_t reg, uint8_t *src, size_t len)
{
    khal_i2c_read_reg_ExpectAndReturn(NULL, reg, NULL, len, KODE_OK);
    khal_i2c_read_reg_IgnoreArg_dev();
    khal_i2c_read_reg_IgnoreArg_buf();
    khal_i2c_read_reg_ReturnMemThruPtr_buf(src, len);
}

/** @brief Queue one mocked read of @p len bytes at @p reg returning @p bytes. */
static void expect_read(uint8_t reg, const uint8_t *bytes, size_t len)
{
    uint8_t * const hold = s_hold[s_hold_n % 16U];

    s_hold_n += 1U;
    (void)memcpy(hold, bytes, len);
    expect_read_at(reg, hold, len);
}

/** @brief Queue one mocked one-byte read. */
static void expect_r8(uint8_t reg, uint8_t val)
{
    expect_read(reg, &val, 1U);
}

/** @brief Expect one register write. */
static void expect_w8(uint8_t reg, uint8_t val)
{
    khal_i2c_write_reg_ExpectAndReturn(NULL, reg, val, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
}

/** @brief Expect a write that the driver reads back. */
static void expect_w8_checked(uint8_t reg, uint8_t val)
{
    expect_w8(reg, val);
    expect_r8(reg, val);
}

/** @brief The running configuration, as create() and st_end() both write it. */
static void expect_scales_and_enable(void)
{
    expect_w8_checked(REG_CTRL2, 0x23U);   /* +-8 g, 896.8 Hz   */
    expect_w8_checked(REG_CTRL3, 0x53U);   /* +-512 dps, 896.8 Hz */
}

/**
 * @brief Bring the driver up once per binary - after the three things only a
 *        driver that is not up can show.
 *
 * Not static, and called from test_imu_ao.c as well as from every case here.
 * The driver brings itself up exactly once: after a create() has succeeded a
 * further one returns at the door, so "nothing is readable yet", "nothing
 * ACKs" and "something else answers" can be asserted at one moment in the
 * whole run, and so can every byte the first create() writes. That moment
 * belongs to whichever case file runs first, and cases run in the order the
 * linker lays their constructors down - which is the sorted case list, where
 * `test_imu_ao.c` comes before this file. So that file calls this, and the
 * assertions stay here, where the part they are about lives. Idempotent.
 */
void test_qmi8658_bring_up(void)
{
    static bool           s_up;
    kdrv_qmi8658_id_t     id = { 0U, 0U };
    kdrv_qmi8658_sample_t s;
    uint8_t const         foreign[2] = { 0x70U, 0x00U };  /* an LSM6DSV16X */
    uint8_t const         ours[2]    = { 0x05U, 0x7CU };

    if ( s_up )
    {
        return;
    }

    /* Nothing is readable before bring-up. */
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_qmi8658_read(&s));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_qmi8658_st_begin(KDRV_QMI8658_XL));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_qmi8658_st_end());
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_qmi8658_standby(false));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_qmi8658_standby(true));

    /* Nothing ACKs at 0x6A: absent, and said as an I/O error. */
    khal_i2c_init_ExpectAndReturn(KODE_OK);
    khal_i2c_probe_ExpectAndReturn(KBSP_I2C_ADDR_IMU, 20U, false);
    TEST_ASSERT_EQUAL_INT(KODE_IO_ERROR, kdrv_qmi8658_create(&id));

    /* Something else answers there - another IMU at the same address. */
    khal_i2c_init_ExpectAndReturn(KODE_OK);
    khal_i2c_probe_ExpectAndReturn(KBSP_I2C_ADDR_IMU, 20U, true);
    khal_i2c_add_ExpectAnyArgsAndReturn(KODE_OK);
    khal_i2c_add_ReturnThruPtr_out(&s_handle);
    expect_read(REG_WHO_AM_I, foreign, 2U);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_qmi8658_create(&id));
    TEST_ASSERT_EQUAL_HEX8(0x70U, id.whoami);

    /* The part itself. The device is already attached, so no second add. */
    khal_i2c_init_ExpectAndReturn(KODE_OK);
    khal_i2c_probe_ExpectAndReturn(KBSP_I2C_ADDR_IMU, 20U, true);
    expect_read(REG_WHO_AM_I, ours, 2U);
    expect_w8(REG_CTRL7, 0x00U);           /* sensors off, and left off: the
                                            * part is born at rest            */
    /* Auto-increment on, LITTLE-endian (the reset value is big), INT1 driven -
     * on DVT that pin is the expander's P5 and has no pull-up - and INT2
     * driven too, for the self-test flag that is said to mirror it. */
    expect_w8_checked(REG_CTRL1, 0x58U);
    expect_scales_and_enable();
    expect_w8_checked(REG_CTRL5, 0x00U);
    /* CTRL8 bit 7 = 1: "use STATUSINT.bit7 as CTRL9 handshake" (Table 22).
     * Zero would leave the handshake on INT1, which on DVT is the expander's
     * P5 - an interrupt to AO_IoExp for every FIFO command. */
    expect_w8_checked(REG_CTRL8, 0x80U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_qmi8658_create(&id));
    TEST_ASSERT_EQUAL_HEX8(0x05U, id.whoami);
    TEST_ASSERT_EQUAL_HEX8(0x7CU, id.revision);
    s_up = true;
}

TEST_CASE("qmi8658: absent, foreign, then the part and its configuration",
          "[kdrv_qmi8658]")
{
    Mockkhal_i2c_Init();
    test_qmi8658_bring_up();
    /* Up already: a further create touches nothing, and still says who the
     * part is - the first DVT board printed whoami=0x00 for a part that had
     * identified itself at boot. */
    {
        kdrv_qmi8658_id_t again = { 0U, 0U };

        TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_qmi8658_create(NULL));
        TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_qmi8658_create(&again));
        TEST_ASSERT_EQUAL_HEX8(0x05U, again.whoami);
        TEST_ASSERT_EQUAL_HEX8(0x7CU, again.revision);
    }
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("qmi8658: leaving standby enables both sensors, entering it "
          "disables both", "[kdrv_qmi8658]")
{
    Mockkhal_i2c_Init();
    test_qmi8658_bring_up();

    expect_w8_checked(REG_CTRL7, 0x03U);   /* aEN | gEN */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_qmi8658_standby(false));
    expect_w8_checked(REG_CTRL7, 0x00U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_qmi8658_standby(true));

    /* A part that does not hold the write is said, not believed. */
    expect_w8(REG_CTRL7, 0x03U);
    expect_r8(REG_CTRL7, 0x00U);
    TEST_ASSERT_EQUAL_INT(KODE_IO_ERROR, kdrv_qmi8658_standby(false));
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("qmi8658: one burst is a sample in mg, mdps and tenths of a degree",
          "[kdrv_qmi8658]")
{
    kdrv_qmi8658_sample_t s;
    /* TEMP 26.5 C = 0x1A80; a = +1 g, -0.5 g, 0 at 4096 LSB/g;
     * w = +1 dps, -10 dps, +full scale at 64 LSB/dps. All little-endian. */
    uint8_t const raw[14] = {
        0x80U, 0x1AU,
        0x00U, 0x10U,  0x00U, 0xF8U,  0x00U, 0x00U,
        0x40U, 0x00U,  0x80U, 0xFDU,  0xFFU, 0x7FU,
    };

    Mockkhal_i2c_Init();
    test_qmi8658_bring_up();
    expect_read(REG_TEMP_L, raw, sizeof(raw));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_qmi8658_read(&s));
    TEST_ASSERT_EQUAL_INT32(265, s.temp_dc);
    TEST_ASSERT_EQUAL_INT32(1000, s.a_mg[0]);
    TEST_ASSERT_EQUAL_INT32(-500, s.a_mg[1]);
    TEST_ASSERT_EQUAL_INT32(0, s.a_mg[2]);
    TEST_ASSERT_EQUAL_INT32(1000, s.w_mdps[0]);
    TEST_ASSERT_EQUAL_INT32(-10000, s.w_mdps[1]);
    TEST_ASSERT_EQUAL_INT32(511984, s.w_mdps[2]);   /* 32767 / 64 dps */
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_qmi8658_read(NULL));
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("qmi8658: the accelerometer's self-test follows the datasheet",
          "[kdrv_qmi8658]")
{
    bool    done = true;
    int32_t shift[3] = { 0, 0, 0 };
    /* U5.11 of a g: 2048 = 1000 mg, -1024 = -500 mg, 410 = 200 mg. */
    uint8_t const dv[6] = { 0x00U, 0x08U,  0x00U, 0xFCU,  0x9AU, 0x01U };

    Mockkhal_i2c_Init();
    test_qmi8658_bring_up();

    /* 1- sensors off; 2- the rate kept and aST set. */
    expect_w8(REG_CTRL7, 0x00U);
    expect_w8(REG_CTRL2, 0xA3U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_qmi8658_st_begin(KDRV_QMI8658_XL));

    /* 3- still working: one read, and nothing else touched. */
    expect_r8(REG_STATUSINT, 0x00U);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_qmi8658_st_poll(KDRV_QMI8658_XL, &done, shift));
    TEST_ASSERT_FALSE(done);

    /* 3..6- done: aST cleared, the flag seen to fall, the result read. */
    expect_r8(REG_STATUSINT, 0x01U);
    expect_w8(REG_CTRL2, 0x23U);
    expect_r8(REG_STATUSINT, 0x01U);
    expect_r8(REG_STATUSINT, 0x00U);
    expect_read(REG_DVX_L, dv, sizeof(dv));
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_qmi8658_st_poll(KDRV_QMI8658_XL, &done, shift));
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQUAL_INT32(1000, shift[0]);
    TEST_ASSERT_EQUAL_INT32(-500, shift[1]);
    TEST_ASSERT_EQUAL_INT32(200, shift[2]);

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("qmi8658: the gyroscope's self-test uses CTRL3 and sixteenths of a dps",
          "[kdrv_qmi8658]")
{
    bool    done = false;
    int32_t shift[3] = { 0, 0, 0 };
    /* U12.4 of a dps: 4800 = 300 dps, -8000 = -500 dps, 16 = 1 dps. */
    uint8_t const dv[6] = { 0xC0U, 0x12U,  0xC0U, 0xE0U,  0x10U, 0x00U };

    Mockkhal_i2c_Init();
    test_qmi8658_bring_up();
    expect_w8(REG_CTRL7, 0x00U);
    expect_w8(REG_CTRL3, 0xD3U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_qmi8658_st_begin(KDRV_QMI8658_GY));

    expect_r8(REG_STATUSINT, 0x01U);
    expect_w8(REG_CTRL3, 0x53U);
    expect_r8(REG_STATUSINT, 0x00U);
    expect_read(REG_DVX_L, dv, sizeof(dv));
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kdrv_qmi8658_st_poll(KDRV_QMI8658_GY, &done, shift));
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQUAL_INT32(300000, shift[0]);
    TEST_ASSERT_EQUAL_INT32(-500000, shift[1]);
    TEST_ASSERT_EQUAL_INT32(1000, shift[2]);
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("qmi8658: the verdict is the datasheet's 200 mg and 300 dps, per axis",
          "[kdrv_qmi8658]")
{
    int32_t const xl_ok[3]  = { 200, -200, 1500 };
    int32_t const xl_bad[3] = { 900, 199, 900 };
    int32_t const gy_ok[3]  = { 300000, -300000, 450000 };
    int32_t const gy_bad[3] = { 450000, 450000, -299999 };

    TEST_ASSERT_TRUE(kdrv_qmi8658_st_judge(KDRV_QMI8658_XL, xl_ok));
    TEST_ASSERT_FALSE(kdrv_qmi8658_st_judge(KDRV_QMI8658_XL, xl_bad));
    TEST_ASSERT_TRUE(kdrv_qmi8658_st_judge(KDRV_QMI8658_GY, gy_ok));
    TEST_ASSERT_FALSE(kdrv_qmi8658_st_judge(KDRV_QMI8658_GY, gy_bad));
    /* The accelerometer's numbers are not a pass for the gyroscope. */
    TEST_ASSERT_FALSE(kdrv_qmi8658_st_judge(KDRV_QMI8658_GY, xl_ok));
    TEST_ASSERT_FALSE(kdrv_qmi8658_st_judge(KDRV_QMI8658_XL, NULL));
}

TEST_CASE("qmi8658: leaving self-test rewrites the scales and turns both on",
          "[kdrv_qmi8658]")
{
    Mockkhal_i2c_Init();
    test_qmi8658_bring_up();
    expect_w8(REG_CTRL7, 0x00U);
    expect_scales_and_enable();
    expect_w8_checked(REG_CTRL7, 0x03U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_qmi8658_st_end());

    /* A part that does not hold what it was written is an I/O error. */
    expect_w8(REG_CTRL7, 0x00U);
    expect_w8(REG_CTRL2, 0x23U);
    expect_r8(REG_CTRL2, 0x63U);
    TEST_ASSERT_EQUAL_INT(KODE_IO_ERROR, kdrv_qmi8658_st_end());
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/**
 * @brief Expect one whole CTRL9 handshake for @p cmd (datasheet 5.10.1).
 *
 * The command, the part reporting it done on STATUSINT bit 7, the host's
 * CTRL_CMD_ACK (0x00), and the bit seen to fall. A part that works answers
 * inside the first read of each wait, which is what this queues.
 */
static void expect_ctrl9(uint8_t cmd)
{
    expect_w8(REG_CTRL9, cmd);
    expect_r8(REG_STATUSINT, 0x80U);
    expect_w8(REG_CTRL9, 0x00U);
    expect_r8(REG_STATUSINT, 0x00U);
}

/** @brief A FIFO record that reads as a sample rather than as junk. */
static uint8_t const SAMPLE_REC[6] = { 0x01U, 0x00U,  0xFFU, 0xFFU,  0x00U, 0x10U };

/** @brief One with a full-scale axis, which is what a torn record looks like. */
static uint8_t const JUNK_REC[6] = { 0x00U, 0x80U,  0x00U, 0x00U,  0x00U, 0x00U };

/** @brief Arm one window, with the bus traffic the datasheet asks for. */
static void arm_window(void)
{
    /* The gyroscope off and the accelerometer left RUNNING rather than
     * switched off and on: a window's first sample is then one of a sensor
     * already converting, where a sensor turning on is a transient across
     * every frequency bin - including the one the window is judged at. */
    expect_w8(REG_CTRL7, 0x01U);
    expect_w8(REG_FIFO_CTRL, 0x00U);       /* bypass: FIFO off, read mode off  */
    expect_ctrl9(0x04U);                   /* CTRL_CMD_RST_FIFO                */
    /* FIFO_SIZE 11 (128 samples) and FIFO_MODE 01 (FIFO: it fills once and
     * stops), with FIFO_RD_MODE clear so the part may write into it. */
    expect_w8(REG_FIFO_CTRL, 0x0DU);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_qmi8658_fifo_arm());
}

/**
 * @brief The head read every open makes, answering with one whole sample.
 *
 * The bench found the first record or two of a window to be whatever the FIFO
 * memory held before it, so the driver reads the head off and keeps the first
 * record that looks like a sample. One read, when nothing is skipped.
 */
static void expect_head(void)
{
    expect_read(REG_FIFO_DATA, SAMPLE_REC, sizeof(SAMPLE_REC));
}

/** @brief End the window and leave the driver running, as every case must. */
static void disarm_window(void)
{
    expect_w8(REG_FIFO_CTRL, 0x00U);
    expect_w8_checked(REG_CTRL1, 0x58U);   /* the increment back on */
    expect_w8_checked(REG_CTRL7, 0x03U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_qmi8658_fifo_disarm());
}

TEST_CASE("qmi8658: arming a window empties the FIFO before it is filled",
          "[kdrv_qmi8658]")
{
    Mockkhal_i2c_Init();
    test_qmi8658_bring_up();
    arm_window();
    disarm_window();
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("qmi8658: a FIFO reset never reported done is a timeout",
          "[kdrv_qmi8658]")
{
    Mockkhal_i2c_Init();
    test_qmi8658_bring_up();

    /* The command goes out, and STATUSINT.bit7 never rises. The bounded wait
     * is the whole point: the handshake has no interrupt behind it here, so a
     * part that died mid-command must not hang AO_Imu's step. One read, then
     * one a millisecond until the window is spent - the bench found a FIFO
     * reset that takes longer than reads back to back allow for. */
    expect_w8(REG_CTRL7, 0x01U);
    expect_w8(REG_FIFO_CTRL, 0x00U);
    expect_w8(REG_CTRL9, 0x04U);
    for ( uint8_t i = 0U; CTRL9_READS > i; ++i )
    {
        expect_r8(REG_STATUSINT, 0x00U);
    }
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kdrv_qmi8658_fifo_arm());

    /* It failed with the sensors off; only a disarm puts them back. */
    disarm_window();
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("qmi8658: a FIFO reset reported done on the window's last read is done",
          "[kdrv_qmi8658]")
{
    Mockkhal_i2c_Init();
    test_qmi8658_bring_up();

    /* A part that works but is slow: STATUSINT.bit7 rises only on the last
     * read the 50 ms window allows. That is the case the bench found, where
     * a FIFO reset took longer than reads back to back waited for, so the
     * wait must hold out to its end and then finish the handshake. */
    expect_w8(REG_CTRL7, 0x01U);
    expect_w8(REG_FIFO_CTRL, 0x00U);
    expect_w8(REG_CTRL9, 0x04U);
    for ( uint8_t i = 0U; (CTRL9_READS - 1U) > i; ++i )
    {
        expect_read_at(REG_STATUSINT, &s_cmd_busy, 1U);
    }
    expect_read_at(REG_STATUSINT, &s_cmd_done, 1U);
    expect_w8(REG_CTRL9, 0x00U);           /* CTRL_CMD_ACK        */
    expect_read_at(REG_STATUSINT, &s_cmd_busy, 1U);
    expect_w8(REG_FIFO_CTRL, 0x0DU);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_qmi8658_fifo_arm());

    disarm_window();
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("qmi8658: the FIFO count is in words and three of them are a sample",
          "[kdrv_qmi8658]")
{
    size_t samples = 0U;
    /* 0x180 = 384 words: the low byte in FIFO_SMPL_CNT (0x15), the two high
     * bits in FIFO_STATUS[1:0] (0x16), read as one two-byte burst. Section
     * 8.4: the unit is a word of two bytes, and one sample is six bytes. */
    uint8_t const full[2]  = { 0x80U, 0x01U };
    uint8_t const part[2]  = { 0x1EU, 0x00U };  /* 30 words: 10 samples      */
    /* 0x3FF = 1023 words is more than the part can hold; the count is ten
     * bits wide whatever the configured size, so the caller is protected. */
    uint8_t const over[2]  = { 0xFFU, 0x03U };

    Mockkhal_i2c_Init();
    test_qmi8658_bring_up();
    arm_window();

    expect_read(REG_FIFO_CNT, full, sizeof(full));
    expect_ctrl9(0x05U);                   /* CTRL_CMD_REQ_FIFO */
    expect_w8_checked(REG_CTRL1, 0x18U);   /* the increment off for the drain */
    expect_head();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_qmi8658_fifo_open(&samples));
    TEST_ASSERT_EQUAL_size_t(128U, samples);

    expect_read(REG_FIFO_CNT, part, sizeof(part));
    expect_ctrl9(0x05U);
    expect_w8_checked(REG_CTRL1, 0x18U);
    expect_head();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_qmi8658_fifo_open(&samples));
    TEST_ASSERT_EQUAL_size_t(10U, samples);

    expect_read(REG_FIFO_CNT, over, sizeof(over));
    expect_ctrl9(0x05U);
    expect_w8_checked(REG_CTRL1, 0x18U);
    expect_head();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_qmi8658_fifo_open(&samples));
    TEST_ASSERT_EQUAL_size_t(128U, samples);

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_qmi8658_fifo_open(NULL));

    disarm_window();
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("qmi8658: a window comes out of one register, x y z, little-endian",
          "[kdrv_qmi8658]")
{
    size_t  samples = 0U;
    int16_t xyz[6]  = { 0, 0, 0, 0, 0, 0 };
    uint8_t const cnt[2] = { 0x06U, 0x00U };   /* 6 words: two samples */
    /* Section 8.9: with the accelerometer alone the pattern is AX_L, AX_H,
     * AY_L, AY_H, AZ_L, AZ_H per sample, and CTRL1 made the part little-
     * endian. Every byte read is erased from the FIFO. */
    uint8_t const raw[12] = {
        0x01U, 0x00U,  0xFFU, 0xFFU,  0x00U, 0x10U,
        0x00U, 0x80U,  0xFFU, 0x7FU,  0x00U, 0x00U,
    };

    Mockkhal_i2c_Init();
    test_qmi8658_bring_up();
    arm_window();

    expect_read(REG_FIFO_CNT, cnt, sizeof(cnt));
    expect_ctrl9(0x05U);
    expect_w8_checked(REG_CTRL1, 0x18U);
    /* The head read: this one IS a sample, so it is kept rather than skipped
     * and the first kdrv_qmi8658_fifo_read() hands it out without asking the
     * part again. */
    expect_read(REG_FIFO_DATA, raw, 6U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_qmi8658_fifo_open(&samples));
    TEST_ASSERT_EQUAL_size_t(2U, samples);

    /* One burst of six bytes a sample, from FIFO_DATA (0x17) and nowhere
     * else: the FIFO is a window onto one register, not a block of them. The
     * first sample came off the head, so only the second is asked for. */
    expect_read(REG_FIFO_DATA, &raw[6], 6U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_qmi8658_fifo_read(xyz, 2U));
    TEST_ASSERT_EQUAL_INT16(1, xyz[0]);
    TEST_ASSERT_EQUAL_INT16(-1, xyz[1]);
    TEST_ASSERT_EQUAL_INT16(4096, xyz[2]);
    TEST_ASSERT_EQUAL_INT16(-32768, xyz[3]);
    TEST_ASSERT_EQUAL_INT16(32767, xyz[4]);
    TEST_ASSERT_EQUAL_INT16(0, xyz[5]);

    /* Nothing that would not fit the driver's own buffer reaches the bus. */
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_qmi8658_fifo_read(NULL, 2U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_qmi8658_fifo_read(xyz, 0U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_qmi8658_fifo_read(xyz, 33U));

    disarm_window();
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* The bench found the head of a window to be whatever the FIFO memory held
 * before it: the first record is not a sample about half the time, and once
 * in four the second is a torn one with a full-scale axis. A window that
 * handed those to the detector would carry a step across every frequency bin,
 * including the one it is judged at. So the head is read off and the first
 * record that looks like a sample is kept - and the count drops by whatever
 * was thrown away, because those records were never samples to begin with. */
TEST_CASE("qmi8658: a torn record at the head is skipped and not counted",
          "[kdrv_qmi8658]")
{
    size_t  samples = 0U;
    int16_t xyz[3]  = { 0, 0, 0 };
    uint8_t const cnt[2] = { 0x09U, 0x00U };   /* 9 words: three samples */

    Mockkhal_i2c_Init();
    test_qmi8658_bring_up();
    arm_window();

    expect_read(REG_FIFO_CNT, cnt, sizeof(cnt));
    expect_ctrl9(0x05U);
    expect_w8_checked(REG_CTRL1, 0x18U);
    expect_read(REG_FIFO_DATA, JUNK_REC, sizeof(JUNK_REC));
    expect_head();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_qmi8658_fifo_open(&samples));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(2U, samples,
                                     "a record that was never a sample was "
                                     "still counted as one");

    /* And the sample that WAS found is the first one handed out, without
     * asking the part again. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_qmi8658_fifo_read(xyz, 1U));
    TEST_ASSERT_EQUAL_INT16(1, xyz[0]);
    TEST_ASSERT_EQUAL_INT16(-1, xyz[1]);
    TEST_ASSERT_EQUAL_INT16(4096, xyz[2]);

    disarm_window();
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("qmi8658: disarming returns the FIFO to bypass and both sensors on",
          "[kdrv_qmi8658]")
{
    Mockkhal_i2c_Init();
    test_qmi8658_bring_up();
    arm_window();

    /* A part that will not take CTRL7 back is an I/O error, and the window
     * stays wherever it is: the caller is told rather than left guessing. */
    expect_w8(REG_FIFO_CTRL, 0x00U);
    expect_w8_checked(REG_CTRL1, 0x58U);
    expect_w8(REG_CTRL7, 0x03U);
    expect_r8(REG_CTRL7, 0x01U);
    TEST_ASSERT_EQUAL_INT(KODE_IO_ERROR, kdrv_qmi8658_fifo_disarm());

    disarm_window();
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}
