/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file radio_knocks.h
 * @brief Package-private: the three knocks other tasks reach AO_Radio with.
 *
 * The lost link, the BLE state and the pairing note come from the radio
 * worker, esp-hosted's tasks and NimBLE's host task; the pairing note also
 * from AO_Radio's own thread, when it refuses a pairing. Each is a knock
 * (radio_knock.h): one static event, posted with KQP_RADIO_KNOCK_MARGIN, that
 * a full queue owes and never loses.
 */
#ifndef RADIO_KNOCKS_H_
#define RADIO_KNOCKS_H_

#include <stdbool.h>
#include <stdint.h>

#include "kqp.h"

/** @brief The knocks, by what each tells AO_Radio to read. */
typedef enum {
    RADIO_KNOCK_LOST = 0,   /**< RADIO_LINK_LOST_SIG: the link to the C5 is gone; carries the first loss's kode_status_t. */
    RADIO_KNOCK_BLE,        /**< BLE_LINK_SIG: the BLE state or the bond count moved; the driver keeps both. */
    RADIO_KNOCK_PAIR,       /**< BLE_PAIR_TAKE_SIG: a pairing moved; the driver keeps the record. */
    RADIO_KNOCK_COUNT       /**< How many there are. */
} radio_knock_id_t;

/**
 * @brief Make the knocks: each has its door, nothing is pending and nothing is owed.
 *
 * AO_Radio's initial transition calls it before the callbacks that post are
 * registered, and no knock is posted before it; a queue this start has not
 * had owes nothing.
 */
void radio_knocks_reset(void);

/**
 * @brief Post a knock, from any task.
 *
 * A refusal is said once in the log and owed.
 *
 * @param id  The knock.
 * @param arg What it carries; the first of the posts that fold stands.
 */
void radio_knocks_post(radio_knock_id_t id, uint8_t arg);

/**
 * @brief Post again what a full queue refused, on AO_Radio's own thread.
 *
 * Called at the head of the handler of every state that can be the current
 * one, so it runs before each event the AO handles.
 *
 * @param e The event about to be handled; the entry, exit and init signals
 *          of a transition are not events and pay nothing.
 */
void radio_knocks_pay(const QEvt *e);

/**
 * @brief Take a knock, with its event in hand, before the state it stands for is read.
 * @param id  The knock.
 * @param arg Receives what the first post carried, when a knock was pending; NULL to ignore it.
 * @return true when a knock was pending; false for an event that was heard already.
 */
bool radio_knocks_take(radio_knock_id_t id, uint8_t *arg);

/**
 * @brief Whether a knock waits to be taken, on AO_Radio's own thread.
 * @param id The knock.
 * @return true when its event is in the queue or owed: it is handled later, for certain.
 */
bool radio_knocks_pending(radio_knock_id_t id);

#endif /* RADIO_KNOCKS_H_ */
