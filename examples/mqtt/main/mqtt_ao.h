/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file mqtt_ao.h
 * @brief The example's own Active Object: the buttons talk to an MQTT broker.
 */
#ifndef MQTT_AO_H_
#define MQTT_AO_H_

#include "kode_sys.h"

/** @brief The singleton, as an opaque QActive - the pattern every kodeOS AO
 *         follows, so nothing outside mqtt_ao.c can reach its members. */
extern QActive * const AO_MqttDemo;

/**
 * @brief Construct it.
 *
 * Call after kode_init() and before MqttDemo_start(). It only initialises
 * memory, and builds the two topics from this board's serial: nothing is
 * posted and nothing is subscribed until MqttDemo_start() runs the initial
 * transition, in the caller's thread, before the AO's own task exists.
 */
void MqttDemo_ctor(void);

/**
 * @brief Give it a thread, a queue and a stack, and let it run.
 *
 * Kept next to the storage it uses rather than open-coded in app_main(), and
 * next to the one line that says why this AO does not start at the level the
 * simplest examples do.
 */
void MqttDemo_start(void);

#endif /* MQTT_AO_H_ */
