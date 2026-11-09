/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_gpio_esp.c
 * @brief khal_gpio on ESP-IDF: the GPIO driver's shared interrupt service.
 */
#include "khal_gpio.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_sleep.h"
#include "soc/gpio_struct.h"

#include "kode_log.h"

#define TAG "khal.gpio"

/** @brief What an armed pin calls, and with what. Indexed by pin number. */
typedef struct {
    khal_gpio_cb_t  cb;
    void           *arg;
    gpio_int_type_t type;   /**< What the interrupt was attached for.  */
} slot_t;

static slot_t s_slots[GPIO_NUM_MAX];
static bool   s_service;

/** @brief The one handler behind every pin: disarm, then call back. */
static void gpio_isr(void *arg)
{
    gpio_num_t const pin = (gpio_num_t)(uintptr_t)arg;

    (void)gpio_intr_disable(pin);
    if ( NULL != s_slots[pin].cb )
    {
        s_slots[pin].cb(s_slots[pin].arg);
    }
}

kode_status_t khal_gpio_input(uint8_t pin, khal_gpio_pull_t pull)
{
    gpio_pullup_t   up   = GPIO_PULLUP_DISABLE;
    gpio_pulldown_t down = GPIO_PULLDOWN_DISABLE;

    if ( (uint8_t)GPIO_NUM_MAX <= pin )
    {
        return KODE_DENIED;
    }
    if ( KHAL_GPIO_PULL_UP == pull )
    {
        up = GPIO_PULLUP_ENABLE;
    }
    if ( KHAL_GPIO_PULL_DOWN == pull )
    {
        down = GPIO_PULLDOWN_ENABLE;
    }
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = up,
        .pull_down_en = down,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    if ( gpio_config(&cfg) != ESP_OK )
    {
        KLOG_E(TAG, "cannot configure GPIO%u as an input", (unsigned)pin);
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t khal_gpio_output(uint8_t pin, bool level)
{
    uint32_t lv = 0U;

    if ( (uint8_t)GPIO_NUM_MAX <= pin )
    {
        return KODE_DENIED;
    }
    if ( level )
    {
        lv = 1U;
    }
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode         = GPIO_MODE_INPUT_OUTPUT,   /* keeps the pad readable */
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    /* Level first, so the pin never shows the old value as an output. */
    (void)gpio_set_level((gpio_num_t)pin, lv);
    if ( gpio_config(&cfg) != ESP_OK )
    {
        KLOG_E(TAG, "cannot configure GPIO%u as an output", (unsigned)pin);
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t khal_gpio_write(uint8_t pin, bool level)
{
    uint32_t lv = 0U;
    if ( level )
    {
        lv = 1U;
    }
    if ( gpio_set_level((gpio_num_t)pin, lv) != ESP_OK )
    {
        return KODE_DENIED;
    }
    return KODE_OK;
}

void khal_gpio_release(uint8_t pin)
{
    if ( (uint8_t)GPIO_NUM_MAX <= pin )
    {
        return;
    }
    if ( NULL != s_slots[pin].cb )
    {
        (void)gpio_intr_disable((gpio_num_t)pin);
        (void)gpio_isr_handler_remove((gpio_num_t)pin);
        s_slots[pin].cb  = NULL;
        s_slots[pin].arg = NULL;
    }
    (void)gpio_reset_pin((gpio_num_t)pin);
    /* ESP-IDF's reset leaves the pull-up on; a released pin floats. */
    (void)gpio_set_pull_mode((gpio_num_t)pin, GPIO_FLOATING);
}

bool khal_gpio_level(uint8_t pin)
{
    return gpio_get_level((gpio_num_t)pin) != 0;
}

bool khal_gpio_driven_low(uint8_t pin)
{
    if ( (uint8_t)GPIO_NUM_MAX <= pin )
    {
        return false;
    }
    uint32_t const bit = 1UL << ((uint32_t)pin % 32U);

    if ( 32U > pin )
    {
        return ((GPIO.enable.val & bit) != 0U) && ((GPIO.out.val & bit) == 0U);
    }
    return ((GPIO.enable1.val & bit) != 0U) && ((GPIO.out1.val & bit) == 0U);
}

kode_status_t khal_gpio_service(void)
{
    if ( !s_service )
    {
        /* No flags, not even IRAM: while the flash is written the interrupt
         * waits, latched, instead of every handler having to live in IRAM. */
        esp_err_t const err = gpio_install_isr_service(0);
        /* Someone else may have installed it first; that is not a failure. */
        if ( (ESP_OK != err) && (ESP_ERR_INVALID_STATE != err) )
        {
            KLOG_E(TAG, "no GPIO interrupt service: %s", esp_err_to_name(err));
            return KODE_ERROR;
        }
        s_service = true;
    }
    return KODE_OK;
}

kode_status_t khal_gpio_irq_attach(uint8_t pin, khal_gpio_irq_t kind,
                                   khal_gpio_cb_t cb, void *arg)
{
    if ( (NULL == cb) || ((uint8_t)GPIO_NUM_MAX <= pin) )
    {
        return KODE_DENIED;
    }
    if ( khal_gpio_service() != KODE_OK )
    {
        return KODE_ERROR;
    }

    gpio_int_type_t type = GPIO_INTR_LOW_LEVEL;
    if ( KHAL_GPIO_IRQ_ANY_EDGE == kind )
    {
        type = GPIO_INTR_ANYEDGE;
    }
    if ( KHAL_GPIO_IRQ_HIGH_LEVEL == kind )
    {
        type = GPIO_INTR_HIGH_LEVEL;
    }
    s_slots[pin].cb   = cb;
    s_slots[pin].arg  = arg;
    s_slots[pin].type = type;
    if ( (gpio_set_intr_type((gpio_num_t)pin, type) != ESP_OK)
        || (gpio_isr_handler_add((gpio_num_t)pin, gpio_isr,
                                 (void *)(uintptr_t)pin) != ESP_OK) )
    {
        s_slots[pin].cb = NULL;
        KLOG_E(TAG, "cannot arm GPIO%u", (unsigned)pin);
        return KODE_ERROR;
    }
    return KODE_OK;
}

void khal_gpio_irq_enable(uint8_t pin)
{
    (void)gpio_intr_enable((gpio_num_t)pin);
}

kode_status_t khal_gpio_wake_low(uint8_t pin)
{
    if ( ((uint8_t)GPIO_NUM_MAX <= pin) || (NULL == s_slots[pin].cb) )
    {
        return KODE_DENIED;
    }
    if ( (gpio_wakeup_enable((gpio_num_t)pin, GPIO_INTR_LOW_LEVEL) != ESP_OK)
        || (esp_sleep_enable_gpio_wakeup() != ESP_OK) )
    {
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t khal_gpio_wake(uint8_t pin, bool on)
{
    if ( ((uint8_t)GPIO_NUM_MAX <= pin) || (NULL == s_slots[pin].cb) )
    {
        return KODE_DENIED;
    }
    if ( on )
    {
        if ( (gpio_wakeup_enable((gpio_num_t)pin, s_slots[pin].type) != ESP_OK)
            || (esp_sleep_enable_gpio_wakeup() != ESP_OK) )
        {
            return KODE_ERROR;
        }
        return KODE_OK;
    }
    /* Withdrawing a wake clears the pin's interrupt type with it; the type the
     * owner attached is put back, or its interrupt would never fire again. */
    if ( (gpio_wakeup_disable((gpio_num_t)pin) != ESP_OK)
        || (gpio_set_intr_type((gpio_num_t)pin, s_slots[pin].type) != ESP_OK) )
    {
        return KODE_ERROR;
    }
    return KODE_OK;
}
