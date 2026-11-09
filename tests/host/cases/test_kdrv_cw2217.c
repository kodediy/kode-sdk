/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_cw2217.c
 * @brief The CW2217 driver, checked by what it says on the bus.
 */
#include <string.h>

#include "unity.h"

#include "Mockkhal_i2c.h"

#include "fakes.h"
#include "kbsp.h"
#include "kdrv_cw2217.h"

/* The register map, repeated rather than imported, so a wrong driver constant fails here. */
#define CW_VERSION    0x00U
#define CW_VCELL_H    0x02U
#define CW_SOC_H      0x04U
#define CW_TEMP       0x06U
#define CW_CONFIG     0x08U
#define CW_INT_CONF   0x0AU
#define CW_SOC_ALERT  0x0BU
#define CW_TEMP_MAX   0x0CU
#define CW_TEMP_MIN   0x0DU
#define CW_CURRENT_H  0x0EU
#define CW_PROFILE    0x10U
#define CW_T_HOST_H   0xA0U
#define CW_T_HOST_L   0xA1U
#define CW_USER_CONF  0xA2U
#define CW_CYCLECNT_H 0xA4U
#define CW_SOH        0xA6U
#define CW_IC_STATE   0xA7U
#define CW_AVE_CUR_H  0xA8U
#define CW_FW_VERSION 0xABU

/** @brief Any non-NULL handle; every use of it is mocked. */
static khal_i2c_dev_t s_handle = (khal_i2c_dev_t)(void *)&s_handle;

static void expect_r8(uint8_t reg, uint8_t val, uint8_t *hold)
{
    *hold = val;
    khal_i2c_read_reg_ExpectAndReturn(NULL, reg, NULL, 1U, KODE_OK);
    khal_i2c_read_reg_IgnoreArg_dev();
    khal_i2c_read_reg_IgnoreArg_buf();
    khal_i2c_read_reg_ReturnMemThruPtr_buf(hold, 1U);
}

static void expect_r16(uint8_t reg, uint16_t val, uint8_t *hold)
{
    hold[0] = (uint8_t)(val >> 8);
    hold[1] = (uint8_t)(val & 0xFFU);
    khal_i2c_read_reg_ExpectAndReturn(NULL, reg, NULL, 2U, KODE_OK);
    khal_i2c_read_reg_IgnoreArg_dev();
    khal_i2c_read_reg_IgnoreArg_buf();
    khal_i2c_read_reg_ReturnMemThruPtr_buf(hold, 2U);
}

static void expect_r8_fail(uint8_t reg)
{
    khal_i2c_read_reg_ExpectAndReturn(NULL, reg, NULL, 1U, KODE_ERROR);
    khal_i2c_read_reg_IgnoreArg_dev();
    khal_i2c_read_reg_IgnoreArg_buf();
}

static void expect_w8(uint8_t reg, uint8_t val)
{
    khal_i2c_write_reg_ExpectAndReturn(NULL, reg, val, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
}

static void expect_bring_up(uint8_t version, uint8_t *hold)
{
    khal_i2c_init_ExpectAndReturn(KODE_OK);
    khal_i2c_probe_ExpectAndReturn(KBSP_I2C_ADDR_GAUGE, 100U, true);
    khal_i2c_add_ExpectAnyArgsAndReturn(KODE_OK);
    khal_i2c_add_ReturnThruPtr_out(&s_handle);
    expect_r8(CW_VERSION, version, hold);
}

static void bring_up(void)
{
    static bool    s_up;
    static uint8_t ver;

    if ( s_up )
    {
        return;
    }
    expect_bring_up(0xA0U, &ver);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cw2217_init());
    s_up = true;
}

static int32_t get8(kdrv_cw2217_field_t f, uint8_t reg, uint8_t raw)
{
    uint8_t hold;
    int32_t v = -1;

    expect_r8(reg, raw, &hold);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cw2217_get(f, &v));
    return v;
}

static int32_t get16(kdrv_cw2217_field_t f, uint8_t reg, uint16_t raw)
{
    uint8_t hold[2];
    int32_t v = -1;

    expect_r16(reg, raw, hold);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cw2217_get(f, &v));
    return v;
}

/* ---- Bring-up ------------------------------------------------------------ */

/* First in the file, on purpose: the only case that sees the driver before
 * it is up. VERSION is 0xA0 for the whole CW221x family, asleep or awake. */
TEST_CASE("a chip that does not answer 0xA0 as its version is refused",
          "[kdrv_cw2217]")
{
    uint8_t ver;

    Mockkhal_i2c_Init();
    expect_bring_up(0x00U, &ver);
    /* ...and the device it was attached as goes back: the next bring-up
     * adds it again, and a handle forgotten instead would be a second one. */
    khal_i2c_remove_ExpectAndReturn(s_handle, KODE_OK);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_cw2217_init());
    TEST_ASSERT_FALSE(kdrv_cw2217_present());
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* A gauge on a nearly flat pack ACKs and then NAKs the first read. Nothing
 * is known about it yet, so it is not up - and the device goes back. */
TEST_CASE("a version that would not read leaves the gauge down and the device handed back",
          "[kdrv_cw2217]")
{
    Mockkhal_i2c_Init();
    khal_i2c_init_ExpectAndReturn(KODE_OK);
    khal_i2c_probe_ExpectAndReturn(KBSP_I2C_ADDR_GAUGE, 100U, true);
    khal_i2c_add_ExpectAnyArgsAndReturn(KODE_OK);
    khal_i2c_add_ReturnThruPtr_out(&s_handle);
    expect_r8_fail(CW_VERSION);
    khal_i2c_remove_ExpectAndReturn(s_handle, KODE_OK);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_cw2217_init());
    TEST_ASSERT_FALSE(kdrv_cw2217_present());
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("bringing the gauge up reads its version and writes nothing",
          "[kdrv_cw2217]")
{
    Mockkhal_i2c_Init();
    bring_up();
    TEST_ASSERT_TRUE(kdrv_cw2217_present());
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* ---- Power states -------------------------------------------------------- */

/* The bench measured 900-1100 ms from the second write to a chip that
 * answers real numbers. A wake that waited for that here would be a wake
 * nobody could call from an AO; this one returns and the owner asks. */
TEST_CASE("wake on a chip in shutdown is the two-step sequence and returns without polling",
          "[kdrv_cw2217]")
{
    uint8_t h;

    Mockkhal_i2c_Init();
    bring_up();

    expect_r8(CW_CONFIG, 0xF0U, &h);
    expect_w8(CW_CONFIG, 0x30U);
    expect_w8(CW_CONFIG, 0x00U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cw2217_wake());

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* The chip runs off the cell, so every reset of the P4 finds it active. The
 * same two writes there are a restart: the estimate is thrown away and READY
 * drops for about 1.5 s. */
TEST_CASE("wake on a chip that is already active writes nothing",
          "[kdrv_cw2217]")
{
    uint8_t h;

    Mockkhal_i2c_Init();
    bring_up();

    expect_r8(CW_CONFIG, 0x00U, &h);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cw2217_wake());

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("wake writes nothing when the config will not read",
          "[kdrv_cw2217]")
{
    Mockkhal_i2c_Init();
    bring_up();

    expect_r8_fail(CW_CONFIG);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_cw2217_wake());

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("sleep clears the sleep bits and then sets them all", "[kdrv_cw2217]")
{
    Mockkhal_i2c_Init();
    bring_up();

    expect_w8(CW_CONFIG, 0x30U);
    expect_w8(CW_CONFIG, 0xF0U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cw2217_sleep());

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* Readiness is bits 2 and 3 of a register the datasheet does not list. Until
 * both are set, VCELL reads 0 mV and FW_VERSION 0x00 - numbers, not errors. */
TEST_CASE("ready is bits 2 and 3 of the vendor register, both of them",
          "[kdrv_cw2217]")
{
    Mockkhal_i2c_Init();
    bring_up();

    TEST_ASSERT_EQUAL_INT32(0, get8(KDRV_CW_READY, CW_IC_STATE, 0x00U));
    TEST_ASSERT_EQUAL_INT32(0, get8(KDRV_CW_READY, CW_IC_STATE, 0x04U));
    TEST_ASSERT_EQUAL_INT32(0, get8(KDRV_CW_READY, CW_IC_STATE, 0x08U));
    TEST_ASSERT_EQUAL_INT32(1, get8(KDRV_CW_READY, CW_IC_STATE, 0x0CU));
    TEST_ASSERT_EQUAL_INT32(1, get8(KDRV_CW_READY, CW_IC_STATE, 0xFFU));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("the datasheet's restart writes the three configs and waits for ready",
          "[kdrv_cw2217]")
{
    uint8_t h[2];

    Mockkhal_i2c_Init();
    bring_up();

    expect_w8(CW_CONFIG, 0xF0U);
    expect_w8(CW_CONFIG, 0x30U);
    expect_w8(CW_CONFIG, 0x00U);
    expect_r8(CW_IC_STATE, 0x00U, &h[0]);
    expect_r8(CW_IC_STATE, 0x0CU, &h[1]);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cw2217_reset());

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* ---- Measurements -------------------------------------------------------- */

/* 14 bits at 312.5 uV. 0x2E80 is the 3720 mV the bench read; all ones
 * masked is 16383 steps, about 5.12 V, not the 20 V an unmasked read gives. */
TEST_CASE("the cell voltage is 14 bits at 312.5 microvolts", "[kdrv_cw2217]")
{
    Mockkhal_i2c_Init();
    bring_up();

    TEST_ASSERT_EQUAL_INT32(3720, get16(KDRV_CW_VCELL_MV, CW_VCELL_H, 0x2E80U));
    TEST_ASSERT_EQUAL_INT32(5120, get16(KDRV_CW_VCELL_MV, CW_VCELL_H, 0xFFFFU));
    TEST_ASSERT_EQUAL_INT32(0,    get16(KDRV_CW_VCELL_MV, CW_VCELL_H, 0x0000U));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* SOC comes back raw: the integer percent in the high byte and 1/256 % in the
 * low. 0xFEB3 is what a gauge with no profile answers, and it must survive
 * the trip so the owner can recognise it rather than draw 254 %. */
TEST_CASE("the state of charge passes through in 1/256 percent",
          "[kdrv_cw2217]")
{
    Mockkhal_i2c_Init();
    bring_up();

    TEST_ASSERT_EQUAL_INT32(0x2A80, get16(KDRV_CW_SOC_X256, CW_SOC_H, 0x2A80U));
    TEST_ASSERT_EQUAL_INT32(0xFEB3, get16(KDRV_CW_SOC_X256, CW_SOC_H, 0xFEB3U));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* T = -40 + raw/2, kept in half degrees so nothing is rounded away. 0x00 is
 * -40 C, which is what this board's floating TS pin reads forever. */
TEST_CASE("temperatures are minus forty plus half the byte", "[kdrv_cw2217]")
{
    Mockkhal_i2c_Init();
    bring_up();

    TEST_ASSERT_EQUAL_INT32(-80, get8(KDRV_CW_TEMP_C2,     CW_TEMP,     0x00U));
    TEST_ASSERT_EQUAL_INT32(50,  get8(KDRV_CW_TEMP_C2,     CW_TEMP,     0x82U));
    TEST_ASSERT_EQUAL_INT32(175, get8(KDRV_CW_TEMP_MAX_C2, CW_TEMP_MAX, 0xFFU));
    TEST_ASSERT_EQUAL_INT32(80,  get8(KDRV_CW_TEMP_MIN_C2, CW_TEMP_MIN, 0xA0U));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* I = 52.4 mV x raw / (32768 x Rsense). Across this board's 10 mOhm that is
 * 160 uA per step, 5.24 A full scale, and the sign is the datasheet's:
 * charging positive, which is also the board's: R75 sits across CSP/CSN the
 * right way round. */
TEST_CASE("current scales through the board's shunt with the datasheet's sign",
          "[kdrv_cw2217]")
{
    Mockkhal_i2c_Init();
    bring_up();

    TEST_ASSERT_EQUAL_UINT(10U, KBSP_CELL_RSENSE_MOHM);
    TEST_ASSERT_EQUAL_INT32(5240,  get16(KDRV_CW_CURRENT_MA, CW_CURRENT_H, 0x7FFFU));
    TEST_ASSERT_EQUAL_INT32(-5240, get16(KDRV_CW_CURRENT_MA, CW_CURRENT_H, 0x8001U));
    TEST_ASSERT_EQUAL_INT32(0,     get16(KDRV_CW_CURRENT_MA, CW_CURRENT_H, 0x0000U));
    TEST_ASSERT_EQUAL_INT32(-253,  get16(KDRV_CW_CURRENT_MA, CW_CURRENT_H, 0xF9D0U));
    TEST_ASSERT_EQUAL_INT32(263,   get16(KDRV_CW_AVE_CURRENT_MA, CW_AVE_CUR_H, 0x066CU));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("cycles, health and versions come back as the chip holds them",
          "[kdrv_cw2217]")
{
    Mockkhal_i2c_Init();
    bring_up();

    TEST_ASSERT_EQUAL_INT32(32,   get16(KDRV_CW_CYCLES_X16, CW_CYCLECNT_H, 0x0020U));
    TEST_ASSERT_EQUAL_INT32(100,  get8(KDRV_CW_SOH_PCT, CW_SOH, 0x64U));
    TEST_ASSERT_EQUAL_INT32(0xA0, get8(KDRV_CW_VERSION, CW_VERSION, 0xA0U));
    TEST_ASSERT_EQUAL_INT32(0x4A, get8(KDRV_CW_FW_VERSION, CW_FW_VERSION, 0x4AU));
    TEST_ASSERT_EQUAL_INT32(0x5A, get8(KDRV_CW_USER_CONF, CW_USER_CONF, 0x5AU));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("the alarm registers decode their bits", "[kdrv_cw2217]")
{
    Mockkhal_i2c_Init();
    bring_up();

    TEST_ASSERT_EQUAL_INT32(1, get8(KDRV_CW_EN_SOC_INT,    CW_INT_CONF,  0x40U));
    TEST_ASSERT_EQUAL_INT32(0, get8(KDRV_CW_EN_TMX_INT,    CW_INT_CONF,  0x40U));
    TEST_ASSERT_EQUAL_INT32(1, get8(KDRV_CW_EN_TMN_INT,    CW_INT_CONF,  0x10U));
    TEST_ASSERT_EQUAL_INT32(5, get8(KDRV_CW_INT_FLAGS,     CW_INT_CONF,  0x75U));
    TEST_ASSERT_EQUAL_INT32(1, get8(KDRV_CW_UPDATE_FLAG,   CW_SOC_ALERT, 0x94U));
    TEST_ASSERT_EQUAL_INT32(20, get8(KDRV_CW_SOC_ALERT_PCT, CW_SOC_ALERT, 0x94U));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("a failed read is an error and leaves the value alone",
          "[kdrv_cw2217]")
{
    int32_t v = 12345;

    Mockkhal_i2c_Init();
    bring_up();

    expect_r8_fail(CW_SOH);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_cw2217_get(KDRV_CW_SOH_PCT, &v));
    TEST_ASSERT_EQUAL_INT32(12345, v);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_cw2217_get(KDRV_CW_SOH_PCT, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_cw2217_get(KDRV_CW_FIELD_MAX, &v));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* ---- Writing ------------------------------------------------------------- */

/* INT_CONF holds three enables and three flags. Enabling one alarm must not
 * clear the others, and clearing the flags must not disable the alarms. */
TEST_CASE("alarm bits are written with a mask and read back", "[kdrv_cw2217]")
{
    uint8_t h[2];

    Mockkhal_i2c_Init();
    bring_up();

    expect_r8(CW_INT_CONF, 0x40U, &h[0]);
    expect_w8(CW_INT_CONF, 0x60U);
    expect_r8(CW_INT_CONF, 0x60U, &h[1]);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cw2217_set(KDRV_CW_EN_TMX_INT, 1));

    /* Flags pending on every source: writing 0 clears them, enables stay. */
    expect_r8(CW_INT_CONF, 0x67U, &h[0]);
    expect_w8(CW_INT_CONF, 0x60U);
    expect_r8(CW_INT_CONF, 0x60U, &h[1]);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cw2217_set(KDRV_CW_INT_FLAGS, 0));

    /* The threshold shares its register with UPDATE_FLAG, which must stay:
     * 0x94 is a loaded profile and a 20 % alarm; 10 % keeps the profile. */
    expect_r8(CW_SOC_ALERT, 0x94U, &h[0]);
    expect_w8(CW_SOC_ALERT, 0x8AU);
    expect_r8(CW_SOC_ALERT, 0x8AU, &h[1]);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cw2217_set(KDRV_CW_SOC_ALERT_PCT, 10));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("whole-register fields are written and read back", "[kdrv_cw2217]")
{
    uint8_t h;

    Mockkhal_i2c_Init();
    bring_up();

    expect_w8(CW_TEMP_MAX, 0xBEU);                 /* 55 C = 110 half degrees */
    expect_r8(CW_TEMP_MAX, 0xBEU, &h);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cw2217_set(KDRV_CW_TEMP_MAX_C2, 110));

    expect_w8(CW_TEMP_MIN, 0x50U);                 /* 0 C */
    expect_r8(CW_TEMP_MIN, 0x50U, &h);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cw2217_set(KDRV_CW_TEMP_MIN_C2, 0));

    expect_w8(CW_USER_CONF, 0x5AU);
    expect_r8(CW_USER_CONF, 0x5AU, &h);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cw2217_set(KDRV_CW_USER_CONF, 0x5A));

    /* A byte the chip did not keep. */
    expect_w8(CW_USER_CONF, 0x5AU);
    expect_r8(CW_USER_CONF, 0x00U, &h);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_cw2217_set(KDRV_CW_USER_CONF, 0x5A));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* The algorithm ignores a T_HOST whose low byte equals the previous one, so
 * a host feeding the same temperature every second would be feeding nothing.
 * The driver alternates the low byte's last bit and does not read it back. */
TEST_CASE("the host temperature's low byte never repeats", "[kdrv_cw2217]")
{
    Mockkhal_i2c_Init();
    bring_up();

    expect_w8(CW_T_HOST_H, 130U);                  /* 25 C */
    expect_w8(CW_T_HOST_L, 0x00U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cw2217_set(KDRV_CW_T_HOST_C2, 50));

    expect_w8(CW_T_HOST_H, 130U);                  /* the same again */
    expect_w8(CW_T_HOST_L, 0x01U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cw2217_set(KDRV_CW_T_HOST_C2, 50));

    expect_w8(CW_T_HOST_H, 140U);                  /* 30 C */
    expect_w8(CW_T_HOST_L, 0x00U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cw2217_set(KDRV_CW_T_HOST_C2, 60));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("a measurement, a status or a bad value is refused without a write",
          "[kdrv_cw2217]")
{
    Mockkhal_i2c_Init();
    bring_up();

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_cw2217_set(KDRV_CW_VCELL_MV, 3000));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_cw2217_set(KDRV_CW_READY, 1));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_cw2217_set(KDRV_CW_UPDATE_FLAG, 1));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_cw2217_set(KDRV_CW_SOC_ALERT_PCT, 128));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_cw2217_set(KDRV_CW_TEMP_MAX_C2, 200));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_cw2217_set(KDRV_CW_TEMP_MAX_C2, -81));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_cw2217_set(KDRV_CW_EN_SOC_INT, 2));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_cw2217_set(KDRV_CW_USER_CONF, 256));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_cw2217_set(KDRV_CW_FIELD_MAX, 0));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* ---- The profile --------------------------------------------------------- */

/** @brief The 80 profile bytes, distinct from each other and from zero. */
static void fill_profile(uint8_t *p)
{
    for ( uint32_t i = 0U; KDRV_CW2217_PROFILE_LEN > i; ++i )
    {
        p[i] = (uint8_t)(0x5AU ^ i);
    }
}

/** @brief The bus side of a profile load up to the readiness poll. */
static void expect_profile_write(const uint8_t *p, uint8_t alert)
{
    expect_w8(CW_CONFIG, 0x30U);
    expect_w8(CW_CONFIG, 0xF0U);
    for ( uint32_t i = 0U; KDRV_CW2217_PROFILE_LEN > i; ++i )
    {
        expect_w8((uint8_t)(CW_PROFILE + i), p[i]);
    }
    expect_w8(CW_SOC_ALERT, (uint8_t)(0x80U | alert));
    expect_w8(CW_INT_CONF, 0x00U);
    expect_w8(CW_CONFIG, 0x30U);
    expect_w8(CW_CONFIG, 0x00U);
}

/* The reference driver's sequence, with the two details the bench added: the
 * alert threshold goes in with UPDATE_FLAG rather than being wiped by it,
 * and the bytes are read back, because a profile that did not stick looks
 * exactly like a profile that did not help. */
TEST_CASE("a profile is loaded the vendor's way and read back byte for byte",
          "[kdrv_cw2217]")
{
    uint8_t p[KDRV_CW2217_PROFILE_LEN];
    uint8_t h[KDRV_CW2217_PROFILE_LEN + 4U];

    Mockkhal_i2c_Init();
    bring_up();
    fill_profile(p);

    expect_profile_write(p, 20U);
    expect_r8(CW_IC_STATE, 0x00U, &h[0]);
    expect_r8(CW_IC_STATE, 0x0CU, &h[1]);
    for ( uint32_t i = 0U; KDRV_CW2217_PROFILE_LEN > i; ++i )
    {
        expect_r8((uint8_t)(CW_PROFILE + i), p[i], &h[2U + i]);
    }
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cw2217_profile_load(p, 20U));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("a profile byte that did not stick fails the load", "[kdrv_cw2217]")
{
    uint8_t p[KDRV_CW2217_PROFILE_LEN];
    uint8_t h[KDRV_CW2217_PROFILE_LEN + 4U];

    Mockkhal_i2c_Init();
    bring_up();
    fill_profile(p);

    expect_profile_write(p, 20U);
    expect_r8(CW_IC_STATE, 0x0CU, &h[0]);
    for ( uint32_t i = 0U; 5U >= i; ++i )
    {
        uint8_t back = p[i];
        if ( 5U == i )
        {
            back = (uint8_t)(p[i] ^ 0xFFU);   /* the driver stops here */
        }
        expect_r8((uint8_t)(CW_PROFILE + i), back, &h[1U + i]);
    }
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_cw2217_profile_load(p, 20U));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* The bench saw 1100 ms; the driver waits up to five seconds. A chip that is
 * still not up after that is not going to be. */
TEST_CASE("a profile load gives up when the algorithm never comes up",
          "[kdrv_cw2217]")
{
    uint8_t p[KDRV_CW2217_PROFILE_LEN];
    uint8_t h[50];

    Mockkhal_i2c_Init();
    bring_up();
    fill_profile(p);

    expect_profile_write(p, 20U);
    for ( uint32_t i = 0U; 50U > i; ++i )
    {
        expect_r8(CW_IC_STATE, 0x00U, &h[i]);
    }
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kdrv_cw2217_profile_load(p, 20U));

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_cw2217_profile_load(NULL, 20U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_cw2217_profile_load(p, 128U));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("the profile reads back one byte at a time", "[kdrv_cw2217]")
{
    uint8_t p[KDRV_CW2217_PROFILE_LEN];
    uint8_t h[KDRV_CW2217_PROFILE_LEN];
    uint8_t out[KDRV_CW2217_PROFILE_LEN];

    Mockkhal_i2c_Init();
    bring_up();
    fill_profile(p);

    for ( uint32_t i = 0U; KDRV_CW2217_PROFILE_LEN > i; ++i )
    {
        expect_r8((uint8_t)(CW_PROFILE + i), p[i], &h[i]);
    }
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cw2217_profile_read(out));
    TEST_ASSERT_EQUAL_MEMORY(p, out, KDRV_CW2217_PROFILE_LEN);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_cw2217_profile_read(NULL));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* ---- The dump ------------------------------------------------------------ */

/* Every field in table order, then CONFIG raw, then the profile block. */
TEST_CASE("the dump reads every field, the config byte and the profile",
          "[kdrv_cw2217]")
{
    uint8_t h[64];
    uint8_t hp[KDRV_CW2217_PROFILE_LEN];
    bool    decoded = false;
    bool    profile = false;

    Mockkhal_i2c_Init();
    bring_up();
    fake_klog_reset();

    expect_r8(CW_VERSION, 0xA0U, &h[0]);
    expect_r16(CW_VCELL_H, 0x2E80U, &h[1]);
    expect_r16(CW_SOC_H, 0xFEB3U, &h[3]);
    expect_r8(CW_TEMP, 0x00U, &h[5]);
    expect_r8(CW_INT_CONF, 0x40U, &h[6]);       /* en_soc_int   */
    expect_r8(CW_INT_CONF, 0x40U, &h[7]);       /* en_tmx_int   */
    expect_r8(CW_INT_CONF, 0x40U, &h[8]);       /* en_tmn_int   */
    expect_r8(CW_INT_CONF, 0x40U, &h[9]);       /* int_flags    */
    expect_r8(CW_SOC_ALERT, 0x14U, &h[10]);     /* update_flag  */
    expect_r8(CW_SOC_ALERT, 0x14U, &h[11]);     /* soc_alert    */
    expect_r8(CW_TEMP_MAX, 0xAAU, &h[12]);
    expect_r8(CW_TEMP_MIN, 0x50U, &h[13]);
    expect_r16(CW_CURRENT_H, 0xF9D0U, &h[14]);
    expect_r16(CW_T_HOST_H, 0x0000U, &h[16]);
    expect_r8(CW_USER_CONF, 0x00U, &h[18]);
    expect_r16(CW_CYCLECNT_H, 0x0000U, &h[19]);
    expect_r8(CW_SOH, 0x00U, &h[21]);
    expect_r8(CW_IC_STATE, 0x0CU, &h[22]);
    expect_r16(CW_AVE_CUR_H, 0x0000U, &h[23]);
    expect_r8(CW_FW_VERSION, 0x4AU, &h[25]);
    expect_r8(CW_CONFIG, 0x00U, &h[26]);
    for ( uint32_t i = 0U; KDRV_CW2217_PROFILE_LEN > i; ++i )
    {
        expect_r8((uint8_t)(CW_PROFILE + i), 0x00U, &hp[i]);
    }
    kdrv_cw2217_dump();

    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, "vcell_mv=3720") != NULL) )
        {
            decoded = true;
        }
        if ( (NULL != l) && (strstr(l, "profile") != NULL) )
        {
            profile = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(decoded, "the dump did not decode the fields");
    TEST_ASSERT_TRUE_MESSAGE(profile, "the dump did not show the profile");

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* ---- Names --------------------------------------------------------------- */

TEST_CASE("every gauge field's name looks itself up", "[kdrv_cw2217]")
{
    for ( uint32_t i = 0U; (uint32_t)KDRV_CW_FIELD_MAX > i; ++i )
    {
        kdrv_cw2217_field_t              f = KDRV_CW_FIELD_MAX;
        const kdrv_cw2217_field_info_t * const info =
            kdrv_cw2217_field_info((kdrv_cw2217_field_t)i);

        TEST_ASSERT_NOT_NULL(info);
        TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_cw2217_field_by_name(info->name, &f));
        TEST_ASSERT_EQUAL_UINT32(i, (uint32_t)f);
    }
    TEST_ASSERT_NULL(kdrv_cw2217_field_info(KDRV_CW_FIELD_MAX));
}

TEST_CASE("a gauge field's info states its range and writability",
          "[kdrv_cw2217]")
{
    const kdrv_cw2217_field_info_t *i;
    kdrv_cw2217_field_t             f;

    i = kdrv_cw2217_field_info(KDRV_CW_CURRENT_MA);
    TEST_ASSERT_EQUAL_STRING("current_ma", i->name);
    TEST_ASSERT_EQUAL_INT32(-5240, i->min);
    TEST_ASSERT_EQUAL_INT32(5240, i->max);
    TEST_ASSERT_FALSE(i->writable);

    i = kdrv_cw2217_field_info(KDRV_CW_SOC_ALERT_PCT);
    TEST_ASSERT_EQUAL_INT32(127, i->max);
    TEST_ASSERT_TRUE(i->writable);

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_cw2217_field_by_name("vcell", &f));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_cw2217_field_by_name(NULL, &f));
}
