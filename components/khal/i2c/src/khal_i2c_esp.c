/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_i2c_esp.c
 * @brief khal_i2c on ESP-IDF's I2C master driver.
 */
#include "khal_i2c.h"

#include <string.h>

#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_rom_sys.h"

#include "kbsp_board.h"
#include "kbsp_pins.h"
#include "kode_log.h"
#include "soc/soc_caps.h"

_Static_assert(KBSP_I2C_BOARD < (uint32_t)SOC_HP_I2C_NUM,
               "KBSP_I2C_BOARD must name one of the chip's HP I2C controllers");

#define TAG               "khal.i2c"
#define I2C_TIMEOUT_MS    100     /**< Longest a transaction may take. */
#define I2C_GLITCH_CYCLES 7U      /**< Pulses shorter than this many clock cycles are ignored. */
#define I2C_PROBE_ANSWERS 2U      /**< Probes that must all answer for an address to count. */

#define I2C_FREE_PULSES   18U     /**< Clocks given to a device holding SDA: nine are the rule, twice to be sure. */
#define I2C_FREE_HALF_US  5U      /**< Half a clock of them: 100 kHz. */

static i2c_master_bus_handle_t s_bus;

/**
 * @brief Free a bus a device was left holding, before the driver takes it.
 *
 * A reset of this chip does not reset the devices: on the battery they stay up
 * and one left in the middle of a read goes on holding SDA low for clocks that
 * never come, and then nothing on the bus answers until the battery is taken
 * off. Clocks by hand end its byte, and a STOP ends the transaction. The levels
 * are said, for a bus this does not free: SCL low or both low is no device
 * waiting for clocks, but a short or a rail that is down.
 */
static void i2c_bus_free(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << KBSP_PIN_I2C_SDA) | (1ULL << KBSP_PIN_I2C_SCL),
        .mode         = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    if ( ESP_OK != gpio_config(&io) )
    {
        return;
    }
    (void)gpio_set_level(KBSP_PIN_I2C_SDA, 1);
    (void)gpio_set_level(KBSP_PIN_I2C_SCL, 1);
    esp_rom_delay_us(50U);

    int const sda0 = gpio_get_level(KBSP_PIN_I2C_SDA);
    int const scl0 = gpio_get_level(KBSP_PIN_I2C_SCL);

    if ( (0 == sda0) && (0 != scl0) )
    {
        uint32_t n = 0U;

        while ( (I2C_FREE_PULSES > n) && (0 == gpio_get_level(KBSP_PIN_I2C_SDA)) )
        {
            (void)gpio_set_level(KBSP_PIN_I2C_SCL, 0);
            esp_rom_delay_us(I2C_FREE_HALF_US);
            (void)gpio_set_level(KBSP_PIN_I2C_SCL, 1);
            esp_rom_delay_us(I2C_FREE_HALF_US);
            ++n;
        }
        /* A STOP: SDA up while SCL is up. */
        (void)gpio_set_level(KBSP_PIN_I2C_SCL, 0);
        esp_rom_delay_us(I2C_FREE_HALF_US);
        (void)gpio_set_level(KBSP_PIN_I2C_SDA, 0);
        esp_rom_delay_us(I2C_FREE_HALF_US);
        (void)gpio_set_level(KBSP_PIN_I2C_SCL, 1);
        esp_rom_delay_us(I2C_FREE_HALF_US);
        (void)gpio_set_level(KBSP_PIN_I2C_SDA, 1);
        esp_rom_delay_us(I2C_FREE_HALF_US);
        KLOG_W(TAG, "SDA was held low: %u clocks given, SDA now %d", (unsigned)n,
               gpio_get_level(KBSP_PIN_I2C_SDA));
    }
    else if ( (0 == sda0) || (0 == scl0) )
    {
        KLOG_E(TAG, "the bus is down: SDA %d, SCL %d (a short, or its rail is off)", sda0, scl0);
    }
    else
    {
        KLOG_D(TAG, "the bus is free: SDA %d, SCL %d", sda0, scl0);
    }
    /* Given back: driven by hand they are ESP-IDF's reserved outputs, and the
     * I2C driver would take them saying "GPIO 4 is not usable". The reset
     * leaves the pull-ups up, so the bus stays idle until the driver has it. */
    (void)gpio_reset_pin(KBSP_PIN_I2C_SDA);
    (void)gpio_reset_pin(KBSP_PIN_I2C_SCL);
}

kode_status_t khal_i2c_init(void)
{
    /* No lock: the first call runs in an AO's initial transition, and QP runs
     * those one after another on kode_init()'s task. */
    kode_status_t st = KODE_OK;
    if ( NULL == s_bus )
    {
        const i2c_master_bus_config_t cfg = {
            .i2c_port                     = (i2c_port_num_t)KBSP_I2C_BOARD,
            .sda_io_num                   = KBSP_PIN_I2C_SDA,
            .scl_io_num                   = KBSP_PIN_I2C_SCL,
            .clk_source                   = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt            = I2C_GLITCH_CYCLES,
            .flags.enable_internal_pullup = true,
        };

        i2c_bus_free();

        esp_err_t const err = i2c_new_master_bus(&cfg, &s_bus);
        if ( ESP_OK == err )
        {
            KLOG_D(TAG, "I2C0 up (SDA=%u SCL=%u @%luHz)",
                   (unsigned)KBSP_PIN_I2C_SDA, (unsigned)KBSP_PIN_I2C_SCL,
                   (unsigned long)KBSP_I2C_FREQ_HZ);
        }
        else
        {
            s_bus = NULL;
            st = KODE_ERROR;
            KLOG_E(TAG, "i2c_new_master_bus failed (%s)", esp_err_to_name(err));
        }
    }
    return st;
}

i2c_master_bus_handle_t khal_i2c_bus(void)
{
    return s_bus;
}

bool khal_i2c_probe(uint16_t addr, uint32_t timeout_ms)
{
    /* One probe can answer ESP_OK for an address nobody holds: after a
     * NACK, IDF's master driver can take the late completion of the transfer
     * before for this one's (espressif/esp-idf#13547). Measured on the Kode
     * Dot at about one empty address in a hundred; twice running, about one
     * in ten thousand. So an address counts only when it answers each time. */
    bool found = ( NULL != s_bus );
    for ( uint32_t i = 0U; found && (I2C_PROBE_ANSWERS > i); ++i )
    {
        found = ( ESP_OK == i2c_master_probe(s_bus, addr, (int)timeout_ms) );
    }
    return found;
}

kode_status_t khal_i2c_add(uint16_t addr, khal_i2c_dev_t *out)
{
    if ( (NULL == s_bus) || (NULL == out) )
    {
        return KODE_ERROR;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = KBSP_I2C_FREQ_HZ,
    };
    esp_err_t const err = i2c_master_bus_add_device(s_bus, &cfg, out);
    if ( ESP_OK != err )
    {
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t khal_i2c_remove(khal_i2c_dev_t dev)
{
    if ( NULL == dev )
    {
        return KODE_OK;
    }
    if ( i2c_master_bus_rm_device(dev) != ESP_OK )
    {
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t khal_i2c_write_reg(khal_i2c_dev_t dev, uint8_t reg, uint8_t val)
{
    uint8_t const buf[2] = { reg, val };
    esp_err_t const err = i2c_master_transmit(dev, buf, sizeof(buf),
                                              I2C_TIMEOUT_MS);
    if ( ESP_OK != err )
    {
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t khal_i2c_read_reg(khal_i2c_dev_t dev, uint8_t reg,
                                uint8_t *buf, size_t len)
{
    esp_err_t const err = i2c_master_transmit_receive(dev, &reg, 1U, buf, len,
                                                      I2C_TIMEOUT_MS);
    if ( ESP_OK != err )
    {
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t khal_i2c_write_regs(khal_i2c_dev_t dev, uint8_t reg,
                                  const uint8_t *buf, size_t len)
{
    uint8_t frame[1U + KHAL_I2C_BURST_MAX];

    if ( (NULL == buf) || (0U == len) || (KHAL_I2C_BURST_MAX < len) )
    {
        return KODE_DENIED;
    }
    frame[0] = reg;
    (void)memcpy(&frame[1], buf, len);
    if ( i2c_master_transmit(dev, frame, len + 1U, I2C_TIMEOUT_MS) != ESP_OK )
    {
        return KODE_ERROR;
    }
    return KODE_OK;
}
