/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fakes.h
 * @brief What the fakes under kdrv_es8311.c record, and the state they model.
 */
#ifndef HOST_CODEC_FAKES_H_
#define HOST_CODEC_FAKES_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief One thing the driver did to the I2S driver, the codec or the pad. */
typedef enum {
    EV_I2S_INIT,        /**< i2s_channel_init_std_mode(): the pins are routed.   */
    EV_I2S_ROUTE_TX,    /**< i2s_channel_reconfig_std_gpio() on the transmit half. */
    EV_I2S_ROUTE_RX,    /**< The same on the receive half.                       */
    EV_CODEC_OPEN,      /**< esp_codec_dev_open().                               */
    EV_CODEC_CLOSE,     /**< esp_codec_dev_close().                              */
    EV_PIN_RELEASE,     /**< khal_gpio_release(); the argument is the pin.       */
    EV_PIN_PULLUP,      /**< khal_gpio_input() with the pull-up; the pin.        */
    EV_PIN_OTHER,       /**< khal_gpio_input() with any other pull; the pin.     */
} fake_ev_t;

/** @brief One entry of the log. */
typedef struct {
    fake_ev_t ev;       /**< What happened.                                      */
    int       arg;      /**< The pin, where the event has one; else 0.           */
} fake_entry_t;

/** @brief How many entries the log holds. */
#define FAKE_LOG_MAX  64U

extern fake_entry_t g_log[FAKE_LOG_MAX];  /**< Every call, in order.            */
extern size_t       g_log_n;              /**< How many of them.                */

extern bool g_mclk_routed;     /**< The I2S master clock is connected to its pad.   */
extern int  g_mclk_pin;        /**< The pad it was last connected to.               */
extern bool g_pin_pullup;      /**< The pad is an input with the pull-up on.        */
extern bool g_chan_running;    /**< A channel is enabled: the codec layer opened it. */
extern int  g_open_result;     /**< What esp_codec_dev_open() answers. A refusal still
                                *   leaves the channels enabled, as the component's does. */

extern bool g_fail_route_rx;   /**< The receive half refuses the pins when they are routed again. */
extern bool g_fail_rx_init;    /**< The receive half's init refuses the format.     */
extern bool g_fail_data_if;    /**< The codec's data interface cannot be made.      */
extern bool g_fail_codec_new;  /**< The codec object cannot be made.                */
extern bool g_fail_dev_new;    /**< The codec device cannot be made.                */
extern bool g_fail_vol_curve;  /**< The codec layer refuses the volume curve.       */

/** @brief Calls that let go of the clock pad, or set it as an input, while a channel was enabled. */
extern unsigned g_viol_pad;
/** @brief Control interface accesses while a channel was enabled and the clock pad was not routed. */
extern unsigned g_viol_ctrl;

/**
 * @brief Make one write through the codec's control interface, as the component does.
 *
 * The component's open() writes the chip's registers after it has enabled the
 * channels; the fake's open() calls this, and a case calls it to show the
 * witness that watches it.
 */
void fake_ctrl_poke(void);

/**
 * @brief How many violations the witnesses recorded.
 * @return g_viol_pad plus g_viol_ctrl.
 */
unsigned fake_violations(void);

/** @brief Forget everything and put the fakes back to a board with a codec fitted. */
void fake_reset(void);

/**
 * @brief How many times the log holds @p ev.
 * @param ev The event.
 * @return The count.
 */
size_t fake_count(fake_ev_t ev);

/**
 * @brief Where in the log @p ev last happened.
 * @param ev The event.
 * @return Its index, or FAKE_LOG_MAX when it never did.
 */
size_t fake_last(fake_ev_t ev);

/**
 * @brief Where in the log @p ev first happened at or after @p from.
 * @param ev   The event.
 * @param from The first index to look at.
 * @return Its index, or FAKE_LOG_MAX when it did not.
 */
size_t fake_first_from(fake_ev_t ev, size_t from);

#endif /* HOST_CODEC_FAKES_H_ */
