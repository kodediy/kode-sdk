/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kmirror_proto.h
 * @brief The mirror's wire format and its flow control, with no hardware in it.
 */
#ifndef KMIRROR_PROTO_H_
#define KMIRROR_PROTO_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The frame header, on the wire: little-endian, 14 bytes, no padding. */
#define KMIRROR_HDR_LEN     14U

/** @brief What precedes every frame's JPEG bytes. */
typedef struct {
    uint16_t seq;      /**< Wraps at 65535; an ack names this.           */
    uint16_t x;        /**< Where this rectangle sits on the panel.      */
    uint16_t y;
    uint16_t w;        /**< Its size, already scaled.                    */
    uint16_t h;
    uint32_t len;      /**< JPEG bytes following the header.             */
} kmirror_hdr_t;

/** @brief The sender's half of the credit scheme. */
typedef struct {
    uint16_t next;     /**< The sequence the next frame will carry.      */
    uint16_t acked;    /**< The highest sequence the host has confirmed. */
    uint8_t  window;   /**< How many frames may be outstanding.          */
} kmirror_credit_t;

/**
 * @brief Serialise @p h into @p dst.
 * @param dst At least KMIRROR_HDR_LEN bytes.
 * @param h   The header to write; NULL is tolerated as a no-op.
 */
void kmirror_hdr_put(uint8_t *dst, const kmirror_hdr_t *h);

/**
 * @brief Parse KMIRROR_HDR_LEN bytes into @p h.
 * @param src KMIRROR_HDR_LEN bytes, little-endian.
 * @param h   Receives the parsed header.
 * @return false when either pointer is NULL.
 */
bool kmirror_hdr_get(const uint8_t *src, kmirror_hdr_t *h);

/**
 * @brief Start a credit window.
 * @param c      The window to initialise; NULL is tolerated as a no-op.
 * @param window Frames that may be in flight. Zero is corrected to one; a
 *               window of zero would stall the stream forever.
 */
void kmirror_credit_init(kmirror_credit_t *c, uint8_t window);

/**
 * @brief Whether another frame may go out now.
 * @param c The window to inspect.
 * @return true when fewer frames are in flight than the window allows.
 */
bool kmirror_may_send(const kmirror_credit_t *c);

/**
 * @brief How many frames are out but not yet acknowledged.
 * @param c The window to inspect; NULL returns 0 rather than crashing.
 * @return Never more than @c c->window - kmirror_may_send() is what keeps
 *         it that way. A diagnostic (mirror_info) is the reason this is
 *         public: the sender itself never needed to ask, only to compare
 *         against the window, which kmirror_may_send() already does.
 */
uint16_t kmirror_in_flight(const kmirror_credit_t *c);

/**
 * @brief Take the next sequence number and count the frame as in flight.
 * @param c The window to draw from; NULL returns 0 rather than crashing -
 *          there is no sequence number that would signal the error back to
 *          a caller that already broke the contract, so this only stops the
 *          fault from reaching the caller as a fault of its own.
 * @return The sequence number the caller must stamp on the frame it sends.
 */
uint16_t kmirror_sent(kmirror_credit_t *c);

/**
 * @brief Record the host's acknowledgement of @p seq.
 * @param c   The window to update.
 * @param seq The sequence number the host says it received.
 *
 * The ack is cumulative: acknowledging the newest of several outstanding
 * frames retires every older one along with it, the way TCP's does. A caller
 * that expects one ack per frame will misread the window - only the most
 * recent ack the host sends need ever arrive.
 *
 * @return false when @p seq is not one this sender has issued and not yet
 *         retired - a stale ack from a previous run, or a host inventing
 *         numbers. Refusing rather than trusting is what stops a bad ack
 *         from opening the window indefinitely.
 */
bool kmirror_ack(kmirror_credit_t *c, uint16_t seq);

/**
 * @brief Turn a dirty band into a scaled, clamped rectangle.
 *
 * @param y1     First dirty row, inclusive.
 * @param y2     Last dirty row, exclusive. @p y1 >= @p y2 is treated as an
 *               empty band (zero height) rather than an error.
 * @param scale  1 for full size, 2 for half. Zero is corrected to one, the
 *               same way a zero credit window is - it has no meaning as a
 *               divisor and silently refusing to scale is worse than
 *               ignoring it.
 * @param v_res  The panel's height.
 * @param out_y  Receives the scaled top row.
 * @param out_h  Receives the scaled height; zero when the band is empty.
 */
void kmirror_band(int32_t y1, int32_t y2, uint32_t scale, uint32_t v_res,
                  uint32_t *out_y, uint32_t *out_h);

#ifdef __cplusplus
}
#endif

#endif /* KMIRROR_PROTO_H_ */
