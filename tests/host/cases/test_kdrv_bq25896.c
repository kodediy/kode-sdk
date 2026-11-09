/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_bq25896.c
 * @brief The BQ25896 driver, checked by what it says on the bus.
 */
#include <string.h>

#include "unity.h"

#include "Mockkhal_i2c.h"

#include "fakes.h"
#include "kbsp.h"
#include "kdrv_bq25896.h"

/* The register map, repeated rather than imported, so a wrong driver constant fails here. */
#define REG00_INPUT     0x00U
#define REG01_VINDPM_OS 0x01U
#define REG02_ADC_CTRL  0x02U
#define REG03_SYS       0x03U
#define REG04_ICHG      0x04U
#define REG05_PRE_TERM  0x05U
#define REG06_VREG      0x06U
#define REG07_TIMERS    0x07U
#define REG08_IRCOMP    0x08U
#define REG09_BATFET    0x09U
#define REG0A_BOOST     0x0AU
#define REG0B_STATUS    0x0BU
#define REG0C_FAULT     0x0CU
#define REG0D_VINDPM    0x0DU
#define REG0E_BATV      0x0EU
#define REG10_TSPCT     0x10U
#define REG14_ID        0x14U

/** @brief Any non-NULL handle; every use of it is mocked. */
static khal_i2c_dev_t s_handle = (khal_i2c_dev_t)(void *)&s_handle;

/**
 * @brief Queue one mocked register read returning @p val.
 * @param reg  The register the driver is expected to ask for.
 * @param val  What the chip answers.
 * @param hold Storage for the value; must outlive the call.
 */
static void expect_r8(uint8_t reg, uint8_t val, uint8_t *hold)
{
    *hold = val;
    khal_i2c_read_reg_ExpectAndReturn(NULL, reg, NULL, 1U, KODE_OK);
    khal_i2c_read_reg_IgnoreArg_dev();
    khal_i2c_read_reg_IgnoreArg_buf();
    khal_i2c_read_reg_ReturnMemThruPtr_buf(hold, 1U);
}

/** @brief Refuse the next read of @p reg, as a dead bus would. */
static void expect_r8_fail(uint8_t reg)
{
    khal_i2c_read_reg_ExpectAndReturn(NULL, reg, NULL, 1U, KODE_ERROR);
    khal_i2c_read_reg_IgnoreArg_dev();
    khal_i2c_read_reg_IgnoreArg_buf();
}

/** @brief Expect one register write of @p val to @p reg. */
static void expect_w8(uint8_t reg, uint8_t val)
{
    khal_i2c_write_reg_ExpectAndReturn(NULL, reg, val, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
}

/** @brief The bus side of a bring-up, up to and including the identity read. */
static void expect_bring_up(uint8_t id, uint8_t *hold)
{
    khal_i2c_init_ExpectAndReturn(KODE_OK);
    khal_i2c_probe_ExpectAndReturn(KBSP_I2C_ADDR_CHARGER, 100U, true);
    khal_i2c_add_ExpectAnyArgsAndReturn(KODE_OK);
    khal_i2c_add_ReturnThruPtr_out(&s_handle);
    expect_r8(REG14_ID, id, hold);
}

/** @brief Bring the driver up once per binary. */
static void bring_up(void)
{
    static bool    s_up;
    static uint8_t id;

    if ( s_up )
    {
        return;
    }
    /* PN = 000 in bits 5:3, DEV_REV = 10: the datasheet's reset value. */
    expect_bring_up(0x02U, &id);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_bq25896_init());
    s_up = true;
}

/** @brief Read @p f after @p reg answers @p raw. */
static int32_t get_after(kdrv_bq25896_field_t f, uint8_t reg, uint8_t raw)
{
    uint8_t hold;
    int32_t v = -1;

    expect_r8(reg, raw, &hold);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_bq25896_get(f, &v));
    return v;
}

/* ---- Bring-up ------------------------------------------------------------ */

/* First in the file, and it has to be: it is the only case that can see the
 * driver before it is up. A bq25890 answers at the same address with PN =
 * 011; decoding its registers with this table would be quietly wrong. */
TEST_CASE("a chip whose part number is not the bq25896's is refused",
          "[kdrv_bq25896]")
{
    uint8_t id;

    Mockkhal_i2c_Init();
    expect_bring_up(0x1AU, &id);              /* PN = 011 */
    khal_i2c_remove_ExpectAndReturn(s_handle, KODE_OK);   /* and it goes back */
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_bq25896_init());
    TEST_ASSERT_FALSE(kdrv_bq25896_present());
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* Bring-up identifies the chip and writes nothing. What the PMIC should be
 * doing is the owner's decision, made through the table after this. */
TEST_CASE("bringing the PMIC up identifies it and writes nothing",
          "[kdrv_bq25896]")
{
    Mockkhal_i2c_Init();
    bring_up();
    TEST_ASSERT_TRUE(kdrv_bq25896_present());
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* ---- Decoding ------------------------------------------------------------ */

/* The datasheet prints a reset value for every register and the number it
 * stands for. A table that decodes all of these back to those numbers has
 * its offsets and steps right. */
TEST_CASE("the reset values decode to the datasheet's numbers",
          "[kdrv_bq25896]")
{
    Mockkhal_i2c_Init();
    bring_up();

    TEST_ASSERT_EQUAL_INT32(500,  get_after(KDRV_BQ_IINLIM_MA,   REG00_INPUT,     0x08U));
    TEST_ASSERT_EQUAL_INT32(600,  get_after(KDRV_BQ_VINDPM_OS_MV, REG01_VINDPM_OS, 0x06U));
    TEST_ASSERT_EQUAL_INT32(3500, get_after(KDRV_BQ_SYS_MIN_MV,  REG03_SYS,       0x1AU));
    TEST_ASSERT_EQUAL_INT32(2048, get_after(KDRV_BQ_ICHG_MA,     REG04_ICHG,      0x20U));
    TEST_ASSERT_EQUAL_INT32(128,  get_after(KDRV_BQ_IPRECHG_MA,  REG05_PRE_TERM,  0x13U));
    TEST_ASSERT_EQUAL_INT32(256,  get_after(KDRV_BQ_ITERM_MA,    REG05_PRE_TERM,  0x13U));
    TEST_ASSERT_EQUAL_INT32(4208, get_after(KDRV_BQ_VREG_MV,     REG06_VREG,      0x5EU));
    TEST_ASSERT_EQUAL_INT32(1,    get_after(KDRV_BQ_BATLOWV,     REG06_VREG,      0x5EU));
    TEST_ASSERT_EQUAL_INT32(1,    get_after(KDRV_BQ_WATCHDOG,    REG07_TIMERS,    0x9DU));
    TEST_ASSERT_EQUAL_INT32(2,    get_after(KDRV_BQ_CHG_TIMER,   REG07_TIMERS,    0x9DU));
    TEST_ASSERT_EQUAL_INT32(120,  get_after(KDRV_BQ_TREG_C,      REG08_IRCOMP,    0x03U));
    TEST_ASSERT_EQUAL_INT32(4998, get_after(KDRV_BQ_BOOSTV_MV,   REG0A_BOOST,     0x73U));
    TEST_ASSERT_EQUAL_INT32(3,    get_after(KDRV_BQ_BOOST_LIM,   REG0A_BOOST,     0x73U));
    TEST_ASSERT_EQUAL_INT32(4400, get_after(KDRV_BQ_VINDPM_MV,   REG0D_VINDPM,    0x12U));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* Two fields the bench leans on. VBUS_STAT = 111 is the only proof that the
 * 5 V on the header is the PMIC's and not the cable's; and bit 7 of REG0E is
 * a thermal flag, not 2.5 V of battery. */
TEST_CASE("status and ADC fields decode their own bits", "[kdrv_bq25896]")
{
    Mockkhal_i2c_Init();
    bring_up();

    TEST_ASSERT_EQUAL_INT32(KDRV_BQ_VBUS_OTG,  get_after(KDRV_BQ_VBUS_STAT, REG0B_STATUS, 0xE0U));
    TEST_ASSERT_EQUAL_INT32(KDRV_BQ_CHRG_FAST, get_after(KDRV_BQ_CHRG_STAT, REG0B_STATUS, 0x10U));
    TEST_ASSERT_EQUAL_INT32(1,                 get_after(KDRV_BQ_PG_STAT,   REG0B_STATUS, 0x04U));
    TEST_ASSERT_EQUAL_INT32(0,                 get_after(KDRV_BQ_PG_STAT,   REG0B_STATUS, 0xFBU));

    /* 80 steps of 20 mV above 2304, with THERM set and ignored. */
    TEST_ASSERT_EQUAL_INT32(3904,  get_after(KDRV_BQ_BATV_MV,      REG0E_BATV,  0xD0U));
    TEST_ASSERT_EQUAL_INT32(1,     get_after(KDRV_BQ_THERM_STAT,   REG0E_BATV,  0xD0U));
    /* Full scale on TS: 21 % + 127 x 0.465 %. */
    TEST_ASSERT_EQUAL_INT32(80055, get_after(KDRV_BQ_TS_PCT_X1000, REG10_TSPCT, 0x7FU));
    TEST_ASSERT_EQUAL_INT32(0,     get_after(KDRV_BQ_PN,           REG14_ID,    0x02U));
    TEST_ASSERT_EQUAL_INT32(2,     get_after(KDRV_BQ_DEV_REV,      REG14_ID,    0x02U));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* A register that could not be read decodes to nothing. Zero would decode
 * to 2304 mV, no fault, not charging: exactly what a flat cell reads like. */
TEST_CASE("a failed read is an error and leaves the value alone",
          "[kdrv_bq25896]")
{
    int32_t v = 12345;

    Mockkhal_i2c_Init();
    bring_up();

    expect_r8_fail(REG0E_BATV);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_bq25896_get(KDRV_BQ_BATV_MV, &v));
    TEST_ASSERT_EQUAL_INT32(12345, v);

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_bq25896_get(KDRV_BQ_BATV_MV, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_bq25896_get(KDRV_BQ_FIELD_MAX, &v));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* ---- Writing ------------------------------------------------------------- */

/** @brief The bus side of one verified write: read, write, read back. */
static void expect_rmw(uint8_t reg, uint8_t before, uint8_t after, uint8_t *hold)
{
    expect_r8(reg, before, &hold[0]);
    expect_w8(reg, after);
    expect_r8(reg, after, &hold[1]);
}

/* REG04 carries EN_PUMPX above ICHG. A write that forgot the mask would
 * clear it; the bench's ichg_lab did exactly this by hand and the datasheet
 * has no register with only one field in it. */
TEST_CASE("a write changes its own bits and leaves the register's others alone",
          "[kdrv_bq25896]")
{
    uint8_t h[2];

    Mockkhal_i2c_Init();
    bring_up();

    /* EN_PUMPX set, ICHG at 2048: ICHG becomes 512 (code 8), EN_PUMPX stays. */
    expect_rmw(REG04_ICHG, 0xA0U, 0x88U, h);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_bq25896_set(KDRV_BQ_ICHG_MA, 512));

    /* Watchdog off: bits 5:4 of REG07 cleared, the other six untouched. */
    expect_rmw(REG07_TIMERS, 0x9DU, 0x8DU, h);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_bq25896_set(KDRV_BQ_WATCHDOG, 0));

    /* A bool, set and cleared. */
    expect_rmw(REG02_ADC_CTRL, 0x11U, 0x51U, h);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_bq25896_set(KDRV_BQ_CONV_RATE, 1));
    expect_rmw(REG02_ADC_CTRL, 0x51U, 0x11U, h);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_bq25896_set(KDRV_BQ_CONV_RATE, 0));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* The chip only knows multiples of the step. Asking for 500 mA means the
 * nearest one, 512, and the caller finds that out from the readback. */
TEST_CASE("a value between steps is rounded to the nearest step",
          "[kdrv_bq25896]")
{
    uint8_t h[2];

    Mockkhal_i2c_Init();
    bring_up();

    expect_rmw(REG04_ICHG, 0x00U, 0x08U, h);      /* 500 -> 512, code 8  */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_bq25896_set(KDRV_BQ_ICHG_MA, 500));

    expect_rmw(REG06_VREG, 0x02U, 0x5EU, h);      /* 4200 -> 4208, code 23 */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_bq25896_set(KDRV_BQ_VREG_MV, 4200));

    expect_rmw(REG00_INPUT, 0x00U, 0x08U, h);     /* 480 -> 500, code 8  */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_bq25896_set(KDRV_BQ_IINLIM_MA, 480));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* Out of range is refused before the bus is touched. The datasheet says the
 * chip clamps ICHG above 3008 mA; a driver that relayed that would let a
 * caller believe it asked for 3.1 A and got it. */
TEST_CASE("a value outside the field's range is refused without a write",
          "[kdrv_bq25896]")
{
    Mockkhal_i2c_Init();
    bring_up();

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_bq25896_set(KDRV_BQ_ICHG_MA, 3100));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_bq25896_set(KDRV_BQ_ICHG_MA, -1));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_bq25896_set(KDRV_BQ_VINDPM_MV, 3000));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_bq25896_set(KDRV_BQ_IINLIM_MA, 50));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_bq25896_set(KDRV_BQ_CONV_RATE, 2));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_bq25896_set(KDRV_BQ_BOOST_LIM, 7));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_bq25896_set(KDRV_BQ_FIELD_MAX, 0));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("a status or ADC field cannot be written", "[kdrv_bq25896]")
{
    Mockkhal_i2c_Init();
    bring_up();

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_bq25896_set(KDRV_BQ_BATV_MV, 3000));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_bq25896_set(KDRV_BQ_CHRG_STAT, 0));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_bq25896_set(KDRV_BQ_PN, 0));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* The setting itself is free to read back, so it is read back. A write the
 * chip did not take must be an error here, not a surprise forty seconds
 * later when the watchdog has quietly restored the default. */
TEST_CASE("a write the chip did not take is an error", "[kdrv_bq25896]")
{
    uint8_t h[2];

    Mockkhal_i2c_Init();
    bring_up();

    expect_r8(REG07_TIMERS, 0x9DU, &h[0]);
    expect_w8(REG07_TIMERS, 0x8DU);
    expect_r8(REG07_TIMERS, 0x9DU, &h[1]);       /* still 40 s */
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_bq25896_set(KDRV_BQ_WATCHDOG, 0));

    /* And a bus that fails at any of the three steps is an error too. */
    expect_r8_fail(REG07_TIMERS);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_bq25896_set(KDRV_BQ_WATCHDOG, 0));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* WD_RST, FORCE_ICO, PUMPX_*, CONV_START, FORCE_DPDM and REG_RST return to
 * zero by themselves, so a readback proves nothing about them. Written and
 * trusted. */
TEST_CASE("a self-clearing field is written and not read back",
          "[kdrv_bq25896]")
{
    uint8_t h;

    Mockkhal_i2c_Init();
    bring_up();

    expect_r8(REG03_SYS, 0x1AU, &h);
    expect_w8(REG03_SYS, 0x5AU);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_bq25896_set(KDRV_BQ_WD_RST, 1));

    expect_r8(REG09_BATFET, 0x44U, &h);
    expect_w8(REG09_BATFET, 0xC4U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_bq25896_set(KDRV_BQ_FORCE_ICO, 1));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* ---- The registers a table cannot express ------------------------------- */

/**
 * @brief Queue one mocked multi-byte read of @p len registers from @p reg.
 * @param hold At least @p len bytes; must outlive the call.
 */
static void expect_rn(uint8_t reg, const uint8_t *bytes, size_t len, uint8_t *hold)
{
    (void)memcpy(hold, bytes, len);
    khal_i2c_read_reg_ExpectAndReturn(NULL, reg, NULL, len, KODE_OK);
    khal_i2c_read_reg_IgnoreArg_dev();
    khal_i2c_read_reg_IgnoreArg_buf();
    khal_i2c_read_reg_ReturnMemThruPtr_buf(hold, len);
}

/* Five registers, one transaction: a voltage and a current from the same
 * conversion, and one bus round trip per poll instead of five. */
TEST_CASE("the ADC block is read as one transaction and decoded whole",
          "[kdrv_bq25896]")
{
    static const uint8_t regs[5] = { 0xD0U, 0x50U, 0x7FU, 0x98U, 0x05U };
    uint8_t              h[5];
    kdrv_bq25896_adc_t   a;

    Mockkhal_i2c_Init();
    bring_up();

    expect_rn(REG0E_BATV, regs, 5U, h);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_bq25896_adc_read(&a));
    TEST_ASSERT_EQUAL_UINT16(3904U,  a.vbat_mv);       /* 80 steps, THERM masked */
    TEST_ASSERT_TRUE(a.therm_stat);
    TEST_ASSERT_EQUAL_UINT16(3904U,  a.vsys_mv);
    TEST_ASSERT_EQUAL_UINT16(80055U, a.ts_pct_x1000);
    TEST_ASSERT_TRUE(a.vbus_gd);
    TEST_ASSERT_EQUAL_UINT16(5000U,  a.vbus_mv);       /* 24 steps of 100 */
    TEST_ASSERT_EQUAL_UINT16(250U,   a.ichg_ma);       /* 5 steps of 50   */

    /* A failed transaction decodes nothing. */
    khal_i2c_read_reg_ExpectAndReturn(NULL, REG0E_BATV, NULL, 5U, KODE_ERROR);
    khal_i2c_read_reg_IgnoreArg_dev();
    khal_i2c_read_reg_IgnoreArg_buf();
    (void)memset(&a, 0xA5, sizeof(a));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_bq25896_adc_read(&a));
    TEST_ASSERT_EQUAL_HEX16(0xA5A5U, a.vbat_mv);

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_bq25896_adc_read(NULL));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* REG0C keeps everything since the last read. One read is the history, the
 * next is the present, and NTC is live in both. */
TEST_CASE("the fault register is read twice: what happened, then what is true",
          "[kdrv_bq25896]")
{
    uint8_t               h[4];
    kdrv_bq25896_faults_t latched;
    kdrv_bq25896_faults_t live;

    Mockkhal_i2c_Init();
    bring_up();

    /* Watchdog expired and the safety timer ran out at some point; the
     * thermistor reads hot right now. */
    expect_r8(REG0C_FAULT, 0xB6U, &h[0]);
    expect_r8(REG0C_FAULT, 0x06U, &h[1]);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_bq25896_faults(&latched, &live));
    TEST_ASSERT_TRUE(latched.watchdog);
    TEST_ASSERT_FALSE(latched.boost);
    TEST_ASSERT_EQUAL_UINT8(3U, latched.chrg);
    TEST_ASSERT_FALSE(latched.bat_ovp);
    TEST_ASSERT_EQUAL_UINT8(6U, latched.ntc);
    TEST_ASSERT_EQUAL_HEX8(0xB6U, latched.raw);
    TEST_ASSERT_FALSE(live.watchdog);
    TEST_ASSERT_EQUAL_UINT8(0U, live.chrg);
    TEST_ASSERT_EQUAL_UINT8(6U, live.ntc);
    TEST_ASSERT_EQUAL_HEX8(0x06U, live.raw);

    /* Only the history: one read, and the latch is now clear. */
    expect_r8(REG0C_FAULT, 0x08U, &h[2]);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_bq25896_faults(&latched, NULL));
    TEST_ASSERT_TRUE(latched.bat_ovp);

    /* Only the present still costs two reads; the first is the latch. */
    expect_r8(REG0C_FAULT, 0x80U, &h[2]);
    expect_r8(REG0C_FAULT, 0x00U, &h[3]);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_bq25896_faults(NULL, &live));
    TEST_ASSERT_EQUAL_HEX8(0x00U, live.raw);

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_bq25896_faults(NULL, NULL));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* Ship mode sets BATFET_DIS and must leave BATFET_RST_EN alone or the QON
 * long-press reset stops working. On hardware that needs a battery, no cable
 * and somebody in the room; here it is three literals. */
TEST_CASE("ship mode is one write that sets DIS, chooses DLY and keeps the rest",
          "[kdrv_bq25896]")
{
    uint8_t h;

    Mockkhal_i2c_Init();
    bring_up();

    /* DLY and RST_EN both set: DLY goes, RST_EN stays, DIS arrives. */
    expect_r8(REG09_BATFET, 0x0CU, &h);
    expect_w8(REG09_BATFET, 0x24U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_bq25896_ship_mode(false));

    /* Asked for the grace period: DLY set alongside DIS. */
    expect_r8(REG09_BATFET, 0x04U, &h);
    expect_w8(REG09_BATFET, 0x2CU);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_bq25896_ship_mode(true));

    /* No read, no guess, no write. */
    expect_r8_fail(REG09_BATFET);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_bq25896_ship_mode(false));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* The dump reads the map in three transactions, because REG0C cannot be
 * part of a burst, and REG0C twice. */
TEST_CASE("the dump reads the whole map around the fault register and decodes it",
          "[kdrv_bq25896]")
{
    static const uint8_t lo[12] = { 0x08U, 0x06U, 0x11U, 0x1AU, 0x20U, 0x13U,
                                    0x5EU, 0x9DU, 0x03U, 0x44U, 0x73U, 0x10U };
    static const uint8_t hi[8]  = { 0x12U, 0xD0U, 0x50U, 0x7FU, 0x98U, 0x05U,
                                    0x00U, 0x02U };
    uint8_t h[24];
    bool    decoded = false;
    bool    hex     = false;

    Mockkhal_i2c_Init();
    bring_up();
    fake_klog_reset();

    expect_rn(REG00_INPUT, lo, 12U, &h[0]);
    expect_r8(REG0C_FAULT, 0x80U, &h[12]);
    expect_r8(REG0C_FAULT, 0x00U, &h[13]);
    expect_rn(REG0D_VINDPM, hi, 8U, &h[14]);
    kdrv_bq25896_dump();

    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, "ichg_ma=2048") != NULL) )
        {
            decoded = true;
        }
        if ( (NULL != l) && (strstr(l, "REG04=0x20") != NULL) )
        {
            hex = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(decoded, "the dump did not decode the fields");
    TEST_ASSERT_TRUE_MESSAGE(hex, "the dump did not print the raw registers");

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* ---- Names --------------------------------------------------------------- */

/* The console reaches every field by the name field_info() prints, so the two
 * directions must agree for all of them, not for the ones somebody tried. */
TEST_CASE("every field's name looks itself up", "[kdrv_bq25896]")
{
    for ( uint32_t i = 0U; (uint32_t)KDRV_BQ_FIELD_MAX > i; ++i )
    {
        kdrv_bq25896_field_t              f = KDRV_BQ_FIELD_MAX;
        const kdrv_bq25896_field_info_t * const info =
            kdrv_bq25896_field_info((kdrv_bq25896_field_t)i);

        TEST_ASSERT_NOT_NULL(info);
        TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_bq25896_field_by_name(info->name, &f));
        TEST_ASSERT_EQUAL_UINT32(i, (uint32_t)f);
    }
    TEST_ASSERT_NULL(kdrv_bq25896_field_info(KDRV_BQ_FIELD_MAX));
}

TEST_CASE("a name nobody printed is refused", "[kdrv_bq25896]")
{
    kdrv_bq25896_field_t f;

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_bq25896_field_by_name("ichg", &f));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_bq25896_field_by_name("", &f));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_bq25896_field_by_name(NULL, &f));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_bq25896_field_by_name("ichg_ma", NULL));
}

TEST_CASE("a field's info states its range, step and writability",
          "[kdrv_bq25896]")
{
    const kdrv_bq25896_field_info_t *i;

    i = kdrv_bq25896_field_info(KDRV_BQ_ICHG_MA);
    TEST_ASSERT_EQUAL_STRING("ichg_ma", i->name);
    TEST_ASSERT_EQUAL_STRING("mA", i->unit);
    TEST_ASSERT_EQUAL_INT32(0, i->min);
    TEST_ASSERT_EQUAL_INT32(3008, i->max);
    TEST_ASSERT_EQUAL_UINT16(64U, i->step);
    TEST_ASSERT_TRUE(i->writable);

    i = kdrv_bq25896_field_info(KDRV_BQ_VINDPM_MV);
    TEST_ASSERT_EQUAL_INT32(3900, i->min);
    TEST_ASSERT_EQUAL_INT32(15300, i->max);

    i = kdrv_bq25896_field_info(KDRV_BQ_BATV_MV);
    TEST_ASSERT_FALSE(i->writable);
    TEST_ASSERT_EQUAL_INT32(2304, i->min);
}
