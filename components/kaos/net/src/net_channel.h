/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file net_channel.h
 * @brief The Kode channel's pure logic: reading answers, building bodies, and when to retry.
 */
#ifndef NET_CHANNEL_H_
#define NET_CHANNEL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kdrv_net.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The last will the broker publishes when the session drops. */
#define KCHAN_WILL  "{\"online\":false}"

/** @brief The shortest wait before a retry, in milliseconds. */
#define KCHAN_BACKOFF_MIN_MS   5000U

/** @brief The longest wait before a retry, in milliseconds (before jitter). */
#define KCHAN_BACKOFF_MAX_MS   600000U

/** @brief The longest `shots` upload prefix kept, without its terminator. */
#define KSHOT_PREFIX_MAX   128U

/** @brief What the api's connect answer describes: one MQTT session. */
typedef struct {
    char serial[24];                      /**< The unit's serial.           */
    char broker[KDRV_MQTT_URI_MAX];       /**< `mqtts://host:port`.         */
    char user[KDRV_MQTT_USER_MAX];        /**< The session's user name.     */
    char client_id[48];                   /**< As the api issued it.        */
    char token[KDRV_MQTT_TOKEN_MAX];      /**< The password, a JWT.         */
    char t_in[KDRV_MQTT_TOPIC_MAX];       /**< Commands arrive here.        */
    char t_out[KDRV_MQTT_TOPIC_MAX];      /**< Replies go out here.         */
    char t_status[KDRV_MQTT_TOPIC_MAX];   /**< Retained presence.           */
    char shots[KSHOT_PREFIX_MAX + 1U];    /**< Where screen captures are
                                           *   uploaded (`https://…/`); empty
                                           *   when the api named none.      */
} kchan_session_t;

/** @brief Whether a failed request is worth repeating. */
typedef enum {
    KCHAN_RETRY = 0,   /**< Back off and ask again.                         */
    KCHAN_TERMINAL     /**< The unit's identity was refused: asking again
                        *   changes nothing, so wait for a new one.         */
} kchan_verdict_t;

/**
 * @brief Find the string value of a key in a JSON text.
 *
 * Only a string followed by a colon is a key, so a value that spells a key
 * never matches. `\"`, `\\` and `\/` are unescaped; any other escape, or a
 * control character, is refused.
 *
 * @param json The text, not necessarily NUL-terminated.
 * @param len  Its length.
 * @param key  The key, without quotes.
 * @param out  Receives the value and a terminator.
 * @param cap  Bytes @p out holds; the value may be at most cap - 1 long.
 * @return true when the key was found with a string value that fit. On false
 *         @p out holds the empty string.
 */
bool kchan_json_str(const char *json, size_t len, const char *key,
                    char *out, size_t cap);

/**
 * @brief Find the whole-number value of a key in a JSON text.
 *
 * The value is decimal digits that fit 32 bits, followed by `,`, `}`, `]` or
 * white space. A sign, a fraction, an exponent, a string, or a number at the
 * end of the text (it may have been cut) is refused.
 *
 * @param json The text, not necessarily NUL-terminated.
 * @param len  Its length.
 * @param key  The key, without quotes.
 * @param out  Receives the value; 0 on false.
 * @return true when the key was found with such a value.
 */
bool kchan_json_u32(const char *json, size_t len, const char *key,
                    uint32_t *out);

/**
 * @brief Read the connect answer into a session.
 *
 * All eight fields (`serial`, `brokerUrl`, `username`, `clientId`, `token`,
 * `in`, `out`, `status`) must be present, non-empty and fit. The broker must be
 * `mqtts://`, the serial this unit's when @p own is known, and no topic may hold
 * a wildcard. Anything else is refused whole. `shots` is optional: kept when
 * kchan_shots_valid() accepts it, left empty otherwise.
 *
 * @param json The answer's body, not necessarily NUL-terminated.
 * @param len  Its length.
 * @param own  This unit's serial; NULL or empty when it is not known.
 * @param out  Receives the session; zeroed on false.
 * @return true when the answer describes a complete session for this unit.
 */
bool kchan_parse_connect(const char *json, size_t len, const char *own,
                         kchan_session_t *out);

/**
 * @brief Whether a `shots` prefix may be uploaded to.
 *
 * It must be TLS (`https://`) with a host after the scheme (a byte other
 * than `/`), end in `/` so a file name can follow, fit `KSHOT_PREFIX_MAX`
 * bytes and hold no byte below 0x21 (no space, no control character).
 *
 * @param prefix The prefix; NULL is refused.
 * @return true when the prefix is acceptable.
 */
bool kchan_shots_valid(const char *prefix);

/**
 * @brief Read the challenge answer's nonce.
 *
 * @param json  The answer's body, not necessarily NUL-terminated.
 * @param len   Its length.
 * @param nonce Receives the nonce and a terminator.
 * @param cap   Bytes @p nonce holds.
 * @return true when `nonce` is present and inside `[A-Za-z0-9._-]{1,200}`.
 *         On false @p nonce holds the empty string.
 */
bool kchan_parse_nonce(const char *json, size_t len, char *nonce, size_t cap);

/**
 * @brief Build the connect request's body:
 *        `{"cert":"…","nonce":"…","signature":"…"}`.
 *
 * @param out      Receives the body and a terminator.
 * @param cap      Bytes @p out holds.
 * @param cert_b64 The unit certificate, base64.
 * @param nonce    The challenge's nonce.
 * @param sig_b64  The signature over the nonce, base64.
 * @return The body's length, or 0 when it does not fit or an argument holds
 *         a character JSON would have to escape.
 */
size_t kchan_connect_body(char *out, size_t cap, const char *cert_b64,
                          const char *nonce, const char *sig_b64);

/**
 * @brief Build the retained presence message:
 *        `{"online":true,"app":"<app>","version":"<v>"}`.
 *
 * @param out     Receives the body and a terminator.
 * @param cap     Bytes @p out holds.
 * @param app     The running firmware's project name.
 * @param version The firmware version.
 * @return The body's length, or 0 when it does not fit or @p app or
 *         @p version holds a character JSON would have to escape.
 */
size_t kchan_status_body(char *out, size_t cap, const char *app,
                         const char *version);

/**
 * @brief Decide whether a failed request is worth repeating.
 *
 * @param http_status The HTTP status, 0 when none arrived.
 * @param error_word  The api's `error` word, or NULL or "" when it sent none.
 * @return KCHAN_TERMINAL for a 401 with `bad_cert`, `untrusted_cert`,
 *         `cert_expired` or `not_client_cert`; KCHAN_RETRY for all else.
 */
kchan_verdict_t kchan_classify(uint16_t http_status, const char *error_word);

/**
 * @brief How long to wait before attempt number @p attempt + 1.
 *
 * `KCHAN_BACKOFF_MIN_MS` doubled per attempt up to `KCHAN_BACKOFF_MAX_MS`,
 * then scaled to 75..125 % by @p rnd so a fleet that lost the broker together
 * does not return together.
 *
 * @param attempt Failed attempts so far in a row; 0 is the first retry.
 * @param rnd     Any random number.
 * @return The wait in milliseconds.
 */
uint32_t kchan_backoff_ms(uint8_t attempt, uint32_t rnd);

/* ---- The reply ring -------------------------------------------------------
 * Replies to cmd/in commands wait here between AO_Log (the one producer) and
 * AO_Net (the one consumer), which publishes them at esp-mqtt's pace. Records
 * are a two-byte length and the line, in PSRAM, under one mutex. A line that
 * does not fit is counted as lost.
 */

/** @brief The ring's size in bytes (PSRAM, taken with the channel's work area). */
#define KCHAN_OUT_BYTES   (32U * 1024U)

/**
 * @brief The function the ring calls when it stops being empty.
 *
 * It must only post: it runs on the producer's task, outside the ring's lock.
 *
 * @return true when the consumer was told, false when it could not be (the
 *         ring then tries again with the next line).
 */
typedef bool (*kchan_out_wake_t)(void);

/**
 * @brief Take the ring's memory, if it has not been taken yet.
 *
 * Safe to call again: a ring that exists is left as it is.
 *
 * @return true when the ring exists afterwards, false when PSRAM or the lock
 *         could not be had.
 */
bool kchan_out_init(void);

/**
 * @brief Set who is told when the ring stops being empty.
 *
 * @param wake The function, or NULL for nobody.
 */
void kchan_out_set_wake(kchan_out_wake_t wake);

/**
 * @brief Append one reply line, without its newline.
 *
 * Calls the wake function when the ring was empty (or the last call could
 * not tell the consumer). A line that does not fit, is longer than
 * `KODE_LOG_LINE_MAX`, or arrives before the ring exists is counted as lost.
 *
 * @param line The line; not NUL-terminated necessarily.
 * @param len  Its length, 1 to `KODE_LOG_LINE_MAX`.
 * @return true when the line is in the ring.
 */
bool kchan_out_push(const char *line, size_t len);

/**
 * @brief Take the oldest line out of the ring.
 *
 * @param line Receives the line and a terminator.
 * @param cap  Bytes @p line holds; a longer line is cut to cap - 1.
 * @return The line's length, 0 when the ring is empty.
 */
size_t kchan_out_pop(char *line, size_t cap);

/** @brief Whether a line is waiting. */
bool kchan_out_pending(void);

/** @brief Lines lost since boot: refused by a full ring, or dropped by AO_Net. */
uint32_t kchan_out_lost(void);

/**
 * @brief Count lines AO_Net could not publish as lost.
 *
 * @param n How many.
 */
void kchan_out_lose(uint32_t n);

/** @brief Drop every waiting line, counting each as lost. */
void kchan_out_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* NET_CHANNEL_H_ */
