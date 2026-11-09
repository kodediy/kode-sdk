/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_net.h
 * @brief The host network stack: sockets, ICMP, HTTP(S) and MQTT.
 */
#ifndef KDRV_NET_H_
#define KDRV_NET_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Bounds -------------------------------------------------------------
 * These have twins in kqp_config.h (KODE_NET_*, KODE_MQTT_*), because the
 * events that carry these payloads are sized there and a driver cannot
 * include the scheduler's configuration. The two halves must agree; each
 * side's comment names the other.
 */
#define KDRV_NET_HOST_MAX       64U   /**< Host name or literal address.     */
#define KDRV_NET_URL_MAX        80U   /**< A URL, console-sized.             */
#define KDRV_NET_UDP_MAX        64U   /**< Datagram payload, console path.   */
#define KDRV_NET_PING_MAX       10U   /**< Echoes one ping may report.       */
/** Body bytes the worker keeps. Not a round number: it is exactly what the
 *  event bus can carry back - 8 records of 192 bytes, each holding 191
 *  characters and a terminator (KODE_NET_HTTP_CHUNK[S] in kqp_config.h).
 *  Keeping more would mean keeping bytes nobody could ever be shown, which
 *  would make the `shown=` count a lie. */
#define KDRV_NET_BODY_MAX     1528U
#define KDRV_MQTT_URI_MAX       96U   /**< mqtt[s]://host:port               */
#define KDRV_MQTT_USER_MAX      33U
#define KDRV_MQTT_PASS_MAX      65U   /**< The bench verb's password.        */
/** The session's password buffer, terminator included: a token of up to 511
 *  characters. A JWT from the Kode channel measures 379. Only the session
 *  setter carries it - no event does - so it has no twin in kqp_config.h. */
#define KDRV_MQTT_TOKEN_MAX    512U
#define KDRV_MQTT_TOPIC_MAX     80U
/** Of one message, in or out: a whole console line. The twin of
 *  KODE_CONSOLE_LINE_MAX, and net_ao.c asserts it, because a command that
 *  arrives on a topic is cut here or nowhere. */
#define KDRV_MQTT_PAYLOAD_MAX  240U
#define KDRV_MQTT_CLIENT_ID_MAX 48U   /**< Of a session's client id.         */
#define KDRV_MQTT_WILL_MAX      64U   /**< Of a session's last-will message. */
/** Bytes of JSON body one HTTP_POST job can send. */
/* cppcheck-suppress misra-c2012-2.5 ; read by kdrv_net.c alone, no twin */
#define KDRV_NET_POST_MAX     1024U
/** The most kdrv_net_stream_open() may spend in total, all its I/O together.
 *  AO_Net's guard on the open job is this plus a margin, so the cap answers
 *  first. */
#define KDRV_NET_STREAM_OPEN_MAX_MS  20000U
/** The most one kdrv_net_stream_read() call may spend in total, all its
 *  rounds together, waiting for bytes that do not come. AO_Net's guard on the
 *  read job is this plus a margin. */
#define KDRV_NET_STREAM_READ_MAX_MS  20000U
/** Body bytes one HTTP_PUT job can send: a JPEG of the glass. */
/* cppcheck-suppress misra-c2012-2.5 ; read by kdrv_net.c alone, no twin */
#define KDRV_NET_PUT_MAX      (64U * 1024U)
/** A PUT's URL, terminator included: the Kode channel's upload link carries a
 *  token in its path, so it does not fit the console-sized KDRV_NET_URL_MAX.
 *  It holds the longest link the api may name (128) and the longest token
 *  (128) whole; AO_Net asserts that. */
/* cppcheck-suppress misra-c2012-2.5 ; read by kdrv_net.c and AO_Net, no twin */
#define KDRV_NET_PUT_URL_MAX  260U
/** The Authorization header's value, terminator included: "Bearer " and a JWT
 *  of up to 511 characters (the Kode channel's measures 379). */
/* cppcheck-suppress misra-c2012-2.5 ; read by kdrv_net.c alone, no twin */
#define KDRV_NET_AUTH_MAX     520U
/** A PUT's content type, terminator included. */
/* cppcheck-suppress misra-c2012-2.5 ; read by kdrv_net.c alone, no twin */
#define KDRV_NET_PUT_TYPE_MAX  32U

/* ---- The one-shots (blocking, reentrant, never from an AO) -------------- */

/**
 * @brief Resolve @p host to an IPv4 address.
 *
 * @param host Name or dotted-quad literal.
 * @param ip4  Out: the address, in network byte order (the form the whole
 *             system passes addresses in, WifiStateEvt included).
 * @return KODE_OK, KODE_ERROR when the name does not resolve.
 */
kode_status_t kdrv_net_lookup(const char *host, uint32_t *ip4);

/** @brief One ICMP echo's outcome. */
typedef struct {
    uint16_t seq;       /**< Sequence number the session used.             */
    uint16_t time_ms;   /**< Round trip; meaningless when !ok.             */
    uint8_t  ttl;       /**< As the reply reported it.                     */
    bool     ok;        /**< false: this one timed out.                    */
} kdrv_net_ping_reply_t;

/** @brief What a whole ping session added up to. */
typedef struct {
    uint32_t ip4;       /**< What the name resolved to.                    */
    uint16_t tx;        /**< Echoes sent.                                  */
    uint16_t rx;        /**< Replies received.                             */
    uint16_t min_ms;
    uint16_t avg_ms;
    uint16_t max_ms;
} kdrv_net_ping_stats_t;

/**
 * @brief Ping @p host @p count times, one echo per second. Blocks throughout.
 *
 * @param host    Name or literal; resolved first.
 * @param count   Echoes to send, 1..KDRV_NET_PING_MAX.
 * @param rows    Out: one entry per echo, in order. May be NULL when only the
 *                summary is wanted - which is what the facade wants.
 * @param cap     How many entries @p rows holds.
 * @param rows_written Out: how many of them were filled. May be NULL. This
 *                exists because it is not derivable from the summary: an
 *                echo whose send() failed outright is a row (a loss) that
 *                stats.tx never counted, so reading tx as a row count would
 *                hide exactly the failures worth seeing.
 * @param stats   Out: the summary. Required.
 * @return KODE_OK when the session ran (even if every echo timed out - that
 *         is an answer, and stats.rx says so); KODE_ERROR when the name did
 *         not resolve or the session could not be created; KODE_TIMEOUT when
 *         the session outlived its ceiling, in which case @p rows and
 *         @p stats hold nothing worth reading.
 */
kode_status_t kdrv_net_ping(const char *host, uint8_t count,
                            kdrv_net_ping_reply_t *rows, uint8_t cap,
                            uint8_t *rows_written,
                            kdrv_net_ping_stats_t *stats);

/** @brief What an HTTP request came back with. */
typedef struct {
    uint16_t status;          /**< HTTP status code; 0 if nothing answered. */
    int32_t  content_length;  /**< As declared; -1 when chunked or absent.  */
    uint32_t body_len;        /**< Bytes the body actually held.            */
    uint32_t kept;            /**< Bytes written into the caller's buffer.  */
    bool     truncated;       /**< body_len > kept: the rest was counted,
                               *   not stored. Saying so is the point.      */
} kdrv_net_http_result_t;

/**
 * @brief GET @p url into @p buf. Blocks. https:// verifies against the
 *        Mozilla certificate bundle.
 *
 * A 404 is a successful call: the server answered, and @c status carries what
 * it said. KODE_OK means the transport worked, nothing more - conflating the
 * two would leave a caller unable to tell "no such page" from "no network".
 *
 * @param buf        Out: the first @p cap bytes of the body. May be NULL.
 * @param cap        Room in @p buf.
 * @param out        Out: status, lengths and whether the body was cut.
 * @param timeout_ms Per-operation deadline handed to the client.
 * @return KODE_OK, KODE_TIMEOUT, or KODE_ERROR.
 */
kode_status_t kdrv_net_http_get(const char *url, uint8_t *buf, uint32_t cap,
                                kdrv_net_http_result_t *out,
                                uint32_t timeout_ms);

/**
 * @brief POST @p body to @p url and read the answer into @p buf. Blocks the
 *        caller's task, through the same exchange as kdrv_net_http_get().
 *
 * @param body       The request body; may be NULL only when @p len is 0.
 * @param len        Bytes of @p body; 0 sends an empty body.
 * @param ctype      Content-Type header, or NULL to send none.
 * @param buf        Out: the first @p cap bytes of the answer. May be NULL.
 * @param cap        Room in @p buf.
 * @param out        Out: status, lengths and whether the body was cut.
 * @param timeout_ms Per-operation deadline handed to the client.
 * @return KODE_OK, KODE_TIMEOUT, or KODE_ERROR (also for a NULL @p url or
 *         @p out, or a NULL @p body with @p len above 0).
 */
kode_status_t kdrv_net_http_post(const char *url, const char *body,
                                 uint32_t len, const char *ctype,
                                 uint8_t *buf, uint32_t cap,
                                 kdrv_net_http_result_t *out,
                                 uint32_t timeout_ms);

/* ---- A URL as a medium ---------------------------------------------------
 * Deliberately the shape of kdrv_storage_open/read/close rather than a
 * callback: esp_http_client is a pull API, so a download can be read a chunk
 * at a time by whoever wants it, and an image arriving over the network is
 * then the same conversation as one arriving off the card. One transfer at a
 * time, for the reason kdrv_storage gives for one open file: a second would
 * mean a second reader.
 */

/**
 * @brief Open @p url and read its headers.
 *
 * Blocks for the request and the response headers - at most
 * KDRV_NET_STREAM_OPEN_MAX_MS in total, each I/O given only the time left.
 * Never call from an Active Object: AO_Net runs it as a worker job.
 *
 * @param[out] total   Content-Length, or 0 when the server did not declare
 *                     one (a chunked response). A container carries its own
 *                     length, so 0 is only fatal for a raw .bin.
 * @param[out] status  The HTTP status code.
 * @return KODE_OK when the status is 200 and the body is ready to read;
 *         KODE_TIMEOUT when the total cap ran out before an I/O could
 *         start; KODE_DENIED when a transfer is already open; KODE_ERROR
 *         otherwise.
 */
kode_status_t kdrv_net_stream_open(const char *url, uint32_t *total,
                                   uint16_t *status);

/**
 * @brief Read the next piece of the open body.
 *
 * Blocks for at most KDRV_NET_STREAM_READ_MAX_MS in total.
 *
 * @param[out] buf  Where the bytes go.
 * @param      max  Room in @p buf.
 * @param[out] got  Bytes read; 0 means the body is finished.
 * @return KODE_OK, or KODE_TIMEOUT when the body stopped arriving.
 */
kode_status_t kdrv_net_stream_read(void *buf, size_t max, size_t *got);

/** @brief Close the transfer. Safe when nothing is open; may block while the
 *         connection is shut. */
void kdrv_net_stream_close(void);

/**
 * @brief Send one UDP datagram to @p host : @p port. Blocks (on the lookup).
 * @return KODE_OK when the datagram left, KODE_ERROR otherwise.
 */
kode_status_t kdrv_net_udp_send(const char *host, uint16_t port,
                                const void *data, uint16_t len);

/** @brief The station interface's addresses, all network byte order. */
typedef struct {
    uint32_t ip4;
    uint32_t gw4;
    uint32_t mask4;
    uint32_t dns4;
} kdrv_net_info_t;

/**
 * @brief Read the station's addresses. Cheap and non-blocking: any task.
 * @return KODE_OK, or KODE_DENIED when there is no station interface yet -
 *         which is the honest answer before a first join.
 */
kode_status_t kdrv_net_info(kdrv_net_info_t *out);

/* ---- The jobs ----------------------------------------------------------
 * One job at a time, because a job's results land in tables this driver owns
 * and a second would overwrite the first one's answer before AO_Net had
 * published it. The driver does not enforce that - it has no state about it -
 * AO_Net does: it hands one job to its worker, and the next only after the
 * done event of the last.
 */

/** @brief What a job can be asked to do. */
typedef enum {
    KDRV_NET_JOB_PING = 0,     /**< Ping req.host req.count times.          */
    KDRV_NET_JOB_HTTP_GET,     /**< GET req.url into this driver's buffer.  */
    KDRV_NET_JOB_HTTP_POST,    /**< POST the body kdrv_net_http_post_body()
                                *   stored to req.url; the answer lands where
                                *   GET's does.                             */
    KDRV_NET_JOB_HTTP_PUT,     /**< PUT what kdrv_net_http_put_prepare()
                                *   stored; req.keep leaves the connection open
                                *   for the next PUT to the same URL. The
                                *   answer lands where GET's does.            */
    KDRV_NET_JOB_HTTP_CLOSE,   /**< Close the connection a kept PUT left open;
                                *   answers with status 0 and
                                *   KODE_OK, also when nothing was open.      */
    KDRV_NET_JOB_UDP_SEND,     /**< req.data to req.host:req.port.          */
    KDRV_NET_JOB_MQTT_START,   /**< Create and start the client on req.uri,
                                *   with the session if one is set.        */
    KDRV_NET_JOB_MQTT_STOP,    /**< Stop and destroy it.                    */
} kdrv_net_job_t;

/** @brief One job, carrying copies of everything it needs (kodeOS rule 7). */
typedef struct {
    kdrv_net_job_t type;
    char           host[KDRV_NET_HOST_MAX];
    char           url[KDRV_NET_URL_MAX];
    uint16_t       port;
    uint8_t        count;                    /**< PING: how many echoes.    */
    uint16_t       len;                      /**< UDP_SEND: payload bytes.  */
    uint8_t        data[KDRV_NET_UDP_MAX];
    char           uri[KDRV_MQTT_URI_MAX];   /**< MQTT_START.               */
    char           user[KDRV_MQTT_USER_MAX];
    char           pass[KDRV_MQTT_PASS_MAX];
    bool           keep;                     /**< HTTP_PUT: leave the handle
                                              *   open after the answer.     */
} kdrv_net_req_t;

/**
 * @brief Run one job, now, on the calling task. Blocks for as long as the job
 *        takes.
 *
 * The body of a worker job: AO_Net calls it from its KqpWorker and reads the
 * result tables below once the done event arrives. The driver keeps no record
 * of a job running; calling it from two tasks at once would make them write
 * the same tables, and keeping them apart is the caller's.
 *
 * @param req The job. Not retained.
 * @return What the job came to: KODE_OK, KODE_TIMEOUT, KODE_ERROR and the
 *         rest of each call's own contract; KODE_DENIED for a NULL @p req or
 *         a job type this driver does not have.
 */
kode_status_t kdrv_net_run(const kdrv_net_req_t *req);

/* ---- The time -----------------------------------------------------------
 * The one thing on this board that can learn what day it is by itself.
 */

/**
 * @brief Start the time client, and keep it running.
 *
 * Idempotent: calling it again once started does nothing, which is what lets
 * AO_Net call it on every address it gets without counting them. The server
 * is `pool.ntp.org`.
 *
 * The answer arrives on the client's own thread, which is why @p on_sync
 * exists rather than a return value.
 *
 * @param[in] on_sync Called with the epoch seconds each time a server
 *                    answers. Runs off every AO's thread: it may set the
 *                    clock, which is one atomic write, and it may log. It
 *                    may not touch a queue or an event.
 * @return KODE_OK, or KODE_ERROR when the client would not start.
 */
kode_status_t kdrv_net_sntp_start(void (*on_sync)(uint64_t unix_s));

/**
 * @brief Ask for a fresh answer now rather than at the next interval.
 *
 * @return KODE_OK, or KODE_DENIED when the client is not running - which is
 *         what a board with no address looks like from here.
 */
kode_status_t kdrv_net_sntp_sync_now(void);

/* ---- Results of the last job -------------------------------------------
 * Valid between a job's end and the next kdrv_net_run(), which is exactly
 * the window AO_Net reads them in: from the worker's done event until it
 * hands the worker another job. The kdrv_wifi scan table, again.
 */

/** @brief How many echoes the last PING job reported. */
uint8_t kdrv_net_ping_count(void);

/** @brief Read one of them. @return false when @p idx is past the count. */
bool kdrv_net_ping_get(uint8_t idx, kdrv_net_ping_reply_t *out);

/** @brief The last PING job's summary. */
void kdrv_net_ping_stats(kdrv_net_ping_stats_t *out);

/** @brief The last HTTP_GET job's status and lengths. */
void kdrv_net_http_result(kdrv_net_http_result_t *out);

/**
 * @brief Copy body bytes the last HTTP_GET job kept.
 * @param off Offset into the kept body.
 * @param dst Out.
 * @param cap Room in @p dst.
 * @return Bytes copied; 0 once @p off is past what was kept.
 */
uint32_t kdrv_net_http_body(uint32_t off, uint8_t *dst, uint32_t cap);

/**
 * @brief Wipe the kept body of the last HTTP job, once its reader has it.
 *
 * A body can carry a token; it does not wait in the driver for the next job.
 * A running job writes the body itself, so this is not safe while one runs.
 * Called by AO_Net only while no job is in flight, because it knows when its
 * worker is idle.
 */
void kdrv_net_http_forget(void);

/**
 * @brief Store the body of the next HTTP_POST job.
 *
 * The body does not travel in the job: a kilobyte there would be paid for by
 * every job of every kind. It is copied into a buffer this driver owns (PSRAM, taken on the
 * first call, never freed) and stays until the next call replaces it. The
 * POST sends it with `Content-Type: application/json`; this driver does not
 * look inside it. The answer lands where GET's does, in
 * kdrv_net_http_result() and kdrv_net_http_body() (KDRV_NET_BODY_MAX bytes of
 * it are kept).
 *
 * The buffer could be the one a POST is sending, so this is not safe while a
 * job runs. Called by AO_Net only while no job is in flight, because it knows
 * when its worker is idle.
 *
 * @param json Body bytes; no terminator needed.
 * @param len  1 to KDRV_NET_POST_MAX.
 * @return KODE_OK; KODE_ERROR when @p json is NULL or @p len is 0 or past
 *         KDRV_NET_POST_MAX; KODE_NO_RESOURCES when PSRAM cannot give the
 *         buffer.
 */
kode_status_t kdrv_net_http_post_body(const char *json, size_t len);

/**
 * @brief Store everything the next HTTP_PUT job sends.
 *
 * Like the POST body, none of it travels in the job. All five are copied into
 * buffers this driver owns (PSRAM, taken on the first call, never freed) and
 * stay until the next call replaces them - except the bearer, which the job
 * wipes once it is done, so every PUT needs its own call. The PUT sends the
 * body as is, with `Content-Type: <content_type>` and `Authorization: Bearer
 * <bearer>`, follows no redirect, and never logs @p url's path (the host
 * alone, see kdrv_net_url_host()). The answer lands where GET's does.
 *
 * The driver keeps no record of a job running: the caller (AO_Net) calls this
 * only while its worker is idle, because the buffers could be the ones a job
 * is sending.
 *
 * @param url          `https://` target, at most KDRV_NET_PUT_URL_MAX - 1
 *                     characters.
 * @param body         Body bytes; no terminator needed.
 * @param len          1 to KDRV_NET_PUT_MAX.
 * @param content_type Value of the Content-Type header, shorter than
 *                     KDRV_NET_PUT_TYPE_MAX.
 * @param bearer       The token alone, without "Bearer ": the driver adds the
 *                     scheme. Non-empty, shorter than KDRV_NET_AUTH_MAX - 7.
 * @return KODE_OK; KODE_ERROR when an argument is NULL, empty or past its
 *         limit, or @p url is not `https://`; KODE_NO_RESOURCES when PSRAM
 *         cannot give them.
 */
kode_status_t kdrv_net_http_put_prepare(const char *url, const uint8_t *body,
                                        size_t len, const char *content_type,
                                        const char *bearer);

/**
 * @brief The host of @p url, for a log line that must not carry its path.
 *
 * @param url The URL.
 * @param out Out: what lies between `://` and the next `/`, `?` or `#`.
 * @param cap Room in @p out, terminator included.
 * @return KODE_OK; KODE_ERROR when @p url has no `://`, no host, or the host
 *         does not fit - @p out then holds the empty string.
 */
kode_status_t kdrv_net_url_host(const char *url, char *out, size_t cap);

/** @brief Bytes the last UDP_SEND job put on the wire. */
uint16_t kdrv_net_udp_sent(void);

/* ---- MQTT: one client, owned by AO_Net ----------------------------------
 * Declared unconditionally, like every Kconfig-gated request helper in kaos.h:
 * a header that changes shape with a CONFIG_ symbol only works for the files
 * that remembered to include sdkconfig.h first, and one that forgets gets a
 * link error instead of a compile error. What KODEOS_MQTT gates is the
 * implementation in kdrv_net.c.
 */

/** @brief Where the client is. */
typedef enum {
    KDRV_MQTT_DOWN = 0,   /**< No client, or it has been stopped.           */
    KDRV_MQTT_CONNECTING, /**< Started; waiting for the broker.             */
    KDRV_MQTT_UP,         /**< Connected.                                   */
} kdrv_mqtt_state_t;

/** @brief Why the client is not up. */
typedef enum {
    KDRV_MQTT_ERR_NONE = 0,
    KDRV_MQTT_ERR_NO_NET,   /**< This board has no address.                 */
    KDRV_MQTT_ERR_REFUSED,  /**< The broker or the transport said no.       */
    KDRV_MQTT_ERR_TIMEOUT,  /**< Nothing answered in time.                  */
    KDRV_MQTT_ERR_BUSY,     /**< The request was refused because the Kode
                             *   channel holds the client. Reported once to
                             *   whoever asked and never kept as the client's
                             *   own reason for being down.               */
} kdrv_mqtt_err_t;

/** @brief What the MQTT callback is reporting. */
typedef enum {
    KDRV_NET_MQTT_CONNECTED = 0,
    KDRV_NET_MQTT_DISCONNECTED,
    KDRV_NET_MQTT_ERROR,    /**< A transport failure. A bench client is
                             *   retried by esp-mqtt; a channel session
                             *   (kdrv_net_mqtt_session()) is not, and its
                             *   DISCONNECTED follows.                     */
    KDRV_NET_MQTT_MSG,      /**< A message is waiting in the receive ring;
                             *   @c msg is NULL (see kdrv_net_mqtt_cb_t).  */
    KDRV_NET_MQTT_REFUSED,  /**< The broker answered the CONNECT with a
                             *   refusal (bad or expired credentials). A
                             *   bench client is retried with the same
                             *   ones.                                     */
} kdrv_net_mqtt_evt_t;

/**
 * @brief One message from the broker, already bounded by this driver.
 *
 * The bounding happens here rather than in AO_Net because this is the layer
 * that knows esp-mqtt splits a long payload across several events: only the
 * first piece is carried, and @c data_cut says so. A truncated message is
 * still delivered - a message silently shortened would be a message that
 * lies, and one silently dropped would be worse.
 */
typedef struct {
    char     topic[KDRV_MQTT_TOPIC_MAX];
    uint8_t  data[KDRV_MQTT_PAYLOAD_MAX];
    uint16_t len;         /**< Bytes in @c data.                            */
    bool     topic_cut;
    bool     data_cut;
    bool     retained;    /**< The broker replayed a stored message.        */
    bool     channel;     /**< It came from the client started with a session
                           *   (kdrv_net_mqtt_session()), the Kode channel's,
                           *   and not from a bench client. Set by the driver
                           *   when the message arrives, so the owner can tell
                           *   the two after the session is gone.             */
} kdrv_mqtt_msg_t;

/** @brief A @c session_expiry_s that asks the broker for no expiry at all. */
#define KDRV_MQTT_EXPIRY_NEVER  0xFFFFFFFFU

/**
 * @brief Everything a broker session needs that the bench verb leaves out.
 *
 * Strings are terminated; a field that is empty is not sent. @c pass is the
 * channel's token: up to KDRV_MQTT_TOKEN_MAX - 1 characters.
 */
typedef struct {
    char     client_id[KDRV_MQTT_CLIENT_ID_MAX]; /**< The broker files the
                                                  *   session under this.   */
    char     user[KDRV_MQTT_USER_MAX];
    char     pass[KDRV_MQTT_TOKEN_MAX];
    char     will_topic[KDRV_MQTT_TOPIC_MAX];    /**< Empty: no last will.   */
    char     will_msg[KDRV_MQTT_WILL_MAX];       /**< Sent at QoS 0.         */
    bool     will_retain;
    uint16_t keepalive_s;                        /**< 0: esp-mqtt's 120 s.   */
    bool     mqtt5;                              /**< Speak MQTT 5; false is
                                                  *   3.1.1.                 */
    bool     keep_session;                       /**< No clean start: the
                                                  *   broker resumes the
                                                  *   session filed under
                                                  *   @c client_id.          */
    uint32_t session_expiry_s;                   /**< With @c mqtt5: how long
                                                  *   the broker keeps the
                                                  *   session after the
                                                  *   connection ends.
                                                  *   KDRV_MQTT_EXPIRY_NEVER
                                                  *   asks for no expiry; 0
                                                  *   keeps none.            */
    uint8_t  sub_qos;                            /**< The QoS of every
                                                  *   subscribe while this
                                                  *   client lives.          */
} kdrv_mqtt_session_t;

/**
 * @brief Set the session the next MQTT_START job connects with.
 *
 * Copied into a buffer this driver owns (PSRAM, taken on the first call). It
 * applies to the next START and to no other: once the client has been created
 * from it (or the START was refused) the copy is wiped and forgotten, so a
 * token never outlives its use and the bench `mqtt_up` can never inherit the
 * channel's identity. The owner therefore sets it before every START and
 * never has to clear it. With a session set, MQTT_START takes client id,
 * user, password, last will and keepalive from it and ignores req.user and
 * req.pass; req.uri still names the broker. A client started from a session
 * does not redial: when its connection ends the owner is told
 * (KDRV_NET_MQTT_DISCONNECTED) and decides when to start another, so the
 * retries follow the owner's back-off and not esp-mqtt's fixed one. With none set the client is the
 * bench's: the board's serial as client id, req.user and req.pass, esp-mqtt's
 * own keepalive, no last will, a clean session, QoS 0 subscribes. MQTT 3.1.1
 * unless the session says @c mqtt5.
 *
 * A START may be reading the copy, so this is not safe while a job runs.
 * Called by AO_Net only while no job is in flight, because it knows when its
 * worker is idle.
 *
 * @param s The session, or NULL to forget one that was set and not used.
 * @return KODE_OK; KODE_ERROR when @p s has no client id; KODE_DENIED in a
 *         build without MQTT; KODE_NO_RESOURCES when PSRAM cannot give the
 *         buffer.
 */
kode_status_t kdrv_net_mqtt_session(const kdrv_mqtt_session_t *s);

/** @brief Messages the driver keeps for its owner between esp-mqtt's task and its own.
 *         A resume burst of the channel's queue (32) must fit, with room for what arrives while
 *         the owner is busy; the ring is PSRAM, KDRV_MQTT_RX_RING slots of one message. */
#define KDRV_MQTT_RX_RING  48U

/**
 * @brief The MQTT outcome channel: runs on esp-mqtt's own task.
 *
 * @c msg is NULL for every event. @c session_present is the CONNACK's flag
 * and means something for KDRV_NET_MQTT_CONNECTED alone: the broker resumed
 * the session it held for this client id. It is false for every other event.
 *
 * KDRV_NET_MQTT_MSG is only a notice: the message is in the driver's receive
 * ring (KDRV_MQTT_RX_RING), and the owner empties the ring with
 * kdrv_net_mqtt_rx_take(). It is sent when the ring goes from empty to
 * holding a message, not once a message, and again with the next message
 * when it is owed (see kdrv_net_mqtt_rx_owe()). The callback must return at
 * once, and may not wait for anything: esp-mqtt calls it inside its API lock,
 * where the owner may be blocked in a publish.
 *
 * @return For KDRV_NET_MQTT_MSG: true when the notice was posted; false when
 *         it was refused, and the driver sends another with the next message.
 *         Ignored for every other event.
 */
typedef bool (*kdrv_net_mqtt_cb_t)(kdrv_net_mqtt_evt_t evt,
                                   const kdrv_mqtt_msg_t *msg,
                                   bool session_present);

/**
 * @brief Take the oldest message the broker sent, if any.
 *
 * Any task; the ring has its own lock, held for one copy. The owner calls it
 * until it says false, on a KDRV_NET_MQTT_MSG notice and now and then
 * besides, so a notice that was refused costs a delay and not a message.
 *
 * @param out Receives the message.
 * @return true when @p out holds one; false when the ring is empty.
 */
bool kdrv_net_mqtt_rx_take(kdrv_mqtt_msg_t *out);

/**
 * @brief Record that a notice is owed: the next message sends one.
 *
 * For an owner that could not empty the ring (its own post to itself was
 * refused) and has nothing else that will look at the ring again: without
 * this, messages already in the ring would wait for a notice that is sent
 * only when the ring goes from empty to non-empty. The next message that
 * arrives, kept or dropped, sends it. Any task; the flag has the ring's lock.
 */
void kdrv_net_mqtt_rx_owe(void);

/** @brief Messages dropped since boot because the receive ring was full. */
uint32_t kdrv_net_mqtt_rx_dropped(void);

/** @brief Install the MQTT callback. Call once, before the first START job. */
void kdrv_net_mqtt_set_cb(kdrv_net_mqtt_cb_t cb);

/** @brief Where the client is right now. */
kdrv_mqtt_state_t kdrv_net_mqtt_state(void);

/**
 * @brief Copy the broker URI the client was last started with ("" if never).
 *
 * @param out Buffer for the URI, always terminated; cut to fit.
 * @param cap Its size in bytes; 0 leaves @p out alone.
 */
void kdrv_net_mqtt_uri_copy(char *out, size_t cap);

/**
 * @brief Publish at QoS 0, written to the socket before it returns.
 *
 * Nothing is stored for later: a message the client cannot send now is a
 * failure now, so the caller knows what went out and what did not. It can
 * wait on the socket for a moment, which AO_Net's own thread can afford.
 *
 * @param topic  Where to.
 * @param data   Payload bytes; may be NULL when @p len is 0.
 * @param len    Payload length, an empty payload being legitimate.
 * @param retain true: the broker keeps the message for later subscribers.
 * @return KODE_DENIED when the client is not up, KODE_NO_RESOURCES when
 *         esp-mqtt could not send it.
 */
kode_status_t kdrv_net_mqtt_publish(const char *topic, const void *data,
                                    uint16_t len, bool retain);

/**
 * @brief Subscribe at the QoS the client's session asked for (0 for the
 *        bench's own). Same contract as publish.
 */
kode_status_t kdrv_net_mqtt_subscribe(const char *topic);

/** @brief Unsubscribe. Same contract. */
kode_status_t kdrv_net_mqtt_unsubscribe(const char *topic);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_NET_H_ */
