/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file net_ao.c
 * @brief AO_Net: the Active Object that owns the host's network services.
 */
#include "kaos.h"
#include "kaos_ask.h"
#include "kaos_start.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "kqp.h"
#include "kqp_worker.h"
#include "kdrv_clocks.h"
#include "kdrv_net.h"
#include "khal_time.h"
#include "khal_identity.h"
#include "khal_nvs.h"
#include "kdrv_storage.h"
#include "kdrv_wifi.h"   /* kdrv_wifi_state_t: what WIFI_STATE_SIG carries */
#include "kode_log.h"
#include "khal_system.h"
#include "sdkconfig.h"
#include "kaos_claim.h"     /* kaos_require(): the register's tables         */
#include "krows.h"
#include "kverbs.h"

#if CONFIG_KODEOS_CHANNEL
#include "kaos_wire.h"      /* the base64 encoder shared with the console    */
#include "kdrv_slot.h"      /* kdrv_running_desc(): the version `info` shows */
#include "kode_wire.h"      /* KODE_ST_*: what the C5 answers with           */
#include "net_channel.h"
#include "kset.h"
#endif

#define TAG "kaos.net"

/* The bounds are written twice, in kqp_config.h for the events and in
 * kdrv_net.h for the driver, which cannot include the scheduler's
 * configuration. This file sees both, so it checks that they match. */
_Static_assert(KODE_NET_HOST_MAX     == KDRV_NET_HOST_MAX,     "host cap");
_Static_assert(KODE_NET_URL_MAX      == KDRV_NET_URL_MAX,      "url cap");
_Static_assert(KODE_NET_UDP_MAX      == KDRV_NET_UDP_MAX,      "datagram cap");
_Static_assert(KODE_NET_PING_MAX     == KDRV_NET_PING_MAX,     "echo cap");
_Static_assert(KODE_MQTT_URI_MAX     == KDRV_MQTT_URI_MAX,     "uri cap");
_Static_assert(KODE_MQTT_USER_MAX    == KDRV_MQTT_USER_MAX,    "user cap");
_Static_assert(KODE_MQTT_PASS_MAX    == KDRV_MQTT_PASS_MAX,    "password cap");
_Static_assert(KODE_MQTT_TOPIC_MAX   == KDRV_MQTT_TOPIC_MAX,   "topic cap");
_Static_assert(KODE_MQTT_PAYLOAD_MAX == KDRV_MQTT_PAYLOAD_MAX, "payload cap");
/* A command arrives on a topic as a whole line, so the message carries what
 * the console accepts - not a byte less, or a line would be refused that the
 * cable runs. */
_Static_assert(KDRV_MQTT_PAYLOAD_MAX == KODE_CONSOLE_LINE_MAX,
               "payload cap is a console line");
/* And the one relationship rather than equality: the preview must cover
 * exactly what the driver keeps, or `shown=` counts bytes nobody saw. */
_Static_assert((KODE_NET_HTTP_CHUNKS * (KODE_NET_HTTP_CHUNK - 1U))
                   == KDRV_NET_BODY_MAX,
               "the body preview must carry exactly what kdrv_net keeps");

/** How far an MQTT session has got. Separate from the driver's own state
 *  because they answer different questions: the driver knows whether a socket
 *  is connected, this knows whether anyone asked for one. */
typedef enum {
    MQTT_IDLE = 0,   /**< No client.                                        */
    MQTT_STARTING,   /**< Asked for; waiting for the broker (tmo is armed). */
    MQTT_LIVE,       /**< Connected at least once; the driver reconnects.   */
    MQTT_STOPPING,   /**< Teardown with the worker.                         */
} mqtt_phase_t;

/* ---- A URL as a medium ----------------------------------------------------
 * Two buffers, lent by index and not refilled until the borrower gives one
 * back, so chunk N+1 is read while AO_AppMgr writes chunk N to flash.
 */
#define NET_STREAM_BUFS   2U
#define NET_STREAM_CHUNK  (32U * 1024U)

/** @brief The network worker's stack, in bytes. The TLS handshake asks for
 *         it: verifying a chain against the full Mozilla bundle is the
 *         deepest call this firmware makes. */
#define NET_WORKER_STACK       12288U

/** @brief The download's worker stack, in bytes (PSRAM). Its open is the same
 *         TLS handshake, so it needs the same depth. */
#define NET_STREAM_WORKER_STACK 12288U

/** @brief Jobs that may wait besides the one running: none is ever queued,
 *         because this AO submits only while no job is in flight. */
#define NET_WORKER_DEPTH       1U

/** @brief The network worker's task name. */
#define NET_WORKER_NAME        "net_worker"

/** @brief The download's worker task name. */
#define NET_STREAM_WORKER_NAME "net_stream"

/** @brief The guard of the download's open: the driver's cap and a margin. */
#define NET_STREAM_OPEN_GUARD_MS \
    (KDRV_NET_STREAM_OPEN_MAX_MS + KODE_NET_STREAM_SLACK_MS)

/** @brief The guard of a download read: the driver's cap and a margin. */
#define NET_STREAM_READ_GUARD_MS \
    (KDRV_NET_STREAM_READ_MAX_MS + KODE_NET_STREAM_SLACK_MS)

/** @brief What the download's job with its worker is. */
typedef enum {
    NETS_NONE = 0,  /**< Nothing is with the download's worker.             */
    NETS_OPEN,      /**< The download's open.                               */
    NETS_READ,      /**< One read of the download.                          */
    NETS_CLOSE      /**< The download's close.                              */
} net_stream_kind_t;

/** @brief What a download job is given. The worker reads it for as long as
 *         the job runs; the sizes a job measures come back in the done
 *         event's value. */
typedef struct {
    uint8_t  kind;                    /**< net_stream_kind_t.                 */
    char     url[KODE_FLASH_SRC_MAX]; /**< NETS_OPEN: the URL, as long as a
                                       *   request carries one.               */
    uint8_t *buf;                     /**< NETS_READ: where the chunk goes,
                                       *   one of @c sbuf.                    */
} NetStreamJob;

/** @brief The kinds of ask, one door each per asker. */
enum {
    NET_ASK_LOOKUP = 0,  /**< net_lookup_ask() */
    NET_ASK_PING,        /**< net_ping_ask()   */
    NET_ASK_HTTP,        /**< net_http_ask()   */
    NET_ASK_UDP,         /**< net_udp_ask()    */
    NET_ASK_KINDS        /**< How many there are. */
};

/** @brief The doors, one per kind of ask. */
static kaos_ask_door_t s_net_doors[NET_ASK_KINDS];

/** @brief What an ask's job is given and gives back. The worker reads the
 *         inputs and writes the outputs; who and the answers are the AO's,
 *         written before the submit and read after. */
typedef struct {
    uint8_t                kind;   /**< NET_ASK_*.                               */
    bool                   post;   /**< HTTP: a POST.                            */
    uint8_t                count;  /**< PING: echoes.                            */
    uint16_t               port;   /**< UDP: the port.                           */
    uint16_t               dlen;   /**< UDP: bytes at @c data.                   */
    char                   host[KDRV_NET_HOST_MAX];   /**< LOOKUP, PING, UDP.    */
    char                   url[KDRV_NET_URL_MAX];     /**< HTTP.                 */
    char                   ctype[KODE_NET_CTYPE_MAX]; /**< A POST's type, or "". */
    const char            *body;   /**< A POST's body, lent.                     */
    uint32_t               len;    /**< Its bytes.                               */
    uint8_t               *buf;    /**< HTTP: the answer's body, lent.           */
    uint32_t               cap;    /**< Its room.                                */
    const void            *data;   /**< UDP: the datagram, lent.                 */
    uint32_t               ip4;    /**< LOOKUP out: the address.                 */
    kdrv_net_ping_stats_t  stats;  /**< PING out: the session.                   */
    kdrv_net_http_result_t result; /**< HTTP out: status and lengths.            */
    QActive               *who;    /**< The asking AO.                           */
    NetHostRspEvt         *hrsp;   /**< LOOKUP, PING: the answer.                */
    NetHttpRspEvt         *wrsp;   /**< HTTP: the answer.                        */
    NetUdpDoneEvt         *ursp;   /**< UDP: the answer.                         */
} NetAskJob;

/** @brief The storage the workers read, in PSRAM (taken at the initial
 *         transition, so the static AO stays small). */
typedef struct {
    kdrv_net_req_t req;               /**< The kdrv_net job with @c worker.   */
    NetAskJob      ask;               /**< Or an ask's job with @c worker.    */
    NetStreamJob   s;                 /**< The download job with @c sworker.  */
    char           purl[KODE_FLASH_SRC_MAX]; /**< The staged open's URL.      */
    kdrv_mqtt_msg_t rx;               /**< The message being taken from the
                                       *   driver's ring: one copy, off the
                                       *   AO's stack.                         */
} NetJobs;

/** @brief A job's guard: the one-shot, and what QP has queued of it. */
typedef struct {
    QTimeEvt te;      /**< The one-shot.                                      */
    uint32_t stale;   /**< Expired events still queued.                        */
    bool     live;    /**< Armed and unexpired.                                */
} NetGuard;

#if CONFIG_KODEOS_CHANNEL
/** @brief The api's root. The broker is never written here: connect names it. */
#define CHAN_API_URL        "https://api.kode.diy"

/** @brief The broker's keepalive. 10 000 Kodes at 60 s is ~170 pings/s. */
#define CHAN_KEEPALIVE_S    60U
/** The QoS of the subscribe to cmd/in: the broker queues what was sent while
 *  the Kode was away, and redelivers what it did not acknowledge. */
#define CHAN_CMD_QOS        1U

/** @brief A session this long up has earned a fresh back-off. */
#define CHAN_STAY_UP_US     60000000ULL

/** @brief How long the C5 may take to answer before the step is a failure. */
#define CHAN_ID_GUARD_MS    15000U

/** @brief Poll period when the worker is busy with somebody else's job. */
#define CHAN_RETRY_MS       250U

/** @brief Polls before a worker that never frees is a failure (10 s). */
#define CHAN_RETRY_MAX      40U

/** @brief The fast mode outlives the last cmd/in line by this long (60 s). */
#define CHAN_FAST_HOLD_MS   60000U

/** @brief How often a channel that says "up" is compared with the driver (15 s). */
#define CHAN_WATCH_MS       15000U

/** @brief Microseconds in a millisecond. */
#define CHAN_US_PER_MS      1000ULL

/**
 * @brief The wait after an AWAKE before a long timer that fell due asleep acts.
 *
 * AO_Radio takes the C5 link out of its rest on the same STATE(AWAKE), with a
 * job of its own, so the join must not begin on the instant.
 */
#define CHAN_LP_SETTLE_MS   2000U

#ifndef CONFIG_KODEOS_CHANNEL_RENEW_S
#define CONFIG_KODEOS_CHANNEL_RENEW_S 82800
#endif

/** @brief A connect to its renewal: 23 h unless a bench shortened it. */
#define NET_CHAN_RENEW_MS   ((uint32_t)CONFIG_KODEOS_CHANNEL_RENEW_S * 1000U)

/** @brief Which step of the join is awaited. IDLE: nothing is. */
typedef enum {
    CP_IDLE = 0,   /**< Off, offline, or a terminal state.                  */
    CP_CERT,       /**< The C5's certificate parts and end.                 */
    CP_CHALLENGE,  /**< The challenge POST (wanted or in flight).           */
    CP_SIGN,       /**< The C5's signature.                                 */
    CP_CONNECT,    /**< The connect POST (wanted or in flight).             */
    CP_STOPPING,   /**< The old client going down before the new one.       */
    CP_BROKER,     /**< The new client starting; the broker's CONNACK.      */
    CP_UP,         /**< Connected; the timer is the renewal.                */
    CP_WAIT        /**< A try failed; the timer is the back-off.            */
} chan_phase_t;

/** @brief Whose HTTP job the worker is running. */
/* cppcheck-suppress misra-c2012-2.4 ; the type names the values of a uint8_t field, which keeps AO_Net's struct small */
typedef enum {
    CJ_NONE = 0,   /**< Not the channel's.                                  */
    CJ_LIVE,       /**< The channel's, and its join is still the one.       */
    CJ_STALE       /**< The channel's, but the join it belonged to ended.   */
/* cppcheck-suppress misra-c2012-2.3 ; the type names the values of a uint8_t field, which keeps AO_Net's struct small */
} chan_job_t;

/** @brief What the C5 owes this AO an answer to. */
/* cppcheck-suppress misra-c2012-2.4 ; the type names the values of a uint8_t field, which keeps AO_Net's struct small */
typedef enum {
    IDO_NONE = 0,
    IDO_CERT,
    IDO_SIGN
/* cppcheck-suppress misra-c2012-2.3 ; the type names the values of a uint8_t field, which keeps AO_Net's struct small */
} chan_id_t;

#if CONFIG_KODEOS_SHOT_HTTP
/** @brief The type an upload is sent as. */
#define SHOT_CONTENT_TYPE  "image/jpeg"

/** @brief The lowest status that says the server took the upload: any 2xx
 *         (the contract names 201; a later 200 or 204 is taken the same). */
#define SHOT_HTTP_OK_MIN   200U

/** @brief The first status past the 2xx range. */
#define SHOT_HTTP_OK_END   300U

/** @brief How long AO_Display has to answer a shot before the upload ends. */
#define KSHOT_DISPLAY_MS   2000U

/** @brief Network errors in a row that end a stream. */
#define KSHOT_STREAM_ERR_MAX  3U

/** @brief A stream's link expired, or names another unit: the stream ends. */
#define SHOT_HTTP_EXPIRED       404U
/** @brief Frames under 100 ms apart: that frame is dropped, the stream goes on. */
#define SHOT_HTTP_TOO_MANY      429U
/** @brief From this status up the server failed, not the frame: counted as a
 *         network error. */
#define SHOT_HTTP_SERVER_ERROR  500U

/** @brief Milliseconds in a second. */
#define SHOT_MS_PER_S      1000U

/** @brief Frames over milliseconds times this is frames a second times ten. */
#define SHOT_FPS_X10_SCALE 10000U

/** @brief Tenths in one: how `fps x10` is printed whole and tenths. */
#define SHOT_TENTHS        10U

/** @brief QP ticks to the close that follows a stream's end: the next one, so
 *         that a channel step ending the stream takes the worker first. */
#define SHOT_CLOSE_TICKS   1U

/** @brief The stream said once that the display did not answer a frame. */
#define SHOT_SAID_DISPLAY  0x01U

/** @brief The stream said once that the network driver refused a frame. */
#define SHOT_SAID_DRIVER   0x02U

/* The longest link the api may name and the longest token fit one PUT URL,
 * terminator included: shot_url_build()'s refusal is only a defence. */
_Static_assert((KSHOT_PREFIX_MAX + KSHOT_TOKEN_MAX) < KDRV_NET_PUT_URL_MAX,
               "a shots link and a token must fit one PUT URL");

/** @brief Where the upload in hand, or the stream's frame, is. */
/* cppcheck-suppress misra-c2012-2.4 ; the type names the values of a uint8_t field, which keeps AO_Net's struct small */
typedef enum {
    SP_IDLE = 0,   /**< None.                                               */
    SP_WAIT,       /**< A capture waits for the last stream's connection to
                    *   close before it asks for the glass.                 */
    SP_DISPLAY,    /**< AO_Display is encoding the glass.                   */
    SP_PUT         /**< The worker is sending it.                           */
/* cppcheck-suppress misra-c2012-2.3 ; the type names the values of a uint8_t field, which keeps AO_Net's struct small */
} shot_phase_t;

/** @brief Whose PUT the worker is running. */
/* cppcheck-suppress misra-c2012-2.4 ; the type names the values of a uint8_t field, which keeps AO_Net's struct small */
typedef enum {
    SJ_NONE = 0,   /**< Not an upload's.                                    */
    SJ_LIVE,       /**< An upload's, and somebody waits for its answer.     */
    SJ_STALE,      /**< An upload's that already ended: the channel left,
                    *   or its stream ended, while it was out.              */
    SJ_CLOSE       /**< The close of a stream's kept connection.            */
/* cppcheck-suppress misra-c2012-2.3 ; the type names the values of a uint8_t field, which keeps AO_Net's struct small */
} shot_job_t;
#endif /* CONFIG_KODEOS_SHOT_HTTP */

/** @brief The channel's buffers. PSRAM, taken on the first join (rule 8). */
typedef struct {
    kchan_session_t     sess;      /**< What connect described.             */
    kchan_session_t     scratch;   /**< A connect answer being read: the
                                    *   live session is replaced only by a
                                    *   whole one.                          */
    kdrv_mqtt_session_t ks;        /**< The same, in the driver's words.    */
    uint8_t             der[KODE_ID_PARTS_MAX * KODE_ID_PART_BYTES];
    uint16_t            der_len;   /**< Bytes of the pair received.         */
    uint16_t            unit_len;  /**< Of those, the unit certificate.     */
    uint8_t             parts_got;
    uint8_t             parts_of;
    char                cert_b64[(((((KODE_ID_PARTS_MAX * KODE_ID_PART_BYTES) + 2U) / 3U) * 4U) + 1U)];
    char                nonce[KODE_ID_NONCE_MAX + 1U];
    char                sig_b64[(((((72U) + 2U) / 3U) * 4U) + 1U)];
    char                body[KDRV_NET_POST_MAX];
    char                answer[KDRV_NET_BODY_MAX + 1U];
    char                held[KODE_LOG_LINE_MAX]; /**< A reply line taken from
                                    *   the ring and not yet published.      */
    uint16_t            held_len;  /**< Its length; 0 when there is none.   */
    char                cq[KODE_CHAN_QUEUE_MAX][KODE_CONSOLE_LINE_MAX]; /**<
                                    *   The lines that arrived on cmd/in and
                                    *   have not ended, oldest first from
                                    *   @c cq_head: a ring.                  */
    uint8_t             cq_head;   /**< Index of the oldest line.           */
    uint8_t             cq_len;    /**< Lines held, the one out included.   */
    bool                cq_out;    /**< The oldest line is with the console
                                    *   and its end is awaited.              */
    uint8_t             cq_seq;    /**< The sequence of the line last given to
                                    *   the console, new for each offer; only
                                    *   the end that names it is taken.      */
    bool                cq_said;   /**< The refusal of the next post was
                                    *   logged once already.                 */
    bool                cq_bsaid;  /**< So was the console's being busy; both
                                    *   end with the line.                   */
#if CONFIG_KODEOS_SHOT_HTTP
    char                jwt[KDRV_MQTT_TOKEN_MAX]; /**< The session's token, the
                                    *   uploads' bearer: the MQTT start wipes
                                    *   sess.token, this lives as long as the
                                    *   session does.                        */
    char                shot_token[KSHOT_TOKEN_MAX + 1U]; /**< The upload in
                                    *   hand's token.                        */
    char                shot_url[KDRV_NET_PUT_URL_MAX]; /**< `shots` + that
                                    *   token, built from the live session.  */
#endif
} ChanWork;
#endif /* CONFIG_KODEOS_CHANNEL */

#if CONFIG_KODE_LOWPOWER
/** @brief Microseconds in a second, for the wall clock's whole seconds. */
#define NET_CHK_US_PER_S    1000000ULL

/** @brief The scheduler's check has no time. */
#define NET_CHK_NONE        (-1LL)
#endif /* CONFIG_KODE_LOWPOWER */

typedef struct {
    QActive  super;
    QTimeEvt tmo;      /**< The connect deadline; nothing else uses it.     */
    bool     online;   /**< There is an address to work with.               */
    bool     sntp_asked; /**< The time client is up: the next address asks it
                        *   again rather than starting it.                  */
    bool     busy;     /**< A job is with the worker: set at submit, cleared
                        *   only by its done event.                         */
    bool     testing;  /**< The job in flight belongs to `test`.            */
    bool     wreplied; /**< Whoever waits for the job in flight has been
                        *   answered (its guard fired); its late done event
                        *   answers nobody.                                  */
    uint16_t wseq;     /**< Sequence number of that job.                    */
    bool     asking;   /**< The job in flight is an ask's (@c jobs->ask).    */
    bool     ask_late; /**< Its guard fired with its lent buffer held: the
                        *   answer waits for the worker and says
                        *   KODE_TIMEOUT.                                   */
    KqpWorker worker;  /**< Runs the six kinds, on a PSRAM stack.           */
    NetGuard  wguard;  /**< NET_JOB_GUARD_SIG: a job that never came back.  */
    /* Each part of @c jobs is written only while no job is with its worker: a job whose guard
     * fired may still be reading it. */
    NetJobs  *jobs;    /**< What the workers read (PSRAM).                   */
    bool     sbusy;    /**< A download job is with its worker.              */
    bool     sreplied; /**< The asker of that job has been answered.        */
    uint8_t  skind;    /**< net_stream_kind_t: what that job is.            */
    uint16_t sseq;     /**< Sequence number of that job.                    */
    KqpWorker sworker; /**< Runs the download's jobs, on a PSRAM stack.     */
    NetGuard  sguard;  /**< NET_STREAM_GUARD_SIG.                           */
    bool     leaving;  /**< A leave was asked: no channel session starts
                        *   again this boot.                                 */
    bool     leave_owed; /**< BOOT_NET_LEFT_SIG is owed to AO_Boot.          */
#if CONFIG_KODE_LOWPOWER
    uint8_t  lp_who;   /**< The id lowpower_join_radio() gave.              */
    bool     lp_rest;  /**< The board sleeps: nothing new is started.       */
    bool     lp_radio; /**< A radio wake is on and this AO has not said it is done. */
    bool     lp_owed;  /**< An OFF round waits for the sessions to close.   */
    bool     lp_rejoin;/**< The channel is joined at the next AWAKE: its
                        *   sessions were closed for an OFF round, or its
                        *   start came while the board slept.                */
    uint32_t lp_round; /**< That round's number.                            */
    int64_t  chk_s;    /**< Unix seconds the scheduler's `mqtt` check falls due; -1 for none. */
    bool     chk_dwell;/**< The check is due in this radio wake and its dwell has not run out. */
    bool     link_up;  /**< AO_Radio says the C5 link is up.                */
    QTimeEvt chk_tmo;  /**< NET_CHECK_TMO_SIG: the dwell.                   */
#endif
    uint8_t  phase;    /**< mqtt_phase_t                                    */
    uint8_t  err;      /**< kdrv_mqtt_err_t: why MQTT is not up.            */

    QActive *reader;                     /**< Who has the transfer open.     */
    uint8_t *sbuf[NET_STREAM_BUFS];      /**< PSRAM, allocated on first use. */
    bool     slent[NET_STREAM_BUFS];     /**< Out on loan; do not touch.     */
    bool     sopen;                      /**< A transfer is open.            */
    bool     sheld;      /**< The driver may hold the transfer's client: set
                          *   when an open is submitted, cleared by a failed
                          *   open's or a close's done event.                */
    bool     sclose_owed; /**< The client must be closed when the worker is
                           *   free.                                         */
    QActive *swho;       /**< Who asked for the stream job in flight.        */
    uint8_t  sidx;       /**< The buffer that read is filling.               */
    uint8_t  sreads;     /**< Reads asked for and not started; never more than
                          *   there are free buffers.                        */
    bool     popen;      /**< An open waits for the close before it, its URL
                          *   in @c jobs->purl.                              */
    QActive *pwho;       /**< Who asked for it.                              */

#if CONFIG_KODEOS_CHANNEL
    /* The Kode channel. About 100 bytes here; the work area is in PSRAM. */
    QTimeEvt  ctmo;          /**< The channel's one timer (see chan_phase_t). */
    QTimeEvt  otmo;          /**< One-shot: the reply drain's retry, when its
                              *   own post was refused. Delivers
                              *   NET_CHAN_OUT_SIG.                           */
    QTimeEvt  ftmo;          /**< One-shot: the fast mode's hold, re-armed by
                              *   every cmd/in line. Delivers
                              *   NET_CHAN_FAST_SIG.                          */
    QTimeEvt  wtmo;          /**< One-shot, re-armed while the channel is up:
                              *   is the client still there? Delivers
                              *   NET_CHAN_WATCH_SIG.                         */
    NetGuard  lguard;        /**< One-shot: the bound on the line the console
                              *   holds, or the retry of a refused post.
                              *   Delivers NET_CHAN_LINE_SIG.                 */
    ChanWork *cw;            /**< PSRAM, taken on the first join.            */
    uint64_t  cstate_us;     /**< When the state last changed.               */
    uint64_t  cup_us;        /**< When the broker last connected.            */
    uint64_t  cdue_us;       /**< khal_time_us() when the long timer is due:
                              *   the renewal while @c cp is Up, the end of
                              *   the back-off while it is Wait (kept across
                              *   a Wi-Fi flap while @c cwait_valid). Real
                              *   time: a QTimeEvt does not count asleep.    */
    uint8_t   cstate;        /**< chan_state_t                               */
    uint8_t   cp;            /**< chan_phase_t: which step is awaited.       */
    uint8_t   attempt;       /**< Failures in a row; the back-off's index.   */
    uint8_t   cretry;        /**< Worker-busy polls spent on this step.      */
    uint8_t   id_out;        /**< chan_id_t: what the C5 owes an answer to.  */
    uint8_t   cjob;          /**< chan_job_t: whose HTTP the worker holds.   */
    bool      cert_ok;       /**< ChanWork holds the unit's certificate.     */
    bool      crenew;        /**< The sequence replaces a live session.      */
    bool      cwait_valid;   /**< A back-off is owed across a Wi-Fi flap.    */
    bool      cup_set;       /**< cup_us means something.                    */
    bool      stop_pending;  /**< The client must be stopped when the worker
                              *   is free.                                   */
    bool      cfast;         /**< The fast mode is on: AO_Radio was asked for
                              *   no power save.                             */
    bool      chan_on;       /**< `chan`: join whenever there is Wi-Fi.      */
    uint32_t  keep_s;        /**< `chan_keep`, in seconds: the session age the
                              *   next connect asks for.                     */
    uint32_t  nkeep_s;       /**< The age the connect in flight asked for.   */
    uint32_t  ckeep_s;       /**< The age the live session's connect asked for;
                              *   0 until one has.                           */
    uint8_t   csess;         /**< chan_session_t: what the broker said of the
                              *   live session at its connect.               */
    char      cwhy[24];      /**< Why the state is what it is.               */
    char      cwait_why[24]; /**< The reason for the wait that is owed.      */
    char      cnote[24];     /**< The last cause a NOTICE was written for.   */
#if CONFIG_KODEOS_SHOT_HTTP
    QTimeEvt  sdtmo;         /**< One-shot: the display's deadline for a
                              *   shot. Delivers NET_SHOT_WAIT_SIG.          */
    QTimeEvt  stmo;          /**< One-shot, re-armed per tick: a stream's
                              *   pace, and the close after it ends.
                              *   Delivers NET_SHOT_TICK_SIG.                */
    QActive  *shwho;         /**< Who the upload in hand answers; NULL when
                              *   there is none (a stream is answered at
                              *   once).                                     */
    uint32_t  sbytes;        /**< JPEG bytes the upload in hand sends.       */
    uint32_t  sfrom_ms;      /**< khal_time_ms() when the stream started.    */
    uint32_t  sran_ms;       /**< How long the last stream ran, once ended.  */
    uint32_t  ssent;         /**< Frames the server took, this stream or the
                              *   last one.                                  */
    uint32_t  sskip;         /**< Frames skipped: codec busy, too big, the
                              *   worker taken, a 429.                       */
    uint16_t  ssecs;         /**< The stream's length, in seconds.           */
    uint8_t   sfps;          /**< The stream's frames a second.              */
    uint8_t   smode;         /**< kshot_mode_t: which upload runs.           */
    uint8_t   sp;            /**< shot_phase_t: where that upload is.        */
    uint8_t   sjob;          /**< shot_job_t: whose PUT the worker holds.    */
    uint8_t   serrs;         /**< The stream's network errors in a row.      */
    uint8_t   ssaid;         /**< SHOT_SAID_*: the per-frame warnings this
                              *   stream already wrote once.                 */
    bool      sdue;          /**< The stream's tick fired and no frame has
                              *   started since.                             */
    bool      sclose;        /**< A stream ended: its kept connection is
                              *   closed before anything else goes out.      */
#endif
#endif
} Net;

static Net l_net;
QActive * const AO_Net = &l_net.super;

static QState Net_initial(Net * const me, void const * const par);
static QState Net_root(Net * const me, QEvt const * const e);
static QState Net_offline(Net * const me, QEvt const * const e);
static QState Net_online(Net * const me, QEvt const * const e);

/* ---- Request helpers ----------------------------------------------------- */

void net_ping_request(const char *host, uint8_t count)
{
    NetPingReqEvt *e = Q_NEW_X(NetPingReqEvt, 0U, NET_PING_REQ_SIG);
    if ( NULL != e )
    {
        (void)snprintf(e->host, sizeof(e->host), "%s", host);
        e->count = (0U == count) ? 1U : count;
        QACTIVE_POST(AO_Net, &e->super, NULL);
    }
}

void net_http_get_request(const char *url)
{
    NetHttpReqEvt *e = Q_NEW_X(NetHttpReqEvt, 0U, NET_HTTP_REQ_SIG);
    if ( NULL != e )
    {
        (void)snprintf(e->url, sizeof(e->url), "%s", url);
        QACTIVE_POST(AO_Net, &e->super, NULL);
    }
}

void net_udp_send_request(const char *host, uint16_t port,
                          const void *data, uint16_t len)
{
    NetUdpReqEvt *e = Q_NEW_X(NetUdpReqEvt, 0U, NET_UDP_REQ_SIG);
    if ( NULL != e )
    {
        (void)snprintf(e->host, sizeof(e->host), "%s", host);
        e->port = port;
        e->len  = ((uint16_t)sizeof(e->data) < len)
                  ? (uint16_t)sizeof(e->data) : len;
        /* The whole array: a pool block holds what the last event left in
         * it. */
        (void)memset(e->data, 0, sizeof(e->data));
        (void)memcpy(e->data, data, e->len);
        QACTIVE_POST(AO_Net, &e->super, NULL);
    }
}

/* ---- The asks: kode_sys's requests, answered to the asking AO --------------
 * One door per asker and kind, the answer taken with the request. The job
 * runs on the worker like any other; the answer is posted once it is back.
 */

/**
 * @brief Post a lookup or a ping ask.
 * @param who   The asking AO.
 * @param ping  A ping; false looks the host up.
 * @param host  Copied.
 * @param count A ping's echoes.
 * @return As the asks in kaos.h.
 */
static kode_status_t host_ask(QActive *who, bool ping, const char *host, uint8_t count)
{
    uint8_t        kind = (uint8_t)NET_ASK_LOOKUP;
    enum_t         rsig = NET_LOOKUP_RSP_SIG;
    kode_status_t  rc;
    NetHostRspEvt *a;
    NetHostAskEvt *e;

    if ( ping )
    {
        kind = (uint8_t)NET_ASK_PING;
        rsig = NET_PING_RSP_SIG;
    }
    rc = kaos_ask_take(&s_net_doors[kind], who);
    if ( KODE_OK != rc )
    {
        return rc;
    }
    a = Q_NEW_X(NetHostRspEvt, 0U, rsig);
    if ( NULL == a )
    {
        return kaos_ask_cancel(&s_net_doors[kind], who, NULL);
    }
    e = Q_NEW_X(NetHostAskEvt, 0U, NET_HOST_ASK_SIG);
    if ( NULL == e )
    {
        return kaos_ask_cancel(&s_net_doors[kind], who, &a->super);
    }
    e->ping  = ping;
    e->count = count;
    (void)snprintf(e->host, sizeof(e->host), "%s", host);
    e->who   = who;
    e->rsp   = a;
    return kaos_ask_post(&s_net_doors[kind], who, AO_Net, &e->super, &a->super,
                         KQP_NET_CMD_MARGIN);
}

kode_status_t net_lookup_ask(QActive *who, const char *host)
{
    return host_ask(who, false, host, 0U);
}

kode_status_t net_ping_ask(QActive *who, const char *host, uint8_t count)
{
    return host_ask(who, true, host, count);
}

kode_status_t net_http_ask(QActive *who, bool post, const char *url, const void *body,
                           uint32_t len, const char *ctype, void *buf, uint32_t cap)
{
    kode_status_t  rc = kaos_ask_take(&s_net_doors[NET_ASK_HTTP], who);
    NetHttpRspEvt *a;
    NetHttpAskEvt *e;

    if ( KODE_OK != rc )
    {
        return rc;
    }
    a = Q_NEW_X(NetHttpRspEvt, 0U, NET_HTTP_RSP_SIG);
    if ( NULL == a )
    {
        return kaos_ask_cancel(&s_net_doors[NET_ASK_HTTP], who, NULL);
    }
    e = Q_NEW_X(NetHttpAskEvt, 0U, NET_HTTP_ASK_SIG);
    if ( NULL == e )
    {
        return kaos_ask_cancel(&s_net_doors[NET_ASK_HTTP], who, &a->super);
    }
    a->post = post;
    e->post = post;
    (void)snprintf(e->url, sizeof(e->url), "%s", url);
    e->ctype[0] = '\0';
    if ( NULL != ctype )
    {
        (void)snprintf(e->ctype, sizeof(e->ctype), "%s", ctype);
    }
    e->body = body;
    e->len  = len;
    e->buf  = buf;
    e->cap  = cap;
    e->who  = who;
    e->rsp  = a;
    return kaos_ask_post(&s_net_doors[NET_ASK_HTTP], who, AO_Net, &e->super, &a->super,
                         KQP_NET_CMD_MARGIN);
}

kode_status_t net_udp_ask(QActive *who, const char *host, uint16_t port, const void *data,
                          uint16_t len)
{
    kode_status_t  rc = kaos_ask_take(&s_net_doors[NET_ASK_UDP], who);
    NetUdpDoneEvt *a;
    NetUdpAskEvt  *e;

    if ( KODE_OK != rc )
    {
        return rc;
    }
    a = Q_NEW_X(NetUdpDoneEvt, 0U, NET_UDP_RSP_SIG);
    if ( NULL == a )
    {
        return kaos_ask_cancel(&s_net_doors[NET_ASK_UDP], who, NULL);
    }
    e = Q_NEW_X(NetUdpAskEvt, 0U, NET_UDP_ASK_SIG);
    if ( NULL == e )
    {
        return kaos_ask_cancel(&s_net_doors[NET_ASK_UDP], who, &a->super);
    }
    (void)snprintf(e->host, sizeof(e->host), "%s", host);
    e->port = port;
    e->len  = len;
    e->data = data;
    e->who  = who;
    e->rsp  = a;
    return kaos_ask_post(&s_net_doors[NET_ASK_UDP], who, AO_Net, &e->super, &a->super,
                         KQP_NET_CMD_MARGIN);
}

/* cppcheck-suppress misra-c2012-8.7 ; called by kgui, a consumer outside this component */
bool channel_state_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(CHANNEL_STATE_REQ_SIG);
    QACTIVE_POST(AO_Net, &evt, NULL);
    return true;
}

#if CONFIG_KODEOS_CHANNEL
/* cppcheck-suppress misra-c2012-8.7 ; called by AO_Console, in another component */
void net_line_done(uint8_t seq)
{
    NetLineDoneEvt * const e = Q_NEW_X(NetLineDoneEvt, 0U, NET_LINE_DONE_SIG);

    if ( NULL != e )
    {
        e->seq = seq;
        /* Only a full queue refuses this post, and then the bound frees the
         * line. A refused post recycles the event itself. */
        if ( !QACTIVE_POST_X(AO_Net, &e->super, 0U, NULL) )
        {
            KLOG_E(TAG, "the console's answer to a command line was dropped: the queue is full");
        }
    }
    else
    {
        KLOG_E(TAG, "the console's answer to a command line was dropped: no event is free");
    }
}

/* cppcheck-suppress misra-c2012-8.7 ; called by AO_Console, in another component */
void net_line_busy(void)
{
    static const QEvt busy = QEVT_INITIALIZER(NET_LINE_BUSY_SIG);

    /* Refused only by a queue that is completely full, and then the bound
     * frees the line. */
    if ( !QACTIVE_POST_X(AO_Net, &busy, 0U, NULL) )
    {
        KLOG_E(TAG, "the console's refusal of a command line was dropped: the queue is full");
    }
}
#endif

void net_info_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(NET_INFO_REQ_SIG);
    QACTIVE_POST(AO_Net, &evt, NULL);
}

void net_leave_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(NET_LEAVE_REQ_SIG);
    QACTIVE_POST(AO_Net, &evt, NULL);
}

bool net_check_at(int64_t due_wall_s)
{
    NetCheckAtEvt * const e = Q_NEW_X(NetCheckAtEvt, 0U, NET_CHECK_AT_SIG);
    bool posted = false;

    if ( NULL == e )
    {
        KLOG_W(TAG, "out of events; the time of the scheduler's check was dropped");
    }
    else
    {
        e->due_s = due_wall_s;
        /* A refused post recycles the event itself. */
        posted = QACTIVE_POST_X(AO_Net, &e->super, KODE_SCHED_POST_MARGIN, NULL);
        if ( !posted )
        {
            KLOG_W(TAG, "the time of the scheduler's check was refused: the queue is full");
        }
    }
    return posted;
}

#if CONFIG_KODEOS_SHOT_HTTP
/* cppcheck-suppress misra-c2012-8.7 ; kaos.h's interface: the console orders the uploads */
void net_shot_request(QActive *who, const char *token, uint8_t fps,
                      uint16_t secs)
{
    NetShotReqEvt *e = Q_NEW_X(NetShotReqEvt, 0U, NET_SHOT_REQ_SIG);

    if ( NULL != e )
    {
        e->who = who;
        /* A token that would be cut to fit is sent as none, which AO_Net
         * refuses: a shortened token names somebody else's link. */
        e->token[0] = '\0';
        if ( kshot_token_valid(token) )
        {
            (void)snprintf(e->token, sizeof(e->token), "%s", token);
        }
        e->fps  = fps;
        e->secs = secs;
        QACTIVE_POST(AO_Net, &e->super, NULL);
    }
    else
    {
        KLOG_E(TAG, "out of events; an upload request was dropped");
    }
}

/* cppcheck-suppress misra-c2012-8.7 ; kaos.h's interface: the console orders the stop */
void net_shot_stop_request(QActive *who)
{
    NetShotStopEvt *e = Q_NEW_X(NetShotStopEvt, 0U, NET_SHOT_STOP_SIG);

    if ( NULL != e )
    {
        e->who = who;
        QACTIVE_POST(AO_Net, &e->super, NULL);
    }
    else
    {
        KLOG_E(TAG, "out of events; a stop of the uploads was dropped");
    }
}
#endif

void net_test_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(NET_TEST_REQ_SIG);
    QACTIVE_POST(AO_Net, &evt, NULL);
}

QEvt const *mqtt_up_evt(const char *uri, const char *user, const char *pass)
{
    MqttUpEvt  *e   = Q_NEW_X(MqttUpEvt, 0U, MQTT_UP_REQ_SIG);
    QEvt const *evt = NULL;

    if ( NULL != e )
    {
        (void)snprintf(e->uri, sizeof(e->uri), "%s", uri);
        e->user[0] = '\0';
        if ( NULL != user )
        {
            (void)snprintf(e->user, sizeof(e->user), "%s", user);
        }
        e->pass[0] = '\0';
        if ( NULL != pass )
        {
            (void)snprintf(e->pass, sizeof(e->pass), "%s", pass);
        }
        evt = &e->super;
    }
    return evt;
}

/**
 * @brief Post an MQTT request with the margin of an app's traffic to AO_Net.
 * @param e The request, or NULL when the pool was dry.
 * @return true when it was queued; QP recycles a refused one.
 */
static bool mqtt_door_post(QEvt const *e)
{
    bool posted = false;

    if ( NULL != e )
    {
        posted = QACTIVE_POST_X(AO_Net, e, KQP_NET_CMD_MARGIN, NULL);
    }
    return posted;
}

bool mqtt_up_request(const char *uri, const char *user, const char *pass)
{
    return mqtt_door_post(mqtt_up_evt(uri, user, pass));
}

QEvt const *mqtt_down_evt(void)
{
    static const QEvt evt = QEVT_INITIALIZER(MQTT_DOWN_REQ_SIG);
    return &evt;
}

bool mqtt_down_request(void)
{
    return mqtt_door_post(mqtt_down_evt());
}

QEvt const *mqtt_pub_evt(const char *topic, const void *data, uint16_t len)
{
    MqttPubEvt *e   = Q_NEW_X(MqttPubEvt, 0U, MQTT_PUB_REQ_SIG);
    QEvt const *evt = NULL;

    if ( NULL != e )
    {
        (void)snprintf(e->topic, sizeof(e->topic), "%s", topic);
        e->len = len;
        if ( (uint16_t)sizeof(e->data) < len )
        {
            e->len = (uint16_t)sizeof(e->data);
        }
        /* Cleared before the copy: a pool block holds the last event's bytes,
         * and esp-mqtt may read a payload as a string. */
        (void)memset(e->data, 0, sizeof(e->data));
        (void)memcpy(e->data, data, e->len);
        evt = &e->super;
    }
    return evt;
}

bool mqtt_pub_request(const char *topic, const void *data, uint16_t len)
{
    return mqtt_door_post(mqtt_pub_evt(topic, data, len));
}

/**
 * @brief A topic-only request (subscribe / unsubscribe).
 * @param sig   MQTT_SUB_REQ_SIG or MQTT_UNSUB_REQ_SIG.
 * @param topic The topic filter.
 * @return The event, or NULL when the pool is dry.
 */
static QEvt const *mqtt_topic_evt(enum KodeSignals sig, const char *topic)
{
    MqttTopicEvt *e   = Q_NEW_X(MqttTopicEvt, 0U, (QSignal)sig);
    QEvt const   *evt = NULL;

    if ( NULL != e )
    {
        (void)snprintf(e->topic, sizeof(e->topic), "%s", topic);
        evt = &e->super;
    }
    return evt;
}

QEvt const *mqtt_sub_evt(const char *topic)
{
    return mqtt_topic_evt(MQTT_SUB_REQ_SIG, topic);
}

QEvt const *mqtt_unsub_evt(const char *topic)
{
    return mqtt_topic_evt(MQTT_UNSUB_REQ_SIG, topic);
}

bool mqtt_sub_request(const char *topic)
{
    return mqtt_door_post(mqtt_sub_evt(topic));
}

bool mqtt_unsub_request(const char *topic)
{
    return mqtt_door_post(mqtt_unsub_evt(topic));
}

void mqtt_state_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(MQTT_STATE_REQ_SIG);
    QACTIVE_POST(AO_Net, &evt, NULL);
}

/* ---- Plumbing ------------------------------------------------------------ */

#if CONFIG_KODEOS_MQTT
/* Inbound MQTT posts with KQP_NET_MQTT_MARGIN: a broker sets its rate, and a
 * full queue must refuse it, not assert. The margin leaves room for an identity
 * answer from AO_Radio (KODE_ID_PARTS_MAX parts and the end); an app's commands
 * stop above it (KQP_NET_CMD_MARGIN), so they never starve the messages. */

/**
 * Runs on esp-mqtt's own task: post, and return at once.
 *
 * The message is in the driver's ring; this posts only the notice. It must not
 * wait (esp-mqtt holds its API lock, which AO_Net may be blocked on) and reads
 * none of AO_Net's state. A refused notice returns false, and the driver sends
 * another with the next message.
 */
static bool net_on_mqtt(kdrv_net_mqtt_evt_t evt, const kdrv_mqtt_msg_t *msg,
                        bool session_present)
{
    static const QEvt notice = QEVT_INITIALIZER(NET_MQTT_RX_SIG);
    bool posted = true;

    (void)msg;
    if ( KDRV_NET_MQTT_MSG == evt )
    {
        posted = QACTIVE_POST_X(AO_Net, &notice, KQP_NET_MQTT_MARGIN, NULL);
    }
    else
    {
        NetMqttLinkEvt *e = Q_NEW_X(NetMqttLinkEvt, 0U, NET_MQTT_LINK_SIG);
        if ( NULL != e )
        {
            e->evt = (uint8_t)evt;
            e->session_present = session_present;
            /* A tighter margin than the messages get: a lost DISCONNECTED
             * leaves the channel believing in a dead session. */
            (void)QACTIVE_POST_X(AO_Net, &e->super, KQP_NET_LINK_MARGIN, NULL);
        }
    }
    return posted;
}
#endif /* CONFIG_KODEOS_MQTT */

/* ---- Answers ------------------------------------------------------------- */

/**
 * @brief Take a ping terminator from the pool and fill it in.
 *
 * Claimed before the entries: `net_ping` disarms the console's watchdog, so a
 * lost terminator would wedge the console, while a lost entry only shortens
 * the listing.
 *
 * @return NULL when even that block was not there.
 */
static NetPingEndEvt *net_new_ping_end(bool ok)
{
    NetPingEndEvt *e = Q_NEW_X(NetPingEndEvt, 0U, NET_PING_END_SIG);
    if ( NULL != e )
    {
        kdrv_net_ping_stats_t s;
        (void)memset(&s, 0, sizeof(s));
        if ( ok )
        {
            kdrv_net_ping_stats(&s);
        }
        /* Every field written: a pool block holds what the last event left. */
        e->ip4    = s.ip4;
        e->tx     = s.tx;
        e->rx     = s.rx;
        e->min_ms = s.min_ms;
        e->avg_ms = s.avg_ms;
        e->max_ms = s.max_ms;
        e->ok     = ok;
    }
    return e;
}

/** @brief Publish a ping terminator on its own - the shape of a refusal. */
static void net_publish_ping_end(Net * const me, bool ok)
{
    NetPingEndEvt * const e = net_new_ping_end(ok);
    if ( NULL != e )
    {
        QACTIVE_PUBLISH(&e->super, &me->super);
    }
    else
    {
        KLOG_E(TAG, "no event blocks for a ping answer; whoever asked is "
                    "still waiting");
    }
}

/** @brief One event per echo, then the end marker - the listing shape. */
static void net_publish_ping(Net * const me)
{
    uint8_t const   total = kdrv_net_ping_count();
    NetPingEndEvt * const end = net_new_ping_end(true);

    for ( uint8_t i = 0U; i < total; ++i )
    {
        kdrv_net_ping_reply_t r;
        if ( !kdrv_net_ping_get(i, &r) )
        {
            break;
        }
        NetPingEntryEvt *e = Q_NEW_X(NetPingEntryEvt, 0U, NET_PING_ENTRY_SIG);
        if ( NULL == e )
        {
            KLOG_W(TAG, "ping listing cut short: no event blocks left");
            break;
        }
        e->seq     = r.seq;
        e->time_ms = r.time_ms;
        e->ttl     = r.ttl;
        e->ok      = r.ok;
        QACTIVE_PUBLISH(&e->super, &me->super);
    }

    if ( NULL != end )
    {
        QACTIVE_PUBLISH(&end->super, &me->super);
    }
    else
    {
        KLOG_E(TAG, "no event blocks for the ping terminator; whoever asked "
                    "is still waiting");
    }
}

/** @brief Take a fetch terminator from the pool and fill it in. */
static NetHttpDoneEvt *net_new_http_done(kode_status_t rc)
{
    NetHttpDoneEvt *e = Q_NEW_X(NetHttpDoneEvt, 0U, NET_HTTP_DONE_SIG);
    if ( NULL != e )
    {
        kdrv_net_http_result_t r;
        (void)memset(&r, 0, sizeof(r));
        r.content_length = -1;
        if ( KODE_OK == rc )
        {
            kdrv_net_http_result(&r);
        }
        e->content_length = r.content_length;
        e->body_len       = r.body_len;
        e->kept           = r.kept;
        e->status         = r.status;
        e->rc             = (uint8_t)rc;
        e->truncated      = r.truncated;
    }
    return e;
}

/** @brief Publish a fetch terminator on its own - the shape of a refusal. */
static void net_publish_http_done(Net * const me, kode_status_t rc)
{
    NetHttpDoneEvt * const e = net_new_http_done(rc);
    if ( NULL != e )
    {
        QACTIVE_PUBLISH(&e->super, &me->super);
    }
    else
    {
        KLOG_E(TAG, "no event blocks for a fetch answer; whoever asked is "
                    "still waiting");
    }
}

/**
 * @brief Publish the body as printable text, then the outcome.
 *
 * A body may hold anything, NULs included, and the console quotes only three
 * escapes, so every byte that is not printable (a space is) becomes '.'.
 */
static void net_publish_http(Net * const me)
{
    kdrv_net_http_result_t r;
    kdrv_net_http_result(&r);

    /* The terminator first: `http_get` has disarmed its watchdog, so losing
     * this block would wedge the console. */
    NetHttpDoneEvt * const done = net_new_http_done(KODE_OK);

    uint32_t off = 0U;
    for ( uint8_t seq = 0U;
         ((uint8_t)KODE_NET_HTTP_CHUNKS > seq) && (off < r.kept); ++seq )
    {
        NetHttpDataEvt *e = Q_NEW_X(NetHttpDataEvt, 0U, NET_HTTP_DATA_SIG);
        if ( NULL == e )
        {
            KLOG_W(TAG, "body preview cut short: no event blocks left");
            break;
        }
        uint8_t raw[KODE_NET_HTTP_CHUNK];
        uint32_t const n = kdrv_net_http_body(off, raw,
                                              (uint32_t)sizeof(raw) - 1U);
        for ( uint32_t i = 0U; i < n; ++i )
        {
            e->text[i] = ((0x20U <= raw[i]) && (0x7FU > raw[i]))
                         ? (char)raw[i] : '.';
        }
        e->text[n] = '\0';
        e->len     = (uint16_t)n;
        e->seq     = seq;
        QACTIVE_PUBLISH(&e->super, &me->super);
        off += n;
    }

    if ( NULL != done )
    {
        QACTIVE_PUBLISH(&done->super, &me->super);
    }
    else
    {
        KLOG_E(TAG, "no event blocks for the fetch terminator; whoever asked "
                    "is still waiting");
    }
}

/** @brief Publish whether the datagram left. */
static void net_publish_udp(Net * const me, kode_status_t rc, uint16_t sent)
{
    NetUdpDoneEvt *e = Q_NEW_X(NetUdpDoneEvt, 0U, NET_UDP_DONE_SIG);
    if ( NULL != e )
    {
        e->sent = sent;
        e->rc   = (uint8_t)rc;
        QACTIVE_PUBLISH(&e->super, &me->super);
    }
}

/** @brief Publish this board's addresses. Answerable in every state. */
static void net_publish_info(Net * const me)
{
    NetInfoEvt *e = Q_NEW_X(NetInfoEvt, 0U, NET_INFO_SIG);
    if ( NULL != e )
    {
        kdrv_net_info_t info;
        if ( kdrv_net_info(&info) != KODE_OK )
        {
            (void)memset(&info, 0, sizeof(info));
        }
        e->ip4    = info.ip4;
        e->gw4    = info.gw4;
        e->mask4  = info.mask4;
        e->dns4   = info.dns4;
        e->mqtt   = (uint8_t)kdrv_net_mqtt_state();
        e->online = me->online;
        e->rx_dropped = kdrv_net_mqtt_rx_dropped();
        QACTIVE_PUBLISH(&e->super, &me->super);
    }
}

/** Why the last MQTT_STATE_SIG said what it did: kdrv_mqtt_err_t. */
static _Atomic uint8_t  s_mqtt_last_err;

/** How many MQTT_STATE_SIG AO_Net has published since boot. */
static _Atomic uint32_t s_mqtt_answers;

uint32_t mqtt_answers(uint8_t *err)
{
    uint32_t const n = atomic_load_explicit(&s_mqtt_answers, memory_order_acquire);

    if ( NULL != err )
    {
        *err = atomic_load_explicit(&s_mqtt_last_err, memory_order_relaxed);
    }
    return n;
}

/** Whether the Kode channel holds the client; chan_phase_set() keeps it. */
static _Atomic bool s_chan_holds;

bool mqtt_channel_holds(void)
{
    return atomic_load_explicit(&s_chan_holds, memory_order_acquire);
}

/**
 * @brief Count one MQTT_STATE_SIG carrying @p err, then publish it.
 * @param err The kdrv_mqtt_err_t the answer carries.
 */
static void mqtt_publish_answer(uint8_t err)
{
    MqttStateEvt *e = Q_NEW_X(MqttStateEvt, 0U, MQTT_STATE_SIG);

    if ( NULL != e )
    {
        kdrv_net_mqtt_uri_copy(e->uri, sizeof(e->uri));
        e->state = (uint8_t)kdrv_net_mqtt_state();
        e->err   = err;
        atomic_store_explicit(&s_mqtt_last_err, err, memory_order_relaxed);
        /* Only AO_Net's thread writes the count, so load + store is whole. */
        uint32_t const given = atomic_load_explicit(&s_mqtt_answers,
                                                    memory_order_relaxed);
        atomic_store_explicit(&s_mqtt_answers, given + 1U, memory_order_release);
        QACTIVE_PUBLISH(&e->super, AO_Net);
    }
}

/** @brief Publish where the MQTT client is, and why it is not up. */
static void mqtt_publish_state(Net * const me)
{
    mqtt_publish_answer(me->err);
}

/** @brief Close an mqtt_pub, mqtt_sub or mqtt_unsub that was taken.
 *
 *  Not with the client's own reason for being down: it may be left over from
 *  an earlier connect. */
static void mqtt_publish_taken(void)
{
    mqtt_publish_answer((uint8_t)KDRV_MQTT_ERR_NONE);
}

#if CONFIG_KODEOS_CHANNEL
/** @brief Refuse an MQTT request because the Kode channel holds the client.
 *
 *  The client's own reason is left alone, so `mqtt_info` does not then say
 *  "busy" about a client that is fine.
 *
 *  @param verb The refused verb's name, without quotes. */
static void mqtt_publish_busy(const char *verb)
{
    KLOG_W(TAG, "the Kode channel holds the MQTT client; `%s` refused "
                "(an app that uses MQTT sets CONFIG_KODEOS_CHANNEL=n)", verb);
    mqtt_publish_answer((uint8_t)KDRV_MQTT_ERR_BUSY);
}
#endif /* CONFIG_KODEOS_CHANNEL */

/** @brief Answer `test component=net` with a verdict. */
static void net_publish_test(Net * const me, bool ok)
{
    TestResultEvt *e = Q_NEW_X(TestResultEvt, 0U, TEST_RESULT_SIG);
    if ( NULL != e )
    {
        (void)snprintf(e->name, sizeof(e->name), "%s", "net");
        e->ok = ok;
        e->origin = TEST_ORIGIN_AUTOTEST;
        QACTIVE_PUBLISH(&e->super, &me->super);
    }
}

/* ---- Jobs ---------------------------------------------------------------- */

/** @brief Stop a guard, counting an expiry that is already queued. */
static void net_guard_stop(NetGuard * const g)
{
    if ( kqp_timer_stale(&g->te, g->live) )
    {
        g->stale += 1U;
    }
    g->live = false;
}

/**
 * @brief Start the guard on a job just handed to a worker.
 *
 * @param g        The guard.
 * @param guard_ms How long the job may take before it is called wedged.
 */
static void net_guard_start(NetGuard * const g, uint32_t guard_ms)
{
    net_guard_stop(g);
    QTimeEvt_armX(&g->te, KQP_TICKS_PER_MS(guard_ms), 0U);
    g->live = true;
}

/**
 * @brief A guard's event arrived: is it the expiry of the job in flight?
 *
 * @param g The guard.
 * @return false for the expiry of a job that had already answered, counted
 *         off. Do not clear @c live then: it belongs to the next job's guard.
 */
static bool net_guard_genuine(NetGuard * const g)
{
    bool genuine = true;

    if ( 0U < g->stale )
    {
        g->stale -= 1U;
        genuine = false;
    }
    else
    {
        g->live = false;   /* this event IS its expiry */
    }
    return genuine;
}

/**
 * @brief The job as the first worker runs it: the driver's blocking call.
 *
 * Runs on the worker's task and touches only its argument, which is the AO's
 * @c jobs->req and stays valid until the done event.
 *
 * @param arg   The AO's @c jobs->req.
 * @param value Unused; what a job measured is in the driver's result tables.
 * @return What the driver answered.
 */
/* cppcheck-suppress constParameterCallback ; KqpJobFn fixes the signature, and its argument is void * */
static kode_status_t net_job_run(void *arg, uint32_t *value)
{
    *value = 0U;
    /* cppcheck-suppress misra-c2012-11.5 ; the worker hands back the pointer it was given, which is the AO's job storage */
    return kdrv_net_run((const kdrv_net_req_t *)arg);
}

/**
 * @brief The job as the download's worker runs it: the driver's blocking call
 *        for the kind it is.
 *
 * Runs on the worker's task and touches only its argument, which is the AO's
 * @c jobs->s and stays valid until the done event. Its sizes come back as the
 * value: the Content-Length for an open, the bytes read for a read.
 *
 * @param arg   The AO's @c jobs->s.
 * @param value Receives the one word the AO is told besides the status.
 * @return What the driver answered.
 */
/* cppcheck-suppress constParameterCallback ; KqpJobFn fixes the signature, and its argument is void * */
static kode_status_t net_stream_run(void *arg, uint32_t *value)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the worker hands back the pointer it was given, which is the AO's job storage */
    NetStreamJob const * const j  = (NetStreamJob const *)arg;
    kode_status_t              st = KODE_DENIED;

    *value = 0U;
    switch ( (net_stream_kind_t)j->kind )
    {
        case NETS_OPEN: {
            uint32_t total = 0U;
            uint16_t code  = 0U;

            st     = kdrv_net_stream_open(j->url, &total, &code);
            *value = total;
            break;
        }
        case NETS_READ: {
            /* The buffer is lent to this task until the done event (the
             * exception to "events carry copies": a chunk does not travel in
             * an event). */
            size_t got = 0U;

            st     = kdrv_net_stream_read(j->buf, NET_STREAM_CHUNK, &got);
            *value = (uint32_t)got;
            break;
        }
        case NETS_CLOSE: {
            kdrv_net_stream_close();
            st = KODE_OK;   /* closing what is not open is not an error */
            break;
        }
        default: {
            break;
        }
    }
    return st;
}

/**
 * @brief Hand the first worker @p fn over @p arg and start its guard, marking
 *        this AO busy.
 *
 * @param me  The AO.
 * @param fn  The job.
 * @param arg Its storage in @c jobs.
 * @return true when the worker has it; false when a job is with it already or
 *         it would not take this one.
 */
static bool net_worker_give(Net * const me, KqpJobFn fn, void *arg)
{
    uint16_t seq = 0U;

    if ( me->busy )
    {
        return false;
    }
    if ( kqp_worker_submit(&me->worker, fn, arg, NET_JOB_SIG, &seq) != KODE_OK )
    {
        return false;
    }
    me->wseq     = seq;
    me->wreplied = false;
    me->busy     = true;
    net_guard_start(&me->wguard, KODE_NET_GUARD_MS);
    return true;
}

/**
 * @brief Hand @c jobs->req to the first worker and start its guard, marking
 *        this AO busy.
 *
 * Nothing is handed over while a job is with the worker, one whose guard
 * fired included: the worker may still be using its storage.
 *
 * @param me The AO.
 * @return true when the worker has it; false when a job is with the worker,
 *         there is no storage or the worker would not take it.
 */
static bool net_job_start(Net * const me)
{
    if ( NULL == me->jobs )
    {
        return false;
    }
    return net_worker_give(me, &net_job_run, &me->jobs->req);
}

/**
 * @brief Hand the download's worker a job of @p kind and start its guard.
 *
 * Its arguments are in @c jobs->s already (the URL, the buffer).
 *
 * @param me       The AO.
 * @param kind     What the job is.
 * @param guard_ms How long it may take.
 * @return true when the worker has it; false when a download job is with the
 *         worker, there is no storage or the worker would not take it.
 */
static bool stream_job_start(Net * const me, net_stream_kind_t kind,
                             uint32_t guard_ms)
{
    uint16_t seq = 0U;

    if ( me->sbusy || (NULL == me->jobs) )
    {
        return false;
    }
    /* Before the submit: the worker may be reading it the instant it has the
     * job. */
    me->jobs->s.kind = (uint8_t)kind;
    if ( kqp_worker_submit(&me->sworker, &net_stream_run, &me->jobs->s,
                          NET_STREAM_JOB_SIG, &seq) != KODE_OK )
    {
        me->jobs->s.kind = (uint8_t)NETS_NONE;
        return false;
    }
    me->sseq     = seq;
    me->skind    = (uint8_t)kind;
    me->sreplied = false;
    me->sbusy    = true;
    net_guard_start(&me->sguard, guard_ms);
    return true;
}

/** @brief Hand a kdrv_net job to the first worker. */
static bool net_submit(Net * const me, const kdrv_net_req_t *req)
{
    if ( me->busy )
    {
        KLOG_W(TAG, "the worker is busy; job %d refused", (int)req->type);
        return false;
    }
    if ( NULL == me->jobs )
    {
        KLOG_W(TAG, "no job storage; job %d refused", (int)req->type);
        return false;
    }
    me->jobs->req = *req;
    if ( !net_job_start(me) )
    {
        KLOG_W(TAG, "the worker would not take job %d", (int)req->type);
        return false;
    }
    return true;
}

/** @brief Start a request struct of the right kind. */
static void net_req_init(kdrv_net_req_t *req, kdrv_net_job_t type)
{
    (void)memset(req, 0, sizeof(*req));
    req->type = type;
}

/* ---- The asks' jobs -------------------------------------------------------
 * An ask runs on the first worker over @c jobs->ask. A lookup or a ping lends
 * nothing, so its guard answers it at once; a GET, a POST or a datagram lends
 * the asker's memory, so its answer waits for the worker and says KODE_TIMEOUT.
 */

/**
 * @brief An ask's job as the worker runs it: the driver's blocking call for its kind.
 *
 * Runs on the worker's task and touches its argument, the AO's @c jobs->ask,
 * and the buffers that lends.
 *
 * @param arg   The AO's @c jobs->ask.
 * @param value Unused; the outputs are in @c jobs->ask.
 * @return What the driver answered.
 */
/* cppcheck-suppress constParameterCallback ; KqpJobFn fixes the signature, and its argument is void * */
static kode_status_t net_ask_run(void *arg, uint32_t *value)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the worker hands back the pointer it was given, which is the AO's job storage */
    NetAskJob * const j  = (NetAskJob *)arg;
    kode_status_t     st = KODE_DENIED;

    *value = 0U;
    if ( (uint8_t)NET_ASK_LOOKUP == j->kind )
    {
        st = kdrv_net_lookup(j->host, &j->ip4);
    }
    else if ( (uint8_t)NET_ASK_PING == j->kind )
    {
        st = kdrv_net_ping(j->host, j->count, NULL, 0U, NULL, &j->stats);
    }
    else if ( (uint8_t)NET_ASK_UDP == j->kind )
    {
        st = kdrv_net_udp_send(j->host, j->port, j->data, j->dlen);
    }
    else if ( j->post )
    {
        const char *ctype = NULL;

        if ( '\0' != j->ctype[0] )
        {
            ctype = j->ctype;
        }
        st = kdrv_net_http_post(j->url, j->body, j->len, ctype, j->buf, j->cap,
                                &j->result, KODE_HTTP_TIMEOUT_MS);
    }
    else
    {
        st = kdrv_net_http_get(j->url, j->buf, j->cap, &j->result, KODE_HTTP_TIMEOUT_MS);
    }
    return st;
}

/**
 * @brief Answer a lookup or a ping ask, once.
 * @param who The asking AO.
 * @param a   Its answer.
 * @param rc  The outcome.
 * @param j   The job whose outputs it carries; NULL for none.
 */
static void host_reply(QActive *who, NetHostRspEvt *a, kode_status_t rc, NetAskJob const *j)
{
    uint8_t    kind = (uint8_t)NET_ASK_LOOKUP;
    bool const ran  = (NULL != j) && (KODE_OK == rc);

    (void)memset(&a->stats, 0, sizeof(a->stats));
    a->rc = (uint8_t)rc;
    if ( (QSignal)NET_PING_RSP_SIG == a->super.sig )
    {
        kind = (uint8_t)NET_ASK_PING;
        if ( ran )
        {
            a->stats.ip4    = j->stats.ip4;
            a->stats.tx     = j->stats.tx;
            a->stats.rx     = j->stats.rx;
            a->stats.min_ms = j->stats.min_ms;
            a->stats.avg_ms = j->stats.avg_ms;
            a->stats.max_ms = j->stats.max_ms;
        }
    }
    else if ( ran )
    {
        a->stats.ip4 = j->ip4;
    }
    else
    {
        /* nothing resolved */
    }
    kaos_ask_answer(&s_net_doors[kind], who, &a->super, AO_Net);
}

/**
 * @brief Answer a GET or a POST ask, once.
 * @param who The asking AO.
 * @param a   Its answer.
 * @param rc  The outcome.
 * @param j   The job whose result it carries; NULL for none.
 */
static void http_reply(QActive *who, NetHttpRspEvt *a, kode_status_t rc, NetAskJob const *j)
{
    a->rc = (uint8_t)rc;
    (void)memset(&a->result, 0, sizeof(a->result));
    a->result.content_length = -1;
    if ( NULL != j )
    {
        a->result.status         = j->result.status;
        a->result.content_length = j->result.content_length;
        a->result.body_len       = j->result.body_len;
        a->result.kept           = j->result.kept;
        a->result.truncated      = j->result.truncated;
    }
    kaos_ask_answer(&s_net_doors[NET_ASK_HTTP], who, &a->super, AO_Net);
}

/**
 * @brief Answer a datagram ask, once.
 * @param who  The asking AO.
 * @param a    Its answer.
 * @param rc   The outcome.
 * @param sent Bytes that left.
 */
static void udp_reply(QActive *who, NetUdpDoneEvt *a, kode_status_t rc, uint16_t sent)
{
    a->rc   = (uint8_t)rc;
    a->sent = sent;
    kaos_ask_answer(&s_net_doors[NET_ASK_UDP], who, &a->super, AO_Net);
}

/**
 * @brief Refuse an ask that never became a job, answering its asker.
 * @param e  NET_HOST_ASK_SIG, NET_HTTP_ASK_SIG or NET_UDP_ASK_SIG.
 * @param rc The refusal.
 */
static void ask_refuse(QEvt const * const e, kode_status_t rc)
{
    if ( (QSignal)NET_HOST_ASK_SIG == e->sig )
    {
        NetHostAskEvt const * const q = Q_EVT_CAST(NetHostAskEvt);

        host_reply(q->who, q->rsp, rc, NULL);
    }
    else if ( (QSignal)NET_HTTP_ASK_SIG == e->sig )
    {
        NetHttpAskEvt const * const q = Q_EVT_CAST(NetHttpAskEvt);

        http_reply(q->who, q->rsp, rc, NULL);
    }
    else
    {
        NetUdpAskEvt const * const q = Q_EVT_CAST(NetUdpAskEvt);

        udp_reply(q->who, q->rsp, rc, 0U);
    }
}

/**
 * @brief Make an ask the worker's job, or refuse it while the worker is busy.
 * @param me The AO.
 * @param e  NET_HOST_ASK_SIG, NET_HTTP_ASK_SIG or NET_UDP_ASK_SIG.
 */
static void ask_take(Net * const me, QEvt const * const e)
{
    NetAskJob *j;

    if ( NULL == me->jobs )
    {
        KLOG_W(TAG, "no job storage; an app's request refused");
        ask_refuse(e, KODE_DENIED);
        return;
    }
    if ( me->busy )
    {
        KLOG_W(TAG, "a network job is already running; an app's request refused");
        ask_refuse(e, KODE_DENIED);
        return;
    }
    j = &me->jobs->ask;
    (void)memset(j, 0, sizeof(*j));
    j->result.content_length = -1;
    if ( (QSignal)NET_HOST_ASK_SIG == e->sig )
    {
        NetHostAskEvt const * const q = Q_EVT_CAST(NetHostAskEvt);

        j->kind = (uint8_t)NET_ASK_LOOKUP;
        if ( q->ping )
        {
            j->kind = (uint8_t)NET_ASK_PING;
        }
        j->count = q->count;
        (void)snprintf(j->host, sizeof(j->host), "%s", q->host);
        j->who  = q->who;
        j->hrsp = q->rsp;
    }
    else if ( (QSignal)NET_HTTP_ASK_SIG == e->sig )
    {
        NetHttpAskEvt const * const q = Q_EVT_CAST(NetHttpAskEvt);

        j->kind = (uint8_t)NET_ASK_HTTP;
        j->post = q->post;
        (void)snprintf(j->url, sizeof(j->url), "%s", q->url);
        (void)snprintf(j->ctype, sizeof(j->ctype), "%s", q->ctype);
        /* cppcheck-suppress misra-c2012-11.5 ; the asker lends its body as bytes, untyped in the event */
        j->body = q->body;
        j->len  = q->len;
        /* cppcheck-suppress misra-c2012-11.5 ; the asker lends its buffer as bytes, untyped in the event */
        j->buf  = q->buf;
        j->cap  = q->cap;
        j->who  = q->who;
        j->wrsp = q->rsp;
    }
    else
    {
        NetUdpAskEvt const * const q = Q_EVT_CAST(NetUdpAskEvt);

        j->kind = (uint8_t)NET_ASK_UDP;
        (void)snprintf(j->host, sizeof(j->host), "%s", q->host);
        j->port = q->port;
        j->data = q->data;
        j->dlen = q->len;
        j->who  = q->who;
        j->ursp = q->rsp;
    }
    if ( !net_worker_give(me, &net_ask_run, j) )
    {
        KLOG_W(TAG, "the worker would not take an app's request");
        ask_refuse(e, KODE_DENIED);
        return;
    }
    me->asking   = true;
    me->ask_late = false;
}

/**
 * @brief An ask's job came back: answer it, unless its guard already did.
 * @param me      The AO.
 * @param rc      How it went.
 * @param replied Whether the guard answered it.
 */
static void ask_done(Net * const me, kode_status_t rc, bool replied)
{
    NetAskJob const * const j   = &me->jobs->ask;
    NetAskJob const        *out = j;
    kode_status_t           st  = rc;

    if ( replied )
    {
        return;
    }
    if ( me->ask_late )
    {
        /* The worker let go of the lent memory only now; what it did is not vouched for. */
        me->ask_late = false;
        st  = KODE_TIMEOUT;
        out = NULL;
    }
    if ( (uint8_t)NET_ASK_HTTP == j->kind )
    {
        http_reply(j->who, j->wrsp, st, out);
    }
    else if ( (uint8_t)NET_ASK_UDP == j->kind )
    {
        uint16_t sent = 0U;

        if ( KODE_OK == st )
        {
            sent = j->dlen;
        }
        udp_reply(j->who, j->ursp, st, sent);
    }
    else
    {
        host_reply(j->who, j->hrsp, st, out);
    }
}

/**
 * @brief An ask's job outlived its guard. Nothing lent: answered now. Memory
 *        lent: the answer waits for the worker.
 * @param me The AO.
 */
static void ask_expired(Net * const me)
{
    NetAskJob const * const j = &me->jobs->ask;

    if ( ((uint8_t)NET_ASK_HTTP == j->kind) || ((uint8_t)NET_ASK_UDP == j->kind) )
    {
        me->ask_late = true;
        return;
    }
    host_reply(j->who, j->hrsp, KODE_TIMEOUT, NULL);
    me->wreplied = true;
}

/** @brief Write @p ip4 (network byte order) as a dotted quad into @p out. */
static void net_ip_str(uint32_t ip4, char *out, size_t cap)
{
    (void)snprintf(out, cap, "%u.%u.%u.%u",
                   (unsigned)(ip4 & 0xFFU),
                   (unsigned)((ip4 >> 8) & 0xFFU),
                   (unsigned)((ip4 >> 16) & 0xFFU),
                   (unsigned)((ip4 >> 24) & 0xFFU));
}

/* ---- The Kode channel ----------------------------------------------------
 * One sequence, one phase, one timer. Every step arms what it waits for (the
 * C5's answer, the worker's outcome, the broker's CONNACK); every failure ends
 * in chan_fail(). A unit without a certificate stays off, a certificate the
 * api refuses is terminal until the Wi-Fi cycles, and anything else is retried
 * on a doubling, jittered wait.
 */
#if CONFIG_KODEOS_CHANNEL

#if CONFIG_KODEOS_SHOT_HTTP
static void shot_channel_gone(Net * const me);

/** @brief How long the stream has run: until now, or until it ended. */
static uint32_t shot_ran_ms(const Net * const me)
{
    uint32_t ran = me->sran_ms;

    if ( (uint8_t)KSHOT_STREAM == me->smode )
    {
        ran = khal_time_ms() - me->sfrom_ms;
    }
    return ran;
}

/** @brief The frames a second the stream achieved, times ten, in integers. */
static uint16_t shot_fps_x10(const Net * const me)
{
    uint32_t const ran = shot_ran_ms(me);
    uint32_t       x10 = 0U;

    if ( 0U != ran )
    {
        x10 = (uint32_t)(((uint64_t)me->ssent * SHOT_FPS_X10_SCALE) / ran);
    }
    if ( (uint32_t)UINT16_MAX < x10 )
    {
        x10 = (uint32_t)UINT16_MAX;
    }
    return (uint16_t)x10;
}

/** @brief Whole seconds the running stream has left, rounded up; 0 when none runs. */
static uint32_t shot_left_s(const Net * const me)
{
    uint32_t left = 0U;

    if ( (uint8_t)KSHOT_STREAM == me->smode )
    {
        uint32_t const total = (uint32_t)me->ssecs * SHOT_MS_PER_S;
        uint32_t const ran   = shot_ran_ms(me);

        if ( ran < total )
        {
            left = ((total - ran) + (SHOT_MS_PER_S - 1U)) / SHOT_MS_PER_S;
        }
    }
    return left;
}
#endif

/** @brief Milliseconds as QP ticks, never zero (a zero arms nothing). */
static uint32_t chan_ticks(uint32_t ms)
{
    uint32_t t = (ms / 1000U) * KQP_TICKS_PER_SEC;
    t += ((ms % 1000U) * KQP_TICKS_PER_SEC) / 1000U;
    if ( 0U == t )
    {
        t = 1U;
    }
    return t;
}

/** @brief (Re)arm the channel's timer for @p ms. */
static void chan_arm(Net * const me, uint32_t ms)
{
    (void)QTimeEvt_disarm(&me->ctmo);
    QTimeEvt_armX(&me->ctmo, chan_ticks(ms), 0U);
}

#if CONFIG_KODE_LOWPOWER
/** @brief Whether the timer is a long one, whose deadline is `cdue_us`. Only the rest asks. */
static bool chan_due_pending(const Net * const me)
{
    return ((uint8_t)CP_UP == me->cp) || ((uint8_t)CP_WAIT == me->cp);
}
#endif

/** @brief Milliseconds to the long timer's deadline, rounded up; 0 when it has come. */
static uint32_t chan_due_left_ms(const Net * const me)
{
    uint64_t const now  = khal_time_us();
    uint64_t       left = 0U;

    if ( me->cdue_us > now )
    {
        left = ((me->cdue_us - now) + (CHAN_US_PER_MS - 1U)) / CHAN_US_PER_MS;
    }
    if ( (uint64_t)UINT32_MAX < left )
    {
        left = (uint64_t)UINT32_MAX;
    }
    return (uint32_t)left;
}

#if CONFIG_KODE_LOWPOWER
static bool net_lp_wake_note(Net const * const me);
#endif

/**
 * @brief Put the long timer where the board is: a QTimeEvt awake, the wake note asleep.
 *
 * @param me The AO, with `cdue_us` set.
 */
static void chan_due_arm(Net * const me)
{
    bool asleep = false;

#if CONFIG_KODE_LOWPOWER
    asleep = me->lp_rest;
#endif
    if ( asleep )
    {
#if CONFIG_KODE_LOWPOWER
        (void)QTimeEvt_disarm(&me->ctmo);
        if ( !net_lp_wake_note(me) )
        {
            KLOG_W(TAG, "channel: the wake time was not given; the deadline "
                        "waits for the next wake");
        }
#endif
    }
    else
    {
        chan_arm(me, chan_due_left_ms(me));
    }
}

#if CONFIG_KODE_LOWPOWER
static void net_lp_radio_check(Net * const me);
#endif

/**
 * @brief Wait @p ms of real time, for the renewal or the back-off.
 *
 * @param me The AO, with `cp` already Up or Wait.
 * @param ms How long.
 */
static void chan_wait(Net * const me, uint32_t ms)
{
    me->cdue_us = khal_time_us() + ((uint64_t)ms * CHAN_US_PER_MS);
    chan_due_arm(me);
#if CONFIG_KODE_LOWPOWER
    net_lp_radio_check(me);   /* a join that ended in a wait is the work done */
#endif
}

/**
 * @brief Turn the fast mode on or off, once: AO_Radio is asked, a NOTICE says so.
 *
 * Off also ends the hold. Asking for what already is does nothing, so a
 * burst of lines is one request and one line in the log.
 */
static void chan_fast_set(Net * const me, bool on)
{
    if ( me->cfast != on )
    {
        me->cfast = on;
        wifi_fast_request(on);
        if ( on )
        {
            KLOG_N(TAG, "channel: fast mode on (no Wi-Fi power save)");
        }
        else
        {
            KLOG_N(TAG, "channel: fast mode off");
        }
    }
    if ( !on )
    {
        (void)QTimeEvt_disarm(&me->ftmo);
    }
}

/** @brief A command line arrived: the fast mode is on, and held CHAN_FAST_HOLD_MS more. */
static void chan_fast_touch(Net * const me)
{
    chan_fast_set(me, true);
    (void)QTimeEvt_disarm(&me->ftmo);
    QTimeEvt_armX(&me->ftmo, chan_ticks(CHAN_FAST_HOLD_MS), 0U);
}

/** @brief (Re)arm the look at the client, CHAN_WATCH_MS from now. */
static void chan_watch_arm(Net * const me)
{
    (void)QTimeEvt_disarm(&me->wtmo);
    QTimeEvt_armX(&me->wtmo, chan_ticks(CHAN_WATCH_MS), 0U);
}

/** @brief Tell whoever listens where the channel stands. */
static void chan_publish(const Net * const me)
{
    ChannelStateEvt * const e = Q_NEW_X(ChannelStateEvt, 0U, CHANNEL_STATE_SIG);

    if ( NULL == e )
    {
        KLOG_W(TAG, "no event blocks; the channel's state went unreported");
        return;
    }
    e->state   = me->cstate;
    e->since_s = (uint32_t)((khal_time_us() - me->cstate_us) / 1000000ULL);
    e->next_s  = 0U;
    if ( ((uint8_t)CHAN_WAITING == me->cstate) && ((uint8_t)CP_WAIT == me->cp) )
    {
        e->next_s = (chan_due_left_ms(me) + 999U) / 1000U;
    }
    e->lost = kchan_out_lost();
    e->fast = me->cfast;
    e->keep_s  = 0U;
    e->session = (uint8_t)CHAN_SESSION_NONE;
    if ( (uint8_t)CHAN_UP == me->cstate )
    {
        e->keep_s  = me->ckeep_s;
        e->session = me->csess;
    }
#if CONFIG_KODEOS_SHOT_HTTP
    e->shot         = me->smode;
    e->shot_sent    = me->ssent;
    e->shot_fps_x10 = shot_fps_x10(me);
    e->shot_left_s  = shot_left_s(me);
#else
    e->shot         = (uint8_t)KSHOT_IDLE;
    e->shot_sent    = 0U;
    e->shot_fps_x10 = 0U;
    e->shot_left_s  = 0U;
#endif
    (void)snprintf(e->why, sizeof(e->why), "%s", me->cwhy);
    e->broker[0] = '\0';
    if ( NULL != me->cw )
    {
        (void)snprintf(e->broker, sizeof(e->broker), "%s", me->cw->sess.broker);
    }
    QACTIVE_PUBLISH(&e->super, &me->super);
}

/** @brief Tell AO_Net the ring holds lines. Posts a static event, from any task. */
static bool chan_out_wake(void)
{
    static const QEvt evt = QEVT_INITIALIZER(NET_CHAN_OUT_SIG);
    return QACTIVE_POST_X(AO_Net, &evt, KQP_NET_WAKE_MARGIN, NULL);
}

static void chan_q_pump(Net * const me);
static void net_rx_drain(Net * const me);

/**
 * @brief Throw away the reply lines nobody can publish any more, counted.
 *
 * What waits is the ring and at most one line taken from it.
 */
static void chan_out_drop(Net * const me)
{
    if ( (NULL != me->cw) && (0U != me->cw->held_len) )
    {
        me->cw->held_len = 0U;
        kchan_out_lose(1U);
    }
    kchan_out_clear();
}

/** @brief Move to @p state for @p why; reported when either changed. */
static void chan_set(Net * const me, chan_state_t state, const char *why)
{
    bool const moved = ((uint8_t)state != me->cstate);

    if ( moved || (strcmp(me->cwhy, why) != 0) )
    {
        if ( moved && ((CHAN_NO_NET == state) || (CHAN_REFUSED == state)
                      || (CHAN_NO_IDENTITY == state)) )
        {
            /* No session is coming that these lines could go out on. */
            chan_out_drop(me);
        }
        if ( moved )
        {
            me->cstate_us = khal_time_us();
        }
        me->cstate = (uint8_t)state;
        (void)snprintf(me->cwhy, sizeof(me->cwhy), "%s", why);
        chan_publish(me);
    }
}

/** @brief A session that stayed up long enough has earned a fresh back-off. */
static void chan_session_ended(Net * const me)
{
    if ( me->cup_set && ((khal_time_us() - me->cup_us) >= CHAN_STAY_UP_US) )
    {
        me->attempt = 0U;
    }
    me->cup_set = false;
}

/**
 * @brief Say "offline" ourselves, before a stop that is meant to be final.
 *
 * A clean stop sends a DISCONNECT, after which the broker does not fire the
 * last will, so the will's message is published first, best effort: a client
 * already down refuses it, and the broker then fires the will itself.
 */
static void chan_say_offline(const Net * const me)
{
    if ( (NULL != me->cw) && ((uint8_t)MQTT_LIVE == me->phase)
        && ('\0' != me->cw->sess.t_status[0]) )
    {
        (void)kdrv_net_mqtt_publish(me->cw->sess.t_status, KCHAN_WILL,
                                    (uint16_t)(sizeof(KCHAN_WILL) - 1U), true);
    }
}

/** @brief Stop the MQTT client, now or as soon as the worker is free. */
static void chan_stop_client(Net * const me)
{
    if ( ((uint8_t)MQTT_IDLE == me->phase)
        || ((uint8_t)MQTT_STOPPING == me->phase) )
    {
        me->stop_pending = false;
        return;
    }
    if ( !me->stop_pending )
    {
        chan_say_offline(me);   /* once per stop, not once per retry of it */
    }
    (void)QTimeEvt_disarm(&me->tmo);
    if ( me->busy )
    {
        me->stop_pending = true;
    }
    else
    {
        kdrv_net_req_t req;
        net_req_init(&req, KDRV_NET_JOB_MQTT_STOP);
        if ( net_submit(me, &req) )
        {
            me->phase        = (uint8_t)MQTT_STOPPING;
            me->stop_pending = false;
        }
        else
        {
            me->stop_pending = true;
        }
    }
}

/**
 * @brief Move the join to @p phase and keep what mqtt_channel_holds() reads.
 * @param me    The AO.
 * @param phase A chan_phase_t.
 */
static void chan_phase_set(Net * const me, uint8_t phase)
{
    me->cp = phase;
    atomic_store_explicit(&s_chan_holds, (uint8_t)CP_IDLE != phase,
                          memory_order_release);
}

/** @brief The channel holds the client or is about to ask for it. */
static bool chan_active(const Net * const me)
{
    return (uint8_t)CP_IDLE != me->cp;
}

/**
 * @brief A try failed: say so once, wait, start over.
 *
 * The only place a wait is chosen. A renewal keeps the old client (its token
 * has an hour left); anything else drops it, so no client outlives a failed
 * join.
 *
 * @param me   The AO.
 * @param why  One word for the owner's eyes and the log.
 * @param http The HTTP status that went with it, 0 when none arrived.
 */
static void chan_fail(Net * const me, const char *why, uint16_t http)
{
    uint32_t wait;

    chan_session_ended(me);
    if ( !me->crenew )
    {
        chan_stop_client(me);
#if CONFIG_KODEOS_SHOT_HTTP
        shot_channel_gone(me);   /* a renewal's old session still serves,
                                  * with its bearer */
#endif
    }
    if ( (uint8_t)CJ_LIVE == me->cjob )
    {
        /* A drop can fail the join with its POST still out: that answer
         * belongs to this try, not to the step the wait leads to. */
        me->cjob = (uint8_t)CJ_STALE;
    }
    wait = kchan_backoff_ms(me->attempt, khal_random());
    if ( 255U > me->attempt )
    {
        me->attempt++;
    }
    chan_phase_set(me, (uint8_t)CP_WAIT);
    me->cretry        = 0U;
    me->cwait_valid   = true;
    (void)snprintf(me->cwait_why, sizeof(me->cwait_why), "%s", why);
    chan_wait(me, wait);
    chan_set(me, CHAN_WAITING, why);
    if ( strcmp(me->cnote, why) != 0 )
    {
        (void)snprintf(me->cnote, sizeof(me->cnote), "%s", why);
        KLOG_N(TAG, "channel: waiting %u s (%u %s)",
               (unsigned)((wait + 500U) / 1000U), (unsigned)http, why);
    }
}

/** @brief The api refused this unit's certificate: terminal until a new Wi-Fi cycle. */
static void chan_refused(Net * const me, const char *why)
{
    (void)QTimeEvt_disarm(&me->ctmo);
    chan_session_ended(me);
    chan_phase_set(me, (uint8_t)CP_IDLE);
    me->crenew      = false;
    me->cwait_valid = false;
    (void)QTimeEvt_disarm(&me->wtmo);
    chan_fast_set(me, false);
    chan_stop_client(me);
#if CONFIG_KODEOS_SHOT_HTTP
    shot_channel_gone(me);
#endif
    chan_set(me, CHAN_REFUSED, why);
    KLOG_N(TAG, "channel: refused (%s)", why);
#if CONFIG_KODE_LOWPOWER
    net_lp_radio_check(me);
#endif
}

/** @brief The C5 holds no certificate: terminal until a new Wi-Fi cycle. */
static void chan_no_identity(Net * const me)
{
    (void)QTimeEvt_disarm(&me->ctmo);
    chan_phase_set(me, (uint8_t)CP_IDLE);
    me->crenew      = false;
    me->cwait_valid = false;
    (void)QTimeEvt_disarm(&me->wtmo);
    chan_fast_set(me, false);
    chan_stop_client(me);   /* a renewal's old client must not outlive this */
#if CONFIG_KODEOS_SHOT_HTTP
    shot_channel_gone(me);
#endif
    chan_set(me, CHAN_NO_IDENTITY, "no_identity");
    KLOG_N(TAG, "channel: no identity in the C5");
#if CONFIG_KODE_LOWPOWER
    net_lp_radio_check(me);
#endif
}

/**
 * @brief The worker is not free: try this step again shortly, or give up.
 *
 * Somebody else's job holds the one worker. A step that waits longer than
 * CHAN_RETRY_MAX polls is a failure like any other.
 */
static void chan_retry(Net * const me)
{
    if ( (uint8_t)CHAN_RETRY_MAX <= me->cretry )
    {
        chan_fail(me, "worker_busy", 0U);
    }
    else
    {
        me->cretry++;
        chan_arm(me, CHAN_RETRY_MS);
    }
}

/**
 * @brief Submit a channel job, or try again shortly.
 *
 * @return true when the job is with the worker. On false a retry is armed
 *         (or the join has failed, after CHAN_RETRY_MAX polls).
 */
static bool chan_submit(Net * const me, const kdrv_net_req_t *req)
{
    if ( net_submit(me, req) )
    {
        me->cretry = 0U;
        (void)QTimeEvt_disarm(&me->ctmo);
        return true;
    }
    chan_retry(me);
    return false;
}

/** @brief POST @p body to @p path through the worker. */
static void chan_post(Net * const me, const char *path, const char *body)
{
    kdrv_net_req_t req;

    if ( me->busy )
    {
        chan_retry(me);
        return;
    }
    if ( kdrv_net_http_post_body(body, strlen(body)) != KODE_OK )
    {
        chan_fail(me, "post_body", 0U);
        return;
    }
    net_req_init(&req, KDRV_NET_JOB_HTTP_POST);
    (void)snprintf(req.url, sizeof(req.url), "%s%s", CHAN_API_URL, path);
    if ( chan_submit(me, &req) )
    {
        me->cjob = (uint8_t)CJ_LIVE;
    }
}

/** @brief Step 2: ask the api for a challenge. */
static void chan_challenge(Net * const me)
{
    chan_phase_set(me, (uint8_t)CP_CHALLENGE);
    chan_post(me, "/kodes/challenge", "{}");
}

/** @brief Step 4: send the signed challenge to the api. */
static void chan_connect(Net * const me)
{
    chan_phase_set(me, (uint8_t)CP_CONNECT);
    chan_post(me, "/kodes/connect", me->cw->body);
}

/** @brief Step 1: the unit's certificate from the C5. */
static void chan_cert(Net * const me)
{
    chan_phase_set(me, (uint8_t)CP_CERT);
    if ( (uint8_t)IDO_CERT != me->id_out )
    {
        me->cw->der_len   = 0U;
        me->cw->unit_len  = 0U;
        me->cw->parts_got = 0U;
        me->cw->parts_of  = 0U;
        if ( !id_request(&me->super, (uint8_t)ID_OP_CERT, 0U, 0U, NULL, 0U) )
        {
            chan_fail(me, "no_events", 0U);
            return;
        }
        me->id_out = (uint8_t)IDO_CERT;
    }
    chan_arm(me, CHAN_ID_GUARD_MS);
}

/** @brief Start at step 1 (or 2, with a certificate in hand). */
static void chan_begin(Net * const me)
{
    if ( me->leaving )
    {
        return;   /* a leave was asked: no session starts again this boot */
    }
    me->cwait_valid = false;
    if ( NULL == me->cw )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; PSRAM storage aligned for any object, and sized for this one */
        me->cw = (ChanWork *)khal_alloc_psram(64U, sizeof(ChanWork));
        if ( NULL == me->cw )
        {
            chan_fail(me, "no_memory", 0U);
            return;
        }
        (void)memset(me->cw, 0, sizeof(*me->cw));
    }
    /* The ring the replies of cmd/in commands wait in, taken with the work
     * area (PSRAM, rule 8) and told whom to wake. */
    if ( !kchan_out_init() )
    {
        chan_fail(me, "no_memory", 0U);
        return;
    }
    kchan_out_set_wake(&chan_out_wake);
    if ( (uint8_t)CHAN_UP != me->cstate )
    {
        chan_set(me, CHAN_JOINING, "");
    }
    if ( me->cert_ok )
    {
        chan_challenge(me);
    }
    else
    {
        chan_cert(me);
    }
}

/**
 * @brief Entering Net_online: join, honouring a back-off a flap interrupted.
 *
 * @param me        The AO.
 * @param settle_ms The least wait before the join begins: 0 on the address
 *                  itself, CHAN_LP_SETTLE_MS for a start that waited for the
 *                  board to wake (AO_Radio is bringing the C5 link back on
 *                  that same event).
 */
static void chan_enter(Net * const me, uint32_t settle_ms)
{
    uint32_t left = 0U;

    if ( !me->chan_on )
    {
        return;   /* `chan=off`: the address is not a reason to join */
    }
    if ( me->leaving )
    {
        return;   /* nor is it after a leave */
    }
#if CONFIG_KODE_LOWPOWER
    if ( me->lp_rest )
    {
        me->lp_rejoin = true;   /* nothing starts while the board sleeps */
        /* ... and nothing else would wake the board for it. */
        if ( !lowpower_wake_at(me->lp_who, khal_time_us()) )
        {
            KLOG_W(TAG, "channel: the wake time was not given; the start "
                        "waits for the next wake");
        }
        return;
    }
#endif
    if ( me->cwait_valid )
    {
        left = chan_due_left_ms(me);
    }
    if ( 0U != left )
    {
        chan_phase_set(me, (uint8_t)CP_WAIT);
        if ( left < settle_ms )
        {
            me->cdue_us = khal_time_us() + ((uint64_t)settle_ms * CHAN_US_PER_MS);
        }
        chan_due_arm(me);
        chan_set(me, CHAN_WAITING, me->cwait_why);
        return;
    }
    if ( 0U != settle_ms )
    {
        /* The same wait as a back-off, its end the join's beginning. */
        chan_phase_set(me, (uint8_t)CP_WAIT);
        chan_wait(me, settle_ms);
        if ( (uint8_t)CHAN_UP != me->cstate )
        {
            chan_set(me, CHAN_JOINING, "");
        }
        return;
    }
    chan_begin(me);
}

#if CONFIG_KODE_LOWPOWER
/**
 * @brief The board is awake again: the long timer counts what is left of its
 *        real time.
 *
 * What the sleep ate comes off. A timer that fell due while the board slept
 * acts after CHAN_LP_SETTLE_MS, not on the instant. It repeats with every
 * STATE(AWAKE), an abandoned round's included, and re-arming is idempotent.
 *
 * @param me The AO.
 */
static void chan_lp_resume(Net * const me)
{
    if ( chan_due_pending(me) )
    {
        uint32_t left = chan_due_left_ms(me);

        if ( CHAN_LP_SETTLE_MS > left )
        {
            left = CHAN_LP_SETTLE_MS;
        }
        chan_arm(me, left);
    }
}
#endif

static void chan_queue_drop(Net * const me);

/**
 * @brief Whether the channel was told to go, as opposed to losing its network.
 *
 * `config key="chan" value=off` clears @c chan_on before it leaves, and the
 * leave for a restart or a jump sets @c leaving first. A loss of the address,
 * `wifi_down` and an OFF round of the board set neither: the commands that
 * wait for the channel keep waiting.
 */
static bool chan_told_to_go(const Net * const me)
{
    return (!me->chan_on) || me->leaving;
}

/**
 * @brief End everything the channel had going, and wipe its session.
 *
 * Says nothing to the broker and does not touch the client: the callers say
 * "offline" first (chan_say_offline() needs the session this wipes) and stop
 * the client after.
 */
static void chan_halt(Net * const me)
{
    (void)QTimeEvt_disarm(&me->ctmo);
    (void)QTimeEvt_disarm(&me->otmo);
    (void)QTimeEvt_disarm(&me->wtmo);
    chan_fast_set(me, false);
    chan_session_ended(me);
    chan_phase_set(me, (uint8_t)CP_IDLE);
    me->crenew  = false;
    me->cert_ok = false;
    me->cretry  = 0U;
    me->cnote[0] = '\0';
    chan_out_drop(me);
    if ( chan_told_to_go(me) )
    {
        chan_queue_drop(me);
    }
    if ( (uint8_t)CJ_LIVE == me->cjob )
    {
        /* The worker cannot be recalled: its answer will come, and belongs
         * to a join that is over. */
        me->cjob = (uint8_t)CJ_STALE;
    }
#if CONFIG_KODEOS_SHOT_HTTP
    shot_channel_gone(me);
#endif
    if ( NULL != me->cw )
    {
        /* Nothing of the session lingers: the token least of all. */
        (void)memset(&me->cw->sess, 0, sizeof(me->cw->sess));
        (void)memset(&me->cw->scratch, 0, sizeof(me->cw->scratch));
        (void)memset(&me->cw->ks, 0, sizeof(me->cw->ks));
        /* And the driver's copy, if no START took it. A START with the
         * worker reads it and spends it itself, and wiping it from under
         * that job would start a bench client in the channel's place. */
        if ( !me->busy )
        {
            (void)kdrv_net_mqtt_session(NULL);
        }
    }
#if CONFIG_KODE_LOWPOWER
    net_lp_radio_check(me);   /* a channel that ended is no work left */
#endif
}

/**
 * @brief Leaving Net_online: end everything the channel had going.
 *
 * @param me  The AO.
 * @param why What the state says: empty when the network went, a word when
 *            the channel was told to go (a leave for a jump).
 */
static void chan_leave(Net * const me, const char *why)
{
    if ( !me->stop_pending )
    {
        chan_say_offline(me);   /* before the session below is wiped */
    }
    chan_halt(me);
    if ( me->chan_on && ((uint8_t)CHAN_NO_NET != me->cstate) )
    {
        chan_set(me, CHAN_NO_NET, why);
        if ( '\0' == why[0] )
        {
            KLOG_N(TAG, "channel: no network");
        }
        else
        {
            KLOG_N(TAG, "channel: left (%s)", why);
        }
    }
}

/** @brief The seconds of each `chan_keep` word, in the order of the words. */
static const uint32_t    CHAN_KEEP_SECS[]     = KSET_CHAN_KEEP_SECS;
/** @brief How many ages `chan_keep` offers. */
#define CHAN_KEEP_N  ((uint8_t)(sizeof(CHAN_KEEP_SECS) / sizeof(CHAN_KEEP_SECS[0])))

_Static_assert(KSET_S_FOREVER == KDRV_MQTT_EXPIRY_NEVER,
               "`chan_keep` never is the driver's no-expiry value");

/**
 * @brief Which `chan_keep` word stands for @p secs.
 *
 * @param secs The seconds.
 * @return The index into the words, or CHAN_KEEP_N when none does.
 */
static uint8_t chan_keep_word(uint32_t secs)
{
    uint8_t at = CHAN_KEEP_N;

    for ( uint8_t i = 0U; CHAN_KEEP_N > i; i++ )
    {
        if ( CHAN_KEEP_SECS[i] == secs )
        {
            at = i;
        }
    }
    return at;
}

/**
 * @brief Read `chan_keep` from the store.
 *
 * A value that is missing, the wrong size or not one of the words is the
 * default, a day.
 *
 * @param[out] stored Whether the store held a usable value.
 * @return The seconds.
 */
static uint32_t chan_keep_load(bool *stored)
{
    uint32_t secs = KSET_CHAN_KEEP_DEF_S;
    uint32_t got  = 0U;
    size_t   len  = sizeof(got);

    *stored = false;
    if ( (khal_nvs_get_blob(KSET_NAMESPACE, KSET_KEY_CHAN_KEEP, &got, &len) == KODE_OK)
        && (sizeof(got) == len) && (chan_keep_word(got) < CHAN_KEEP_N) )
    {
        secs    = got;
        *stored = true;
    }
    return secs;
}

/**
 * @brief Answer `config key="chan_keep"`, setting it first when asked.
 *
 * The age is stored at once and the channel is left as it is: the next
 * connect, a renewal's included, asks the broker for it.
 *
 * @param me The AO.
 * @param r  The request.
 */
static void chan_keep_config(Net * const me, KodeConfigReqEvt const * const r)
{
    static const char *const WORDS[] = KSET_CHAN_KEEP_WORDS;
    kode_status_t status = KODE_OK;
    bool          stored = false;
    bool          wrote  = false;
    const char   *word   = "";
    uint8_t       at;

    _Static_assert((sizeof(WORDS) / sizeof(WORDS[0])) ==
                   (sizeof(CHAN_KEEP_SECS) / sizeof(CHAN_KEEP_SECS[0])),
                   "every `chan_keep` age has a word and its seconds");

    if ( r->set && (CHAN_KEEP_N > r->e) )
    {
        uint32_t const secs = CHAN_KEEP_SECS[r->e];

        wrote      = (khal_nvs_set_blob(KSET_NAMESPACE, KSET_KEY_CHAN_KEEP, &secs,
                                        sizeof(secs)) == KODE_OK);
        me->keep_s = secs;
        if ( wrote )
        {
            KLOG_N(TAG, "channel: keep the session %s (stored); the next connect "
                        "asks for it", WORDS[r->e]);
        }
        else
        {
            KLOG_W(TAG, "channel: keep the session %s (not stored: the write failed); "
                        "the next connect asks for it, a reset returns to the stored age",
                   WORDS[r->e]);
        }
    }
    else if ( r->set )
    {
        status = KODE_DENIED;
    }
    else
    {
        /* A read. */
    }
    if ( r->set && (CHAN_KEEP_N > r->e) )
    {
        /* What the store holds is the answer, not what an old value says. */
        stored = wrote;
    }
    else
    {
        (void)chan_keep_load(&stored);
    }
    at = chan_keep_word(me->keep_s);
    if ( CHAN_KEEP_N > at )
    {
        word = WORDS[at];
    }
    (void)kode_config_answer(&me->super, r, status, stored, word);
}

/**
 * @brief Answer `config key="chan"`, setting it first when asked.
 *
 * Off leaves the channel at once - the session ends, its client stops, the
 * state reads CHAN_OFF - and the next address joins nothing. On joins at
 * once when there is an address and the one MQTT client is free; with a
 * client of somebody's own up, the channel joins at the next address.
 *
 * @param me The AO.
 * @param r  The request.
 */
static void chan_config(Net * const me, KodeConfigReqEvt const * const r)
{
    uint8_t     v    = 0U;
    const char *word = "off";

    if ( strcmp(r->key, KSET_KEY_CHAN_KEEP) == 0 )
    {
        chan_keep_config(me, r);
        return;
    }
    if ( strcmp(r->key, KSET_KEY_CHAN) != 0 )
    {
        return;   /* another owner's key */
    }
    if ( r->set && (r->b != me->chan_on) )
    {
        if ( r->b )
        {
            v = 1U;
        }
        me->chan_on = r->b;
        (void)khal_nvs_set_u8(KSET_NAMESPACE, KSET_KEY_CHAN, v);
        if ( !r->b )
        {
            bool const held = chan_active(me);

            me->cwait_valid = false;
            if ( me->online )
            {
                chan_leave(me, "");   /* the halt drops the queue */
            }
            else
            {
                /* No network, so no halt: the queued commands must not run
                 * when the channel is switched on again. */
                chan_queue_drop(me);
            }
            if ( held )
            {
                chan_stop_client(me);
            }
            chan_set(me, CHAN_OFF, "");
            KLOG_N(TAG, "channel: off (stored)");
        }
        else if ( me->online && ((uint8_t)MQTT_IDLE == me->phase) )
        {
            KLOG_N(TAG, "channel: on (stored)");
            chan_enter(me, 0U);
        }
        else if ( me->online )
        {
            /* Somebody's own client holds the one MQTT client. */
            chan_set(me, CHAN_WAITING, "mqtt_busy");
            KLOG_N(TAG, "channel: on (stored); an MQTT client is up, so it "
                        "joins at the next address");
        }
        else
        {
            chan_set(me, CHAN_NO_NET, "");
            KLOG_N(TAG, "channel: on (stored); it joins at the next address");
        }
    }
    if ( me->chan_on )
    {
        word = "on";
    }
    bool const stored = (khal_nvs_get_u8(KSET_NAMESPACE, KSET_KEY_CHAN, &v) == KODE_OK);
    (void)kode_config_answer(&me->super, r, KODE_OK, stored, word);
}

/** @brief Step 6: the new client, with the session the api described. */
static void chan_start_now(Net * const me)
{
    kdrv_net_req_t req;
    kdrv_mqtt_session_t * const ks = &me->cw->ks;

    if ( (uint8_t)MQTT_IDLE != me->phase )
    {
        /* A client exists or is coming: this is a stale timer, or a STOP that
         * has not finished. A second START would be refused by the driver and
         * leave the phase idle over a live client. */
        return;
    }
    chan_phase_set(me, (uint8_t)CP_BROKER);
    if ( me->busy )
    {
        chan_retry(me);
        return;
    }
    (void)memset(ks, 0, sizeof(*ks));
    (void)snprintf(ks->client_id, sizeof(ks->client_id), "%s", me->cw->sess.client_id);
    (void)snprintf(ks->user, sizeof(ks->user), "%s", me->cw->sess.user);
    (void)snprintf(ks->pass, sizeof(ks->pass), "%s", me->cw->sess.token);
    (void)snprintf(ks->will_topic, sizeof(ks->will_topic), "%s", me->cw->sess.t_status);
    (void)snprintf(ks->will_msg, sizeof(ks->will_msg), "%s", KCHAN_WILL);
    ks->will_retain = true;
    ks->keepalive_s = (uint16_t)CHAN_KEEPALIVE_S;
    /* The broker keeps what is sent to cmd/in while the Kode is away, for the
     * age `chan_keep` holds now; one that changed since the last connect is
     * used from this one. */
    ks->mqtt5            = true;
    ks->keep_session     = true;
    ks->session_expiry_s = me->keep_s;
    ks->sub_qos          = CHAN_CMD_QOS;
    me->nkeep_s          = me->keep_s;
    if ( kdrv_net_mqtt_session(ks) != KODE_OK )
    {
        chan_fail(me, "session", 0U);
        return;
    }
    net_req_init(&req, KDRV_NET_JOB_MQTT_START);
    (void)snprintf(req.uri, sizeof(req.uri), "%s", me->cw->sess.broker);
    if ( chan_submit(me, &req) )
    {
        me->phase = (uint8_t)MQTT_STARTING;
        me->err   = (uint8_t)KDRV_MQTT_ERR_NONE;
        QTimeEvt_armX(&me->tmo, KQP_TICKS_PER_MS(KODE_MQTT_CONNECT_MS), 0U);
        /* The driver holds its own copy until the client exists, and wipes it
         * then: ours is not needed any more. */
        (void)memset(ks, 0, sizeof(*ks));
        (void)memset(me->cw->sess.token, 0, sizeof(me->cw->sess.token));
    }
}

/** @brief Step 5: the old client goes before the new one starts. */
static void chan_start_broker(Net * const me)
{
    chan_phase_set(me, (uint8_t)CP_STOPPING);
    /* From here the old client is going: a failure no longer has a session
     * to keep, so it must not be treated as a renewal that can wait. */
    me->crenew = false;
    if ( (uint8_t)MQTT_STOPPING == me->phase )
    {
        return;   /* its STOP_DONE carries on from here */
    }
    if ( (uint8_t)MQTT_IDLE == me->phase )
    {
        me->stop_pending = false;
        chan_start_now(me);
        return;
    }
    {
        kdrv_net_req_t req;
        (void)QTimeEvt_disarm(&me->tmo);
        net_req_init(&req, KDRV_NET_JOB_MQTT_STOP);
        if ( chan_submit(me, &req) )
        {
            me->phase        = (uint8_t)MQTT_STOPPING;
            me->stop_pending = false;
        }
    }
}

/**
 * @brief Read the HTTP job's answer into the work area.
 *
 * @param[out] http The status the server sent; 0 when none arrived.
 * @param[out] len  Bytes of body now in `answer`.
 * @return true when the server answered at all. On false the join has failed.
 */
static bool chan_read_answer(Net * const me, uint8_t job_status,
                             uint16_t *http, size_t *len)
{
    kdrv_net_http_result_t r;
    uint32_t               n;

    *http = 0U;
    *len  = 0U;
    me->cw->answer[0] = '\0';
    if ( (uint8_t)KODE_OK != job_status )
    {
        if ( (uint8_t)KODE_TIMEOUT == job_status )
        {
            chan_fail(me, "timeout", 0U);
        }
        else
        {
            chan_fail(me, "no_reply", 0U);
        }
        return false;
    }
    kdrv_net_http_result(&r);
    n = kdrv_net_http_body(0U, (uint8_t *)me->cw->answer,
                           (uint32_t)sizeof(me->cw->answer) - 1U);
    kdrv_net_http_forget();   /* a connect answer holds the token */
    me->cw->answer[n] = '\0';
    *http = r.status;
    *len  = (size_t)n;
    return true;
}

/** @brief The api answered, but not with what a join needs. */
static void chan_bad_answer(Net * const me, uint16_t http, size_t len)
{
    char word[24];

    if ( !kchan_json_str(me->cw->answer, len, "error", word, sizeof(word)) )
    {
        word[0] = '\0';
    }
    if ( kchan_classify(http, word) == KCHAN_TERMINAL )
    {
        chan_refused(me, word);
    }
    else if ( '\0' != word[0] )
    {
        chan_fail(me, word, http);
    }
    else
    {
        chan_fail(me, "bad_answer", http);
    }
}

/** @brief The challenge came back: hand its nonce to the C5. */
static void chan_on_challenge(Net * const me, uint8_t job_status)
{
    uint16_t http;
    size_t   len;

    if ( !chan_read_answer(me, job_status, &http, &len) )
    {
        return;
    }
    if ( (200U != http)
        || (!kchan_parse_nonce(me->cw->answer, len, me->cw->nonce,
                               sizeof(me->cw->nonce))) )
    {
        chan_bad_answer(me, http, len);
        return;
    }
    if ( !id_sign_request(&me->super, me->cw->nonce) )
    {
        chan_fail(me, "no_events", 0U);
        return;
    }
    me->id_out = (uint8_t)IDO_SIGN;
    chan_phase_set(me, (uint8_t)CP_SIGN);
    chan_arm(me, CHAN_ID_GUARD_MS);
}

/** @brief The connect came back: a session, or the reason there is none. */
static void chan_on_connect(Net * const me, uint8_t job_status)
{
    uint16_t http;
    size_t   len;

    if ( !chan_read_answer(me, job_status, &http, &len) )
    {
        return;
    }
    if ( (200U == http)
        && kchan_parse_connect(me->cw->answer, len, khal_identity_serial(),
                               &me->cw->scratch) )
    {
        /* Read whole, so it may replace the session. A failed read leaves
         * the live one alone: during a renewal the old client still uses its
         * topics. */
        me->cw->sess = me->cw->scratch;
        (void)memset(&me->cw->scratch, 0, sizeof(me->cw->scratch));
#if CONFIG_KODEOS_SHOT_HTTP
        /* The uploads' bearer, kept: the MQTT start wipes the session's. */
        (void)snprintf(me->cw->jwt, sizeof(me->cw->jwt), "%s", me->cw->sess.token);
        if ( !kchan_shots_valid(me->cw->sess.shots) )
        {
            KLOG_W(TAG, "channel: the session names no upload link; "
                        "uploads are refused");
        }
#endif
        /* The token is in the session now; the answer need not keep a copy. */
        (void)memset(me->cw->answer, 0, sizeof(me->cw->answer));
        chan_start_broker(me);
        return;
    }
    chan_bad_answer(me, http, len);
    (void)memset(me->cw->answer, 0, sizeof(me->cw->answer));
}

/** @brief An HTTP job the channel submitted has finished. */
static void chan_on_http(Net * const me, uint8_t job_status)
{
    if ( (uint8_t)CP_CHALLENGE == me->cp )
    {
        chan_on_challenge(me, job_status);
    }
    else if ( (uint8_t)CP_CONNECT == me->cp )
    {
        chan_on_connect(me, job_status);
    }
    else
    {
        /* The channel left or restarted while this was in the air: the
         * answer belongs to a join that no longer exists. */
        KLOG_I(TAG, "channel: an answer from a join that ended was dropped");
    }
}

/** @brief Step 3: the signature is here; send the certificate and it to the api. */
static void chan_on_signature(Net * const me, const IdSignRspEvt *r)
{
    uint8_t der[72];

    if ( (uint8_t)KODE_ST_NOT_FOUND == r->status )
    {
        chan_no_identity(me);
        return;
    }
    if ( ((uint8_t)KODE_ST_OK != r->status) || (0U == r->der_len)
        || (sizeof(der) < r->der_len) )
    {
        if ( (uint8_t)KODE_ST_REFUSED == r->status )
        {
            chan_fail(me, "c5_busy", 0U);
        }
        else
        {
            chan_fail(me, "c5_error", 0U);
        }
        return;
    }
    (void)memcpy(der, r->der, r->der_len);
    (void)kaos_wire_b64_encode(der, r->der_len, me->cw->sig_b64);
    if ( kchan_connect_body(me->cw->body, sizeof(me->cw->body),
                           me->cw->cert_b64, me->cw->nonce,
                           me->cw->sig_b64) == 0U )
    {
        /* A certificate too big for the request is not going to shrink. */
        chan_refused(me, "request_size");
        return;
    }
    chan_connect(me);
}

/** @brief The end of the C5's certificate answer. */
static void chan_on_cert_end(Net * const me, const IdEndEvt *r)
{
    ChanWork * const w = me->cw;

    if ( (uint8_t)KODE_ST_NOT_FOUND == r->status )
    {
        chan_no_identity(me);
        return;
    }
    if ( ((uint8_t)KODE_ST_OK != r->status) || (0U == w->parts_got)
        || (w->parts_got != w->parts_of) || (0U == r->unit_len)
        || (r->unit_len > w->der_len) )
    {
        if ( (uint8_t)KODE_ST_REFUSED == r->status )
        {
            chan_fail(me, "c5_busy", 0U);
        }
        else
        {
            chan_fail(me, "c5_error", 0U);
        }
        return;
    }
    w->unit_len = r->unit_len;
    (void)kaos_wire_b64_encode(w->der, w->unit_len, w->cert_b64);
    me->cert_ok = true;
    chan_challenge(me);
}

/**
 * @brief The broker said yes: subscribe, say we are here, arm the renewal.
 *
 * Also runs for a second CONNECTED of a live session: a clean session forgets
 * its subscription, and the will may have flipped the presence off.
 *
 * @param me              The AO.
 * @param session_present The CONNACK's flag: the broker resumed the session it
 *                        held for this client id.
 */
static void chan_on_connected(Net * const me, bool session_present)
{
    char             status[128];
    kdrv_slot_desc_t desc;
    const char      *app     = "unknown";
    const char      *version = "unknown";
    size_t           n;
    bool const       fresh = ((uint8_t)CP_BROKER == me->cp);
    bool const       live  = fresh || ((uint8_t)CP_UP == me->cp);

    if ( (!live) && (!me->crenew) )
    {
        return;   /* a client nobody is waiting for */
    }
    if ( kdrv_net_mqtt_subscribe(me->cw->sess.t_in) != KODE_OK )
    {
        chan_fail(me, "subscribe", 0U);
        return;
    }
    if ( kdrv_running_desc(&desc) == KODE_OK )
    {
        app     = desc.project;
        version = desc.version;
    }
    n = kchan_status_body(status, sizeof(status), app, version);
    if ( 0U == n )
    {
        n = kchan_status_body(status, sizeof(status), "unknown", "unknown");
    }
    if ( (0U == n)
        || (kdrv_net_mqtt_publish(me->cw->sess.t_status, status,
                                  (uint16_t)n, true) != KODE_OK) )
    {
        chan_fail(me, "publish", 0U);
        return;
    }
    if ( !live )
    {
        return;   /* the old client of a renewal under way: served, no more */
    }
    me->cup_us  = khal_time_us();
    me->cup_set = true;
    if ( fresh )
    {
        chan_phase_set(me, (uint8_t)CP_UP);
        /* What channel_info shows is the new session's, from its connect; the
         * old one's stood until now. */
        me->ckeep_s = me->nkeep_s;
        if ( session_present )
        {
            me->csess = (uint8_t)CHAN_SESSION_RESUMED;
            KLOG_N(TAG, "channel: session resumed");
        }
        else
        {
            me->csess = (uint8_t)CHAN_SESSION_NEW;
            KLOG_N(TAG, "channel: new session (nothing kept for this Kode)");
        }
        chan_wait(me, NET_CHAN_RENEW_MS);
        chan_watch_arm(me);
        if ( (uint8_t)CHAN_UP == me->cstate )
        {
            KLOG_N(TAG, "channel: renewed as %s", me->cw->sess.client_id);
        }
    }
    me->cnote[0] = '\0';
    if ( (uint8_t)CHAN_UP != me->cstate )
    {
        chan_set(me, CHAN_UP, "");
        KLOG_N(TAG, "channel: up as %s", me->cw->sess.client_id);
    }
    if ( (0U != me->cw->held_len) || kchan_out_pending() )
    {
        /* Lines that waited for this session go out now. */
        (void)chan_out_wake();
    }
    /* So do the commands that waited for it: their answers have a way out. */
    chan_q_pump(me);
}

/**
 * @brief The broker connection ended: the wait before another is ours.
 *
 * The channel's client does not redial, so a drop is a failure like any other
 * and ends in chan_fail().
 */
static void chan_on_disconnected(Net * const me)
{
    if ( me->crenew )
    {
        /* The old client of a renewal: it will not redial, so the renewal
         * ends and the join starts over; the dead client is stopped. */
        me->crenew = false;
        chan_fail(me, "broker_lost", 0U);
    }
    else if ( (uint8_t)CP_UP == me->cp )
    {
        chan_fail(me, "broker_lost", 0U);
    }
    else if ( (uint8_t)CP_BROKER == me->cp )
    {
        chan_fail(me, "broker_unreachable", 0U);
    }
    else
    {
        /* Nothing the channel was waiting for. */
    }
}

/** @brief The broker refused the credentials: back to step 1 through the wait. */
static void chan_on_refused(Net * const me)
{
    if ( ((uint8_t)CP_BROKER == me->cp) || ((uint8_t)CP_UP == me->cp) )
    {
        chan_fail(me, "broker_refused", 0U);
    }
}

/**
 * @brief The look at the client: the channel's belief against the driver's.
 *
 * The link event that says a client is gone can be lost on a full queue. So
 * while a client serves (up, or a renewal's old client) the driver is asked
 * every CHAN_WATCH_MS, and one that is not up is a broker lost. Not re-armed
 * once the channel is not serving: the next CONNECTED arms it.
 */
static void chan_on_watch(Net * const me)
{
    bool const serving = ((uint8_t)MQTT_LIVE == me->phase)
                         && (((uint8_t)CP_UP == me->cp) || me->crenew);

    if ( !serving )
    {
        return;
    }
    /* A notice the driver could not post leaves messages in its ring: this is
     * the look that finds them, within CHAN_WATCH_MS. */
    net_rx_drain(me);
    if ( kdrv_net_mqtt_state() == KDRV_MQTT_UP )
    {
        chan_watch_arm(me);
    }
    else
    {
        me->crenew = false;
        chan_fail(me, "broker_lost", 0U);
    }
}

/** @brief The one timer fired: what it means depends on the step. */
static void chan_on_timer(Net * const me)
{
#if CONFIG_KODE_LOWPOWER
    if ( me->lp_rest && chan_due_pending(me) )
    {
        /* It fell due as the board went to sleep: nothing starts now. The
         * deadline stands, so the note brings the board back for it. */
        (void)net_lp_wake_note(me);
        return;
    }
#endif
    switch ( (chan_phase_t)me->cp )
    {
        case CP_UP: {
            /* 23 h of a 24 h token: ask again while the old session serves. */
            KLOG_I(TAG, "channel: renewing the session");
            me->crenew  = true;
            me->attempt = 0U;
            chan_challenge(me);
            break;
        }
        case CP_WAIT: {
            chan_begin(me);
            break;
        }
        case CP_CERT:
        case CP_SIGN: {
            me->id_out = (uint8_t)IDO_NONE;
            chan_fail(me, "c5_timeout", 0U);
            break;
        }
        case CP_CHALLENGE: {
            chan_challenge(me);
            break;
        }
        case CP_CONNECT: {
            chan_connect(me);
            break;
        }
        case CP_STOPPING: {
            chan_start_broker(me);
            break;
        }
        case CP_BROKER: {
            chan_start_now(me);
            break;
        }
        default: {
            break;
        }
    }
}

/** @brief One part of the certificate pair arrived. */
static void chan_on_part(Net * const me, const IdPartEvt *p)
{
    ChanWork * const w = me->cw;
    size_t const     at = ((size_t)p->n - 1U) * (size_t)KODE_ID_PART_BYTES;

    if ( (NULL == w) || ((uint8_t)IDO_CERT != me->id_out) || (0U == p->n)
        || (0U == p->of) || ((uint8_t)KODE_ID_PARTS_MAX < p->of)
        || (p->n > p->of) || ((uint8_t)KODE_ID_PART_BYTES < p->len)
        || ((at + p->len) > sizeof(w->der)) )
    {
        return;
    }
    (void)memcpy(&w->der[at], p->der, p->len);
    w->parts_of = p->of;
    w->parts_got++;
    w->der_len = (uint16_t)(at + p->len);
}

/**
 * @brief An MQTT job the worker finished, seen from the channel.
 *
 * @param me   The AO.
 * @param type Which job: MQTT_START or MQTT_STOP.
 * @param rc   How it went.
 */
static void chan_on_mqtt_job(Net * const me, kdrv_net_job_t type,
                             kode_status_t rc)
{
    if ( (KDRV_NET_JOB_MQTT_START == type)
        && ((uint8_t)CP_BROKER == me->cp)
        && (KODE_OK != rc) )
    {
        chan_fail(me, "start_failed", 0U);
    }
    else if ( (KDRV_NET_JOB_MQTT_STOP == type)
             && ((uint8_t)CP_STOPPING == me->cp) )
    {
        chan_start_now(me);
    }
    else
    {
        /* Nothing the channel was waiting for. */
    }
}

/** @brief After any outcome: a stop owed, and a step that waited for the worker. */
static void chan_after_job(Net * const me)
{
    if ( me->stop_pending && (!me->busy) )
    {
        chan_stop_client(me);
    }
}

/** @brief Reply lines handed to the client per step; the rest wait their turn. */
#define CHAN_OUT_BURST  8U

/** @brief Wait before the drain tries again after its own post was refused. */
#define CHAN_OUT_RETRY_MS  100U

/** @brief Slots AO_Console's queue keeps free when a line off the channel is posted to it. */
#define CHAN_LINE_MARGIN  2U

/**
 * @brief Whether the session's client may be handed a line right now.
 *
 * A connected client of the live session, or the old client while a renewal
 * runs (`crenew`). `crenew` ends the moment that client is told to stop, so a
 * line is never handed to one on its way out.
 */
static bool chan_out_ready(const Net * const me)
{
    bool ready = false;

    if ( NULL != me->cw )
    {
        if ( ((uint8_t)CP_UP == me->cp) && ((uint8_t)CHAN_UP == me->cstate) )
        {
            ready = true;
        }
        else if ( ((uint8_t)MQTT_LIVE == me->phase) && me->crenew )
        {
            ready = true;
        }
        else
        {
            /* Joining, waiting or replacing the client: the next CONNECTED
             * brings the drain back. */
        }
    }
    return ready;
}

/**
 * @brief The ring holds lines: publish a few, or leave them, or drop them.
 *
 * Up, a step hands up to CHAN_OUT_BURST lines to esp-mqtt and posts itself
 * again while more wait, so the rest of the system runs between bursts (rule
 * 5). Joining or waiting, the lines stay for the next CONNECTED. When no
 * session can come, they are dropped.
 */
static void chan_out_step(Net * const me)
{
    if ( ((uint8_t)CHAN_NO_NET == me->cstate) || ((uint8_t)CHAN_NO_IDENTITY == me->cstate)
        || ((uint8_t)CHAN_REFUSED == me->cstate) || ((uint8_t)CHAN_OFF == me->cstate) )
    {
        chan_out_drop(me);
        return;
    }
    if ( !chan_out_ready(me) )
    {
        return;   /* joining, waiting or renewing: the next CONNECTED comes back */
    }
    for ( uint8_t i = 0U; CHAN_OUT_BURST > i; i++ )
    {
        ChanWork * const cw = me->cw;
        kode_status_t    rc;

        if ( 0U == cw->held_len )
        {
            cw->held_len = (uint16_t)kchan_out_pop(cw->held, sizeof(cw->held));
        }
        if ( 0U == cw->held_len )
        {
            return;   /* drained */
        }
        rc = kdrv_net_mqtt_publish(cw->sess.t_out, cw->held, cw->held_len, false);
        if ( KODE_DENIED == rc )
        {
            return;   /* the client just went down: its LINK event follows */
        }
        cw->held_len = 0U;
        if ( KODE_OK != rc )
        {
            kchan_out_lose(1U);
        }
    }
    if ( kchan_out_pending() && (!chan_out_wake()) )
    {
        /* The queue has no room for the next step (a post keeps one slot
         * free): come back on a timer, the ring holds the lines meanwhile. */
        (void)QTimeEvt_disarm(&me->otmo);
        QTimeEvt_armX(&me->otmo, chan_ticks(CHAN_OUT_RETRY_MS), 0U);
    }
}

/* ---- The queue of cmd/in lines ---------------------------------------------
 * The console takes one command at a time, so cmd/in lines wait here (a ring
 * in PSRAM) and go to it in order. A line is out until the console ends it
 * (net_line_done(), with the sequence it was offered under) or
 * KODE_CHAN_LINE_BOUND_MS passes. esp-mqtt has already acknowledged the lines:
 * a reset or the channel being told to go loses them; a loss of network does not.
 */

/* The ring's indices are bytes, and the driver's ring holds at least a full queue. */
_Static_assert(KODE_CHAN_QUEUE_MAX <= UINT8_MAX, "the channel queue's head and length are bytes");
_Static_assert(KDRV_MQTT_RX_RING >= KODE_CHAN_QUEUE_MAX,
               "the driver's receive ring must hold a full channel queue");

/** @brief Wait before the head is offered to the console again after the post was refused. */
#define CHAN_LINE_RETRY_MS  100U

/** @brief Whether the ring has no room for another line. */
static bool chan_q_full(const ChanWork * const cw)
{
    return ((uint8_t)KODE_CHAN_QUEUE_MAX <= cw->cq_len);
}

/**
 * @brief Append a line to the ring.
 *
 * @param cw The work area.
 * @param m  The message: exactly @c len bytes, which chan_cmd_flaw() has
 *           found to fit a console line with its terminator.
 */
static void chan_q_push(ChanWork * const cw, const kdrv_mqtt_msg_t * const m)
{
    uint8_t const at = (uint8_t)(((uint32_t)cw->cq_head + (uint32_t)cw->cq_len)
                                 % (uint32_t)KODE_CHAN_QUEUE_MAX);

    (void)memcpy(cw->cq[at], m->data, m->len);
    cw->cq[at][m->len] = '\0';
    cw->cq_len++;
}

/** @brief Remove the oldest line: it has ended, or the bound gave up on it. */
static void chan_q_pop(ChanWork * const cw)
{
    cw->cq_head = (uint8_t)(((uint32_t)cw->cq_head + 1U) % (uint32_t)KODE_CHAN_QUEUE_MAX);
    cw->cq_len--;
    cw->cq_out  = false;
    cw->cq_said  = false;
    cw->cq_bsaid = false;
}

/** @brief Empty the ring: every line in it, the one out included, is dropped. */
static void chan_q_clear(ChanWork * const cw)
{
    cw->cq_head  = 0U;
    cw->cq_len   = 0U;
    cw->cq_out   = false;
    cw->cq_said  = false;
    cw->cq_bsaid = false;
}

/**
 * @brief Arm the line timer for @p ms, in place of any earlier arming.
 *
 * @param me The AO.
 * @param ms The delay: the bound on a line out, or the retry of one not taken.
 */
static void chan_line_arm(Net * const me, uint32_t ms)
{
    net_guard_stop(&me->lguard);
    QTimeEvt_armX(&me->lguard.te, chan_ticks(ms), 0U);
    me->lguard.live = true;
}

/**
 * @brief Give the console the oldest line, when nothing is out.
 *
 * Only while the channel can carry the answer back (chan_out_ready()); the
 * next CONNECTED comes back here. The line stays in the ring until it ends. A
 * refused post is tried again after CHAN_LINE_RETRY_MS, never dropped.
 */
static void chan_q_pump(Net * const me)
{
    ChanWork * const cw = me->cw;

    if ( (NULL != cw) && (!cw->cq_out) && (0U != cw->cq_len) && chan_out_ready(me) )
    {
        ConsoleLineEvt *e     = Q_NEW_X(ConsoleLineEvt, 0U, CONSOLE_LINE_SIG);
        bool            taken = false;

        cw->cq_seq++;
        if ( NULL != e )
        {
            e->src = (uint8_t)KODE_CONSOLE_SRC_NET;
            e->seq = cw->cq_seq;
            (void)snprintf(e->line, sizeof(e->line), "%s", cw->cq[cw->cq_head]);
            /* A refused post recycles the event itself. */
            taken = QACTIVE_POST_X(AO_Console, &e->super, CHAN_LINE_MARGIN,
                                   AO_Net);
        }
        if ( taken )
        {
            cw->cq_out = true;
            chan_line_arm(me, KODE_CHAN_LINE_BOUND_MS);
        }
        else
        {
            if ( !cw->cq_said )
            {
                cw->cq_said = true;
                KLOG_W(TAG, "channel: the console has no room; the command waits (%u queued)",
                       (unsigned)cw->cq_len);
            }
            chan_line_arm(me, CHAN_LINE_RETRY_MS);
        }
    }
}

/**
 * @brief NET_LINE_DONE_SIG: the console ended a line; if it is the one out, the next goes.
 *
 * @param me  The AO.
 * @param seq The sequence the console returned.
 */
static void chan_q_done(Net * const me, uint8_t seq)
{
    ChanWork * const cw = me->cw;

    if ( (NULL != cw) && cw->cq_out && (seq == cw->cq_seq) )
    {
        net_guard_stop(&me->lguard);
        chan_q_pop(cw);
        chan_q_pump(me);
    }
    else
    {
        KLOG_D(TAG, "channel: the console ended a line AO_Net no longer waits for");
    }
}

/**
 * @brief NET_LINE_BUSY_SIG: the console was busy with another command and did not take the line.
 *
 * The line stays at the head and is offered again after CHAN_LINE_RETRY_MS:
 * it is not answered busy and not dropped. The bound is not running for a
 * line nobody holds.
 */
static void chan_q_busy(Net * const me)
{
    ChanWork * const cw = me->cw;

    if ( (NULL != cw) && cw->cq_out )
    {
        cw->cq_out = false;
        if ( !cw->cq_bsaid )
        {
            /* Once an episode: it ends with the line. */
            cw->cq_bsaid = true;
            KLOG_W(TAG, "channel: the console is busy with another command; the command waits (%u queued)",
                   (unsigned)cw->cq_len);
        }
        chan_line_arm(me, CHAN_LINE_RETRY_MS);
    }
    else
    {
        KLOG_D(TAG, "channel: the console refused a line AO_Net no longer waits for");
    }
}

/**
 * @brief NET_CHAN_LINE_SIG: the bound on the line out has passed, or a refused post is due again.
 *
 * A line the console never answers is given up on and the next goes; its
 * late end names an old sequence and is ignored. A stale expiry is dropped
 * (net_guard_genuine()).
 */
static void chan_q_timer(Net * const me)
{
    ChanWork * const cw = me->cw;

    if ( !net_guard_genuine(&me->lguard) )
    {
        KLOG_D(TAG, "channel: a line timer that was already disarmed is ignored");
    }
    else if ( NULL != cw )
    {
        if ( cw->cq_out )
        {
            KLOG_W(TAG, "channel: the console did not answer in %u ms; the next command goes",
                   (unsigned)KODE_CHAN_LINE_BOUND_MS);
            chan_q_pop(cw);
        }
        chan_q_pump(me);
    }
    else
    {
        /* No work area: nothing was ever queued. */
    }
}

/**
 * @brief Why a cmd/in payload cannot be a console line, or NULL when it can.
 *
 * A line must arrive whole: a cut payload, one with no room for the
 * terminator, an empty one, and one with a line break or a NUL are refused.
 * So is a retained one: the broker replays it on every connect, and it would
 * run again with each reconnect.
 */
static const char *chan_cmd_flaw(const kdrv_mqtt_msg_t *m)
{
    const char *flaw = NULL;

    if ( m->retained )
    {
        flaw = "retained";
    }
    else if ( m->data_cut || ((uint16_t)KODE_CONSOLE_LINE_MAX <= m->len) )
    {
        flaw = "too long";
    }
    else if ( 0U == m->len )
    {
        flaw = "empty";
    }
    else
    {
        for ( uint16_t i = 0U; i < m->len; ++i )
        {
            uint8_t const b = m->data[i];
            if ( ((uint8_t)'\n' == b) || ((uint8_t)'\r' == b) || (0U == b) )
            {
                flaw = "line break";
                break;
            }
        }
    }
    return flaw;
}

/**
 * @brief A message on cmd/in: queue it, to go to the console as a line from NET.
 *
 * Exactly @c len bytes are copied (no NUL). The line goes to the console in
 * its turn, posted with a margin: a full console queue must not assert. A line
 * that finds the queue full is answered `>err code=4 msg=busy` on the channel.
 */
static void chan_on_cmd(Net * const me, const kdrv_mqtt_msg_t *m)
{
    const char * const flaw = chan_cmd_flaw(m);

    if ( NULL != flaw )
    {
        KLOG_W(TAG, "channel: command refused, %s (%u bytes)", flaw,
               (unsigned)m->len);
        /* The owner sees why, on the channel's own path. */
        if ( m->retained )
        {
            kode_log_reply_src(">err code=3 msg=retained",
                               (uint8_t)KODE_CONSOLE_SRC_NET);
        }
        else
        {
            kode_log_reply_src(">err code=3 msg=line",
                               (uint8_t)KODE_CONSOLE_SRC_NET);
        }
    }
    else
    {
        /* Somebody is typing: answer them at once for a while. */
        chan_fast_touch(me);

        if ( (NULL != me->cw) && (!chan_q_full(me->cw)) )
        {
            chan_q_push(me->cw, m);
            chan_q_pump(me);
        }
        else
        {
            KLOG_W(TAG, "channel: command refused, queue full (%u bytes)",
                   (unsigned)m->len);
            kode_log_reply_src(">err code=4 msg=busy",
                               (uint8_t)KODE_CONSOLE_SRC_NET);
        }
    }
}

#if CONFIG_KODEOS_SHOT_HTTP
/* ---- Screen uploads -------------------------------------------------------
 * One capture, one step at a time: AO_Display encodes the glass (within
 * KSHOT_DISPLAY_MS), kdrv_net PUTs it to the session's `shots` link plus the
 * order's token, and the asker is answered. Link and bearer come from the live
 * session. A channel that ends answers the upload at once; a PUT that is out
 * is marked stale.
 *
 * A stream is answered at once and runs on a timer that marks a frame due; a
 * due frame starts only when the last PUT is done, the worker is free and no
 * channel step waits, so a slow link lowers the rate. Its PUTs keep the
 * connection open; when the stream ends, a close goes to the worker first.
 */

/**
 * @brief Whether the session can take an upload now.
 *
 * Up, or the old session of a renewal that still serves: its token has an
 * hour left. The api must have named a valid `shots` link.
 */
static bool shot_ready(const Net * const me)
{
    bool const serving = ((uint8_t)CHAN_UP == me->cstate)
                         || (me->crenew && ((uint8_t)MQTT_LIVE == me->phase));
    bool       ready   = false;

    if ( (NULL != me->cw) && serving )
    {
        ready = kchan_shots_valid(me->cw->sess.shots) && ('\0' != me->cw->jwt[0]);
    }
    return ready;
}

/**
 * @brief Whether a channel step waits for the worker.
 *
 * A stop owed (the leave's, or a client that must go), or a step that found
 * the worker taken and polls for it (chan_retry()). A stream gives way: a
 * renewal that kept losing the worker to frames would fail as worker_busy.
 */
static bool shot_chan_waits(const Net * const me)
{
    bool const stepping = ((uint8_t)CP_IDLE != me->cp)
                          && ((uint8_t)CP_UP != me->cp)
                          && ((uint8_t)CP_WAIT != me->cp);

    return me->stop_pending || (stepping && (0U != me->cretry));
}

/**
 * @brief Write `shots` + the upload's token, from the live session.
 *
 * @return false when the two do not fit one URL.
 */
static bool shot_url_build(Net * const me)
{
    ChanWork * const w = me->cw;
    int const        n = snprintf(w->shot_url, sizeof(w->shot_url), "%s%s",
                                  w->sess.shots, w->shot_token);

    return (0 < n) && ((size_t)n < sizeof(w->shot_url));
}

/** @brief Forget the upload's token and link. */
static void shot_wipe(Net * const me)
{
    (void)memset(me->cw->shot_token, 0, sizeof(me->cw->shot_token));
    (void)memset(me->cw->shot_url, 0, sizeof(me->cw->shot_url));
}

/** @brief The word the log says for @p result. */
static const char *shot_word(uint8_t result)
{
    const char *word = "unknown";

    switch ( (kshot_result_t)result )
    {
        case KSHOT_R_OK: {
            word = "uploaded";
            break;
        }
        case KSHOT_R_REFUSED: {
            word = "refused";
            break;
        }
        case KSHOT_R_NO_ANSWER: {
            word = "no answer";
            break;
        }
        case KSHOT_R_BUSY: {
            word = "busy";
            break;
        }
        case KSHOT_R_SIZE: {
            word = "too big";
            break;
        }
        case KSHOT_R_CHANNEL: {
            word = "no channel";
            break;
        }
        case KSHOT_R_STREAM: {
            word = "streaming";
            break;
        }
        case KSHOT_R_STOPPED: {
            word = "stopped";
            break;
        }
        case KSHOT_R_NO_STREAM: {
            word = "no stream";
            break;
        }
        default: {
            break;
        }
    }
    return word;
}

/**
 * @brief Answer an upload order, and say how it ended in one NOTICE.
 *
 * The NOTICE names the outcome, the bytes and the status, or a stream's rate
 * and length: never the link, the token or the bearer.
 *
 * @param me  The AO.
 * @param who Who ordered it.
 * @param a   The answer; its `super` is not read.
 */
static void shot_reply(Net * const me, QActive *who, const NetShotRspEvt *a)
{
    NetShotRspEvt * const e = Q_NEW_X(NetShotRspEvt, 0U, NET_SHOT_RSP_SIG);

    (void)me;   /* the sender is only named when QP traces */
    if ( (uint8_t)KSHOT_R_STREAM == a->result )
    {
        KLOG_N(TAG, "shot: stream started, %u fps for %u s", (unsigned)a->fps,
               (unsigned)a->secs);
    }
    else if ( ((uint8_t)KSHOT_R_STOPPED == a->result)
             || ((uint8_t)KSHOT_R_NO_STREAM == a->result) )
    {
        /* A stop moves no bytes: the stream's own end line has the numbers. */
        KLOG_N(TAG, "shot: stop, %s", shot_word(a->result));
    }
    else
    {
        KLOG_N(TAG, "shot: %s, %u bytes, http %u", shot_word(a->result),
               (unsigned)a->bytes, (unsigned)a->http);
    }
    if ( NULL == e )
    {
        KLOG_E(TAG, "no event blocks for an upload's answer; whoever asked is "
                    "still waiting");
        return;
    }
    e->result = a->result;
    e->http   = a->http;
    e->bytes  = a->bytes;
    (void)snprintf(e->id, sizeof(e->id), "%s", a->id);
    e->seq    = a->seq;
    e->fps    = a->fps;
    e->secs   = a->secs;
    QACTIVE_POST(who, &e->super, &me->super);
}

/** @brief Answer @p who with @p result alone: nothing was sent. */
static void shot_refuse(Net * const me, QActive *who, kshot_result_t result)
{
    NetShotRspEvt a;

    (void)memset(&a, 0, sizeof(a));
    a.result = (uint8_t)result;
    shot_reply(me, who, &a);
}

/** @brief The capture in hand is over: answer it and forget its token and link. */
static void shot_end(Net * const me, const NetShotRspEvt *a)
{
    (void)QTimeEvt_disarm(&me->sdtmo);
    shot_reply(me, me->shwho, a);
    shot_wipe(me);
    me->shwho  = NULL;
    me->sbytes = 0U;
    me->sp     = (uint8_t)SP_IDLE;
    me->smode  = (uint8_t)KSHOT_IDLE;
}

/** @brief End the capture in hand with @p result alone. */
static void shot_end_with(Net * const me, kshot_result_t result)
{
    NetShotRspEvt a;

    (void)memset(&a, 0, sizeof(a));
    a.result = (uint8_t)result;
    shot_end(me, &a);
}

/**
 * @brief The stream is over: say how it went, and owe its connection a close.
 *
 * A PUT that is out is marked stale. The close goes out on the next tick, or
 * once the worker is free: a channel step that ended the stream takes it
 * first. The stream's numbers stay for `channel_info`.
 *
 * @param me  The AO.
 * @param why One word: time, expired, refused, channel, network, replaced,
 *            display, stopped.
 */
static void shot_stream_end(Net * const me, const char *why)
{
    uint32_t x10;

    (void)QTimeEvt_disarm(&me->sdtmo);
    if ( (uint8_t)SJ_LIVE == me->sjob )
    {
        me->sjob = (uint8_t)SJ_STALE;
    }
    me->sran_ms = khal_time_ms() - me->sfrom_ms;
    me->smode   = (uint8_t)KSHOT_IDLE;
    me->sp      = (uint8_t)SP_IDLE;
    me->sbytes  = 0U;
    me->sdue    = false;
    me->serrs   = 0U;
    me->sclose  = true;
    shot_wipe(me);
    x10 = (uint32_t)shot_fps_x10(me);
    KLOG_N(TAG, "shot: stream ended (%s): %u frames in %u s, %u.%u fps, "
                "%u skipped", why,
           (unsigned)me->ssent, (unsigned)(me->sran_ms / SHOT_MS_PER_S),
           (unsigned)(x10 / SHOT_TENTHS), (unsigned)(x10 % SHOT_TENTHS),
           (unsigned)me->sskip);
    chan_publish(me);
    (void)QTimeEvt_disarm(&me->stmo);
    QTimeEvt_armX(&me->stmo, SHOT_CLOSE_TICKS, 0U);
}

/**
 * @brief The channel's session ended: forget its bearer, end the upload in hand.
 *
 * A capture is answered now, wherever it is; a PUT that is out is marked
 * stale and its answer dropped. A stream ends with `channel`.
 */
static void shot_channel_gone(Net * const me)
{
    if ( NULL != me->cw )
    {
        (void)memset(me->cw->jwt, 0, sizeof(me->cw->jwt));
    }
    if ( (uint8_t)KSHOT_STREAM == me->smode )
    {
        shot_stream_end(me, "channel");
    }
    else if ( (uint8_t)SP_IDLE != me->sp )
    {
        if ( (uint8_t)SJ_LIVE == me->sjob )
        {
            me->sjob = (uint8_t)SJ_STALE;
        }
        shot_end_with(me, KSHOT_R_CHANNEL);
    }
    else
    {
        /* No upload in hand. */
    }
}

/** @brief Ask AO_Display for the glass, with KSHOT_DISPLAY_MS to answer. */
static void shot_ask_display(Net * const me)
{
    me->sp = (uint8_t)SP_DISPLAY;
    (void)QTimeEvt_disarm(&me->sdtmo);
    QTimeEvt_armX(&me->sdtmo, chan_ticks(KSHOT_DISPLAY_MS), 0U);
    display_shot_request(&me->super, (uint8_t)KSHOT_QUALITY);
}

/**
 * @brief Come back for the owed close in @p ticks, unless a stream runs: its
 *        own ticks kick often enough, and its timer is its pace.
 */
static void shot_close_later(Net * const me, uint32_t ticks)
{
    if ( (uint8_t)KSHOT_STREAM != me->smode )
    {
        (void)QTimeEvt_disarm(&me->stmo);
        QTimeEvt_armX(&me->stmo, ticks, 0U);
    }
}

/**
 * @brief Move the uploads on, if the worker lets them: called after every
 *        worker outcome, every tick of the stream and every order.
 *
 * In this order: the close a stream owes; a capture that waited for that
 * close; a stream's due frame. Neither the close nor a frame goes while a
 * channel step waits for the worker.
 */
static void shot_kick(Net * const me)
{
    kdrv_net_req_t req;

    if ( me->busy )
    {
        return;   /* the job in flight answers, and that comes back here */
    }
    if ( me->sclose )
    {
        if ( shot_chan_waits(me) )
        {
            /* The channel's step goes first; its outcome kicks again. */
            shot_close_later(me, chan_ticks(CHAN_RETRY_MS));
        }
        else
        {
            net_req_init(&req, KDRV_NET_JOB_HTTP_CLOSE);
            if ( net_submit(me, &req) )
            {
                me->sclose = false;
                me->sjob   = (uint8_t)SJ_CLOSE;
            }
            else
            {
                shot_close_later(me, SHOT_CLOSE_TICKS);
            }
        }
    }
    else if ( (uint8_t)SP_WAIT == me->sp )
    {
        shot_ask_display(me);
    }
    else if ( ((uint8_t)KSHOT_STREAM == me->smode) && me->sdue
             && ((uint8_t)SP_IDLE == me->sp) && (!shot_chan_waits(me)) )
    {
        if ( !shot_ready(me) )
        {
            shot_stream_end(me, "channel");
        }
        else
        {
            me->sdue = false;
            chan_fast_touch(me);   /* every frame holds the fast mode */
            shot_ask_display(me);
        }
    }
    else
    {
        /* Nothing is due, or a channel step goes first. */
    }
}

/** @brief A stream's frame did not go out: count it, and try the next one. */
static void shot_frame_skip(Net * const me)
{
    me->sp = (uint8_t)SP_IDLE;
    me->sskip++;
    shot_kick(me);
}

/**
 * @brief Start a stream: answer at once, publish the state, ask the first frame.
 *
 * A rate or a length of 0 takes its default and one past the most is cut: the
 * console checks both, this is the defence.
 */
static void shot_stream_start(Net * const me, const NetShotReqEvt *r)
{
    NetShotRspEvt a;
    uint8_t       fps  = r->fps;
    uint16_t      secs = r->secs;

    if ( 0U == fps )
    {
        fps = (uint8_t)KSHOT_FPS_DEFAULT;
    }
    if ( (uint8_t)KSHOT_FPS_MAX < fps )
    {
        fps = (uint8_t)KSHOT_FPS_MAX;
    }
    if ( 0U == secs )
    {
        secs = (uint16_t)KSHOT_SECS_DEFAULT;
    }
    if ( (uint16_t)KSHOT_SECS_MAX < secs )
    {
        secs = (uint16_t)KSHOT_SECS_MAX;
    }
    me->sfps     = fps;
    me->ssecs    = secs;
    me->sfrom_ms = khal_time_ms();
    me->sran_ms  = 0U;
    me->ssent    = 0U;
    me->sskip    = 0U;
    me->serrs    = 0U;
    me->ssaid    = 0U;
    me->sdue     = true;   /* the first frame at once */
    me->smode    = (uint8_t)KSHOT_STREAM;

    (void)memset(&a, 0, sizeof(a));
    a.result = (uint8_t)KSHOT_R_STREAM;
    a.fps    = fps;
    a.secs   = secs;
    shot_reply(me, r->who, &a);
    chan_publish(me);
    (void)QTimeEvt_disarm(&me->stmo);
    QTimeEvt_armX(&me->stmo, chan_ticks(SHOT_MS_PER_S / (uint32_t)fps), 0U);
    shot_kick(me);
}

/**
 * @brief The stream's tick: end it when its time is up, else a frame is due.
 *
 * Also the close's tick after an end, when no stream runs.
 */
static void shot_on_tick(Net * const me)
{
    if ( (uint8_t)KSHOT_STREAM == me->smode )
    {
        if ( (khal_time_ms() - me->sfrom_ms)
                >= ((uint32_t)me->ssecs * SHOT_MS_PER_S) )
        {
            shot_stream_end(me, "time");
        }
        else
        {
            /* Disarmed first: a tick already queued when an order replaced
             * the stream arrives with the new one's timer armed. */
            (void)QTimeEvt_disarm(&me->stmo);
            QTimeEvt_armX(&me->stmo,
                          chan_ticks(SHOT_MS_PER_S / (uint32_t)me->sfps), 0U);
            me->sdue = true;
        }
    }
    shot_kick(me);
}

/**
 * @brief The display did not answer in KSHOT_DISPLAY_MS.
 *
 * A capture ends as busy; a stream skips the frame and says so once. A shot
 * that lands later is given back.
 */
static void shot_on_wait(Net * const me)
{
    if ( (uint8_t)SP_DISPLAY == me->sp )
    {
        bool const stream = ((uint8_t)KSHOT_STREAM == me->smode);

        if ( (!stream) || ((me->ssaid & SHOT_SAID_DISPLAY) == 0U) )
        {
            KLOG_W(TAG, "shot: the display did not answer in %u ms",
                   (unsigned)KSHOT_DISPLAY_MS);
        }
        if ( stream )
        {
            me->ssaid |= (uint8_t)SHOT_SAID_DISPLAY;   /* once a stream */
            shot_frame_skip(me);
        }
        else
        {
            shot_end_with(me, KSHOT_R_BUSY);
        }
    }
}

/**
 * @brief An upload order: check it, then a capture or a stream.
 *
 * One upload at a time. A capture is answered before anything else is taken;
 * a stream is replaced by any valid order, which ends it (`replaced`). A
 * capture that follows a stream waits for that stream's close.
 */
static void shot_on_request(Net * const me, const NetShotReqEvt *r)
{
    bool const stream = (0U != r->fps) || (0U != r->secs);

    if ( (uint8_t)KSHOT_ONE == me->smode )
    {
        shot_refuse(me, r->who, KSHOT_R_BUSY);
    }
    else if ( !shot_ready(me) )
    {
        shot_refuse(me, r->who, KSHOT_R_CHANNEL);
    }
    else if ( !kshot_token_valid(r->token) )
    {
        shot_refuse(me, r->who, KSHOT_R_REFUSED);
    }
    else
    {
        if ( (uint8_t)KSHOT_STREAM == me->smode )
        {
            shot_stream_end(me, "replaced");
        }
        (void)snprintf(me->cw->shot_token, sizeof(me->cw->shot_token), "%s",
                       r->token);
        if ( !shot_url_build(me) )
        {
            KLOG_W(TAG, "shot: the link and the token do not fit %u bytes",
                   (unsigned)KDRV_NET_PUT_URL_MAX);
            shot_wipe(me);
            shot_refuse(me, r->who, KSHOT_R_CHANNEL);
        }
        else if ( stream )
        {
            shot_stream_start(me, r);
        }
        else
        {
            me->shwho = r->who;
            me->smode = (uint8_t)KSHOT_ONE;
            if ( me->sclose || ((uint8_t)SJ_NONE != me->sjob) )
            {
                me->sp = (uint8_t)SP_WAIT;   /* the last stream's close first */
                shot_kick(me);
            }
            else
            {
                shot_ask_display(me);
            }
        }
    }
}

/**
 * @brief A stop order: end the running stream (`stopped`), or say none runs.
 *
 * The stream ends the way every stream does, its connection's close owed
 * before anything else of the uploads goes out. A capture in hand is not a
 * stream and is left to finish.
 */
static void shot_on_stop(Net * const me, const NetShotStopEvt *r)
{
    if ( (uint8_t)KSHOT_STREAM == me->smode )
    {
        shot_stream_end(me, "stopped");
        shot_refuse(me, r->who, KSHOT_R_STOPPED);
    }
    else
    {
        shot_refuse(me, r->who, KSHOT_R_NO_STREAM);
    }
}

/**
 * @brief A stream's frame landed: PUT it on the kept connection, or skip it.
 *
 * A busy codec, a frame past KSHOT_JPEG_CAP or a taken worker skip this frame
 * only. A display that cannot encode at all ends the stream with `display`.
 * The shot is given back before the job is submitted.
 */
static void shot_frame_landed(Net * const me, const DisplayShotRspEvt *d)
{
    kdrv_net_req_t req;
    kode_status_t  rc;

    if ( (uint8_t)KODE_NO_RESOURCES == d->status )
    {
        shot_stream_end(me, "display");
        return;
    }
    if ( (uint8_t)KODE_OK != d->status )
    {
        shot_frame_skip(me);
        return;
    }
    if ( (!shot_ready(me)) || (!shot_url_build(me)) )
    {
        display_shot_release();
        shot_stream_end(me, "channel");
        return;
    }
    if ( me->busy || shot_chan_waits(me) )
    {
        display_shot_release();
        shot_frame_skip(me);
        return;
    }
    rc = kdrv_net_http_put_prepare(me->cw->shot_url, display_shot_buf(),
                                   (size_t)d->len, SHOT_CONTENT_TYPE,
                                   me->cw->jwt);
    display_shot_release();
    if ( KODE_OK != rc )
    {
        if ( (me->ssaid & SHOT_SAID_DRIVER) == 0U )
        {
            me->ssaid |= (uint8_t)SHOT_SAID_DRIVER;   /* once a stream */
            KLOG_W(TAG, "shot: the network driver refused a frame (%d)", (int)rc);
        }
        shot_frame_skip(me);
        return;
    }
    net_req_init(&req, KDRV_NET_JOB_HTTP_PUT);
    req.keep = true;   /* a stream: the next frame reuses the connection */
    if ( !net_submit(me, &req) )
    {
        shot_frame_skip(me);
        return;
    }
    me->sbytes = d->len;
    me->sjob   = (uint8_t)SJ_LIVE;
    me->sp     = (uint8_t)SP_PUT;
}

/**
 * @brief The shot landed: hand it to the worker and give it back at once.
 *
 * kdrv_net copies the JPEG into its own buffer, so the display's is released
 * before the job is submitted, whatever happens next.
 */
static void shot_on_display(Net * const me, const DisplayShotRspEvt *d)
{
    kdrv_net_req_t req;
    kode_status_t  rc;

    if ( (uint8_t)SP_DISPLAY != me->sp )
    {
        /* Nothing here asked for it: give back whatever was lent. */
        if ( (uint8_t)KODE_OK == d->status )
        {
            display_shot_release();
        }
        return;
    }
    (void)QTimeEvt_disarm(&me->sdtmo);
    if ( (uint8_t)KSHOT_STREAM == me->smode )
    {
        shot_frame_landed(me, d);
        return;
    }
    if ( (uint8_t)KODE_OK != d->status )
    {
        if ( (uint8_t)KODE_ERROR == d->status )
        {
            shot_end_with(me, KSHOT_R_SIZE);
        }
        else
        {
            shot_end_with(me, KSHOT_R_BUSY);
        }
        return;
    }
    if ( (!shot_ready(me)) || (!shot_url_build(me)) )
    {
        display_shot_release();
        shot_end_with(me, KSHOT_R_CHANNEL);
        return;
    }
    if ( me->busy )
    {
        display_shot_release();
        shot_end_with(me, KSHOT_R_BUSY);
        return;
    }
    rc = kdrv_net_http_put_prepare(me->cw->shot_url, display_shot_buf(),
                                   (size_t)d->len, SHOT_CONTENT_TYPE,
                                   me->cw->jwt);
    display_shot_release();
    if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "shot: the network driver refused the upload (%d)", (int)rc);
        shot_end_with(me, KSHOT_R_BUSY);
        return;
    }
    net_req_init(&req, KDRV_NET_JOB_HTTP_PUT);
    req.keep = false;   /* one capture: the connection closes with the job */
    if ( !net_submit(me, &req) )
    {
        shot_end_with(me, KSHOT_R_BUSY);
        return;
    }
    me->sbytes = d->len;
    me->sjob   = (uint8_t)SJ_LIVE;
    me->sp     = (uint8_t)SP_PUT;
}

/**
 * @brief A stream's PUT came back: count it, drop it, or end the stream.
 *
 * Any 2xx is a frame taken. 429 drops that frame (frames under 100 ms apart). 404
 * is a link that expired; 401, 413, 415 and any other refusal would refuse
 * every frame after it. No answer, no status line or a server error is a
 * network error, and KSHOT_STREAM_ERR_MAX of them in a row end the stream.
 */
static void shot_on_frame(Net * const me, uint8_t job_status)
{
    kdrv_net_http_result_t r;
    bool                   failed = false;

    me->sp     = (uint8_t)SP_IDLE;
    me->sbytes = 0U;
    if ( (uint8_t)KODE_OK != job_status )
    {
        kdrv_net_http_forget();
        failed = true;
    }
    else
    {
        kdrv_net_http_result(&r);
        kdrv_net_http_forget();
        if ( (SHOT_HTTP_OK_MIN <= r.status) && (SHOT_HTTP_OK_END > r.status) )
        {
            me->ssent++;
            me->serrs = 0U;
        }
        else if ( SHOT_HTTP_TOO_MANY == r.status )
        {
            me->sskip++;
            me->serrs = 0U;   /* an answer: the network works */
        }
        else if ( SHOT_HTTP_EXPIRED == r.status )
        {
            shot_stream_end(me, "expired");
        }
        else if ( (0U == r.status) || (SHOT_HTTP_SERVER_ERROR <= r.status) )
        {
            failed = true;
        }
        else
        {
            /* 401, 413, 415, a redirect (none is followed), a 409. */
            shot_stream_end(me, "refused");
        }
    }
    if ( failed )
    {
        me->serrs++;
        if ( (uint8_t)KSHOT_STREAM_ERR_MAX <= me->serrs )
        {
            shot_stream_end(me, "network");
        }
    }
}

/**
 * @brief The capture's PUT came back: read the answer, wipe it, answer the order.
 *
 * Any 2xx took it, with the capture's `id` and `seq` when the answer has
 * them; any other status is a refusal (a redirect included: none is
 * followed).
 */
static void shot_on_http(Net * const me, uint8_t job_status)
{
    ChanWork * const       w = me->cw;
    NetShotRspEvt          a;
    kdrv_net_http_result_t r;
    uint32_t               n;

    (void)memset(&a, 0, sizeof(a));
    a.bytes = me->sbytes;
    if ( (uint8_t)KODE_OK != job_status )
    {
        kdrv_net_http_forget();
        a.bytes  = 0U;
        a.result = (uint8_t)KSHOT_R_NO_ANSWER;
        shot_end(me, &a);
        return;
    }
    kdrv_net_http_result(&r);
    n = kdrv_net_http_body(0U, (uint8_t *)w->answer,
                           (uint32_t)sizeof(w->answer) - 1U);
    kdrv_net_http_forget();
    w->answer[n] = '\0';
    a.http = r.status;
    if ( (SHOT_HTTP_OK_MIN <= r.status) && (SHOT_HTTP_OK_END > r.status) )
    {
        a.result = (uint8_t)KSHOT_R_OK;
        (void)kchan_json_str(w->answer, (size_t)n, "id", a.id, sizeof(a.id));
        (void)kchan_json_u32(w->answer, (size_t)n, "seq", &a.seq);
    }
    else if ( 0U == r.status )
    {
        a.bytes  = 0U;   /* no status line: as good as no answer */
        a.result = (uint8_t)KSHOT_R_NO_ANSWER;
    }
    else
    {
        a.result = (uint8_t)KSHOT_R_REFUSED;
    }
    (void)memset(w->answer, 0, sizeof(w->answer));
    shot_end(me, &a);
}

/**
 * @brief The worker finished a job of the uploads (a PUT or a close).
 *
 * A live PUT is read by the capture or the stream it belongs to; a stale one
 * - its upload already ended - is only wiped; a close says nothing.
 */
static void shot_on_job(Net * const me, uint8_t job_status)
{
    uint8_t const job = me->sjob;

    me->sjob = (uint8_t)SJ_NONE;
    if ( (uint8_t)SJ_LIVE == job )
    {
        if ( (uint8_t)KSHOT_STREAM == me->smode )
        {
            shot_on_frame(me, job_status);
        }
        else
        {
            shot_on_http(me, job_status);
        }
    }
    else if ( (uint8_t)SJ_STALE == job )
    {
        kdrv_net_http_forget();
        KLOG_I(TAG, "shot: the answer to an upload that had ended was dropped");
    }
    else
    {
        /* The close: nothing to read. */
    }
}

/**
 * @brief The PUT with the worker outlived its guard: end its upload now.
 *
 * The worker may still be writing the driver's tables, so nothing is read:
 * the PUT is marked stale. A capture is answered KSHOT_R_NO_ANSWER; a stream's
 * frame counts as a network error.
 *
 * @param me The AO.
 */
static void shot_put_expired(Net * const me)
{
    if ( (uint8_t)SJ_LIVE == me->sjob )
    {
        me->sjob = (uint8_t)SJ_STALE;
        if ( (uint8_t)KSHOT_STREAM == me->smode )
        {
            me->sp     = (uint8_t)SP_IDLE;
            me->sbytes = 0U;
            me->serrs++;
            if ( (uint8_t)KSHOT_STREAM_ERR_MAX <= me->serrs )
            {
                shot_stream_end(me, "network");
            }
        }
        else
        {
            shot_end_with(me, KSHOT_R_NO_ANSWER);
        }
    }
}
#endif /* CONFIG_KODEOS_SHOT_HTTP */

#endif /* CONFIG_KODEOS_CHANNEL */

/* ---- Leaving for a jump --------------------------------------------------
 * AO_Boot asks once before it restarts the board. The channel says "offline"
 * and stops its client, then BOOT_NET_LEFT_SIG lets the jump go on. `leaving`
 * stays set for the rest of the boot, so no join starts before the reset.
 */

/**
 * @brief Pay the answer a leave owes, once the stop it waits for is done.
 *
 * Called when a leave is asked and at the end of every worker outcome, so
 * the answer goes out exactly once, and only when no client is on its way
 * down (the stop in flight, or one owed to the worker).
 *
 * @param me The AO.
 */
static void net_leave_settle(Net * const me)
{
    bool waiting = false;

#if CONFIG_KODEOS_CHANNEL
    waiting = me->stop_pending || ((uint8_t)MQTT_STOPPING == me->phase);
#endif
    if ( me->leave_owed && (!waiting) )
    {
        me->leave_owed = false;
        boot_net_left();
    }
}

/**
 * @brief Leave the channel: end the join, stop the client, answer AO_Boot.
 *
 * A second ask while the first one's answer is owed is logged and dropped;
 * one asked after the answer is answered again at once.
 *
 * @param me The AO.
 */
static void net_leave(Net * const me)
{
    if ( me->leave_owed )
    {
        KLOG_W(TAG, "a leave is already under way; this ask is dropped");
        return;
    }
    me->leaving    = true;
    me->leave_owed = true;
#if CONFIG_KODEOS_CHANNEL
    if ( chan_active(me) )
    {
        chan_leave(me, "leaving");   /* says offline first, then ends the join */
        chan_stop_client(me);
    }
#endif
    net_leave_settle(me);
}

#if CONFIG_KODE_LOWPOWER
/* ---- The board's rest -----------------------------------------------------
 * ASLEEP keeps the sessions and starts nothing; OFF closes them.
 */

/** @brief The busy reasons of a round, each short enough for LowPowerReadyEvt.why. */
#define NET_LP_WHY_JOB       "net job"        /**< A worker job is out.              */
#define NET_LP_WHY_TRANSFER  "net transfer"   /**< A download is open.               */
#define NET_LP_WHY_SHOT      "screen upload"  /**< A capture or a stream is under way. */
#define NET_LP_WHY_LEAVING   "net leaving"    /**< A jump asked for the leave.       */
#define NET_LP_WHY_JOINING   "net joining"    /**< The channel is between steps of a join. */
#define NET_LP_WHY_WAKE      "net wake"       /**< The wake time had no event to go in. */
#define NET_LP_WHY_DWELL     "check dwell"    /**< The scheduler's check is letting what is held flow in. */

/**
 * @brief What stops the sessions from being put to rest now.
 *
 * @param me    The AO.
 * @param dwell Whether the check's dwell is in the way: not for a round to
 *              OFF, which closes the sessions the dwell would listen to.
 * @return A busy reason, or NULL when nothing is in the way.
 */
static const char *net_lp_busy_why(Net const * const me, bool dwell)
{
    const char *why = NULL;

    if ( me->leave_owed )
    {
        why = NET_LP_WHY_LEAVING;
    }
    else if ( me->busy )
    {
        why = NET_LP_WHY_JOB;
    }
    else if ( me->sbusy || me->sopen || me->popen )
    {
        why = NET_LP_WHY_TRANSFER;
    }
#if CONFIG_KODEOS_CHANNEL && CONFIG_KODEOS_SHOT_HTTP
    else if ( (uint8_t)KSHOT_IDLE != me->smode )
    {
        why = NET_LP_WHY_SHOT;
    }
#endif
    else if ( dwell && me->chk_dwell )
    {
        why = NET_LP_WHY_DWELL;
    }
    else
    {
        /* Nothing is in the way. */
    }
    return why;
}

#if CONFIG_KODEOS_CHANNEL
/**
 * @brief Whether the channel is between steps of a join or a renewal.
 *
 * Its guards and polls are QTimeEvts, which would freeze in the rest. It
 * takes seconds, so the board sleeps at the next idle time instead.
 *
 * @param me The AO.
 * @return true while a step of the join awaits its answer.
 */
static bool net_lp_joining(Net const * const me)
{
    return chan_active(me) && (!chan_due_pending(me));
}
#endif

/**
 * @brief Whether a client is stopping or is owed a stop.
 * @param me The AO.
 * @return true while the sessions are not all closed.
 */
static bool net_lp_closing(Net const * const me)
{
    bool closing = ((uint8_t)MQTT_STOPPING == me->phase);

#if CONFIG_KODEOS_CHANNEL
    closing = closing || me->stop_pending;
#endif
    return closing;
}

/**
 * @brief Whether the scheduler's `mqtt` check has fallen due.
 *
 * @param me The AO.
 * @return true when a time was given, the clock is set and the time has come.
 */
static bool net_chk_due(Net const * const me)
{
    bool due = false;

    if ( (0 <= me->chk_s) && khal_time_wall_known() )
    {
        uint64_t const wall_s = khal_time_wall_us() / NET_CHK_US_PER_S;

        due = ((uint64_t)me->chk_s <= wall_s);
    }
    return due;
}

/**
 * @brief The check was made: forget its time and tell AO_Scheduler, which
 *        answers with the next.
 *
 * @param me The AO.
 */
static void net_chk_pass(Net * const me)
{
    int64_t const passed = me->chk_s;

    (void)QTimeEvt_disarm(&me->chk_tmo);
    me->chk_dwell = false;
    me->chk_s     = NET_CHK_NONE;
    if ( 0 <= passed )
    {
        sched_check_done(passed);
    }
}

/**
 * @brief A round finds the board awake: a check that is due is made at once.
 *
 * The sessions were served while the time came, and a check left due would
 * wake the board again at once. A running dwell refuses the round
 * (net_lp_busy_why()), so none is cut short here.
 *
 * @param me The AO.
 */
static void net_chk_serve(Net * const me)
{
    if ( (!me->chk_dwell) && net_chk_due(me) )
    {
        net_chk_pass(me);
    }
}

/**
 * @brief Whether something is there to listen to: the C5 link is up, Wi-Fi has
 *        an address, and the Kode channel is in use or a client of the user's
 *        is connected.
 *
 * @param me The AO.
 * @return true when a radio wake for the scheduler's check has anything to do.
 */
static bool net_chk_listens(Net const * const me)
{
    bool sessions = ((uint8_t)MQTT_LIVE == me->phase);

#if CONFIG_KODEOS_CHANNEL
    sessions = sessions || chan_active(me);
#endif
    return me->link_up && me->online && sessions;
}

/**
 * @brief A wake begins (a radio wake, or a whole one from ASLEEP): a check
 *        that is due is made after its dwell when something listens, and at
 *        once when nothing does. A dwell already on is kept.
 *
 * The dwell is the time for what the C5 or the broker holds to flow in.
 *
 * @param me The AO.
 */
static void net_chk_begin(Net * const me)
{
    if ( (!me->chk_dwell) && net_chk_due(me) )
    {
        if ( net_chk_listens(me) )
        {
            me->chk_dwell = true;
            QTimeEvt_armX(&me->chk_tmo, KQP_TICKS_PER_MS(KODE_NET_CHECK_DWELL_MS), 0U);
        }
        else
        {
            net_chk_pass(me);
        }
    }
}

/**
 * @brief Tell AO_LowPower when the board must wake for this AO: the earlier of
 *        the channel's deadline and the scheduler's check.
 *
 * The check counts only when something listens (net_chk_listens()); with
 * nothing to listen to there is nothing for a radio wake to do.
 *
 * @param me The AO.
 * @return false when no event was left to carry the note; true otherwise, also
 *         when there is nothing to note.
 */
static bool net_lp_wake_note(Net const * const me)
{
    bool have = false;
    uint64_t due = 0U;

#if CONFIG_KODEOS_CHANNEL
    if ( chan_due_pending(me) )
    {
        have = true;
        due  = me->cdue_us;
    }
#endif
    if ( net_chk_listens(me) && (0 <= me->chk_s) )
    {
        uint64_t const at = khal_time_at_wall_s(me->chk_s);

        if ( (!have) || (at < due) )
        {
            have = true;
            due  = at;
        }
    }
    return (!have) || lowpower_wake_at(me->lp_who, due);
}

/**
 * @brief Whether AO_Net has nothing left to do in a radio wake.
 *
 * Nothing is in the way of resting, and the channel is either not in use or
 * waiting for a time further off than CHAN_LP_SETTLE_MS.
 *
 * @param me The AO.
 * @return true when the board can go back to sleep for this AO.
 */
static bool net_lp_radio_settled(Net const * const me)
{
    bool settled = (net_lp_busy_why(me, true) == NULL);

#if CONFIG_KODEOS_CHANNEL
    if ( settled && chan_active(me) )
    {
        settled = chan_due_pending(me)
                  && (chan_due_left_ms(me) > CHAN_LP_SETTLE_MS);
    }
#endif
    return settled;
}

/**
 * @brief Tell AO_LowPower the work of a radio wake is finished, once, when it is.
 *
 * Called when the wake begins and at every end of a step of the channel.
 *
 * @param me The AO.
 */
static void net_lp_radio_check(Net * const me)
{
    if ( me->lp_radio && net_lp_radio_settled(me) )
    {
        me->lp_radio = false;
        lowpower_radio_done(me->lp_who);
    }
}

/**
 * @brief Close the sessions for an OFF round: the channel first, then any
 *        client of the user's, as soon as the worker allows.
 *
 * @param me The AO.
 */
static void net_lp_close(Net * const me)
{
#if CONFIG_KODEOS_CHANNEL
    if ( chan_active(me) )
    {
        chan_leave(me, "off");   /* says offline first, then ends the join */
    }
    chan_stop_client(me);
#else
    if ( (uint8_t)MQTT_IDLE != me->phase )
    {
        kdrv_net_req_t req;

        (void)QTimeEvt_disarm(&me->tmo);
        net_req_init(&req, KDRV_NET_JOB_MQTT_STOP);
        if ( net_submit(me, &req) )
        {
            me->phase = (uint8_t)MQTT_STOPPING;
        }
    }
#endif
}

/**
 * @brief Answer the OFF round once every session is closed, and tell AO_Radio.
 *
 * Called when the round begins and at the end of every worker outcome, so the
 * answer goes out exactly once and only after the last stop.
 *
 * @param me The AO.
 */
static void net_lp_settle(Net * const me)
{
    if ( me->lp_owed && (!net_lp_closing(me)) )
    {
        me->lp_owed = false;
        lowpower_ready(me->lp_who, me->lp_round, false, NULL);
        radio_net_left(me->lp_round);
    }
}

/**
 * @brief Bring the channel back after an OFF round that did not happen.
 *
 * Idempotent: it acts once, when every session is closed, the board is awake
 * and the channel is wanted. A client of the user's stays gone.
 *
 * @param me The AO.
 */
static void net_lp_rejoin(Net * const me)
{
    if ( me->lp_rejoin && (!me->lp_rest) && (!net_lp_closing(me)) )
    {
        me->lp_rejoin = false;
#if CONFIG_KODEOS_CHANNEL
        if ( me->online && me->chan_on && (!me->leaving) && (!chan_active(me))
            && ((uint8_t)MQTT_IDLE == me->phase) )
        {
            chan_enter(me, CHAN_LP_SETTLE_MS);
        }
#endif
    }
}

/**
 * @brief LOWPOWER_ROUND_SIG: keep the sessions and start nothing, or close them.
 *
 * A round for OFF is not refused for the check's dwell: it closes the sessions
 * the dwell listens to. The dwell runs on and ends as it would, and the check
 * is told made then; if the board goes off first, the boot forgets it.
 *
 * @param me The AO.
 * @param r  The round.
 */
static void net_lp_round(Net * const me, const LowPowerRoundEvt * const r)
{
    bool const dwell = ((uint8_t)KODE_LP_OFF != r->target);
    const char * const why = net_lp_busy_why(me, dwell);

    me->lp_round = r->round;
    me->lp_owed  = false;
    me->lp_radio = false;   /* a round ends the radio wake's work */
    if ( (NULL == why) && ((uint8_t)KODE_LP_OFF != r->target) )
    {
        net_chk_serve(me);
    }
    if ( NULL != why )
    {
        lowpower_ready(me->lp_who, r->round, true, why);
    }
    else if ( (uint8_t)KODE_LP_OFF == r->target )
    {
        me->lp_rest   = true;
        me->lp_rejoin = true;
        me->lp_owed   = true;
        net_lp_close(me);
        net_lp_settle(me);
    }
#if CONFIG_KODEOS_CHANNEL
    else if ( net_lp_joining(me) )
    {
        lowpower_ready(me->lp_who, r->round, true, NET_LP_WHY_JOINING);
    }
#endif
    else if ( !net_lp_wake_note(me) )
    {
        lowpower_ready(me->lp_who, r->round, true, NET_LP_WHY_WAKE);
    }
    else
    {
        me->lp_rest = true;
#if CONFIG_KODEOS_CHANNEL
        if ( chan_due_pending(me) )
        {
            /* The deadline is with AO_LowPower now; chan_lp_resume() arms
             * the timer again on AWAKE. */
            (void)QTimeEvt_disarm(&me->ctmo);
        }
#endif
        lowpower_ready(me->lp_who, r->round, false, NULL);
    }
}

/**
 * @brief LOWPOWER_STATE_SIG: on AWAKE, or on a radio wake, start working again.
 *
 * It may repeat, and an answer owed to a round that is over is not given. The
 * long timer counts its real time again and a start that waited begins. A
 * radio wake is the same work with the glass asleep (net_lp_radio_check()).
 *
 * @param me The AO.
 * @param s  The change.
 */
static void net_lp_state(Net * const me, const LowPowerStateEvt * const s)
{
    bool const radio = ((uint8_t)KODE_LP_RADIO == s->state);

    if ( ((uint8_t)KODE_LP_AWAKE == s->state) || radio )
    {
        me->lp_rest  = false;
        me->lp_owed  = false;
        me->lp_radio = radio;
        if ( radio || ((uint8_t)KODE_LP_AWAKE != s->prev) )
        {
            net_chk_begin(me);
        }
        else
        {
            net_chk_serve(me);
        }
#if CONFIG_KODEOS_CHANNEL
        chan_lp_resume(me);
#endif
        net_lp_rejoin(me);
        net_lp_radio_check(me);
    }
}

/**
 * @brief NET_CHECK_AT_SIG: the scheduler gives the next time of its `mqtt` check.
 *
 * It replaces the last time, and a dwell that was for that one. Asleep, the
 * board is told at once: nothing else would wake it for the new time.
 *
 * @param me The AO.
 * @param c  The note.
 */
static void net_lp_check_at(Net * const me, const NetCheckAtEvt * const c)
{
    (void)QTimeEvt_disarm(&me->chk_tmo);
    me->chk_dwell = false;
    me->chk_s     = c->due_s;
    if ( me->lp_rest )
    {
        if ( !net_lp_wake_note(me) )
        {
            KLOG_W(TAG, "the wake time of the check was not given; the check "
                        "waits for the next wake");
        }
    }
    else
    {
        net_lp_radio_check(me);
    }
}

/**
 * @brief NET_CHECK_TMO_SIG: the dwell of the check's radio wake has run out.
 *
 * @param me The AO.
 */
static void net_lp_check_dwelled(Net * const me)
{
    if ( me->chk_dwell )
    {
        net_chk_pass(me);
        net_lp_radio_check(me);
    }
}

/**
 * @brief RADIO_STATE_SIG: remember whether the C5 link is up.
 *
 * @param me The AO.
 * @param s  The link's state.
 */
static void net_lp_link(Net * const me, const RadioStateEvt * const s)
{
    me->link_up = ((uint8_t)KRADIO_UP == s->link);
}
#endif /* CONFIG_KODE_LOWPOWER */

/**
 * @brief One message from the broker, taken from the driver's ring.
 *
 * A message tagged as the channel's goes to the channel's queue, or is dropped
 * when the channel was told to go; it is never republished. The tag, not the
 * topic, decides: the session is wiped when the channel ends, the ring is not.
 * Everything else is republished as MQTT_MSG_SIG from this AO's thread.
 */
static void net_on_msg(Net * const me, const kdrv_mqtt_msg_t * const m)
{
    bool handled = false;

#if CONFIG_KODEOS_CHANNEL
    if ( m->channel )
    {
        /* The channel's client: a command, never a message for the general
         * stream. It waits in the queue while the channel is not up. */
        if ( chan_told_to_go(me) || (NULL == me->cw) )
        {
            KLOG_N(TAG, "channel: halted; 1 command(s) that had not run are dropped");
        }
        else
        {
            chan_on_cmd(me, m);
        }
        handled = true;
    }
#endif
    if ( !handled )
    {
        MqttMsgEvt *out = Q_NEW_X(MqttMsgEvt, 0U, MQTT_MSG_SIG);
        if ( NULL != out )
        {
            /* The driver bounds a message by the same caps (asserted at the
             * top of this file); the copy still stops at the event's own. */
            uint16_t n = m->len;

            if ( (uint16_t)sizeof(out->data) < n )
            {
                n = (uint16_t)sizeof(out->data);
            }
            (void)snprintf(out->topic, sizeof(out->topic), "%s", m->topic);
            (void)memset(out->data, 0, sizeof(out->data));
            (void)memcpy(out->data, m->data, n);
            out->len       = n;
            out->topic_cut = m->topic_cut;
            out->data_cut  = m->data_cut;
            out->retained  = m->retained;
            QACTIVE_PUBLISH(&out->super, &me->super);
        }
        KLOG_N(TAG, "mqtt rx topic=%s len=%u%s", m->topic,
               (unsigned)m->len, m->data_cut ? " (cut)" : "");
    }
}

/**
 * @brief NET_MQTT_RX_SIG: empty the driver's receive ring.
 *
 * At most one ring's worth a step, one message at a time; a ring that still
 * holds messages after that posts a notice to itself.
 */
static void net_rx_drain(Net * const me)
{
    uint32_t taken = 0U;
    bool     more  = (NULL != me->jobs);

    while ( more && ((uint32_t)KDRV_MQTT_RX_RING > taken) )
    {
        /* Taken into the AO's scratch in PSRAM and handled where it lies:
         * the message is copied once, and not onto this stack. */
        more = kdrv_net_mqtt_rx_take(&me->jobs->rx);
        if ( more )
        {
            net_on_msg(me, &me->jobs->rx);
            taken++;
        }
    }
    if ( more )
    {
        /* A step's worth is done and the ring may hold more: come back. */
        static const QEvt again = QEVT_INITIALIZER(NET_MQTT_RX_SIG);

        if ( !QACTIVE_POST_X(AO_Net, &again, 1U, NULL) )
        {
            /* No room: the driver is told a notice is owed, so the next
             * message sends one. */
            kdrv_net_mqtt_rx_owe();
        }
    }
}

#if CONFIG_KODEOS_CHANNEL
/**
 * @brief The channel was told to go: its commands that have not run are dropped.
 *
 * The channel was switched off, or the board leaves for an app. The queue is
 * emptied, and so are the channel's entries in the driver's ring; a bench
 * client's messages keep their path. One NOTICE says how many were dropped.
 *
 * @param me The AO.
 */
static void chan_queue_drop(Net * const me)
{
    ChanWork * const cw = me->cw;

    if ( NULL != cw )
    {
        uint32_t dropped = (uint32_t)cw->cq_len;
        uint32_t taken   = 0U;
        bool     more    = (NULL != me->jobs);

        chan_q_clear(cw);
        net_guard_stop(&me->lguard);
        while ( more && ((uint32_t)KDRV_MQTT_RX_RING > taken) )
        {
            more = kdrv_net_mqtt_rx_take(&me->jobs->rx);
            if ( more )
            {
                if ( me->jobs->rx.channel )
                {
                    dropped++;
                }
                else
                {
                    net_on_msg(me, &me->jobs->rx);
                }
                taken++;
            }
        }
        if ( 0U != dropped )
        {
            KLOG_N(TAG, "channel: halted; %lu command(s) that had not run are dropped",
                   (unsigned long)dropped);
        }
    }
}
#endif /* CONFIG_KODEOS_CHANNEL */

/* ---- The AO -------------------------------------------------------------- */

void Net_ctor(void)
{
    Net * const me = &l_net;
    QActive_ctor(&me->super, Q_STATE_CAST(&Net_initial));
    QTimeEvt_ctorX(&me->tmo, &me->super, NET_TMO_SIG, 0U);
    QTimeEvt_ctorX(&me->wguard.te, &me->super, NET_JOB_GUARD_SIG, 0U);
    QTimeEvt_ctorX(&me->sguard.te, &me->super, NET_STREAM_GUARD_SIG, 0U);
    me->phase = (uint8_t)MQTT_IDLE;
    me->err   = (uint8_t)KDRV_MQTT_ERR_NONE;
    me->sntp_asked = false;
    me->busy  = false;
    me->wreplied      = false;
    me->wseq          = 0U;
    me->wguard.stale  = 0U;
    me->wguard.live   = false;
    me->sbusy         = false;
    me->sreplied      = false;
    me->skind         = (uint8_t)NETS_NONE;
    me->sseq          = 0U;
    me->sguard.stale  = 0U;
    me->sguard.live   = false;
    me->sheld         = false;
    me->sclose_owed   = false;
    me->swho          = NULL;
    me->sidx          = 0U;
    me->sreads        = 0U;
    me->popen         = false;
    me->pwho          = NULL;
    me->leaving    = false;
    me->leave_owed = false;
#if CONFIG_KODE_LOWPOWER
    me->lp_who     = KODE_LOWPOWER_WHO_NONE;
    me->lp_rest    = false;
    me->lp_radio   = false;
    me->lp_owed    = false;
    me->lp_rejoin  = false;
    me->lp_round   = 0U;
    me->chk_s      = NET_CHK_NONE;
    me->chk_dwell  = false;
    me->link_up    = false;
    QTimeEvt_ctorX(&me->chk_tmo, &me->super, NET_CHECK_TMO_SIG, 0U);
#endif
#if CONFIG_KODEOS_CHANNEL
    QTimeEvt_ctorX(&me->ctmo, &me->super, NET_CHAN_TMO_SIG, 0U);
    QTimeEvt_ctorX(&me->otmo, &me->super, NET_CHAN_OUT_SIG, 0U);
    QTimeEvt_ctorX(&me->ftmo, &me->super, NET_CHAN_FAST_SIG, 0U);
    QTimeEvt_ctorX(&me->wtmo, &me->super, NET_CHAN_WATCH_SIG, 0U);
    QTimeEvt_ctorX(&me->lguard.te, &me->super, NET_CHAN_LINE_SIG, 0U);
    me->lguard.stale = 0U;
    me->lguard.live  = false;
    me->cw           = NULL;
    me->cfast        = false;
    me->chan_on      = true;
    me->cstate       = (uint8_t)CHAN_NO_NET;
    chan_phase_set(me, (uint8_t)CP_IDLE);
    me->attempt      = 0U;
    me->cretry       = 0U;
    me->id_out       = (uint8_t)IDO_NONE;
    me->cjob         = (uint8_t)CJ_NONE;
    me->cert_ok      = false;
    me->crenew       = false;
    me->cwait_valid  = false;
    me->cdue_us      = 0U;
    me->cup_set      = false;
    me->stop_pending = false;
    me->cwhy[0]      = '\0';
    me->cnote[0]     = '\0';
#if CONFIG_KODEOS_SHOT_HTTP
    QTimeEvt_ctorX(&me->sdtmo, &me->super, NET_SHOT_WAIT_SIG, 0U);
    QTimeEvt_ctorX(&me->stmo, &me->super, NET_SHOT_TICK_SIG, 0U);
    me->shwho        = NULL;
    me->sbytes       = 0U;
    me->sfrom_ms     = 0U;
    me->sran_ms      = 0U;
    me->ssent        = 0U;
    me->sskip        = 0U;
    me->ssecs        = 0U;
    me->sfps         = 0U;
    me->smode        = (uint8_t)KSHOT_IDLE;
    me->sp           = (uint8_t)SP_IDLE;
    me->sjob         = (uint8_t)SJ_NONE;
    me->serrs        = 0U;
    me->ssaid        = 0U;
    me->sdue         = false;
    me->sclose       = false;
#endif
#endif
}

AO_STORAGE(net, KQP_NET_QUEUE_LEN, KQP_AO_STACK_MEDIUM);   /* room for an identity answer under inbound MQTT */

#if CONFIG_KODEOS_AUTOTEST
/* ---- The self-test rows: registered by kaos_net_register() ---------------- */

/**
 * @brief One echo to the gateway, judged by AO_Net.
 *
 * The gateway, not a public host: the test proves our route, ARP and ICMP.
 */
static bool net_row_net(void)
{
    net_test_request();
    return false;   /* answered later, with TEST_RESULT_SIG */
}
#endif

void kaos_net_register(void)
{
#if CONFIG_KODEOS_AUTOTEST
    static const krow_t NET_ROWS[] = {
        { "net", AUTOTEST_ID_NET, net_row_net, KROW_ASYNC, AUTOTEST_G_NONE, KODE_TEST_ROW_BUDGET_MS,
          NULL, 0U, "Network",
          "Network not answering",
          AUTOTEST_ORDER_NET },
    };

    kaos_require(krows_add(NET_ROWS, sizeof(NET_ROWS) / sizeof(NET_ROWS[0])),
                 "net rows");
#endif
#if CONFIG_KODEOS_CONSOLE
    kaos_require(kverbs_add(&net_verbs), "net verbs");
#endif
}

void kaos_net_start(void)
{
    Net_ctor();
    kaos_net_register();
    AO_START(AO_Net, net, KODE_PRIO_NET);
#if CONFIG_KODEOS_CONSOLE && CONFIG_KODEOS_CHANNEL
    /* The settings this AO answers, registered once it has the queue their
     * requests are posted to: an app that does not link it has no such keys. */
    static const char *const CHAN_KEEP[] = KSET_CHAN_KEEP_WORDS;   /* the ages `chan_keep` offers */
    static const kode_config_t NET_CONFIG[] = {
        { .name = KSET_KEY_CHAN, .type = KODE_TYPE_BOOL },
        { .name = KSET_KEY_CHAN_KEEP, .type = KODE_TYPE_ENUM, .enums = CHAN_KEEP,
          .n_enums = (uint8_t)(sizeof(CHAN_KEEP) / sizeof(CHAN_KEEP[0])) },
    };

    kaos_require(KODE_CONFIG_ADD(AO_Net, KQP_NET_CMD_MARGIN, NET_CONFIG), "net settings");
#endif
}

/* ---- The stream loan ------------------------------------------------------ */

void net_stream_open_request(const char *url, QActive *who)
{
    NetStreamOpenEvt *e = Q_NEW_X(NetStreamOpenEvt, 0U, NET_STREAM_OPEN_REQ_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; stream open dropped");
        return;
    }
    (void)snprintf(e->url, sizeof(e->url), "%s", url);
    e->who = who;
    QACTIVE_POST(AO_Net, &e->super, NULL);
}

void net_stream_read_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(NET_STREAM_READ_REQ_SIG);
    QACTIVE_POST(AO_Net, &evt, NULL);
}

void net_stream_return(uint8_t idx)
{
    StorageBufEvt *e = Q_NEW_X(StorageBufEvt, 0U, NET_STREAM_NEXT_SIG);
    if ( NULL == e )
    {
        return;   /* the buffer stays lent; the transfer stalls and times out */
    }
    e->idx = idx;
    QACTIVE_POST(AO_Net, &e->super, NULL);
}

void net_stream_close_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(NET_STREAM_CLOSE_REQ_SIG);
    QACTIVE_POST(AO_Net, &evt, NULL);
}


/** @brief AO_Net's instance, for the stream calls made outside its state machine. */
static Net *stream_owner(void) { return &l_net; }

const uint8_t *net_stream_buf(uint8_t idx)
{
    Net * const me = stream_owner();
    return ((uint8_t)NET_STREAM_BUFS > idx) ? me->sbuf[idx] : NULL;
}

/** @brief Allocate the pair once, the first time a transfer needs them. */
static bool stream_bufs_ready(Net * const me)
{
    for ( uint8_t i = 0U; (uint8_t)NET_STREAM_BUFS > i; i++ )
    {
        if ( NULL == me->sbuf[i] )
        {
            me->sbuf[i] = khal_alloc_psram(64U, NET_STREAM_CHUNK);
            if ( NULL == me->sbuf[i] )
            {
                KLOG_E(TAG, "no PSRAM for the stream buffers");
                return false;
            }
        }
    }
    return true;
}

/**
 * @brief Tell whoever waits for a GET or a POST how it came out.
 *
 * The channel's own POST is read by the channel and shown to nobody - nor read
 * at all when the join it belonged to has ended. Any other GET is shown to
 * whoever asked.
 *
 * @param me The AO.
 * @param rc How it went.
 */
static void net_http_outcome(Net * const me, kode_status_t rc)
{
#if CONFIG_KODEOS_CHANNEL
    if ( (uint8_t)CJ_NONE != me->cjob )
    {
        bool const live = ((uint8_t)CJ_LIVE == me->cjob);

        me->cjob = (uint8_t)CJ_NONE;
        if ( live )
        {
            chan_on_http(me, (uint8_t)rc);
        }
        return;
    }
#endif
    if ( KODE_OK == rc )
    {
        net_publish_http(me);
    }
    else
    {
        net_publish_http_done(me, rc);
    }
}

/**
 * @brief A kdrv_net job outlived its guard: answer whoever waits for it.
 *
 * The answers are the failures each request already has, with KODE_TIMEOUT
 * where the reply carries a status. A START or a STOP waits for nobody - the
 * connect deadline and the phase are theirs - so the late done event does the
 * work.
 *
 * @param me The AO.
 */
static void net_expired_net(Net * const me)
{
    switch ( me->jobs->req.type )
    {
        case KDRV_NET_JOB_PING: {
            if ( me->testing )
            {
                me->testing = false;
                net_publish_test(me, false);
            }
            else
            {
                net_publish_ping_end(me, false);
            }
            me->wreplied = true;
            break;
        }
        case KDRV_NET_JOB_HTTP_GET:
        case KDRV_NET_JOB_HTTP_POST: {
            net_http_outcome(me, KODE_TIMEOUT);
            me->wreplied = true;
            break;
        }
        case KDRV_NET_JOB_UDP_SEND: {
            net_publish_udp(me, KODE_TIMEOUT, 0U);
            me->wreplied = true;
            break;
        }
        case KDRV_NET_JOB_HTTP_PUT: {
#if CONFIG_KODEOS_SHOT_HTTP
            shot_put_expired(me);
#endif
            me->wreplied = true;
            break;
        }
        default: {
            break;
        }
    }
}

/**
 * @brief Answer an open request.
 *
 * @param who  Who asked.
 * @param rc   How it went.
 * @param size The Content-Length, 0 when there is none to report.
 */
static void stream_reply_open(QActive * const who, kode_status_t rc,
                              uint32_t size)
{
    StorageOpenRspEvt * const rsp = Q_NEW_X(StorageOpenRspEvt, 0U,
                                            NET_STREAM_OPEN_RSP_SIG);

    if ( NULL == rsp )
    {
        KLOG_E(TAG, "out of events; an open's answer was dropped");
        return;
    }
    rsp->medium = (uint8_t)KDRV_MEDIUM_COUNT;   /* neither card nor FAT */
    rsp->size   = size;
    rsp->status = (uint8_t)rc;
    QACTIVE_POST(who, &rsp->super, AO_Net);
}

/**
 * @brief Send the reader the outcome of one read.
 *
 * @param who  The reader.
 * @param idx  The buffer the read filled.
 * @param got  Bytes read.
 * @param rc   How it went.
 * @return true when the event was posted.
 */
static bool stream_reply_data(QActive * const who, uint8_t idx, uint16_t got,
                              kode_status_t rc)
{
    StorageDataEvt * const d = Q_NEW_X(StorageDataEvt, 0U,
                                       NET_STREAM_DATA_SIG);

    if ( NULL == d )
    {
        KLOG_E(TAG, "out of events; the transfer cannot continue");
        return false;
    }
    d->len    = got;
    d->idx    = idx;
    d->status = (uint8_t)rc;
    d->eof    = (KODE_OK == rc) && (0U == got);
    QACTIVE_POST(who, &d->super, AO_Net);
    return true;
}

/**
 * @brief How many buffers a new read could use.
 *
 * A buffer is taken while it is out on loan, and while the read in flight is
 * filling it. Reads already asked for will take their share of what is left.
 *
 * @param me The AO.
 * @return The free buffers, before the reads asked for and not started.
 */
static uint8_t stream_free(const Net * const me)
{
    uint8_t n = 0U;

    for ( uint8_t i = 0U; (uint8_t)NET_STREAM_BUFS > i; i++ )
    {
        bool const filling = me->sbusy && ((uint8_t)NETS_READ == me->skind)
                             && (me->sidx == i);

        if ( (!me->slent[i]) && (!filling) )
        {
            n++;
        }
    }
    return n;
}

/**
 * @brief Open the transfer for @p who, or refuse it. The worker is free and no
 *        close is owed.
 *
 * @param me  The AO.
 * @param who Who asked; told how it went.
 * @param url The URL.
 */
static void stream_begin_open(Net * const me, QActive * const who,
                              const char *url)
{
    kode_status_t rc = KODE_OK;

    if ( me->sopen )
    {
        KLOG_E(TAG, "a transfer is already open");
        rc = KODE_DENIED;
    }
    else if ( !stream_bufs_ready(me) )
    {
        rc = KODE_NO_RESOURCES;
    }
    else if ( NULL == me->jobs )
    {
        rc = KODE_NO_RESOURCES;
    }
    else
    {
        (void)snprintf(me->jobs->s.url, sizeof(me->jobs->s.url), "%s", url);
        me->swho  = who;
        me->sheld = true;
        if ( !stream_job_start(me, NETS_OPEN, NET_STREAM_OPEN_GUARD_MS) )
        {
            me->sheld = false;
            me->swho  = NULL;
            rc        = KODE_DENIED;
        }
    }
    if ( KODE_OK != rc )
    {
        stream_reply_open(who, rc, 0U);
    }
}

static void stream_forget(Net * const me);

/**
 * @brief One step of starting the next download job.
 *
 * The close owed to the driver first, then the open that waited for it, then
 * a read asked for.
 *
 * @param me The AO.
 * @return true when the state changed so that another step may have work (a
 *         read the worker would not take ended the transfer).
 */
static bool stream_pump_once(Net * const me)
{
    bool again = false;

    if ( me->sbusy )
    {
        return false;
    }
    if ( me->sclose_owed )
    {
        if ( !me->sheld )
        {
            me->sclose_owed = false;
        }
        else if ( stream_job_start(me, NETS_CLOSE, KODE_NET_CLOSE_GUARD_MS) )
        {
            me->sclose_owed = false;
            return false;
        }
        else
        {
            KLOG_W(TAG, "the worker would not take the close; it stays owed");
            return false;
        }
    }
    if ( me->popen && (NULL != me->jobs) )
    {
        QActive * const who = me->pwho;

        me->popen = false;
        me->pwho  = NULL;
        stream_begin_open(me, who, me->jobs->purl);
    }
    else if ( me->sopen && (NULL != me->reader) && (0U < me->sreads) )
    {
        uint8_t idx = (uint8_t)NET_STREAM_BUFS;

        for ( uint8_t i = 0U; (uint8_t)NET_STREAM_BUFS > i; i++ )
        {
            if ( !me->slent[i] )
            {
                idx = i;
                break;
            }
        }
        me->sreads--;
        if ( (uint8_t)NET_STREAM_BUFS > idx )
        {
            me->jobs->s.buf = me->sbuf[idx];
            me->swho        = me->reader;
            me->sidx        = idx;
            if ( !stream_job_start(me, NETS_READ, NET_STREAM_READ_GUARD_MS) )
            {
                KLOG_E(TAG, "the worker would not take the read; the "
                            "transfer cannot continue");
                stream_forget(me);
                again = true;
            }
        }
    }
    else
    {
        /* Nothing is asked for. */
    }
    return again;
}

/**
 * @brief Start the next download job, if the worker is free.
 *
 * Called after every download job and every request that changes what is owed.
 *
 * @param me The AO.
 */
static void stream_pump(Net * const me)
{
    while ( stream_pump_once(me) )
    {
        /* the state changed under a step: look again */
    }
}

/**
 * @brief Forget the transfer, whatever it was doing, and owe the driver its
 *        close. A close always wins.
 *
 * The AO's side goes at once, since an exit from Net_online cannot wait. A
 * staged open and the one in flight are refused (a late success is closed,
 * not adopted), and a read in flight posts nothing when it ends.
 *
 * @param me The AO.
 */
static void stream_forget(Net * const me)
{
    me->sopen   = false;
    me->reader  = NULL;
    me->sreads  = 0U;
    for ( uint8_t i = 0U; (uint8_t)NET_STREAM_BUFS > i; i++ )
    {
        me->slent[i] = false;
    }
    if ( me->popen )
    {
        QActive * const who = me->pwho;

        me->popen = false;
        me->pwho  = NULL;
        stream_reply_open(who, KODE_DENIED, 0U);
    }
    if ( me->sbusy && (!me->sreplied) )
    {
        if ( ((uint8_t)NETS_OPEN == me->skind) && (NULL != me->swho) )
        {
            stream_reply_open(me->swho, KODE_DENIED, 0U);
        }
        me->sreplied = true;
    }
    /* A close already inside the worker covers it: nothing opens until that
     * one is done. */
    if ( me->sheld && (!(me->sbusy && ((uint8_t)NETS_CLOSE == me->skind))) )
    {
        me->sclose_owed = true;
    }
}

/**
 * @brief Forget the transfer and close the driver's side of it.
 *
 * The close is a job: it runs now when the worker is free, and when the
 * worker is with a job it runs the moment that job's done event comes in. A
 * new open meanwhile waits behind it, so the driver never sees two transfers.
 *
 * @param me The AO.
 */
static void stream_abandon(Net * const me)
{
    stream_forget(me);
    stream_pump(me);
}

/**
 * @brief An open was asked for.
 *
 * Refused while a transfer is open, being opened, or waiting. While a close is
 * owed or a forgotten transfer's job is out, the open waits, staged in
 * @c jobs, and runs when the worker is free.
 *
 * @param me  The AO.
 * @param req The request.
 */
static void stream_open_request(Net * const me,
                                NetStreamOpenEvt const * const req)
{
    bool const opening = me->sbusy && ((uint8_t)NETS_OPEN == me->skind)
                         && (!me->sreplied);

    if ( me->sopen || opening || me->popen )
    {
        KLOG_E(TAG, "a transfer is already open");
        stream_reply_open(req->who, KODE_DENIED, 0U);
    }
    else if ( me->sbusy || me->sclose_owed )
    {
        if ( NULL == me->jobs )
        {
            stream_reply_open(req->who, KODE_NO_RESOURCES, 0U);
        }
        else
        {
            (void)snprintf(me->jobs->purl, sizeof(me->jobs->purl), "%s",
                           req->url);
            me->pwho  = req->who;
            me->popen = true;
        }
    }
    else
    {
        stream_begin_open(me, req->who, req->url);
    }
}

/**
 * @brief The reader wants another chunk.
 *
 * Counted against the buffers that are free, so that every request is either
 * kept until the worker is free or refused for want of a buffer - never lost
 * behind a job in flight.
 *
 * @param me The AO.
 */
static void stream_want_read(Net * const me)
{
    if ( (!me->sopen) || (NULL == me->reader) )
    {
        return;
    }
    if ( stream_free(me) > me->sreads )
    {
        me->sreads++;
        stream_pump(me);
    }
    else
    {
        KLOG_W(TAG, "both buffers are out on loan; read request ignored");
    }
}

/**
 * @brief The open job came back.
 *
 * A late success is adopted only when its asker still waits and no close is
 * owed: otherwise nobody has the transfer and the driver's client is closed.
 *
 * @param me      The AO.
 * @param rc      How it went.
 * @param total   The Content-Length it reported.
 * @param replied Whether the asker was answered already (the guard fired, or
 *                the transfer was forgotten).
 */
static void stream_open_done(Net * const me, kode_status_t rc, uint32_t total,
                             bool replied)
{
    QActive * const who = me->swho;

    me->swho = NULL;
    if ( KODE_OK != rc )
    {
        if ( !me->sclose_owed )
        {
            me->sheld = false;
        }
    }
    else if ( replied || me->sclose_owed )
    {
        me->sclose_owed = true;
    }
    else
    {
        me->sopen  = true;
        me->reader = who;
    }
    if ( (!replied) && (NULL != who) )
    {
        if ( KODE_OK != rc )
        {
            stream_reply_open(who, rc, 0U);
        }
        else
        {
            stream_reply_open(who, rc, total);
        }
    }
}

/**
 * @brief A read job came back: send the reader what it produced.
 *
 * A read that ends after its transfer was forgotten, or whose reader was
 * answered already (the guard fired), posts nothing.
 *
 * @param me      The AO.
 * @param rc      How it went.
 * @param got     Bytes read.
 * @param replied Whether the reader was answered already.
 */
static void stream_read_done(Net * const me, kode_status_t rc, uint32_t got,
                             bool replied)
{
    QActive * const who = me->swho;

    me->swho = NULL;
    if ( replied || (NULL == who) || (!me->sopen) )
    {
        return;
    }
    if ( stream_reply_data(who, me->sidx, (uint16_t)got, rc) )
    {
        if ( (KODE_OK == rc) && ((uint8_t)NET_STREAM_BUFS > me->sidx) )
        {
            me->slent[me->sidx] = true;
        }
    }
    else
    {
        stream_abandon(me);
    }
}

/**
 * @brief The jobs the AO ran for the six kinds kdrv_net has: what each came to.
 *
 * Reads the driver's result tables, which are the job's until the next one is
 * handed over.
 *
 * @param me      The AO.
 * @param type    Which job it was.
 * @param rc      How it went.
 * @param replied Whether whoever waited has been answered already.
 */
static void net_done_net(Net * const me, kdrv_net_job_t type, kode_status_t rc,
                         bool replied)
{
    switch ( type )
    {
        case KDRV_NET_JOB_PING: {
            if ( replied )
            {
                break;
            }
            if ( me->testing )
            {
                kdrv_net_ping_stats_t s;
                kdrv_net_ping_stats(&s);
                me->testing = false;
                net_publish_test(me, (KODE_OK == rc) && (0U < s.rx));
            }
            else if ( KODE_OK == rc )
            {
                net_publish_ping(me);
            }
            else
            {
                net_publish_ping_end(me, false);
            }
            break;
        }
        case KDRV_NET_JOB_HTTP_GET:
        case KDRV_NET_JOB_HTTP_POST: {
            if ( !replied )
            {
                net_http_outcome(me, rc);
            }
            break;
        }
        case KDRV_NET_JOB_UDP_SEND: {
            if ( !replied )
            {
                net_publish_udp(me, rc, kdrv_net_udp_sent());
            }
            break;
        }
        case KDRV_NET_JOB_HTTP_PUT:
        case KDRV_NET_JOB_HTTP_CLOSE: {
            /* Never gated on @p replied: the upload's own record says whether
             * somebody still waits (SJ_LIVE) and what a late end owes
             * (SJ_STALE wipes the answer). */
#if CONFIG_KODEOS_SHOT_HTTP
            shot_on_job(me, (uint8_t)rc);
#endif
            break;
        }
        case KDRV_NET_JOB_MQTT_START: {
            if ( KODE_OK != rc )
            {
                /* The client never got as far as dialling. Answer now; the
                 * deadline has nothing left to wait for. */
                (void)QTimeEvt_disarm(&me->tmo);
                me->phase = (uint8_t)MQTT_IDLE;
                me->err   = (uint8_t)KDRV_MQTT_ERR_REFUSED;
                mqtt_publish_state(me);
            }
            /* Started is not connected: MQTT_EVENT_CONNECTED is the answer,
             * and me->tmo is what guarantees one either way. */
#if CONFIG_KODEOS_CHANNEL
            chan_on_mqtt_job(me, type, rc);
#endif
            break;
        }
        case KDRV_NET_JOB_MQTT_STOP: {
            me->phase = (uint8_t)MQTT_IDLE;
            mqtt_publish_state(me);
#if CONFIG_KODEOS_CHANNEL
            chan_on_mqtt_job(me, type, rc);
#endif
            break;
        }
        default: {
            break;
        }
    }
}

/**
 * @brief One job came back from the first worker.
 *
 * An event whose sequence number is not the job in flight's, or that comes
 * with no job in flight, is dropped, and the guard is left alone.
 *
 * @param me The AO.
 * @param j  What finished, and how it went.
 */
static void net_job_done(Net * const me, KqpWorkerDoneEvt const * const j)
{
    kode_status_t const rc = (kode_status_t)j->status;
    kdrv_net_job_t      type;
    bool                replied;
    bool                asking;

    if ( (!me->busy) || (j->seq != me->wseq) )
    {
        KLOG_D(TAG, "worker answer seq %u is not the job's (%u); dropped",
               (unsigned)j->seq, (unsigned)me->wseq);
        return;
    }
    /* Cleared before anything else is submitted: the steps below hand the
     * worker the next job, and that overwrites @c jobs->req. */
    type         = me->jobs->req.type;
    replied      = me->wreplied;
    asking       = me->asking;
    me->busy     = false;
    me->wreplied = false;
    me->asking   = false;
    net_guard_stop(&me->wguard);

    if ( asking )
    {
        ask_done(me, rc, replied);
    }
    else
    {
        net_done_net(me, type, rc, replied);
    }
#if CONFIG_KODEOS_CHANNEL
    chan_after_job(me);
#if CONFIG_KODEOS_SHOT_HTTP
    shot_kick(me);   /* after the channel's own step: it goes first */
#endif
#endif
    net_leave_settle(me);
#if CONFIG_KODE_LOWPOWER
    net_lp_settle(me);
    net_lp_rejoin(me);   /* an AWAKE that came while the stop was out */
    net_lp_radio_check(me);
#endif
}

/**
 * @brief One job came back from the download's worker.
 *
 * Same contract as net_job_done(), for the download's sequence numbers.
 *
 * @param me The AO.
 * @param j  What finished, and how it went.
 */
static void stream_job_done(Net * const me, KqpWorkerDoneEvt const * const j)
{
    kode_status_t const rc = (kode_status_t)j->status;
    net_stream_kind_t   kind;
    bool                replied;

    if ( (!me->sbusy) || (j->seq != me->sseq) )
    {
        KLOG_D(TAG, "download answer seq %u is not the job's (%u); dropped",
               (unsigned)j->seq, (unsigned)me->sseq);
        return;
    }
    kind         = (net_stream_kind_t)me->skind;
    replied      = me->sreplied;
    me->sbusy    = false;
    me->sreplied = false;
    net_guard_stop(&me->sguard);

    switch ( kind )
    {
        case NETS_OPEN: {
            stream_open_done(me, rc, j->value, replied);
            break;
        }
        case NETS_READ: {
            stream_read_done(me, rc, j->value, replied);
            break;
        }
        case NETS_CLOSE: {
            if ( !me->sclose_owed )
            {
                me->sheld = false;
            }
            break;
        }
        default: {
            KLOG_W(TAG, "a download outcome %u is not one of ours",
                   (unsigned)kind);
            break;
        }
    }
    stream_pump(me);
}

/**
 * @brief The job with the first worker outlived its guard.
 *
 * The job is still with the worker, so @c busy stays up. Whoever waits is
 * given the timed-out answer, and the late done event answers nobody. The
 * tables a running job writes are not read here.
 *
 * @param me The AO.
 */
static void net_job_guard(Net * const me)
{
    if ( !net_guard_genuine(&me->wguard) )
    {
        return;
    }
    if ( !me->busy )
    {
        return;
    }
    if ( me->asking )
    {
        KLOG_W(TAG, "the worker did not answer an app's request inside its guard");
        if ( !me->wreplied )
        {
            ask_expired(me);
        }
        return;
    }
    KLOG_W(TAG, "the worker did not answer inside its guard (job %d)",
           (int)me->jobs->req.type);
    if ( !me->wreplied )
    {
        net_expired_net(me);
    }
}

/**
 * @brief The download's job outlived its guard.
 *
 * Same contract as net_job_guard(): the job stays with the worker, its asker
 * is told it timed out, and a late success is closed and not adopted because
 * the asker was answered.
 *
 * @param me The AO.
 */
static void stream_job_guard(Net * const me)
{
    if ( !net_guard_genuine(&me->sguard) )
    {
        return;
    }
    if ( !me->sbusy )
    {
        return;
    }
    KLOG_W(TAG, "the download's worker did not answer inside its guard "
                "(job %u)", (unsigned)me->skind);
    if ( me->sreplied )
    {
        return;
    }
    switch ( (net_stream_kind_t)me->skind )
    {
        case NETS_OPEN: {
            if ( NULL != me->swho )
            {
                stream_reply_open(me->swho, KODE_TIMEOUT, 0U);
            }
            me->sreplied = true;
            break;
        }
        case NETS_READ: {
            if ( NULL != me->swho )
            {
                (void)stream_reply_data(me->swho, me->sidx, 0U, KODE_TIMEOUT);
            }
            me->sreplied = true;
            break;
        }
        default: {
            break;
        }
    }
}

/**
 * @brief Subscribe, take the jobs' PSRAM, start both workers and ask where Wi-Fi stands.
 *
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to Net_offline.
 */
static QState Net_initial(Net * const me, void const * const par)
{
    (void)par;

    /* No claim: this AO owns no board device. The MQTT client is a singleton
     * the driver enforces, not a resource anybody has to own. */
    QActive_subscribe(&me->super, WIFI_STATE_SIG);
#if CONFIG_KODE_LOWPOWER
    me->lp_who = lowpower_join_radio(&me->super, "net");
    if ( KODE_LOWPOWER_WHO_NONE != me->lp_who )
    {
        QActive_subscribe(&me->super, LOWPOWER_ROUND_SIG);
        QActive_subscribe(&me->super, LOWPOWER_STATE_SIG);
        radio_lp_expect_net(true);   /* an OFF round waits for this AO's leave */
    }
    /* The scheduler's check wakes the radio, and has nothing to wake while the
     * C5 link is down: ask where it stands rather than wait for an edge. */
    QActive_subscribe(&me->super, RADIO_STATE_SIG);
    radio_state_request();
#endif
#if CONFIG_KODEOS_CHANNEL
    /* The setting this AO owns, `chan`: on unless the store says off. */
    {
        uint8_t v    = 1U;
        bool    kept = false;

        me->chan_on = true;
        if ( (khal_nvs_get_u8(KSET_NAMESPACE, KSET_KEY_CHAN, &v) == KODE_OK)
            && (0U == v) )
        {
            me->chan_on = false;
            me->cstate  = (uint8_t)CHAN_OFF;
            KLOG_N(TAG, "channel: off (stored)");
        }
        me->keep_s = chan_keep_load(&kept);
    }
#endif

    /* What the workers read lives in PSRAM, taken once like the stream's
     * buffers, so the static AO stays small. */
    if ( NULL == me->jobs )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; PSRAM storage aligned for any object, and sized for this one */
        me->jobs = (NetJobs *)khal_alloc_psram(64U, sizeof(NetJobs));
    }
    if ( NULL != me->jobs )
    {
        (void)memset(me->jobs, 0, sizeof(*me->jobs));
    }
    else
    {
        KLOG_E(TAG, "no PSRAM for the jobs; the network services stay down");
    }
    if ( kqp_worker_start(&me->worker, &me->super, NET_WORKER_NAME,
                         NET_WORKER_STACK, KODE_PRIO_NET, NET_WORKER_DEPTH,
                         true) != KODE_OK )
    {
        KLOG_E(TAG, "no worker; the network services stay down");
    }
    if ( kqp_worker_start(&me->sworker, &me->super, NET_STREAM_WORKER_NAME,
                         NET_STREAM_WORKER_STACK, KODE_PRIO_NET,
                         NET_WORKER_DEPTH, true) != KODE_OK )
    {
        KLOG_E(TAG, "no worker for the download; transfers are refused");
    }
#if CONFIG_KODEOS_MQTT
    kdrv_net_mqtt_set_cb(&net_on_mqtt);
#endif

    /* Ask rather than wait for an edge that may have passed. Safe because
     * kode_init() starts AO_Radio before this AO: posting to an AO with no
     * queue asserts. */
    wifi_state_request();

    KLOG_I(TAG, "network services up (offline until there is an address)");
    return Q_TRAN(&Net_offline);
}

/**
 * @brief What both states answer the same way.
 *
 * The queries, and the worker outcomes: a job submitted online may land after
 * the network has gone, and must still be answered.
 */
static QState Net_root(Net * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
#if CONFIG_KODEOS_CHANNEL
        case CONFIG_REQ_SIG: {
            chan_config(me, Q_EVT_CAST(KodeConfigReqEvt));
            status = Q_HANDLED();
            break;
        }
#endif
        case NET_INFO_REQ_SIG: {
            net_publish_info(me);
            status = Q_HANDLED();
            break;
        }
        case MQTT_STATE_REQ_SIG: {
            mqtt_publish_state(me);
            status = Q_HANDLED();
            break;
        }
        case NET_JOB_SIG: {
            net_job_done(me, Q_EVT_CAST(KqpWorkerDoneEvt));
            status = Q_HANDLED();
            break;
        }
        case NET_JOB_GUARD_SIG: {
            net_job_guard(me);
            status = Q_HANDLED();
            break;
        }
        case NET_STREAM_JOB_SIG: {
            stream_job_done(me, Q_EVT_CAST(KqpWorkerDoneEvt));
            status = Q_HANDLED();
            break;
        }
        case NET_STREAM_GUARD_SIG: {
            stream_job_guard(me);
            status = Q_HANDLED();
            break;
        }
        case NET_LEAVE_REQ_SIG: {
            net_leave(me);
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODE_LOWPOWER
        case LOWPOWER_ROUND_SIG: {
            net_lp_round(me, Q_EVT_CAST(LowPowerRoundEvt));
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_STATE_SIG: {
            net_lp_state(me, Q_EVT_CAST(LowPowerStateEvt));
            status = Q_HANDLED();
            break;
        }
        case NET_CHECK_AT_SIG: {
            net_lp_check_at(me, Q_EVT_CAST(NetCheckAtEvt));
            status = Q_HANDLED();
            break;
        }
        case NET_CHECK_TMO_SIG: {
            net_lp_check_dwelled(me);
            status = Q_HANDLED();
            break;
        }
        case RADIO_STATE_SIG: {
            net_lp_link(me, Q_EVT_CAST(RadioStateEvt));
            status = Q_HANDLED();
            break;
        }
#endif
        case NET_MQTT_LINK_SIG: {
            NetMqttLinkEvt const * const l = Q_EVT_CAST(NetMqttLinkEvt);
            switch ( (kdrv_net_mqtt_evt_t)l->evt )
            {
                case KDRV_NET_MQTT_CONNECTED:
                    (void)QTimeEvt_disarm(&me->tmo);
                    me->phase = (uint8_t)MQTT_LIVE;
                    me->err   = (uint8_t)KDRV_MQTT_ERR_NONE;
#if CONFIG_KODEOS_CHANNEL
                    chan_on_connected(me, l->session_present);
#endif
                    break;
                case KDRV_NET_MQTT_REFUSED:
                    me->err = (uint8_t)KDRV_MQTT_ERR_REFUSED;
#if CONFIG_KODEOS_CHANNEL
                    chan_on_refused(me);
#endif
                    break;
                case KDRV_NET_MQTT_ERROR:
                    me->err = (uint8_t)KDRV_MQTT_ERR_REFUSED;
                    break;
                default:
                    /* A disconnect: a bench client reconnects by itself, so
                     * the phase stands and the report carries the driver's
                     * state; the channel's client does not, and fails. */
#if CONFIG_KODEOS_CHANNEL
                    chan_on_disconnected(me);
#endif
                    break;
            }
            mqtt_publish_state(me);
            status = Q_HANDLED();
            break;
        }
        case NET_MQTT_RX_SIG: {
            net_rx_drain(me);
            status = Q_HANDLED();
            break;
        }
        case NET_TMO_SIG: {
            /* The broker never answered: stop the client, so a failed
             * `mqtt_up` does not leave it dialling in the background. */
            if ( (uint8_t)MQTT_STARTING == me->phase )
            {
                kdrv_net_req_t req;
                me->err = (uint8_t)KDRV_MQTT_ERR_TIMEOUT;
                KLOG_W(TAG, "no answer from the broker in %us",
                       (unsigned)(KODE_MQTT_CONNECT_MS / 1000U));
                net_req_init(&req, KDRV_NET_JOB_MQTT_STOP);
                if ( net_submit(me, &req) )
                {
                    me->phase = (uint8_t)MQTT_STOPPING;
                }
                else
                {
                    /* The worker was busy and the client still dials. The
                     * phase stays: idle would make `mqtt_down` answer
                     * "already down" over a running client. */
                    KLOG_W(TAG, "could not stop the client yet; `mqtt_down` "
                                "once the current job finishes");
                }
                mqtt_publish_state(me);
#if CONFIG_KODEOS_CHANNEL
                if ( (uint8_t)CP_BROKER == me->cp )
                {
                    chan_fail(me, "broker_timeout", 0U);
                }
#endif
            }
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODEOS_CHANNEL
        case NET_CHAN_TMO_SIG: {
            chan_on_timer(me);
            status = Q_HANDLED();
            break;
        }
        case NET_CHAN_OUT_SIG: {
            chan_out_step(me);
            status = Q_HANDLED();
            break;
        }
        case NET_CHAN_FAST_SIG: {
            chan_fast_set(me, false);
            status = Q_HANDLED();
            break;
        }
        case NET_CHAN_WATCH_SIG: {
            chan_on_watch(me);
            status = Q_HANDLED();
            break;
        }
        case NET_LINE_DONE_SIG: {
            chan_q_done(me, Q_EVT_CAST(NetLineDoneEvt)->seq);
            status = Q_HANDLED();
            break;
        }
        case NET_LINE_BUSY_SIG: {
            chan_q_busy(me);
            status = Q_HANDLED();
            break;
        }
        case NET_CHAN_LINE_SIG: {
            chan_q_timer(me);
            status = Q_HANDLED();
            break;
        }
        case ID_PART_SIG: {
            chan_on_part(me, Q_EVT_CAST(IdPartEvt));
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODEOS_SHOT_HTTP
        case NET_SHOT_REQ_SIG: {
            shot_on_request(me, Q_EVT_CAST(NetShotReqEvt));
            status = Q_HANDLED();
            break;
        }
        case NET_SHOT_STOP_SIG: {
            shot_on_stop(me, Q_EVT_CAST(NetShotStopEvt));
            status = Q_HANDLED();
            break;
        }
        case DISPLAY_SHOT_RSP_SIG: {
            shot_on_display(me, Q_EVT_CAST(DisplayShotRspEvt));
            status = Q_HANDLED();
            break;
        }
        case NET_SHOT_WAIT_SIG: {
            shot_on_wait(me);
            status = Q_HANDLED();
            break;
        }
        case NET_SHOT_TICK_SIG: {
            shot_on_tick(me);
            status = Q_HANDLED();
            break;
        }
#endif
        case ID_END_SIG: {
            if ( (uint8_t)IDO_CERT == me->id_out )
            {
                me->id_out = (uint8_t)IDO_NONE;
                if ( (uint8_t)CP_CERT == me->cp )
                {
                    (void)QTimeEvt_disarm(&me->ctmo);
                    chan_on_cert_end(me, Q_EVT_CAST(IdEndEvt));
                }
            }
            status = Q_HANDLED();
            break;
        }
        case ID_SIGN_RSP_SIG: {
            if ( (uint8_t)IDO_SIGN == me->id_out )
            {
                me->id_out = (uint8_t)IDO_NONE;
                if ( (uint8_t)CP_SIGN == me->cp )
                {
                    (void)QTimeEvt_disarm(&me->ctmo);
                    chan_on_signature(me, Q_EVT_CAST(IdSignRspEvt));
                }
            }
            status = Q_HANDLED();
            break;
        }
#endif
        case CHANNEL_STATE_REQ_SIG: {
#if CONFIG_KODEOS_CHANNEL
            chan_publish(me);
#else
            ChannelStateEvt * const cs = Q_NEW_X(ChannelStateEvt, 0U, CHANNEL_STATE_SIG);
            if ( NULL != cs )
            {
                (void)memset(&cs->why, 0, sizeof(cs->why));
                (void)memset(&cs->broker, 0, sizeof(cs->broker));
                cs->state   = (uint8_t)CHAN_OFF;
                cs->since_s = 0U;
                cs->next_s  = 0U;
                cs->lost    = 0U;
                cs->fast    = false;
                cs->shot         = (uint8_t)KSHOT_IDLE;
                cs->shot_sent    = 0U;
                cs->shot_fps_x10 = 0U;
                cs->shot_left_s  = 0U;
                cs->keep_s       = 0U;
                cs->session      = (uint8_t)CHAN_SESSION_NONE;
                QACTIVE_PUBLISH(&cs->super, &me->super);
            }
#endif
            status = Q_HANDLED();
            break;
        }
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }
    return status;
}

/** @brief Refuse a request, saying which one and why. */
static void net_say_offline(const char *what)
{
    KLOG_W(TAG, "no network; %s needs an address - `wifi_join` first", what);
}

/** @brief Answer a transfer's open with a refusal: nothing was opened. */
static void stream_open_refuse(NetStreamOpenEvt const * const req)
{
    StorageOpenRspEvt *rsp = Q_NEW_X(StorageOpenRspEvt, 0U,
                                     NET_STREAM_OPEN_RSP_SIG);

    if ( NULL != rsp )
    {
        rsp->medium = (uint8_t)KDRV_MEDIUM_COUNT;
        rsp->size   = 0U;
        rsp->status = (uint8_t)KODE_DENIED;
        QACTIVE_POST(req->who, &rsp->super, AO_Net);
    }
    else
    {
        KLOG_E(TAG, "out of events; open request dropped");
    }
}

/**
 * @brief No address. Every request is refused *with an answer*.
 *
 * Never in silence: the console closes its commands on these answers.
 */
static QState Net_offline(Net * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            me->online = false;
            status = Q_HANDLED();
            break;
        }
        case WIFI_STATE_SIG: {
            WifiStateEvt const * const w = Q_EVT_CAST(WifiStateEvt);
            if ( ((uint8_t)KDRV_WIFI_GOT_IP == w->state) && (0U != w->ip4) )
            {
                status = Q_TRAN(&Net_online);
            }
            else
            {
                status = Q_HANDLED();
            }
            break;
        }
        case NET_STREAM_OPEN_REQ_SIG: {
            /* Refused with its own outcome event: the callers wait on it with
             * no deadline of their own. */
            net_say_offline("a transfer over HTTP");
            stream_open_refuse(Q_EVT_CAST(NetStreamOpenEvt));
            status = Q_HANDLED();
            break;
        }
        /* The other three only follow a successful open: a straggler after
         * the network went is swallowed here, with nothing to close. */
        case NET_STREAM_READ_REQ_SIG:
        case NET_STREAM_NEXT_SIG:
        case NET_STREAM_CLOSE_REQ_SIG: {
            status = Q_HANDLED();
            break;
        }
        case NET_PING_REQ_SIG: {
            net_say_offline("net_ping");
            net_publish_ping_end(me, false);
            status = Q_HANDLED();
            break;
        }
        case NET_HTTP_REQ_SIG: {
            net_say_offline("http_get");
            net_publish_http_done(me, KODE_DENIED);
            status = Q_HANDLED();
            break;
        }
        case NET_UDP_REQ_SIG: {
            net_say_offline("udp_send");
            net_publish_udp(me, KODE_DENIED, 0U);
            status = Q_HANDLED();
            break;
        }
        case NET_HOST_ASK_SIG:
        case NET_HTTP_ASK_SIG:
        case NET_UDP_ASK_SIG: {
            net_say_offline("an app's request");
            ask_refuse(e, KODE_IO_ERROR);
            status = Q_HANDLED();
            break;
        }
        case NET_TEST_REQ_SIG: {
            KLOG_N(TAG, "test net: no address - `wifi_join` first");
            net_publish_test(me, false);
            status = Q_HANDLED();
            break;
        }
        case MQTT_UP_REQ_SIG: {
            net_say_offline("mqtt_up");
            me->err = (uint8_t)KDRV_MQTT_ERR_NO_NET;
            mqtt_publish_state(me);
            status = Q_HANDLED();
            break;
        }
        case MQTT_DOWN_REQ_SIG: {
            mqtt_publish_state(me);   /* already down: that is the answer */
            status = Q_HANDLED();
            break;
        }
        case MQTT_PUB_REQ_SIG:
        case MQTT_SUB_REQ_SIG:
        case MQTT_UNSUB_REQ_SIG: {
            /* The reason is a '!' line; the state event is what closes the
             * verb, as it does online, and it carries the refusal. */
            net_say_offline("that MQTT request");
            mqtt_publish_answer((uint8_t)KDRV_MQTT_ERR_NO_NET);
            status = Q_HANDLED();
            break;
        }
        default: {
            status = Q_SUPER(&Net_root);
            break;
        }
    }
    return status;
}

/** @brief Refuse because the one job in flight is not done. */
static void net_say_busy(const char *what)
{
    KLOG_W(TAG, "a network job is already running; %s refused", what);
}

/**
 * @brief A time server answered. Runs on the client's thread, not an AO's.
 *
 * Setting the clock is one atomic write with no owner, so it is legal here.
 * Do not touch a queue or an event from this callback.
 *
 * @param[in] unix_s Seconds since the Unix epoch.
 */
static void net_sntp_synced(uint64_t unix_s)
{
    char iso[32];

    khal_time_wall_set(unix_s * 1000000ULL);
    kdrv_clocks_wall_note_sntp();

    /* Said: the log's timestamps change from time since boot to dates here. */
    if ( kdrv_clocks_wall_iso(iso, sizeof(iso)) == KODE_OK )
    {
        KLOG_N(TAG, "the time arrived from the network: %s", iso);
    }
}

#if CONFIG_KODE_LOWPOWER
/**
 * @brief While the board sleeps, refuse a request that would start work.
 *
 * Each is answered the way the offline state answers it, with the event the
 * asker waits for, and logged once. The sessions in place keep running.
 *
 * @param me The AO.
 * @param e  The event.
 * @return true when @p e was one of those and has been refused.
 */
static bool net_lp_refuses(Net * const me, QEvt const * const e)
{
    bool refused = me->lp_rest;

    if ( refused )
    {
        switch ( e->sig )
        {
            case NET_PING_REQ_SIG:
                net_publish_ping_end(me, false);
                break;
            case NET_HTTP_REQ_SIG:
                net_publish_http_done(me, KODE_DENIED);
                break;
            case NET_UDP_REQ_SIG:
                net_publish_udp(me, KODE_DENIED, 0U);
                break;
            case NET_TEST_REQ_SIG:
                net_publish_test(me, false);
                break;
            case MQTT_UP_REQ_SIG:
                /* The asker hears busy, as it does while the channel holds
                 * the client; the line below says why. */
                mqtt_publish_answer((uint8_t)KDRV_MQTT_ERR_BUSY);
                break;
            case NET_STREAM_OPEN_REQ_SIG:
                stream_open_refuse(Q_EVT_CAST(NetStreamOpenEvt));
                break;
#if CONFIG_KODEOS_CHANNEL && CONFIG_KODEOS_SHOT_HTTP
            case NET_SHOT_REQ_SIG:
                shot_refuse(me, Q_EVT_CAST(NetShotReqEvt)->who, KSHOT_R_BUSY);
                break;
#endif
            default:
                refused = false;
                break;
        }
        if ( refused )
        {
            KLOG_W(TAG, "the board is asleep; a request that starts work is refused");
        }
    }
    return refused;
}

static QState Net_online_work(Net * const me, QEvt const * const e);

/**
 * @brief With an address: work refused while the board sleeps, the rest to Net_online_work().
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState Net_online(Net * const me, QEvt const * const e)
{
    QState status;

    if ( net_lp_refuses(me, e) )
    {
        status = Q_HANDLED();
    }
    else
    {
        status = Net_online_work(me, e);
    }
    return status;
}

static QState Net_online_work(Net * const me, QEvt const * const e)
#else
/**
 * @brief With an address: transfers, asks, ping, HTTP, UDP, the test and MQTT.
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState Net_online(Net * const me, QEvt const * const e)
#endif
{
    QState status;
    switch ( e->sig )
    {
        case NET_STREAM_OPEN_REQ_SIG: {
            stream_open_request(me, Q_EVT_CAST(NetStreamOpenEvt));
            status = Q_HANDLED();
            break;
        }
        case NET_STREAM_READ_REQ_SIG: {
            stream_want_read(me);
            status = Q_HANDLED();
            break;
        }
        case NET_STREAM_NEXT_SIG: {
            /* A return is also the request for the next chunk. The buffer is
             * free at once, whatever job is in flight, and the read is
             * counted, so neither is lost. */
            StorageBufEvt const * const b = Q_EVT_CAST(StorageBufEvt);
            if ( (uint8_t)NET_STREAM_BUFS > b->idx )
            {
                me->slent[b->idx] = false;
            }
            stream_want_read(me);
            status = Q_HANDLED();
            break;
        }
        case NET_STREAM_CLOSE_REQ_SIG: {
            /* The AO's side goes now; the driver's close runs as soon as the
             * worker is free. */
            stream_abandon(me);
            status = Q_HANDLED();
            break;
        }
        case Q_EXIT_SIG: {
            /* The address is going: the transfer is forgotten, so a later
             * open is not refused, and the close runs now or when the job in
             * flight is done. */
            if ( me->sopen )
            {
                KLOG_W(TAG, "the network went while a transfer was open; "
                            "closing it");
            }
            stream_abandon(me);
            status = Q_HANDLED();
            break;
        }
        case Q_ENTRY_SIG: {
            kdrv_net_info_t info;
            char            ip[16];
            me->online = true;
            if ( kdrv_net_info(&info) == KODE_OK )
            {
                net_ip_str(info.ip4, ip, sizeof(ip));
                KLOG_N(TAG, "online as %s", ip);
            }
            /* The time client starts with the first address; every later
             * address asks again, since lwIP's own schedule waits up to an
             * hour. */
            if ( !me->sntp_asked )
            {
                if ( kdrv_net_sntp_start(net_sntp_synced) == KODE_OK )
                {
                    me->sntp_asked = true;
                }
                else
                {
                    KLOG_W(TAG, "the time client would not start");
                }
            }
            else if ( kdrv_net_sntp_sync_now() == KODE_OK )
            {
                KLOG_I(TAG, "back online: asking for the time again");
            }
            else
            {
                KLOG_W(TAG, "the time client would not ask again");
            }
#if CONFIG_KODEOS_CHANNEL
            /* With the address comes the channel, unless `chan` is off. */
            chan_enter(me, 0U);
#endif
            status = Q_HANDLED();
            break;
        }
        case WIFI_STATE_SIG: {
            WifiStateEvt const * const w = Q_EVT_CAST(WifiStateEvt);
            if ( ((uint8_t)KDRV_WIFI_GOT_IP == w->state) && (0U != w->ip4) )
            {
                status = Q_HANDLED();   /* still on, possibly a new address */
                break;
            }
            /* The network went away: the MQTT client must not stay "up". */
            KLOG_W(TAG, "the network is gone");
#if CONFIG_KODEOS_CHANNEL
            chan_leave(me, "");
#endif
            if ( ((uint8_t)MQTT_IDLE != me->phase)
                && ((uint8_t)MQTT_STOPPING != me->phase) )
            {
                kdrv_net_req_t req;
                (void)QTimeEvt_disarm(&me->tmo);
                me->err = (uint8_t)KDRV_MQTT_ERR_NO_NET;
                net_req_init(&req, KDRV_NET_JOB_MQTT_STOP);
                if ( net_submit(me, &req) )
                {
                    me->phase = (uint8_t)MQTT_STOPPING;
                }
                else
                {
                    /* The phase stays, so `mqtt_down` can still stop it. */
                    KLOG_W(TAG, "could not stop the client yet; it will be "
                                "stopped by the next `mqtt_down`");
#if CONFIG_KODEOS_CHANNEL
                    me->stop_pending = true;
#endif
                }
                mqtt_publish_state(me);
            }
            status = Q_TRAN(&Net_offline);
            break;
        }

        case NET_HOST_ASK_SIG:
        case NET_HTTP_ASK_SIG:
        case NET_UDP_ASK_SIG: {
            ask_take(me, e);
            status = Q_HANDLED();
            break;
        }
        case NET_PING_REQ_SIG: {
            NetPingReqEvt const * const p = Q_EVT_CAST(NetPingReqEvt);
            kdrv_net_req_t req;
            if ( me->busy )
            {
                net_say_busy("net_ping");
                net_publish_ping_end(me, false);
            }
            else
            {
                net_req_init(&req, KDRV_NET_JOB_PING);
                (void)snprintf(req.host, sizeof(req.host), "%s", p->host);
                req.count = p->count;
                me->testing = false;
                if ( !net_submit(me, &req) )
                {
                    net_publish_ping_end(me, false);
                }
            }
            status = Q_HANDLED();
            break;
        }
        case NET_HTTP_REQ_SIG: {
            NetHttpReqEvt const * const h = Q_EVT_CAST(NetHttpReqEvt);
            kdrv_net_req_t req;
            if ( me->busy )
            {
                net_say_busy("http_get");
                net_publish_http_done(me, KODE_DENIED);
            }
            else
            {
                net_req_init(&req, KDRV_NET_JOB_HTTP_GET);
                (void)snprintf(req.url, sizeof(req.url), "%s", h->url);
                if ( !net_submit(me, &req) )
                {
                    net_publish_http_done(me, KODE_DENIED);
                }
                else
                {
                    KLOG_I(TAG, "GET %s", h->url);
                }
            }
            status = Q_HANDLED();
            break;
        }
        case NET_UDP_REQ_SIG: {
            NetUdpReqEvt const * const u = Q_EVT_CAST(NetUdpReqEvt);
            kdrv_net_req_t req;
            if ( me->busy )
            {
                net_say_busy("udp_send");
                net_publish_udp(me, KODE_DENIED, 0U);
            }
            else
            {
                net_req_init(&req, KDRV_NET_JOB_UDP_SEND);
                (void)snprintf(req.host, sizeof(req.host), "%s", u->host);
                req.port = u->port;
                req.len  = ((uint16_t)sizeof(req.data) < u->len)
                           ? (uint16_t)sizeof(req.data) : u->len;
                (void)memcpy(req.data, u->data, req.len);
                if ( !net_submit(me, &req) )
                {
                    net_publish_udp(me, KODE_DENIED, 0U);
                }
            }
            status = Q_HANDLED();
            break;
        }
        case NET_TEST_REQ_SIG: {
            /* One echo to the gateway: a route, an ARP and an ICMP round
             * trip, with nothing outside the building. */
            kdrv_net_info_t info;
            kdrv_net_req_t  req;
            if ( me->busy || (kdrv_net_info(&info) != KODE_OK)
                || (0U == info.gw4) )
            {
                net_publish_test(me, false);
            }
            else
            {
                net_req_init(&req, KDRV_NET_JOB_PING);
                net_ip_str(info.gw4, req.host, sizeof(req.host));
                req.count   = 1U;
                me->testing = true;
                if ( !net_submit(me, &req) )
                {
                    me->testing = false;
                    net_publish_test(me, false);
                }
            }
            status = Q_HANDLED();
            break;
        }

        case MQTT_UP_REQ_SIG: {
            MqttUpEvt const * const m = Q_EVT_CAST(MqttUpEvt);
            kdrv_net_req_t req;
#if CONFIG_KODEOS_CHANNEL
            if ( chan_active(me) )
            {
                /* The channel holds the one esp-mqtt client. Refused with an
                 * answer: the console closes `mqtt_up` on it. */
                mqtt_publish_busy("mqtt_up");
            }
            else
#endif
            if ( me->busy )
            {
                /* Another network job holds the worker: refused. */
                net_say_busy("mqtt_up");
                me->err = (uint8_t)KDRV_MQTT_ERR_REFUSED;
                mqtt_publish_state(me);
            }
            else if ( (uint8_t)MQTT_IDLE != me->phase )
            {
                /* Answered with the state as it stands and no error: there
                 * is a client, which is what was asked for. */
                KLOG_W(TAG, "an MQTT client is already running; "
                            "`mqtt_down` first to point it somewhere else");
                mqtt_publish_state(me);
            }
            else
            {
                net_req_init(&req, KDRV_NET_JOB_MQTT_START);
                (void)snprintf(req.uri,  sizeof(req.uri),  "%s", m->uri);
                (void)snprintf(req.user, sizeof(req.user), "%s", m->user);
                (void)snprintf(req.pass, sizeof(req.pass), "%s", m->pass);
                if ( !net_submit(me, &req) )
                {
                    me->err = (uint8_t)KDRV_MQTT_ERR_REFUSED;
                    mqtt_publish_state(me);
                }
                else
                {
                    me->phase = (uint8_t)MQTT_STARTING;
                    me->err   = (uint8_t)KDRV_MQTT_ERR_NONE;
                    QTimeEvt_armX(&me->tmo,
                                  KQP_TICKS_PER_MS(KODE_MQTT_CONNECT_MS), 0U);
                    KLOG_I(TAG, "connecting to %s", m->uri);
                }
            }
            status = Q_HANDLED();
            break;
        }
        case MQTT_DOWN_REQ_SIG: {
            kdrv_net_req_t req;
#if CONFIG_KODEOS_CHANNEL
            if ( chan_active(me) )
            {
                mqtt_publish_busy("mqtt_down");
            }
            else
#endif
            if ( (uint8_t)MQTT_IDLE == me->phase )
            {
                mqtt_publish_state(me);   /* already down */
            }
            else if ( me->busy )
            {
                net_say_busy("mqtt_down");
                mqtt_publish_state(me);
            }
            else
            {
                (void)QTimeEvt_disarm(&me->tmo);
                me->err = (uint8_t)KDRV_MQTT_ERR_NONE;
                net_req_init(&req, KDRV_NET_JOB_MQTT_STOP);
                if ( !net_submit(me, &req) )
                {
                    mqtt_publish_state(me);
                }
                else
                {
                    me->phase = (uint8_t)MQTT_STOPPING;
                }
            }
            status = Q_HANDLED();
            break;
        }
        case MQTT_PUB_REQ_SIG: {
            MqttPubEvt const * const p = Q_EVT_CAST(MqttPubEvt);
#if CONFIG_KODEOS_CHANNEL
            if ( chan_active(me) )
            {
                /* The client is the channel's: a bench publish on it would
                 * speak as the Kode. */
                mqtt_publish_busy("mqtt_pub");
                status = Q_HANDLED();
                break;
            }
#endif
            if ( kdrv_net_mqtt_publish(p->topic, p->data, p->len, false)
                    != KODE_OK )
            {
                KLOG_W(TAG, "publish to '%s' refused: no broker", p->topic);
            }
            mqtt_publish_taken();   /* closes the verb: busy or ok */
            status = Q_HANDLED();
            break;
        }
        case MQTT_SUB_REQ_SIG: {
            MqttTopicEvt const * const t = Q_EVT_CAST(MqttTopicEvt);
#if CONFIG_KODEOS_CHANNEL
            if ( chan_active(me) )
            {
                mqtt_publish_busy("mqtt_sub");
                status = Q_HANDLED();
                break;
            }
#endif
            if ( kdrv_net_mqtt_subscribe(t->topic) != KODE_OK )
            {
                KLOG_W(TAG, "subscribe to '%s' refused: no broker", t->topic);
            }
            else
            {
                KLOG_N(TAG, "subscribed to '%s'", t->topic);
            }
            mqtt_publish_taken();
            status = Q_HANDLED();
            break;
        }
        case MQTT_UNSUB_REQ_SIG: {
            MqttTopicEvt const * const t = Q_EVT_CAST(MqttTopicEvt);
#if CONFIG_KODEOS_CHANNEL
            if ( chan_active(me) )
            {
                /* The one that would deafen the channel: a remote
                 * `mqtt_unsub` of cmd/in must not get through. */
                mqtt_publish_busy("mqtt_unsub");
                status = Q_HANDLED();
                break;
            }
#endif
            if ( kdrv_net_mqtt_unsubscribe(t->topic) != KODE_OK )
            {
                KLOG_W(TAG, "unsubscribe from '%s' refused: no broker",
                       t->topic);
            }
            mqtt_publish_taken();
            status = Q_HANDLED();
            break;
        }
        default: {
            status = Q_SUPER(&Net_root);
            break;
        }
    }
    return status;
}
