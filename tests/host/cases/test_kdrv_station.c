/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_station.c
 * @brief The E5 station's relays and ADC, with the transfers on J3's bus mocked.
 */
#include <stdint.h>

#include "unity.h"

#include "Mockkdrv_station_i2c.h"

#include "kbsp_station.h"
#include "kdrv_ads1115.h"
#include "kdrv_pcf8574.h"

#define BYTE_FORBIDDEN_A  0xFDU   /**< R2 on, R1 at rest, the rest at rest.        */
#define BYTE_FORBIDDEN_B  0xF5U   /**< R2 on, R1 at rest, with another relay on.   */
#define BYTE_SAFE         0xFEU   /**< R1 on, R2 at rest.                          */
#define ADS_CH            1U      /**< The input a case reads.                     */
#define ADS_REG_CONV      0x00U   /**< The conversion register (datasheet 8.6.1).  */
#define ADS_REG_CONFIG    0x01U   /**< The configuration register (datasheet 8.6.3). */
#define ADS_CFG_CH1_HI    0xD3U   /**< 0xD383: start, AIN1 to GND, +-4.096 V, single shot: high byte. */
#define ADS_CFG_CH1_LO    0x83U   /**< 128 SPS, comparator off: low byte.          */
#define ADS_HALF_SCALE_HI 0x40U   /**< 0x4000, half of full scale: its high byte.  */
#define ADS_HALF_SCALE_LO 0x00U   /**< Its low byte.                               */
#define ADS_HALF_SCALE_MV 2048    /**< Half of +-4.096 V, in mV.                   */
#define ADS_CONV_BYTES    2U      /**< A conversion's bytes.                       */

/** @brief Mock up for one case. */
static void station_fresh(void)
{
    Mockkdrv_station_i2c_Init();
}

/** @brief Every expected transfer happened; the mock goes down. */
static void station_done(void)
{
    Mockkdrv_station_i2c_Verify();
    Mockkdrv_station_i2c_Destroy();
}

TEST_CASE("test_pcf8574_refuses_r2_without_r1", "[kdrv_station]")
{
    station_fresh();
    /* No transfer is expected: one would fail the case at Verify. */
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_pcf8574_write(BYTE_FORBIDDEN_A));
    TEST_ASSERT_EQUAL(KODE_DENIED, kdrv_pcf8574_write(BYTE_FORBIDDEN_B));
    station_done();
}

TEST_CASE("test_pcf8574_writes_a_safe_byte", "[kdrv_station]")
{
    const uint8_t want = BYTE_SAFE;

    station_fresh();
    kdrv_station_i2c_write_ExpectWithArrayAndReturn(KBSP_STATION_PCF8574_ADDR, &want, 1U, 1U,
                                                    KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_pcf8574_write(BYTE_SAFE));
    station_done();
}

TEST_CASE("test_ads1115_converts_counts_to_mv", "[kdrv_station]")
{
    const uint8_t config[] = { ADS_REG_CONFIG, ADS_CFG_CH1_HI, ADS_CFG_CH1_LO };
    const uint8_t reg      = ADS_REG_CONV;
    uint8_t       counts[] = { ADS_HALF_SCALE_HI, ADS_HALF_SCALE_LO };
    int32_t       mv       = 0;

    station_fresh();
    kdrv_station_i2c_write_ExpectWithArrayAndReturn(KBSP_STATION_ADS1115_ADDR, config,
                                                    sizeof(config), sizeof(config), KODE_OK);
    kdrv_station_i2c_write_read_ExpectWithArrayAndReturn(KBSP_STATION_ADS1115_ADDR, &reg, 1U, 1U,
                                                         counts, ADS_CONV_BYTES, ADS_CONV_BYTES,
                                                         KODE_OK);
    kdrv_station_i2c_write_read_IgnoreArg_rx();
    kdrv_station_i2c_write_read_ReturnArrayThruPtr_rx(counts, ADS_CONV_BYTES);
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_ads1115_read_mv(ADS_CH, &mv));
    TEST_ASSERT_EQUAL_INT32(ADS_HALF_SCALE_MV, mv);
    station_done();
}

TEST_CASE("test_station_present_needs_both", "[kdrv_station]")
{
    static const bool answers[][2] = {
        { true, false },
        { false, true },
        { false, false },
    };

    station_fresh();
    for ( uint32_t i = 0U; (sizeof(answers) / sizeof(answers[0])) > i; ++i )
    {
        kdrv_station_i2c_probe_ExpectAndReturn(KBSP_STATION_PCF8574_ADDR, answers[i][0]);
        kdrv_station_i2c_probe_ExpectAndReturn(KBSP_STATION_ADS1115_ADDR, answers[i][1]);
        TEST_ASSERT_FALSE(kdrv_station_present());
    }
    kdrv_station_i2c_probe_ExpectAndReturn(KBSP_STATION_PCF8574_ADDR, true);
    kdrv_station_i2c_probe_ExpectAndReturn(KBSP_STATION_ADS1115_ADDR, true);
    TEST_ASSERT_TRUE(kdrv_station_present());
    station_done();
}
