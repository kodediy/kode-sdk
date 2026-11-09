/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kir_wire.h
 * @brief The infrared service between the P4 and the ESP32-C5: the wire.
 *
 * One message id each way over esp-hosted's peer data transfer, and a first
 * byte that says which request or which event it is. One id rather than one
 * per verb because the slave's handler table is three entries long by
 * default and this is not the only thing that may want one.
 *
 * Every multi-byte field is little-endian, which is what both chips are; the
 * structs are packed and their sizes asserted, so a compiler that disagreed
 * would not build. THE SAME FILE LIVES IN TWO REPOSITORIES - the SDK's
 * kdrv/ir/include/ and the C5 firmware's main/ - and the two copies must be
 * identical: KIR_WIRE_VERSION goes up with any change, and the host refuses
 * a slave that answers with another one.
 */
#ifndef KIR_WIRE_H_
#define KIR_WIRE_H_

#include <stdint.h>

/** @brief Bumped on any change to this file. */
#define KIR_WIRE_VERSION   1U

/** @brief Host to C5: a request. 'K','I','R', 0x01. */
#define KIR_MSG_REQ        0x4B495201UL
/** @brief C5 to host: an event. */
#define KIR_MSG_EVT        0x4B495281UL

/** @brief Most symbols one request or one capture carries. */
#define KIR_MAX_SYMBOLS    256U

/* ---- Requests: the first byte of a KIR_MSG_REQ --------------------------- */
#define KIR_REQ_SEND       1U  /**< kir_send_t, then @c n symbols.            */
#define KIR_REQ_ARM        2U  /**< Arm the receiver for one capture.         */
#define KIR_REQ_IDLE       3U  /**< Read the receiver's level at rest.        */
#define KIR_REQ_PRESENT    4U  /**< Judge whether a receiver is fitted.       */

/* ---- Events: the first byte of a KIR_MSG_EVT ----------------------------- */
#define KIR_EVT_SENT       1U  /**< kir_status_t: the transmission ended.     */
#define KIR_EVT_CAPTURE    2U  /**< kir_capture_t, then @c n symbols.         */
#define KIR_EVT_LEVEL      3U  /**< kir_level_t: answer to IDLE or PRESENT.   */
#define KIR_EVT_ARMED      4U  /**< kir_status_t: answer to ARM.              */

/* ---- Status bytes --------------------------------------------------------- */
#define KIR_OK             0U  /**< Done.                                     */
#define KIR_FAIL           1U  /**< The peripheral refused.                   */
#define KIR_BAD_REQUEST    2U  /**< Malformed, or a count out of range.       */

/** @brief One mark (carrier on) and the space after it, microseconds. */
typedef struct __attribute__((packed)) {
    uint16_t mark_us;     /**< Carrier on.                                    */
    uint16_t space_us;    /**< Carrier off after it.                          */
} kir_symbol_t;

/** @brief KIR_REQ_SEND's header; @c n kir_symbol_t follow it. */
typedef struct __attribute__((packed)) {
    uint8_t  op;          /**< KIR_REQ_SEND.                                  */
    uint8_t  version;     /**< KIR_WIRE_VERSION.                              */
    uint16_t n;           /**< 1..KIR_MAX_SYMBOLS.                            */
    uint32_t carrier_hz;  /**< The carrier the marks are modulated at.        */
} kir_send_t;

/** @brief The three requests that carry nothing but themselves. */
typedef struct __attribute__((packed)) {
    uint8_t op;           /**< KIR_REQ_ARM, _IDLE or _PRESENT.                */
    uint8_t version;      /**< KIR_WIRE_VERSION.                              */
} kir_plain_t;

/** @brief KIR_EVT_SENT and KIR_EVT_ARMED. */
typedef struct __attribute__((packed)) {
    uint8_t evt;          /**< KIR_EVT_SENT or KIR_EVT_ARMED.                 */
    uint8_t version;      /**< KIR_WIRE_VERSION.                              */
    uint8_t status;       /**< KIR_OK, KIR_FAIL or KIR_BAD_REQUEST.           */
} kir_status_t;

/** @brief KIR_EVT_CAPTURE's header; @c n kir_symbol_t follow it. */
typedef struct __attribute__((packed)) {
    uint8_t  evt;         /**< KIR_EVT_CAPTURE.                               */
    uint8_t  version;     /**< KIR_WIRE_VERSION.                              */
    uint16_t n;           /**< 0..KIR_MAX_SYMBOLS.                            */
} kir_capture_t;

/** @brief KIR_EVT_LEVEL. */
typedef struct __attribute__((packed)) {
    uint8_t evt;          /**< KIR_EVT_LEVEL.                                 */
    uint8_t version;      /**< KIR_WIRE_VERSION.                              */
    uint8_t op;           /**< The request this answers: _IDLE or _PRESENT.   */
    uint8_t status;       /**< KIR_OK or KIR_FAIL.                            */
    uint8_t high;         /**< 1 when the line read high.                     */
} kir_level_t;

_Static_assert(sizeof(kir_symbol_t)  == 4U, "kir_symbol_t is four bytes on the wire");
_Static_assert(sizeof(kir_send_t)    == 8U, "kir_send_t is eight bytes on the wire");
_Static_assert(sizeof(kir_plain_t)   == 2U, "kir_plain_t is two bytes on the wire");
_Static_assert(sizeof(kir_status_t)  == 3U, "kir_status_t is three bytes on the wire");
_Static_assert(sizeof(kir_capture_t) == 4U, "kir_capture_t is four bytes on the wire");
_Static_assert(sizeof(kir_level_t)   == 5U, "kir_level_t is five bytes on the wire");

#endif /* KIR_WIRE_H_ */
