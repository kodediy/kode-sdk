/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kaos_ask.h
 * @brief Package-private: an owner's doors for kode_sys's requests, one per asker and kind.
 */
#ifndef KAOS_ASK_H_
#define KAOS_ASK_H_

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "kode_defs.h"
#include "kqp.h"

/** @brief Rows of doors: one per QP priority, 1..QF_MAX_ACTIVE, indexed by it. */
#define KAOS_ASKERS  (QF_MAX_ACTIVE + 1U)

/** @brief One kind of ask: an asker's door is shut while its ask waits for the answer. */
typedef struct {
    atomic_bool shut[KAOS_ASKERS];  /**< Indexed by the asker's priority. */
} kaos_ask_door_t;

/**
 * @brief Take @p who's door.
 * @param door The kind's doors.
 * @param who  The asking AO.
 * @return KODE_OK; KODE_DENIED for a NULL or unstarted @p who, or its door shut.
 */
kode_status_t kaos_ask_take(kaos_ask_door_t *door, QActive const *who);

/**
 * @brief Open @p who's door.
 * @param door The kind's doors.
 * @param who  An asker whose door kaos_ask_take() shut.
 */
void kaos_ask_release(kaos_ask_door_t *door, QActive const *who);

/**
 * @brief Give back an ask never posted: its answer to the pool, its door open.
 * @param door The kind's doors.
 * @param who  The asking AO, its door shut.
 * @param rsp  The answer taken for it; NULL when the pool had none.
 * @return KODE_NO_RESOURCES.
 */
kode_status_t kaos_ask_cancel(kaos_ask_door_t *door, QActive const *who, QEvt const *rsp);

/**
 * @brief Post an ask to its owner, keeping the owner's margin.
 * @param door   The kind's doors.
 * @param who    The asking AO, its door shut.
 * @param owner  The owner AO.
 * @param req    The request.
 * @param rsp    Its answer.
 * @param margin Slots the owner's queue keeps free for its own traffic.
 * @return KODE_OK when queued; KODE_NO_RESOURCES otherwise, both given back.
 */
kode_status_t kaos_ask_post(kaos_ask_door_t *door, QActive const *who, QActive *owner,
                            QEvt const *req, QEvt const *rsp, uint_fast16_t margin);

/**
 * @brief Open @p who's door, then post it the answer with no margin.
 *
 * Its door gives the asker one answer of a kind in flight; a queue with no
 * slot for it is the asker's design error, which QP asserts.
 * @param door  The kind's doors.
 * @param who   The asking AO.
 * @param rsp   The answer, filled in.
 * @param owner The owner that answers.
 */
void kaos_ask_answer(kaos_ask_door_t *door, QActive *who, QEvt const *rsp,
                     QActive const *owner);

#endif /* KAOS_ASK_H_ */
