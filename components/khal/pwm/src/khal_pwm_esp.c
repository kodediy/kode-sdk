/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_pwm_esp.c
 * @brief khal_pwm's port on ESP-IDF: the low-speed LEDC, every timer on PLL_F80M.
 */
#include "khal_pwm_port.h"

#include "driver/ledc.h"
#include "esp_err.h"
#include "soc/soc_caps.h"

#include "khal_gpio.h"
#include "khal_pwm.h"
#include "kode_log.h"

#define TAG "khal.pwm"   /**< This file's log tag. */

/* khal_pwm's counts against the chip's. */
_Static_assert((uint32_t)LEDC_TIMER_MAX == (uint32_t)KHAL_PWM_TIMERS,
               "KHAL_PWM_TIMERS must be the LEDC timers this chip has");
_Static_assert((uint32_t)SOC_LEDC_CHANNEL_NUM == (uint32_t)KHAL_PWM_CHANNELS,
               "KHAL_PWM_CHANNELS must be the LEDC channels this chip has");
_Static_assert((uint32_t)SOC_LEDC_TIMER_BIT_WIDTH == (uint32_t)KHAL_PWM_PORT_BITS_MAX,
               "KHAL_PWM_PORT_BITS_MAX must be this chip's timer width");

#define PWM_MODE        LEDC_LOW_SPEED_MODE    /**< The one LEDC group the P4 has. */
#define PWM_CLK         LEDC_USE_PLL_DIV_CLK   /**< PLL_F80M: KHAL_PWM_PORT_CLK_HZ, one source for all four timers. */
#define PWM_IDLE_LEVEL  0U                     /**< The level a stopped channel leaves on its pin. */
#define PWM_HPOINT      0                      /**< Every channel goes high at the start of its period. */

kode_status_t khal_pwm_port_timer_set(uint8_t timer, uint32_t hz, uint8_t bits)
{
    const ledc_timer_config_t cfg = {
        .speed_mode      = PWM_MODE,
        .duty_resolution = (ledc_timer_bit_t)bits,
        .timer_num       = (ledc_timer_t)timer,
        .freq_hz         = hz,
        .clk_cfg         = PWM_CLK,
    };
    if ( ledc_timer_config(&cfg) != ESP_OK )
    {
        KLOG_E(TAG, "LEDC timer %u will not run at %lu Hz, %u bits",
               (unsigned)timer, (unsigned long)hz, (unsigned)bits);
        return KODE_ERROR;
    }
    return KODE_OK;
}

void khal_pwm_port_timer_stop(uint8_t timer)
{
    /* The driver de-configures only a paused timer. */
    const ledc_timer_config_t cfg = {
        .speed_mode  = PWM_MODE,
        .timer_num   = (ledc_timer_t)timer,
        .deconfigure = true,
    };
    (void)ledc_timer_pause(PWM_MODE, (ledc_timer_t)timer);
    (void)ledc_timer_config(&cfg);
}

kode_status_t khal_pwm_port_timer_pause(uint8_t timer)
{
    if ( ledc_timer_pause(PWM_MODE, (ledc_timer_t)timer) != ESP_OK )
    {
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t khal_pwm_port_timer_resume(uint8_t timer)
{
    if ( ledc_timer_resume(PWM_MODE, (ledc_timer_t)timer) != ESP_OK )
    {
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t khal_pwm_port_chan_start(uint8_t chan, uint8_t gpio, uint8_t timer,
                                       uint32_t duty)
{
    const ledc_channel_config_t cfg = {
        .gpio_num   = (int)gpio,
        .speed_mode = PWM_MODE,
        .channel    = (ledc_channel_t)chan,
        .timer_sel  = (ledc_timer_t)timer,
        .duty       = duty,
        .hpoint     = PWM_HPOINT,
    };
    if ( ledc_channel_config(&cfg) != ESP_OK )
    {
        KLOG_E(TAG, "LEDC channel %u will not take GPIO%u", (unsigned)chan,
               (unsigned)gpio);
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t khal_pwm_port_chan_bind(uint8_t chan, uint8_t timer, uint32_t duty)
{
    if ( ledc_bind_channel_timer(PWM_MODE, (ledc_channel_t)chan,
                                 (ledc_timer_t)timer) != ESP_OK )
    {
        KLOG_E(TAG, "LEDC channel %u will not move to timer %u", (unsigned)chan,
               (unsigned)timer);
        return KODE_ERROR;
    }
    return khal_pwm_port_chan_duty(chan, duty);
}

kode_status_t khal_pwm_port_chan_duty(uint8_t chan, uint32_t duty)
{
    if ( ledc_set_duty(PWM_MODE, (ledc_channel_t)chan, duty) != ESP_OK )
    {
        return KODE_ERROR;
    }
    if ( ledc_update_duty(PWM_MODE, (ledc_channel_t)chan) != ESP_OK )
    {
        return KODE_ERROR;
    }
    return KODE_OK;
}

void khal_pwm_port_chan_stop(uint8_t chan, uint8_t gpio)
{
    /* ledc_stop() alone leaves the pad routed to the channel: the next user of
     * the channel would drive this pin too. De-configuring takes the channel
     * off every pin it had and gives up their reservation. */
    const ledc_channel_config_t cfg = {
        .speed_mode  = PWM_MODE,
        .channel     = (ledc_channel_t)chan,
        .deconfigure = true,
    };
    (void)ledc_stop(PWM_MODE, (ledc_channel_t)chan, PWM_IDLE_LEVEL);
    (void)ledc_channel_config(&cfg);
    /* The pin stays an output at the idle level rather than floating. */
    (void)khal_gpio_output(gpio, false);
}
