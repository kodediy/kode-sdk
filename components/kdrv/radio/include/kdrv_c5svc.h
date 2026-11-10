/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_c5svc.h
 * @brief The P4's end of the KODE services on the C5 (kode_wire.h).
 */
#ifndef KDRV_C5SVC_H_
#define KDRV_C5SVC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"
#include "kode_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A service's answer.
 *
 * Runs on esp-hosted's thread: copy what is needed, post, return.
 *
 * It also carries a service's events: a frame whose op has KODE_OP_EVENT
 * reaches the listener whether or not a request is in flight, and never
 * completes one. An event for a service with no listener is dropped.
 *
 * @param service KODE_SVC_*.
 * @param op      The request's op, or the event's (KODE_OP_EVENT set).
 * @param status  KODE_ST_*.
 * @param data    Payload; valid only during the call.
 * @param len     Payload bytes.
 * @param ctx     What kdrv_c5svc_listen() was given.
 */
typedef void (*kdrv_c5svc_cb_t)(uint8_t service, uint8_t op, uint8_t status,
                                const uint8_t *data, size_t len, void *ctx);

/**
 * @brief Register the transport with the C5 link. Idempotent.
 * @return KODE_OK, or KODE_NO_RESOURCES when no buffer or peer slot is left.
 */
kode_status_t kdrv_c5svc_init(void);

/**
 * @brief Route a service's answers to cb; NULL stops listening.
 * @param service KODE_SVC_*.
 * @param cb      The listener, or NULL.
 * @param ctx     Handed back to cb.
 * @return KODE_OK, or KODE_DENIED for a service out of range.
 */
kode_status_t kdrv_c5svc_listen(uint8_t service, kdrv_c5svc_cb_t cb, void *ctx);

/**
 * @brief Send one request; the answer arrives through the service's listener.
 *
 * Returns after one RPC round trip, which can wait behind a long one: call
 * it from a worker task, never from an Active Object's step.
 *
 * @param service KODE_SVC_*.
 * @param op      Per service.
 * @param payload The request's payload, or NULL when len is 0.
 * @param len     0..KODE_MAX_PAYLOAD.
 * @return KODE_OK sent; KODE_DENIED for the link down, a bad argument or a
 *         request already in flight for this service; KODE_NO_RESOURCES
 *         before kdrv_c5svc_init(); KODE_IO_ERROR when the link refused it.
 */
kode_status_t kdrv_c5svc_send(uint8_t service, uint8_t op,
                              const void *payload, size_t len);

/**
 * @brief Send and wait for the answer. NEVER from an Active Object.
 *
 * @param service    KODE_SVC_*.
 * @param op         Per service.
 * @param payload    As kdrv_c5svc_send().
 * @param len        As kdrv_c5svc_send().
 * @param status     Out: the answer's KODE_ST_*.
 * @param out        Out: the payload, truncated to cap; may be NULL if cap is 0.
 * @param cap        Bytes available in out.
 * @param out_len    Out: payload bytes copied.
 * @param timeout_ms How long to wait.
 * @return KODE_OK answered; KODE_TIMEOUT no answer in time (the request is
 *         abandoned); otherwise as kdrv_c5svc_send().
 */
kode_status_t kdrv_c5svc_call(uint8_t service, uint8_t op,
                              const void *payload, size_t len,
                              uint8_t *status, void *out, size_t cap,
                              size_t *out_len, uint32_t timeout_ms);

/**
 * @brief Give up on a service's request in flight; a late answer is dropped.
 * @param service KODE_SVC_*.
 */
void kdrv_c5svc_abandon(uint8_t service);

/**
 * @brief Forget every request in flight and the last INFO.
 *
 * For a link that went down: whatever was asked will not be answered.
 * Listeners stay.
 */
void kdrv_c5svc_forget(void);

/**
 * @brief The C5's last INFO answer, kept for whoever reads it (`info`).
 * @param out Out: the answer, when there is one.
 * @return true when an INFO in this build's wire version arrived since the
 *         link was last forgotten.
 */
bool kdrv_c5svc_info(kode_info_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_C5SVC_H_ */
