/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file exp_bus_main.c
 * @brief kodeOS example: an I2C sensor on the expansion header J3, read from a task with kode.h.
 */
#include <stdbool.h>
#include <stdint.h>

#include "kode.h"
#include "kode_log.h"

#define TAG "exp_bus"

/* Positions on J3, not GPIO numbers. EXP1 to EXP3 stay free for a C5 flasher. */
/**
 * @name The bus
 * @{
 */
#define BUS_SDA_POS      ((uint8_t)4U)   /**< J3 position of the data line.  */
#define BUS_SCL_POS      ((uint8_t)5U)   /**< J3 position of the clock line. */
#define BUS_HZ           100000U         /**< The clock: I2C standard mode.  */
/** @} */

/**
 * @name The scan: the 7-bit addresses a device may have
 * @brief 0x00 to 0x07 and 0x78 to 0x7F are reserved by the I2C specification.
 * @{
 */
#define SCAN_FIRST_ADDR  ((uint8_t)0x08U)  /**< The first address probed. */
#define SCAN_LAST_ADDR   ((uint8_t)0x77U)  /**< The last address probed.  */
/** @} */

/**
 * @name The sensor: the LM75 family
 * @{
 */
#define TEMP_ADDR        ((uint8_t)0x48U)  /**< Its address with A0..A2 low.           */
#define TEMP_REG         ((uint8_t)0x00U)  /**< The temperature register.              */
#define TEMP_LEN         2U                /**< Bytes of the temperature, MSB first.   */
#define TEMP_LSB_PER_C   256               /**< Counts of the register per degree C.   */
#define TEMP_CENTI       100               /**< Hundredths in a degree, for the print. */
#define TEMP_BYTE_BITS   8U                /**< Bits the high byte is shifted by.      */
/** @} */

/** How often the temperature is read, ms. */
#define READ_PERIOD_MS   1000U

/* What the launcher shows, and which category it files this app under. */
KODE_APP_MANIFEST("J3 I2C sensor", "example");

/**
 * @brief Log every address that answers on the bus.
 *
 * kode_i2c_probe() answers false with kode_error() empty when nobody is at
 * an address, and false with a reason when the bus itself could not be
 * asked - a stuck bus, with a line held low, for one; the second ends the
 * scan.
 */
static void bus_scan(void)
{
    uint8_t addr;
    uint32_t found = 0U;

    for ( addr = SCAN_FIRST_ADDR; addr <= SCAN_LAST_ADDR; addr++ )
    {
        if ( kode_i2c_probe(addr) )
        {
            KLOG_I(TAG, "a device answers at 0x%02X", (unsigned)addr);
            found++;
        }
        else if ( kode_error()[0] != '\0' )
        {
            KLOG_W(TAG, "the scan stopped at 0x%02X: %s", (unsigned)addr,
                   kode_error());
            break;
        }
        else
        {
            /* Nobody at this address. */
        }
    }
    KLOG_I(TAG, "%u device(s) on the bus", (unsigned)found);
}

/**
 * @brief Read the sensor once and log the temperature.
 *
 * One kode_i2c_write_read(): the register's number goes out, its two bytes
 * come back, with no stop between - what a register read is on I2C.
 */
static void temp_read(void)
{
    const uint8_t reg = TEMP_REG;
    uint8_t raw[TEMP_LEN];

    if ( !kode_i2c_write_read(TEMP_ADDR, &reg, sizeof(reg), raw, sizeof(raw)) )
    {
        KLOG_W(TAG, "no reading: %s", kode_error());
    }
    else
    {
        const uint16_t word = (uint16_t)(((uint16_t)raw[0] << TEMP_BYTE_BITS) |
                                         (uint16_t)raw[1]);
        const int16_t counts = (int16_t)word;
        int32_t centi = ((int32_t)counts * TEMP_CENTI) / TEMP_LSB_PER_C;
        const char *sign = "";

        if ( centi < 0 )
        {
            sign  = "-";
            centi = -centi;
        }
        KLOG_I(TAG, "%s%ld.%02ld C", sign, (long)(centi / TEMP_CENTI),
               (long)(centi % TEMP_CENTI));
    }
}

void app_main(void)
{
    /* Brings up logging, QF, the event pools and the core Active Objects.
     * J3 has no owner to start: the bus calls drive their own driver. */
    kode_init();

    /* The bus calls wait on the wire, so they belong to a task, never to an AO.
     * With the radio built in, J3's 3.3 V needs kode_pin_power_3v3(true). */
    if ( !kode_i2c_begin(BUS_SDA_POS, BUS_SCL_POS, BUS_HZ) )
    {
        KLOG_E(TAG, "the bus did not begin: %s", kode_error());
    }
    else
    {
        KLOG_I(TAG, "I2C up on EXP%u (SDA) and EXP%u (SCL) at %u Hz",
               (unsigned)BUS_SDA_POS, (unsigned)BUS_SCL_POS, (unsigned)BUS_HZ);

        bus_scan();

        /* The press count is shared and starts at boot: the first ask clears
         * any press made before this point. */
        (void)kode_button_was_pressed("bottom");
        while ( !kode_button_was_pressed("bottom") )
        {
            temp_read();
            kode_delay(READ_PERIOD_MS);
        }

        if ( !kode_i2c_end() )
        {
            KLOG_W(TAG, "the bus did not end: %s", kode_error());
        }
        KLOG_I(TAG, "bus ended, pins back at high impedance; leaving");
        kode_exit();
    }
}
