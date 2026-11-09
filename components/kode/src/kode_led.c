/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_led.c
 * @brief The standard layer's colours and LED: kode_rgb() and the four LED calls.
 */
#include "kode.h"

#include "kode_entry.h"
#include "kode_sys.h"

/** @brief How long an LED call keeps trying when the system is busy, in ms. */
#define KODE_LED_BOUND_MS 100U

/** @brief The text kode_error() gives when the LED never took a request. */
#define KODE_LED_REFUSED "the LED did not take the request"

/** @brief Bits in one colour byte. */
#define KODE_COLOR_BYTE_BITS 8U

/** @brief Mask of one colour byte. */
#define KODE_COLOR_BYTE_MASK 0xFFU

/** @brief What a request asks of the LED. */
typedef enum {
    LED_OP_COLOR = 0,  /**< Light it in a colour.   */
    LED_OP_OFF,        /**< Switch it off.          */
    LED_OP_TOGGLE      /**< Flip it.                */
} led_op_t;

/** @brief What one try needs: the colour, or the LED off, or a flip. */
typedef struct {
    led_op_t op;  /**< What to ask; the colour is read only for LED_OP_COLOR. */
    uint8_t  r;   /**< Red.                                                   */
    uint8_t  g;   /**< Green.                                                 */
    uint8_t  b;   /**< Blue.                                                  */
} led_request_t;

kode_color_t kode_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((kode_color_t)r << (2U * KODE_COLOR_BYTE_BITS))
         | ((kode_color_t)g << KODE_COLOR_BYTE_BITS)
         | (kode_color_t)b;
}

/** @brief One try of a request: true when AO_LedMgr's event was posted. */
static bool led_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the led_request_t led_send() passed */
    const led_request_t *const req = (const led_request_t *)ctx;
    kode_status_t status;

    switch ( req->op )
    {
    case LED_OP_OFF:
        status = kode_sys_led_set(false);
        break;
    case LED_OP_TOGGLE:
        status = kode_sys_led_toggle();
        break;
    default:
        status = kode_sys_led_set_rgb(req->r, req->g, req->b);
        break;
    }
    return KODE_OK == status;
}

/** @brief Post @p req, waiting its turn; sets the task's error on failure. */
static bool led_send(led_request_t *req)
{
    bool taken = kode_retry(&led_attempt, req, KODE_LED_BOUND_MS);

    if ( !taken )
    {
        kode_err_set(KODE_LED_REFUSED);
    }
    return taken;
}

bool kode_led_set_color(kode_color_t c)
{
    bool taken = false;

    if ( kode_enter("kode_led_set_color") )
    {
        led_request_t req = {
            LED_OP_COLOR,
            (uint8_t)((c >> (2U * KODE_COLOR_BYTE_BITS)) & KODE_COLOR_BYTE_MASK),
            (uint8_t)((c >> KODE_COLOR_BYTE_BITS) & KODE_COLOR_BYTE_MASK),
            (uint8_t)(c & KODE_COLOR_BYTE_MASK)
        };

        taken = led_send(&req);
    }
    return taken;
}

bool kode_led_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    bool taken = false;

    if ( kode_enter("kode_led_set_rgb") )
    {
        led_request_t req = { LED_OP_COLOR, r, g, b };

        taken = led_send(&req);
    }
    return taken;
}

bool kode_led_off(void)
{
    bool taken = false;

    if ( kode_enter("kode_led_off") )
    {
        led_request_t req = { LED_OP_OFF, 0U, 0U, 0U };

        taken = led_send(&req);
    }
    return taken;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_led_toggle(void)
{
    bool taken = false;

    if ( kode_enter("kode_led_toggle") )
    {
        led_request_t req = { LED_OP_TOGGLE, 0U, 0U, 0U };

        taken = led_send(&req);
    }
    return taken;
}
