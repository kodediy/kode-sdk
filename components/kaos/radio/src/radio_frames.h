/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file radio_frames.h
 * @brief Package-private: AO_Radio's Wi-Fi frames door, the asker and what it waits for.
 *
 * The C5 hears Wi-Fi frames on request and sends them in batches
 * (kdrv_frames decodes them). This module is the part of AO_Radio that offers
 * that to one asker at a time: it keeps who asked and which request of the
 * service waits for its answer, and posts what the C5 says to the asker. The
 * AO keeps the link, and sends every request through its own KODE service
 * path, which this module reaches by the function it was handed.
 */
#ifndef RADIO_FRAMES_H_
#define RADIO_FRAMES_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"
#include "kqp.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief How a request of the frames service goes out; AO_Radio's, with its deadline.
 * @param ctx     What radio_frames_init() was given.
 * @param op      KODE_SNIFF_START or KODE_SNIFF_STOP.
 * @param payload The request's payload; NULL when @p len is 0.
 * @param len     Its bytes.
 * @return KODE_OK when the request is on its way; KODE_DENIED when a request
 *         of the service already waits; another status when it cannot go.
 */
typedef kode_status_t (*RadioFramesSend)(void *ctx, uint8_t op, const void *payload, size_t len);

/** @brief Which request of the service waits for its answer (RadioFrames.wait). */
/* cppcheck-suppress misra-c2012-2.4 ; the type names the values of a uint8_t field, which keeps the door its size */
typedef enum {
    RADIO_FRAMES_WAIT_NONE = 0,  /**< None.                                         */
    RADIO_FRAMES_WAIT_START,     /**< The asker's START.                            */
    RADIO_FRAMES_WAIT_STOP,      /**< The asker's STOP.                             */
    RADIO_FRAMES_WAIT_QUIET,     /**< A STOP whose answer nobody waits for: a batch came with no
                                  *   asker, or the asker's stream was ended by AO_Radio. */
    RADIO_FRAMES_WAIT_ORPHAN     /**< A START whose asker's stream AO_Radio ended while it waited:
                                  *   if the C5 says it listens, a quiet STOP follows. */
/* cppcheck-suppress misra-c2012-2.3 ; the type names the values of a uint8_t field, which keeps the door its size */
} radio_frames_wait_t;

/** @brief The door's state; AO_Radio keeps one. */
typedef struct {
    QActive        *who;        /**< The asker, from its START until its STOP is answered; NULL: none. */
    uint8_t         wait;       /**< radio_frames_wait_t.                          */
    bool            ring_ok;    /**< kdrv_frames_init() worked on this link.       */
    bool            stop_owed;  /**< The asker stopped while its START waited.     */
    uint32_t        lost;       /**< Frames the asker's queue had no room for, since the START. */
    RadioFramesSend send;       /**< How a request goes out.                       */
    void           *ctx;        /**< Its first argument.                           */
} RadioFrames;

/**
 * @brief Put the door in its first state.
 * @param f    The door.
 * @param send How a request goes out.
 * @param ctx  Its first argument.
 */
void radio_frames_init(RadioFrames *f, RadioFramesSend send, void *ctx);

/**
 * @brief The link is up: take the ring the C5's batches are held in.
 * @param f The door.
 */
void radio_frames_link(RadioFrames *f);

/**
 * @brief Take a request with the link up, and answer it as the case is.
 * @param f The door.
 * @param e The request.
 */
void radio_frames_request(RadioFrames *f, WifiFramesReqEvt const *e);

/**
 * @brief Answer a request without taking it: not listening, and why.
 *
 * For a state with no link (KWIFI_FRAMES_NO_LINK), a board at rest
 * (KWIFI_FRAMES_RESTING) and a radio busy with work a stream would disturb
 * (KWIFI_FRAMES_BUSY). The door is not touched.
 *
 * @param e   The request.
 * @param why The reason.
 */
void radio_frames_refuse(WifiFramesReqEvt const *e, kwifi_frames_why_t why);

/**
 * @brief AO_Radio ends the asker's stream itself, before work that would
 *        leave it on with nothing listening: the board's rest, Wi-Fi stopped
 *        or restarted, a C5 update.
 *
 * The STOP goes out when the service has no request waiting (best effort: a
 * batch that still comes with nobody asking quiets the C5); a START or a STOP
 * that waits has its answer made nobody's. The door is freed, the ring
 * forgotten and the asker told `{on false, why}` at once. Runs in AO_Radio,
 * before the work is handed to a worker. With nobody asking it changes
 * nothing.
 *
 * @param f   The door.
 * @param why What the asker is told.
 * @return true while the C5 may still be listening for this chip: there was
 *         a stream or a start to end, or a request of the service (a quiet
 *         STOP included) still waits for its answer.
 */
bool radio_frames_end(RadioFrames *f, kwifi_frames_why_t why);

/**
 * @brief The frames service answered the request in flight.
 * @param f       The door.
 * @param status  KODE_ST_*.
 * @param payload The answer's payload.
 * @param len     Its bytes.
 */
void radio_frames_answer(RadioFrames *f, uint8_t status, const uint8_t *payload, size_t len);

/**
 * @brief The request in flight will not be answered: it expired, or could not be sent.
 * @param f The door.
 */
void radio_frames_lost(RadioFrames *f);

/**
 * @brief The link is gone: tell the asker, forget the ring, free the door.
 * @param f The door.
 */
void radio_frames_gone(RadioFrames *f);

/**
 * @brief The knock: a batch waits in the ring. Hand it to the asker, or, with
 *        none, send one stop.
 * @param f The door.
 */
void radio_frames_knock(RadioFrames *f);

/** @brief A knock in a state with no link: the ring is emptied and nobody is told. */
void radio_frames_drop(void);

/**
 * @brief The C5 sent an event of the frames service. Runs on esp-hosted's thread.
 *
 * A batch is decoded into the ring and, when taken, AO_Radio is knocked with
 * a static event posted with a margin. Any other event is dropped.
 *
 * @param op   The event's op.
 * @param data Its payload; valid only during the call.
 * @param len  Its bytes.
 */
void radio_frames_heard(uint8_t op, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* RADIO_FRAMES_H_ */
