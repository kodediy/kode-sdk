/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_idf.c
 * @brief The I2S driver, the codec component, khal and the FreeRTOS tick, as one recorder.
 *
 * Enough of the I2S driver's rules are modelled to make order matter: the
 * pins are routed by the channel's init, they can be routed again only while
 * no channel is enabled (IDF answers ESP_ERR_INVALID_STATE otherwise), and the
 * codec layer's open enables the channels and its close disables them.
 *
 * Two witnesses watch the properties the driver exists for. The pad is never
 * let go while a channel is enabled (g_viol_pad), and the chip's control
 * interface is never used while a channel is enabled and the clock is not
 * routed to its pad (g_viol_ctrl): the chip needs its clock to take a
 * register write. Both count; a case reads them through fake_violations().
 */
#include <stdarg.h>
#include <string.h>

#include "es8311_codec.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "driver/i2s_std.h"

#include "fakes.h"
#include "kdrv_es8311.h"
#include "khal_gpio.h"
#include "khal_i2c.h"
#include "khal_sync.h"
#include "khal_system.h"
#include "khal_time.h"
#include "kbsp.h"

fake_entry_t g_log[FAKE_LOG_MAX];
size_t       g_log_n;
bool         g_mclk_routed;
int          g_mclk_pin;
bool         g_pin_pullup;
bool         g_chan_running;
int          g_open_result;
bool         g_fail_route_rx;
bool         g_fail_rx_init;
bool         g_fail_data_if;
bool         g_fail_codec_new;
bool         g_fail_dev_new;
bool         g_fail_vol_curve;
uint32_t     g_viol_pad;
uint32_t     g_viol_ctrl;

static uint8_t s_tx_obj;                 /**< The address is the transmit handle. */
static uint8_t s_rx_obj;                 /**< The address is the receive handle.  */
static uint8_t s_data_obj;               /**< The data interface.                 */
static uint8_t s_gpio_obj;               /**< The codec's pin interface.          */
static uint8_t s_dev_obj;                /**< The codec device.                   */
static uint8_t s_bus_obj;                /**< The I2C bus.                        */
static i2s_std_gpio_config_t s_pins;     /**< What the channels were last given.  */

static void log_add(fake_ev_t ev, int arg)
{
    if ( FAKE_LOG_MAX > g_log_n )
    {
        g_log[g_log_n].ev  = ev;
        g_log[g_log_n].arg = arg;
        ++g_log_n;
    }
}

void fake_reset(void)
{
    (void)memset(g_log, 0, sizeof(g_log));
    g_log_n        = 0U;
    g_mclk_routed  = false;
    g_mclk_pin     = -1;
    g_pin_pullup   = false;
    g_chan_running = false;
    g_open_result  = ESP_CODEC_DEV_OK;
    g_fail_route_rx  = false;
    g_fail_rx_init   = false;
    g_fail_data_if   = false;
    g_fail_codec_new = false;
    g_fail_dev_new   = false;
    g_fail_vol_curve = false;
    g_viol_pad     = 0U;
    g_viol_ctrl    = 0U;
    (void)memset(&s_pins, 0, sizeof(s_pins));
}

uint32_t fake_violations(void)
{
    return g_viol_pad + g_viol_ctrl;
}

size_t fake_count(fake_ev_t ev)
{
    size_t n = 0U;

    for ( size_t i = 0U; g_log_n > i; ++i )
    {
        if ( ev == g_log[i].ev )
        {
            ++n;
        }
    }
    return n;
}

size_t fake_last(fake_ev_t ev)
{
    size_t at = FAKE_LOG_MAX;

    for ( size_t i = 0U; g_log_n > i; ++i )
    {
        if ( ev == g_log[i].ev )
        {
            at = i;
        }
    }
    return at;
}

size_t fake_first_from(fake_ev_t ev, size_t from)
{
    for ( size_t i = from; g_log_n > i; ++i )
    {
        if ( ev == g_log[i].ev )
        {
            return i;
        }
    }
    return FAKE_LOG_MAX;
}

/* ---- The I2S driver ------------------------------------------------------- */

esp_err_t i2s_new_channel(const i2s_chan_config_t *chan_cfg,
                          i2s_chan_handle_t *tx_handle, i2s_chan_handle_t *rx_handle)
{
    (void)chan_cfg;
    *tx_handle = (i2s_chan_handle_t)&s_tx_obj;
    *rx_handle = (i2s_chan_handle_t)&s_rx_obj;
    return ESP_OK;
}

esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t handle, const i2s_std_config_t *std_cfg)
{
    if ( g_fail_rx_init && (handle == (i2s_chan_handle_t)&s_rx_obj) )
    {
        return ESP_FAIL;
    }
    s_pins        = std_cfg->gpio_cfg;
    g_mclk_routed = true;
    g_mclk_pin    = std_cfg->gpio_cfg.mclk;
    g_pin_pullup  = false;
    log_add(EV_I2S_INIT, 0);
    return ESP_OK;
}

esp_err_t i2s_channel_reconfig_std_gpio(i2s_chan_handle_t handle,
                                        const i2s_std_gpio_config_t *gpio_cfg)
{
    if ( g_chan_running )
    {
        return ESP_ERR_INVALID_STATE;
    }
    if ( g_fail_route_rx && (handle == (i2s_chan_handle_t)&s_rx_obj) )
    {
        return ESP_FAIL;
    }
    s_pins        = *gpio_cfg;
    g_mclk_routed = true;
    g_mclk_pin    = gpio_cfg->mclk;
    g_pin_pullup  = false;
    if ( handle == (i2s_chan_handle_t)&s_tx_obj )
    {
        log_add(EV_I2S_ROUTE_TX, gpio_cfg->mclk);
    }
    else
    {
        log_add(EV_I2S_ROUTE_RX, gpio_cfg->mclk);
    }
    return ESP_OK;
}

/* ---- The codec component -------------------------------------------------- */

/** @brief Record an access to the chip while its clock cannot be reaching it. */
static void ctrl_witness(void)
{
    if ( g_chan_running && !g_mclk_routed )
    {
        ++g_viol_ctrl;
    }
}

static int ctrl_read(const audio_codec_ctrl_if_t *ctrl, int reg, int reg_len,
                     void *data, int data_len)
{
    (void)ctrl; (void)reg_len; (void)data_len;
    uint8_t *out = (uint8_t *)data;

    ctrl_witness();
    *out = 0U;
    if ( 0xFD == reg )
    {
        *out = (uint8_t)KDRV_ES8311_ID1;
    }
    if ( 0xFE == reg )
    {
        *out = (uint8_t)KDRV_ES8311_ID2;
    }
    return 0;
}

static int ctrl_write(const audio_codec_ctrl_if_t *ctrl, int reg, int reg_len,
                      void *data, int data_len)
{
    (void)ctrl; (void)reg; (void)reg_len; (void)data; (void)data_len;
    ctrl_witness();
    return 0;
}

static int chip_enable(const audio_codec_if_t *h, bool enable)
{
    (void)h; (void)enable;
    return ESP_CODEC_DEV_OK;
}

static const audio_codec_ctrl_if_t s_ctrl = { .read_reg = ctrl_read, .write_reg = ctrl_write };
static const audio_codec_if_t      s_chip = { .enable = chip_enable };

void fake_ctrl_poke(void)
{
    uint8_t v = 0U;

    (void)s_ctrl.write_reg(&s_ctrl, 0, 1, &v, 1);
}

const audio_codec_ctrl_if_t *audio_codec_new_i2c_ctrl(audio_codec_i2c_cfg_t *cfg)
{
    (void)cfg;
    return &s_ctrl;
}

const audio_codec_data_if_t *audio_codec_new_i2s_data(audio_codec_i2s_cfg_t *cfg)
{
    (void)cfg;
    if ( g_fail_data_if )
    {
        return NULL;
    }
    return (const audio_codec_data_if_t *)&s_data_obj;
}

const audio_codec_gpio_if_t *audio_codec_new_gpio(void)
{
    return (const audio_codec_gpio_if_t *)&s_gpio_obj;
}

const audio_codec_if_t *es8311_codec_new(es8311_codec_cfg_t *cfg)
{
    (void)cfg;
    if ( g_fail_codec_new )
    {
        return NULL;
    }
    return &s_chip;
}

esp_codec_dev_handle_t esp_codec_dev_new(const esp_codec_dev_cfg_t *cfg)
{
    (void)cfg;
    if ( g_fail_dev_new )
    {
        return NULL;
    }
    return (esp_codec_dev_handle_t)&s_dev_obj;
}

int esp_codec_dev_open(esp_codec_dev_handle_t dev, esp_codec_dev_sample_info_t *fs)
{
    (void)dev; (void)fs;
    log_add(EV_CODEC_OPEN, 0);
    /* The component enables both channels first and writes the chip's
     * registers after, and a refusal at any later step leaves the channels
     * enabled (esp_codec_dev.c, esp_codec_dev_open()). */
    g_chan_running = true;
    fake_ctrl_poke();
    return g_open_result;
}

int esp_codec_dev_close(esp_codec_dev_handle_t dev)
{
    (void)dev;
    log_add(EV_CODEC_CLOSE, 0);
    g_chan_running = false;
    return ESP_CODEC_DEV_OK;
}

int esp_codec_dev_write(esp_codec_dev_handle_t dev, void *data, int len)
{
    (void)dev; (void)data; (void)len;
    return ESP_CODEC_DEV_OK;
}

int esp_codec_dev_read(esp_codec_dev_handle_t dev, void *data, int len)
{
    (void)dev;
    (void)memset(data, 0, (size_t)len);
    return ESP_CODEC_DEV_OK;
}

int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t dev, int volume)
{
    (void)dev; (void)volume;
    return ESP_CODEC_DEV_OK;
}

int esp_codec_dev_set_in_gain(esp_codec_dev_handle_t dev, float db)
{
    (void)dev; (void)db;
    return ESP_CODEC_DEV_OK;
}

int esp_codec_dev_set_out_mute(esp_codec_dev_handle_t dev, bool mute)
{
    (void)dev; (void)mute;
    return ESP_CODEC_DEV_OK;
}

int esp_codec_dev_set_in_mute(esp_codec_dev_handle_t dev, bool mute)
{
    (void)dev; (void)mute;
    return ESP_CODEC_DEV_OK;
}

int esp_codec_dev_set_vol_curve(esp_codec_dev_handle_t dev, esp_codec_dev_vol_curve_t *curve)
{
    (void)dev; (void)curve;
    if ( g_fail_vol_curve )
    {
        return -1;
    }
    return ESP_CODEC_DEV_OK;
}

int esp_codec_set_disable_when_closed(esp_codec_dev_handle_t dev, bool disable)
{
    (void)dev; (void)disable;
    return ESP_CODEC_DEV_OK;
}

/* ---- khal ----------------------------------------------------------------- */

void khal_gpio_release(uint8_t pin)
{
    if ( ((int)KBSP_PIN_I2S_MCLK == (int)pin) && g_chan_running )
    {
        ++g_viol_pad;
    }
    if ( (int)pin == g_mclk_pin )
    {
        g_mclk_routed = false;
        g_pin_pullup  = false;
    }
    log_add(EV_PIN_RELEASE, (int)pin);
}

kode_status_t khal_gpio_input(uint8_t pin, khal_gpio_pull_t pull)
{
    if ( ((int)KBSP_PIN_I2S_MCLK == (int)pin) && g_chan_running )
    {
        ++g_viol_pad;
    }
    if ( (int)pin == g_mclk_pin )
    {
        g_mclk_routed = false;
        g_pin_pullup  = (KHAL_GPIO_PULL_UP == pull);
    }
    if ( KHAL_GPIO_PULL_UP == pull )
    {
        log_add(EV_PIN_PULLUP, (int)pin);
    }
    else
    {
        log_add(EV_PIN_OTHER, (int)pin);
    }
    return KODE_OK;
}

kode_status_t khal_i2c_init(void)
{
    return KODE_OK;
}

struct i2c_master_bus_t *khal_i2c_bus(void)
{
    return (struct i2c_master_bus_t *)&s_bus_obj;
}

bool khal_i2c_probe(uint16_t addr, uint32_t timeout_ms)
{
    (void)addr; (void)timeout_ms;
    return true;
}

kode_status_t khal_i2c_add(uint16_t addr, khal_i2c_dev_t *out)
{
    (void)addr; (void)out;
    return KODE_ERROR;
}

kode_status_t khal_i2c_remove(khal_i2c_dev_t dev)
{
    (void)dev;
    return KODE_OK;
}

kode_status_t khal_i2c_read_reg(khal_i2c_dev_t dev, uint8_t reg, uint8_t *buf, size_t len)
{
    (void)dev; (void)reg; (void)buf; (void)len;
    return KODE_ERROR;
}

kode_status_t khal_i2c_write_reg(khal_i2c_dev_t dev, uint8_t reg, uint8_t val)
{
    (void)dev; (void)reg; (void)val;
    return KODE_ERROR;
}

khal_sem_t khal_sem_create(void)
{
    return NULL;
}

bool khal_sem_wait_timeout(khal_sem_t s, uint32_t ms)
{
    (void)s; (void)ms;
    return false;
}

void khal_sem_signal(khal_sem_t s)
{
    (void)s;
}

void *khal_alloc_psram(size_t align, size_t size)
{
    (void)align; (void)size;
    return NULL;
}

void khal_delay_ms(uint32_t ms)
{
    (void)ms;
}
