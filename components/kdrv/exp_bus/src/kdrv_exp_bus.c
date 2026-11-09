/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_exp_bus.c
 * @brief J3's buses on ESP-IDF: IDF's I2C master, SPI master and UART drivers.
 */
#include "kdrv_exp_bus.h"

#include <limits.h>
#include <stdbool.h>

#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "kbsp_board.h"
#include "kode_log.h"
#include "soc/soc_caps.h"

#define TAG "kdrv.exp_bus"

#define EXP_I2C_GLITCH_CYCLES  7U  /**< Pulses shorter than this many clock cycles are ignored. */
#define EXP_I2C_PROBE_ANSWERS  2U  /**< Probes that must all answer for an address to count.    */
#define EXP_SPI_QUEUE          1U  /**< Transactions the device's queue holds: polling uses none, IDF wants one. */
#define EXP_UART_TX_BYTES      0   /**< The UART's transmit ring: none, a write waits for the FIFO.   */
#define EXP_UART_EVENTS        0   /**< The UART's event queue: none, nobody listens for its events.  */
#define EXP_UART_INTR_FLAGS    0   /**< Interrupt flags: IDF adds ESP_INTR_FLAG_IRAM itself under CONFIG_UART_ISR_IN_IRAM. */

_Static_assert(KDRV_EXP_SPI_XFER_MAX == (uint32_t)SOC_SPI_MAXIMUM_BUFFER_SIZE,
               "one SPI transfer with no DMA is the controller's buffer");
_Static_assert(KDRV_EXP_UART_RX_BYTES > (uint32_t)SOC_UART_FIFO_LEN,
               "IDF wants a receive ring longer than the UART's FIFO");

static i2c_master_bus_handle_t s_i2c_bus;       /**< The open bus; NULL while none is.        */
static i2c_master_dev_handle_t s_i2c_dev;       /**< The one device, added at the first transfer. */
static uint32_t                s_i2c_hz;        /**< The clock the bus was opened at.          */
static spi_device_handle_t     s_spi_dev;       /**< The SPI bus's one device; NULL while no bus is open. */
static spi_host_device_t       s_spi_host;      /**< The host the SPI bus is open on.          */
static bool                    s_uart_open;     /**< Whether the UART is open.                 */
static uart_port_t             s_uart_port;     /**< The port it is open on.                   */

/**
 * @brief What an IDF answer to a transfer means here.
 *
 * A synchronous transfer ends in ESP_ERR_INVALID_RESPONSE whenever it does not
 * finish DONE, for a missing acknowledge and for the software and hardware
 * timeouts alike, so one code covers both.
 * @param err The driver's answer.
 * @return KODE_OK, KODE_IO_ERROR when the transfer did not complete, or KODE_ERROR.
 */
static kode_status_t i2c_xfer_status(esp_err_t err)
{
    kode_status_t st = KODE_ERROR;

    if ( ESP_OK == err )
    {
        st = KODE_OK;
    }
    else if ( ESP_ERR_INVALID_RESPONSE == err )
    {
        st = KODE_IO_ERROR;
    }
    else
    {
        st = KODE_ERROR;
    }
    return st;
}

/**
 * @brief What an IDF answer to a probe means here: a probe names the acknowledge and the timeout apart.
 * @param err The driver's answer.
 * @return KODE_OK, KODE_IO_ERROR for no acknowledge, KODE_TIMEOUT for a stuck bus, or KODE_ERROR.
 */
static kode_status_t i2c_probe_status(esp_err_t err)
{
    kode_status_t st = KODE_ERROR;

    if ( ESP_OK == err )
    {
        st = KODE_OK;
    }
    else if ( ESP_ERR_NOT_FOUND == err )
    {
        st = KODE_IO_ERROR;
    }
    else if ( ESP_ERR_TIMEOUT == err )
    {
        st = KODE_TIMEOUT;
    }
    else
    {
        st = KODE_ERROR;
    }
    return st;
}

kode_status_t kdrv_exp_i2c_open(uint8_t ctrl, uint32_t sda_gpio, uint32_t scl_gpio, uint32_t hz)
{
    kode_status_t st = KODE_OK;

    if ( (NULL != s_i2c_bus) || ((uint8_t)KBSP_I2C_BOARD == ctrl) || ((uint8_t)SOC_HP_I2C_NUM <= ctrl)
        || (0U == hz) )
    {
        KLOG_W(TAG, "i2c: refused I2C%u (a bus open, the board's own, or no such controller)",
               (unsigned)ctrl);
        st = KODE_DENIED;
    }
    else
    {
        const i2c_master_bus_config_t cfg = {
            .i2c_port                     = (i2c_port_num_t)ctrl,
            .sda_io_num                   = (gpio_num_t)sda_gpio,
            .scl_io_num                   = (gpio_num_t)scl_gpio,
            .clk_source                   = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt            = EXP_I2C_GLITCH_CYCLES,
            .flags.enable_internal_pullup = true,
        };
        esp_err_t const err = i2c_new_master_bus(&cfg, &s_i2c_bus);

        if ( ESP_OK == err )
        {
            s_i2c_hz = hz;
            KLOG_I(TAG, "I2C%u up (SDA=%lu SCL=%lu @%luHz)", (unsigned)ctrl,
                   (unsigned long)sda_gpio, (unsigned long)scl_gpio, (unsigned long)hz);
        }
        else
        {
            s_i2c_bus = NULL;
            st        = KODE_ERROR;
            KLOG_W(TAG, "i2c_new_master_bus failed (%s)", esp_err_to_name(err));
        }
    }
    return st;
}

/**
 * @brief Point the one device at @p addr, adding it on the first transfer.
 * @param addr The 7-bit address.
 * @param ms   How long a change of address may wait for the bus.
 * @return ESP_OK, or what the driver answered.
 */
static esp_err_t i2c_dev_at(uint8_t addr, uint32_t ms)
{
    esp_err_t err = ESP_OK;

    if ( NULL == s_i2c_dev )
    {
        const i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = addr,
            .scl_speed_hz    = s_i2c_hz,
        };
        err = i2c_master_bus_add_device(s_i2c_bus, &cfg, &s_i2c_dev);
        if ( ESP_OK != err )
        {
            s_i2c_dev = NULL;
        }
    }
    else
    {
        err = i2c_master_device_change_address(s_i2c_dev, addr, (int)ms);
    }
    return err;
}

kode_status_t kdrv_exp_i2c_xfer(uint8_t addr, const uint8_t *out, size_t out_len, uint8_t *in,
                                size_t in_len, uint32_t ms)
{
    bool const    bad = (NULL == s_i2c_bus) || (KDRV_EXP_I2C_ADDR_MAX < addr)
                     || ((0U == out_len) && (0U == in_len)) || ((0U < out_len) && (NULL == out))
                     || ((0U < in_len) && (NULL == in));
    kode_status_t st  = KODE_DENIED;

    if ( !bad )
    {
        esp_err_t err = i2c_dev_at(addr, ms);

        if ( ESP_OK == err )
        {
            if ( (0U < out_len) && (0U < in_len) )
            {
                err = i2c_master_transmit_receive(s_i2c_dev, out, out_len, in, in_len, (int)ms);
            }
            else if ( 0U < out_len )
            {
                err = i2c_master_transmit(s_i2c_dev, out, out_len, (int)ms);
            }
            else
            {
                err = i2c_master_receive(s_i2c_dev, in, in_len, (int)ms);
            }
        }
        st = i2c_xfer_status(err);
    }
    return st;
}

kode_status_t kdrv_exp_i2c_probe(uint8_t addr)
{
    /* One probe can answer ESP_OK for an address nobody holds: after a NACK,
     * IDF's master driver can take the late completion of the transfer before
     * for this one's (espressif/esp-idf#13547). Measured on the Kode Dot's own
     * bus at about one empty address in a hundred; twice running, about one in
     * ten thousand. So an address counts only when it answers each time. */
    kode_status_t st = KODE_DENIED;

    if ( (NULL != s_i2c_bus) && (KDRV_EXP_I2C_ADDR_MAX >= addr) )
    {
        st = KODE_OK;
        for ( uint32_t i = 0U; (KODE_OK == st) && (EXP_I2C_PROBE_ANSWERS > i); ++i )
        {
            st = i2c_probe_status(i2c_master_probe(s_i2c_bus, addr, (int)KDRV_EXP_I2C_PROBE_MS));
        }
    }
    return st;
}

kode_status_t kdrv_exp_i2c_close(void)
{
    kode_status_t st = KODE_OK;

    if ( NULL != s_i2c_dev )
    {
        if ( i2c_master_bus_rm_device(s_i2c_dev) != ESP_OK )
        {
            st = KODE_ERROR;
        }
        s_i2c_dev = NULL;
    }
    if ( NULL != s_i2c_bus )
    {
        if ( i2c_del_master_bus(s_i2c_bus) != ESP_OK )
        {
            st = KODE_ERROR;
        }
        s_i2c_bus = NULL;
        KLOG_I(TAG, "I2C down");
    }
    if ( KODE_OK != st )
    {
        KLOG_W(TAG, "i2c: the driver refused to close; the bus is forgotten");
    }
    return st;
}

kode_status_t kdrv_exp_spi_open(uint8_t host, uint32_t sck, uint32_t mosi, uint32_t miso,
                                uint32_t cs, uint32_t hz, uint8_t mode)
{
    kode_status_t st = KODE_OK;

    if ( (NULL != s_spi_dev) || ((uint8_t)SPI1_HOST == host) || ((uint8_t)KBSP_SPI_NFC == host)
        || ((uint8_t)SOC_SPI_PERIPH_NUM <= host) || (KDRV_EXP_SPI_HZ_MIN > hz)
        || (KDRV_EXP_SPI_MODE_MAX < mode) )
    {
        KLOG_W(TAG, "spi: refused host %u at %lu Hz, mode %u (a bus open, the flash's or the "
               "board's, no such host, too slow a clock or no such mode)", (unsigned)host,
               (unsigned long)hz, (unsigned)mode);
        st = KODE_DENIED;
    }
    else
    {
        const spi_bus_config_t bus = {
            .mosi_io_num     = (int)mosi,
            .miso_io_num     = (int)miso,
            .sclk_io_num     = (int)sck,
            .quadwp_io_num   = (int)GPIO_NUM_NC,
            .quadhd_io_num   = (int)GPIO_NUM_NC,
            .max_transfer_sz = (int)KDRV_EXP_SPI_XFER_MAX,
        };
        const spi_device_interface_config_t dev = {
            .mode           = mode,
            .clock_speed_hz = (int)hz,
            .spics_io_num   = (int)cs,
            .queue_size     = (int)EXP_SPI_QUEUE,
        };
        spi_host_device_t const id  = (spi_host_device_t)host;
        esp_err_t               err = spi_bus_initialize(id, &bus, SPI_DMA_DISABLED);

        if ( ESP_OK == err )
        {
            err = spi_bus_add_device(id, &dev, &s_spi_dev);
            if ( ESP_OK != err )
            {
                s_spi_dev = NULL;
                (void)spi_bus_free(id);
            }
        }
        if ( ESP_OK == err )
        {
            s_spi_host = id;
            KLOG_I(TAG, "SPI host %u up (SCK=%lu MOSI=%lu MISO=%lu CS=%lu @%luHz mode %u)",
                   (unsigned)host, (unsigned long)sck, (unsigned long)mosi, (unsigned long)miso,
                   (unsigned long)cs, (unsigned long)hz, (unsigned)mode);
        }
        else
        {
            st = KODE_ERROR;
            KLOG_W(TAG, "spi: the bus would not open (%s)", esp_err_to_name(err));
        }
    }
    return st;
}

kode_status_t kdrv_exp_spi_xfer(const uint8_t *out, uint8_t *in, size_t len)
{
    kode_status_t st = KODE_DENIED;

    if ( (NULL != s_spi_dev) && (NULL != out) && (0U < len) && (KDRV_EXP_SPI_XFER_MAX >= len) )
    {
        spi_transaction_t t = {
            .length    = len * (size_t)CHAR_BIT,
            .rxlength  = 0U,
            .tx_buffer = out,
            .rx_buffer = in,
        };
        /* rxlength 0 is "as long as length" in full duplex; a NULL rx_buffer
         * leaves the data-in line out of the transfer. */
        if ( spi_device_polling_transmit(s_spi_dev, &t) == ESP_OK )
        {
            st = KODE_OK;
        }
        else
        {
            st = KODE_ERROR;
        }
    }
    return st;
}

kode_status_t kdrv_exp_spi_close(void)
{
    kode_status_t st = KODE_OK;

    if ( NULL != s_spi_dev )
    {
        if ( spi_bus_remove_device(s_spi_dev) != ESP_OK )
        {
            st = KODE_ERROR;
        }
        if ( spi_bus_free(s_spi_host) != ESP_OK )
        {
            st = KODE_ERROR;
        }
        s_spi_dev = NULL;
        KLOG_I(TAG, "SPI down");
    }
    if ( KODE_OK != st )
    {
        KLOG_W(TAG, "spi: the driver refused to close; the bus is forgotten");
    }
    return st;
}

/**
 * @brief Whether @p port is one an app may open: a high-power UART, and not the console's.
 * @param port The port as handed in.
 * @return true when it may.
 */
static bool uart_port_ok(uint8_t port)
{
    bool ok = (uint8_t)SOC_UART_HP_NUM > port;

#ifdef CONFIG_ESP_CONSOLE_UART
    if ( (uint8_t)CONFIG_ESP_CONSOLE_UART_NUM == port )
    {
        ok = false;
    }
#endif
    return ok;
}

kode_status_t kdrv_exp_uart_open(uint8_t port, uint32_t tx, uint32_t rx, uint32_t baud)
{
    kode_status_t st = KODE_OK;

    if ( s_uart_open || !uart_port_ok(port) || (0U == baud) )
    {
        KLOG_W(TAG, "uart: refused UART%u (one open, the console's, no such port, or no rate)",
               (unsigned)port);
        st = KODE_DENIED;
    }
    else
    {
        const uart_config_t cfg = {
            .baud_rate  = (int)baud,
            .data_bits  = UART_DATA_8_BITS,
            .parity     = UART_PARITY_DISABLE,
            .stop_bits  = UART_STOP_BITS_1,
            .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        uart_port_t const id  = (uart_port_t)port;
        esp_err_t         err = uart_driver_install(id, (int)KDRV_EXP_UART_RX_BYTES, EXP_UART_TX_BYTES,
                                                    EXP_UART_EVENTS, NULL, EXP_UART_INTR_FLAGS);

        if ( ESP_OK == err )
        {
            err = uart_param_config(id, &cfg);
            if ( ESP_OK == err )
            {
                err = uart_set_pin(id, (int)tx, (int)rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
            }
            if ( ESP_OK != err )
            {
                (void)uart_driver_delete(id);
            }
        }
        if ( ESP_OK == err )
        {
            s_uart_open = true;
            s_uart_port = id;
            KLOG_I(TAG, "UART%u up (TX=%lu RX=%lu @%lu baud)", (unsigned)port, (unsigned long)tx,
                   (unsigned long)rx, (unsigned long)baud);
        }
        else
        {
            st = KODE_ERROR;
            KLOG_W(TAG, "uart: UART%u would not open (%s)", (unsigned)port, esp_err_to_name(err));
        }
    }
    return st;
}

kode_status_t kdrv_exp_uart_write(const uint8_t *d, size_t len, size_t *sent)
{
    kode_status_t st = KODE_DENIED;

    if ( NULL != sent )
    {
        *sent = 0U;
        if ( s_uart_open && (NULL != d) && (0U < len) )
        {
            int const n = uart_write_bytes(s_uart_port, d, len);

            if ( 0 <= n )
            {
                *sent = (size_t)n;
                st    = KODE_OK;
            }
            else
            {
                st = KODE_ERROR;
            }
        }
    }
    return st;
}

kode_status_t kdrv_exp_uart_read(uint8_t *d, size_t cap, uint32_t ms, size_t *got)
{
    /* uart_read_bytes() waits its ticks again after every chunk that arrives,
     * so one call over the whole time can wait far past it on a line that
     * keeps talking. The time is kept here instead: what the ring holds is
     * taken with no wait, and only one byte is waited for at a time, for no
     * longer than what is left. */
    kode_status_t st = KODE_DENIED;

    if ( NULL != got )
    {
        *got = 0U;
        if ( s_uart_open && (NULL != d) && (0U < cap) )
        {
            TickType_t const total = pdMS_TO_TICKS(ms);
            TickType_t const start = xTaskGetTickCount();
            bool             more  = true;

            st = KODE_OK;
            while ( more )
            {
                int n = uart_read_bytes(s_uart_port, &d[*got], (uint32_t)(cap - *got), 0U);

                if ( 0 < n )
                {
                    *got += (size_t)n;
                }
                if ( 0 > n )
                {
                    st   = KODE_ERROR;
                    more = false;
                }
                else if ( *got >= cap )
                {
                    more = false;
                }
                else
                {
                    TickType_t const gone = xTaskGetTickCount() - start;

                    if ( gone >= total )
                    {
                        more = false;
                    }
                    else
                    {
                        n = uart_read_bytes(s_uart_port, &d[*got], 1U, total - gone);
                        if ( 0 < n )
                        {
                            *got += (size_t)n;
                        }
                        else if ( 0 > n )
                        {
                            st   = KODE_ERROR;
                            more = false;
                        }
                        else
                        {
                            more = false;
                        }
                    }
                }
            }
        }
    }
    return st;
}

kode_status_t kdrv_exp_uart_close(void)
{
    kode_status_t st = KODE_OK;

    if ( s_uart_open )
    {
        if ( uart_driver_delete(s_uart_port) != ESP_OK )
        {
            st = KODE_ERROR;
            KLOG_W(TAG, "uart: the driver refused to close; the UART is forgotten");
        }
        s_uart_open = false;
        KLOG_I(TAG, "UART down");
    }
    return st;
}
