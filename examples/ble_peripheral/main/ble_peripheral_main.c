/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file ble_peripheral_main.c
 * @brief kodeOS example: a Bluetooth service of the app's own, from a task with kode.h.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "kode.h"
#include "kode_log.h"

#define TAG "ble_peripheral"

/** The name a phone sees in a scan, beside the console's service. */
#define BLE_NAME          "Kode BLE demo"

/**
 * @name The service
 * @brief 128-bit UUIDs of this example's own; an app of yours makes its own.
 * @{
 */
#define SVC_UUID          "8f3c0001-5b7e-4a52-9d2e-6b1f0c7a4e21"  /**< The service.      */
#define CHR_COUNT_UUID    "8f3c0002-5b7e-4a52-9d2e-6b1f0c7a4e21"  /**< The counter.      */
#define CHR_LED_UUID      "8f3c0003-5b7e-4a52-9d2e-6b1f0c7a4e21"  /**< The LED's colour. */
#define CHR_COUNT         "count"   /**< The counter's name, as the events give it. */
#define CHR_LED           "led"     /**< The LED's name.                            */
/** @} */

/**
 * @name What the values hold
 * @{
 */
#define COUNT_LEN         4U    /**< Bytes of the counter.                      */
#define BYTE_BITS         8U    /**< Bits in a byte, for the little-endian copy. */
#define BYTE_MASK         0xFFU /**< One byte of the counter.                   */
#define LED_LEN           3     /**< Bytes a write to `led` carries: R, G, B.    */
#define LED_R             0     /**< Where red is in the write.                 */
#define LED_G             1     /**< Where green is.                            */
#define LED_B             2     /**< Where blue is.                             */
/** @} */

/** How often the counter is notified, ms. */
#define NOTIFY_PERIOD_MS  1000U

/* What the launcher shows, and which category it files this app under. */
KODE_APP_MANIFEST("BLE peripheral", "example");

/* The app's service. At file scope, in the file that holds app_main(): the
 * radio looks for the constant this defines by name. A table it cannot serve
 * (a bad UUID, a name used twice) is left out with one log line, and the
 * first kode_ble_notify() says why. */
static const kode_ble_characteristic_t s_chars[] = {
    { CHR_COUNT, CHR_COUNT_UUID, KODE_BLE_READ | KODE_BLE_NOTIFY },
    { CHR_LED,   CHR_LED_UUID,   KODE_BLE_WRITE },
};
KODE_BLE_SERVICE(SVC_UUID, s_chars);

/**
 * @brief A phone wrote to the service: three bytes to `led` set the LED.
 *
 * The event's source is the characteristic's name and its value the number
 * of bytes. Its text is the bytes themselves, NULs included, so it is read by
 * index up to that number and never as a string. A listener may call any
 * kode.h call but kode_event_wait().
 *
 * @param e The event; its strings live until this returns.
 */
static void on_written(const kode_event_t *e)
{
    if ( (strcmp(e->source, CHR_LED) == 0) && (e->value == LED_LEN) )
    {
        const uint8_t r = (uint8_t)e->text[LED_R];
        const uint8_t g = (uint8_t)e->text[LED_G];
        const uint8_t b = (uint8_t)e->text[LED_B];

        if ( kode_led_set_rgb(r, g, b) )
        {
            KLOG_I(TAG, "led set to %u %u %u", (unsigned)r, (unsigned)g, (unsigned)b);
        }
        else
        {
            KLOG_W(TAG, "the LED refused the colour: %s", kode_error());
        }
    }
    else
    {
        KLOG_I(TAG, "%ld byte(s) written to %s, ignored", (long)e->value,
               e->source);
    }
}

/**
 * @brief A phone connected or left: any phone, the console's included.
 * @param e The event: its name says which.
 */
static void on_link(const kode_event_t *e)
{
    KLOG_I(TAG, "%s", e->name);
}

/**
 * @brief Listen for the service's writes and for phones coming and going.
 *
 * The constants link the radio's events; the first listener starts the
 * standard layer's own Active Object, which runs every listener.
 */
static void ble_listen(void)
{
    if ( !kode_event_add_listener(KODE_EVENT_BLE_WRITTEN, &on_written) )
    {
        KLOG_E(TAG, "no listener for writes: %s", kode_error());
    }
    if ( !kode_event_add_listener(KODE_EVENT_BLE_CONNECTED, &on_link) )
    {
        KLOG_E(TAG, "no listener for connections: %s", kode_error());
    }
    if ( !kode_event_add_listener(KODE_EVENT_BLE_DISCONNECTED, &on_link) )
    {
        KLOG_E(TAG, "no listener for disconnections: %s", kode_error());
    }
}

/**
 * @brief Notify the counter, and say when that starts or stops working.
 *
 * With no phone subscribed the value is set for the next read and the call
 * answers true. A failure (Bluetooth off, the service left out, the ring of
 * notifications full) repeats every second, so only the change is logged.
 *
 * @param count  The counter to send.
 * @param failed In and out: whether the last notify failed.
 */
static void count_notify(uint32_t count, bool *failed)
{
    uint8_t value[COUNT_LEN];
    uint32_t i;
    bool ok;

    for ( i = 0U; i < COUNT_LEN; i++ )
    {
        value[i] = (uint8_t)((count >> (i * BYTE_BITS)) & BYTE_MASK);
    }

    ok = kode_ble_notify(CHR_COUNT, value, sizeof(value));
    if ( !ok && !*failed )
    {
        KLOG_W(TAG, "no notify: %s", kode_error());
    }
    else if ( ok && *failed )
    {
        KLOG_I(TAG, "notifying again");
    }
    else
    {
        /* No change. */
    }
    *failed = !ok;
}

void app_main(void)
{
    uint32_t count = 0U;
    bool failed = false;

    /* Brings up logging, QF, the event pools, the core Active Objects and
     * AO_Radio, which every kode_ble_*() call links; its Bluetooth starts
     * with the app's service in it. */
    kode_init();

    if ( !kode_ble_set_name(BLE_NAME) )
    {
        KLOG_W(TAG, "the name was not set: %s", kode_error());
    }
    ble_listen();

    (void)kode_button_was_pressed("bottom");
    while ( !kode_button_was_pressed("bottom") )
    {
        count_notify(count, &failed);
        count++;
        kode_delay(NOTIFY_PERIOD_MS);
    }
    kode_exit();
}
