/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_wifi_events.h
 * @brief The control surface of fake_wifi_events.c, kept out of fakes.h
 *        because it speaks esp_event's types.
 */
#ifndef FAKE_WIFI_EVENTS_H_
#define FAKE_WIFI_EVENTS_H_

#include "esp_event.h"

/**
 * @brief The stub for the mocked esp_event_handler_instance_register().
 *
 * Pass it to esp_event_handler_instance_register_Stub() before bringing the
 * stack up; it keeps the handler so fake_wifi_events_deliver() can call it.
 */
esp_err_t fake_wifi_events_register(esp_event_base_t base, int32_t id,
                                    esp_event_handler_t handler, void *arg,
                                    esp_event_handler_instance_t *instance,
                                    int calls);

/** @brief How many registrations the stub has kept. */
unsigned fake_wifi_events_registered(void);

/** @brief Call every handler registered for @p base and @p id, in order. */
void fake_wifi_events_deliver(esp_event_base_t base, int32_t id, void *data);

#endif /* FAKE_WIFI_EVENTS_H_ */
