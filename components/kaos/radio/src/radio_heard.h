/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file radio_heard.h
 * @brief Package-private: AO_Radio's BLE advertisement door, the asker and the hear job that serves it.
 *
 * The host stack hears BLE advertisements while a passive scan with no end
 * runs (kdrv_ble_hear(), one job of the radio worker), and keeps them in
 * the ring of kdrv_ble_heard.h. This module is the part of AO_Radio that offers
 * that to one asker at a time: it keeps who asked and which hear job is
 * with the worker, and posts what the ring holds to the asker. The AO keeps
 * the link, and hands every job to its own worker through the function this
 * module was given. It mirrors radio_frames.h, the Wi-Fi frames door.
 */
#ifndef RADIO_HEARD_H_
#define RADIO_HEARD_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode_defs.h"
#include "kdrv_ble.h"
#include "kqp.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief How a hear job goes to the worker; AO_Radio's radio_submit_req().
 * @param ctx What radio_heard_init() was given.
 * @param on  true for a start, false for a stop.
 * @return KODE_OK when the job is with the worker; KODE_NO_RESOURCES when the
 *         worker is inside another job; another status when it would not
 *         take the job.
 */
typedef kode_status_t (*RadioHeardSubmit)(void *ctx, bool on);

/** @brief The door's state; AO_Radio keeps one. */
typedef struct {
    QActive         *who;       /**< The asker, from its start until its stream ends; NULL: none. */
    bool             scanning;  /**< The stream's passive scan is on, or may be.                  */
    bool             job;       /**< A hear job (start or stop) is on the worker.                 */
    bool             start_owed;/**< The asker's start waits for the worker.                      */
    bool             stop_owed; /**< The asker stopped while its start's job ran.                 */
    uint16_t         margin;    /**< Slots a record leaves free in the asker's queue.             */
    uint32_t         lost;      /**< Records the asker's queue or the pool had no room for.       */
    RadioHeardSubmit submit;    /**< How a hear job goes out.                                     */
    void            *ctx;       /**< Its first argument.                                          */
} RadioHeard;

/**
 * @brief Put the door in its first state.
 * @param h      The door.
 * @param submit How a hear job goes out.
 * @param ctx    Its first argument.
 */
void radio_heard_init(RadioHeard *h, RadioHeardSubmit submit, void *ctx);

/**
 * @brief Take a request with the link up, and answer it as the case is.
 *
 * A start takes the ring on the first one that gets this far, empties it and
 * hands the worker one hear job; the asker is told `{on true, OK}` when the
 * job answers, and what the scan heard meanwhile comes after that answer.
 * With the worker inside another job the start waits for it
 * (radio_heard_settle()) and answers then: AO_Radio turns away, before this
 * call, a start asked behind a job it does not wait for. A stop hands the
 * worker one stop job and is answered with the totals when that job answers.
 * Another asker's start takes the stream: the holder is told
 * `{on false, KBLE_HEARD_CANCELLED}` and the passive scan goes on for the new
 * one. A stop from an asker that does not hold the stream is answered
 * `{on false, OK}` and touches nothing.
 *
 * @param h The door.
 * @param e The request.
 */
void radio_heard_request(RadioHeard *h, BleHeardReqEvt const *e);

/**
 * @brief Answer a request without taking it: not listening, and why.
 *
 * For a state with no link (KBLE_HEARD_NO_LINK) and for a start AO_Radio turns
 * away itself (Bluetooth off, busy, resting). The door is not touched.
 *
 * @param e   The request.
 * @param why The reason.
 */
void radio_heard_refuse(BleHeardReqEvt const *e, kble_heard_why_t why);

/**
 * @brief A hear job answered.
 * @param h  The door.
 * @param on What the job was: true a start, false a stop.
 * @param rc The worker's status.
 */
void radio_heard_done(RadioHeard *h, bool on, kode_status_t rc);

/**
 * @brief AO_Radio ends the asker's stream itself, before work that would
 *        leave it on with nothing listening: Bluetooth switched off, the
 *        board's rest, a C5 update.
 *
 * The asker is told `{on false, why}` at once with the totals so far, the
 * door is freed and the ring forgotten. The stop goes out as a job when the
 * worker is free (best effort); with nobody asking, a passive scan that is still
 * on is stopped the same way. Runs in AO_Radio, before the work is handed to
 * the worker.
 *
 * @param h   The door.
 * @param why What the asker is told.
 * @return true while the passive scan may still be on for this chip: a job (a
 *         start's or a stop) is with the worker, or the stop was not submitted.
 */
bool radio_heard_end(RadioHeard *h, kble_heard_why_t why);

/**
 * @brief A newer Bluetooth request has the radio: a listen somebody asked
 *        for, or a job that starts a scan of its own.
 *
 * The asker is told `{on false, KBLE_HEARD_CANCELLED}` at once with the
 * totals so far, the door is freed and the ring forgotten; a start that
 * waited for the worker is forgotten with it. No stop job is sent: the job
 * that follows cancels the passive scan in the driver as it starts
 * (kdrv_ble_scan(), kdrv_ble_scan_find()). If that job never runs, the next
 * record finds nobody asking and stops the scan (radio_heard_knock()). A
 * start still with the worker is stopped when it answers. With a stop on the
 * worker nothing changes: its answer stands. With nobody asking it only
 * notes that the scan is going.
 *
 * @param h The door.
 */
void radio_heard_cancel(RadioHeard *h);

/**
 * @brief The worker is back: hand it the start that waited for it.
 *
 * A worker that will not take the job tells the asker
 * `{on false, KBLE_HEARD_BUSY}` and frees the door.
 *
 * @param h The door.
 * @return true when the start's job is with the worker.
 */
bool radio_heard_settle(RadioHeard *h);

/**
 * @brief Whether the asker's start waits for the worker.
 * @param h The door.
 * @return true while radio_heard_settle() has a start to hand over.
 */
bool radio_heard_waits(RadioHeard const *h);

/**
 * @brief The stack was made quiet or stopped: nothing listens any more.
 *
 * kdrv_ble_quiet() and kdrv_ble_stop() end a passive scan and say nothing, so this
 * is the one place the door learns it. An asker still held is told
 * `{on false, KBLE_HEARD_BLE_OFF}`.
 *
 * @param h The door.
 */
void radio_heard_quiet(RadioHeard *h);

/**
 * @brief The link is gone: tell the asker, forget the ring, free the door. No job is sent.
 * @param h The door.
 */
void radio_heard_gone(RadioHeard *h);

/**
 * @brief The knock: records wait in the ring, or the stack ended the passive scan.
 *
 * Hands every record to the asker, unless its start is still with the
 * worker: the records wait then for the start's answer. With no asker,
 * empties the ring and, when the scan is on, sends one stop job. A scan the
 * stack ended tells the asker `{on false, KBLE_HEARD_REFUSED}` and frees
 * the door.
 *
 * @param h The door.
 */
void radio_heard_knock(RadioHeard *h);

/** @brief A knock in a state with no link: the ring is emptied and nobody is told. */
void radio_heard_drop(void);

/**
 * @brief The hear hook: a record waits, or the stack ended the passive scan.
 *        Runs on NimBLE's host task.
 *
 * Posts AO_Radio's knock for the stream (radio_knocks.h): one waits at a
 * time, since its handler drains every record the ring holds, and a queue
 * with no room owes it, so the end of the scan is told as soon as there is
 * room. Installed with kdrv_ble_set_hear_cb().
 *
 * @param note KDRV_BLE_HEAR_RECORD or KDRV_BLE_HEAR_ENDED.
 */
void radio_heard_note(kdrv_ble_hear_note_t note);

#ifdef __cplusplus
}
#endif

#endif /* RADIO_HEARD_H_ */
