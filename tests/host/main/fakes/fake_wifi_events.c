/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_wifi_events.c
 * @brief The system event loop for this suite: it records handlers so a case can fire events.
 */
#include <string.h>

#include "esp_event.h"

#include "fakes.h"

#define WIFI_EVENTS_MAX 4U

typedef struct {
    esp_event_base_t    base;
    int32_t             id;
    esp_event_handler_t handler;
    void               *arg;
} registration_t;

/* Never reset: kdrv_wifi registers once per binary, and that stays, as on the board. */
static registration_t s_regs[WIFI_EVENTS_MAX];
static unsigned       s_count;

esp_err_t fake_wifi_events_register(esp_event_base_t base, int32_t id,
                                    esp_event_handler_t handler, void *arg,
                                    esp_event_handler_instance_t *instance,
                                    int calls)
{
    (void)instance;
    (void)calls;
    if ( (NULL == handler) || (WIFI_EVENTS_MAX <= s_count) )
    {
        return ESP_ERR_NO_MEM;
    }
    s_regs[s_count].base    = base;
    s_regs[s_count].id      = id;
    s_regs[s_count].handler = handler;
    s_regs[s_count].arg     = arg;
    ++s_count;
    return ESP_OK;
}

unsigned fake_wifi_events_registered(void)
{
    return s_count;
}

void fake_wifi_events_deliver(esp_event_base_t base, int32_t id, void *data)
{
    for ( unsigned i = 0U; i < s_count; ++i )
    {
        const registration_t * const r = &s_regs[i];
        if ( (r->base == base)
            && ((ESP_EVENT_ANY_ID == r->id) || (r->id == id)) )
        {
            r->handler(r->arg, base, id, data);
        }
    }
}
