/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_station_i2c.c
 * @brief The E5 station's transfers on J3's bus, the one the camera's probe brought up.
 */
#include "kdrv_station_i2c.h"

#include "driver/i2c_master.h"
#include "esp_err.h"

#include "kbsp_station.h"
#include "kdrv_ov5640.h"

/** @brief The three kinds of transfer, for xfer(). */
typedef enum {
    XFER_WRITE = 0,  /**< Bytes out.                       */
    XFER_READ,       /**< Bytes in.                        */
    XFER_WRITE_READ, /**< Bytes out, a repeated start, in. */
} xfer_t;

/**
 * @brief What an IDF answer to a transfer means here.
 *
 * A synchronous transfer ends in ESP_ERR_INVALID_RESPONSE whenever it does not
 * finish, for a missing acknowledge and a timeout alike.
 */
static kode_status_t xfer_status(esp_err_t err)
{
    kode_status_t st = KODE_ERROR;

    if ( ESP_OK == err )
    {
        st = KODE_OK;
    }
    else if ( (ESP_ERR_INVALID_RESPONSE == err) || (ESP_ERR_TIMEOUT == err) )
    {
        st = KODE_IO_ERROR;
    }
    else
    {
        st = KODE_ERROR;
    }
    return st;
}

/** @brief One transfer of @p kind at @p addr, its device added before and removed after. */
static kode_status_t xfer(xfer_t kind, uint8_t addr, const uint8_t *tx, size_t ntx, uint8_t *rx,
                          size_t nrx)
{
    const i2c_master_bus_handle_t bus = (i2c_master_bus_handle_t)kdrv_ov5640_bus();
    const i2c_device_config_t     dc  = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = KBSP_STATION_I2C_HZ,
    };
    i2c_master_dev_handle_t dev = NULL;
    kode_status_t           st  = KODE_OK;
    esp_err_t               err = ESP_OK;

    if ( NULL == bus )
    {
        st = KODE_DENIED;
    }
    else if ( ESP_OK != i2c_master_bus_add_device(bus, &dc, &dev) )
    {
        st = KODE_ERROR;
    }
    else
    {
        if ( XFER_WRITE == kind )
        {
            err = i2c_master_transmit(dev, tx, ntx, (int)KDRV_STATION_I2C_MS);
        }
        else if ( XFER_READ == kind )
        {
            err = i2c_master_receive(dev, rx, nrx, (int)KDRV_STATION_I2C_MS);
        }
        else
        {
            err = i2c_master_transmit_receive(dev, tx, ntx, rx, nrx, (int)KDRV_STATION_I2C_MS);
        }
        st = xfer_status(err);
        (void)i2c_master_bus_rm_device(dev);
    }
    return st;
}

kode_status_t kdrv_station_i2c_write(uint8_t addr, const uint8_t *tx, size_t n)
{
    kode_status_t st = KODE_DENIED;

    if ( NULL != tx )
    {
        st = xfer(XFER_WRITE, addr, tx, n, NULL, 0U);
    }
    return st;
}

kode_status_t kdrv_station_i2c_read(uint8_t addr, uint8_t *rx, size_t n)
{
    kode_status_t st = KODE_DENIED;

    if ( NULL != rx )
    {
        st = xfer(XFER_READ, addr, NULL, 0U, rx, n);
    }
    return st;
}

kode_status_t kdrv_station_i2c_write_read(uint8_t addr, const uint8_t *tx, size_t ntx, uint8_t *rx,
                                          size_t nrx)
{
    kode_status_t st = KODE_DENIED;

    if ( (NULL != tx) && (NULL != rx) )
    {
        st = xfer(XFER_WRITE_READ, addr, tx, ntx, rx, nrx);
    }
    return st;
}

bool kdrv_station_i2c_probe(uint8_t addr)
{
    const i2c_master_bus_handle_t bus = (i2c_master_bus_handle_t)kdrv_ov5640_bus();
    bool                          ack = false;

    if ( NULL != bus )
    {
        ack = (ESP_OK == i2c_master_probe(bus, addr, (int)KDRV_STATION_I2C_MS));
    }
    return ack;
}
