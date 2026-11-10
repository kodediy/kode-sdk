/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file radio_ao.c
 * @brief AO_Radio: the Active Object that owns the ESP32-C5.
 */
#include "kaos.h"
#include "kaos_ask.h"
#include "kaos_start.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "kqp.h"
#include "kqp_worker.h"
#include "kaos_claim.h"
#include "kbsp.h"
#include "kdrv_c5.h"
#include "kdrv_c5svc.h"
#include "khal_identity.h"
#include "kode_wire.h"
#include "kdrv_radio.h"
#include "kdrv_kfw.h"
#include "kapp_desc.h"      /* kapp_idf_desc(): what the staged image says it is */
#include "khal_crc32.h"
#include "khal_nvs.h"
#include "khal_system.h"
#include "khal_time.h"
#include "kdrv_wifi.h"
#if CONFIG_KODEOS_BLE
#include "kdrv_ble.h"
#endif
#include "kdrv_sd.h"
#include "kset.h"
#include "kode_log.h"
#include "sdkconfig.h"
#include "kverbs.h"
#include "krows.h"

#define TAG "kaos.radio"

#define RADIO_OLD_WIFI_NS  "kwifi" /**< NVS namespace of the store before the vault. */
#define RADIO_OLD_WIFI_KEY "nets"  /**< Its one key: every network, in clear.     */

/** @brief Whether this build has a scan an AO or a task can ask for: the
 *         Wi-Fi sweep, the BLE listen, or both. */
#define RADIO_ASKS  (CONFIG_KODEOS_WIFI || CONFIG_KODEOS_BLE)

/** @brief The radio worker's task name; `ps` prints it. */
#define RADIO_WORKER_NAME    "radio_worker"

/** @brief The radio worker's stack, in bytes: what the driver's own task had.
 *         Internal RAM: the bring-up and the BLE jobs reach NVS, which a
 *         PSRAM stack must not. */
#define RADIO_WORKER_STACK   6144U

/** @brief Jobs that may wait besides the one running: none is ever queued,
 *         because this AO submits only while no job is in flight. */
#define RADIO_WORKER_DEPTH   1U

/** @brief The service worker's task name. */
#define RADIO_SVC_WORKER_NAME   "radio_svc"

/** @brief The service worker's stack, in bytes. PSRAM: only the control block
 *         is internal. An overflow there corrupts a neighbour silently: 8 KB
 *         at least, whatever `ps` reads as free. */
#define RADIO_SVC_WORKER_STACK  8192U

/** @brief Requests of one service that may be with the service worker at
 *         once: the one waiting for its answer, and the one before it whose
 *         done event has not been read yet (an answer can beat it). */
#define RADIO_SVC_SLOTS      2U

/** @brief Jobs the service worker may hold: every slot of every service. */
#define RADIO_SVC_WORKER_DEPTH  ((uint8_t)(KODE_SVC_COUNT * RADIO_SVC_SLOTS))

/** @brief What a job outcome is, as the state handlers read it. One per kind
 *         of job, plus the link being lost. */
typedef enum {
    RADIO_OUT_CONNECT_DONE = 0, /**< A CONNECT job finished.                 */
    RADIO_OUT_DISCONNECT_DONE,  /**< A DISCONNECT job finished.              */
    RADIO_OUT_LINK_LOST,        /**< The link is gone. With KODE_ERROR the
                                 *   transport dropped on its own; with
                                 *   KODE_TIMEOUT a job failed and the C5 no
                                 *   longer answers.                         */
    RADIO_OUT_WIFI_SCAN_DONE,
    RADIO_OUT_WIFI_JOIN_DONE,   /**< Also the end of a WIFI_AWAIT or a WIFI_ADOPT. */
    RADIO_OUT_WIFI_LEAVE_DONE,
    RADIO_OUT_WIFI_STOP_DONE,
    RADIO_OUT_WIFI_RSSI_DONE,
    RADIO_OUT_WIFI_PREPARE_DONE,
    RADIO_OUT_C5_BEGIN_DONE,
    RADIO_OUT_C5_WRITE_DONE,
    RADIO_OUT_C5_FINISH_DONE,
    RADIO_OUT_C5_ABORT_DONE,
    RADIO_OUT_C5_INFO_DONE,     /**< KODE_OK: kdrv_c5's table is fresh.      */
    RADIO_OUT_BLE_START_DONE,
    RADIO_OUT_BLE_ADV_DONE,
    RADIO_OUT_BLE_SCAN_DONE,
    RADIO_OUT_BLE_QUIET_DONE,
    RADIO_OUT_BLE_STOP_DONE,
    RADIO_OUT_BLE_UNPAIR_DONE,
    RADIO_OUT_BLE_RSP_DONE,
    RADIO_OUT_WIFI_PS_DONE,
    RADIO_OUT_EXP_POWER_DONE,
    RADIO_OUT_AMP_DONE,
    RADIO_OUT_WIFI_PROBE_DONE,  /**< One channel of the radio check was heard. */
    RADIO_OUT_BLE_FIND_DONE,    /**< The radio check's BLE listen ended.      */
#if CONFIG_KODE_LOWPOWER
    RADIO_OUT_SLEEP_DONE,       /**< The sentinel was started or stopped.    */
#endif
} radio_out_t;

/** @brief What an answer from AO_Storage is for (see radio_ask()). */
typedef enum {
    RADIO_ASK_NONE = 0, /**< Nothing: an answer nobody is waiting for.        */
    RADIO_ASK_CONNECT,  /**< The unmount before the bring-up resets the C5.   */
    RADIO_ASK_BEGIN,    /**< The unmount before a C5 update opens its session. */
    RADIO_ASK_TEARDOWN, /**< The unmount before esp-hosted is torn down.      */
    RADIO_ASK_MOUNT     /**< The remount after the link came up.              */
} radio_ask_t;

/** @brief Most asks of AO_Storage waiting for an answer at once. */
#define RADIO_ASK_MAX  6U

#if CONFIG_KODE_LOWPOWER
/** @brief What the sentinel job in the worker is for (Radio.lp_job). */
enum {
    RADIO_LPJ_NONE = 0,   /**< No sentinel job is out.                      */
    RADIO_LPJ_START,      /**< Starting it for an ASLEEP round.             */
    RADIO_LPJ_STOP_AWAKE, /**< Stopping it because the board is awake.      */
    RADIO_LPJ_STOP_OFF    /**< Stopping it before an OFF round is answered. */
};

/* The busy reasons of a round, each short enough for LowPowerReadyEvt.why. */
#define RADIO_LP_WHY_UPDATE    "c5 update"           /**< An image is being written to the C5.   */
#define RADIO_LP_WHY_PARK      "c5 park"             /**< The C5 is being parked for a jump.     */
#define RADIO_LP_WHY_TEST      "c5 test"             /**< The production row runs.               */
#define RADIO_LP_WHY_BUSY      "radio busy"          /**< A job or a request holds the channel.  */
#define RADIO_LP_WHY_STARTING  "link starting"       /**< The link is coming up.                 */
#define RADIO_LP_WHY_STOPPING  "link stopping"       /**< The link is going down.                */
#define RADIO_LP_WHY_LOST      "link lost"           /**< The link went while the round waited.  */
#define RADIO_LP_WHY_PS        "c5 refused sleep"    /**< The C5 did not take the power save.    */
#define RADIO_LP_WHY_SDIO      "sdio not idle"       /**< esp-hosted's SDIO tasks never settled. */
#define RADIO_LP_WHY_WAKE      "wake line"           /**< The C5's wake line would not arm.      */
#define RADIO_LP_WHY_FAILED    "radio sleep failed"  /**< The sentinel could not be made.        */
#define RADIO_LP_WHY_DEAF      "c5 would not wake"   /**< The C5 did not answer the stop.        */

/** @brief Free places AO_Radio's queue keeps for work that matters more than
 *         a wake note (see radio_sleep_note()). */
#define RADIO_LP_NOTE_MARGIN  4U

/** @brief The port named in the log when the sentinel asks for AWAKE: it
 *         is not a console line, and the field only labels the request. */
#define RADIO_LP_SRC  ((uint8_t)KODE_CONSOLE_SRC_SERIAL)

_Static_assert(sizeof(RADIO_LP_WHY_FAILED) <= KODE_LOWPOWER_WHY_MAX,
               "the longest busy reason must fit LowPowerReadyEvt.why");
#endif /* CONFIG_KODE_LOWPOWER */

/** @brief How long after a refused unmount (the card answered, and is still
 *         mounted) it is asked again, in ms. */
#define RADIO_UNMOUNT_RETRY_MS  500U

/** @brief How long a refused unmount is asked again before the link, the
 *         update or the teardown goes on without the card, in ms. */
#define RADIO_UNMOUNT_WAIT_MS   10000U

/** @brief Milliseconds in a second, to print RADIO_UNMOUNT_WAIT_MS. */
#define RADIO_MS_PER_S          1000U

/** @brief Networks whose last channel is kept, most recent first. */
#define RADIO_HINTS_MAX      4U

/** @brief The NVS key of the channels (KSET_NAMESPACE). */
#define RADIO_HINTS_KEY      "wifi_hints"

/** @brief How long the hint listens on an open channel, in ms. A probe answers
 *         in a few; a radar channel is raised by kdrv_wifi_probe() to a beacon. */
#define RADIO_HINT_DWELL_MS  120U

/** @brief What a state handler reads of an outcome. */
typedef struct {
    uint8_t evt;     /**< radio_out_t: which job ended, or the link lost.     */
    uint8_t status;  /**< kode_status_t: how it went.                         */
} RadioOutcome;

/** @brief One request to a KODE service, as the service worker runs it. */
typedef struct {
    uint8_t *buf;       /**< The payload: PSRAM, KODE_MAX_PAYLOAD bytes, taken
                         *   on the first link.                              */
    uint16_t len;       /**< Its length.                                      */
    uint16_t seq;       /**< The worker's sequence number of this job.        */
    uint8_t  service;   /**< KODE_SVC_*.                                      */
    uint8_t  op;        /**< Per service.                                     */
    bool     inflight;  /**< Set at submit, cleared only by the done event of
                         *   the job: the worker reads @c buf until then.     */
    volatile bool cancelled; /**< The request is not wanted any more (its
                         *   link is gone, or its asker was answered): a job
                         *   the worker has not started does not send it.    */
} RadioSvcJob;

/** @brief What the two workers read, in PSRAM: it would cost the internal
 *         RAM that is left a third of a kilobyte. */
typedef struct {
    kdrv_radio_req_t wreq;       /**< The job with the worker: the worker's from
                                  *   the submit to the done event.            */
    RadioSvcJob      svc_job[KODE_SVC_COUNT][RADIO_SVC_SLOTS]; /**< Their jobs. */
} RadioJobs;

/** How far a C5 update has got. The phases exist because each one waits for
 * a different answer, and a single flag could not tell them apart. */
typedef enum {
    OTA_IDLE = 0,
    OTA_OPENING,   /**< Waiting for AO_Storage to open the file.            */
    OTA_DECODING,  /**< Reading the medium into PSRAM; the C5 is untouched. */
    OTA_BEGIN,     /**< Waiting for the C5 to open its OTA session.         */
    OTA_WRITING,   /**< Chunk in flight, or waiting for the next one.       */
    OTA_FINISH,    /**< Waiting for end+activate; the C5 then reboots.      */
    OTA_TEARDOWN,  /**< Activated; the link is coming down for this boot. */
} ota_phase_t;

/** Where a terminal install to the C5 has got. It shares the update's buffer
 * and, from the push on, the update's phases; what is its own is the wire
 * before that and the verdict after. */
typedef enum {
    INST_IDLE = 0,
    INST_ARMING,     /**< Waiting for AO_Log to say the wire is raw.         */
    INST_RECEIVING,  /**< Chunks arriving into PSRAM through the decoder.    */
    INST_READY,      /**< Verified; the push is about to start, or is owed.  */
    INST_LINKING,    /**< Verified; the link is coming up for it.            */
    INST_PUSHING,    /**< In Radio_updating from BEGIN on.                   */
    INST_ACTIVATED,  /**< Pushed and activated; answered once the link is down.*/
} inst_phase_t;

/** The bring-up reconcile, one service at a time: the C5 runs one job at a
 * time. Each step's *_DONE hands the baton on only while the chain runs
 * (RECON_BLE): a user's BLE start completing later must not restart it. */
typedef enum {
    RECON_DONE = 0,  /**< No chain running - every *_DONE is somebody else's. */
    RECON_START,     /**< Radio_up entry set this; the first step is next.    */
    RECON_BLE,       /**< BLE step in flight (start, then the advert); its
                      *   DONEs hand the baton to the wifi step.              */
    RECON_ADOPT,     /**< The adoption of a parked C5's station in flight;
                      *   its WIFI_JOIN_DONE hands the baton to the BLE step. */
} recon_t;

/** Where a park for a jump has got (Radio.park_phase, RADIO_PARK_REQ_SIG). */
enum {
    PARK_IDLE = 0,   /**< No park under way.                                  */
    PARK_QUIET,      /**< BLE is being hung up; the amplifier goes low when
                      *   that job is done.                                   */
    PARK_MUTE,       /**< The amplifier's enable is going low; the park is
                      *   asked when that job is done.                        */
    PARK_ASKED,      /**< The C5 was asked (SELF/PARK); its answer, or the
                      *   service deadline, is what AO_Boot is waiting for.   */
    PARK_DONE        /**< The C5 parked (answered OK, the note is left).
                      *   Terminal: nothing more is asked of the C5 until
                      *   the board resets.                                   */
};

/** Where the production row has got. Each step waits for a different outcome,
 * and the phase tells the row's own from somebody else's: a `wifi_scan` typed
 * a second earlier ends with the same event as the row's scan. */
typedef enum {
    TEST_IDLE = 0,   /**< No row in flight.                                  */
    TEST_LINK,       /**< Waiting for the bring-up the row asked for.        */
    TEST_INFO,       /**< The snapshot job is next, or in flight.            */
    TEST_SCAN,       /**< The WiFi scan is next, or in flight.               */
    TEST_BLE_START,  /**< The BLE stack's start is next, or in flight.       */
    TEST_BLE_SCAN,   /**< The BLE listen is next, or in flight.              */
} c5_test_phase_t;

/** The one Wi-Fi request that waits for the radio (Radio.wifi_owed). */
enum {
    WOWED_NONE = 0,  /**< Nothing waits.                                      */
    WOWED_JOIN,      /**< A join, with its network and password.              */
    WOWED_AUTO       /**< The round over the remembered networks.             */
};

/** Which of the two services a flag call is about. An enum rather than a
 *  bool, because `radio_flag_set(me, true, true)` says nothing. */
typedef enum {
    SVC_WIFI = 0,
    SVC_BLE,
} svc_t;

typedef struct {
    QActive super;
    uint8_t link;        /**< kradio_link_t, mirrored into every state event. */
    bool    busy;        /**< The channel is occupied: a job is with the
                          *   worker, or a chain of them is under way.        */

    /* The radio worker. */
    KqpWorker        worker;     /**< Runs the link's jobs, on an internal stack. */
    RadioJobs       *jobs;       /**< What the workers read (PSRAM, taken
                                  *   once): the job with the radio worker,
                                  *   the requests with the service worker.    */
    RadioOutcome     out;        /**< The outcome being handled; filled by
                                  *   radio_outcome() before a handler reads it. */
    uint16_t         wseq;       /**< Sequence number of that job.              */
    bool             winflight;  /**< A job is with the worker: set at submit,
                                  *   cleared only by its done event.           */
    bool             wreplied;   /**< Its guard fired and the AO has answered
                                  *   for it: the done event answers nobody.    */
    bool             wdrop;      /**< The link was lost under it: its done event
                                  *   answers nobody.                           */
    bool             disc_owed;  /**< A DISCONNECT is owed once that job and
                                  *   every KODE send are back.                 */
    QTimeEvt         guard;      /**< RADIO_JOB_GUARD_SIG: the job never came
                                  *   back.                                     */
    uint32_t         guard_stale; /**< Expired guard events still queued.        */
    bool             guard_live;  /**< The guard is armed and unexpired.         */

    /* The service worker: the KODE sends, off the AO's thread. */
    KqpWorker        svc_worker;                      /**< Runs the sends.      */
    bool             svc_worker_up;                   /**< It was created.      */
    uint8_t          svc_last[KODE_SVC_COUNT];        /**< Slot of each service's
                                                       *   latest request.       */
    bool             svc_wait[KODE_SVC_COUNT];        /**< A request of this
                                                       *   service waits for its
                                                       *   answer.               */

    /* The desired state of the two services - what the kset flags say, held
     * here so the store is read once. Radio_up's entry reconciles the link
     * to these; the requests that change them persist them in the same
     * breath. This AO is the flags' one writer. */
    bool    wifi_want;
    bool    ble_want;
    uint8_t recon;       /**< Where the bring-up reconcile has got (recon_t). */

    /* The advert's plan: visible or hidden, fast for a while after somebody
     * may be looking, then slow. Times in khal_time_ms(); see
     * radio_ble_adv_reconcile(). */
    bool     ble_fast_owed;   /**< The next advert starts fast.                 */
    bool     ble_asked;       /**< Somebody switched it on: fast even with no bond. */
    uint32_t ble_fast_until;  /**< The fast advert ends here.                   */
    bool     ble_pair_open;   /**< Pair new device was asked: visible until
                               *   ble_pair_until, or until a device pairs.     */
    uint32_t ble_pair_until;  /**< The time to pair ends here.                  */
    bool     ble_adv_hidden;  /**< The last advert planned hides the board.     */
    bool     ble_store_away;  /**< A DISCONNECT job is with the worker, which
                               *   takes NimBLE's store down with the link:
                               *   ble_bonds_seen stands for the count from the
                               *   submit until the worker is back.            */
    uint8_t  ble_bonds_seen;  /**< The bond count the last BLE state published. */
    bool     ble_linked;      /**< A central was on, as BLE_LINK_SIG last saw it. */

    /* The link as BLE_APP_LINK_SIG last told it: whether a central was on,
     * and the driver's count of links then, so a link that came and went
     * between two reads still shows as one that ended and one that began. */
    bool     ble_app_up;      /**< ble_connected was the last edge published. */
    uint32_t ble_links_seen;  /**< kdrv_ble_links() at that edge.             */

    /* A request that could not be carried out: the channel was busy, or the
     * worker would not take the job. The settle pays these debts and nothing
     * else, so it never stops a stack `ble_scan` brought up for itself. Per
     * service at most one of the two is set: asking for a service cancels a
     * stop it still owed, and vice versa (off then on in one busy job must
     * end on). */
    bool    wifi_stop_pending;
    bool    ble_stop_pending;
    bool    wifi_start_pending;
    bool    ble_start_pending;

    /* The production row. `test_pending` is its debt, paid by the same
     * settle that pays the services' - after them, because the operator
     * asked for those. `test_job` marks the job in flight as the row's own,
     * so its outcome is judged and not published as a listing: a scan the
     * row ran ends with the same event as one somebody typed. */
    uint8_t test_phase;    /**< c5_test_phase_t                             */
    bool    test_pending;  /**< Asked while the channel was busy.           */
    bool    test_user;     /**< The row in flight is the owner's, not the
                            *   production line's: the scan has only to
                            *   complete, with no floor on what it hears.   */
    bool    test_job;      /**< The job in flight belongs to the row.       */

    /* The scan in flight was typed as `wifi_scan`: only then are the sweep's
     * per-channel knocks published. An autojoin round and the production row
     * also scan, with no listener. `scan_sent` is what the end marker reports:
     * the entries that reached the bus, fewer than found if the pool ran dry. */
    bool     scan_live;
    uint16_t scan_sent;

    /* The asked scan: a Wi-Fi sweep (wifi_scan_ask(), wifi_scan_request_wait())
     * or a BLE listen (ble_scan_ask(), ble_scan_request_wait()), one at a time.
     * Owed only while the link comes up and the reconcile runs; refused behind
     * a console scan, a test row or an install. Its entries go to `ask_who`,
     * never to the bus, so `scan_live` stays false. */
    uint8_t     ask_scan;  /**< SCAN_ASK_NONE, _OWED or _RUNNING              */
    uint8_t     ask_kind;  /**< KQP_SCAN_WIFI or KQP_SCAN_BLE                 */
    uint32_t    ask_ms;    /**< KQP_SCAN_BLE: how long to listen.             */
    QActive    *ask_who;   /**< The asking AO; NULL for a task.               */
    ScanRspEvt *ask_rsp;   /**< Its end, taken with the request.              */
    ScanWaiter *ask_wait;  /**< The task's waiter; NULL for an AO.            */
    uint32_t    ask_gen;   /**< The task's tag.                               */
    uint16_t    ask_seen;  /**< Networks or devices handed over.              */
    bool        ask_cut;   /**< An entry did not fit the asker's queue.       */
    bool        ble_scan_live; /**< The console's BLE listen is on the worker:
                                *   an asked scan is refused behind it, as
                                *   behind `scan_live`.                       */

    /* The radio check (radio_probe_request()), one at a time. It is a chain
     * of the worker's jobs, each one's done event starting the next, and it
     * is answered exactly once with the readings it has. */
    uint8_t        pb_phase;  /**< PB_*: where the check has got.             */
    bool           pb_job;    /**< A job of the check is with the worker.     */
    uint8_t        pb_pass;   /**< Passes made over this phase's network.     */
    uint8_t        pb_rc;     /**< kode_status_t of the first job that failed.*/
    uint32_t       pb_t0;     /**< khal_time_ms() when its jobs began.        */
    uint32_t       pb_asked;  /**< khal_time_ms() when it was asked.          */
    uint32_t       pb_job_t0; /**< khal_time_ms() when its running job began. */
    QActive       *pb_who;    /**< Who is answered.                           */
    RadioProbeEvt *pb_rsp;    /**< The answer, taken with the request.        */
    radio_probe_t  pb_req;    /**< What to check.                             */

    /* The scan response an app asked for (BLE_RSP_REQ_SIG), kept for the
     * boot and never stored. `rsp_dirty` is the debt radio_settle() pays
     * with a BLE_RSP job; the job borrows `rsp_sent`, so a request arriving
     * meanwhile changes only the copy above it. */
    char        rsp_name[KQP_BLE_NAME_MAX];  /**< "" for the serial.          */
    uint8_t     rsp_len;                     /**< Bytes of rsp_data.          */
    uint8_t     rsp_data[KQP_BLE_DATA_MAX];  /**< The maker data.             */
    uint8_t     rsp_sent[KQP_BLE_DATA_MAX];  /**< What the job in flight reads. */
    bool        rsp_dirty;                   /**< Not handed to the radio yet. */


    /* The update in flight. */
    uint8_t  phase;      /**< ota_phase_t                                     */
    char     file[KODE_APP_FILE_MAX];
    uint32_t total;      /**< Image size, from the open response.             */
    uint32_t written;
    uint8_t  pct;        /**< Last decile reported, so logs stay readable.    */
    uint8_t  reported;   /**< Last percent published (APP_FLASH_PROGRESS_SIG). */
    bool     announced;  /**< A 0 % went out: the glass and the LED are owed
                          *   the done or the failure.                        */
    uint8_t  buf_idx;    /**< The storage buffer on loan to the worker.       */
    uint32_t buf_len;    /**< How much of it the worker is sending.           */
    bool     first_chunk;

    /* The C5's image, decoded whole into PSRAM before a byte is sent: the
     * bytes may arrive over the radio the write disturbs, and the C5's RPC is
     * asynchronous, so the decoder never has to pause mid-inflate. Freed as
     * soon as the push finishes. */
    kfw_buf_ctx_t kbuf;
    uint32_t      push_off;   /**< How far through kbuf the push has got. */
    /* The C5 has an OTA session open. Most failures happen while decoding,
     * before the C5 is asked for anything, and aborting a session it never
     * opened makes it log an error. */
    bool          c5_open;
    /* A URL or a file: the same conversation, this only decides which AO to
     * ask. */
    bool          from_net;

    /* ---- An image arriving on the console (RADIO_RECV_REQ_SIG) ---------- */
    /* A terminal install: the same staging buffer, filled from the wire. The
     * host is the only clock a transfer has: a host that dies mid-image is
     * caught by `inst_idle` instead of leaving the console waiting forever. */
    QTimeEvt inst_idle;       /**< No chunk for this long: the host is gone.  */
    bool     relink_owed;     /**< The C5 went mute and its link is being torn
                               *   down: build it again when that is done,
                               *   which resets the chip.                    */
    bool     relinked;        /**< This link is that rebuild and no job has
                               *   been answered on it yet: a second silence
                               *   is not retried.                           */
    QTimeEvt relink_tmo;      /**< RADIO_RELINK_SIG: the next bring-up.      */
    uint32_t relink_t0;       /**< When the window of tries began (ms).      */
    uint8_t  relink_n;        /**< Bring-ups by itself in that window.       */
    bool     relink_dead;     /**< No more of them until somebody asks.      */
    bool     relink_wake;     /**< One is owed to the board waking.          */
    bool     lp_rest;         /**< A round for the rest has begun, or the
                               *   board sleeps or is in a radio wake: nothing
                               *   this AO starts by itself may hold it up.  */
    bool     lp_slept;        /**< The board has been ASLEEP since last AWAKE. */
#if CONFIG_KODEOS_WIFI
    QTimeEvt wifi_retry;      /**< WIFI_RETRY_SIG: the next round to get back
                               *   on a network.                             */
    uint8_t  retry_step;      /**< Index into RADIO_RETRY_MS of the next wait. */
    bool     retry_on;        /**< Off the network and working on it.        */
    bool     retry_owed;      /**< The wait is over; a round starts when the
                               *   channel and the board allow.              */
    bool     wifi_left;       /**< The person stepped off the network: nothing
                               *   goes back until somebody asks.            */
#endif
    uint8_t  ask[RADIO_ASK_MAX]; /**< The asks of AO_Storage that are answered
                               *   and not yet heard, oldest first (radio_ask_t):
                               *   the answers come in the order the asks went. */
    uint8_t  ask_n;           /**< How many.                                   */
    QTimeEvt unmount_retry;   /**< RADIO_UNMOUNT_RETRY_SIG: a refused unmount
                               *   is asked again when this expires.          */
    uint8_t  retry_kind;      /**< radio_ask_t whose unmount is being asked
                               *   again; RADIO_ASK_NONE when none is.        */
    uint16_t retry_waited_ms; /**< Time spent since the first refusal.         */
    bool     begin_wait;      /**< A C5 update waits for the card's unmount
                               *   before it opens its session.               */
    bool     card_wait;       /**< The link is up and the card's remount has
                               *   been asked for: Radio_starting holds until
                               *   AO_Storage says it has run.                */
    bool     link_asked;      /**< The link was asked for by itself - `radio_up`,
                               *   a row, a C5 update - with neither service
                               *   wanted, so no service going off ends it.   */
    /* The expansion header's supply, +3.3V_EXT. Its enable is the C5's IO5,
     * pulled up by R72: the rail is on whenever the C5 does not drive it low,
     * after every C5 boot and while it is held in reset. `exp_want` is the
     * wish, `exp_on` the believed rail, `exp_known` whether that belief is
     * the C5's doing this boot. A mismatch, or a rail not known, is a debt
     * radio_exp_apply() pays as soon as the C5 can. */
    bool     exp_want;        /**< kode_sys_exp_power() or `exp_power` wants it on. */
    bool     exp_on;          /**< The rail is believed on.                    */
    bool     exp_known;       /**< The belief is the C5's doing this boot: it
                               *   drove IO5, or was reset and let go of it.
                               *   Cleared by a failed bring-up, a lost link,
                               *   a C5 update and a link-up whose switch did
                               *   not go through.                            */
    bool     exp_link;        /**< What the connect in flight leaves it at.    */
    bool     exp_job;         /**< An EXP_POWER job is with the worker.        */
    bool     exp_owed;        /**< A request waits for EXP_POWER_SIG to say the
                               *   wish is carried out (radio_exp_said()).    */
    QTimeEvt exp_pulse;       /**< EXP_PULSE_SIG: the reset that switches the
                               *   rail on has lasted long enough.            */
#if defined(CONFIG_KODEOS_C5_HOLD)
    bool     amp_asked;       /**< Something asked for the C5 for the amplifier
                               *   this boot: the C5 is never held again.     */
#endif
    /* The speaker amplifier's enable, the C5's IO4 (radio_amp_request()).
     * The C5's firmware raises it at its own boot. `amp_wished` says somebody
     * asked this boot, `amp_off` what for; `amp_low` is what the pin is
     * believed to be, worth anything only while `amp_known`. A wish the pin
     * does not match is a debt radio_amp_apply() pays over the link. */
    bool     amp_wished;      /**< radio_amp_request() was called this boot.   */
    bool     amp_off;         /**< And the last call wanted it off.            */
    bool     amp_low;         /**< IO4 is believed low.                        */
    bool     amp_known;       /**< That belief stands: a link brought it.      */
    bool     amp_answer_owed; /**< An "on" waits to be told C5_RUNNING_SIG
                               *   once the pin is known to be high.          */
    bool     linking;         /**< In Radio_starting: a link is on its way.   */
#if defined(CONFIG_KODEOS_C5_HOLD)
    QTimeEvt c5_boot;         /**< C5_BOOT_SIG: the C5 just let go has booted. */
    bool     c5_booting;      /**< That one-shot is running.                  */
#endif
    uint8_t  inst;            /**< inst_phase_t                               */
    bool     inst_pending;    /**< Verified and owed the channel (a debt).    */
    uint32_t inst_size;       /**< Bytes the host promised to put on the wire.*/
    uint32_t inst_want;       /**< The CRC they must add up to.               */
    uint32_t inst_crc;        /**< Running CRC over what has arrived.         */
    uint32_t inst_wire;       /**< Bytes taken off the wire so far.           */
    uint32_t inst_owed;       /**< Bytes since the last `>|ack`.              */
    bool     inst_ok;         /**< false once the decoder refused; the rest is
                               *   drained, because the host owes its count.  */
    char     inst_project[32]; /**< What the image says it is, kept for the
                                *   read-back after the C5 reboots.          */
    char     inst_version[32];

    /* The join in flight, kept so success can be remembered. */
    char ssid[KODE_SSID_MAX + 1U];
    char pass[KODE_PASS_MAX + 1U];

    /* The automatic round: which scan results have already been tried, so a
     * network that refuses us is not offered again on the same pass. One bit
     * per scan slot, which is why KDRV_WIFI_SCAN_MAX must stay under 32. */
    uint32_t tried;
    bool     autojoin;   /**< This join came from the automatic path. */

    /* Where the networks last joined were. A hint, not a fact: what is heard on
     * that channel decides, and a miss falls back to the sweep. NVS holds the
     * names and the channels, which are public; the passwords stay in the C5. */
    struct {
        char    ssid[KDRV_WIFI_SSID_LEN + 1U];
        uint8_t channel;
    } hints[RADIO_HINTS_MAX];    /**< Most recent first; an empty name ends it. */
    uint8_t  hint_at;            /**< The next one the round looks at.         */
    bool     hint_on;            /**< The round is still on the hints: no sweep
                                  *   has run, so the driver's table is not this
                                  *   round's.                                 */

    /* The KODE services on the C5 (kode_wire.h): one answer buffer and one
     * deadline per service, because one request per service may be in
     * flight. The buffers are PSRAM, taken on the first link. */
    QTimeEvt svc_tmo[KODE_SVC_COUNT];  /**< KODE_SVC_TMO_SIG per service.  */
    uint8_t *svc_buf[KODE_SVC_COUNT];  /**< The answer's payload.          */
    bool     info_asked;               /**< INFO went out on this link.    */

    /* Across a jump. A park is asked by AO_Boot before it restarts the
     * board; an adoption is what the next firmware does with a C5 the last
     * one parked (https://docs.kode.diy/sdk/kaos/radio). */
    uint8_t  park_phase;                /**< PARK_*.                       */
    uint8_t  park_target;               /**< kdrv_radio_note_t the note names. */
    bool     adopt;                     /**< The connect kept a parked C5 and
                                         *   Wi-Fi is wanted: the reconcile
                                         *   adopts its station first.       */
    bool     wifi_adopted;              /**< The station was adopted this
                                         *   link: the wifi step has no
                                         *   round to run.                   */

    /* The unit's identity on the C5: the one ID request in flight, who
     * hears its answer, and the certificate pair the factory is putting,
     * part by part (PSRAM, taken on the first part). */
    uint8_t  id_op;              /**< IOP_*.                               */
    QActive *id_who;             /**< Where the answer goes.               */
    uint8_t *id_put;             /**< A kode_id_cert_t being filled.       */
    uint16_t id_fill;            /**< DER bytes in it so far.              */
    uint8_t  id_next;            /**< The part expected next; 0 = none.    */

    /* c5_wipe: one at a time, its step, and who hears the end. */
    bool     wiping;             /**< A wipe is under way.                 */
    uint8_t  wipe_step;          /**< C5WipeStep in flight.                */
    QActive *wipe_who;           /**< Where the answer goes.               */

#if CONFIG_KODEOS_WIFI
    /* The Wi-Fi vault on the C5: the names it keeps (never a password), from
     * its last LIST, and what the vault request in flight is for. */
    kode_vault_list_t vlist;     /**< Names only.                          */
    bool     vlist_ok;           /**< vlist holds an answer.               */
    bool     nets_none_said;     /**< "no networks remembered" was said and the vault has none still. */
    bool     vlist_publish;      /**< The LIST in flight answers wifi_list. */
    bool     remember;           /**< Save the join in flight once it works. */
    uint8_t  vault_op;           /**< VOP_*.                               */

    /* The station's fast mode, asked for by the Kode channel's AO_Net: no
     * power save while commands are arriving. `fast_want` is the wish;
     * `ps_fast` is what the C5 is believed to hold, set when the job is
     * submitted. The two differing is a debt that radio_settle() pays.
     * A job the C5 refused puts the belief back and holds the debt until
     * the next request or the next other job, so a failure never spins. */
    bool     fast_want;          /**< The channel wants no power save.     */
    bool     ps_fast;            /**< The C5 holds no power save.          */
    bool     ps_held;            /**< A refused job waits for a new event. */
    bool     ps_on;              /**< `wifi_ps`: the station may save power
                                  *   when nobody wants it fast.          */
    bool     join_auto;          /**< `wifi_join`: the link coming up joins
                                  *   a remembered network by itself.     */

    /* The one join or autojoin that arrived while the radio could not serve
     * it: the latest replaces the one kept, a leave or a stop drops it. */
    uint8_t  wifi_owed;          /**< WOWED_*.                             */
    bool     owed_remember;      /**< The kept join saves its network.     */
    char     owed_ssid[KODE_SSID_MAX + 1U];
    char     owed_pass[KODE_PASS_MAX + 1U];
#endif

#if CONFIG_KODE_LOWPOWER
    /* The board's rest. */
    uint8_t  lp_who;             /**< The id lowpower_join_radio() gave.   */
    bool     lp_radio;           /**< A radio wake is on and this AO has not said it is done. */
    bool     lp_owed;            /**< A round waits for this AO's answer.  */
    uint32_t lp_round;           /**< That round's number.                 */
    uint8_t  lp_target;          /**< Its target, a kode_lp_state_t.       */
    uint8_t  lp_job;             /**< RADIO_LPJ_*: the sentinel job in the worker. */
    bool     lp_asleep;          /**< The sentinel is armed: the bus is down
                                  *   between wakes.                       */
    bool     lp_wake_owed;       /**< AWAKE came while a job was out: the
                                  *   sentinel is stopped as it returns.    */
    bool     lp_net_expected;    /**< AO_Net joined the rounds: OFF waits for it. */
    bool     lp_net_seen;        /**< AO_Net has said it left...           */
    uint32_t lp_net_round;       /**< ...for this round.                   */
    bool     lp_note_valid;      /**< A wake note has arrived since boot.  */
    RadioSleepNoteEvt lp_note;   /**< The last one.                        */
#endif
} Radio;

static Radio l_radio;

/* radio_boot_hold(): the first connect of this boot leaves the card mounted. */
static bool s_boot_card_stays;

/**
 * @brief Ask AO_Storage for the card's mount or unmount, WITH an answer, and
 *        remember what the answer is for.
 *
 * AO_Storage's mount and unmount run on its worker, so a posted one has not
 * run when the post returns. Whatever must not happen before the card has
 * let go (resetting the C5, deinitialising esp-hosted, which gives up the
 * SDMMC host the card sits on) waits for the answer instead. The answers come
 * in the order the asks went, so a short queue of what each is for is enough.
 *
 * @param me   The AO.
 * @param op   STORAGE_OP_MOUNT or STORAGE_OP_UNMOUNT.
 * @param kind What the answer is for.
 * @return true when the ask is with AO_Storage and its answer will come;
 *         false when it could not be made, and none will.
 */
static bool radio_ask(Radio * const me, storage_op_t op, radio_ask_t kind)
{
    if ( (uint8_t)RADIO_ASK_MAX <= me->ask_n )
    {
        KLOG_W(TAG, "too many asks of the card are waiting for an answer");
        return false;
    }
    if ( !storage_request_answered(KDRV_MEDIUM_SD, op, &me->super) )
    {
        return false;
    }
    me->ask[me->ask_n] = (uint8_t)kind;
    me->ask_n += 1U;
    return true;
}

/**
 * @brief Take the oldest ask off the list: AO_Storage has answered it.
 * @param me The AO.
 * @return What it was for; RADIO_ASK_NONE for an answer nobody is waiting for.
 */
static radio_ask_t radio_asked(Radio * const me)
{
    radio_ask_t kind = RADIO_ASK_NONE;

    if ( 0U < me->ask_n )
    {
        kind = (radio_ask_t)me->ask[0];
        for ( uint8_t i = 1U; i < me->ask_n; ++i )
        {
            me->ask[i - 1U] = me->ask[i];
        }
        me->ask_n -= 1U;
    }
    return kind;
}

/** @brief Whether an ask of @p kind is waiting for its answer. */
static bool radio_asking(Radio const * const me, radio_ask_t kind)
{
    for ( uint8_t i = 0U; i < me->ask_n; ++i )
    {
        if ( (uint8_t)kind == me->ask[i] )
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief The unmount of @p kind is settled: stop asking again.
 * @param me   The AO.
 * @param kind The ask that ended.
 */
static void radio_unmount_end(Radio * const me, radio_ask_t kind)
{
    if ( (uint8_t)kind == me->retry_kind )
    {
        (void)QTimeEvt_disarm(&me->unmount_retry);
        me->retry_kind      = (uint8_t)RADIO_ASK_NONE;
        me->retry_waited_ms = 0U;
    }
}

/**
 * @brief Say that the card stayed busy for the whole window, and what comes of it.
 * @param kind The ask that gave up.
 */
static void radio_unmount_gave_up(radio_ask_t kind)
{
    unsigned const secs = RADIO_UNMOUNT_WAIT_MS / RADIO_MS_PER_S;

    switch ( kind )
    {
        case RADIO_ASK_CONNECT: {
            KLOG_W(TAG, "the SD card stayed busy for %u s; link not started",
                   secs);
            break;
        }
        case RADIO_ASK_BEGIN: {
            KLOG_W(TAG, "the SD card stayed busy for %u s; C5 update not "
                        "started", secs);
            break;
        }
        default: {
            KLOG_W(TAG, "the SD card stayed busy for %u s; the link comes "
                        "down anyway", secs);
            break;
        }
    }
}

/**
 * @brief An unmount was answered: wait and ask again, or settle it.
 *
 * AO_Storage refuses an unmount while a transfer has the card open, and
 * answers all the same: a card still mounted is asked again every
 * RADIO_UNMOUNT_RETRY_MS until RADIO_UNMOUNT_WAIT_MS have gone by. One window
 * at a time: another ask refused meanwhile gets no second window.
 *
 * @param me   The AO.
 * @param kind What the answered ask was for.
 * @return true when the card is still mounted and the ask will be made again
 *         (the caller waits); false when the card let go, or the window is
 *         spent (one warning said), and the caller goes on.
 */
static bool radio_unmount_wait(Radio * const me, radio_ask_t kind)
{
    bool wait = false;

    if ( !kdrv_sd_mounted() || ((RADIO_ASK_CONNECT == kind) && s_boot_card_stays) )
    {
        radio_unmount_end(me, kind);
    }
    else if ( ((uint8_t)RADIO_ASK_NONE != me->retry_kind)
             && ((uint8_t)kind != me->retry_kind) )
    {
        /* The timer is busy with another ask: no second window. */
    }
    else
    {
        if ( (uint8_t)RADIO_ASK_NONE == me->retry_kind )
        {
            me->retry_kind      = (uint8_t)kind;
            me->retry_waited_ms = 0U;
        }
        if ( RADIO_UNMOUNT_WAIT_MS > me->retry_waited_ms )
        {
            QTimeEvt_armX(&me->unmount_retry,
                          KQP_TICKS_PER_MS(RADIO_UNMOUNT_RETRY_MS), 0U);
            wait = true;
        }
        else
        {
            radio_unmount_gave_up(kind);
            radio_unmount_end(me, kind);
        }
    }
    return wait;
}

/**
 * @brief The retry timer expired: ask for the unmount again.
 *
 * @param me The AO.
 * @return true when the ask was made again (its answer will come), or could
 *         not be made and the timer was set again; false when it could not
 *         be made and the window is spent: the caller runs the answer's path,
 *         which finds the window spent.
 */
static bool radio_unmount_refire(Radio * const me)
{
    bool again = true;

    me->retry_waited_ms += (uint16_t)RADIO_UNMOUNT_RETRY_MS;
    if ( !radio_ask(me, STORAGE_OP_UNMOUNT, (radio_ask_t)me->retry_kind) )
    {
        if ( RADIO_UNMOUNT_WAIT_MS > me->retry_waited_ms )
        {
            QTimeEvt_armX(&me->unmount_retry,
                          KQP_TICKS_PER_MS(RADIO_UNMOUNT_RETRY_MS), 0U);
        }
        else
        {
            again = false;
        }
    }
    return again;
}

/** @brief Whether the teardown still waits for the card: an unmount asked and
 *         not answered, or one that is to be asked again. */
static bool radio_teardown_waiting(Radio const * const me)
{
    return radio_asking(me, RADIO_ASK_TEARDOWN)
        || ((uint8_t)RADIO_ASK_TEARDOWN == me->retry_kind);
}

QActive * const AO_Radio = &l_radio.super;

/* The C5's own slots are 1.9 MB each (https://docs.kode.diy/sdk/kdrv/c5), so an
 * image larger than that could not be flashed even if it fitted in PSRAM.
 * The ceiling is here so a header that lies about its length is refused
 * before it can ask for the heap. */
#define C5_IMAGE_MAX   (1920U * 1024U)
/* What one KDRV_RADIO_JOB_C5_WRITE carries: 32 KB, the pacing the C5 was
 * validated at. */
#define C5_PUSH_CHUNK  (32U * 1024U)

/* The install's watchdog on the gap between chunks - the same deadline
 * AO_AppMgr keeps for its terminal installs - and the long-stop insurance on
 * the arming handshake itself, armed until AO_Log has answered. */
#define RADIO_RECV_IDLE_TICKS      KQP_TICKS_PER_MS(KODE_RECV_IDLE_MS)
#define RADIO_RECV_LONGSTOP_TICKS  KQP_TICKS_PER_MS(30000U)

/** How long the C5 is held in reset to switch the header's supply on with no
 *  link: the pulse esp-hosted itself gives the chip's enable line. */
#define RADIO_EXP_PULSE_MS  10U

/* The receive protocol's answers, static and immutable: they cannot fail to
 * allocate, and the console has given up its own watchdog for this command,
 * so one of them must always be able to arrive. */
static const QEvt s_recv_ready_evt = QEVT_INITIALIZER(RECV_READY_SIG);
static const QEvt s_recv_ack_evt   = QEVT_INITIALIZER(RECV_ACK_SIG);
static const QEvt s_recv_done_evt  = QEVT_INITIALIZER(RECV_DONE_SIG);
static const QEvt s_recv_fail_evt  = QEVT_INITIALIZER(RECV_FAIL_SIG);
static const QEvt s_recv_stall_evt = QEVT_INITIALIZER(RECV_STALL_SIG);
static const QEvt s_raw_end_evt    = QEVT_INITIALIZER(CONSOLE_RAW_END_SIG);
static const QEvt s_inst_go_evt    = QEVT_INITIALIZER(RADIO_INSTALL_GO_SIG);
/* How a C5 update ends for the Opening sheet and the LED - the same two
 * AO_AppMgr's writes end with; see c5_report(). */
static const AppFlashDoneEvt s_flash_done_evt = {
    QEVT_INITIALIZER(APP_FLASH_DONE_SIG), ""   /* a C5 image installs no app */
};
static const AppFlashFailEvt s_flash_fail_evt = {
    QEVT_INITIALIZER(APP_FLASH_FAIL_SIG), (uint8_t)KFAIL_WRITE
};

static QState Radio_initial(Radio * const me, void const * const par);
static QState Radio_down(Radio * const me, QEvt const * const e);
static QState Radio_off(Radio * const me, QEvt const * const e);
static QState Radio_starting(Radio * const me, QEvt const * const e);
static QState Radio_stopping(Radio * const me, QEvt const * const e);
static QState Radio_failed(Radio * const me, QEvt const * const e);
static QState Radio_up(Radio * const me, QEvt const * const e);
static QState Radio_updating(Radio * const me, QEvt const * const e);
#if CONFIG_KODEOS_WIFI
static QState Radio_autojoin(Radio * const me, QEvt const * const e);
#endif

/** @brief Start the radio check that waited for the channel; see the definition. */
static bool probe_owed_start(Radio * const me);
static void c5_test_step(Radio * const me);
static void c5_test_end(Radio * const me, bool ok, const char *why);
#if CONFIG_KODEOS_BLE
static void c5_test_judge_ble_start(Radio * const me, bool ok);
#endif
static void inst_end(Radio * const me, const QEvt *verdict);
static void inst_fail(Radio * const me);
static void ota_fail(Radio * const me, const char *why);
static bool radio_submit(Radio * const me, kdrv_radio_job_t job);
static void radio_disc_release(Radio * const me);
static void inst_post_go(Radio * const me);
static void c5_release(Radio * const me);
static kode_status_t c5_reserve(void *ctx, uint32_t img_len, const char *name);
#if CONFIG_KODE_LOWPOWER
static void radio_sleep_note(const kdrv_radio_sleep_note_t *n, void *arg);
#endif

/* ---- Request helpers ----------------------------------------------------- */

void radio_up_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(RADIO_UP_REQ_SIG);

    /* AO_Ir asks for app traffic: the margin is AO_Radio's own. */
    if ( !QACTIVE_POST_X(AO_Radio, &evt, KQP_RADIO_POST_MARGIN, NULL) )
    {
        KLOG_W(TAG, "AO_Radio's queue is full; a request for the link is dropped");
    }
}

void radio_down_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(RADIO_DOWN_REQ_SIG);
    QACTIVE_POST(AO_Radio, &evt, NULL);
}

void radio_state_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(RADIO_STATE_REQ_SIG);
    QACTIVE_POST(AO_Radio, &evt, NULL);
}

void radio_boot_hold(void)
{
    s_boot_card_stays = true;
    kdrv_radio_gate(true);
}

void radio_boot_release(void)
{
    kdrv_radio_gate(false);
}

void radio_park_request(kdrv_radio_note_t target)
{
    RadioParkEvt *e = Q_NEW_X(RadioParkEvt, 0U, RADIO_PARK_REQ_SIG);
    if ( NULL != e )
    {
        e->target = (uint8_t)target;
        QACTIVE_POST(AO_Radio, &e->super, NULL);
    }
    else
    {
        /* No block to carry the ask, so no park: the jump is answered
         * as if the C5 had refused, rather than left waiting for it. */
        KLOG_E(TAG, "out of events; the C5 is not parked");
        boot_radio_parked();
    }
}

/**
 * @brief The length of @p s, counting no further than @p cap (strnlen()
 *        without POSIX).
 */
static size_t id_text_len(const char *s, size_t cap)
{
    size_t n = 0U;

    while ( (n < cap) && ('\0' != s[n]) )
    {
        n++;
    }
    return n;
}

bool id_sign_request(QActive *who, const char *nonce)
{
    IdSignReqEvt *const e = Q_NEW_X(IdSignReqEvt, 0U, ID_SIGN_REQ_SIG);
    if ( NULL == e )
    {
        return false;
    }
    e->who      = who;
    e->nonce[0] = '\0';
    if ( NULL != nonce )
    {
        size_t const n = id_text_len(nonce, (size_t)KODE_ID_NONCE_MAX + 1U);
        /* One too long stays empty, which the nonce rule refuses: cut to
         * fit, it would be signed as a nonce nobody sent. */
        if ( (size_t)KODE_ID_NONCE_MAX >= n )
        {
            (void)memcpy(e->nonce, nonce, n);
            e->nonce[n] = '\0';
        }
    }
    QACTIVE_POST(AO_Radio, &e->super, NULL);
    return true;
}

bool c5_wipe_request(QActive *who)
{
    C5WipeReqEvt *const e = Q_NEW_X(C5WipeReqEvt, 0U, C5_WIPE_REQ_SIG);
    if ( NULL == e )
    {
        return false;
    }
    e->who = who;
    QACTIVE_POST(AO_Radio, &e->super, NULL);
    return true;
}

bool id_request(QActive *who, uint8_t op, uint8_t n, uint8_t of,
                const uint8_t *der, size_t len)
{
    IdReqEvt *const e = Q_NEW_X(IdReqEvt, 0U, ID_REQ_SIG);
    if ( NULL == e )
    {
        return false;
    }
    e->who = who;
    e->op  = op;
    e->n   = n;
    e->of  = of;
    e->len = 0U;
    if ( (NULL != der) && ((size_t)KODE_ID_PART_BYTES >= len) )
    {
        (void)memcpy(e->der, der, len);
        e->len = (uint8_t)len;
    }
    QACTIVE_POST(AO_Radio, &e->super, NULL);
    return true;
}

/** @brief Where the one asked scan stands (Radio.ask_scan). */
enum {
    SCAN_ASK_NONE = 0,  /**< Nothing asked.                                */
    SCAN_ASK_OWED,      /**< Held, waiting for the link or the channel.    */
    SCAN_ASK_RUNNING    /**< Its sweep is on the worker.                   */
};

#if CONFIG_KODEOS_WIFI
void wifi_scan_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(WIFI_SCAN_REQ_SIG);
    QACTIVE_POST(AO_Radio, &evt, NULL);
}
#endif /* CONFIG_KODEOS_WIFI */

#if RADIO_ASKS
/** @brief The most an asked scan's end can count: its @c seen is a byte. */
#define SCAN_SEEN_MAX  255U

/** @brief The kinds of ask AO_Radio has doors for; the first compiled is 0. */
enum {
#if CONFIG_KODEOS_WIFI
    RADIO_DOOR_WIFI_SCAN,      /**< wifi_scan_ask().      */
#endif
#if CONFIG_KODEOS_BLE
    RADIO_DOOR_BLE_SCAN,       /**< ble_scan_ask().       */
#endif
    RADIO_DOORS                /**< How many kinds.       */
};

/**
 * @brief The door of a kind of scan.
 * @param kind KQP_SCAN_WIFI or KQP_SCAN_BLE, one this build compiles.
 * @return Its door.
 */
static kaos_ask_door_t *scan_door(uint8_t kind)
{
    /* One door per asker and kind: an asker has one scan of a kind waiting at a time. */
    static kaos_ask_door_t s_radio_doors[RADIO_DOORS];
    kaos_ask_door_t       *door = &s_radio_doors[0];

#if CONFIG_KODEOS_WIFI && CONFIG_KODEOS_BLE
    if ( (uint8_t)KQP_SCAN_BLE == kind )
    {
        door = &s_radio_doors[RADIO_DOOR_BLE_SCAN];
    }
#else
    (void)kind;
#endif
    return door;
}

/**
 * @brief Stamp @p gen as the answer's, ordered after everything written before it.
 * @param ack Where the requester reads it.
 * @param gen The request's tag.
 */
static void scan_ack_write(uint32_t *ack, uint32_t gen)
{
    /* cppcheck-suppress misra-c2012-17.3 ; GCC's __atomic builtins are the compiler's own, never declared in a header */
    __atomic_store_n(ack, gen, __ATOMIC_RELEASE);
}

/**
 * @brief Ask for a scan for an AO through its kind's door.
 * @param who  The asking AO.
 * @param kind KQP_SCAN_WIFI or KQP_SCAN_BLE.
 * @param sig  The end's signal, WIFI_SCAN_RSP_SIG or BLE_SCAN_RSP_SIG.
 * @param ms   ScanAskEvt.ms.
 * @return As wifi_scan_ask().
 */
static kode_status_t scan_ask(QActive *who, uint8_t kind, QSignal sig, uint32_t ms)
{
    kaos_ask_door_t *const door = scan_door(kind);
    kode_status_t          rc   = kaos_ask_take(door, who);
    ScanRspEvt            *a;
    ScanAskEvt            *e;

    if ( KODE_OK != rc )
    {
        return rc;
    }
    a = Q_NEW_X(ScanRspEvt, 0U, sig);
    if ( NULL == a )
    {
        return kaos_ask_cancel(door, who, NULL);
    }
    e = Q_NEW_X(ScanAskEvt, 0U, RADIO_SCAN_ASK_SIG);
    if ( NULL == e )
    {
        return kaos_ask_cancel(door, who, &a->super);
    }
    a->rc   = (uint8_t)KODE_NO_RESOURCES;
    a->seen = 0U;
    a->more = false;
    e->ask  = who;
    e->rsp  = a;
    e->wait = NULL;
    e->gen  = 0U;
    e->ms   = ms;
    e->kind = kind;
    return kaos_ask_post(door, who, AO_Radio, &e->super, &a->super, KQP_RADIO_CMD_MARGIN);
}

/**
 * @brief Answer a blocked task's request at once, without AO_Radio.
 * @param wait Its waiter.
 * @param gen  Its tag, stamped with the answer.
 * @param rc   The answer.
 */
static void scan_wait_refuse(ScanWaiter *wait, uint32_t gen, kode_status_t rc)
{
    wait->rc  = (uint8_t)rc;
    wait->got = 0U;
    scan_ack_write(&wait->ack, gen);
    khal_sem_signal(wait->done);
}

/**
 * @brief Ask for a scan for a blocked task.
 * @param wait Its waiter.
 * @param gen  Its tag.
 * @param kind KQP_SCAN_WIFI or KQP_SCAN_BLE.
 * @param ms   ScanAskEvt.ms.
 */
static void scan_request_wait(ScanWaiter *wait, uint32_t gen, uint8_t kind, uint32_t ms)
{
    ScanAskEvt *e = Q_NEW_X(ScanAskEvt, 0U, RADIO_SCAN_ASK_SIG);

    if ( NULL != e )
    {
        e->ask  = NULL;
        e->rsp  = NULL;
        e->wait = wait;
        e->gen  = gen;
        e->ms   = ms;
        e->kind = kind;
        if ( QACTIVE_POST_X(AO_Radio, &e->super, KQP_RADIO_CMD_MARGIN, NULL) )
        {
            return;
        }
        /* QP took the request back; answered below like a dry pool. */
    }
    /* Answered here rather than dropped: a task asleep on a semaphore nothing
     * will signal is the one failure this AO must not cause. The stamp goes
     * with it, or the task would wait out its whole bound for an answer it
     * refused as somebody else's. */
    KLOG_W(TAG, "no room for a scan request; refused");
    scan_wait_refuse(wait, gen, KODE_NO_RESOURCES);
}
#endif /* RADIO_ASKS */

#if CONFIG_KODEOS_WIFI
kode_status_t wifi_scan_ask(QActive *who)
{
    return scan_ask(who, (uint8_t)KQP_SCAN_WIFI, (QSignal)WIFI_SCAN_RSP_SIG, 0U);
}

void wifi_scan_request_wait(ScanWaiter *wait, uint32_t gen)
{
    scan_request_wait(wait, gen, (uint8_t)KQP_SCAN_WIFI, 0U);
}

QEvt const *wifi_join_evt(const char *ssid, const char *pass, bool remember)
{
    WifiJoinEvt *e   = Q_NEW_X(WifiJoinEvt, 0U, WIFI_JOIN_REQ_SIG);
    QEvt const  *evt = NULL;

    if ( NULL != e )
    {
        e->ssid[0] = '\0';
        if ( NULL != ssid )
        {
            (void)snprintf(e->ssid, sizeof(e->ssid), "%s", ssid);
        }
        e->pass[0] = '\0';
        if ( NULL != pass )
        {
            (void)snprintf(e->pass, sizeof(e->pass), "%s", pass);
        }
        e->remember = remember;
        evt         = &e->super;
    }
    return evt;
}

bool wifi_join_request(const char *ssid, const char *pass, bool remember)
{
    QEvt const *e = wifi_join_evt(ssid, pass, remember);

    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; Wi-Fi join request dropped");
        return false;
    }
    /* QActive_post_ recycles the event itself when it refuses. */
    return QACTIVE_POST_X(AO_Radio, e, KQP_RADIO_POST_MARGIN, NULL);
}

QEvt const *wifi_leave_evt(void)
{
    static const QEvt evt = QEVT_INITIALIZER(WIFI_LEAVE_REQ_SIG);
    return &evt;
}

bool wifi_leave_request(void)
{
    return QACTIVE_POST_X(AO_Radio, wifi_leave_evt(), KQP_RADIO_POST_MARGIN, NULL);
}

void wifi_start_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(WIFI_START_REQ_SIG);
    QACTIVE_POST(AO_Radio, &evt, NULL);
}

void wifi_stop_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(WIFI_STOP_REQ_SIG);
    QACTIVE_POST(AO_Radio, &evt, NULL);
}

void wifi_state_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(WIFI_STATE_REQ_SIG);
    QACTIVE_POST(AO_Radio, &evt, NULL);
}

void wifi_fast_request(bool on)
{
    /* Static events, one per wish: nothing to allocate, so a wish to go back
     * to the default can never be lost to an empty pool. */
    static const WifiFastEvt fast_on  = { QEVT_INITIALIZER(WIFI_FAST_REQ_SIG), true };
    static const WifiFastEvt fast_off = { QEVT_INITIALIZER(WIFI_FAST_REQ_SIG), false };

    if ( on )
    {
        QACTIVE_POST(AO_Radio, &fast_on.super, NULL);
    }
    else
    {
        QACTIVE_POST(AO_Radio, &fast_off.super, NULL);
    }
}

void wifi_rssi_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(WIFI_RSSI_REQ_SIG);
    QACTIVE_POST(AO_Radio, &evt, NULL);
}

/** @brief The autojoin request, one static event for both ways of posting it. */
static const QEvt s_wifi_auto_evt = QEVT_INITIALIZER(WIFI_AUTO_REQ_SIG);

QEvt const *wifi_auto_evt(void)
{
    return &s_wifi_auto_evt;
}

bool wifi_auto_request(void)
{
    return QACTIVE_POST_X(AO_Radio, wifi_auto_evt(), KQP_RADIO_POST_MARGIN, NULL);
}

/** @brief AO_Radio asks itself to autojoin: the owner keeps no margin from its own queue. */
static void wifi_auto_self(void)
{
    QACTIVE_POST(AO_Radio, &s_wifi_auto_evt, NULL);
}

void wifi_list_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(WIFI_LIST_REQ_SIG);
    QACTIVE_POST(AO_Radio, &evt, NULL);
}

void wifi_forget_request(const char *ssid)
{
    WifiJoinEvt *e = Q_NEW_X(WifiJoinEvt, 0U, WIFI_FORGET_REQ_SIG);
    if ( NULL != e )
    {
        (void)snprintf(e->ssid, sizeof(e->ssid), "%s", ssid);
        e->pass[0] = '\0';   /* forgetting needs no secret */
        QACTIVE_POST(AO_Radio, &e->super, NULL);
    }
}
#endif /* CONFIG_KODEOS_WIFI */

#if CONFIG_KODEOS_BLE
void ble_start_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(BLE_START_REQ_SIG);
    QACTIVE_POST(AO_Radio, &evt, NULL);
}

bool ble_scan_request(uint32_t ms)
{
    BleScanReqEvt *e = Q_NEW_X(BleScanReqEvt, 0U, BLE_SCAN_REQ_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; BLE scan request dropped");
        return false;
    }
    e->ms = ms;
    QACTIVE_POST(AO_Radio, &e->super, NULL);
    return true;
}

/**
 * @brief Whether a listen of @p ms is one AO_Radio runs.
 * @param ms How long, in ms.
 * @return true inside KODE_BLE_SCAN_MIN_MS..KODE_BLE_SCAN_MAX_MS.
 */
static bool ble_scan_ms_ok(uint32_t ms)
{
    return (KODE_BLE_SCAN_MIN_MS <= ms) && (KODE_BLE_SCAN_MAX_MS >= ms);
}

kode_status_t ble_scan_ask(QActive *who, uint32_t ms)
{
    if ( !ble_scan_ms_ok(ms) )
    {
        return KODE_DENIED;
    }
    return scan_ask(who, (uint8_t)KQP_SCAN_BLE, (QSignal)BLE_SCAN_RSP_SIG, ms);
}

void ble_scan_request_wait(ScanWaiter *wait, uint32_t gen, uint32_t ms)
{
    if ( !ble_scan_ms_ok(ms) )
    {
        scan_wait_refuse(wait, gen, KODE_DENIED);
        return;
    }
    scan_request_wait(wait, gen, (uint8_t)KQP_SCAN_BLE, ms);
}

QEvt const *ble_rsp_evt(const char *name, const uint8_t *data, size_t len)
{
    const char   *who = "";
    BleRspReqEvt *e;
    QEvt const   *evt = NULL;

    if ( NULL != name )
    {
        who = name;
    }
    if ( (memchr(who, (int)'\0', KQP_BLE_NAME_MAX) == NULL)
        || (KQP_BLE_DATA_MAX < len) || ((0U < len) && (NULL == data)) )
    {
        return NULL;
    }
    e = Q_NEW_X(BleRspReqEvt, 0U, BLE_RSP_REQ_SIG);
    if ( NULL != e )
    {
        (void)snprintf(e->name, sizeof(e->name), "%s", who);
        e->len = (uint8_t)len;
        (void)memset(e->data, 0, sizeof(e->data));
        if ( 0U < len )
        {
            (void)memcpy(e->data, data, len);
        }
        evt = &e->super;
    }
    return evt;
}

QEvt const *ble_value_evt(const char *chr, bool notify, const void *data, size_t len,
                          BleValueWaiter *wait, uint32_t gen)
{
    BleAppValueReqEvt *e   = NULL;
    QEvt const        *evt = NULL;

    if ( (NULL != chr) && (memchr(chr, (int)'\0', KQP_BLE_APP_NAME_MAX) != NULL)
        && (KQP_BLE_APP_VALUE_MAX >= len) && ((0U == len) || (NULL != data)) )
    {
        e = Q_NEW_X(BleAppValueReqEvt, 0U, BLE_APP_VALUE_REQ_SIG);
    }
    if ( NULL != e )
    {
        e->wait   = wait;
        e->gen    = gen;
        e->notify = notify;
        (void)snprintf(e->name, sizeof(e->name), "%s", chr);
        e->len = (uint16_t)len;
        if ( 0U < len )
        {
            (void)memcpy(e->data, data, len);
        }
        evt = &e->super;
    }
    return evt;
}

/** @brief The `ble` flag as AO_Radio last wrote it, for ble_enabled(). */
static bool s_ble_on;

/**
 * @brief Publish the `ble` flag to ble_enabled()'s readers.
 * @param on What AO_Radio holds now.
 */
static void ble_on_note(bool on)
{
    /* cppcheck-suppress misra-c2012-17.3 ; GCC's __atomic builtins are the compiler's own, never declared in a header */
    __atomic_store_n(&s_ble_on, on, __ATOMIC_RELEASE);
}

bool ble_enabled(void)
{
    /* cppcheck-suppress misra-c2012-17.3 ; GCC's __atomic builtins are the compiler's own, never declared in a header */
    return __atomic_load_n(&s_ble_on, __ATOMIC_ACQUIRE);
}

void ble_stop_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(BLE_STOP_REQ_SIG);
    QACTIVE_POST(AO_Radio, &evt, NULL);
}

void ble_state_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(BLE_STATE_REQ_SIG);
    QACTIVE_POST(AO_Radio, &evt, NULL);
}

void ble_unpair_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(BLE_UNPAIR_REQ_SIG);
    QACTIVE_POST(AO_Radio, &evt, NULL);
}

void ble_pair_open_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(BLE_PAIR_OPEN_REQ_SIG);
    QACTIVE_POST(AO_Radio, &evt, NULL);
}

#if CONFIG_KODEOS_BLE_PAIRING_PASSKEY
void ble_pair_reject_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(BLE_PAIR_REJECT_REQ_SIG);
    QACTIVE_POST(AO_Radio, &evt, NULL);
}
#endif
#endif /* CONFIG_KODEOS_BLE */

void c5_flash_request(uint8_t src_kind, const char *src)
{
    FlashReqEvt *e = Q_NEW_X(FlashReqEvt, 0U, FLASH_REQ_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; C5 update request dropped");
        return;
    }
    e->src_kind = src_kind;
    e->dst_kind = (uint8_t)KFW_TARGET_C5;
    e->launch   = false;
    (void)snprintf(e->src, sizeof(e->src), "%s", src);
    QACTIVE_POST(AO_Radio, &e->super, NULL);
}

bool c5_recv_request(uint32_t size, uint32_t crc32, uint8_t src)
{
    AppRecvReqEvt *e = Q_NEW_X(AppRecvReqEvt, 0U, RADIO_RECV_REQ_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; install dst=c5 request dropped");
        return false;
    }
    e->dst    = (uint8_t)KFW_TARGET_C5;
    e->size   = size;
    e->crc32  = crc32;
    e->launch = false;
    e->src    = src;
    e->name[0] = '\0';
    QACTIVE_POST(AO_Radio, &e->super, NULL);
    return true;
}

void c5_test_request(void)
{
    static const RadioTestReqEvt evt = {
        QEVT_INITIALIZER(RADIO_TEST_REQ_SIG), false
    };
    QACTIVE_POST(AO_Radio, &evt.super, NULL);
}

void c5_test_request_user(void)
{
    static const RadioTestReqEvt evt = {
        QEVT_INITIALIZER(RADIO_TEST_REQ_SIG), true
    };
    QACTIVE_POST(AO_Radio, &evt.super, NULL);
}

void c5_field_request(QActive *who, uint8_t op, uint16_t field, int32_t value)
{
    RadioFieldReqEvt *e = Q_NEW_X(RadioFieldReqEvt, 0U, RADIO_FIELD_REQ_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; a c5 field request is dropped");
        return;
    }
    e->who   = who;
    e->op    = op;
    e->field = field;
    e->value = value;
    QACTIVE_POST(AO_Radio, &e->super, NULL);
}

void radio_amp_request(bool on)
{
    static const QEvt amp_on  = QEVT_INITIALIZER(C5_RUN_REQ_SIG);
    static const QEvt amp_off = QEVT_INITIALIZER(C5_AMP_OFF_SIG);

    if ( on )
    {
        QACTIVE_PUBLISH(&amp_on, NULL);
    }
    else
    {
        QACTIVE_PUBLISH(&amp_off, NULL);
    }
}

QEvt const *exp_power_evt(bool on)
{
    /* Static events, one per wish, as wifi_fast_request() does: an "off"
     * can never be lost to an empty pool. */
    static const ExpPowerEvt power_on  = { QEVT_INITIALIZER(EXP_POWER_REQ_SIG), true };
    static const ExpPowerEvt power_off = { QEVT_INITIALIZER(EXP_POWER_REQ_SIG), false };
    QEvt const              *evt       = &power_off.super;

    if ( on )
    {
        evt = &power_on.super;
    }
    return evt;
}

void exp_power_request(bool on)
{
    QACTIVE_POST(AO_Radio, exp_power_evt(on), NULL);
}

#if CONFIG_KODEOS_WIFI
/** @brief What the vault request in flight is for. */
enum {
    VOP_NONE = 0,   /**< Nothing asked.                                      */
    VOP_LIST,       /**< LIST: refresh the names, maybe answer wifi_list.    */
    VOP_REMEMBER,   /**< REMEMBER: a join that worked.                       */
    VOP_FORGET,     /**< FORGET.                                             */
    VOP_JOIN        /**< JOIN: the C5 joins a saved network.                 */
};
static void radio_vault_answer(Radio * const me, KodeSvcEvt const * const e);
static void radio_vault_lost(Radio * const me);
static bool radio_vault_send(Radio * const me, uint8_t op, uint8_t vop,
                             const void *payload, size_t len);
#endif

static void radio_park_answer(Radio * const me, uint8_t status);
static void radio_park_lost(Radio * const me);
static void radio_park_end(Radio * const me, bool parked, const char *why);

/* ---- The KODE services ------------------------------------------------------
 * The C5 keeps the secrets (kode_wire.h); this AO owns the C5, so every
 * request to them goes out from here and every answer comes back here. */

/** @brief How long a KODE request may go unanswered. */
#define RADIO_SVC_TMO_TICKS  KQP_TICKS_PER_MS(3000U)

/**
 * @brief A KODE service answered. esp-hosted's thread: copy into the
 *        service's buffer, post, return.
 */
static void radio_on_svc(uint8_t service, uint8_t op, uint8_t status,
                         const uint8_t *data, size_t len, void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the listener's context is this AO, as radio_svc_link() registered it */
    Radio *const me = (Radio *)ctx;
    size_t n = len;

    if ( (NULL == me) || (KODE_SVC_COUNT <= service)
        || (NULL == me->svc_buf[service]) )
    {
        return;
    }
    if ( (size_t)KODE_MAX_PAYLOAD < n )
    {
        n = (size_t)KODE_MAX_PAYLOAD;
    }
    if ( 0U < n )
    {
        (void)memcpy(me->svc_buf[service], data, n);
    }
    KodeSvcEvt *const e = Q_NEW_X(KodeSvcEvt, 0U, KODE_SVC_RSP_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events for a KODE answer (service %u)",
               (unsigned)service);
        return;
    }
    e->service = service;
    e->op      = op;
    e->status  = status;
    e->len     = (uint16_t)n;
    QACTIVE_POST(AO_Radio, &e->super, NULL);
}

/**
 * @brief The job as the service worker runs it: the driver's send.
 *
 * Runs on the service worker's task and touches only its argument, which is
 * one of the AO's @c svc_job slots and stays valid until the done event. The
 * driver keeps its own copy for the wire and wipes it; the slot's is wiped by
 * the done event.
 *
 * @param arg   The slot.
 * @param value Unused.
 * @return What the driver answered: KODE_OK once the C5 has taken the
 *         request, which is when the wait for its answer begins.
 */
/* cppcheck-suppress constParameterCallback ; KqpJobFn fixes the signature, and its argument is void * */
static kode_status_t radio_svc_job_run(void *arg, uint32_t *value)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the worker hands back the pointer it was given, which is the AO's job slot */
    RadioSvcJob const * const j = (RadioSvcJob const *)arg;

    *value = 0U;
    if ( j->cancelled )
    {
        return KODE_DENIED;
    }
    return kdrv_c5svc_send(j->service, j->op, j->buf, (size_t)j->len);
}

/**
 * @brief Hand one KODE request to the service worker, and start its
 *        deadline.
 *
 * Returns when the worker has the request, not when the C5 has: a refusal of
 * the link comes back in the done event (radio_svc_job_done()). Refused here:
 * a request of this service already waiting, a payload too long, a service
 * that is not there. The deadline is armed long enough for a send behind a
 * scan, and shortened to RADIO_SVC_TMO_TICKS once the send has returned.
 *
 * @param me      The AO.
 * @param service KODE_SVC_*.
 * @param op      Per service.
 * @param payload The request's payload, or NULL when @p len is 0. Copied.
 * @param len     0..KODE_MAX_PAYLOAD.
 * @return KODE_OK when the worker has it; KODE_DENIED for a bad argument or
 *         a request of this service already waiting; KODE_NO_RESOURCES when
 *         the worker or the payload's storage is not there, or is full.
 */
static kode_status_t radio_svc_send(Radio * const me, uint8_t service,
                                    uint8_t op, const void *payload, size_t len)
{
    RadioSvcJob *job  = NULL;
    uint8_t      slot = 0U;
    uint16_t     seq  = 0U;

    if ( !me->svc_worker_up )
    {
        return KODE_NO_RESOURCES;
    }
    if ( (KODE_SVC_COUNT <= service) || ((size_t)KODE_MAX_PAYLOAD < len)
        || ((NULL == payload) && (0U < len)) )
    {
        return KODE_DENIED;
    }
    if ( me->svc_wait[service] )
    {
        return KODE_DENIED;
    }
    for ( uint8_t k = 0U; (RADIO_SVC_SLOTS > k) && (NULL == job); k++ )
    {
        if ( !me->jobs->svc_job[service][k].inflight )
        {
            job  = &me->jobs->svc_job[service][k];
            slot = k;
        }
    }
    if ( NULL == job )
    {
        return KODE_NO_RESOURCES;   /* both slots are inside a stuck worker */
    }
    if ( NULL == job->buf )
    {
        return KODE_NO_RESOURCES;   /* before the first link took the storage */
    }
    if ( 0U < len )
    {
        (void)memcpy(job->buf, payload, len);
    }
    job->len       = (uint16_t)len;
    job->service   = service;
    job->op        = op;
    job->cancelled = false;
    if ( kqp_worker_submit(&me->svc_worker, &radio_svc_job_run, job,
                          RADIO_SVC_JOB_SIG, &seq) != KODE_OK )
    {
        (void)memset(job->buf, 0, len);
        return KODE_NO_RESOURCES;
    }
    job->seq               = seq;
    job->inflight          = true;
    me->svc_last[service]  = slot;
    me->svc_wait[service]  = true;
    (void)QTimeEvt_disarm(&me->svc_tmo[service]);
    QTimeEvt_armX(&me->svc_tmo[service],
                  KQP_TICKS_PER_MS(KODE_RADIO_SVC_SEND_GUARD_MS), 0U);
    return KODE_OK;
}

/**
 * @brief Withdraw the requests of @p svc that the service worker has not
 *        started: they are not sent, and come back refused.
 *
 * A request already inside the worker cannot be called back; its late return
 * answers nobody (radio_svc_job_done()).
 *
 * @param me  The AO.
 * @param svc The service, or KODE_SVC_COUNT for all of them.
 */
static void radio_svc_cancel(Radio * const me, uint8_t svc)
{
    if ( NULL == me->jobs )
    {
        return;
    }
    for ( uint8_t i = 0U; KODE_SVC_COUNT > i; i++ )
    {
        if ( (KODE_SVC_COUNT == svc) || (svc == i) )
        {
            for ( uint8_t k = 0U; RADIO_SVC_SLOTS > k; k++ )
            {
                if ( me->jobs->svc_job[i][k].inflight )
                {
                    me->jobs->svc_job[i][k].cancelled = true;
                }
            }
        }
    }
}

/**
 * @brief A KODE service's request came back from the service worker.
 *
 * The request may have been a password or a key: its copy is wiped. A send
 * the C5 took starts the wait for its answer, unless the answer already came
 * (it can beat this event). A send the link refused ends the request; the
 * caller tells whoever asked. A done event that is no request's, or an older
 * request's, is dropped.
 *
 * @param me The AO.
 * @param d  What the worker reports.
 * @return The service whose request could not be sent, or KODE_SVC_COUNT.
 */
static uint8_t radio_svc_job_settle(Radio * const me,
                                    KqpWorkerDoneEvt const * const d)
{
    RadioSvcJob *job    = NULL;
    uint8_t      svc    = 0U;
    uint8_t      slot   = 0U;
    uint8_t      failed = KODE_SVC_COUNT;

    if ( NULL == me->jobs )
    {
        return failed;
    }
    for ( uint8_t i = 0U; KODE_SVC_COUNT > i; i++ )
    {
        for ( uint8_t k = 0U; RADIO_SVC_SLOTS > k; k++ )
        {
            if ( me->jobs->svc_job[i][k].inflight && (me->jobs->svc_job[i][k].seq == d->seq) )
            {
                job  = &me->jobs->svc_job[i][k];
                svc  = i;
                slot = k;
            }
        }
    }
    if ( NULL == job )
    {
        KLOG_D(TAG, "service worker answer seq %u is not a request's; dropped",
               (unsigned)d->seq);
        return failed;
    }
    job->inflight = false;
    if ( NULL != job->buf )
    {
        (void)memset(job->buf, 0, job->len);
    }
    if ( (!me->svc_wait[svc]) || (me->svc_last[svc] != slot) )
    {
        return failed;   /* answered already, timed out or forgotten */
    }
    if ( (uint8_t)KODE_OK == d->status )
    {
        (void)QTimeEvt_disarm(&me->svc_tmo[svc]);
        QTimeEvt_armX(&me->svc_tmo[svc], RADIO_SVC_TMO_TICKS, 0U);
    }
    else
    {
        me->svc_wait[svc] = false;
        (void)QTimeEvt_disarm(&me->svc_tmo[svc]);
        KLOG_W(TAG, "KODE service %u: the request could not be sent "
                    "(status %u)", (unsigned)svc, (unsigned)d->status);
        failed = svc;
    }
    return failed;
}

static bool radio_submit(Radio * const me, kdrv_radio_job_t job);

/**
 * @brief Whether any KODE request is inside the service worker, from the
 *        submit to its done event.
 * @param me The AO.
 * @return true when one is.
 */
static bool radio_svc_busy(Radio const * const me)
{
    bool busy = false;

    if ( NULL != me->jobs )
    {
        for ( uint8_t i = 0U; KODE_SVC_COUNT > i; i++ )
        {
            for ( uint8_t k = 0U; RADIO_SVC_SLOTS > k; k++ )
            {
                if ( me->jobs->svc_job[i][k].inflight )
                {
                    busy = true;
                }
            }
        }
    }
    return busy;
}

/**
 * @brief A KODE service's request came back from the service worker; and if
 *        the last one has, the teardown that waited for it goes.
 *
 * esp_hosted_deinit() destroys the semaphore a send in esp-hosted's RPC wait
 * sleeps on, and the send destroys it again when it wakes. So the DISCONNECT
 * is held as owed while a send is inside the worker (radio_submit_req()) and
 * released here. Each send is bounded by esp-hosted's own 5 s response wait.
 *
 * @param me The AO.
 * @param d  What the worker reports.
 * @return The service whose request could not be sent, or KODE_SVC_COUNT.
 */
static uint8_t radio_svc_job_done(Radio * const me,
                                  KqpWorkerDoneEvt const * const d)
{
    uint8_t const failed = radio_svc_job_settle(me, d);

    radio_disc_release(me);
    return failed;
}

/** @brief The link is up: listen to every service and ask for INFO, once. */
static void radio_svc_link(Radio * const me)
{
    if ( me->info_asked )
    {
        return;
    }
    if ( kdrv_c5svc_init() != KODE_OK )
    {
        KLOG_E(TAG, "the KODE transport could not start");
        return;
    }
    for ( uint8_t svc = 0U; KODE_SVC_COUNT > svc; svc++ )
    {
        if ( NULL == me->svc_buf[svc] )
        {
            /* cppcheck-suppress misra-c2012-11.5 ; the allocator returns void *; the buffer is bytes */
            me->svc_buf[svc] = (uint8_t *)khal_alloc_psram(4U,
                                                (size_t)KODE_MAX_PAYLOAD);
        }
        for ( uint8_t k = 0U; RADIO_SVC_SLOTS > k; k++ )
        {
            if ( NULL == me->jobs->svc_job[svc][k].buf )
            {
                /* cppcheck-suppress misra-c2012-11.5 ; the allocator returns void *; the buffer is bytes */
                me->jobs->svc_job[svc][k].buf = (uint8_t *)khal_alloc_psram(4U,
                                                (size_t)KODE_MAX_PAYLOAD);
            }
        }
        (void)kdrv_c5svc_listen(svc, &radio_on_svc, me);
    }
    me->info_asked = true;
    if ( radio_svc_send(me, KODE_SVC_SELF, KODE_SELF_INFO, NULL, 0U) != KODE_OK )
    {
        KLOG_W(TAG, "KODE INFO could not be sent to the C5");
    }
#if CONFIG_KODEOS_WIFI
    /* The names the vault keeps, before an autojoin round needs them. */
    me->vlist_ok      = false;
    me->vlist_publish = false;
    (void)radio_vault_send(me, KODE_VAULT_LIST, (uint8_t)VOP_LIST, NULL, 0U);
#endif
}

/**
 * @brief The link is up: the board is named after its C5, whose base MAC the
 *        bring-up just read.
 */
static void radio_name_board(void)
{
    uint8_t mac[6];

    if ( kdrv_c5_mac(mac) != KODE_OK )
    {
        return;
    }
    if ( khal_identity_set_mac(mac) == KODE_IO_ERROR )
    {
        KLOG_W(TAG, "the C5's MAC could not be kept; the name lasts this boot");
    }
}

/** @brief A new link is starting: nothing asked on the old one will answer. */
static void radio_svc_forget(Radio * const me)
{
    kdrv_c5svc_forget();
    radio_svc_cancel(me, KODE_SVC_COUNT);
    for ( uint8_t svc = 0U; KODE_SVC_COUNT > svc; svc++ )
    {
        (void)QTimeEvt_disarm(&me->svc_tmo[svc]);
        me->svc_wait[svc] = false;
    }
    me->info_asked = false;
#if CONFIG_KODEOS_WIFI
    me->vault_op = (uint8_t)VOP_NONE;
#endif
}

/* ---- The unit's identity, held by the C5 ---------------------------------- */

/** @brief What the ID request in flight is (Radio.id_op). */
enum {
    IOP_NONE = 0,   /**< Nothing asked.                                      */
    IOP_SIGN,       /**< SIGN_AUTH: one ID_SIGN_RSP_SIG.                     */
    IOP_CERT,       /**< CERT_GET: parts of the pair, then the end.          */
    IOP_CSR,        /**< CSR: parts of the CSR, then the end.                */
    IOP_PUT,        /**< CERT_SET, after the last part: the end.             */
    IOP_LOCK        /**< LOCK: the end.                                      */
};

/** @brief Answer a signature request. */
static void id_reply_sign(QActive *who, uint8_t st, const uint8_t *der, size_t len)
{
    IdSignRspEvt *const e = Q_NEW_X(IdSignRspEvt, 0U, ID_SIGN_RSP_SIG);
    if ( (NULL == who) || (NULL == e) )
    {
        if ( NULL != e )
        {
            QF_gc(&e->super);
        }
        KLOG_E(TAG, "a signature answer could not be sent");
        return;
    }
    e->status  = st;
    e->der_len = 0U;
    if ( (KODE_ST_OK == st) && (sizeof(e->der) >= len) )
    {
        (void)memcpy(e->der, der, len);
        e->der_len = (uint8_t)len;
    }
    else if ( KODE_ST_OK == st )
    {
        e->status = KODE_ST_STORAGE;   /* a signature this long is no P-256 */
    }
    else
    {
        /* the reason is the answer */
    }
    if ( !QACTIVE_POST_X(who, &e->super, KQP_ID_ANSWER_MARGIN, AO_Radio) )
    {
        KLOG_E(TAG, "the signature's asker's queue is full; its answer is lost");
    }
}

/** @brief End an identity operation. */
static void id_reply_end(QActive *who, uint8_t st, uint16_t len, uint16_t unit_len)
{
    IdEndEvt *const e = Q_NEW_X(IdEndEvt, 0U, ID_END_SIG);
    if ( (NULL == who) || (NULL == e) )
    {
        if ( NULL != e )
        {
            QF_gc(&e->super);
        }
        KLOG_E(TAG, "an identity answer could not be sent");
        return;
    }
    e->status   = st;
    e->len      = len;
    e->unit_len = unit_len;
    if ( !QACTIVE_POST_X(who, &e->super, KQP_ID_ANSWER_MARGIN, AO_Radio) )
    {
        KLOG_E(TAG, "an identity asker's queue is full; its answer is lost");
    }
}

/**
 * @brief Hand DER back in KODE_ID_PART_BYTES pieces, in order.
 *
 * At most KODE_ID_PARTS_MAX of them and the end, twelve events in one step.
 * An asker below AO_Radio (AO_Net) cannot drain its queue meanwhile, so each
 * part is posted with KQP_ID_PART_MARGIN: a queue that fills stops the parts,
 * and the slot left over carries the end that says so. AO_Net keeps room for
 * all twelve under its inbound MQTT and its app traffic (KQP_NET_MQTT_MARGIN).
 *
 * @return false when a piece could not be allocated or queued; the end
 *         then says so.
 */
static bool id_reply_parts(QActive *who, const uint8_t *der, size_t len)
{
    size_t const of = (len + (size_t)KODE_ID_PART_BYTES - 1U)
                      / (size_t)KODE_ID_PART_BYTES;

    for ( size_t i = 0U; i < of; i++ )
    {
        size_t const at = i * (size_t)KODE_ID_PART_BYTES;
        size_t       n  = len - at;
        if ( (size_t)KODE_ID_PART_BYTES < n )
        {
            n = (size_t)KODE_ID_PART_BYTES;
        }
        IdPartEvt *const e = Q_NEW_X(IdPartEvt, 0U, ID_PART_SIG);
        if ( NULL == e )
        {
            return false;
        }
        e->n   = (uint8_t)(i + 1U);
        e->of  = (uint8_t)of;
        e->len = (uint8_t)n;
        (void)memcpy(e->der, &der[at], n);
        /* One free slot kept for the end, so the asker always hears it. */
        if ( !QACTIVE_POST_X(who, &e->super, KQP_ID_PART_MARGIN, AO_Radio) )
        {
            KLOG_W(TAG, "the asker's queue is full: part %u of %u not sent",
                   (unsigned)(i + 1U), (unsigned)of);
            return false;
        }
    }
    return true;
}

/** @brief Refuse an identity request at once, in its own reply's shape. */
static void id_refuse(QEvt const * const e, uint8_t st)
{
    if ( (QSignal)ID_SIGN_REQ_SIG == e->sig )
    {
        id_reply_sign(((IdSignReqEvt const *)e)->who, st, NULL, 0U);
    }
    else
    {
        id_reply_end(((IdReqEvt const *)e)->who, st, 0U, 0U);
    }
}

/**
 * @brief The length of the DER element at @p der: tag, length, contents.
 * @return 0 when it is not a SEQUENCE whose length fits @p cap.
 */
static size_t der_element_len(const uint8_t *der, size_t cap)
{
    size_t n = 0U;

    if ( (2U <= cap) && (0x30U == der[0]) )
    {
        if ( 0x80U > der[1] )
        {
            n = (size_t)der[1] + 2U;
        }
        else if ( (0x81U == der[1]) && (3U <= cap) )
        {
            n = (size_t)der[2] + 3U;
        }
        else if ( (0x82U == der[1]) && (4U <= cap) )
        {
            n = (((size_t)der[2] << 8) | (size_t)der[3]) + 4U;
        }
        else
        {
            n = 0U;
        }
    }
    if ( n > cap )
    {
        n = 0U;
    }
    return n;
}

/** @brief Send one ID request for @p who, or refuse it. */
static void radio_id_send(Radio * const me, QEvt const * const req, uint8_t iop,
                          QActive *who, uint8_t op, const void *payload, size_t len)
{
    if ( radio_svc_send(me, KODE_SVC_ID, op, payload, len) != KODE_OK )
    {
        id_refuse(req, KODE_ST_REFUSED);
        return;
    }
    me->id_op  = iop;
    me->id_who = who;
}

/**
 * @brief One part of the pair the factory is putting. Kept until the last,
 *        which goes to the C5 as one CERT_SET.
 */
static void radio_id_put(Radio * const me, IdReqEvt const * const q)
{
    kode_id_cert_t *c;

    if ( (0U == q->of) || ((uint8_t)KODE_ID_PARTS_MAX < q->of) || (0U == q->n)
        || (q->n > q->of) || (0U == q->len)
        || ((1U != q->n) && (q->n != me->id_next)) )
    {
        me->id_next = 0U;
        id_reply_end(q->who, KODE_ST_BAD_REQUEST, 0U, 0U);
        return;
    }
    if ( NULL == me->id_put )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; the allocator returns void *; the buffer is bytes */
        me->id_put = (uint8_t *)khal_alloc_psram(4U, sizeof(kode_id_cert_t));
        if ( NULL == me->id_put )
        {
            id_reply_end(q->who, KODE_ST_STORAGE, 0U, 0U);
            return;
        }
    }
    c = (kode_id_cert_t *)me->id_put;
    if ( 1U == q->n )
    {
        me->id_fill = 0U;
    }
    if ( ((size_t)me->id_fill + (size_t)q->len) > sizeof(c->der) )
    {
        me->id_next = 0U;
        id_reply_end(q->who, KODE_ST_BAD_REQUEST, 0U, 0U);
        return;
    }
    (void)memcpy(&c->der[me->id_fill], q->der, q->len);
    me->id_fill = (uint16_t)(me->id_fill + q->len);
    if ( q->n < q->of )
    {
        me->id_next = (uint8_t)(q->n + 1U);
        id_reply_end(q->who, KODE_ST_OK, me->id_fill, 0U);
        return;
    }

    /* The last part: the unit certificate is the first DER element, the
     * intermediate is whatever follows it. */
    me->id_next = 0U;
    size_t const unit = der_element_len(c->der, me->id_fill);
    if ( 0U == unit )
    {
        id_reply_end(q->who, KODE_ST_BAD_REQUEST, 0U, 0U);
        return;
    }
    c->unit_len  = (uint16_t)unit;
    c->chain_len = (uint16_t)(me->id_fill - unit);
    radio_id_send(me, &q->super, (uint8_t)IOP_PUT, q->who, KODE_ID_CERT_SET,
                  c, 4U + (size_t)me->id_fill);
}

/** @brief ID_SIGN_REQ_SIG or ID_REQ_SIG with the link up. */
static void radio_id_request(Radio * const me, QEvt const * const e)
{
    if ( (QSignal)ID_SIGN_REQ_SIG == e->sig )
    {
        IdSignReqEvt const * const q = (IdSignReqEvt const *)e;
        size_t const n = id_text_len(q->nonce, sizeof(q->nonce));
        kode_id_nonce_t w;
        if ( !kode_nonce_ok(q->nonce, n) )
        {
            id_reply_sign(q->who, KODE_ST_BAD_REQUEST, NULL, 0U);
            return;
        }
        if ( (uint8_t)IOP_NONE != me->id_op )
        {
            id_reply_sign(q->who, KODE_ST_REFUSED, NULL, 0U);
            return;
        }
        (void)memset(&w, 0, sizeof(w));
        w.len = (uint8_t)n;
        (void)memcpy(w.nonce, q->nonce, n);
        radio_id_send(me, e, (uint8_t)IOP_SIGN, q->who, KODE_ID_SIGN_AUTH,
                      &w, sizeof(w));
        return;
    }

    IdReqEvt const * const q = (IdReqEvt const *)e;
    if ( ((uint8_t)ID_OP_CERT_PUT == q->op) && (q->n < q->of) )
    {
        radio_id_put(me, q);   /* kept here; the C5 is not asked yet */
    }
    else if ( (uint8_t)IOP_NONE != me->id_op )
    {
        id_reply_end(q->who, KODE_ST_REFUSED, 0U, 0U);
    }
    else if ( (uint8_t)ID_OP_CERT == q->op )
    {
        radio_id_send(me, e, (uint8_t)IOP_CERT, q->who, KODE_ID_CERT_GET, NULL, 0U);
    }
    else if ( (uint8_t)ID_OP_CSR == q->op )
    {
        radio_id_send(me, e, (uint8_t)IOP_CSR, q->who, KODE_ID_CSR, NULL, 0U);
    }
    else if ( (uint8_t)ID_OP_CERT_PUT == q->op )
    {
        radio_id_put(me, q);
    }
    else if ( (uint8_t)ID_OP_LOCK == q->op )
    {
        radio_id_send(me, e, (uint8_t)IOP_LOCK, q->who, KODE_ID_LOCK, NULL, 0U);
    }
    else
    {
        id_reply_end(q->who, KODE_ST_BAD_REQUEST, 0U, 0U);
    }
}

/** @brief The ID service answered (KODE_SVC_RSP_SIG, service ID). */
static void radio_id_answer(Radio * const me, KodeSvcEvt const * const e)
{
    uint8_t const        iop = me->id_op;
    QActive *const       who = me->id_who;
    uint8_t const *const pay = me->svc_buf[KODE_SVC_ID];

    me->id_op  = (uint8_t)IOP_NONE;
    me->id_who = NULL;
    if ( (uint8_t)IOP_SIGN == iop )
    {
        id_reply_sign(who, e->status, pay, e->len);
    }
    else if ( ((uint8_t)IOP_CERT == iop) && (KODE_ST_OK == e->status) )
    {
        kode_id_cert_t const *const c = (kode_id_cert_t const *)pay;
        size_t const total = (size_t)c->unit_len + (size_t)c->chain_len;
        if ( (4U > e->len) || (((size_t)e->len) != (4U + total))
            || (sizeof(c->der) < total) )
        {
            id_reply_end(who, KODE_ST_STORAGE, 0U, 0U);
        }
        else if ( !id_reply_parts(who, c->der, total) )
        {
            id_reply_end(who, KODE_ST_REFUSED, 0U, 0U);
        }
        else
        {
            id_reply_end(who, KODE_ST_OK, (uint16_t)total, c->unit_len);
        }
    }
    else if ( ((uint8_t)IOP_CSR == iop) && (KODE_ST_OK == e->status) )
    {
        if ( !id_reply_parts(who, pay, e->len) )
        {
            id_reply_end(who, KODE_ST_REFUSED, 0U, 0U);
        }
        else
        {
            id_reply_end(who, KODE_ST_OK, e->len, 0U);
        }
    }
    else if ( (uint8_t)IOP_NONE != iop )
    {
        id_reply_end(who, e->status, 0U, 0U);
    }
    else
    {
        /* an answer nobody here asked for */
    }
}

/** @brief The ID request in flight will not be answered: say so. */
static void radio_id_lost(Radio * const me)
{
    uint8_t const  iop = me->id_op;
    QActive *const who = me->id_who;

    me->id_op   = (uint8_t)IOP_NONE;
    me->id_who  = NULL;
    me->id_next = 0U;
    if ( (uint8_t)IOP_SIGN == iop )
    {
        id_reply_sign(who, KODE_ST_REFUSED, NULL, 0U);
    }
    else if ( (uint8_t)IOP_NONE != iop )
    {
        id_reply_end(who, KODE_ST_REFUSED, 0U, 0U);
    }
    else
    {
        /* nothing was asked */
    }
}

static bool radio_submit_req(Radio * const me, kdrv_radio_req_t const *req);

/* ---- c5_wipe ---------------------------------------------------------------
 * The C5's vault, then its BLE keys, then this chip's bond store: each step
 * starts only when the one before it answered OK, and the asker hears once,
 * at the end or at the step that failed. */

/** @brief Answer a wipe. */
static void wipe_reply(QActive *who, uint8_t step, uint8_t st)
{
    C5WipeRspEvt *const e = Q_NEW_X(C5WipeRspEvt, 0U, C5_WIPE_RSP_SIG);
    if ( (NULL == who) || (NULL == e) )
    {
        if ( NULL != e )
        {
            QF_gc(&e->super);
        }
        KLOG_E(TAG, "a wipe's answer could not be sent");
        return;
    }
    e->step   = step;
    e->status = st;
    if ( !QACTIVE_POST_X(who, &e->super, KQP_ID_ANSWER_MARGIN, AO_Radio) )
    {
        KLOG_E(TAG, "the wipe's asker's queue is full; its answer is lost");
    }
}

/** @brief The wipe is over, at @p step. */
static void radio_wipe_end(Radio * const me, uint8_t step, uint8_t st)
{
    QActive *const who = me->wipe_who;

    me->wiping   = false;
    me->wipe_who = NULL;
    if ( (uint8_t)C5_WIPE_DONE == step )
    {
        KLOG_N(TAG, "wipe: the C5's networks and BLE keys and this chip's "
                    "bonds are gone; the identity stays");
    }
    else
    {
        KLOG_W(TAG, "wipe: stopped at step %u (status %u)", (unsigned)step,
               (unsigned)st);
    }
    wipe_reply(who, step, st);
}

/** @brief Start the next step, or end the wipe if it cannot start. */
static void radio_wipe_step(Radio * const me, uint8_t step)
{
    me->wipe_step = step;
    if ( (uint8_t)C5_WIPE_VAULT == step )
    {
        if ( radio_svc_send(me, KODE_SVC_VAULT, KODE_VAULT_WIPE, NULL, 0U) != KODE_OK )
        {
            radio_wipe_end(me, step, KODE_ST_REFUSED);
        }
    }
    else if ( (uint8_t)C5_WIPE_BTKEYS == step )
    {
        if ( radio_svc_send(me, KODE_SVC_BTKEYS, KODE_BTKEYS_WIPE, NULL, 0U) != KODE_OK )
        {
            radio_wipe_end(me, step, KODE_ST_REFUSED);
        }
    }
#if CONFIG_KODEOS_BLE
    else if ( (uint8_t)C5_WIPE_BLE == step )
    {
        /* The same guards as `ble_unpair`: a console attached would keep a
         * link whose key is gone, and the worker takes one job at a time. */
        if ( kdrv_ble_console_attached() || me->busy
            || !radio_submit(me, KDRV_RADIO_JOB_BLE_UNPAIR) )
        {
            radio_wipe_end(me, step, KODE_ST_REFUSED);
        }
    }
#endif
    else
    {
        radio_wipe_end(me, (uint8_t)C5_WIPE_DONE, KODE_ST_OK);
    }
}

/** @brief C5_WIPE_REQ_SIG with the link up. */
static void radio_wipe_start(Radio * const me, C5WipeReqEvt const * const q)
{
    bool busy = me->wiping;
#if CONFIG_KODEOS_WIFI
    busy = busy || ((uint8_t)VOP_NONE != me->vault_op);
#endif
    if ( busy )
    {
        wipe_reply(q->who, (uint8_t)C5_WIPE_VAULT, KODE_ST_REFUSED);
        return;
    }
    me->wiping   = true;
    me->wipe_who = q->who;
    radio_wipe_step(me, (uint8_t)C5_WIPE_VAULT);
}

/**
 * @brief A KODE answer, if it is the wipe's.
 * @return true when the wipe took it.
 */
static bool radio_wipe_answer(Radio * const me, KodeSvcEvt const * const e)
{
    bool mine = false;

    if ( me->wiping && ((uint8_t)C5_WIPE_VAULT == me->wipe_step)
        && (KODE_SVC_VAULT == e->service) )
    {
        mine = true;
    }
    if ( me->wiping && ((uint8_t)C5_WIPE_BTKEYS == me->wipe_step)
        && (KODE_SVC_BTKEYS == e->service) )
    {
        mine = true;
    }
    if ( !mine )
    {
        return false;
    }
    if ( KODE_ST_OK != e->status )
    {
        radio_wipe_end(me, me->wipe_step, e->status);
        return true;
    }
#if CONFIG_KODEOS_WIFI
    if ( (uint8_t)C5_WIPE_VAULT == me->wipe_step )
    {
        me->vlist.count = 0U;   /* the names went with the networks */
        me->vlist_ok    = true;
    }
#endif
    radio_wipe_step(me, (uint8_t)(me->wipe_step + 1U));
    return true;
}

/**
 * @brief The step in flight will not be answered (a timeout on @p svc, or
 *        the link going away when @p svc is KODE_SVC_COUNT).
 */
static void radio_wipe_lost(Radio * const me, uint8_t svc)
{
    bool lost = me->wiping && (KODE_SVC_COUNT == svc);

    if ( me->wiping && (KODE_SVC_VAULT == svc)
        && ((uint8_t)C5_WIPE_VAULT == me->wipe_step) )
    {
        lost = true;
    }
    if ( me->wiping && (KODE_SVC_BTKEYS == svc)
        && ((uint8_t)C5_WIPE_BTKEYS == me->wipe_step) )
    {
        lost = true;
    }
    if ( lost )
    {
        radio_wipe_end(me, me->wipe_step, KODE_ST_REFUSED);
    }
}

/** @brief A KODE answer arrived (KODE_SVC_RSP_SIG). */
static void radio_svc_answer(Radio * const me, KodeSvcEvt const * const e)
{
    if ( KODE_SVC_COUNT <= e->service )
    {
        return;
    }
    (void)QTimeEvt_disarm(&me->svc_tmo[e->service]);
    me->svc_wait[e->service] = false;
    if ( radio_wipe_answer(me, e) )
    {
        /* a step of c5_wipe */
    }
    else if ( (KODE_SVC_SELF == e->service) && (KODE_SELF_INFO == e->op) )
    {
        kode_info_t i;
        if ( (KODE_ST_OK == e->status) && kdrv_c5svc_info(&i) )
        {
            KLOG_N(TAG, "C5: KODE services v%u, %u networks, %u BLE keys, "
                   "identity 0x%02x; heap %lu B free, lowest %lu B, "
                   "largest %lu B",
                   (unsigned)i.wire_version, (unsigned)i.vault_count,
                   (unsigned)i.btkeys_count, (unsigned)i.id_flags,
                   (unsigned long)i.heap_free, (unsigned long)i.heap_low,
                   (unsigned long)i.heap_largest);
        }
        else
        {
            KLOG_W(TAG, "C5: KODE INFO refused (status %u)",
                   (unsigned)e->status);
        }
    }
    else if ( (KODE_SVC_SELF == e->service) && (KODE_SELF_PARK == e->op) )
    {
        radio_park_answer(me, e->status);
    }
    else if ( KODE_SVC_ID == e->service )
    {
        radio_id_answer(me, e);
    }
#if CONFIG_KODEOS_WIFI
    else if ( KODE_SVC_VAULT == e->service )
    {
        radio_vault_answer(me, e);
    }
#endif
    else
    {
        /* a service whose answers nobody here asks for yet */
    }
}

/**
 * @brief A KODE request of @p svc will not be answered: say so to whoever
 *        waits on it.
 *
 * Nothing happens for KODE_SVC_COUNT, which is how radio_svc_job_done()
 * says that no request failed.
 *
 * @param me  The AO.
 * @param svc The service.
 */
static void radio_svc_lost(Radio * const me, uint8_t svc)
{
    if ( KODE_SVC_COUNT <= svc )
    {
        return;
    }
    if ( KODE_SVC_ID == svc )
    {
        radio_id_lost(me);
    }
    if ( (KODE_SVC_SELF == svc) && ((uint8_t)PARK_ASKED == me->park_phase) )
    {
        /* The park's send failed inside the worker: nobody will answer it,
         * and AO_Boot is waiting for the answer. */
        radio_park_end(me, false, "the C5 could not be asked");
    }
    radio_wipe_lost(me, svc);
#if CONFIG_KODEOS_WIFI
    if ( KODE_SVC_VAULT == svc )
    {
        radio_vault_lost(me);
    }
#endif
}

/** @brief A KODE request went unanswered (KODE_SVC_TMO_SIG). */
static void radio_svc_timeout(Radio * const me, QEvt const * const e)
{
    for ( uint8_t svc = 0U; KODE_SVC_COUNT > svc; svc++ )
    {
        if ( e == &me->svc_tmo[svc].super )
        {
            if ( !me->svc_wait[svc] )
            {
                /* The expiry of a request that was answered or forgotten
                 * after the timer had fired: nothing waits on it. */
                continue;
            }
            me->svc_wait[svc] = false;
            radio_svc_cancel(me, svc);
            kdrv_c5svc_abandon(svc);
            if ( (KODE_SVC_SELF == svc)
                && ((uint8_t)PARK_ASKED == me->park_phase) )
            {
                radio_park_lost(me);
            }
            else if ( KODE_SVC_SELF == svc )
            {
                KLOG_W(TAG, "the C5 has no KODE services (no INFO answer): "
                       "vault, identity and BLE keys are refused");
            }
            else
            {
                KLOG_W(TAG, "KODE service %u did not answer", (unsigned)svc);
            }
            radio_svc_lost(me, svc);
        }
    }
}

/* ---- Plumbing ------------------------------------------------------------ */

/** Runs on the radio worker (a job that failed over a mute C5) or on
 *  esp-hosted's event task (the transport dropped): wrap the news into an
 *  event and post it. Nothing else. */
static void radio_on_link_lost(kode_status_t why)
{
    RadioLostEvt *e = Q_NEW_X(RadioLostEvt, 0U, RADIO_LINK_LOST_SIG);
    if ( NULL != e )
    {
        e->status = (uint8_t)why;
        QACTIVE_POST(AO_Radio, &e->super, NULL);
    }
    else
    {
        /* Bounded, but say so loudly: without this event the state machine
         * would wait forever for a link that is already gone. */
        KLOG_E(TAG, "out of events for a lost link; radio state stuck");
    }
}

#if CONFIG_KODEOS_WIFI
/** Runs on the system event task: the network in use went away by itself.
 *  With a margin, as every door from outside this AO: a queue with no room
 *  loses the news, and radio_settle() finds the station off the network. */
static void radio_on_wifi_lost(void)
{
    static const QEvt evt = QEVT_INITIALIZER(WIFI_LOST_SIG);

    if ( !QACTIVE_POST_X(AO_Radio, &evt, KQP_RADIO_POST_MARGIN, NULL) )
    {
        KLOG_W(TAG, "no room to say the network went away");
    }
}

/** Runs on the radio worker, inside a scan: say a channel is in and
 *  return. Static event, so this cannot fail and costs no pool block. */
static void radio_on_scan_more(void)
{
    static const QEvt evt = QEVT_INITIALIZER(RADIO_SCAN_MORE_SIG);
    QACTIVE_POST(AO_Radio, &evt, NULL);
}
#endif

/** @brief Publish the link state (bounded: a full pool drops it). */
static void radio_publish(Radio * const me, kradio_link_t link)
{
    me->link = (uint8_t)link;
    RadioStateEvt *e = Q_NEW_X(RadioStateEvt, 0U, RADIO_STATE_SIG);
    if ( NULL != e )
    {
        kdrv_radio_fwver_t fw;
        e->link      = me->link;
        e->fw_known  = (kdrv_radio_fwversion(&fw) == KODE_OK);
        e->fw_major  = e->fw_known ? fw.major : 0U;
        e->fw_minor  = e->fw_known ? fw.minor : 0U;
        e->fw_patch  = e->fw_known ? fw.patch : 0U;
        e->sd_hosted = (KRADIO_UP == link);
        e->gave_up   = me->relink_dead;
        QACTIVE_PUBLISH(&e->super, &me->super);
    }
}

#if RADIO_ASKS
/* ---- The asked scan ------------------------------------------------------
 * One slot for both kinds, the Wi-Fi sweep and the BLE listen: the channel
 * carries one job, so two asked scans could never run together anyway, and
 * one slot is what keeps that a single rule.
 */

/**
 * @brief What an asked scan is called in the log.
 * @param kind KQP_SCAN_WIFI or KQP_SCAN_BLE.
 * @return "Wi-Fi scan" or "BLE scan".
 */
static const char *scan_ask_name(uint8_t kind)
{
    const char *name = "Wi-Fi scan";

    if ( (uint8_t)KQP_SCAN_BLE == kind )
    {
        name = "BLE scan";
    }
    return name;
}

/**
 * @brief Copy what the scan kept into a blocked task's rows.
 * @param kind KQP_SCAN_WIFI (kdrv_wifi_ap_t rows) or KQP_SCAN_BLE
 *             (kdrv_ble_dev_t rows).
 * @param w    The waiter, its @c rows and @c cap set.
 * @return Rows written.
 */
static uint8_t scan_rows_copy(uint8_t kind, ScanWaiter *w)
{
    uint8_t got = 0U;

#if CONFIG_KODEOS_WIFI
    if ( (uint8_t)KQP_SCAN_WIFI == kind )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; ScanWaiter.rows is kdrv_wifi_ap_t[cap] for a Wi-Fi scan, untyped because kqp cannot name kdrv types */
        kdrv_wifi_ap_t *const rows  = (kdrv_wifi_ap_t *)w->rows;
        uint8_t const         count = kdrv_wifi_scan_count();

        for ( uint8_t i = 0U; (i < count) && (got < w->cap); ++i )
        {
            if ( kdrv_wifi_scan_get(i, &rows[got]) )
            {
                ++got;
            }
        }
    }
#endif
#if CONFIG_KODEOS_BLE
    if ( (uint8_t)KQP_SCAN_BLE == kind )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; ScanWaiter.rows is kdrv_ble_dev_t[cap] for a BLE scan, untyped because kqp cannot name kdrv types */
        kdrv_ble_dev_t *const rows  = (kdrv_ble_dev_t *)w->rows;
        uint8_t const         count = kdrv_ble_scan_count();

        for ( uint8_t i = 0U; (i < count) && (got < w->cap); ++i )
        {
            if ( kdrv_ble_scan_get(i, &rows[got]) )
            {
                ++got;
            }
        }
    }
#endif
    return got;
}

/**
 * @brief Answer a blocked task's scan in its waiter.
 * @param w    The waiter.
 * @param gen  The request's tag, stamped just before the signal.
 * @param kind What the rows are.
 * @param rc   The answer.
 * @param copy Copy the rows the scan kept into the waiter first.
 */
static void scan_ask_wait_answer(ScanWaiter *w, uint32_t gen, uint8_t kind,
                                 kode_status_t rc, bool copy)
{
    uint8_t got = 0U;

    if ( copy )
    {
        got = scan_rows_copy(kind, w);
    }
    w->rc  = (uint8_t)rc;
    w->got = got;
    scan_ack_write(&w->ack, gen);
    khal_sem_signal(w->done);
}

/**
 * @brief Answer an AO's scan: its door opens, then the end is posted.
 * @param kind The scan's kind, whose door it is.
 * @param who  The asking AO.
 * @param rsp  Its end, taken with the request.
 * @param seen Networks or devices handed over.
 * @param rc   As ScanRspEvt.rc.
 * @param more Entries were left out.
 */
static void scan_ask_ao_answer(uint8_t kind, QActive *who, ScanRspEvt *rsp, uint16_t seen,
                               kode_status_t rc, bool more)
{
    rsp->seen = (uint8_t)SCAN_SEEN_MAX;
    if ( SCAN_SEEN_MAX > seen )
    {
        rsp->seen = (uint8_t)seen;
    }
    rsp->rc   = (uint8_t)rc;
    rsp->more = more;
    kaos_ask_answer(scan_door(kind), who, &rsp->super, AO_Radio);
}

/**
 * @brief Refuse an asked scan at once, in its own shape.
 * @param e  The RADIO_SCAN_ASK_SIG.
 * @param rc KODE_NO_RESOURCES, the radio is busy, ask again; KODE_DENIED,
 *           Bluetooth is off in Settings.
 */
static void scan_ask_refuse(QEvt const * const e, kode_status_t rc)
{
    ScanAskEvt const * const a = Q_EVT_CAST(ScanAskEvt);

    if ( NULL != a->wait )
    {
        scan_ask_wait_answer(a->wait, a->gen, a->kind, rc, false);
    }
    else
    {
        scan_ask_ao_answer(a->kind, a->ask, a->rsp, 0U, rc, false);
    }
}

/**
 * @brief Refuse an asked BLE listen with Bluetooth off in Settings, in any state.
 *
 * Read before anything else that refuses an asked scan, so the asker is told
 * KODE_DENIED, which stays true, and never KODE_NO_RESOURCES for a listen
 * that asking again would only get refused once more.
 *
 * @param me The AO.
 * @param e  The RADIO_SCAN_ASK_SIG.
 * @return true when it was refused, KODE_DENIED; false when it may go on.
 */
static bool scan_ask_refuse_off(Radio const * const me, QEvt const * const e)
{
    ScanAskEvt const * const a   = Q_EVT_CAST(ScanAskEvt);
    bool                     off = false;

    if ( ((uint8_t)KQP_SCAN_BLE == a->kind) && !me->ble_want )
    {
        KLOG_N(TAG, "Bluetooth is off in Settings; a BLE scan is refused");
        scan_ask_refuse(e, KODE_DENIED);
        off = true;
    }
    return off;
}

/**
 * @brief Whether the channel is held by something an asked scan is never owed behind.
 *
 * A console scan of either kind (the autojoin round is a state of its own
 * and refuses there), the production row or an install, or an update: each
 * holds the channel for tens of seconds, and a scan owed behind one would
 * outlast its caller's wait. Only the link's bring-up and the reconcile
 * chain, which take seconds, are waited for.
 *
 * @param me The AO.
 * @return true when an asked scan is refused instead of owed.
 */
static bool scan_ask_blocked(Radio const * const me)
{
    return me->scan_live
           || me->ble_scan_live
           || ((uint8_t)TEST_IDLE != me->test_phase)
           || ((uint8_t)INST_IDLE != me->inst)
           || ((uint8_t)OTA_IDLE != me->phase);
}

/**
 * @brief Take an asked scan into the one slot, or refuse it at once.
 * @param me The AO.
 * @param e  The RADIO_SCAN_ASK_SIG.
 * @return true when it is held, owed; false when it was answered already.
 */
static bool scan_ask_take(Radio * const me, QEvt const * const e)
{
    ScanAskEvt const * const a = Q_EVT_CAST(ScanAskEvt);

    if ( scan_ask_refuse_off(me, e) )
    {
        return false;
    }
    if ( (uint8_t)SCAN_ASK_NONE != me->ask_scan )
    {
        KLOG_N(TAG, "a scan is already asked for; this %s is refused",
               scan_ask_name(a->kind));
        scan_ask_refuse(e, KODE_NO_RESOURCES);
        return false;
    }
    if ( scan_ask_blocked(me) )
    {
        KLOG_N(TAG, "the radio is busy with a scan, a test or an install; "
                    "a %s is refused", scan_ask_name(a->kind));
        scan_ask_refuse(e, KODE_NO_RESOURCES);
        return false;
    }
    me->ask_scan = (uint8_t)SCAN_ASK_OWED;
    me->ask_kind = a->kind;
    me->ask_ms   = a->ms;
    me->ask_who  = a->ask;
    me->ask_rsp  = a->rsp;
    me->ask_wait = a->wait;
    me->ask_gen  = a->gen;
    me->ask_seen = 0U;
    me->ask_cut  = false;
    return true;
}

/**
 * @brief Answer the held scan and empty the slot.
 * @param me The AO.
 * @param rc KODE_OK, the scan reached its end; KODE_ERROR, it failed or the
 *           link went; KODE_NO_RESOURCES, the radio turned out busy;
 *           KODE_DENIED, Bluetooth went off in Settings before a BLE scan ran.
 */
static void scan_ask_end(Radio * const me, kode_status_t rc)
{
    if ( (uint8_t)SCAN_ASK_NONE == me->ask_scan )
    {
        return;
    }
    /* Emptied first: the answer opens the asker's door, and its next ask
     * must find the slot free. */
    me->ask_scan = (uint8_t)SCAN_ASK_NONE;
    if ( NULL != me->ask_wait )
    {
        scan_ask_wait_answer(me->ask_wait, me->ask_gen, me->ask_kind, rc, KODE_OK == rc);
    }
    else
    {
        scan_ask_ao_answer(me->ask_kind, me->ask_who, me->ask_rsp, me->ask_seen, rc,
                           me->ask_cut);
    }
    me->ask_who  = NULL;
    me->ask_rsp  = NULL;
    me->ask_wait = NULL;
}

/**
 * @brief Refuse the held scan if it is still owed: a scan, a test row or an
 *        install is about to take the channel, and it is never owed behind one.
 * @param me The AO.
 */
static void scan_ask_refuse_owed(Radio * const me)
{
    if ( (uint8_t)SCAN_ASK_OWED == me->ask_scan )
    {
        KLOG_N(TAG, "the radio is busy with a scan, a test or an install; "
                    "the asked %s is refused", scan_ask_name(me->ask_kind));
        scan_ask_end(me, KODE_NO_RESOURCES);
    }
}

/**
 * @brief Whether a request that waited for the radio can be made now.
 * @param me The AO.
 * @return true when the reconcile is over and no job, update, install or row
 *         holds the channel.
 */
static bool scan_channel_free(Radio const * const me)
{
    return !me->busy
           && ((uint8_t)RECON_DONE == me->recon)
           && ((uint8_t)OTA_IDLE == me->phase)
           && ((uint8_t)INST_IDLE == me->inst)
           && ((uint8_t)TEST_IDLE == me->test_phase);
}

/**
 * @brief Hand the held scan's job to the worker.
 * @param me The AO.
 * @return true when it is submitted.
 */
static bool scan_ask_submit(Radio * const me)
{
    kdrv_radio_req_t req;

    (void)memset(&req, 0, sizeof(req));
    req.type = KDRV_RADIO_JOB_BLE_SCAN;
    req.len  = me->ask_ms;
#if CONFIG_KODEOS_WIFI
    if ( (uint8_t)KQP_SCAN_WIFI == me->ask_kind )
    {
        req.type = KDRV_RADIO_JOB_WIFI_SCAN;
        req.len  = 0U;
    }
#endif
    return radio_submit_req(me, &req);
}

/**
 * @brief Start the held scan if it can start; a refused job ends it.
 * @param me The AO.
 * @return true when its job is on the worker.
 */
static bool scan_ask_start(Radio * const me)
{
    if ( (uint8_t)SCAN_ASK_OWED != me->ask_scan )
    {
        return false;
    }
    if ( scan_ask_blocked(me) )
    {
        /* Something it is never owed behind took the channel meanwhile. */
        scan_ask_refuse_owed(me);
        return false;
    }
    if ( ((uint8_t)KQP_SCAN_BLE == me->ask_kind) && !me->ble_want )
    {
        KLOG_N(TAG, "Bluetooth went off in Settings; the asked BLE scan is refused");
        scan_ask_end(me, KODE_DENIED);
        return false;
    }
    if ( !scan_channel_free(me) )
    {
        return false;
    }
    if ( !scan_ask_submit(me) )
    {
        scan_ask_end(me, KODE_NO_RESOURCES);
        return false;
    }
    me->ask_scan = (uint8_t)SCAN_ASK_RUNNING;
    KLOG_I(TAG, "a %s for an asker", scan_ask_name(me->ask_kind));
    return true;
}

/**
 * @brief Whether the held scan's job is the one on the worker, and of @p kind.
 * @param me   The AO.
 * @param kind KQP_SCAN_WIFI or KQP_SCAN_BLE.
 * @return true when it runs.
 */
static bool scan_ask_running(Radio const * const me, uint8_t kind)
{
    return ((uint8_t)SCAN_ASK_RUNNING == me->ask_scan) && (me->ask_kind == kind);
}
#endif /* RADIO_ASKS */

#if CONFIG_KODEOS_WIFI
/**
 * @brief Whether a join somebody asked for has yet to end.
 *
 * Waiting behind another job, or between the steps of its own: readied, with
 * the vault, or waiting for the address. Whatever is published meanwhile (a
 * refresh of the strength, say) is not what became of it. The steps of an
 * automatic round are nobody's request.
 *
 * @param me The AO.
 * @return true while the join's answer is still to come.
 */
static bool wifi_join_out(Radio const * const me)
{
    bool out = ((uint8_t)WOWED_JOIN == me->wifi_owed);

    if ( !me->autojoin )
    {
        if ( (uint8_t)VOP_JOIN == me->vault_op )
        {
            out = true;
        }
        else if ( me->winflight && (NULL != me->jobs) )
        {
            kdrv_radio_job_t const job = me->jobs->wreq.type;

            out = out || (KDRV_RADIO_JOB_WIFI_JOIN == job)
                  || (KDRV_RADIO_JOB_WIFI_PREPARE == job)
                  || (KDRV_RADIO_JOB_WIFI_AWAIT == job);
        }
        else
        {
            /* Nothing of it is out. */
        }
    }
    return out;
}

/** @brief Publish where the connection stands, from the driver's own view. */
static void wifi_publish_state(Radio * const me)
{
    WifiStateEvt *e = Q_NEW_X(WifiStateEvt, 0U, WIFI_STATE_SIG);
    if ( NULL != e )
    {
        uint32_t ip = 0U;
        e->state   = (uint8_t)kdrv_wifi_state(&ip);
        e->ip4     = ip;
        kdrv_wifi_ssid_copy(e->ssid, sizeof(e->ssid));
        e->rssi    = kdrv_wifi_rssi_last();
        e->enabled = me->wifi_want;
        e->fail    = (uint8_t)kdrv_wifi_fail_last();
        e->joining = wifi_join_out(me);
        e->reconnecting = me->retry_on;
        QACTIVE_PUBLISH(&e->super, &me->super);
    }
}

/**
 * @brief Whether the board should be working on getting back on a network.
 * @param me The AO.
 * @return true with Wi-Fi on and off any network, automatic joins allowed, the
 *         person not having stepped off, and something remembered - or a
 *         vault not heard from yet, which the round finds out about.
 */
static bool wifi_retry_wanted(Radio const * const me)
{
    return me->wifi_want && me->join_auto && (!me->wifi_left)
           && (!(me->vlist_ok && (0U == me->vlist.count)))
           && (kdrv_wifi_state(NULL) != KDRV_WIFI_GOT_IP);
}

/**
 * @brief Nothing to get back on, or nobody wants it: no wait, and the next one
 *        starts from the first.
 * @param me The AO.
 */
static void wifi_retry_stop(Radio * const me)
{
    (void)QTimeEvt_disarm(&me->wifi_retry);
    me->retry_step = 0U;
    me->retry_on   = false;
    me->retry_owed = false;
}

/**
 * @brief Start the next wait, or stop waiting when it is no longer wanted.
 * @param me The AO.
 */
static void wifi_retry_arm(Radio * const me)
{
    /* The waits before the next try at getting back on a network, in ms. The
     * last one repeats. */
    static const uint32_t RADIO_RETRY_MS[] = { 5000U, 30000U, 120000U, 600000U };
    /* How many waits RADIO_RETRY_MS holds. */
    uint8_t const steps = (uint8_t)(sizeof(RADIO_RETRY_MS) / sizeof(RADIO_RETRY_MS[0]));

    if ( wifi_retry_wanted(me) )
    {
        (void)QTimeEvt_disarm(&me->wifi_retry);
        QTimeEvt_armX(&me->wifi_retry,
                      KQP_TICKS_PER_MS(RADIO_RETRY_MS[me->retry_step]), 0U);
        if ( (uint8_t)(me->retry_step + 1U) < steps )
        {
            ++me->retry_step;
        }
        me->retry_on = true;
    }
    else
    {
        wifi_retry_stop(me);
    }
}

/**
 * @brief Start the round that is due, if the board and the channel allow.
 *
 * A request somebody made goes first: a posted round would take the place of
 * one that waits. What cannot start now stays owed, and radio_settle() or the
 * board waking comes back to it.
 *
 * @param me The AO.
 * @return true when the round was asked for.
 */
static bool wifi_retry_pay(Radio * const me)
{
    if ( (!me->retry_owed) || me->lp_rest || me->winflight
        || ((uint8_t)KRADIO_UP != me->link)
        || ((uint8_t)PARK_IDLE != me->park_phase)
        || ((uint8_t)WOWED_NONE != me->wifi_owed)
        || (!scan_channel_free(me)) )
    {
        return false;
    }
    me->retry_owed = false;
    if ( !wifi_retry_wanted(me) )
    {
        wifi_retry_stop(me);
        return false;
    }
    wifi_auto_self();
    return true;
}

/**
 * @brief The channel has nothing left to do: if the board is off the network
 *        and should not be, make sure a wait is running.
 *
 * What brings a join that failed and a news that found no room in the queue
 * back into the wait, with no publish: the next state says so.
 *
 * @param me The AO.
 */
static void wifi_retry_keep(Radio * const me)
{
    if ( (!me->retry_on) && ((uint8_t)RECON_DONE == me->recon)
        && wifi_retry_wanted(me) )
    {
        if ( 0U == me->retry_step )
        {
            me->retry_step = 1U;
        }
        wifi_retry_arm(me);
    }
}

/** @brief Publish the scan's end, with or without results behind it. */
static void wifi_publish_scan_end(Radio * const me, uint8_t count, bool ok)
{
    WifiScanEndEvt *e = Q_NEW_X(WifiScanEndEvt, 0U, WIFI_SCAN_END_SIG);
    if ( NULL != e )
    {
        e->count = count;
        e->ok    = ok;
        e->busy  = false;
        QACTIVE_PUBLISH(&e->super, &me->super);
    }
}

/**
 * @brief Publish the end of a Wi-Fi scan the C5 had no time for.
 *
 * Not a failure of the scan: nothing was started, and the asker is told to
 * ask again (the console answers `busy`, code 4).
 *
 * @param me The AO.
 */
static void wifi_publish_scan_busy(Radio * const me)
{
    WifiScanEndEvt *e = Q_NEW_X(WifiScanEndEvt, 0U, WIFI_SCAN_END_SIG);
    if ( NULL != e )
    {
        e->count = 0U;
        e->ok    = false;
        e->busy  = true;
        QACTIVE_PUBLISH(&e->super, &me->super);
    }
    (void)me;   /* without Q_SPY, QACTIVE_PUBLISH() ignores its sender */
}

/**
 * @brief One network of the asked sweep: counted, and posted to an asking AO
 *        while its queue keeps KODE_STORAGE_LIST_FREE_SLOTS free.
 *
 * A task's rows are copied from the driver's table at the end instead, so
 * nothing is posted for one. The first entry that does not fit ends the
 * posting; the count goes on, and the end says more.
 *
 * @param me The AO.
 * @param ap The network.
 */
static void wifi_ask_entry(Radio * const me, kdrv_wifi_ap_t const *ap)
{
    ++me->ask_seen;
    if ( (NULL == me->ask_who) || me->ask_cut )
    {
        return;
    }
    WifiScanEntryEvt *e = Q_NEW_X(WifiScanEntryEvt, 0U, WIFI_SCAN_ENTRY_SIG);
    if ( NULL == e )
    {
        me->ask_cut = true;
        return;
    }
    (void)memcpy(e->ssid, ap->ssid, sizeof(e->ssid));
    e->rssi     = ap->rssi;
    e->channel  = ap->channel;
    e->authmode = ap->authmode;
    (void)memcpy(e->bssid, ap->bssid, sizeof(e->bssid));
    if ( !QACTIVE_POST_X(me->ask_who, &e->super, KODE_STORAGE_LIST_FREE_SLOTS, &me->super) )
    {
        me->ask_cut = true;   /* QP took the entry back; the posting ends here */
    }
}

/**
 * @brief Empty the driver's hand-over ring, publishing what is in it.
 *
 * The sweep produces the listing a few channels at a time. The ring is drained
 * whether or not anybody reads it: a ring left full by an autojoin round or the
 * production row would have the driver report networks lost from the listing.
 */
static void wifi_drain_scan(Radio * const me)
{
    kdrv_wifi_ap_t ap;

    while ( kdrv_wifi_scan_next(&ap) )
    {
        if ( scan_ask_running(me, (uint8_t)KQP_SCAN_WIFI) )
        {
            wifi_ask_entry(me, &ap);
            continue;
        }
        if ( !me->scan_live )
        {
            continue;
        }
        WifiScanEntryEvt *e = Q_NEW_X(WifiScanEntryEvt, 0U, WIFI_SCAN_ENTRY_SIG);
        if ( NULL == e )
        {
            /* Out of blocks. Stop rather than assert, and let the end
             * marker carry the count that actually went out - a listing
             * that quietly stops short is a listing that lies. */
            KLOG_W(TAG, "scan listing cut short: no event blocks left");
            break;
        }
        (void)memcpy(e->ssid, ap.ssid, sizeof(e->ssid));
        e->rssi     = ap.rssi;
        e->channel  = ap.channel;
        e->authmode = ap.authmode;
        (void)memcpy(e->bssid, ap.bssid, sizeof(e->bssid));
        QACTIVE_PUBLISH(&e->super, &me->super);
        ++me->scan_sent;
    }
}

/** @brief Close a listing: whatever is left in the ring, then the marker. */
static void wifi_finish_scan(Radio * const me, bool ok)
{
    wifi_drain_scan(me);
    if ( scan_ask_running(me, (uint8_t)KQP_SCAN_WIFI) )
    {
        /* An asked sweep: its end goes to the asker, never the bus. */
        kode_status_t rc = KODE_ERROR;
        if ( ok )
        {
            rc = KODE_OK;
        }
        scan_ask_end(me, rc);
        return;
    }

    /* The marker's count is a byte and a room is not: past 255 it saturates,
     * which is a number nobody can act on either way. */
    uint8_t count = 255U;
    if ( 255U > me->scan_sent )
    {
        count = (uint8_t)me->scan_sent;
    }
    wifi_publish_scan_end(me, count, ok);
    me->scan_live = false;
    me->scan_sent = 0U;
}

/**
 * @brief One event per network the vault keeps, then the end marker.
 * @param me The AO.
 * @param ok false when the vault could not be asked: the listing is what
 *           this side last heard, and the end marker says it is not fresh.
 */
static void wifi_publish_saved(Radio * const me, bool ok)
{
    uint8_t sent = 0U;
    uint8_t total = 0U;

    if ( me->vlist_ok )
    {
        total = me->vlist.count;
    }
    for ( uint8_t i = 0U; i < total; ++i )
    {
        WifiSavedEvt *e = Q_NEW_X(WifiSavedEvt, 0U, WIFI_SAVED_ENTRY_SIG);
        if ( NULL == e )
        {
            KLOG_W(TAG, "saved listing cut short: no event blocks left");
            break;
        }
        (void)snprintf(e->ssid, sizeof(e->ssid), "%s", me->vlist.ssid[i]);
        QACTIVE_PUBLISH(&e->super, &me->super);
        ++sent;
    }

    WifiScanEndEvt *end = Q_NEW_X(WifiScanEndEvt, 0U, WIFI_SAVED_END_SIG);
    if ( NULL != end )
    {
        end->count = sent;
        end->ok    = ok;
        end->busy  = false;
        QACTIVE_PUBLISH(&end->super, &me->super);
    }
}

/** @brief Whether the vault keeps a network by this name (its last LIST). */
static bool vault_knows(Radio const * const me, const char *ssid)
{
    bool known = false;

    if ( me->vlist_ok )
    {
        for ( uint8_t i = 0U; (i < me->vlist.count) && !known; ++i )
        {
            known = (strncmp(me->vlist.ssid[i], ssid,
                             sizeof(me->vlist.ssid[i])) == 0);
        }
    }
    return known;
}

/**
 * @brief Send one request to the vault, recording what it is for.
 * @return false when it could not go: no link, or one already in flight.
 */
static bool radio_vault_send(Radio * const me, uint8_t op, uint8_t vop,
                             const void *payload, size_t len)
{
    if ( radio_svc_send(me, KODE_SVC_VAULT, op, payload, len) != KODE_OK )
    {
        return false;
    }
    me->vault_op = vop;
    return true;
}

/** @brief Ask the vault for its names again (after it changed). */
static void radio_vault_refresh(Radio * const me)
{
    (void)radio_vault_send(me, KODE_VAULT_LIST, (uint8_t)VOP_LIST, NULL, 0U);
}

/** @brief Ask the C5 to join a saved network from its vault. */
static bool radio_vault_join(Radio * const me, const char *ssid)
{
    kode_vault_ssid_t s;

    (void)memset(&s, 0, sizeof(s));
    (void)snprintf(s.ssid, sizeof(s.ssid), "%s", ssid);
    return radio_vault_send(me, KODE_VAULT_JOIN, (uint8_t)VOP_JOIN, &s, sizeof(s));
}

/**
 * @brief A join that worked: hand its password to the vault, then forget it
 *        here. The one time a password crosses the link after its join.
 */
static void radio_vault_remember(Radio * const me)
{
    kode_vault_cred_t c;

    (void)memset(&c, 0, sizeof(c));
    (void)snprintf(c.ssid, sizeof(c.ssid), "%s", me->ssid);
    (void)snprintf(c.pass, sizeof(c.pass), "%s", me->pass);
    if ( !radio_vault_send(me, KODE_VAULT_REMEMBER, (uint8_t)VOP_REMEMBER,
                          &c, sizeof(c)) )
    {
        KLOG_W(TAG, "the C5's vault could not be reached; '%s' is not saved",
               me->ssid);
    }
    (void)memset(&c, 0, sizeof(c));
    (void)memset(me->pass, 0, sizeof(me->pass));
}

/** @brief The vault answered (KODE_SVC_RSP_SIG, service VAULT). */
static void radio_vault_answer(Radio * const me, KodeSvcEvt const * const e)
{
    uint8_t const vop = me->vault_op;
    uint8_t const *const pay = me->svc_buf[KODE_SVC_VAULT];

    me->vault_op = (uint8_t)VOP_NONE;
    if ( (uint8_t)VOP_LIST == vop )
    {
        bool const fresh = (KODE_ST_OK == e->status)
                           && (sizeof(kode_vault_list_t) <= e->len);
        if ( fresh )
        {
            /* cppcheck-suppress misra-c2012-21.15 ; the payload is the packed wire struct's bytes, its size asserted */
            (void)memcpy(&me->vlist, pay, sizeof(me->vlist));
            if ( KODE_VAULT_NETS_MAX < me->vlist.count )
            {
                me->vlist.count = KODE_VAULT_NETS_MAX;
            }
            for ( uint8_t i = 0U; i < me->vlist.count; ++i )
            {
                me->vlist.ssid[i][sizeof(me->vlist.ssid[i]) - 1U] = '\0';
            }
            me->vlist_ok = true;
        }
        if ( me->vlist_publish )
        {
            me->vlist_publish = false;
            wifi_publish_saved(me, fresh);
        }
    }
    else if ( (uint8_t)VOP_REMEMBER == vop )
    {
        if ( KODE_ST_OK == e->status )
        {
            KLOG_N(TAG, "'%s' saved in the C5's vault", me->ssid);
        }
        else
        {
            KLOG_W(TAG, "the C5's vault did not keep '%s' (status %u)",
                   me->ssid, (unsigned)e->status);
        }
        radio_vault_refresh(me);
    }
    else if ( (uint8_t)VOP_FORGET == vop )
    {
        if ( KODE_ST_OK == e->status )
        {
            KLOG_N(TAG, "the C5's vault forgot a network");
        }
        else
        {
            KLOG_W(TAG, "that was not one of the remembered networks");
        }
        radio_vault_refresh(me);
    }
    else if ( (uint8_t)VOP_JOIN == vop )
    {
        /* A join asked by name, outside an autojoin round (which takes its
         * own answers): on to the wait, or say why not. */
        me->busy = false;
        if ( KODE_ST_OK == e->status )
        {
            kdrv_radio_req_t req;
            (void)memset(&req, 0, sizeof(req));
            req.type = KDRV_RADIO_JOB_WIFI_AWAIT;
            (void)snprintf(req.ssid, sizeof(req.ssid), "%s", me->ssid);
            if ( !radio_submit_req(me, &req) )
            {
                wifi_publish_state(me);
            }
        }
        else
        {
            KLOG_W(TAG, "the C5's vault would not join '%s' (status %u)",
                   me->ssid, (unsigned)e->status);
            wifi_publish_state(me);
        }
    }
    else
    {
        /* an answer nobody here is waiting for */
    }
}

/** @brief The vault did not answer in time. */
static void radio_vault_lost(Radio * const me)
{
    uint8_t const vop = me->vault_op;

    me->vault_op = (uint8_t)VOP_NONE;
    if ( (uint8_t)VOP_JOIN == vop )
    {
        me->busy = false;
        wifi_publish_state(me);
    }
    else if ( ((uint8_t)VOP_LIST == vop) && me->vlist_publish )
    {
        me->vlist_publish = false;
        wifi_publish_saved(me, false);
    }
    else
    {
        /* nothing waits on it */
    }
}

/**
 * @brief Pick the strongest network in the last scan that the vault keeps.
 *
 * The strongest, because signal is what predicts a join; and only networks the
 * scan saw. Entries already tried this round are skipped, so a network that
 * refuses us is not offered again.
 *
 * @return false when nothing is left to try.
 */
static bool auto_pick(Radio * const me)
{
    uint8_t const n = kdrv_wifi_scan_count();
    int           best = -1;
    int8_t        best_rssi = 0;

    for ( uint8_t i = 0U; i < n; ++i )
    {
        if ( (me->tried & (1UL << i)) != 0UL )
        {
            continue;
        }
        kdrv_wifi_ap_t ap;
        if ( !kdrv_wifi_scan_get(i, &ap) || ('\0' == ap.ssid[0]) )
        {
            continue;
        }
        if ( !vault_knows(me, ap.ssid) )
        {
            continue;
        }
        if ( (0 > best) || (ap.rssi > best_rssi) )
        {
            best      = (int)i;
            best_rssi = ap.rssi;
        }
    }

    if ( 0 > best )
    {
        return false;
    }

    kdrv_wifi_ap_t ap;
    (void)kdrv_wifi_scan_get((uint8_t)best, &ap);
    me->tried |= (1UL << (uint8_t)best);
    (void)snprintf(me->ssid, sizeof(me->ssid), "%s", ap.ssid);
    KLOG_I(TAG, "trying '%s' (%ddBm)", me->ssid, (int)ap.rssi);
    return true;
}
#endif /* CONFIG_KODEOS_WIFI */

#if CONFIG_KODEOS_WIFI
/** @brief Read the channels back from NVS. Anything unexpected reads as none. */
static void radio_hints_load(Radio * const me)
{
    size_t n = sizeof(me->hints);

    (void)memset(me->hints, 0, sizeof(me->hints));
    if ( (khal_nvs_get_blob(KSET_NAMESPACE, RADIO_HINTS_KEY, me->hints, &n) != KODE_OK)
        || (sizeof(me->hints) != n) )
    {
        (void)memset(me->hints, 0, sizeof(me->hints));
    }
    for ( uint8_t i = 0U; RADIO_HINTS_MAX > i; ++i )
    {
        me->hints[i].ssid[KDRV_WIFI_SSID_LEN] = '\0';
    }
}

/**
 * @brief The station has just got its address: remember where the network is.
 *
 * Written only when something changed, so a board that keeps coming back to
 * the same network on the same channel never touches the flash for it. The
 * name comes from the driver because an adoption never knew it otherwise.
 */
static void radio_hint_learn(Radio * const me)
{
    char          ssid[KDRV_WIFI_SSID_LEN + 1U];
    uint8_t const channel = kdrv_wifi_channel_last();
    uint8_t       at = RADIO_HINTS_MAX - 1U;

    kdrv_wifi_ssid_copy(ssid, sizeof(ssid));
    if ( (0U == channel) || ('\0' == ssid[0]) )
    {
        return;
    }
    if ( (strcmp(me->hints[0].ssid, ssid) == 0) && (me->hints[0].channel == channel) )
    {
        return;
    }
    for ( uint8_t i = 0U; RADIO_HINTS_MAX > i; ++i )
    {
        if ( strcmp(me->hints[i].ssid, ssid) == 0 )
        {
            at = i;
            break;
        }
    }
    for ( uint8_t i = at; 0U < i; --i )
    {
        me->hints[i] = me->hints[i - 1U];
    }
    (void)snprintf(me->hints[0].ssid, sizeof(me->hints[0].ssid), "%s", ssid);
    me->hints[0].channel = channel;
    if ( khal_nvs_set_blob(KSET_NAMESPACE, RADIO_HINTS_KEY, me->hints,
                          sizeof(me->hints)) != KODE_OK )
    {
        KLOG_W(TAG, "the channel of '%s' could not be kept", ssid);
    }
}

/**
 * @brief Listen where the next hint says its network was.
 *
 * A name the vault does not hold is skipped, once the vault has said what it
 * holds; before that its join answers for itself.
 *
 * @return true when a probe is with the worker; false when none is left.
 */
static bool auto_hint_next(Radio * const me)
{
    while ( RADIO_HINTS_MAX > me->hint_at )
    {
        uint8_t const i = me->hint_at;

        if ( '\0' == me->hints[i].ssid[0] )
        {
            break;
        }
        if ( (!me->vlist_ok) || vault_knows(me, me->hints[i].ssid) )
        {
            kdrv_radio_req_t req;

            (void)memset(&req, 0, sizeof(req));
            req.type     = KDRV_RADIO_JOB_WIFI_PROBE;
            req.channel  = me->hints[i].channel;
            req.dwell_ms = (uint16_t)RADIO_HINT_DWELL_MS;
            (void)snprintf(req.ssid, sizeof(req.ssid), "%s", me->hints[i].ssid);
            return radio_submit_req(me, &req);
        }
        ++me->hint_at;
    }
    return false;
}
#endif /* CONFIG_KODEOS_WIFI */

#if CONFIG_KODEOS_BLE
static void ble_app_link_edges(Radio * const me);
static void ble_app_link_publish(Radio * const me, bool connected);

/**
 * @brief The boards bonded, as NimBLE's store counts them.
 *
 * Read from the store except while the worker takes the link down: the
 * teardown frees the store's lock under this AO. The count the last state
 * published (ble_bonds_seen) stands for it until the worker is back.
 *
 * @param me The AO.
 * @return The bond count.
 */
static uint8_t radio_ble_bonds(Radio const * const me)
{
    uint8_t n = me->ble_bonds_seen;

    if ( !me->ble_store_away )
    {
        n = kdrv_ble_bonds();
    }
    return n;
}

/** @brief Publish what the BLE half is doing, then the link's edges for the app's service. */
static void ble_publish_state(Radio * const me)
{
    BleStateEvt *e = Q_NEW_X(BleStateEvt, 0U, BLE_STATE_SIG);
    if ( NULL != e )
    {
        /* OFF comes from the flag, not the driver: the host stack cannot be
         * brought back inside one boot (kdrv_ble_quiet()), so after `ble_down`
         * the driver still reports IDLE, and a screen would show the service
         * on. The flag is the service; the driver is the machinery. */
        e->state      = me->ble_want ? (uint8_t)kdrv_ble_state()
                                     : (uint8_t)KDRV_BLE_OFF;
        e->addr_known = kdrv_ble_addr(e->addr);
        if ( !e->addr_known )
        {
            (void)memset(e->addr, 0, sizeof(e->addr));
        }
        e->console = kdrv_ble_console_attached();
        e->tx_drop = kdrv_ble_console_tx_drop();
        e->backlog = kdrv_ble_console_backlog();
        e->retry   = kdrv_ble_console_retry();
        me->ble_bonds_seen = radio_ble_bonds(me);
        e->bonds   = me->ble_bonds_seen;
        e->hidden    = me->ble_adv_hidden;
        e->pair_open = me->ble_pair_open;
        QACTIVE_PUBLISH(&e->super, &me->super);
    }
    ble_app_link_edges(me);
}

_Static_assert(KQP_BLE_NAME_MAX == (KDRV_BLE_ADV_MAX - 1U),
               "BleRspReqEvt.name holds the longest name the scan response carries");
_Static_assert(KQP_BLE_DATA_MAX == (KDRV_BLE_ADV_MAX - 2U),
               "BleRspReqEvt.data holds the most data the scan response carries");
_Static_assert(KQP_BLE_NAME_MAX < sizeof(((kdrv_radio_req_t *)NULL)->ssid),
               "a BLE_RSP job carries the name in its ssid");
_Static_assert(KQP_BLE_MFG_MAX == KDRV_BLE_MFG_MAX,
               "BleScanEvt.mfg must hold what kdrv_ble keeps");
_Static_assert(KQP_BLE_SVC_MAX == KDRV_BLE_SVC_MAX,
               "BleScanEvt.svc16 must hold what kdrv_ble keeps");

/**
 * @brief One device seen, as an entry.
 * @param d The device.
 * @return The event, BLE_SCAN_ENTRY_SIG; NULL when the pool is dry.
 */
static BleScanEvt *ble_scan_evt(kdrv_ble_dev_t const *d)
{
    BleScanEvt *e = Q_NEW_X(BleScanEvt, 0U, BLE_SCAN_ENTRY_SIG);

    if ( NULL != e )
    {
        (void)memcpy(e->addr, d->addr, sizeof(e->addr));
        e->rssi = d->rssi;
        (void)snprintf(e->name, sizeof(e->name), "%s", d->name);
        e->addr_type = d->addr_type;
        e->mfg_len   = d->mfg_len;
        (void)memcpy(e->mfg, d->mfg, sizeof(e->mfg));
        e->svc_n     = d->svc_n;
        (void)memcpy(e->svc16, d->svc16, sizeof(e->svc16));
    }
    return e;
}

/** @brief One event per device seen, then the end marker. */
static void ble_publish_scan(Radio * const me)
{
    uint8_t const total = kdrv_ble_scan_count();
    uint8_t       sent  = 0U;

    for ( uint8_t i = 0U; i < total; ++i )
    {
        kdrv_ble_dev_t d;
        if ( !kdrv_ble_scan_get(i, &d) )
        {
            break;
        }
        BleScanEvt *e = ble_scan_evt(&d);
        if ( NULL == e )
        {
            KLOG_W(TAG, "BLE listing cut short: no event blocks left");
            break;
        }
        QACTIVE_PUBLISH(&e->super, &me->super);
        ++sent;
    }

    WifiScanEndEvt *end = Q_NEW_X(WifiScanEndEvt, 0U, BLE_SCAN_END_SIG);
    if ( NULL != end )
    {
        end->count = sent;
        end->ok    = true;
        end->busy  = false;
        QACTIVE_PUBLISH(&end->super, &me->super);
    }
}

/** @brief Publish an empty BLE listing - the answer when there is no link. */
static void ble_publish_scan_end(Radio * const me, bool ok)
{
    WifiScanEndEvt *end = Q_NEW_X(WifiScanEndEvt, 0U, BLE_SCAN_END_SIG);
    if ( NULL != end )
    {
        end->count = 0U;
        end->ok    = ok;
        end->busy  = false;
        QACTIVE_PUBLISH(&end->super, &me->super);
    }
}

/**
 * @brief Publish the end of a BLE scan the C5 had no time for (see wifi_publish_scan_busy()).
 *
 * @param me The AO.
 */
static void ble_publish_scan_busy(Radio * const me)
{
    WifiScanEndEvt *end = Q_NEW_X(WifiScanEndEvt, 0U, BLE_SCAN_END_SIG);
    if ( NULL != end )
    {
        end->count = 0U;
        end->ok    = false;
        end->busy  = true;
        QACTIVE_PUBLISH(&end->super, &me->super);
    }
    (void)me;   /* without Q_SPY, QACTIVE_PUBLISH() ignores its sender */
}

/**
 * @brief The end of an asked listen: its devices, then its answer.
 *
 * An asking AO is posted one entry per device while its queue keeps
 * KODE_STORAGE_LIST_FREE_SLOTS free; the first that does not fit ends the
 * posting, the count goes on, and the end says more. A task's rows are
 * copied from the driver's table by scan_ask_end() instead.
 *
 * @param me The AO.
 * @param ok The listen ended; false when it failed.
 */
static void ble_ask_finish(Radio * const me, bool ok)
{
    kode_status_t rc = KODE_ERROR;

    if ( ok )
    {
        uint8_t const total = kdrv_ble_scan_count();

        for ( uint8_t i = 0U; i < total; ++i )
        {
            kdrv_ble_dev_t d;

            if ( !kdrv_ble_scan_get(i, &d) )
            {
                break;
            }
            ++me->ask_seen;
            if ( (NULL != me->ask_who) && !me->ask_cut )
            {
                BleScanEvt *e = ble_scan_evt(&d);

                if ( NULL == e )
                {
                    me->ask_cut = true;
                }
                else if ( !QACTIVE_POST_X(me->ask_who, &e->super,
                                         KODE_STORAGE_LIST_FREE_SLOTS, &me->super) )
                {
                    me->ask_cut = true;   /* QP took the entry back; the posting ends here */
                }
                else
                {
                    /* posted */
                }
            }
        }
        rc = KODE_OK;
    }
    scan_ask_end(me, rc);
}

/** @brief Pool blocks a central's write leaves for the owners' own events. */
#define RADIO_BLE_WRITE_POOL_MARGIN  1U

_Static_assert(KQP_BLE_APP_NAME_MAX == KDRV_BLE_APP_NAME_MAX,
               "BleAppWriteEvt.name holds a characteristic's name");
_Static_assert(KQP_BLE_APP_VALUE_MAX == KDRV_BLE_APP_VALUE_MAX,
               "BleAppWriteEvt.data holds a whole value");

/**
 * @brief Runs on NimBLE's host task: a central wrote to the app's service.
 *        A copy goes to AO_Radio, which publishes it. Post and go.
 * @param chr  The characteristic's place.
 * @param name Its name.
 * @param data The bytes.
 * @param len  Their count.
 * @return true when the copy is in AO_Radio's queue; false makes the write fail.
 */
static bool radio_on_ble_write(uint8_t chr, const char *name, const uint8_t *data, uint16_t len)
{
    bool            posted = false;
    BleAppWriteEvt *e      = Q_NEW_X(BleAppWriteEvt, RADIO_BLE_WRITE_POOL_MARGIN, BLE_APP_WRITE_SIG);

    if ( (NULL != e) && (KQP_BLE_APP_VALUE_MAX >= len) )
    {
        e->chr = chr;
        (void)snprintf(e->name, sizeof(e->name), "%s", name);
        e->len = len;
        if ( 0U < len )
        {
            (void)memcpy(e->data, data, len);
        }
        /* QP takes the event back when the queue is full. */
        posted = QACTIVE_POST_X(AO_Radio, &e->super, KQP_RADIO_CMD_MARGIN, NULL);
    }
    else if ( NULL != e )
    {
        QF_gc(&e->super);
    }
    else
    {
        /* the pool is dry: the write fails */
    }
    return posted;
}

/**
 * @brief Publish BLE_APP_LINK_SIG for each edge of the link since the last.
 *
 * A central is on when the driver says CONNECTED and Bluetooth is on in
 * Settings. A link that ended and another that began between two reads -
 * the driver's count moved while the state stayed CONNECTED - is told as
 * both edges.
 *
 * @param me AO_Radio.
 */
static void ble_app_link_edges(Radio * const me)
{
    bool const     now   = me->ble_want && (kdrv_ble_state() == KDRV_BLE_CONNECTED);
    uint32_t const links = kdrv_ble_links();

    if ( now && me->ble_app_up && (links != me->ble_links_seen) )
    {
        ble_app_link_publish(me, false);
        ble_app_link_publish(me, true);
    }
    else if ( now != me->ble_app_up )
    {
        ble_app_link_publish(me, now);
    }
    else
    {
        /* no edge */
    }
    me->ble_app_up     = now;
    me->ble_links_seen = links;
}

/**
 * @brief Publish one edge of the link.
 * @param me        AO_Radio.
 * @param connected true for a link that began.
 */
/* cppcheck-suppress misra-c2012-2.7 ; @p me is the publish's sender, which only a QS build reads */
static void ble_app_link_publish(Radio * const me, bool connected)
{
    BleAppLinkEvt *e = Q_NEW_X(BleAppLinkEvt, 0U, BLE_APP_LINK_SIG);

    if ( NULL != e )
    {
        e->connected = connected;
        QACTIVE_PUBLISH(&e->super, &me->super);
    }
}

/** Runs on NimBLE's host task: a connection arrived or left. Post and go. */
static void radio_on_ble(kdrv_ble_state_t state)
{
    (void)state;
    /* BLE_LINK_SIG and not BLE_STATE_REQ_SIG, though the two publish the same
     * record. This one also restores the advertising invariant, and a
     * disconnect is the whole reason that invariant needs restoring: the
     * advert was consumed when the central arrived. Keeping `ble_info`'s
     * signal free of the side effect is what stops a read-only verb from
     * quietly starting a radio. */
    static const QEvt evt = QEVT_INITIALIZER(BLE_LINK_SIG);
    /* Asking ourselves to publish rather than publishing from a foreign
     * thread: the state is read back from the driver on our own thread, the
     * way every other answer here is built. */
    QACTIVE_POST(AO_Radio, &evt, NULL);
}

#if CONFIG_KODEOS_BLE_PAIRING_PASSKEY
/** Runs on NimBLE's host task: a pairing moved. Post and go. */
static void radio_on_ble_pair(void)
{
    /* Carries nothing for the same reason radio_on_ble() ignores its
     * argument: the record is collected with kdrv_ble_pair_take() on our own
     * thread, where reading it cannot race the host task writing it. */
    static const QEvt evt = QEVT_INITIALIZER(BLE_PAIR_TAKE_SIG);
    QACTIVE_POST(AO_Radio, &evt, NULL);
}

/** @brief Collect what the driver is holding and put it on the bus. */
static void ble_publish_pair(Radio * const me)
{
    kdrv_ble_pair_t rec;

    /* A nudge per phase and a take per nudge, so an empty answer is normal:
     * two phases can collapse into one record when a pairing fails inside
     * the same instant it started, and the second take then finds nothing. */
    if ( !kdrv_ble_pair_take(&rec) )
    {
        return;
    }

    BlePairEvt *ev = Q_NEW_X(BlePairEvt, 0U, BLE_PAIR_SIG);
    if ( NULL == ev )
    {
        /* The margin is zero on purpose - every other publisher here does
         * the same - but a lost SHOW is a card that never appears and a
         * human left waiting, so it is worth a line. */
        KLOG_W(TAG, "no event for the pairing record");
        return;
    }
    ev->phase   = (uint8_t)rec.phase;
    ev->passkey = rec.passkey;
    ev->outcome = (uint8_t)rec.outcome;
    if ( (KDRV_BLE_PAIR_DONE == rec.phase) && (KDRV_BLE_PAIR_OK == rec.outcome) )
    {
        /* The new device is in: the board hides again once it leaves. */
        me->ble_pair_open = false;
    }
    QACTIVE_PUBLISH(&ev->super, &me->super);
}
#endif /* CONFIG_KODEOS_BLE_PAIRING_PASSKEY */
#endif /* CONFIG_KODEOS_BLE */

/**
 * @brief Post a request to AO_Storage and go on.
 *
 * Posted, not waited for: AO_Storage's unmount and mount are calls on its
 * worker, so nothing here may assume they have run when this returns. What
 * must not go ahead of one is asked for with radio_ask().
 */
static void radio_storage_ask(storage_op_t op)
{
    storage_request(KDRV_MEDIUM_SD, op);
}

/** @brief Back to the plain mount path, card remounted. */
static void radio_restore_plain_sd(void)
{
    kdrv_sd_set_hosted(false);
    radio_storage_ask(STORAGE_OP_MOUNT);
}

/**
 * @brief Everything that must happen before the C5 can be reset, then the
 *        CONNECT job.
 *
 * In order: hosted mode first, so a mount typed during bring-up fails cleanly
 * instead of fighting esp-hosted for the controller; then the unmount, asked
 * WITH an answer; only when it has run is the job handed to the worker
 * (radio_connect_after_unmount(), from Radio_starting). The channel is held
 * meanwhile. A request that cannot be asked undoes all of it.
 *
 * @return true when the unmount is asked for and `starting` may be entered;
 *         false when it could not be asked, and nothing changed.
 */
static bool radio_begin_connect(Radio * const me)
{
    /* Whoever asks for the link with neither service wanted is asking for
     * the link itself, and it is theirs until `radio_down`: no service
     * going off will take it away. A service request sets its flag before
     * it gets here, so this is false for those. */
    if ( !me->wifi_want && !me->ble_want )
    {
        me->link_asked = true;
    }
    /* Whoever asks, this is the connect a mute C5 was owed - and a link
     * built for that reason is the one retry: it is not owed another until
     * the C5 has answered a job. A link somebody asked for starts clean. */
    if ( !me->relink_owed )
    {
        me->relink_n    = 0U;       /* somebody asked: the tries start over */
        me->relink_dead = false;
    }
    (void)QTimeEvt_disarm(&me->relink_tmo);
    me->relink_wake = false;
    me->relinked    = me->relink_owed;
    me->relink_owed = false;
    KLOG_N(TAG, "bringing the C5 link up (the card is unmounted while the "
                "C5 reboots)");
    kdrv_sd_set_hosted(true);
    /* An interface reading its first screen off the card asked for the card to
     * stay (radio_boot_hold()): mounted, it is what the answer waits for. The
     * card's supply is the P4's since the DVT, and the C5's reset does not
     * reach it. */
    if ( !radio_ask(me, s_boot_card_stays ? STORAGE_OP_MOUNT : STORAGE_OP_UNMOUNT,
                    RADIO_ASK_CONNECT) )
    {
        KLOG_E(TAG, "the card could not be asked to let go; the C5 is not "
                    "reset");
        radio_restore_plain_sd();
        return false;
    }
    me->busy = true;
    return true;
}

/**
 * @brief Tell whoever was waiting for the link that it will not come.
 *
 * What the refusals at the request did, for a refusal that is only known when
 * the card has answered: an update that was bringing the link up for its image
 * fails, and the service whose switch asked for the link says its state.
 *
 * @param me The AO.
 */
static void radio_connect_refused(Radio * const me)
{
    if ( (uint8_t)INST_LINKING == me->inst )
    {
        KLOG_E(TAG, "install dst=c5 failed: the link could not be brought up");
        inst_fail(me);
    }
#if CONFIG_KODEOS_WIFI
    wifi_publish_state(me);
#endif
#if CONFIG_KODEOS_BLE
    ble_publish_state(me);
#endif
}

/**
 * @brief The card has let go (or has not): reset the C5, or refuse to.
 *
 * Called from Radio_starting when AO_Storage answers the unmount that
 * radio_begin_connect() asked for.
 *
 * @return KODE_OK when the connect job is with the worker; KODE_DENIED when
 *         the card is still mounted and KODE_NO_RESOURCES when the worker
 *         refused: the bring-up is over, the plain mount path is back and the
 *         caller fails the state.
 */
static kode_status_t radio_connect_after_unmount(Radio * const me)
{
    kdrv_radio_req_t req;

    if ( kdrv_sd_mounted() && !s_boot_card_stays )
    {
        /* The reset glitches the card's supply: with a volume mounted that
         * is a corrupted card, so the link is not brought up. */
        KLOG_E(TAG, "the card is still mounted; the C5 is not reset");
        me->busy = false;
        radio_restore_plain_sd();
        return KODE_DENIED;
    }
    /* The bring-up resets the C5, which lets go of J3's supply enable, and
     * then drives it as the wish is now: on from the reset, and where the
     * wish says once the link is up. A pulse still running is the reset's
     * now. */
    s_boot_card_stays = false;
    (void)memset(&req, 0, sizeof(req));
    req.type       = KDRV_RADIO_JOB_CONNECT;
    req.on         = me->exp_want;
    /* A C5 the last firmware parked is kept only for a boot that joins. */
    req.wants_wifi = me->wifi_want;
    me->busy = false;
    if ( !radio_submit_req(me, &req) )
    {
        KLOG_E(TAG, "the worker refused the connect job");
        radio_restore_plain_sd();
        return KODE_NO_RESOURCES;
    }
    (void)QTimeEvt_disarm(&me->exp_pulse);
    me->exp_on    = true;   /* the bring-up resets the C5, which lets go of IO5 */
    me->exp_known = true;
    me->exp_link  = me->exp_want;
    return KODE_OK;
}

/**
 * @brief The card answered the unmount that gates the bring-up.
 *
 * A card still mounted is asked again (radio_unmount_wait()) and the
 * bring-up holds; one that let go gets the connect job; one that stayed
 * mounted for the whole window fails the link.
 *
 * @param me The AO, in Radio_starting.
 * @return The state's answer: handled while the connect is with the worker or
 *         the unmount is asked again, a transition to Radio_failed when the
 *         card stayed mounted and to Radio_off when the worker refused.
 */
static QState radio_connect_answer(Radio * const me)
{
    QState status;

    if ( radio_unmount_wait(me, RADIO_ASK_CONNECT) )
    {
        status = Q_HANDLED();
    }
    else
    {
        /* The connect goes to the worker only if the card really let go. */
        kode_status_t const gate = radio_connect_after_unmount(me);

        if ( KODE_OK == gate )
        {
            status = Q_HANDLED();
        }
        else if ( KODE_DENIED == gate )
        {
            /* A card still mounted is a refusal of its own: the link is
             * failed, not just unasked. */
            if ( (uint8_t)TEST_IDLE != me->test_phase )
            {
                c5_test_end(me, false, "the card is still mounted; the C5 was "
                                       "not reset");
            }
            radio_connect_refused(me);
            status = Q_TRAN(&Radio_failed);
        }
        else
        {
            /* The worker would not take the connect: what a refusal at the
             * request always did - the link stays where it was, off, and
             * whoever asked is told. */
            if ( (uint8_t)TEST_IDLE != me->test_phase )
            {
                c5_test_end(me, false, "the worker refused the connect job");
            }
            radio_connect_refused(me);
            status = Q_TRAN(&Radio_off);
        }
    }
    return status;
}

/**
 * @brief The card has let go (or has not): open the C5 update's session, or
 *        refuse the update.
 *
 * The activate at the end of the push reboots the C5, which switches the
 * card's supply; with a volume still mounted that corrupts the card, so the
 * update does not start.
 *
 * @param me The AO.
 * @return true when the session is being opened; false when the update ended
 *         (the card is still mounted, or the worker refused) and the caller
 *         leaves the state.
 */
static bool radio_begin_after_unmount(Radio * const me)
{
    me->begin_wait = false;
    me->busy       = false;
    if ( kdrv_sd_mounted() )
    {
        c5_release(me);
        ota_fail(me, "the card is still mounted; the C5 was not touched");
        return false;
    }
    KLOG_N(TAG, "sending %luKB to the C5 (do not power the board "
                "off until this finishes)",
           (unsigned long)(me->kbuf.fill / 1024U));
    if ( !radio_submit(me, KDRV_RADIO_JOB_C5_BEGIN) )
    {
        c5_release(me);
        ota_fail(me, "the worker refused to open the session");
        return false;
    }
    return true;
}

/**
 * @brief The card answered the unmount that gates a C5 update's session.
 *
 * A card still mounted is asked again (radio_unmount_wait()) and the update
 * holds; the rest is radio_begin_after_unmount().
 *
 * @param me The AO, in Radio_updating.
 * @return Handled while the session is being opened or the unmount is asked
 *         again; a transition to Radio_up when the update ended.
 */
static QState radio_begin_answer(Radio * const me)
{
    QState status = Q_HANDLED();

    if ( !me->begin_wait )
    {
        radio_unmount_end(me, RADIO_ASK_BEGIN);   /* the update ended meanwhile */
    }
    else if ( !radio_unmount_wait(me, RADIO_ASK_BEGIN) )
    {
        if ( !radio_begin_after_unmount(me) )
        {
            status = Q_TRAN(&Radio_up);
        }
    }
    else
    {
        /* Asked again in a moment. */
    }
    return status;
}

/* ---- The radio worker's jobs ----------------------------------------------- */

/** @brief Stop the guard, counting an expiry that is already queued. */
static void radio_guard_stop(Radio * const me)
{
    if ( kqp_timer_stale(&me->guard, me->guard_live) )
    {
        me->guard_stale += 1U;
    }
    me->guard_live = false;
}

/**
 * @brief Start the guard on a job just handed to the worker.
 *
 * @param me  The AO.
 * @param req The job: a BLE listen is as long as it was asked to be.
 */
static void radio_guard_start(Radio * const me, const kdrv_radio_req_t *req)
{
    uint32_t ms = KODE_RADIO_GUARD_MS;

    if ( (KDRV_RADIO_JOB_BLE_SCAN == req->type) || (KDRV_RADIO_JOB_BLE_FIND == req->type) )
    {
        ms += req->len + KODE_RADIO_BLE_SCAN_SLACK_MS;
    }
    radio_guard_stop(me);
    QTimeEvt_armX(&me->guard, KQP_TICKS_PER_MS(ms), 0U);
    me->guard_live = true;
}

/**
 * @brief The job as the radio worker runs it: the driver's blocking call.
 *
 * Runs on the worker's task and touches only its argument, which is the AO's
 * @c wreq and stays valid until the done event.
 *
 * @param arg   The AO's @c wreq.
 * @param value Unused; what a job measured is in the driver's result tables.
 * @return What the driver answered.
 */
/* cppcheck-suppress constParameterCallback ; KqpJobFn fixes the signature, and its argument is void * */
static kode_status_t radio_job_run(void *arg, uint32_t *value)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the worker hands back the pointer it was given, which is the AO's job storage */
    const kdrv_radio_req_t * const req = (const kdrv_radio_req_t *)arg;

    *value = 0U;
#if CONFIG_KODE_LOWPOWER
    if ( KDRV_RADIO_JOB_SLEEP == req->type )
    {
        /* The sentinel is not a link job: a failure of it must not make the
         * driver ask a C5 that is asleep for its version, so it does not go
         * through kdrv_radio_run(). */
        if ( req->on )
        {
#if CONFIG_KODEOS_BLE
            /* While the bus still works. The rest never pays for a fast
             * advert, nor for a central polled every 15 ms. */
            (void)kdrv_ble_adv_calm(req->len);
            (void)kdrv_ble_conn_calm(true);
#endif
            kode_status_t const down = kdrv_radio_sleep(true, &radio_sleep_note, &l_radio);
#if CONFIG_KODEOS_BLE
            if ( KODE_OK != down )
            {
                /* The board stays awake (the round is answered busy): so
                 * does the central's interval. */
                (void)kdrv_ble_conn_calm(false);
            }
#endif
            return down;
        }
        kode_status_t const st = kdrv_radio_sleep(false, NULL, NULL);
#if CONFIG_KODEOS_BLE
        if ( KODE_OK == st )
        {
            /* The bus is up again: a central still connected gets the
             * awake interval back. */
            (void)kdrv_ble_conn_calm(false);
        }
#endif
        return st;
    }
#endif
    return kdrv_radio_run(req);
}

/**
 * @brief Which outcome the end of a job of @p type is.
 * @param type The job.
 * @return The outcome the state handlers read.
 */
static radio_out_t radio_out_of(kdrv_radio_job_t type)
{
    radio_out_t out = RADIO_OUT_CONNECT_DONE;

    switch ( type )
    {
        case KDRV_RADIO_JOB_CONNECT:      out = RADIO_OUT_CONNECT_DONE;      break;
        case KDRV_RADIO_JOB_DISCONNECT:   out = RADIO_OUT_DISCONNECT_DONE;   break;
        case KDRV_RADIO_JOB_WIFI_SCAN:    out = RADIO_OUT_WIFI_SCAN_DONE;    break;
        case KDRV_RADIO_JOB_WIFI_JOIN:    out = RADIO_OUT_WIFI_JOIN_DONE;    break;
        case KDRV_RADIO_JOB_WIFI_AWAIT:   out = RADIO_OUT_WIFI_JOIN_DONE;    break;
        case KDRV_RADIO_JOB_WIFI_ADOPT:   out = RADIO_OUT_WIFI_JOIN_DONE;    break;
        case KDRV_RADIO_JOB_WIFI_LEAVE:   out = RADIO_OUT_WIFI_LEAVE_DONE;   break;
        case KDRV_RADIO_JOB_WIFI_STOP:    out = RADIO_OUT_WIFI_STOP_DONE;    break;
        case KDRV_RADIO_JOB_WIFI_RSSI:    out = RADIO_OUT_WIFI_RSSI_DONE;    break;
        case KDRV_RADIO_JOB_WIFI_PREPARE: out = RADIO_OUT_WIFI_PREPARE_DONE; break;
        case KDRV_RADIO_JOB_C5_BEGIN:     out = RADIO_OUT_C5_BEGIN_DONE;     break;
        case KDRV_RADIO_JOB_C5_WRITE:     out = RADIO_OUT_C5_WRITE_DONE;     break;
        case KDRV_RADIO_JOB_C5_FINISH:    out = RADIO_OUT_C5_FINISH_DONE;    break;
        case KDRV_RADIO_JOB_C5_ABORT:     out = RADIO_OUT_C5_ABORT_DONE;     break;
        case KDRV_RADIO_JOB_C5_INFO:      out = RADIO_OUT_C5_INFO_DONE;      break;
        case KDRV_RADIO_JOB_BLE_START:    out = RADIO_OUT_BLE_START_DONE;    break;
        case KDRV_RADIO_JOB_BLE_ADV:      out = RADIO_OUT_BLE_ADV_DONE;      break;
        case KDRV_RADIO_JOB_BLE_SCAN:     out = RADIO_OUT_BLE_SCAN_DONE;     break;
        case KDRV_RADIO_JOB_BLE_QUIET:    out = RADIO_OUT_BLE_QUIET_DONE;    break;
        case KDRV_RADIO_JOB_BLE_STOP:     out = RADIO_OUT_BLE_STOP_DONE;     break;
        case KDRV_RADIO_JOB_BLE_UNPAIR:   out = RADIO_OUT_BLE_UNPAIR_DONE;   break;
        case KDRV_RADIO_JOB_BLE_RSP:      out = RADIO_OUT_BLE_RSP_DONE;      break;
        case KDRV_RADIO_JOB_WIFI_PS:      out = RADIO_OUT_WIFI_PS_DONE;      break;
        case KDRV_RADIO_JOB_EXP_POWER:    out = RADIO_OUT_EXP_POWER_DONE;    break;
        case KDRV_RADIO_JOB_AMP:          out = RADIO_OUT_AMP_DONE;          break;
        case KDRV_RADIO_JOB_WIFI_PROBE:   out = RADIO_OUT_WIFI_PROBE_DONE;   break;
        case KDRV_RADIO_JOB_BLE_FIND:     out = RADIO_OUT_BLE_FIND_DONE;     break;
#if CONFIG_KODE_LOWPOWER
        case KDRV_RADIO_JOB_SLEEP:        out = RADIO_OUT_SLEEP_DONE;        break;
#endif
        default:                                                             break;
    }
    return out;
}

/**
 * @brief Submit a job that needs arguments, marking the AO busy.
 *
 * One job at a time: while one is with the worker - one whose guard fired or
 * whose link was lost included, because the worker may still be inside it,
 * using @c wreq and whatever the job lent - nothing is handed over.
 *
 * @param me  The AO.
 * @param req The job. Copied into @c wreq.
 * @return true when the worker has it.
 */
static bool radio_submit_req(Radio * const me, const kdrv_radio_req_t *req)
{
    if ( (uint8_t)PARK_DONE == me->park_phase )
    {
        /* A parked C5 is not asked anything: the restart is on its way. */
        return false;
    }
    uint16_t seq = 0U;

    if ( (NULL == me->jobs) || me->winflight )
    {
        KLOG_E(TAG, "the worker is still inside a job; job %d refused",
               (int)req->type);
        return false;
    }
    if ( (KDRV_RADIO_JOB_DISCONNECT == req->type)
        && (radio_svc_busy(me) || radio_teardown_waiting(me)) )
    {
        /* The teardown destroys what a send inside esp-hosted's RPC wait is
         * sleeping on, gives up the SDMMC host a mounted card sits on, and
         * ends with the C5 held in reset, which switches the card's supply
         * off. It is owed, the channel is held for it, and the last of the
         * sends and of the card's unmount answers pays it
         * (radio_disc_release()). */
        if ( radio_svc_busy(me) )
        {
            KLOG_I(TAG, "the teardown waits for a KODE request inside the C5 "
                        "link");
        }
        else
        {
            KLOG_I(TAG, "the teardown waits for the card to be unmounted");
        }
        me->disc_owed = true;
        me->busy      = true;
        return true;
    }
    me->jobs->wreq = *req;
    if ( kqp_worker_submit(&me->worker, &radio_job_run, &me->jobs->wreq,
                          RADIO_JOB_SIG, &seq) != KODE_OK )
    {
        KLOG_E(TAG, "the worker refused job %d", (int)req->type);
        return false;
    }
    me->wseq      = seq;
    me->winflight = true;
    me->wreplied  = false;
    me->wdrop     = false;
#if CONFIG_KODEOS_BLE
    if ( KDRV_RADIO_JOB_DISCONNECT == req->type )
    {
        me->ble_store_away = true;
    }
#endif
    radio_guard_start(me, req);
    me->busy = true;
    return true;
}

/** @brief Submit an argument-less job, marking the AO busy. */
static bool radio_submit(Radio * const me, kdrv_radio_job_t job)
{
    kdrv_radio_req_t req;
    (void)memset(&req, 0, sizeof(req));
    req.type = job;
    return radio_submit_req(me, &req);
}

/**
 * @brief Send the owed teardown, if nothing it waits for is left.
 *
 * Three things hold a DISCONNECT: the job inside the radio worker (it uses the
 * worker's storage), a KODE send inside the service worker, and an unmount of
 * the card that has not been answered. It goes once, when the last of them is
 * back, whichever that is; this is called by each of their ends.
 *
 * @param me The AO.
 */
static void radio_disc_release(Radio * const me)
{
    if ( me->disc_owed && (!me->winflight) && (!radio_svc_busy(me))
        && (!radio_teardown_waiting(me)) )
    {
        me->disc_owed = false;
        (void)radio_submit(me, KDRV_RADIO_JOB_DISCONNECT);
    }
}

/**
 * @brief The card has let go, or could not: the teardown it held may go.
 *
 * A teardown cannot wait for ever on a card that will not unmount: a refused
 * unmount is asked again for RADIO_UNMOUNT_WAIT_MS (radio_unmount_wait()),
 * then the link comes down with the card mounted under it, said in the log.
 *
 * @param me The AO.
 */
static void radio_teardown_answered(Radio * const me)
{
    if ( radio_unmount_wait(me, RADIO_ASK_TEARDOWN) )
    {
        return;     /* asked again in a moment; the teardown keeps waiting */
    }
    if ( kdrv_sd_mounted() )
    {
        KLOG_W(TAG, "the card is still mounted; the link comes down anyway");
    }
    radio_disc_release(me);
}

/**
 * @brief The retry timer expired where only a teardown's unmount can be
 *        waiting (every state but the two that own the others).
 *
 * @param me The AO.
 */
static void radio_teardown_retry(Radio * const me)
{
    if ( (uint8_t)RADIO_ASK_TEARDOWN != me->retry_kind )
    {
        /* An old expiry of a window that ended. */
        radio_unmount_end(me, (radio_ask_t)me->retry_kind);
    }
    else if ( !radio_unmount_refire(me) )
    {
        radio_teardown_answered(me);
    }
    else
    {
        /* Asked again; the answer comes. */
    }
}

/**
 * @brief Ask the card to let go before the link is torn down.
 *
 * Asked WITH an answer: the DISCONNECT that follows is held until it comes
 * (radio_submit_req()). When the ask cannot be made at all the unmount is
 * posted without one and the teardown does not wait.
 *
 * @param me The AO.
 */
static void radio_unmount_for_teardown(Radio * const me)
{
    if ( !radio_ask(me, STORAGE_OP_UNMOUNT, RADIO_ASK_TEARDOWN) )
    {
        KLOG_W(TAG, "the card could not be asked to let go; the teardown "
                    "does not wait for it");
        radio_storage_ask(STORAGE_OP_UNMOUNT);
    }
}

/**
 * @brief The link is lost: have it torn down, as soon as the worker can.
 *
 * With no job inside the worker the DISCONNECT goes now. With one inside (the
 * transport dropping under a scan, or the job that found the C5 mute) the
 * worker is still using the job's storage: the teardown is owed, the channel
 * is held, and that job's done event, which answers nothing now, pays it.
 *
 * @param me The AO.
 */
static void radio_disconnect_after_loss(Radio * const me)
{
    me->busy = false;
    if ( me->winflight )
    {
        me->wdrop     = true;
        me->disc_owed = true;
        me->busy      = true;
    }
    else
    {
        (void)radio_submit(me, KDRV_RADIO_JOB_DISCONNECT);
    }
}

/**
 * @brief Turn what the radio worker or the driver posted into an outcome.
 *
 * Called once per RADIO_JOB_SIG, RADIO_JOB_GUARD_SIG or RADIO_LINK_LOST_SIG,
 * by the state that handles it, never by a parent. A done event is an answer
 * only when its sequence number is @c wseq and nobody (the guard, a lost link)
 * answered for the job already; any other is dropped. The worker is free once
 * the right number comes back. An expired guard answers with KODE_TIMEOUT and
 * leaves the worker busy.
 *
 * @param me The AO.
 * @param e  The event.
 * @return true when @c me->out holds an outcome the state is to handle.
 */
static bool radio_outcome(Radio * const me, QEvt const * const e)
{
    bool deliver = false;

    if ( (QSignal)RADIO_LINK_LOST_SIG == e->sig )
    {
        me->out.evt    = (uint8_t)RADIO_OUT_LINK_LOST;
        me->out.status = Q_EVT_CAST(RadioLostEvt)->status;
        deliver        = true;
    }
    else if ( (QSignal)RADIO_JOB_GUARD_SIG == e->sig )
    {
        if ( 0U < me->guard_stale )
        {
            /* The guard of a job that has already answered. The live flag
             * belongs to the guard armed for the job that REPLACED it and
             * must not be touched here. */
            me->guard_stale -= 1U;
        }
        else
        {
            me->guard_live = false;   /* this event IS its expiry */
            if ( me->winflight && (!me->wreplied) && (!me->wdrop) )
            {
                KLOG_E(TAG, "the radio worker did not answer job %d inside "
                            "its guard", (int)me->jobs->wreq.type);
                me->wreplied   = true;
                me->out.evt    = (uint8_t)radio_out_of(me->jobs->wreq.type);
                me->out.status = (uint8_t)KODE_TIMEOUT;
                deliver        = true;
            }
            else if ( me->winflight && me->wdrop )
            {
                /* The link is lost and nobody waits for this job, but the
                 * teardown that is owed waits for it, and the channel stays
                 * busy meanwhile: say so, as the guard of a job somebody
                 * waits for does. */
                KLOG_W(TAG, "the radio worker did not return from job %d "
                            "inside its guard; the channel stays busy until "
                            "it does", (int)me->jobs->wreq.type);
            }
            else
            {
                /* A job already answered for: nothing to say. */
            }
        }
    }
    else
    {
        KqpWorkerDoneEvt const * const d = Q_EVT_CAST(KqpWorkerDoneEvt);

        if ( (!me->winflight) || (d->seq != me->wseq) )
        {
            KLOG_D(TAG, "worker answer seq %u is not the job's (%u); dropped",
                   (unsigned)d->seq, (unsigned)me->wseq);
        }
        else
        {
            bool const answers = (!me->wreplied) && (!me->wdrop);

            radio_guard_stop(me);
            me->winflight = false;
            me->wreplied  = false;
            me->wdrop     = false;
#if CONFIG_KODEOS_BLE
            /* The store is gone, or was never touched, once the worker is back. */
            if ( KDRV_RADIO_JOB_DISCONNECT == me->jobs->wreq.type )
            {
                me->ble_store_away = false;
            }
            /* What the app notified while the job ran waits for this. */
            kdrv_ble_app_kick();
#endif
            if ( answers )
            {
                me->out.evt    = (uint8_t)radio_out_of(me->jobs->wreq.type);
                me->out.status = d->status;
                deliver        = true;
            }
            else if ( me->disc_owed )
            {
                radio_disc_release(me);
            }
            else
            {
                KLOG_D(TAG, "the worker is back from a job nobody waited for");
            }
            /* A join's password does not stay beyond the job that used it. */
            (void)memset(me->jobs->wreq.pass, 0, sizeof(me->jobs->wreq.pass));
        }
    }
    return deliver;
}

/* ---- Across a jump: parking the C5 --------------------------------------
 * AO_Boot asks before it restarts the board and waits for exactly one
 * answer, so every road out of a park ends in radio_park_end(). */

/**
 * @brief The park is over: tell AO_Boot, once, and leave the note if the C5
 *        is parked.
 *
 * A park that succeeded stays PARK_DONE until the reset; any other end goes
 * back to PARK_IDLE and the AO carries on as before.
 *
 * @param me     The AO.
 * @param parked The C5 said OK to the park.
 * @param why    When it did not: what went wrong, for the log.
 */
static void radio_park_end(Radio * const me, bool parked, const char *why)
{
    me->park_phase = (uint8_t)PARK_IDLE;
    if ( parked )
    {
        me->park_phase = (uint8_t)PARK_DONE;
        kdrv_radio_note_leave((kdrv_radio_note_t)me->park_target);
#if CONFIG_KODEOS_WIFI
        /* The restart must not stop the station the C5 now keeps. */
        kdrv_wifi_keep_station_on_restart();
#endif
        KLOG_N(TAG, "C5 parked for the next firmware");
    }
    else
    {
        KLOG_N(TAG, "C5 was not parked (%s): the next firmware resets it",
               why);
    }
    boot_radio_parked();
}

/** @brief Ask the C5 to park; a request that cannot even be sent ends the park. */
static void radio_park_send(Radio * const me)
{
    me->park_phase = (uint8_t)PARK_ASKED;
    if ( radio_svc_send(me, KODE_SVC_SELF, KODE_SELF_PARK, NULL, 0U) != KODE_OK )
    {
        radio_park_end(me, false, "the C5 could not be asked");
    }
}

static bool radio_amp_apply(Radio * const me);

/**
 * @brief Take the amplifier's enable low, then ask the C5 to park.
 *
 * The C5 outlives the reset and keeps IO4 as it was, and the next firmware
 * walks the codec through a power-up and a power-down at its bring-up: with
 * the amplifier enabled that is a noise on every jump. The next AO_Audio asks
 * it back before its first sound (radio_amp_on()). A worker that cannot take
 * the job skips this rather than hold the jump.
 *
 * @param me The AO.
 */
static void radio_park_mute(Radio * const me)
{
    me->amp_wished = true;
    me->amp_off    = true;
    me->park_phase = (uint8_t)PARK_MUTE;
    if ( !radio_amp_apply(me) )
    {
        radio_park_send(me);
    }
}

/**
 * @brief A jump is coming (RADIO_PARK_REQ_SIG), with the link up.
 *
 * BLE is hung up first when the worker is free, so the phone sees the
 * disconnect now, then the amplifier's enable goes low (radio_park_mute()).
 * A busy worker (a scan, an update) skips those steps rather than hold the
 * jump. The C5 is asked either way.
 *
 * @param me     The AO.
 * @param target The role of the firmware the restart goes to.
 */
static void radio_park_begin(Radio * const me, uint8_t target)
{
    if ( (uint8_t)PARK_IDLE != me->park_phase )
    {
        KLOG_W(TAG, "a park is already under way or done");
        boot_radio_parked();
        return;
    }
    me->park_target = target;
    me->park_phase  = (uint8_t)PARK_QUIET;
#if CONFIG_KODEOS_BLE
    if ( !me->busy && (kdrv_ble_state() != KDRV_BLE_OFF)
        && radio_submit(me, KDRV_RADIO_JOB_BLE_QUIET) )
    {
        return;   /* its completion goes on to the amplifier */
    }
#endif
    radio_park_mute(me);
}

/** @brief The C5 answered the park. Anything but OK leaves no note. */
static void radio_park_answer(Radio * const me, uint8_t status)
{
    if ( (uint8_t)PARK_ASKED != me->park_phase )
    {
        return;   /* stale: the park ended already */
    }
    if ( KODE_ST_OK == status )
    {
        radio_park_end(me, true, "");
    }
    else
    {
        radio_park_end(me, false, "the C5 refused");
    }
}

/** @brief The C5 did not answer the park in time. */
static void radio_park_lost(Radio * const me)
{
    radio_park_end(me, false, "no answer");
}

/* ---- The coprocessor's field table --------------------------------------- */

/**
 * @brief Serve one field request from kdrv_c5's snapshot and answer it.
 *
 * Nothing here crosses the link - the snapshot is what the last bring-up
 * left - so this is served in every state, and a row whose group the
 * snapshot did not fill is refused rather than invented. SET is refused by
 * the driver for every row and still answered, because the console closes
 * its command on the answer.
 */
static void radio_field_serve(RadioFieldReqEvt const *q)
{
    kdrv_c5_field_t const f  = (kdrv_c5_field_t)q->field;
    int32_t               v  = q->value;
    kode_status_t         rc = KODE_DENIED;

    switch ( q->op )
    {
        case FIELD_OP_GET: {
            rc = kdrv_c5_get(f, &v);
            break;
        }
        case FIELD_OP_SET: {
            rc = kdrv_c5_set(f, v);
            if ( KODE_OK == rc )
            {
                rc = kdrv_c5_get(f, &v);
            }
            break;
        }
        case FIELD_OP_DUMP: {
            kdrv_c5_dump();
            rc = KODE_OK;
            break;
        }
        default: {
            break;
        }
    }

    if ( NULL == q->who )
    {
        return;
    }
    RadioFieldEvt *ev = Q_NEW_X(RadioFieldEvt, 0U, RADIO_FIELD_RSP_SIG);
    if ( NULL == ev )
    {
        KLOG_E(TAG, "out of events; a c5 field answer is lost");
        return;
    }
    ev->op    = q->op;
    ev->field = q->field;
    ev->rc    = (uint8_t)rc;
    ev->value = v;
    QACTIVE_POST(q->who, &ev->super, NULL);
}

/* ---- The production row -------------------------------------------------- */

/** @brief Nothing in flight, nothing owed. */
static void radio_test_clear(Radio * const me)
{
    me->test_phase   = (uint8_t)TEST_IDLE;
    me->test_pending = false;
    me->test_job     = false;
    me->test_user    = false;
}

/**
 * @brief The verdict, and the end of the row.
 *
 * Logged before it is published, because the log is what the operator
 * reads: the console prints `result=fail` and nothing else, and the reason
 * has to be on the line above it.
 */
static void c5_test_end(Radio * const me, bool ok, const char *why)
{
    if ( ok )
    {
        KLOG_N(TAG, "test c5: PASS");
    }
    else
    {
        KLOG_E(TAG, "test c5: FAIL - %s", why);
    }
    TestResultEvt *r = Q_NEW_X(TestResultEvt, 0U, TEST_RESULT_SIG);
    if ( NULL != r )
    {
        (void)snprintf(r->name, sizeof(r->name), "%s", "c5");
        r->ok = ok;
        r->origin = TEST_ORIGIN_AUTOTEST;
        QACTIVE_PUBLISH(&r->super, &me->super);
    }
    else
    {
        KLOG_E(TAG, "out of events; the c5 verdict is lost");
    }
    radio_test_clear(me);
}

/** @brief Submit the job the row's phase is waiting to run. Channel free. */
static void c5_test_step(Radio * const me)
{
    if ( (uint8_t)TEST_INFO == me->test_phase )
    {
        if ( radio_submit(me, KDRV_RADIO_JOB_C5_INFO) )
        {
            me->test_job = true;
        }
        else
        {
            c5_test_end(me, false, "the worker refused the info job");
        }
    }
#if CONFIG_KODEOS_WIFI
    else if ( (uint8_t)TEST_SCAN == me->test_phase )
    {
        if ( radio_submit(me, KDRV_RADIO_JOB_WIFI_SCAN) )
        {
            me->test_job = true;
        }
        else
        {
            c5_test_end(me, false, "the worker refused the scan");
        }
    }
#endif
#if CONFIG_KODEOS_BLE
    else if ( (uint8_t)TEST_BLE_START == me->test_phase )
    {
        if ( kdrv_ble_state() != KDRV_BLE_OFF )
        {
            /* Somebody already brought the stack up this boot - the `ble`
             * flag, or a `ble_scan` - and it cannot be brought up twice, so
             * the row judges what is there and moves on. */
            c5_test_judge_ble_start(me, true);
        }
        else if ( radio_submit(me, KDRV_RADIO_JOB_BLE_START) )
        {
            me->test_job = true;
        }
        else
        {
            c5_test_end(me, false, "the worker refused the BLE start");
        }
    }
    else if ( (uint8_t)TEST_BLE_SCAN == me->test_phase )
    {
        kdrv_radio_req_t req;
        (void)memset(&req, 0, sizeof(req));
        req.type = KDRV_RADIO_JOB_BLE_SCAN;
        req.len  = KODE_BLE_SCAN_MS;
        if ( radio_submit_req(me, &req) )
        {
            me->test_job = true;
        }
        else
        {
            c5_test_end(me, false, "the worker refused the BLE scan");
        }
    }
#endif
    else
    {
        /* idle, or waiting on the link: nothing to submit */
    }
}

/** @brief One numeric row of the snapshot, or -1 for one it did not fill. */
static long c5_num(kdrv_c5_field_t f)
{
    int32_t v = 0;

    if ( kdrv_c5_get(f, &v) != KODE_OK )
    {
        return -1L;
    }
    return (long)v;
}

/** @brief One of the snapshot's strings into @p buf, or a dash. */
static const char *c5_str(kode_status_t (*get)(char *, size_t),
                          char *buf, size_t len)
{
    if ( get(buf, len) != KODE_OK )
    {
        return "-";
    }
    return buf;
}

/** @brief esp_reset_reason_t, by name, for the one line that prints it. */
static const char *c5_reset_name(long reason)
{
    static const char *const NAMES[] = {
        "unknown", "power-on", "external", "software", "panic", "int-wdt",
        "task-wdt", "wdt", "deep-sleep", "brownout", "sdio", "usb", "jtag",
        "efuse", "power-glitch", "cpu-lockup",
    };
    if ( (0L > reason) || ((long)(sizeof(NAMES) / sizeof(NAMES[0])) <= reason) )
    {
        return "?";
    }
    return NAMES[reason];
}

/**
 * @brief Every reading the snapshot holds, as log lines.
 *
 * All of it, before any judging: the row is the production line's record
 * of the unit's second chip, and a unit that fails at the fourth check has
 * still told the log who it is. A row the snapshot did not fill prints -1
 * or a dash, never a zero that could be read as a measurement.
 */
static void c5_test_log(void)
{
    char    target[16];
    char    project[32];
    char    version[32];
    char    idf[32];
    char    date[16];
    char    time[16];
    char    shahex[9];
    uint8_t sha[32];
    uint8_t mac[6];

    KLOG_N(TAG, "test c5: chip=%s id=%ld",
           c5_str(kdrv_c5_target, target, sizeof(target)),
           c5_num(KDRV_C5_CHIP_ID));
    KLOG_N(TAG, "test c5: fw slave=%ld.%ld.%ld host=%ld.%ld.%ld",
           c5_num(KDRV_C5_FW_MAJOR), c5_num(KDRV_C5_FW_MINOR),
           c5_num(KDRV_C5_FW_PATCH), c5_num(KDRV_C5_HOST_MAJOR),
           c5_num(KDRV_C5_HOST_MINOR), c5_num(KDRV_C5_HOST_PATCH));
    if ( kdrv_c5_elf_sha(sha) == KODE_OK )
    {
        (void)snprintf(shahex, sizeof(shahex), "%02x%02x%02x%02x",
                       (unsigned)sha[0], (unsigned)sha[1],
                       (unsigned)sha[2], (unsigned)sha[3]);
    }
    else
    {
        (void)snprintf(shahex, sizeof(shahex), "%s", "-");
    }
    KLOG_N(TAG, "test c5: app project=%s version=%s idf=%s date=%s time=%s sha=%s",
           c5_str(kdrv_c5_project_name, project, sizeof(project)),
           c5_str(kdrv_c5_app_version, version, sizeof(version)),
           c5_str(kdrv_c5_idf_ver, idf, sizeof(idf)),
           c5_str(kdrv_c5_build_date, date, sizeof(date)),
           c5_str(kdrv_c5_build_time, time, sizeof(time)),
           shahex);
    if ( kdrv_c5_mac(mac) == KODE_OK )
    {
        KLOG_N(TAG, "test c5: mac=%02x:%02x:%02x:%02x:%02x:%02x",
               (unsigned)mac[0], (unsigned)mac[1], (unsigned)mac[2],
               (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5]);
    }
    else
    {
        KLOG_N(TAG, "test c5: mac=-");
    }
    KLOG_N(TAG, "test c5: reset=%ld (%s) link_ms=%ld",
           c5_num(KDRV_C5_RESET_REASON),
           c5_reset_name(c5_num(KDRV_C5_RESET_REASON)),
           c5_num(KDRV_C5_LINK_MS));
    KLOG_N(TAG, "test c5: heap free=%ldKB largest=%ldKB",
           c5_num(KDRV_C5_HEAP_FREE_KB), c5_num(KDRV_C5_HEAP_LARGEST_KB));
    KLOG_N(TAG, "test c5: sdio width=%ld khz=%ld",
           c5_num(KDRV_C5_SDIO_WIDTH), c5_num(KDRV_C5_SDIO_KHZ));
    KLOG_N(TAG, "test c5: sd_cd=%ld", c5_num(KDRV_C5_SD_CD));
}

/**
 * @brief Judge the fresh snapshot, in order, and go on to the scan or stop.
 *
 * First failure wins and names itself. The order is the order a fault
 * shows: a chip that is not a C5 makes every later check meaningless, a
 * stock slave is the most likely finding on a unit that was never flashed,
 * a version mismatch the next, and the SDIO width and the card pin are
 * about the board around the chip.
 */
static void c5_test_judge_info(Radio * const me)
{
    char why[128];
    char project[32];
    long v;

    c5_test_log();

    if ( c5_num(KDRV_C5_VALID) != 1L )
    {
        c5_test_end(me, false, "the coprocessor would not describe itself");
        return;
    }
    v = c5_num(KDRV_C5_CHIP_ID);
    if ( (long)KDRV_C5_CHIP_ESP32C5 != v )
    {
        char target[16];
        (void)snprintf(why, sizeof(why), "chip id %ld (%s) is not an ESP32-C5",
                       v, c5_str(kdrv_c5_target, target, sizeof(target)));
        c5_test_end(me, false, why);
        return;
    }
    if ( c5_num(KDRV_C5_APP_DESC) != 1L )
    {
        c5_test_end(me, false, "stock or old slave: the app-descriptor RPC "
                               "went unanswered");
        return;
    }
    if ( c5_num(KDRV_C5_PROJECT_KODE) != 1L )
    {
        (void)snprintf(why, sizeof(why),
                       "project_name '%s' is not %s: stock esp-hosted slave",
                       c5_str(kdrv_c5_project_name, project, sizeof(project)),
                       KDRV_C5_PROJECT_NAME);
        c5_test_end(me, false, why);
        return;
    }
    if ( (c5_num(KDRV_C5_FW_MAJOR) != c5_num(KDRV_C5_HOST_MAJOR))
        || (c5_num(KDRV_C5_FW_MINOR) != c5_num(KDRV_C5_HOST_MINOR))
        || (c5_num(KDRV_C5_FW_PATCH) != c5_num(KDRV_C5_HOST_PATCH)) )
    {
        (void)snprintf(why, sizeof(why),
                       "slave esp-hosted %ld.%ld.%ld is not the host's %ld.%ld.%ld",
                       c5_num(KDRV_C5_FW_MAJOR), c5_num(KDRV_C5_FW_MINOR),
                       c5_num(KDRV_C5_FW_PATCH), c5_num(KDRV_C5_HOST_MAJOR),
                       c5_num(KDRV_C5_HOST_MINOR), c5_num(KDRV_C5_HOST_PATCH));
        c5_test_end(me, false, why);
        return;
    }
    if ( (c5_num(KDRV_C5_SDIO_WIDTH) != (long)KBSP_C5_SDIO_WIDTH)
        || (c5_num(KDRV_C5_SDIO_KHZ) != (long)KBSP_C5_SDIO_KHZ) )
    {
        (void)snprintf(why, sizeof(why),
                       "SDIO came up %ld-bit @%ldkHz, not %u-bit @%ukHz: a data "
                       "line is not reaching the C5",
                       c5_num(KDRV_C5_SDIO_WIDTH), c5_num(KDRV_C5_SDIO_KHZ),
                       (unsigned)KBSP_C5_SDIO_WIDTH, (unsigned)KBSP_C5_SDIO_KHZ);
        c5_test_end(me, false, why);
        return;
    }
    /* The card's supply is the P4's and not this row's to judge (the `sd`
     * row mounting a card proves it); the RPC GPIO service is still proved,
     * by the one card pin the C5 reads. */
    v = c5_num(KDRV_C5_SD_CD);
    if ( 0L > v )
    {
        c5_test_end(me, false, "the RPC GPIO service did not answer");
        return;
    }

#if CONFIG_KODEOS_WIFI
    me->test_phase = (uint8_t)TEST_SCAN;
    c5_test_step(me);
#elif CONFIG_KODEOS_BLE
    /* No WiFi built, so no scan to judge the antenna by: BLE is the next
     * thing on this link the row can prove. */
    me->test_phase = (uint8_t)TEST_BLE_START;
    c5_test_step(me);
#else
    c5_test_end(me, true, NULL);
#endif
}

#if CONFIG_KODEOS_WIFI
/**
 * @brief Judge the row's own scan: the antenna, in the only way the board
 *        can see it.
 *
 * A loose or missing antenna does not make the radio deaf, it makes it
 * 20-30 dB deafer, so the check is the strongest network against a floor
 * and not "anything at all". The count and the strongest network are the
 * readings; whether a 5 GHz network was among them says the second band
 * is alive, and is a reading too.
 */
static void c5_test_judge_scan(Radio * const me, bool ok)
{
    char           why[96];
    uint8_t const  n    = kdrv_wifi_scan_count();
    int            best = -1;
    bool           five = false;
    kdrv_wifi_ap_t top;

    (void)memset(&top, 0, sizeof(top));
    for ( uint8_t i = 0U; i < n; ++i )
    {
        kdrv_wifi_ap_t ap;
        if ( !kdrv_wifi_scan_get(i, &ap) )
        {
            break;
        }
        if ( 14U < ap.channel )
        {
            five = true;
        }
        if ( (0 > best) || (ap.rssi > top.rssi) )
        {
            best = (int)i;
            top  = ap;
        }
    }

    if ( 0 <= best )
    {
        unsigned fiveghz = 0U;
        if ( five )
        {
            fiveghz = 1U;
        }
        KLOG_N(TAG, "test c5: scan n=%u best='%s' rssi=%d ch=%u 5ghz=%u",
               (unsigned)n, top.ssid, (int)top.rssi, (unsigned)top.channel,
               fiveghz);
    }
    else
    {
        KLOG_N(TAG, "test c5: scan n=%u best=- rssi=- ch=- 5ghz=0", (unsigned)n);
    }

    if ( !ok )
    {
        c5_test_end(me, false, "the scan failed");
    }
    else if ( me->test_user )
    {
        /* The owner's row: the sweep completed, and what it heard depends
         * on where the unit is rather than on the unit. Judged by nobody. */
#if CONFIG_KODEOS_BLE
        me->test_phase = (uint8_t)TEST_BLE_START;
        c5_test_step(me);
#else
        c5_test_end(me, true, NULL);
#endif
    }
    else if ( 0 > best )
    {
        c5_test_end(me, false, "no network in range: is the antenna connected?");
    }
    else if ( (int)top.rssi < KODE_C5_TEST_RSSI_MIN_DBM )
    {
        (void)snprintf(why, sizeof(why),
                       "the strongest network is %d dBm, under the %d dBm floor: "
                       "check the antenna",
                       (int)top.rssi, (int)KODE_C5_TEST_RSSI_MIN_DBM);
        c5_test_end(me, false, why);
    }
    else
    {
#if CONFIG_KODEOS_BLE
        me->test_phase = (uint8_t)TEST_BLE_START;
        c5_test_step(me);
#else
        c5_test_end(me, true, NULL);
#endif
    }
}
#endif /* CONFIG_KODEOS_WIFI */

#if CONFIG_KODEOS_BLE
/** @brief kdrv_ble_state_t by name, for the one line that prints it. */
static const char *c5_ble_state_name(kdrv_ble_state_t st)
{
    const char *name = "?";
    switch ( st )
    {
        case KDRV_BLE_OFF:         name = "off";         break;
        case KDRV_BLE_IDLE:        name = "idle";        break;
        case KDRV_BLE_ADVERTISING: name = "advertising"; break;
        case KDRV_BLE_CONNECTED:   name = "connected";   break;
        default:                                         break;
    }
    return name;
}

/**
 * @brief Judge the BLE stack's start: the C5's controller, and the host
 *        that has to synchronise with it before it owns an address.
 *
 * The address is the check, not the start's return code: NimBLE reports the
 * controller up, then synchronises with it over HCI, and a controller that
 * never completes that leaves a stack that looks started and cannot work.
 * The address is what the handshake yields. The stack stays up afterwards:
 * it cannot be taken down again this boot (kdrv_ble_quiet()).
 */
static void c5_test_judge_ble_start(Radio * const me, bool ok)
{
    uint8_t addr[6];

    if ( !ok )
    {
        KLOG_N(TAG, "test c5: ble addr=- state=%s",
               c5_ble_state_name(kdrv_ble_state()));
        c5_test_end(me, false, "the C5's BT controller would not start");
        return;
    }
    if ( !kdrv_ble_addr(addr) )
    {
        KLOG_N(TAG, "test c5: ble addr=- state=%s",
               c5_ble_state_name(kdrv_ble_state()));
        c5_test_end(me, false, "NimBLE did not synchronise with the C5's "
                               "controller: no own address");
        return;
    }
    KLOG_N(TAG, "test c5: ble addr=%02x:%02x:%02x:%02x:%02x:%02x state=%s",
           (unsigned)addr[0], (unsigned)addr[1], (unsigned)addr[2],
           (unsigned)addr[3], (unsigned)addr[4], (unsigned)addr[5],
           c5_ble_state_name(kdrv_ble_state()));
    me->test_phase = (uint8_t)TEST_BLE_SCAN;
    c5_test_step(me);
}

/**
 * @brief Judge the row's BLE listen: the other radio, heard from.
 *
 * At least one advertiser, because a controller that synchronises but never
 * receives is a fault the address cannot see; the line keeps a phone or a
 * beacon within reach. The count and the strongest advertiser are readings.
 */
static void c5_test_judge_ble_scan(Radio * const me, bool ok)
{
    uint8_t const  n    = kdrv_ble_scan_count();
    int            best = -1;
    kdrv_ble_dev_t top;

    (void)memset(&top, 0, sizeof(top));
    for ( uint8_t i = 0U; i < n; ++i )
    {
        kdrv_ble_dev_t d;
        if ( !kdrv_ble_scan_get(i, &d) )
        {
            break;
        }
        if ( (0 > best) || (d.rssi > top.rssi) )
        {
            best = (int)i;
            top  = d;
        }
    }

    if ( 0 <= best )
    {
        KLOG_N(TAG, "test c5: ble scan n=%u best=%02x:%02x:%02x:%02x:%02x:%02x "
                    "rssi=%d name='%s'",
               (unsigned)n,
               (unsigned)top.addr[0], (unsigned)top.addr[1], (unsigned)top.addr[2],
               (unsigned)top.addr[3], (unsigned)top.addr[4], (unsigned)top.addr[5],
               (int)top.rssi, top.name);
    }
    else
    {
        KLOG_N(TAG, "test c5: ble scan n=%u best=- rssi=- name=-", (unsigned)n);
    }

    if ( !ok )
    {
        c5_test_end(me, false, "the BLE scan failed");
    }
    else if ( (0 > best) && !me->test_user )
    {
        c5_test_end(me, false, "no BLE advertiser in range: put a phone or a "
                               "beacon near the line");
    }
    else
    {
        c5_test_end(me, true, NULL);
    }
}
#endif /* CONFIG_KODEOS_BLE */

/**
 * @brief The bring-up the row asked for did not happen. Name the reason.
 *
 * The status is the worker's, and each one has a different fix: a refused
 * connect is a worker that would not take the job; a timeout is a C5 that never
 * answered, which on a fresh unit means no slave firmware; anything else is
 * esp-hosted itself.
 */
static void c5_test_link_failed(Radio * const me, kode_status_t status)
{
    if ( (uint8_t)TEST_LINK != me->test_phase )
    {
        return;
    }
    if ( KODE_DENIED == status )
    {
        /* The worker would not take the connect at all. */
        c5_test_end(me, false, "the link could not be asked for; the radio "
                               "worker refused the connect");
    }
    else if ( KODE_TIMEOUT == status )
    {
        c5_test_end(me, false, "no CP_INIT in 10 s: is the slave firmware "
                               "flashed?");
    }
    else
    {
        c5_test_end(me, false, "esp-hosted could not bring the link up");
    }
}

#if CONFIG_KODEOS_WIFI
/**
 * @brief Drop the Wi-Fi request that waits for the radio, and wipe its password.
 * @param me The AO.
 */
static void wifi_owed_clear(Radio * const me)
{
    me->wifi_owed     = (uint8_t)WOWED_NONE;
    me->owed_remember = false;
    me->owed_ssid[0]  = '\0';
    (void)memset(me->owed_pass, 0, sizeof(me->owed_pass));
}

/**
 * @brief Keep a join or an autojoin the radio cannot serve now.
 * @param me The AO.
 * @param e  A WIFI_JOIN_REQ_SIG or WIFI_AUTO_REQ_SIG; it replaces the one kept.
 */
static void wifi_owe(Radio * const me, QEvt const * const e)
{
    wifi_owed_clear(me);
    me->wifi_left = false;   /* somebody wants a network again */
    if ( (QSignal)WIFI_JOIN_REQ_SIG == e->sig )
    {
        WifiJoinEvt const * const j = Q_EVT_CAST(WifiJoinEvt);

        me->wifi_owed     = (uint8_t)WOWED_JOIN;
        me->owed_remember = j->remember;
        (void)snprintf(me->owed_ssid, sizeof(me->owed_ssid), "%s", j->ssid);
        (void)snprintf(me->owed_pass, sizeof(me->owed_pass), "%s", j->pass);
    }
    else
    {
        me->wifi_owed = (uint8_t)WOWED_AUTO;
    }
}

/**
 * @brief Post the owed Wi-Fi request to the AO as it came.
 * @param me The AO.
 * @return true when a request was posted; false when none waits or the pool is empty.
 */
static bool wifi_owed_replay(Radio * const me)
{
    bool posted = false;

    if ( (uint8_t)WOWED_AUTO == me->wifi_owed )
    {
        wifi_owed_clear(me);
        wifi_auto_self();
        posted = true;
    }
    else if ( (uint8_t)WOWED_JOIN == me->wifi_owed )
    {
        WifiJoinEvt * const j = Q_NEW_X(WifiJoinEvt, 0U, WIFI_JOIN_REQ_SIG);

        if ( NULL != j )
        {
            j->remember = me->owed_remember;
            (void)snprintf(j->ssid, sizeof(j->ssid), "%s", me->owed_ssid);
            (void)snprintf(j->pass, sizeof(j->pass), "%s", me->owed_pass);
            wifi_owed_clear(me);
            QACTIVE_POST(&me->super, &j->super, &me->super);
            posted = true;
        }
        else
        {
            KLOG_W(TAG, "out of events; the owed Wi-Fi join waits");
        }
    }
    else
    {
        /* nothing waits */
    }
    return posted;
}

/**
 * @brief Whether an owed Wi-Fi request can be made now.
 * @param me The AO.
 * @return true when the reconcile is over and no update, install or row owns the channel.
 */
static bool wifi_owed_ready(Radio const * const me)
{
    return ((uint8_t)WOWED_NONE != me->wifi_owed) && scan_channel_free(me);
}
#endif /* CONFIG_KODEOS_WIFI */

/**
 * @brief Record whether a service is wanted, and remember it.
 *
 * A request records the desire whether or not it can be acted on now: a
 * refusal is about this moment, the flag about every moment after. Only a
 * change is committed (the store is flash). A change cancels the opposite
 * debt: turning a service on cancels a stop it owed, off a start it owed.
 * The operator's claim on the link (`radio_up`) is not touched here: it is
 * cleared only at the two no-link entries.
 *
 * @param me  The AO.
 * @param svc Which service.
 * @param on  What the request asked for.
 */
#if CONFIG_KODEOS_WIFI || CONFIG_KODEOS_BLE
static void radio_flag_set(Radio * const me, svc_t svc, bool on)
{
    bool * const flag  = (SVC_WIFI == svc) ? &me->wifi_want
                                           : &me->ble_want;
    bool * const stopp = (SVC_WIFI == svc) ? &me->wifi_stop_pending
                                           : &me->ble_stop_pending;
    bool * const startp = (SVC_WIFI == svc) ? &me->wifi_start_pending
                                            : &me->ble_start_pending;
    const char * const key = (SVC_WIFI == svc) ? KSET_KEY_WIFI : KSET_KEY_BLE;

    if ( *flag == on )
    {
        return;
    }
    *flag = on;
#if CONFIG_KODEOS_BLE
    if ( SVC_BLE == svc )
    {
        ble_on_note(on);
        me->ble_asked = on;
    }
#endif
    if ( on )
    {
        *stopp = false;
    }
    else
    {
        *startp = false;
#if CONFIG_KODEOS_WIFI
        if ( SVC_WIFI == svc )
        {
            wifi_owed_clear(me);
            wifi_retry_stop(me);
        }
#endif
    }
    /* The request that changed it is the setting, so a store that refuses is
     * logged one layer down and life goes on. */
    (void)khal_nvs_set_u8(KSET_NAMESPACE, key, on ? 1U : 0U);
}

/** @brief Remember a stop the machine was asked for and could not carry out. */
static void radio_stop_owed(Radio * const me, svc_t svc)
{
    if ( SVC_WIFI == svc )
    {
        me->wifi_stop_pending = true;
    }
    else
    {
        me->ble_stop_pending = true;
    }
}

/** @brief Remember a start the machine was asked for and could not carry out. */
static void radio_start_owed(Radio * const me, svc_t svc)
{
    if ( SVC_WIFI == svc )
    {
        me->wifi_start_pending = true;
    }
    else
    {
        me->ble_start_pending = true;
    }
}
#endif /* CONFIG_KODEOS_WIFI || CONFIG_KODEOS_BLE */

/** @brief Bring-ups a lost link gets by itself in RADIO_RELINK_WINDOW_MS. */
#define RADIO_RELINK_TRIES      3U

/** @brief The window they are counted in, in ms. */
#define RADIO_RELINK_WINDOW_MS  600000U

/** @brief A wait over with the worker still busy looks again this soon, in ms. */
#define RADIO_RELINK_BUSY_MS    5000U

/**
 * @brief Whether a bring-up by itself is left in the window, opening a new
 *        window when the last one is over.
 * @param me The AO.
 * @return true when one may be spent.
 */
static bool radio_relink_left(Radio * const me)
{
    uint32_t const now = khal_time_ms();

    if ( (0U == me->relink_n) || ((now - me->relink_t0) >= RADIO_RELINK_WINDOW_MS) )
    {
        me->relink_n  = 0U;
        me->relink_t0 = now;
    }
    return RADIO_RELINK_TRIES > me->relink_n;
}

/**
 * @brief A link was lost: decide whether it is built again by itself.
 *
 * A C5 that went mute (KODE_TIMEOUT) is rebuilt: building the link resets the
 * chip, the one thing that brings it back. So is a transport that dropped
 * (KODE_ERROR) when a service wants it; a link only an operator asked for
 * stays down. A C5 mute again before answering a job stays down, and every
 * bring-up counts against RADIO_RELINK_TRIES. With no service wanted and
 * nobody having asked for the link, it stays down.
 *
 * @param me   The AO.
 * @param lost The status LINK_LOST came with.
 */
static void radio_relink_note(Radio * const me, kode_status_t lost)
{
    bool const wanted = me->wifi_want || me->ble_want;

    if ( (KODE_TIMEOUT != lost) && (KODE_ERROR != lost) )
    {
        return;
    }
    if ( (KODE_TIMEOUT == lost) && me->relinked )
    {
        KLOG_E(TAG, "the C5 went mute again after a reset; the link stays "
                    "down until it is asked for");
        me->relink_dead = wanted || me->link_asked;
        return;
    }
    if ( (!wanted) && ((KODE_ERROR == lost) || (!me->link_asked)) )
    {
        return;
    }
    if ( !radio_relink_left(me) )
    {
        KLOG_E(TAG, "the link to the C5 was lost %u times in a row; it stays "
                    "down until it is asked for", (unsigned)RADIO_RELINK_TRIES);
        me->relink_dead = true;
        return;
    }
    if ( KODE_TIMEOUT == lost )
    {
        KLOG_W(TAG, "the C5 went mute; resetting it and building the link again, "
                    "once");
    }
    else
    {
        KLOG_W(TAG, "the link to the C5 dropped; building it again");
    }
    ++me->relink_n;
    me->relink_owed = true;
}

/**
 * @brief The link is down with no bring-up owed: wait for the next, if one is
 *        left and somebody wants it.
 *
 * Every way into `failed` passes here, so a bring-up that failed is tried again
 * by the same rules as a link that was lost.
 *
 * @param me The AO.
 */
static void radio_relink_plan(Radio * const me)
{
    /* The wait before the bring-up after relink_n of them, in ms: the first
     * follows the loss at once and never waits. */
    static const uint32_t RADIO_RELINK_WAIT_MS[RADIO_RELINK_TRIES] = { 30000U, 30000U, 120000U };

    if ( me->relink_owed || me->relink_dead
        || ((!me->wifi_want) && (!me->ble_want))
        || ((uint8_t)PARK_IDLE != me->park_phase) )
    {
        return;
    }
    if ( !radio_relink_left(me) )
    {
        KLOG_E(TAG, "the C5 link would not come back; it stays down until it "
                    "is asked for");
        me->relink_dead = true;
    }
    else if ( me->lp_rest )
    {
        me->relink_wake = true;
    }
    else
    {
        (void)QTimeEvt_disarm(&me->relink_tmo);
        QTimeEvt_armX(&me->relink_tmo,
                      KQP_TICKS_PER_MS(RADIO_RELINK_WAIT_MS[me->relink_n]), 0U);
    }
}

/**
 * @brief A job came back: if the C5 answered it, the retry is earned again.
 *
 * A silence after an answered job is a new silence, and gets its own reset.
 * Called for every outcome a state with the link up sees.
 *
 * @param me  The AO.
 * @param job The outcome.
 */
static void radio_relink_judge(Radio * const me, RadioOutcome const * const job)
{
    if ( ((uint8_t)RADIO_OUT_LINK_LOST != job->evt)
        && ((uint8_t)KODE_OK == job->status) )
    {
        me->relinked = false;
    }
}

/**
 * @brief Start the next service the flags ask for, one at a time.
 *
 * Called from Radio_up's entry (via posted requests: an entry action must not
 * start a job) and from the BLE *_DONE handlers while the chain is in its BLE
 * step. BLE first, a short handshake, then the autojoin, which can hold the
 * channel for tens of seconds and ends the chain. With the `wifi_join` setting
 * off the wifi step still publishes the state: whoever asked for WiFi waits on
 * that event.
 */
static void radio_reconcile_next(Radio * const me)
{
#if CONFIG_KODEOS_WIFI
    if ( me->adopt )
    {
        /* The C5 still holds the station the last firmware left: take it
         * over first, with no scan, then carry on with BLE. */
        me->adopt = false;
        if ( radio_submit(me, KDRV_RADIO_JOB_WIFI_ADOPT) )
        {
            me->recon = (uint8_t)RECON_ADOPT;
            return;
        }
    }
#endif
#if CONFIG_KODEOS_BLE
    if ( ((uint8_t)RECON_START == me->recon) && me->ble_want )
    {
        me->recon = (uint8_t)RECON_BLE;
        ble_start_request();
        return;
    }
#endif
    me->recon = (uint8_t)RECON_DONE;
#if CONFIG_KODEOS_WIFI
    if ( wifi_owed_ready(me) && wifi_owed_replay(me) )
    {
        /* The request that waited for the link is the wifi step. */
        me->wifi_adopted = false;
    }
    else if ( me->wifi_want )
    {
        if ( me->wifi_adopted )
        {
            /* Already on its network, and said so: the round has nothing
             * left to find. */
            me->wifi_adopted = false;
        }
        else if ( me->join_auto )
        {
            wifi_auto_self();
        }
        else
        {
            wifi_publish_state(me);
        }
    }
    else
    {
        /* WiFi is not wanted: nothing to do */
    }
#endif
#if RADIO_ASKS
    if ( (uint8_t)SCAN_ASK_OWED == me->ask_scan )
    {
        /* A scan asked while the link was down or the chain ran. Posted, as
         * the chain's own steps are: this may run from an entry action. */
        static const QEvt owed = QEVT_INITIALIZER(RADIO_SCAN_OWED_SIG);

        QACTIVE_POST(&me->super, &owed, &me->super);
    }
#endif
}

#if CONFIG_KODEOS_WIFI
/**
 * @brief The adoption ended (WIFI_JOIN_DONE while the chain waits on it).
 *
 * Either way the chain goes on from its start: BLE, then the wifi step -
 * which is state alone after an adoption that worked, and the autojoin round
 * after one that did not.
 *
 * @param me The AO.
 * @param rc What the adoption answered.
 */
static void radio_adopt_done(Radio * const me, kode_status_t rc)
{
    me->recon = (uint8_t)RECON_START;
    if ( KODE_OK == rc )
    {
        KLOG_N(TAG, "Wi-Fi adopted from the parked C5");
        me->wifi_adopted = true;
        radio_hint_learn(me);
        /* The C5 may hold the power save the last firmware left it in, so
         * what this side believes it holds is the opposite of what it wants:
         * radio_fast_apply() then pays the difference as after a join. */
        me->ps_fast = me->ps_on && !me->fast_want;
        wifi_publish_state(me);
    }
    else
    {
        const char *why = "failed";
        if ( KODE_TIMEOUT == rc )
        {
            why = "timed out";
        }
        else if ( KODE_DENIED == rc )
        {
            why = "refused";
        }
        else
        {
            /* some other failure keeps its generic name */
        }
        KLOG_N(TAG, "adoption failed (%s): joining", why);
    }
    radio_reconcile_next(me);
}
#endif

#if CONFIG_KODEOS_BLE
/**
 * @brief Hand the reconcile's baton on, now that its BLE step has ended.
 *
 * Called only where a step has ended: a completed job, or a job the worker
 * would not take (nothing will answer it). Not from a busy refusal: the
 * autojoin would be posted into an occupied channel and dropped;
 * radio_reconcile_retry() asks again once the channel is free. The `recon`
 * guard keeps a plain `ble_up` from dragging an autojoin behind it.
 */
static void radio_reconcile_ble_done(Radio * const me)
{
    if ( (uint8_t)RECON_BLE == me->recon )
    {
        radio_reconcile_next(me);
    }
}
#endif

/**
 * @brief Does anything still use the link to the C5?
 *
 * A wanted service, somebody who asked for the link by itself, a job, a row
 * or an install in flight, and, in a firmware that holds its C5, the
 * amplifier: taking the link down is followed by holding the C5, and a boot
 * in which a sound has asked for the C5 keeps it, link and all.
 *
 * @param me The AO.
 * @return true when the link can come down and nobody would notice.
 */
static bool radio_link_unused(Radio const * const me)
{
    bool unused = !me->wifi_want && !me->ble_want && !me->link_asked
                  && !me->busy
                  && ((uint8_t)TEST_IDLE == me->test_phase)
                  && ((uint8_t)INST_IDLE == me->inst)
                  && ((uint8_t)OTA_IDLE == me->phase);

#if defined(CONFIG_KODEOS_C5_HOLD)
    if ( me->amp_asked )
    {
        unused = false;
    }
#endif
#if RADIO_ASKS
    if ( (uint8_t)SCAN_ASK_NONE != me->ask_scan )
    {
        unused = false;   /* an asked scan holds the link to its end */
    }
#endif
#if CONFIG_KODE_LOWPOWER
    if ( me->lp_asleep )
    {
        unused = false;   /* the sentinel is keeping the link through the sleep */
    }
#endif
    return unused;
}

#if CONFIG_KODEOS_WIFI
/**
 * @brief Make the station's power save match the channel's wish and the
 *        `wifi_ps` setting, if it can.
 *
 * No power save is applied only while the station is on a network; the
 * default is applied whenever the station is up. Nothing is submitted while
 * the worker is busy, an update or install owns the channel, or the row runs:
 * radio_settle() pays the debt later. The belief changes at submit; a refusal
 * puts it back and the debt waits for the next request or job, not a retry
 * that would spin at RPC speed.
 *
 * @param me The AO.
 * @return true when a job was submitted.
 */
static bool radio_fast_apply(Radio * const me)
{
    kdrv_radio_req_t req;
    /* `wifi_ps=off` is a standing wish for no power save. */
    bool const fast = me->fast_want || !me->ps_on;

    if ( me->busy || me->ps_held || (me->ps_fast == fast)
        || ((uint8_t)OTA_IDLE != me->phase)
        || ((uint8_t)INST_IDLE != me->inst)
        || ((uint8_t)TEST_IDLE != me->test_phase) )
    {
        return false;
    }
    if ( !kdrv_wifi_started() )
    {
        return false;   /* the next join pays the debt, if there is one */
    }
    if ( fast && (kdrv_wifi_state(NULL) != KDRV_WIFI_GOT_IP) )
    {
        return false;
    }
    (void)memset(&req, 0, sizeof(req));
    req.type = KDRV_RADIO_JOB_WIFI_PS;
    req.on   = !fast;
    if ( !radio_submit_req(me, &req) )
    {
        return false;
    }
    me->ps_fast = fast;
    return true;
}
#endif

/**
 * @brief Say that a switch of the header's supply is over: EXP_POWER_SIG.
 *
 * Published, to subscribers only. An end that names the wish pays a request
 * that waits for it (exp_owed); one that names an older switch does not.
 *
 * @param me The AO.
 * @param on The switch: true for on.
 * @param rc KODE_OK, the rail is as @p on says; KODE_IO_ERROR, the C5
 *           refused; KODE_DENIED, the link would not come up or went with
 *           the switch, and the next link-up carries it out.
 */
static void radio_exp_said(Radio * const me, bool on, kode_status_t rc)
{
    static const ExpPowerDoneEvt on_done    = { QEVT_INITIALIZER(EXP_POWER_SIG), true,  (uint8_t)KODE_OK };
    static const ExpPowerDoneEvt on_refused = { QEVT_INITIALIZER(EXP_POWER_SIG), true,  (uint8_t)KODE_IO_ERROR };
    static const ExpPowerDoneEvt on_owed    = { QEVT_INITIALIZER(EXP_POWER_SIG), true,  (uint8_t)KODE_DENIED };
    static const ExpPowerDoneEvt off_done    = { QEVT_INITIALIZER(EXP_POWER_SIG), false, (uint8_t)KODE_OK };
    static const ExpPowerDoneEvt off_refused = { QEVT_INITIALIZER(EXP_POWER_SIG), false, (uint8_t)KODE_IO_ERROR };
    static const ExpPowerDoneEvt off_owed    = { QEVT_INITIALIZER(EXP_POWER_SIG), false, (uint8_t)KODE_DENIED };
    ExpPowerDoneEvt const *e = &off_owed;

    if ( on )
    {
        e = &on_owed;
        if ( KODE_OK == rc )
        {
            e = &on_done;
        }
        else if ( KODE_IO_ERROR == rc )
        {
            e = &on_refused;
        }
        else
        {
            /* the link would not come up: owed to the next link-up */
        }
    }
    else if ( KODE_OK == rc )
    {
        e = &off_done;
    }
    else if ( KODE_IO_ERROR == rc )
    {
        e = &off_refused;
    }
    else
    {
        /* off, the link would not come up: owed to the next link-up */
    }
    if ( on == me->exp_want )
    {
        me->exp_owed = false;
    }
    QACTIVE_PUBLISH(&e->super, &me->super);
}

/**
 * @brief Say the wish is carried out when a request waits for it, the rail
 *        is known to be as wished and nothing is on its way that drives the
 *        pin: no switch with the worker, no link coming up (its connect
 *        leaves the rail as the wish was when it went out).
 * @param me The AO.
 */
static void radio_exp_settled(Radio * const me)
{
    if ( me->exp_owed && me->exp_known && (me->exp_want == me->exp_on) && !me->exp_job
        && !me->linking )
    {
        radio_exp_said(me, me->exp_on, KODE_OK);
    }
}

/**
 * @brief Switch the header's supply on with no link: reset the C5.
 *
 * A C5 in reset drives none of its pins, so R72 switches the rail on, and
 * the C5's firmware boots again without touching IO5. The C5 is let go
 * RADIO_EXP_PULSE_MS later, by EXP_PULSE_SIG. A C5 already held in reset
 * has the rail on and is left as it is. Refused, harmlessly, while
 * esp-hosted has the reset line: the link coming up resets the C5 too.
 *
 * @param me The AO.
 */
static void radio_exp_pulse(Radio * const me)
{
    if ( kdrv_radio_c5_held() )
    {
        me->exp_on    = true;
        me->exp_known = true;
        radio_exp_said(me, true, KODE_OK);
        return;
    }
    if ( kdrv_radio_c5_hold(true) != KODE_OK )
    {
        return;
    }
    me->exp_on    = true;
    me->exp_known = true;
    KLOG_N(TAG, "J3's supply on: the C5 is reset so it lets go of the pin");
    (void)QTimeEvt_disarm(&me->exp_pulse);
    QTimeEvt_armX(&me->exp_pulse, KQP_TICKS_PER_MS(RADIO_EXP_PULSE_MS), 0U);
    radio_exp_said(me, true, KODE_OK);
}

/**
 * @brief Make the expansion header's supply match the wish, if the C5 can.
 *
 * With the link up the pin is driven by a job. With no link only "on" can be
 * done (radio_exp_pulse()); "off" needs the link, which Radio_off and
 * Radio_failed bring up (radio_exp_ask_link()), or the next link-up pays it,
 * so any app, finished or crashed, leaves the rail off. Nothing is submitted
 * while the worker or the channel is busy (radio_settle() pays later); the
 * belief changes at submit. A rail not known is never as asked.
 *
 * @param me The AO.
 * @return true when a job was submitted.
 */
static bool radio_exp_apply(Radio * const me)
{
    kdrv_radio_req_t req;

    if ( me->exp_known && (me->exp_want == me->exp_on) )
    {
        radio_exp_settled(me);
        return false;
    }
    if ( !kdrv_radio_link_up() )
    {
        if ( me->exp_want )
        {
            radio_exp_pulse(me);
        }
        /* An "off" waits for a link: the one on its way, the one the state
         * brings up for it, or the next one. */
        return false;
    }
    if ( me->busy || ((uint8_t)OTA_IDLE != me->phase)
        || ((uint8_t)INST_IDLE != me->inst)
        || ((uint8_t)TEST_IDLE != me->test_phase) )
    {
        return false;
    }
    (void)memset(&req, 0, sizeof(req));
    req.type = KDRV_RADIO_JOB_EXP_POWER;
    req.on   = me->exp_want;
    if ( !radio_submit_req(me, &req) )
    {
        return false;
    }
    me->exp_on    = me->exp_want;
    me->exp_known = true;
    me->exp_job   = true;
    return true;
}

#if CONFIG_KODEOS_BLE
/**
 * @brief Hand the scan response an app asked for to the radio, if it can.
 *
 * A debt like the header's supply: kept while Bluetooth is off in Settings
 * or the link is down, and paid with a BLE_RSP job once the reconcile is
 * over and nothing else holds the channel. The advert is not touched; the
 * driver keeps the response for every advert after.
 *
 * @param me The AO.
 * @return true when the job was submitted.
 */
static bool radio_rsp_apply(Radio * const me)
{
    kdrv_radio_req_t req;

    if ( !me->rsp_dirty || !me->ble_want || !kdrv_radio_link_up() )
    {
        return false;
    }
    if ( me->busy || ((uint8_t)RECON_DONE != me->recon)
        || ((uint8_t)OTA_IDLE != me->phase)
        || ((uint8_t)INST_IDLE != me->inst)
        || ((uint8_t)TEST_IDLE != me->test_phase)
        || ((uint8_t)PARK_IDLE != me->park_phase) )
    {
        return false;
    }
    (void)memcpy(me->rsp_sent, me->rsp_data, sizeof(me->rsp_sent));
    (void)memset(&req, 0, sizeof(req));
    req.type = KDRV_RADIO_JOB_BLE_RSP;
    (void)snprintf(req.ssid, sizeof(req.ssid), "%s", me->rsp_name);
    req.data = me->rsp_sent;
    req.len  = me->rsp_len;
    if ( !radio_submit_req(me, &req) )
    {
        return false;
    }
    me->rsp_dirty = false;
    return true;
}

/**
 * @brief BLE_RSP_REQ_SIG: keep the scan response an app asked for, in every
 *        state, and hand it on when the radio can take it.
 *
 * @param me The AO.
 * @param e  The event.
 * @return true when @p e was it and has been handled.
 */
static bool radio_ble_rsp(Radio * const me, QEvt const * const e)
{
    if ( (QSignal)BLE_RSP_REQ_SIG != e->sig )
    {
        return false;
    }
    BleRspReqEvt const * const r = Q_EVT_CAST(BleRspReqEvt);

    (void)snprintf(me->rsp_name, sizeof(me->rsp_name), "%s", r->name);
    me->rsp_len = 0U;
    if ( (uint8_t)sizeof(me->rsp_data) >= r->len )
    {
        (void)memcpy(me->rsp_data, r->data, r->len);
        me->rsp_len = r->len;
    }
    me->rsp_dirty = true;
    KLOG_N(TAG, "the scan response is asked to carry '%s' and %u bytes of data",
           me->rsp_name, (unsigned)me->rsp_len);
    (void)radio_rsp_apply(me);
    return true;
}

/**
 * @brief BLE_APP_VALUE_REQ_SIG: set, or set and notify, one value of the
 *        app's service, in every state, and answer a blocked task.
 *
 * The driver queues a notification only for a subscribed central, so the
 * value is set whatever the link. The drain is woken here, unless a job of
 * the worker runs: that may be a start or a stop of the host, which the
 * wake-up would race, and the job's end wakes it instead.
 *
 * @param me The AO.
 * @param e  The event.
 * @return true when @p e was it and has been handled.
 */
static bool radio_ble_value(Radio const * const me, QEvt const * const e)
{
    if ( (QSignal)BLE_APP_VALUE_REQ_SIG != e->sig )
    {
        return false;
    }
    BleAppValueReqEvt const * const r = Q_EVT_CAST(BleAppValueReqEvt);
    kode_status_t                   rc;

    if ( r->notify )
    {
        rc = kdrv_ble_app_notify(r->name, r->data, r->len);
        if ( (KODE_OK == rc) && !me->winflight )
        {
            kdrv_ble_app_kick();
        }
    }
    else
    {
        rc = kdrv_ble_app_set_value(r->name, r->data, r->len);
    }
    if ( NULL != r->wait )
    {
        r->wait->rc = (uint8_t)rc;
        scan_ack_write(&r->wait->ack, r->gen);
        khal_sem_signal(r->wait->done);
    }
    else if ( KODE_OK != rc )
    {
        /* A command has nobody to answer; its caller checked the table. */
        KLOG_D(TAG, "the app's value '%s' was refused: %d", r->name, (int)rc);
    }
    else
    {
        /* set, and queued when somebody listens */
    }
    return true;
}
#endif /* CONFIG_KODEOS_BLE */

/**
 * @brief No link, and none coming: settle what the header's supply owes.
 *
 * A switch that was with the worker will not be answered: the rail is no
 * longer known, and the switch is said either way. A waiting request is
 * carried out if it can be with no link (an "on" resets the C5, a rail known
 * as asked is said so), and is otherwise said owed to the next link-up
 * (KODE_DENIED). Called by both no-link entries, after the C5 is held where
 * that is the entry's to do.
 *
 * @param me The AO.
 */
static void radio_exp_no_link(Radio * const me)
{
    bool sent = false;

    if ( me->exp_job )
    {
        me->exp_job   = false;
        me->exp_known = false;
        me->exp_owed  = true;
    }
    if ( me->exp_owed )
    {
        sent = radio_exp_apply(me);
    }
    if ( me->exp_owed && !sent )
    {
        radio_exp_said(me, me->exp_want, KODE_DENIED);
    }
}

/**
 * @brief Tell an "on" that waits that the amplifier is there: C5_RUNNING_SIG.
 * @param me The AO.
 */
static void radio_amp_answer(Radio * const me)
{
    static const QEvt running = QEVT_INITIALIZER(C5_RUNNING_SIG);

    if ( me->amp_answer_owed )
    {
        me->amp_answer_owed = false;
        QACTIVE_PUBLISH(&running, &me->super);
    }
}

/**
 * @brief Pay the amplifier's debt: drive IO4 as wished, as a job.
 *
 * Only over the link and with the channel free, like the header's supply: a
 * wish made with no link waits for the next link-up, which settles again.
 *
 * @param me The AO.
 * @return true when a job was submitted.
 */
static bool radio_amp_apply(Radio * const me)
{
    kdrv_radio_req_t req;

    if ( (!me->amp_wished) || (me->amp_known && (me->amp_low == me->amp_off)) )
    {
        return false;
    }
    if ( (!kdrv_radio_link_up()) || me->busy || ((uint8_t)OTA_IDLE != me->phase)
        || ((uint8_t)INST_IDLE != me->inst)
        || ((uint8_t)TEST_IDLE != me->test_phase) )
    {
        return false;
    }
    (void)memset(&req, 0, sizeof(req));
    req.type = KDRV_RADIO_JOB_AMP;
    req.on   = !me->amp_off;
    if ( !radio_submit_req(me, &req) )
    {
        return false;
    }
    me->amp_low   = me->amp_off;
    me->amp_known = true;
    return true;
}

#if defined(CONFIG_KODEOS_C5_HOLD)
static bool radio_c5_run(Radio * const me);
#endif

/**
 * @brief The amplifier wanted from a C5 that runs: answer once its pin is high.
 * @param me The AO.
 */
static void radio_amp_on(Radio * const me)
{
    me->amp_answer_owed = true;
    if ( me->amp_known && !me->amp_low )
    {
        radio_amp_answer(me);
    }
    else if ( kdrv_radio_link_up() || me->linking )
    {
        /* The pin may have been left low by the last firmware: answered once
         * a job has driven it, or a link-up has reset the C5 that holds it.
         * Every way out of that hope pays it too (Radio_off, Radio_failed). */
        (void)radio_amp_apply(me);
    }
    else
    {
        /* No link, none on its way: nothing can drive the pin. Answered now,
         * the amplifier as the C5 left it - its own boot raised it. */
        radio_amp_answer(me);
    }
}

/**
 * @brief C5_RUN_REQ_SIG and C5_AMP_OFF_SIG: the amplifier wanted or not,
 *        which every state answers alike.
 *
 * @param me The AO.
 * @param e  The event.
 * @return true when @p e was one of the two and has been handled.
 */
static bool radio_amp(Radio * const me, QEvt const * const e)
{
    bool handled = true;

    switch ( e->sig )
    {
        case C5_RUN_REQ_SIG: {
            me->amp_wished = true;
            me->amp_off    = false;
#if defined(CONFIG_KODEOS_C5_HOLD)
            /* A held C5 is let go and boots with IO4 high: its boot answers. */
            if ( !radio_c5_run(me) )
            {
                radio_amp_on(me);
            }
#else
            radio_amp_on(me);
#endif
            break;
        }
        case C5_AMP_OFF_SIG: {
            KLOG_D(TAG, "the amplifier is asked off");
            me->amp_wished = true;
            me->amp_off    = true;
            (void)radio_amp_apply(me);
            break;
        }
        default: {
            handled = false;
            break;
        }
    }
    return handled;
}

/**
 * @brief EXP_POWER_REQ_SIG and EXP_PULSE_SIG: the header's supply, which
 *        every state answers alike.
 *
 * @param me The AO.
 * @param e  The event.
 * @return true when @p e was one of the two and has been handled.
 */
static bool radio_exp_power(Radio * const me, QEvt const * const e)
{
    bool handled = true;

    switch ( e->sig )
    {
        case EXP_POWER_REQ_SIG: {
            bool const on = Q_EVT_CAST(ExpPowerEvt)->on;

            if ( on != me->exp_want )
            {
                if ( on )
                {
                    KLOG_N(TAG, "J3's supply is asked on");
                }
                else
                {
                    KLOG_N(TAG, "J3's supply is asked off");
                }
            }
            me->exp_want = on;
            me->exp_owed = true;
            (void)radio_exp_apply(me);
            break;
        }
        case EXP_PULSE_SIG: {
            if ( kdrv_radio_c5_held() && (kdrv_radio_c5_hold(false) != KODE_OK) )
            {
                KLOG_E(TAG, "the C5 would not be let go after J3's supply "
                            "came on");
            }
            break;
        }
        default: {
            handled = false;
            break;
        }
    }
    return handled;
}

/**
 * @brief Pay off a request that was made and could not be carried out.
 *
 * A debt is recorded by the request that was refused and paid here: nothing
 * starts or stops a service merely because of what is or is not running, so
 * a stack `ble_scan` brought up for itself is left alone. Both directions are
 * debts, so off then on inside one busy job ends on. At most one job is
 * submitted or posted, and its completion calls this again. Services first;
 * the link comes down last, when radio_link_unused() says nothing uses it.
 *
 * @param me The AO.
 */
static void radio_settle(Radio * const me)
{
    if ( me->busy )
    {
        return;   /* a job is in flight; its completion calls us back */
    }
#if CONFIG_KODEOS_WIFI
    if ( me->wifi_stop_pending )
    {
        /* Whether the station is up, not whether it is on a network: a scan
         * starts it without joining anything. kdrv_wifi_stop() clears that
         * flag unconditionally, so the pass after this job finds the debt
         * paid and nothing left to do. */
        if ( !kdrv_wifi_started() )
        {
            me->wifi_stop_pending = false;   /* already down; nothing owed */
        }
        else if ( radio_submit(me, KDRV_RADIO_JOB_WIFI_STOP) )
        {
            me->wifi_stop_pending = false;
            return;
        }
        else
        {
            return;   /* refused; the next completion tries again */
        }
    }
#endif
    /* The short switches before any owed Wi-Fi request or asked scan: a
     * round or a sweep holds the worker for tens of seconds, so a switch or
     * a sound waits at most the job in flight, never one owed after it. */
    if ( radio_exp_apply(me) )
    {
        return;
    }
    if ( radio_amp_apply(me) )
    {
        return;
    }
    /* The radio check asked for in a hurry; it has its own budget. */
    if ( probe_owed_start(me) )
    {
        return;
    }
#if CONFIG_KODEOS_WIFI
    if ( me->wifi_start_pending )
    {
        /* Posted, not submitted: "WiFi on" with a link already up means "get
         * on a network", and the autojoin handler owns every rule about how.
         * Paying the debt is doing exactly what the refused request would
         * have done, so there is still only one copy of those rules. */
        me->wifi_start_pending = false;
        wifi_auto_self();
        return;
    }
    if ( wifi_owed_ready(me) && wifi_owed_replay(me) )
    {
        return;
    }
    if ( wifi_retry_pay(me) )
    {
        return;
    }
#endif
#if CONFIG_KODEOS_WIFI
    if ( radio_fast_apply(me) )
    {
        return;
    }
#endif
#if CONFIG_KODEOS_BLE
    if ( me->ble_stop_pending )
    {
        /* IDLE is the debt paid, not OFF: quiet leaves the host up on
         * purpose, so waiting for OFF here would owe this forever and
         * re-submit on every completed job. */
        kdrv_ble_state_t const st = kdrv_ble_state();
        if ( (KDRV_BLE_OFF == st) || (KDRV_BLE_IDLE == st) )
        {
            me->ble_stop_pending = false;
        }
        else if ( radio_submit(me, KDRV_RADIO_JOB_BLE_QUIET) )
        {
            me->ble_stop_pending = false;
            return;
        }
        else
        {
            return;
        }
    }
    if ( me->ble_start_pending )
    {
        if ( kdrv_ble_state() != KDRV_BLE_OFF )
        {
            me->ble_start_pending = false;   /* already up; nothing owed */
        }
        else if ( radio_submit(me, KDRV_RADIO_JOB_BLE_START) )
        {
            me->ble_start_pending = false;
            return;
        }
        else
        {
            return;
        }
    }
    if ( radio_rsp_apply(me) )
    {
        return;
    }
#endif
#if RADIO_ASKS
    /* After the short switches, above. Before the install and the row,
     * which it is never owed behind. */
    if ( scan_ask_start(me) )
    {
        return;
    }
#endif
    /* After the services and before the row: an image already verified in
     * PSRAM has a host blocked on the wire behind it, and the row has a
     * budget to wait in. */
    if ( me->inst_pending )
    {
        me->inst_pending = false;
        inst_post_go(me);
        return;
    }
    /* Last, after the services: the operator asked for those, and the row
     * has a budget to wait in. */
    if ( me->test_pending )
    {
        me->test_pending = false;
        c5_test_step(me);
        return;
    }
#if CONFIG_KODEOS_WIFI
    wifi_retry_keep(me);
#endif
    /* And when every debt is paid, the link itself: with nothing left that
     * uses it, it comes down. Posted rather than done, because only a state
     * can make the transition, and its handler looks again - a service
     * switched back on in between is a link that is wanted. */
    if ( radio_link_unused(me) )
    {
        static const QEvt unused = QEVT_INITIALIZER(RADIO_UNUSED_SIG);

        QACTIVE_POST(&me->super, &unused, me);
    }
}

#if CONFIG_KODEOS_BLE
/**
 * @brief Let the chain past its BLE step once BLE is up, whoever raised it.
 *
 * The chain observes; asking is the start debt's job. Its step is finished
 * the moment BLE runs, whoever raised it: `ble_scan` brings the stack up
 * with no baton, and a chain whose own start it refused would wait at
 * RECON_BLE for good. Called from the job-completion path, where the channel
 * is free, so the advance never posts the autojoin into an occupied channel.
 */
static void radio_reconcile_retry(Radio * const me)
{
    if ( ((uint8_t)RECON_BLE == me->recon) && !me->busy
        && (kdrv_ble_state() != KDRV_BLE_OFF) )
    {
        radio_reconcile_next(me);
    }
}

/** @brief How long Pair new device keeps the board visible, ms. */
#define RADIO_BLE_PAIR_MS     300000U
/** @brief What is left of the time to pair at which it is over, ms. */
#define RADIO_BLE_END_MIN_MS  1000U
/** @brief How long the fast advert lasts, ms. */
#define RADIO_BLE_FAST_MS     30000U
/** @brief A leg of the advert with less than this left is over, ms: NimBLE's
 *         timer may end one a little before the clock here does. */
#define RADIO_BLE_LEG_MIN_MS  500U

/**
 * @brief The time from @p now to @p until, on a clock that wraps.
 * @param until The end.
 * @param now   The present.
 * @return The ms left; 0 when it has passed.
 */
static uint32_t radio_ms_left(uint32_t until, uint32_t now)
{
    uint32_t const left = until - now;

    return (0x80000000U > left) ? left : 0U;
}

/**
 * @brief Whether this firmware is always visible: it is up to whoever built it.
 *
 * A lab build (KODEOS_BLE_AT_BOOT) has to be found whenever the lab looks, and
 * an app that serves its own Bluetooth has said it wants to be reached.
 *
 * @param me The AO.
 * @return true when the board never hides.
 */
static bool radio_ble_always_visible(Radio const * const me)
{
    bool always = false;

#if defined(CONFIG_KODEOS_BLE_AT_BOOT)
    always = true;
#elif defined(CONFIG_KODEOS_BOOT_ROLE_APP)
    always = (kdrv_ble_app_service() != NULL) || (0U < me->rsp_len) || ('\0' != me->rsp_name[0]);
#endif
    (void)me;
    return always;
}

/**
 * @brief Whether the time to pair is what keeps the board visible: it has
 *        bonds, so without that time it would hide.
 *
 * @param me The AO.
 * @return true while Pair new device's time is the reason.
 */
static bool radio_ble_pair_shows(Radio const * const me)
{
    /* cppcheck-suppress knownConditionTrueFalse ; constant in each build: whether the role always advertises */
    return me->ble_pair_open && !radio_ble_always_visible(me) && (radio_ble_bonds(me) > 0U);
}

/**
 * @brief Whether the next advert hides the board.
 *
 * A board that knows nobody is visible: somebody has to be able to find it.
 * Once a device is bonded the board serves that device and hides from
 * everyone else, until Pair new device makes it visible for a while.
 *
 * @param me The AO.
 * @return true when the advert hides the board.
 */
static bool radio_ble_hidden(Radio const * const me)
{
    /* cppcheck-suppress knownConditionTrueFalse ; constant in each build: whether the role always advertises */
    return !radio_ble_always_visible(me) && !me->ble_pair_open && (radio_ble_bonds(me) > 0U);
}

/**
 * @brief The time an advert leg may last, ms: what is left of the time to pair
 *        when that is what shows the board, and no end (0) otherwise.
 *
 * @param me  The AO.
 * @param now khal_time_ms().
 * @return The leg's time, ms; 0 for no end.
 */
static uint32_t radio_ble_leg_ms(Radio const * const me, uint32_t now)
{
    uint32_t ms = 0U;

    if ( radio_ble_pair_shows(me) )
    {
        ms = radio_ms_left(me->ble_pair_until, now);
        if ( RADIO_BLE_END_MIN_MS > ms )
        {
            /* 0 would mean no end. */
            ms = RADIO_BLE_END_MIN_MS;
        }
    }
    return ms;
}

/**
 * @brief The time to pair is over when what is left of it is under a second.
 *
 * @param me  The AO.
 * @param now khal_time_ms().
 */
static void radio_ble_pair_expire(Radio * const me, uint32_t now)
{
    if ( me->ble_pair_open
        && (RADIO_BLE_END_MIN_MS > radio_ms_left(me->ble_pair_until, now)) )
    {
        me->ble_pair_open = false;
        KLOG_N(TAG, "the time to pair a new device is over");
    }
}

/**
 * @brief Whether the board is at rest: the advert stays slow, whatever else it
 *        was promised, so that a rest never costs more than it did.
 *
 * @param me The AO.
 * @return true at rest.
 */
static bool radio_ble_resting(Radio const * const me)
{
#if CONFIG_KODE_LOWPOWER
    return me->lp_asleep;
#else
    (void)me;
    return false;
#endif
}

/**
 * @brief A phone that proved who it is and left is the one coming back: its
 *        next advert starts fast. A stranger that connected and left gets
 *        the advert as it was, so that nobody speeds it up by knocking.
 *
 * @param me The AO.
 */
static void radio_ble_link_note(Radio * const me)
{
    if ( kdrv_ble_state() == KDRV_BLE_CONNECTED )
    {
        me->ble_linked = true;
    }
    else if ( me->ble_linked )
    {
        me->ble_linked = false;
        if ( kdrv_ble_link_secure() )
        {
            me->ble_fast_owed = true;
            me->ble_asked     = true;
        }
    }
    else
    {
        /* the advert's end, or nothing to do with a link */
    }
}

/**
 * @brief THE ADVERTISING INVARIANT: BLE on and nobody connected means on air.
 *
 * Reachable is what the `ble` flag means: this restores that state from
 * wherever the board drifted. Visible while it knows nobody or Pair new
 * device's time lasts, hidden but to its bonds otherwise. The advert runs in
 * legs, each one's end coming back here: fast for 30 s after somebody may be
 * looking, then slow. An advert showing the board the wrong way comes off.
 * OFF is not acted on: a stack that is down is the start debt's.
 *
 * @param me The AO.
 * @return true when an advert was submitted - the caller then owns a job in
 *         flight, and the completion is what finishes whatever it was doing.
 */
static bool radio_ble_adv_reconcile(Radio * const me)
{
    bool submitted = false;
    /* A park hung the phone up on purpose; the advert stays down for the
     * restart that follows. */
    if ( me->ble_want && !me->busy && ((uint8_t)PARK_IDLE == me->park_phase) )
    {
        uint32_t const         now   = khal_time_ms();
        kdrv_ble_state_t const state = kdrv_ble_state();
        kdrv_radio_req_t       req;

        radio_ble_pair_expire(me, now);
        bool const hidden = radio_ble_hidden(me);

        (void)memset(&req, 0, sizeof(req));
        req.type = KDRV_RADIO_JOB_BLE_ADV;
        if ( (KDRV_BLE_ADVERTISING == state) && (hidden != me->ble_adv_hidden) )
        {
            req.on    = false;
            submitted = radio_submit_req(me, &req);
        }
        else if ( KDRV_BLE_IDLE == state )
        {
            if ( me->ble_fast_owed )
            {
                /* Fast only for somebody who may be looking now: whoever
                 * switched it on or asked to pair, or a device that knows
                 * this board. A board nobody knows has nobody to be quick
                 * for. */
                me->ble_fast_owed  = false;
                me->ble_fast_until = now;
                if ( me->ble_asked || (radio_ble_bonds(me) > 0U) )
                {
                    me->ble_fast_until = now + RADIO_BLE_FAST_MS;
                }
                me->ble_asked = false;
            }
            uint32_t const fast = radio_ms_left(me->ble_fast_until, now);

            req.on     = true;
            req.hidden = hidden;
            req.itvl   = (uint16_t)KDRV_BLE_ITVL_SLOW;
            req.len    = radio_ble_leg_ms(me, now);
            if ( (RADIO_BLE_LEG_MIN_MS <= fast) && !radio_ble_resting(me) )
            {
                req.itvl = (uint16_t)KDRV_BLE_ITVL_FAST;
                if ( (0U == req.len) || (fast < req.len) )
                {
                    req.len = fast;
                }
            }
            me->ble_adv_hidden = hidden;
            submitted = radio_submit_req(me, &req);
        }
        else
        {
            /* connected, or off: nothing to put on the air */
        }
    }
    return submitted;
}
#endif

/* ---- The radio check (radio_probe_request()) ------------------------------
 * A chain of the worker's jobs, one at a time, each one's done event starting
 * the next: the C5's card, the two networks (or one sweep that finds them),
 * the BLE name, and the advert the listen took off the air put back. This AO
 * decides the order, the passes and the budget; a job only runs.
 */

/** Where the radio check has got (Radio.pb_phase); the phases run in this order. */
enum {
    PB_IDLE = 0,  /**< No check.                                              */
    PB_OWED,      /**< Asked while the channel was busy; the settle starts it. */
    PB_CARD,      /**< The snapshot of the C5 is with the worker.             */
    PB_SWEEP,     /**< The sweep that finds the channels is with the worker.  */
    PB_W24,       /**< A pass over the 2.4 GHz network.                       */
    PB_W5,        /**< A pass over the 5 GHz network.                         */
    PB_FIND,      /**< The BLE listen.                                        */
    PB_ADV,       /**< The advert is being put back.                          */
    PB_END        /**< Nothing is left: the answer goes.                      */
};

#if CONFIG_KODEOS_BLE
_Static_assert(RADIO_PROBE_BLE_BYTES == (KDRV_BLE_ADV_MAX - 1U),
               "the BLE name of the check is what one advert holds");
#endif

/**
 * @brief The answer goes, and the check is over.
 * @param me The AO.
 * @param rc The status: KODE_OK, or why the check did not run to its end.
 */
static void probe_answer(Radio * const me, kode_status_t rc)
{
    RadioProbeEvt * const rsp = me->pb_rsp;
    QActive * const       who = me->pb_who;

    me->pb_phase = (uint8_t)PB_IDLE;
    me->pb_job   = false;
    me->pb_rsp   = NULL;
    me->pb_who   = NULL;
#if CONFIG_KODEOS_WIFI
    kdrv_wifi_scan_abort(false);
#endif
    if ( NULL == rsp )
    {
        return;
    }
#if CONFIG_KODEOS_BLE
    rsp->ble_adv = (kdrv_ble_state() == KDRV_BLE_ADVERTISING);
#endif
    rsp->status = (uint8_t)rc;
    rsp->ms     = khal_time_ms() - me->pb_asked;
    QACTIVE_POST(who, &rsp->super, &me->super);
}

/**
 * @brief Turn a request away in its own shape: its answer, with nothing in it.
 * @param r  The request.
 * @param rc The status.
 */
static void probe_refuse(RadioProbeReqEvt const * const r, kode_status_t rc)
{
    r->rsp->status = (uint8_t)rc;
    QACTIVE_POST(r->who, &r->rsp->super, AO_Radio);
}

/**
 * @brief Remember the first job of the check that failed.
 * @param me The AO.
 * @param rc The job's status.
 */
static void probe_note(Radio * const me, kode_status_t rc)
{
    if ( (KODE_OK != rc) && ((uint8_t)KODE_OK == me->pb_rc) )
    {
        me->pb_rc = (uint8_t)rc;
    }
}

/**
 * @brief The job of @p phase, when the request leaves it in.
 * @param me    The AO.
 * @param phase PB_CARD..PB_FIND.
 * @param req   Filled in.
 * @return true when there is a job to run.
 */
static bool probe_job_for(Radio const * const me, uint8_t phase, kdrv_radio_req_t *req)
{
    radio_probe_t const * const p = &me->pb_req;
    bool wanted = false;

    (void)memset(req, 0, sizeof(*req));
    switch ( phase )
    {
        case PB_CARD: {
            req->type = KDRV_RADIO_JOB_C5_INFO;
            wanted    = p->card;
            break;
        }
#if CONFIG_KODEOS_WIFI
        case PB_SWEEP: {
            req->type = KDRV_RADIO_JOB_WIFI_SCAN;
            wanted    = p->discover;
            break;
        }
        case PB_W24: {
            req->type = KDRV_RADIO_JOB_WIFI_PROBE;
            (void)snprintf(req->ssid, sizeof(req->ssid), "%s", p->ssid24);
            req->channel  = p->ch24;
            req->dwell_ms = (uint16_t)KODE_RADIO_PROBE_DWELL_MS;
            wanted = (!p->discover) && ('\0' != p->ssid24[0]) && (0U != p->ch24);
            break;
        }
        case PB_W5: {
            req->type = KDRV_RADIO_JOB_WIFI_PROBE;
            (void)snprintf(req->ssid, sizeof(req->ssid), "%s", p->ssid5);
            req->channel  = p->ch5;
            req->dwell_ms = (uint16_t)KODE_RADIO_PROBE_PDWELL_MS;
            wanted = (!p->discover) && ('\0' != p->ssid5[0]) && (0U != p->ch5);
            break;
        }
#endif
#if CONFIG_KODEOS_BLE
        case PB_FIND: {
            req->type = KDRV_RADIO_JOB_BLE_FIND;
            (void)snprintf(req->ssid, sizeof(req->ssid), "%s", p->ble);
            req->len = (uint32_t)KODE_RADIO_FIND_MS;
            wanted   = ('\0' != p->ble[0]);
            break;
        }
#endif
        default: {
            break;
        }
    }
    return wanted;
}

/**
 * @brief Start the job of @p phase, or the first later one the request wants;
 *        answer when none is left.
 *
 * @param me    The AO, with the channel free.
 * @param phase Where to begin looking.
 * @param pass  The pass this job is over its network, from 0.
 */
static void probe_go(Radio * const me, uint8_t phase, uint8_t pass)
{
    bool launched = false;

    while ( (!launched) && ((uint8_t)PB_END > phase) )
    {
        kdrv_radio_req_t req;

        me->pb_phase = phase;
        me->pb_pass  = pass;
        if ( ((uint8_t)PB_SWEEP != phase) && ((uint8_t)PB_ADV != phase)
            && ((khal_time_ms() - me->pb_t0) > KODE_RADIO_PROBE_MS) )
        {
            KLOG_W(TAG, "radio check: out of time before phase %u", (unsigned)phase);
            probe_note(me, KODE_TIMEOUT);
            phase = (uint8_t)PB_END;
        }
#if CONFIG_KODEOS_BLE
        else if ( (uint8_t)PB_ADV == phase )
        {
            /* The listen took the advert off the air; the invariant that
             * puts it back is run here so the answer can say it is on. */
            me->pb_job    = true;
            me->pb_job_t0 = khal_time_ms();
            launched      = radio_ble_adv_reconcile(me);
            if ( !launched )
            {
                me->pb_job = false;
            }
        }
#endif
        else if ( probe_job_for(me, phase, &req) )
        {
            me->pb_job_t0 = khal_time_ms();
            launched = radio_submit_req(me, &req);
            if ( !launched )
            {
                probe_note(me, KODE_NO_RESOURCES);
                phase = (uint8_t)PB_END;
            }
            me->pb_job = launched;
        }
        else
        {
            /* Left out of the request. */
        }
        if ( !launched && ((uint8_t)PB_END > phase) )
        {
            phase = (uint8_t)(phase + 1U);
            pass  = 0U;
        }
    }
    if ( !launched )
    {
        probe_answer(me, (kode_status_t)me->pb_rc);
    }
}

/**
 * @brief Take a new check: start it, or owe it until the channel is free.
 * @param me The AO, in Radio_up.
 * @param e  The RADIO_PROBE_REQ_SIG.
 */
static void probe_take(Radio * const me, QEvt const * const e)
{
    RadioProbeReqEvt const * const r = Q_EVT_CAST(RadioProbeReqEvt);

    if ( (uint8_t)PB_IDLE != me->pb_phase )
    {
        KLOG_N(TAG, "radio check: one is running; refused");
        probe_refuse(r, KODE_NO_RESOURCES);
        return;
    }
    me->pb_who   = r->who;
    me->pb_rsp   = r->rsp;
    me->pb_req   = r->req;
    me->pb_rc    = (uint8_t)KODE_OK;
    me->pb_job   = false;
    me->pb_t0    = khal_time_ms();
    me->pb_asked = me->pb_t0;
    me->pb_rsp->ch24 = r->req.ch24;
    me->pb_rsp->ch5  = r->req.ch5;
    if ( !me->busy )
    {
        probe_go(me, (uint8_t)PB_CARD, 0U);
        return;
    }
    me->pb_phase = (uint8_t)PB_OWED;
    KLOG_I(TAG, "radio check: waiting for the channel");
#if CONFIG_KODEOS_WIFI
    /* A sweep is tens of seconds; it ends after its step on the air, cut
     * (KODE_DENIED), and its owner hears that. The production row's own
     * sweep is not cut: a row judged on half the air would fail a good unit. */
    if ( me->winflight && (NULL != me->jobs) && (!me->test_job)
        && (KDRV_RADIO_JOB_WIFI_SCAN == me->jobs->wreq.type) )
    {
        kdrv_wifi_scan_abort(true);
    }
#endif
}

/**
 * @brief Start the radio check that waited for the channel, if the channel is free.
 *
 * Called by radio_settle(). The budget counts from here, not from the
 * request, and the abort of the sweep that was in the way is withdrawn.
 *
 * @param me The AO.
 * @return true when a job of the check is with the worker.
 */
static bool probe_owed_start(Radio * const me)
{
    if ( ((uint8_t)PB_OWED != me->pb_phase) || me->busy )
    {
        return false;
    }
    me->pb_t0 = khal_time_ms();
#if CONFIG_KODEOS_WIFI
    kdrv_wifi_scan_abort(false);
#endif
    probe_go(me, (uint8_t)PB_CARD, 0U);
    return me->busy;
}

/**
 * @brief Whether the job that just ended is the check's, in @p phase.
 * @param me    The AO.
 * @param phase The phase the outcome belongs to.
 * @return true when it is.
 */
static bool probe_owns(Radio const * const me, uint8_t phase)
{
    return me->pb_job && (me->pb_phase == phase);
}

/** @brief The link is gone: the check ends with what it has. */
static void probe_lost(Radio * const me)
{
    if ( (uint8_t)PB_IDLE != me->pb_phase )
    {
        KLOG_W(TAG, "radio check: the link went");
        probe_answer(me, KODE_DENIED);
    }
}

/**
 * @brief Read the card into the answer.
 * @param rsp The answer.
 */
static void probe_read_card(RadioProbeEvt * const rsp)
{
    long const chip = c5_num(KDRV_C5_CHIP_ID);

    /* The version this image was built against answers in every state. */
    rsp->host[0] = (uint8_t)c5_num(KDRV_C5_HOST_MAJOR);
    rsp->host[1] = (uint8_t)c5_num(KDRV_C5_HOST_MINOR);
    rsp->host[2] = (uint8_t)c5_num(KDRV_C5_HOST_PATCH);
    rsp->card = (c5_num(KDRV_C5_VALID) == 1L)
                && (0L <= chip)
                && (kdrv_c5_mac(rsp->mac) == KODE_OK);
    if ( rsp->card )
    {
        long const width = c5_num(KDRV_C5_SDIO_WIDTH);
        long const khz   = c5_num(KDRV_C5_SDIO_KHZ);

        rsp->chip  = (uint16_t)chip;
        rsp->fw[0] = (uint8_t)c5_num(KDRV_C5_FW_MAJOR);
        rsp->fw[1] = (uint8_t)c5_num(KDRV_C5_FW_MINOR);
        rsp->fw[2] = (uint8_t)c5_num(KDRV_C5_FW_PATCH);
        rsp->kode  = (c5_num(KDRV_C5_PROJECT_KODE) == 1L);
        if ( 0L < width )
        {
            rsp->sdio_width = (uint8_t)width;
        }
        if ( 0L < khz )
        {
            rsp->sdio_khz = (uint32_t)khz;
        }
    }
}

/**
 * @brief Add what the job that just ended took to its phase's time.
 * @param me The AO, its job back.
 */
static void probe_clock(Radio * const me)
{
    RadioProbeEvt * const rsp = me->pb_rsp;
    uint32_t const        dt  = khal_time_ms() - me->pb_job_t0;

    if ( NULL == rsp )
    {
        return;
    }
    switch ( me->pb_phase )
    {
        case PB_CARD: {
            rsp->card_ms += dt;
            break;
        }
        case PB_W24: {
            rsp->w24_ms += dt;
            break;
        }
        case PB_W5: {
            rsp->w5_ms += dt;
            break;
        }
        case PB_FIND:
        case PB_ADV: {
            rsp->ble_ms += dt;
            break;
        }
        default: {
            /* The sweep is the discovery's, not a reading's. */
            break;
        }
    }
}

#if CONFIG_KODEOS_WIFI
/**
 * @brief Merge what one pass heard into a network's reading.
 * @param heard What the pass heard.
 * @param seen  The network's reading so far.
 * @param rssi  Its strongest reading.
 * @param ch    The channel it was heard on.
 */
static void probe_merge(const kdrv_radio_heard_t *heard, bool *seen, int8_t *rssi, uint8_t *ch)
{
    if ( heard->seen && ((!*seen) || (heard->rssi > *rssi)) )
    {
        *seen = true;
        *rssi = heard->rssi;
        *ch   = heard->channel;
    }
}

/**
 * @brief Read where the sweep heard the two networks into the answer.
 * @param me The AO.
 */
static void probe_read_sweep(Radio * const me)
{
    RadioProbeEvt * const rsp = me->pb_rsp;

    rsp->ch24 = 0U;
    rsp->ch5  = 0U;
    for ( uint8_t i = 0U; kdrv_wifi_scan_count() > i; ++i )
    {
        kdrv_wifi_ap_t ap;

        if ( !kdrv_wifi_scan_get(i, &ap) )
        {
            break;
        }
        kdrv_radio_heard_t heard;
        heard.seen    = true;
        heard.rssi    = ap.rssi;
        heard.channel = ap.channel;
        if ( ('\0' != me->pb_req.ssid24[0]) && (strcmp(ap.ssid, me->pb_req.ssid24) == 0) )
        {
            probe_merge(&heard, &rsp->seen24, &rsp->rssi24, &rsp->ch24);
        }
        if ( ('\0' != me->pb_req.ssid5[0]) && (strcmp(ap.ssid, me->pb_req.ssid5) == 0) )
        {
            probe_merge(&heard, &rsp->seen5, &rsp->rssi5, &rsp->ch5);
        }
    }
}

/**
 * @brief One pass over a network ended: merge it, then another pass, or on.
 * @param me The AO.
 * @param rc How the job ended.
 */
static void probe_wifi_done(Radio * const me, kode_status_t rc)
{
    RadioProbeEvt * const rsp = me->pb_rsp;
    kdrv_radio_probe_result_t res;
    bool seen = false;

    kdrv_radio_probe_result(&res);
    if ( (uint8_t)PB_W24 == me->pb_phase )
    {
        probe_merge(&res.net[0], &rsp->seen24, &rsp->rssi24, &rsp->ch24);
        seen = rsp->seen24;
    }
    else
    {
        probe_merge(&res.net[0], &rsp->seen5, &rsp->rssi5, &rsp->ch5);
        rsp->passive5 = res.passive;
        seen = rsp->seen5;
    }
    probe_note(me, rc);
    if ( (KODE_OK == rc) && (!seen) && ((uint8_t)(me->pb_pass + 1U) < (uint8_t)KODE_RADIO_PROBE_TRIES) )
    {
        probe_go(me, me->pb_phase, (uint8_t)(me->pb_pass + 1U));
    }
    else
    {
        probe_go(me, (uint8_t)(me->pb_phase + 1U), 0U);
    }
}
#endif /* CONFIG_KODEOS_WIFI */

/**
 * @brief The check's job ended: read it, then start the next.
 *
 * Called from the outcome handler of the job's kind, with the channel free.
 *
 * @param me  The AO.
 * @param job The outcome.
 */
static void probe_done(Radio * const me, RadioOutcome const * const job)
{
    kode_status_t const rc = (kode_status_t)job->status;

    me->pb_job = false;
    probe_clock(me);
    switch ( me->pb_phase )
    {
        case PB_CARD: {
            probe_note(me, rc);
            if ( KODE_OK == rc )
            {
                probe_read_card(me->pb_rsp);
            }
            probe_go(me, (uint8_t)PB_SWEEP, 0U);
            break;
        }
#if CONFIG_KODEOS_WIFI
        case PB_SWEEP: {
            probe_note(me, rc);
            probe_read_sweep(me);
            /* The sweep is bounded by the job guard; the budget is for what
             * follows it. */
            me->pb_t0 = khal_time_ms();
            probe_go(me, (uint8_t)PB_FIND, 0U);
            break;
        }
        case PB_W24:
        case PB_W5: {
            probe_wifi_done(me, rc);
            break;
        }
#endif
#if CONFIG_KODEOS_BLE
        case PB_FIND: {
            kdrv_radio_probe_result_t res;

            kdrv_radio_probe_result(&res);
            probe_note(me, rc);
            me->pb_rsp->ble_rx  = res.found;
            me->pb_rsp->rssi_rx = res.ble_rssi;
            probe_go(me, (uint8_t)PB_ADV, 0U);
            break;
        }
#endif
        default: {
            probe_go(me, (uint8_t)PB_END, 0U);
            break;
        }
    }
}

bool radio_probe_request(QActive *who, const radio_probe_t *req)
{
    RadioProbeEvt    *rsp = NULL;
    RadioProbeReqEvt *e   = NULL;

    if ( (NULL == who) || (NULL == req) )
    {
        return false;
    }
    rsp = Q_NEW_X(RadioProbeEvt, 0U, RADIO_PROBE_SIG);
    if ( NULL == rsp )
    {
        return false;
    }
    e = Q_NEW_X(RadioProbeReqEvt, 0U, RADIO_PROBE_REQ_SIG);
    if ( NULL == e )
    {
        QF_gc(&rsp->super);
        return false;
    }
    (void)memset(&rsp->status, 0, sizeof(*rsp) - offsetof(RadioProbeEvt, status));
    e->who = who;
    e->rsp = rsp;
    e->req = *req;
    /* The names are text whatever the caller left after them. */
    e->req.ssid24[sizeof(e->req.ssid24) - 1U] = '\0';
    e->req.ssid5[sizeof(e->req.ssid5) - 1U]   = '\0';
    e->req.ble[sizeof(e->req.ble) - 1U]       = '\0';
    if ( !QACTIVE_POST_X(AO_Radio, &e->super, KQP_RADIO_CMD_MARGIN, NULL) )
    {
        QF_gc(&rsp->super);
        return false;
    }
    return true;
}

/* ---- What the glass and the LED see of a C5 update -----------------------
 * The family AO_AppMgr's writes report on: APP_FLASH_PROGRESS_SIG at 0 % and
 * at every whole percent after, then one APP_FLASH_DONE_SIG or
 * APP_FLASH_FAIL_SIG. Staging the image into PSRAM is the first 50 %, the
 * push to the C5 the second; the bar waits at 50 % while the link comes up.
 */

/**
 * @brief Publish one report of the C5 update in flight.
 *
 * @param me  The AO.
 * @param pct How far, 0..100.
 */
static void c5_report(Radio * const me, uint8_t pct)
{
    AppProgressEvt *ev = Q_NEW_X(AppProgressEvt, 0U, APP_FLASH_PROGRESS_SIG);
    if ( NULL != ev )
    {
        ev->percent = pct;
        ev->what    = (uint8_t)KWRITE_C5;
        ev->project[0] = '\0';   /* the radio's image is no app */
        (void)snprintf(ev->file, sizeof(ev->file), "%s", me->file);
        QACTIVE_PUBLISH(&ev->super, &me->super);
    }
    me->reported  = pct;
    me->announced = true;
}

/**
 * @brief Report @p done of @p of bytes through one half of the bar, once per
 *        whole percent.
 *
 * @param me   The AO.
 * @param done Bytes through this half so far.
 * @param of   Bytes this half moves; 0 (not known yet) reports nothing.
 * @param base 0 for staging into PSRAM, 50 for the push to the C5.
 */
static void c5_progress(Radio * const me, uint32_t done, uint32_t of,
                        uint8_t base)
{
    if ( 0U == of )
    {
        return;
    }
    uint8_t const pct = (uint8_t)(base + (uint8_t)((done * 50U) / of));
    if ( pct > me->reported )
    {
        c5_report(me, pct);
    }
}

/**
 * @brief End what the glass was told, once: the done or the failure.
 *
 * A refusal before the first report owes nothing - the glass never saw
 * that update start, and the console already has the reason.
 *
 * @param me  The AO.
 * @param end s_flash_done_evt or s_flash_fail_evt.
 */
static void c5_report_end(Radio * const me, const QEvt *end)
{
    if ( me->announced )
    {
        me->announced = false;
        QACTIVE_PUBLISH(end, &me->super);
    }
}

/** @brief Give up on the update in flight, saying why. */
static void ota_fail(Radio * const me, const char *why)
{
    KLOG_E(TAG, "C5 update failed: %s", why);
    c5_report_end(me, &s_flash_fail_evt.super);
    if ( me->c5_open )
    {
        (void)radio_submit(me, KDRV_RADIO_JOB_C5_ABORT);
        me->c5_open = false;
    }
    if ( (uint8_t)INST_IDLE != me->inst )
    {
        /* The image came down the console, not off a medium: nothing to
         * close, and a host waiting on the wire to tell. */
        inst_fail(me);
    }
    else
    {
        storage_close_request();
    }
    me->phase = (uint8_t)OTA_IDLE;
}

/* ---- install src="terminal" dst=c5: the image over the console wire ------
 * AO_Log hands the wire over raw for the promised byte count, and its reader
 * posts the chunks here to be CRC'd and decoded into the staging buffer.
 * Nothing reaches the C5 until the whole image has verified (digest, header,
 * chip id). The receive needs no link; the push is asked by an event
 * (RADIO_INSTALL_GO_SIG), so a settle or an entry action can ask for it.
 */

/** @brief Close the install with one of the protocol's verdicts. */
static void inst_end(Radio * const me, const QEvt *verdict)
{
    (void)QTimeEvt_disarm(&me->inst_idle);
    me->inst         = (uint8_t)INST_IDLE;
    me->inst_pending = false;
    QACTIVE_PUBLISH(verdict, &me->super);
}

/** @brief Give the buffer back and say the install failed. */
static void inst_fail(Radio * const me)
{
    c5_release(me);
    c5_report_end(me, &s_flash_fail_evt.super);
    inst_end(me, &s_recv_fail_evt);
}

/** @brief Ask self to start the push: the link is up and the image ready. */
static void inst_post_go(Radio * const me)
{
    QACTIVE_POST(&me->super, &s_inst_go_evt, &me->super);
}

/**
 * @brief `install src="terminal" dst=c5` was typed: arm the console for the
 *        payload.
 *
 * Refused with an answer, never with silence - the console closes on these
 * events with its own watchdog disarmed. Served from every state; the two
 * that must refuse (the link being torn down, an update from a medium in
 * flight) do so before this is reached.
 */
static QState inst_request(Radio * const me, AppRecvReqEvt const *req)
{
    if ( (uint8_t)INST_IDLE != me->inst )
    {
        KLOG_E(TAG, "install dst=c5 refused: one is already in progress");
        QACTIVE_PUBLISH(&s_recv_fail_evt, &me->super);
        return Q_HANDLED();
    }
    if ( (uint8_t)OTA_IDLE != me->phase )
    {
        KLOG_E(TAG, "install dst=c5 refused: a C5 update from a medium is in "
                    "progress");
        QACTIVE_PUBLISH(&s_recv_fail_evt, &me->super);
        return Q_HANDLED();
    }
    if ( (0U == req->size) || (C5_IMAGE_MAX < req->size) )
    {
        KLOG_E(TAG, "install dst=c5 refused: %lu bytes is not an image this "
                    "board will send to the coprocessor",
               (unsigned long)req->size);
        QACTIVE_PUBLISH(&s_recv_fail_evt, &me->super);
        return Q_HANDLED();
    }

    c5_release(me);   /* a buffer a failed update left behind */
    me->file[0]   = '\0';   /* a stream has none; a medium's must not linger */
    me->inst_size = req->size;
    me->inst_want = req->crc32;
    me->inst_crc  = KHAL_CRC32_INIT;
    me->inst_wire = 0U;
    me->inst_owed = 0U;
    me->inst_ok   = true;
    me->inst_project[0] = '\0';
    me->inst_version[0] = '\0';

    ConsoleRawReqEvt *raw = Q_NEW_X(ConsoleRawReqEvt, 0U, CONSOLE_RAW_REQ_SIG);
    if ( NULL == raw )
    {
        KLOG_E(TAG, "install dst=c5 failed: out of events arming the console");
        QACTIVE_PUBLISH(&s_recv_fail_evt, &me->super);
        return Q_HANDLED();
    }
    raw->size = req->size;
    raw->who  = &me->super;
    raw->src  = req->src;
    QACTIVE_POST(AO_Log, &raw->super, &me->super);

    QTimeEvt_armX(&me->inst_idle, RADIO_RECV_LONGSTOP_TICKS, 0U);
    me->inst = (uint8_t)INST_ARMING;
    KLOG_N(TAG, "install dst=c5: %lu bytes coming down the console",
           (unsigned long)req->size);
    return Q_HANDLED();
}

/** @brief AO_Log has answered whether the wire is ours; open and say ready. */
static QState inst_armed(Radio * const me, ConsoleRawRspEvt const *rsp)
{
    if ( (uint8_t)INST_ARMING != me->inst )
    {
        return Q_HANDLED();   /* a stale answer to a transfer that ended */
    }
    if ( (uint8_t)KODE_OK != rsp->status )
    {
        KLOG_E(TAG, "install dst=c5 failed: the console would not go raw");
        inst_fail(me);
        return Q_HANDLED();
    }
    /* The wire is raw and quiet - the host will not stream until `>|ready`.
     * Open the decoder over the staging buffer now; the size on the line is
     * what a bare image is, and a container says its own length. */
    if ( kfw_begin(kfw_buf_sink(), &me->kbuf, KFW_TARGET_C5, me->inst_size,
                  c5_reserve, me) != KODE_OK )
    {
        KLOG_E(TAG, "install dst=c5 failed: another transfer is already "
                    "decoding");
        QACTIVE_POST(AO_Log, &s_raw_end_evt, &me->super);
        inst_fail(me);
        return Q_HANDLED();
    }
    me->inst = (uint8_t)INST_RECEIVING;
    (void)QTimeEvt_rearm(&me->inst_idle, RADIO_RECV_IDLE_TICKS);
    QACTIVE_PUBLISH(&s_recv_ready_evt, &me->super);   /* `>|ready` */
    c5_report(me, 0U);
    return Q_HANDLED();
}

/**
 * @brief The image is in PSRAM and verified: start the push, or the link.
 *
 * Which of the two is the link's state. Up: ask self for the push, which
 * checks the channel. Coming up: wait for it. Off or failed: bring it up
 * for the image, exactly as the production row does, and push from
 * Radio_up's entry. Being torn down: nothing will carry it this boot.
 */
static QState inst_go(Radio * const me)
{
    switch ( (kradio_link_t)me->link )
    {
        case KRADIO_UP: {
            me->inst = (uint8_t)INST_READY;
            inst_post_go(me);
            return Q_HANDLED();
        }
        case KRADIO_STARTING: {
            me->inst = (uint8_t)INST_LINKING;
            KLOG_N(TAG, "install dst=c5: the link is coming up; the push waits "
                        "for it");
            return Q_HANDLED();
        }
        case KRADIO_OFF:
        case KRADIO_FAILED: {
            me->inst = (uint8_t)INST_LINKING;
            KLOG_N(TAG, "install dst=c5: bringing the link up for the image");
            if ( radio_begin_connect(me) )
            {
                return Q_TRAN(&Radio_starting);
            }
            KLOG_E(TAG, "install dst=c5 failed: the worker refused the "
                        "connect job");
            inst_fail(me);
            return Q_HANDLED();
        }
        default: {
            KLOG_E(TAG, "install dst=c5 failed: the link is on its way down; "
                        "try again once it is off");
            inst_fail(me);
            return Q_HANDLED();
        }
    }
}

/**
 * @brief One chunk off the wire, posted by AO_Log's reader task.
 *
 * Each one re-arms the idle watchdog, is CRC'd, and goes through the decoder
 * into PSRAM. The `>|ack` is owed per whole chunk and is the transfer's
 * entire flow control; a final partial chunk earns none. A decode failure
 * stops the decoding, never the draining or the acks - the host still owes
 * its count and the wire has no way to say stop.
 */
static QState inst_data(Radio * const me, RecvDataEvt const *d)
{
    if ( (uint8_t)INST_RECEIVING != me->inst )
    {
        return Q_HANDLED();   /* a chunk that outlived its transfer */
    }
    (void)QTimeEvt_rearm(&me->inst_idle, RADIO_RECV_IDLE_TICKS);

    me->inst_crc = khal_crc32(me->inst_crc, d->data, d->len);
    if ( me->inst_ok && (kfw_feed(d->data, d->len) != KODE_OK) )
    {
        me->inst_ok = false;
        KLOG_E(TAG, "install dst=c5: the image could not be decoded at %lu/%lu "
                    "bytes; draining the rest of the transfer",
               (unsigned long)me->inst_wire, (unsigned long)me->inst_size);
    }
    me->inst_wire += d->len;
    me->inst_owed += d->len;
    if ( me->inst_ok )
    {
        c5_progress(me, me->inst_wire, me->inst_size, 0U);
    }
    while ( (uint32_t)KODE_RECV_CHUNK <= me->inst_owed )
    {
        me->inst_owed -= (uint32_t)KODE_RECV_CHUNK;
        QACTIVE_PUBLISH(&s_recv_ack_evt, &me->super);   /* `>|ack` */
    }
    if ( me->inst_wire < me->inst_size )
    {
        return Q_HANDLED();
    }

    /* The whole payload is in; the reader has left raw mode by itself. */
    (void)QTimeEvt_disarm(&me->inst_idle);
    if ( !me->inst_ok )
    {
        KLOG_E(TAG, "install dst=c5 failed: the image could not be decoded; "
                    "the C5 was not touched");
        inst_fail(me);
        return Q_HANDLED();
    }
    if ( me->inst_crc != me->inst_want )
    {
        KLOG_E(TAG, "install dst=c5 failed: CRC mismatch (got 0x%08lx, wanted "
                    "0x%08lx); the C5 was not touched",
               (unsigned long)me->inst_crc, (unsigned long)me->inst_want);
        inst_fail(me);
        return Q_HANDLED();
    }
    if ( kfw_end() != KODE_OK )
    {
        KLOG_E(TAG, "install dst=c5 failed: the image did not verify; the C5 "
                    "was not touched");
        inst_fail(me);
        return Q_HANDLED();
    }
    if ( !kdrv_radio_image_header_ok(me->kbuf.buf, me->kbuf.fill) )
    {
        KLOG_E(TAG, "install dst=c5 failed: that is not an ESP32-C5 firmware "
                    "image; the C5 was not touched");
        inst_fail(me);
        return Q_HANDLED();
    }
    if ( kapp_idf_desc(me->kbuf.buf, (size_t)me->kbuf.fill,
                      me->inst_project, sizeof(me->inst_project),
                      me->inst_version, sizeof(me->inst_version),
                      NULL, 0U, NULL, 0U) != KODE_OK )
    {
        KLOG_E(TAG, "install dst=c5 failed: the image carries no descriptor to "
                    "read back; the C5 was not touched");
        inst_fail(me);
        return Q_HANDLED();
    }
    KLOG_N(TAG, "install dst=c5: %luKB verified - '%s' v%s",
           (unsigned long)(me->kbuf.fill / 1024U), me->inst_project,
           me->inst_version);
    return inst_go(me);
}

/** @brief The host went quiet: a stall, and raw mode comes down with it. */
static QState inst_stall(Radio * const me)
{
    if ( ((uint8_t)INST_ARMING != me->inst)
        && ((uint8_t)INST_RECEIVING != me->inst) )
    {
        return Q_HANDLED();   /* a deadline that outlived its transfer */
    }
    KLOG_E(TAG, "install dst=c5 failed: no data for %u ms at %lu/%lu bytes; "
                "the host is presumed gone", (unsigned)KODE_RECV_IDLE_MS,
           (unsigned long)me->inst_wire, (unsigned long)me->inst_size);
    /* The reader task may still be mid-transfer: tell AO_Log to abandon raw
     * mode rather than leave it waiting for bytes that are never coming. */
    QACTIVE_POST(AO_Log, &s_raw_end_evt, &me->super);
    c5_release(me);
    c5_report_end(me, &s_flash_fail_evt.super);
    inst_end(me, &s_recv_stall_evt);
    return Q_HANDLED();
}

/**
 * @brief An update has landed: say so, answer the install, and say how to see it.
 *
 * The link is not rebuilt to read the new firmware's name back: an image that
 * is not an esp-hosted slave (the certification one) could never be read over
 * SDIO, and `>err` over an image that was written and activated would be
 * wrong. The verdict is what the C5 said: it took every chunk, and its
 * esp_ota_end() checked the image's hash before activating it. Seeing it run
 * takes the link again, which the log says.
 */
static void c5_update_done(Radio * const me)
{
    c5_report_end(me, &s_flash_done_evt.super);
    if ( (uint8_t)INST_ACTIVATED == me->inst )
    {
        KLOG_N(TAG, "install dst=c5: '%s' v%s is written and activated; the C5 "
                    "is booting it", me->inst_project, me->inst_version);
        inst_end(me, &s_recv_done_evt);
    }
    else
    {
        KLOG_N(TAG, "C5 update done: the new image is written and activated; "
                    "the C5 is booting it");
    }
    KLOG_N(TAG, "`radio_up` brings the link back, and `c5` will then name "
                "what the C5 now runs");
}

/**
 * @brief The link went away under an image that needed it.
 *
 * Called from the two no-link entries. A receive in progress is left alone -
 * the wire needs no link, and the image will ask for one when it is in -
 * but an image already verified, being pushed, or waiting on the read-back
 * has lost the thing it was waiting for.
 */
static void inst_link_gone(Radio * const me, const char *why)
{
    switch ( (inst_phase_t)me->inst )
    {
        case INST_READY:
        case INST_LINKING:
        case INST_PUSHING:
        case INST_ACTIVATED: {
            KLOG_E(TAG, "install dst=c5 failed: %s", why);
            inst_fail(me);
            break;
        }
        default: {
            break;
        }
    }
}

/* ---- The AO -------------------------------------------------------------- */

void Radio_ctor(void)
{
    Radio * const me = &l_radio;
    QActive_ctor(&me->super, Q_STATE_CAST(&Radio_initial));
    me->link  = (uint8_t)KRADIO_OFF;
    me->phase = (uint8_t)OTA_IDLE;
    me->busy  = false;
    me->winflight   = false;
    me->wreplied    = false;
    me->wdrop       = false;
    me->disc_owed   = false;
    me->wseq        = 0U;
    me->svc_worker_up = false;
    (void)memset(&me->out, 0, sizeof(me->out));
    QTimeEvt_ctorX(&me->guard, &me->super, RADIO_JOB_GUARD_SIG, 0U);
    me->guard_stale = 0U;
    me->guard_live  = false;
    me->recon = (uint8_t)RECON_DONE;
    me->wifi_stop_pending  = false;
    me->ble_stop_pending   = false;
    me->wifi_start_pending = false;
    me->ble_start_pending  = false;
    me->pb_phase     = (uint8_t)PB_IDLE;
    me->pb_job       = false;
    me->pb_rsp       = NULL;
    me->pb_who       = NULL;
    me->inst         = (uint8_t)INST_IDLE;
    me->inst_pending = false;
    me->reported     = 0U;
    me->announced    = false;
    QTimeEvt_ctorX(&me->inst_idle, &me->super, RADIO_RECV_IDLE_SIG, 0U);
    me->link_asked  = false;
    me->card_wait   = false;
    me->ask_n       = 0U;
    me->ask_scan    = (uint8_t)SCAN_ASK_NONE;
    me->ask_kind    = (uint8_t)KQP_SCAN_WIFI;
    me->ask_ms      = 0U;
    me->ask_who     = NULL;
    me->ask_rsp     = NULL;
    me->ask_wait    = NULL;
    me->ask_gen     = 0U;
    me->ask_seen    = 0U;
    me->ask_cut     = false;
    me->ble_scan_live = false;
    me->rsp_name[0] = '\0';
    me->rsp_len     = 0U;
    me->rsp_dirty   = false;
    QTimeEvt_ctorX(&me->unmount_retry, &me->super, RADIO_UNMOUNT_RETRY_SIG, 0U);
    me->retry_kind      = (uint8_t)RADIO_ASK_NONE;
    me->retry_waited_ms = 0U;
    me->begin_wait  = false;
    me->relink_owed = false;
    me->relinked    = false;
    QTimeEvt_ctorX(&me->relink_tmo, &me->super, RADIO_RELINK_SIG, 0U);
    me->relink_t0   = 0U;
    me->relink_n    = 0U;
    me->relink_dead = false;
    me->relink_wake = false;
    me->lp_rest     = false;
    me->lp_slept    = false;
#if CONFIG_KODEOS_WIFI
    QTimeEvt_ctorX(&me->wifi_retry, &me->super, WIFI_RETRY_SIG, 0U);
    me->retry_step  = 0U;
    me->retry_on    = false;
    me->retry_owed  = false;
    me->wifi_left   = false;
#endif
    /* Not known to be on: whoever ran before this boot may have left the
     * C5 driving the pin low, and a C5 still running keeps that level
     * across the P4's reset. */
    me->exp_want    = false;
    me->exp_on      = false;
    me->exp_known   = false;
    me->exp_job     = false;
    me->exp_owed    = false;
    me->exp_link    = false;
    me->amp_wished  = false;
    me->amp_off     = false;
    me->amp_low     = false;
    me->amp_known   = false;
    me->amp_answer_owed = false;
    me->linking         = false;
    QTimeEvt_ctorX(&me->exp_pulse, &me->super, EXP_PULSE_SIG, 0U);
#if defined(CONFIG_KODEOS_C5_HOLD)
    QTimeEvt_ctorX(&me->c5_boot, &me->super, C5_BOOT_SIG, 0U);
    me->c5_booting = false;
    me->amp_asked  = false;
#endif
    for ( uint8_t svc = 0U; KODE_SVC_COUNT > svc; svc++ )
    {
        QTimeEvt_ctorX(&me->svc_tmo[svc], &me->super, KODE_SVC_TMO_SIG, 0U);
        me->svc_buf[svc] = NULL;
        me->svc_last[svc] = 0U;
        me->svc_wait[svc] = false;
    }
    me->info_asked = false;
    me->park_phase  = (uint8_t)PARK_IDLE;
    me->park_target = 0U;
    me->adopt        = false;
    me->wifi_adopted = false;
    me->id_op      = 0U;   /* IOP_NONE */
    me->id_who     = NULL;
    me->id_put     = NULL;
    me->id_fill    = 0U;
    me->id_next    = 0U;
    me->wiping     = false;
    me->wipe_step  = (uint8_t)C5_WIPE_VAULT;
    me->wipe_who   = NULL;
#if CONFIG_KODE_LOWPOWER
    me->lp_who        = KODE_LOWPOWER_WHO_NONE;
    me->lp_radio      = false;
    me->lp_owed       = false;
    me->lp_round      = 0U;
    me->lp_target     = (uint8_t)KODE_LP_AWAKE;
    me->lp_job        = (uint8_t)RADIO_LPJ_NONE;
    me->lp_asleep     = false;
    me->lp_wake_owed  = false;
    me->lp_net_expected = false;
    me->lp_net_seen   = false;
    me->lp_net_round  = 0U;
    me->lp_note_valid = false;
    (void)memset(&me->lp_note, 0, sizeof(me->lp_note));
#endif
#if CONFIG_KODEOS_WIFI
    (void)memset(&me->vlist, 0, sizeof(me->vlist));
    me->vlist_ok      = false;
    me->nets_none_said = false;
    me->vlist_publish = false;
    me->remember      = false;
    me->vault_op      = (uint8_t)VOP_NONE;
    me->fast_want     = false;
    me->ps_fast       = false;
    me->ps_on         = true;    /* Radio_initial reads both from the store */
    me->join_auto     = false;
#endif
    radio_test_clear(me);
}

AO_STORAGE(radio, KQP_RADIO_QUEUE_LEN, KQP_AO_STACK_LARGE);

#if CONFIG_KODEOS_AUTOTEST
/* ---- The self-test rows: registered by kaos_radio_register() ---------------- */

/**
 * @brief The coprocessor's row: AO_Radio runs it and answers TEST_RESULT_SIG.
 *
 * The coprocessor is AO_Radio's, and its row is the fourth automatic one:
 * the link if it is down (up, it is left alone), a fresh snapshot of the
 * chip, and a scan for the antenna.
 * Every reading goes out as a log line before the verdict.
 *
 * @return false: AO_Radio answers later, with TEST_RESULT_SIG.
 */
static bool radio_row_c5(void)
{
    c5_test_request();
    return false;
}

#if CONFIG_KODEOS_BLE_CONSOLE
/**
 * @brief The BLE console's output ring, judged by the driver that owns it.
 *
 * The body lives in kdrv_ble.c for the reason the unpacker's row does: the state it
 * judges is described by headers that stop at kdrv.
 */
static bool radio_row_ble_console(void)
{
    return (kdrv_ble_console_selftest() == KODE_OK);
}
#endif

/** @brief The coprocessor's row without the production line's access point:
 *         the link, the snapshot, a sweep that completes, the controller. */
static bool radio_row_c5_user(void)
{
    c5_test_request_user();
    return false;
}
#endif

void kaos_radio_register(void)
{
#if CONFIG_KODEOS_AUTOTEST
    static const krow_t RADIO_ROWS[] = {
        { "c5", AUTOTEST_ID_C5, radio_row_c5, KROW_ASYNC, AUTOTEST_G_NONE, KODE_C5_TEST_BUDGET_MS,
          NULL, 0U, "Wi-Fi & Bluetooth",
          "Radio problem",
          AUTOTEST_ORDER_C5 },
#if CONFIG_KODEOS_BLE_CONSOLE
        { "ble_console", AUTOTEST_ID_BLE_CONSOLE, radio_row_ble_console, KROW_SYNC, AUTOTEST_G_NONE, 0U,
          NULL, 0U, "Bluetooth console",
          "Console problem",
          AUTOTEST_ORDER_BLE_CONSOLE },
#endif
        { "c5_user", AUTOTEST_ID_C5_USER, radio_row_c5_user, KROW_ASYNC, AUTOTEST_G_NONE,
          KODE_C5_TEST_BUDGET_MS,
          "c5", 0U, "Wi-Fi & Bluetooth",
          "Radio problem",
          AUTOTEST_ORDER_C5_USER },
    };

    kaos_require(krows_add(RADIO_ROWS, sizeof(RADIO_ROWS) / sizeof(RADIO_ROWS[0])),
                 "radio rows");
#endif
#if CONFIG_KODEOS_CONSOLE
    kaos_require(kverbs_add(&radio_verbs), "radio verbs");
#endif
}

void kaos_radio_start(void)
{
    Radio_ctor();
    kaos_radio_register();
    AO_START(AO_Radio, radio, KODE_PRIO_RADIO);
#if CONFIG_KODEOS_CONSOLE && (CONFIG_KODEOS_WIFI || CONFIG_KODEOS_BLE)
    /* The settings this AO answers, registered once it has the queue their
     * requests are posted to: an app that does not link it has no such keys. */
    static const kode_config_t RADIO_CONFIG[] = {
#if CONFIG_KODEOS_WIFI
        { .name = KSET_KEY_WIFI, .type = KODE_TYPE_BOOL },
#endif
#if CONFIG_KODEOS_BLE
        { .name = KSET_KEY_BLE, .type = KODE_TYPE_BOOL },
#endif
#if CONFIG_KODEOS_WIFI
        { .name = KSET_KEY_WJOIN, .type = KODE_TYPE_BOOL },
        { .name = KSET_KEY_WIFI_PS, .type = KODE_TYPE_BOOL },
#endif
    };

    kaos_require(KODE_CONFIG_ADD(AO_Radio, KQP_RADIO_CMD_MARGIN, RADIO_CONFIG),
                 "radio settings");
#endif
}

void ble_app_subscribe(QActive const *who)
{
    QActive_subscribe(who, BLE_APP_WRITE_SIG);
    QActive_subscribe(who, BLE_APP_LINK_SIG);
}

/* What each service is wanted as on a board whose store has never held its
 * flag: the build's choice, and off unless the build says otherwise. */
/* `wifi_join` on a board whose store has never held it: the build's choice. */
#if defined(CONFIG_KODEOS_WIFI_AUTOJOIN)
#define RADIO_JOIN_DEFAULT  true
#else
#define RADIO_JOIN_DEFAULT  false
#endif

#if defined(CONFIG_KODEOS_WIFI_DEFAULT_ON)
#define RADIO_WIFI_DEFAULT  true
#else
#define RADIO_WIFI_DEFAULT  false
#endif
#if defined(CONFIG_KODEOS_BLE_DEFAULT_ON)
#define RADIO_BLE_DEFAULT   true
#else
#define RADIO_BLE_DEFAULT   false
#endif

#if defined(CONFIG_KODEOS_WIFI) || !defined(CONFIG_KODEOS_BLE_AT_BOOT)
/**
 * @brief Whether a service is wanted at boot: its stored flag, or the
 *        build's default when the store has never held one.
 *
 * A flag that cannot be read is a flag that is not there, whatever the
 * reason: the default answers for it. The first request that changes the
 * service stores its choice (radio_flag_set()), and from then on this reads
 * that.
 *
 * @param key  KSET_KEY_WIFI or KSET_KEY_BLE.
 * @param dflt What an absent flag means in this build.
 * @return true when the service is to be brought up.
 */
static bool radio_want(const char *key, bool dflt)
{
    uint8_t v    = 0U;
    bool    want = dflt;

    if ( khal_nvs_get_u8(KSET_NAMESPACE, key, &v) == KODE_OK )
    {
        want = (0U != v);
    }
    return want;
}
#endif /* CONFIG_KODEOS_WIFI || !CONFIG_KODEOS_BLE_AT_BOOT */

#if defined(CONFIG_KODEOS_C5_HOLD)
/**
 * @brief The amplifier is wanted (C5_RUN_REQ_SIG): a held C5 is let go.
 *
 * The C5's firmware enables the amplifier, so a sound needs the C5 running,
 * not its link. A C5 with no link cannot say it has booted, so the answer
 * waits KODE_C5_BOOT_MS on a one-shot; a second request inside the wait is
 * answered by that one-shot. A C5 asked for here is not held again this boot
 * (radio_link_unused()).
 *
 * @param me The AO.
 * @return true when the answer is given or owed by the C5's boot; false for a
 *         C5 already running, whose pin radio_amp() judges.
 */
static bool radio_c5_run(Radio * const me)
{
    bool answered = true;

    me->amp_asked = true;
    if ( kdrv_radio_c5_held() )
    {
        uint32_t const ticks = KQP_TICKS_PER_MS(KODE_C5_BOOT_MS);

        if ( kdrv_radio_c5_hold(false) != KODE_OK )
        {
            /* Still held: answer now, or the asker waits for a boot that is
             * not happening. */
            KLOG_E(TAG, "the C5 would not be let go; the amplifier may stay off");
            me->amp_answer_owed = true;
            radio_amp_answer(me);
        }
        else
        {
            KLOG_I(TAG, "the C5 is let go: the amplifier is wanted");
            (void)QTimeEvt_disarm(&me->c5_boot);
            QTimeEvt_armX(&me->c5_boot, ticks, 0U);
            me->c5_booting = true;
        }
    }
    else if ( !me->c5_booting )
    {
        answered = false;
    }
    else
    {
        /* the one-shot already running answers this one too */
    }
    return answered;
}

/**
 * @brief C5_BOOT_SIG: the C5 let go for the amplifier has had its boot.
 *
 * @param me The AO.
 * @param e  The event.
 * @return true when @p e was C5_BOOT_SIG and has been handled.
 */
static bool radio_c5_power(Radio * const me, QEvt const * const e)
{
    bool handled = true;

    switch ( e->sig )
    {
        case C5_BOOT_SIG: {
            me->c5_booting = false;
            me->amp_answer_owed = true;
            radio_amp_answer(me);
            break;
        }
        default: {
            handled = false;
            break;
        }
    }
    return handled;
}
#endif /* CONFIG_KODEOS_C5_HOLD */

#if CONFIG_KODEOS_WIFI
/**
 * @brief Answer `config` for `wifi_join` or `wifi_ps`, setting it first when
 *        asked.
 *
 * Both are the station's manners, not services: a set is stored and holds
 * from now on. `wifi_join` is read the next time the link comes up;
 * `wifi_ps` is applied at once when the worker is free and by the settle
 * otherwise, the channel's fast mode's own path.
 *
 * @param me The AO.
 * @param r  The request.
 * @return true when @p r named one of the two.
 */
static bool radio_config_station(Radio * const me, KodeConfigReqEvt const * const r)
{
    bool       *flag = NULL;
    const char *key  = NULL;
    const char *word = "off";
    uint8_t     v    = 0U;

    if ( strcmp(r->key, KSET_KEY_WJOIN) == 0 )
    {
        flag = &me->join_auto;
        key  = KSET_KEY_WJOIN;
    }
    else if ( strcmp(r->key, KSET_KEY_WIFI_PS) == 0 )
    {
        flag = &me->ps_on;
        key  = KSET_KEY_WIFI_PS;
    }
    else
    {
        return false;
    }
    if ( r->set )
    {
        *flag = r->b;
        if ( r->b )
        {
            v = 1U;
        }
        (void)khal_nvs_set_u8(KSET_NAMESPACE, key, v);
        if ( flag == &me->ps_on )
        {
            me->ps_held = false;
            (void)radio_fast_apply(me);
        }
    }
    if ( *flag )
    {
        word = "on";
    }
    if ( r->set )
    {
        KLOG_N(TAG, "%s %s (stored)", key, word);
    }
    bool const stored = (khal_nvs_get_u8(KSET_NAMESPACE, key, &v) == KODE_OK);
    (void)kode_config_answer(&me->super, r, KODE_OK, stored, word);
    return true;
}
#endif

/**
 * @brief CONFIG_REQ_SIG for `wifi` and `ble`, which both top states answer alike.
 *
 * The value is the service's stored wish, not the link's state. A set posts
 * the request `wifi_up` or `wifi_down` (`ble_up`, `ble_down`) posts, and the
 * answer is a read posted behind it, so it reports what that request made
 * of the flag.
 *
 * @param me The AO.
 * @param e  Any event.
 * @return true when @p e was a request for one of this AO's settings.
 */
static bool radio_config(Radio * const me, QEvt const * const e)
{
    const char *key  = NULL;
    bool        want = false;
    bool        is_wifi = false;
    uint8_t     v    = 0U;

    if ( (QSignal)CONFIG_REQ_SIG != e->sig )
    {
        return false;
    }
    KodeConfigReqEvt const * const r = Q_EVT_CAST(KodeConfigReqEvt);
#if CONFIG_KODEOS_WIFI
    if ( radio_config_station(me, r) )
    {
        return true;
    }
    if ( strcmp(r->key, KSET_KEY_WIFI) == 0 )
    {
        key     = KSET_KEY_WIFI;
        want    = me->wifi_want;
        is_wifi = true;
    }
#endif
#if CONFIG_KODEOS_BLE
    if ( strcmp(r->key, KSET_KEY_BLE) == 0 )
    {
        key  = KSET_KEY_BLE;
        want = me->ble_want;
    }
#endif
    if ( NULL == key )
    {
        return true;
    }
    if ( r->set )
    {
        KodeConfigReqEvt * const again = Q_NEW_X(KodeConfigReqEvt, 1U, CONFIG_REQ_SIG);
#if CONFIG_KODEOS_WIFI
        if ( is_wifi )
        {
            if ( r->b )
            {
                wifi_start_request();
            }
            else
            {
                wifi_stop_request();
            }
        }
#endif
#if CONFIG_KODEOS_BLE
        if ( !is_wifi )
        {
            if ( r->b )
            {
                ble_start_request();
            }
            else
            {
                ble_stop_request();
            }
        }
#endif
        if ( NULL != again )
        {
            again->seq  = r->seq;
            again->set  = false;
            (void)snprintf(again->key, sizeof(again->key), "%s", r->key);
            again->i    = 0;
            again->b    = false;
            again->e    = 0U;
            again->s[0] = '\0';
            QACTIVE_POST(&me->super, &again->super, &me->super);
        }
        return true;
    }
    (void)is_wifi;
    bool const stored = (khal_nvs_get_u8(KSET_NAMESPACE, key, &v) == KODE_OK);
    const char *word = "off";
    if ( want )
    {
        word = "on";
    }
    (void)kode_config_answer(&me->super, r, KODE_OK, stored, word);
    return true;
}

/* ---- Taking the link down, and the C5 into reset ---------------------------- */

/**
 * @brief Take the link down the way `radio_down` does: the card lets go, then
 *        esp-hosted is torn down.
 *
 * A refused DISCONNECT restores the card and leaves the link as it was.
 *
 * @param me The AO.
 * @return true when the teardown is under way and the caller moves to
 *         Radio_stopping.
 */
static bool radio_teardown_begin(Radio * const me)
{
    bool going;

    radio_unmount_for_teardown(me);
    going = radio_submit(me, KDRV_RADIO_JOB_DISCONNECT);
    if ( !going )
    {
        radio_storage_ask(STORAGE_OP_MOUNT);
    }
    return going;
}

#if defined(CONFIG_KODEOS_C5_HOLD)
/**
 * @brief Hold the C5 in reset, if nothing else has it.
 *
 * Refused, harmlessly, while esp-hosted still has the reset line. A C5 in
 * reset lets go of IO5, so J3's supply is believed on from then.
 *
 * @param me The AO.
 * @return true when the C5 is held now.
 */
static bool radio_hold_c5(Radio * const me)
{
    bool held = kdrv_radio_c5_held();

    if ( (!held) && (kdrv_radio_c5_hold(true) == KODE_OK) )
    {
        held = true;
        KLOG_I(TAG, "the C5 is held in reset until something needs it");
        me->exp_on    = true;
        me->exp_known = true;
    }
    return held;
}
#endif

#if CONFIG_KODE_LOWPOWER
/* ---- The board's rest ------------------------------------------------------- */

void radio_net_left(uint32_t round)
{
    RadioNetLeftEvt *e = Q_NEW_X(RadioNetLeftEvt, 0U, RADIO_NET_LEFT_SIG);

    if ( NULL != e )
    {
        e->round = round;
        QACTIVE_POST(AO_Radio, &e->super, NULL);
    }
    else
    {
        KLOG_E(TAG, "out of events; AO_Net's leave was not passed on");
    }
}

bool radio_sleep_last_note(RadioSleepNoteEvt *out)
{
    const Radio * const me = &l_radio;

    if ( (NULL == out) || (!me->lp_note_valid) )
    {
        return false;
    }
    out->kind      = me->lp_note.kind;
    out->knocks    = me->lp_note.knocks;
    out->tx_sta    = me->lp_note.tx_sta;
    out->tx_serial = me->lp_note.tx_serial;
    out->tx_hci    = me->lp_note.tx_hci;
    return true;
}

/**
 * @brief The sentinel's note, on the sentinel's task: post a copy and return.
 *
 * @param n   What happened.
 * @param arg The AO, as kdrv_radio_sleep() was given it.
 */
static void radio_sleep_note(const kdrv_radio_sleep_note_t *n, void *arg)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the driver hands back the pointer it was given, which is this AO */
    QActive * const ao = (QActive *)arg;
    RadioSleepNoteEvt *e = Q_NEW_X(RadioSleepNoteEvt, 0U, RADIO_SLEEP_NOTE_SIG);
    /* A stood-down sentinel holds the P4 awake until somebody acts, so that
     * note yields last; the others are news the AO can live without. */
    uint16_t margin = RADIO_LP_NOTE_MARGIN;
    bool     taken  = false;

    if ( (uint8_t)KODE_RADIO_WOKE_STOOD_DOWN == n->kind )
    {
        margin = 0U;
    }
    if ( NULL != e )
    {
        e->kind      = n->kind;
        e->knocks    = n->knocks;
        e->tx_sta    = n->tx_sta;
        e->tx_serial = n->tx_serial;
        e->tx_hci    = n->tx_hci;
        /* With a margin, a full queue drops the note instead of asserting;
         * QActive_post_ recycles the event it refuses. */
        taken = QACTIVE_POST_X(ao, &e->super, margin, NULL);
    }
    if ( !taken )
    {
        static volatile uint32_t drops;   /* written by the sentinel's task only */

        drops += 1U;
        KLOG_W(TAG, "AO_Radio is behind; a wake note was dropped (%lu so far)",
               (unsigned long)drops);
    }
}

/**
 * @brief Whether the sentinel is keeping the link through a sleep.
 * @param me The AO.
 * @return true while the bus is down between wakes: the link is not to be
 *         taken down by a request or by the last service going off.
 */
static bool radio_lp_keeps_link(Radio const * const me)
{
    return me->lp_asleep;
}

void radio_lp_expect_net(bool expected)
{
    l_radio.lp_net_expected = expected;
}

/**
 * @brief Whether the OFF round may take the link down: AO_Net has left, or
 *        there is no AO_Net taking part to wait for.
 *
 * @param me    The AO.
 * @param round The OFF round.
 * @return true when nothing is left to wait for.
 */
static bool radio_lp_net_clear(Radio const * const me, uint32_t round)
{
    return (!me->lp_net_expected)
           || (me->lp_net_seen && (me->lp_net_round == round));
}

/**
 * @brief Answer the round in hand, once.
 *
 * @param me   The AO.
 * @param busy true to refuse.
 * @param why  The reason, or NULL.
 */
static void radio_lp_answer(Radio * const me, bool busy, const char *why)
{
    if ( me->lp_owed )
    {
        me->lp_owed = false;
        lowpower_ready(me->lp_who, me->lp_round, busy, why);
    }
}

/**
 * @brief What stops the link from being put to rest now.
 *
 * @param me The AO, its link up.
 * @return A busy reason, or NULL when nothing holds the channel.
 */
static const char *radio_lp_busy_why(Radio const * const me)
{
    const char *why = NULL;

    if ( ((uint8_t)OTA_IDLE != me->phase) || ((uint8_t)INST_IDLE != me->inst) )
    {
        why = RADIO_LP_WHY_UPDATE;
    }
    else if ( (uint8_t)PARK_IDLE != me->park_phase )
    {
        why = RADIO_LP_WHY_PARK;
    }
    else if ( (uint8_t)TEST_IDLE != me->test_phase )
    {
        why = RADIO_LP_WHY_TEST;
    }
    else if ( me->busy || me->winflight || me->wiping || radio_svc_busy(me) )
    {
        why = RADIO_LP_WHY_BUSY;
    }
    else
    {
        /* Nothing holds the channel. */
    }
    return why;
}

/**
 * @brief Hand the sentinel's start or stop to the radio worker.
 *
 * @param me    The AO.
 * @param start true to start the sentinel, false to stop it.
 * @param job   RADIO_LPJ_* the job is for.
 * @return true when the worker has it.
 */
static bool radio_lp_submit(Radio * const me, bool start, uint8_t job)
{
    kdrv_radio_req_t req;
    bool             ok;

    (void)memset(&req, 0, sizeof(req));
    req.type = KDRV_RADIO_JOB_SLEEP;
    req.on   = start;
#if CONFIG_KODEOS_BLE
    if ( start )
    {
        uint32_t const now = khal_time_ms();

        me->ble_fast_until = now;
        req.len = radio_ble_leg_ms(me, now);
    }
#endif
    me->lp_job = job;
    ok = radio_submit_req(me, &req);
    if ( !ok )
    {
        me->lp_job = (uint8_t)RADIO_LPJ_NONE;
    }
    return ok;
}

/**
 * @brief AO_Net has left: stop the sentinel if it runs, then answer.
 *
 * The link is not touched: tearing esp-hosted down under a running NimBLE host
 * panics. The sentinel is stopped first, so that the bus is
 * awake and nothing is suspended when the board goes to deep sleep; the C5
 * is reset by the board's power-off entry, not here.
 *
 * @param me The AO.
 */
static void radio_lp_off_go(Radio * const me)
{
    if ( (uint8_t)KRADIO_STARTING == me->link )
    {
        radio_lp_answer(me, true, RADIO_LP_WHY_STARTING);
    }
    else if ( (uint8_t)KRADIO_STOPPING == me->link )
    {
        radio_lp_answer(me, true, RADIO_LP_WHY_STOPPING);
    }
    else if ( (uint8_t)KRADIO_UP != me->link )
    {
        radio_lp_answer(me, false, NULL);       /* no link: nothing to rest */
    }
    else
    {
        const char * const why = radio_lp_busy_why(me);

        if ( NULL != why )
        {
            radio_lp_answer(me, true, why);
        }
        else if ( me->lp_asleep )
        {
            /* The bus is down between wakes: the sentinel is stopped, and
             * its outcome answers. */
            if ( !radio_lp_submit(me, false, (uint8_t)RADIO_LPJ_STOP_OFF) )
            {
                radio_lp_answer(me, true, RADIO_LP_WHY_BUSY);
            }
        }
        else
        {
            radio_lp_answer(me, false, NULL);
        }
    }
}

/**
 * @brief Decide the round in hand, with no sentinel job out.
 *
 * The round's number, target and ownership are already in the AO. Called when
 * the round arrives and the AO is free, and again when the stop of the
 * sentinel that held the round back returns.
 *
 * @param me    The AO.
 * @param round The round.
 * @param off   true when the round is for OFF.
 */
static void radio_lp_decide(Radio * const me, uint32_t round, bool off)
{
    if ( (uint8_t)KRADIO_STARTING == me->link )
    {
        radio_lp_answer(me, true, RADIO_LP_WHY_STARTING);
    }
    else if ( (uint8_t)KRADIO_STOPPING == me->link )
    {
        radio_lp_answer(me, true, RADIO_LP_WHY_STOPPING);
    }
    else if ( (uint8_t)KRADIO_UP == me->link )
    {
        const char * const why = radio_lp_busy_why(me);

        if ( NULL != why )
        {
            radio_lp_answer(me, true, why);
        }
        else if ( off )
        {
            if ( radio_lp_net_clear(me, round) )
            {
                radio_lp_off_go(me);
            }
            /* else: AO_Net has not left yet; radio_net_left() continues. */
        }
        else if ( me->lp_asleep )
        {
            /* Already at rest, and a stop that AWAKE owed is not wanted now:
             * if this round is abandoned, AWAKE comes again and owes it. */
            me->lp_wake_owed = false;
            radio_lp_answer(me, false, NULL);
        }
        else if ( !radio_lp_submit(me, true, (uint8_t)RADIO_LPJ_START) )
        {
            radio_lp_answer(me, true, RADIO_LP_WHY_BUSY);
        }
        else
        {
            /* The start's outcome answers. */
        }
    }
    else if ( off )
    {
        if ( radio_lp_net_clear(me, round) )
        {
            radio_lp_off_go(me);
        }
    }
    else
    {
        radio_lp_answer(me, false, NULL);       /* no link: nothing to rest */
    }
}

/**
 * @brief LOWPOWER_ROUND_SIG, in whichever state the link is.
 *
 * A round that arrives while the stop of an awake board is out is not
 * refused: it is owed, and radio_lp_sleep_done() decides it when the stop
 * returns. Any other job out is answered busy.
 *
 * @param me The AO.
 * @param r  The round.
 */
static void radio_lp_round(Radio * const me, const LowPowerRoundEvt * const r)
{
    bool const off    = ((uint8_t)KODE_LP_OFF == r->target);

    me->lp_rest      = true;     /* AWAKE says when it was abandoned */
    me->lp_round     = r->round;
    me->lp_target    = r->target;
    me->lp_owed      = true;
    /* A round ends the radio wake's work: cleared here, so the stop that
     * returns later, whose sleep_done() calls radio_lp_radio_done(), says no
     * done for a wake this round ended. */
    me->lp_radio     = false;
    if ( (uint8_t)RADIO_LPJ_STOP_AWAKE == me->lp_job )
    {
        /* Owed: the stop's outcome decides. */
    }
    else if ( (uint8_t)RADIO_LPJ_NONE != me->lp_job )
    {
        radio_lp_answer(me, true, RADIO_LP_WHY_BUSY);   /* the sentinel is mid-step */
    }
    else
    {
        radio_lp_decide(me, r->round, off);
    }
}

/**
 * @brief RADIO_NET_LEFT_SIG: AO_Net's sessions are closed.
 *
 * @param me The AO.
 * @param e  The news.
 */
static void radio_lp_net_left(Radio * const me, const RadioNetLeftEvt * const e)
{
    me->lp_net_seen  = true;
    me->lp_net_round = e->round;
    if ( me->lp_owed && ((uint8_t)KODE_LP_OFF == me->lp_target)
        && (me->lp_round == e->round) && ((uint8_t)RADIO_LPJ_NONE == me->lp_job) )
    {
        radio_lp_off_go(me);
    }
}

/**
 * @brief The busy reason a refused start gives.
 * @param rc What kdrv_radio_sleep() returned.
 * @return The reason, short enough for the round.
 */
static const char *radio_lp_why_of(kode_status_t rc)
{
    const char *why = RADIO_LP_WHY_FAILED;

    switch ( rc )
    {
        case KODE_IO_ERROR: why = RADIO_LP_WHY_PS;   break;
        case KODE_DENIED:   why = RADIO_LP_WHY_SDIO; break;
        case KODE_ERROR:    why = RADIO_LP_WHY_WAKE; break;
        default:                                     break;
    }
    return why;
}

/**
 * @brief Tell AO_LowPower this AO's part of a radio wake is finished.
 *
 * Once for each radio wake: the work of this AO in it is the bus being awake,
 * and the bus is awake when the sentinel is stopped or was never armed.
 *
 * @param me The AO.
 */
static void radio_lp_radio_done(Radio * const me)
{
    if ( me->lp_radio )
    {
        me->lp_radio = false;
        lowpower_radio_done(me->lp_who);
    }
}

/**
 * @brief Say what the sentinel did while it ran.
 */
static void radio_lp_stats_log(void)
{
    kdrv_radio_sleep_stats_t st;

    kdrv_radio_sleep_stats(&st);
    KLOG_I(TAG, "sleep sentinel stopped: wakes c5=%lu tx=%lu, deaf=%lu, "
                "stood down=%lu, knocks=%lu (most %u)",
           (unsigned long)st.wakes_c5, (unsigned long)st.wakes_tx,
           (unsigned long)st.deaf, (unsigned long)st.stood_down,
           (unsigned long)st.knocks, (unsigned)st.knocks_max);
}

/**
 * @brief The sentinel job came back.
 *
 * @param me The AO.
 * @param rc What kdrv_radio_sleep() returned.
 */
static void radio_lp_sleep_done(Radio * const me, kode_status_t rc)
{
    uint8_t const job    = me->lp_job;

    me->lp_job = (uint8_t)RADIO_LPJ_NONE;
    if ( (uint8_t)RADIO_LPJ_STOP_AWAKE == job )
    {
        radio_lp_radio_done(me);   /* the bus is as awake as it will get */
    }
    if ( (uint8_t)RADIO_LPJ_START == job )
    {
        if ( KODE_OK == rc )
        {
            /* A wake that came while the start was out is paid on the way
             * out of the outcome. */
            me->lp_asleep = true;
            radio_lp_answer(me, false, NULL);
        }
        else
        {
            KLOG_W(TAG, "the sleep sentinel would not start (%d)", (int)rc);
            me->lp_wake_owed = false;
            radio_lp_answer(me, true, radio_lp_why_of(rc));
        }
    }
    else if ( KODE_OK == rc )
    {
        me->lp_wake_owed = false;
        me->lp_asleep = false;
        radio_lp_stats_log();
        if ( ((uint8_t)RADIO_LPJ_STOP_OFF == job) && me->lp_owed )
        {
            radio_lp_off_go(me);
        }
        else if ( ((uint8_t)RADIO_LPJ_STOP_AWAKE == job) && me->lp_owed )
        {
            /* A round came while the stop was out and waited for it. */
            radio_lp_decide(me, me->lp_round, (uint8_t)KODE_LP_OFF == me->lp_target);
        }
        else
        {
            /* Nothing is owed. */
        }
    }
    else
    {
        KLOG_W(TAG, "the sleep sentinel would not stop (%d); it stays armed",
               (int)rc);
        bool const stop = ((uint8_t)RADIO_LPJ_STOP_OFF == job)
                          || ((uint8_t)RADIO_LPJ_STOP_AWAKE == job);

        if ( stop && me->lp_owed )
        {
            radio_lp_answer(me, true, RADIO_LP_WHY_DEAF);
        }
    }
}

/**
 * @brief Stop the sentinel now, or owe it to the job that is out.
 * @param me The AO, its link up and the sentinel armed.
 */
static void radio_lp_wake(Radio * const me)
{
    if ( ((uint8_t)RADIO_LPJ_NONE != me->lp_job) || me->winflight )
    {
        me->lp_wake_owed = true;
    }
    else if ( !radio_lp_submit(me, false, (uint8_t)RADIO_LPJ_STOP_AWAKE) )
    {
        /* Owed, and paid as the next job returns: a sentinel left armed
         * keeps the bus down under a board that is awake. */
        KLOG_W(TAG, "the sleep sentinel could not be stopped now; it is owed");
        me->lp_wake_owed = true;
    }
    else
    {
        me->lp_wake_owed = false;
    }
}

/**
 * @brief Pay a stop that AWAKE owed while a job was out.
 * @param me The AO.
 */
static void radio_lp_wake_pay(Radio * const me)
{
    if ( me->lp_wake_owed && me->lp_asleep
        && ((uint8_t)RADIO_LPJ_NONE == me->lp_job) && (!me->winflight) )
    {
        radio_lp_wake(me);
    }
}

/**
 * @brief LOWPOWER_STATE_SIG: on AWAKE, or on a radio wake, whatever the rest
 *        changed comes back.
 *
 * It repeats (an abandoned round publishes it again), so every step looks at
 * what is still to do. A radio wake wants the bus and nothing else of the
 * rest: the sentinel is stopped, and AO_LowPower is told when it is.
 *
 * @param me The AO.
 * @param s  The change.
 */
static void radio_lp_state(Radio * const me, const LowPowerStateEvt * const s)
{
    bool const radio = ((uint8_t)KODE_LP_RADIO == s->state);

    me->lp_rest = ((uint8_t)KODE_LP_AWAKE != s->state);
    if ( (uint8_t)KODE_LP_ASLEEP == s->state )
    {
        me->lp_slept = true;
    }
    if ( ((uint8_t)KODE_LP_AWAKE == s->state) || radio )
    {
        me->lp_owed     = false;     /* an abandoned round is not answered */
        me->lp_net_seen = false;
        me->lp_radio    = radio;
        if ( (me->lp_asleep || ((uint8_t)RADIO_LPJ_NONE != me->lp_job))
            && ((uint8_t)KRADIO_UP == me->link) )
        {
            radio_lp_wake(me);   /* the stop's outcome says done */
        }
        else
        {
            radio_lp_radio_done(me);
        }
    }
    if ( (uint8_t)KODE_LP_AWAKE == s->state )
    {
#if CONFIG_KODEOS_WIFI
        /* A board that slept tries at once: somebody may be holding it. An
         * abandoned round was no sleep, and its wait goes on as it was. */
        if ( me->lp_slept && wifi_retry_wanted(me) )
        {
            (void)QTimeEvt_disarm(&me->wifi_retry);
            me->retry_step = 0U;
            me->retry_on   = true;
            me->retry_owed = true;
        }
        (void)wifi_retry_pay(me);
#endif
        me->lp_slept = false;
        if ( me->relink_wake )
        {
            static const QEvt relink = QEVT_INITIALIZER(RADIO_RELINK_SIG);

            me->relink_wake = false;
            QACTIVE_POST(&me->super, &relink, &me->super);
        }
    }
}

/**
 * @brief RADIO_SLEEP_NOTE_SIG: the sentinel woke the P4, or could not sleep.
 *
 * @param me The AO.
 * @param n  The note.
 */
static void radio_lp_note(Radio * const me, const RadioSleepNoteEvt * const n)
{
    me->lp_note.kind      = n->kind;
    me->lp_note.knocks    = n->knocks;
    me->lp_note.tx_sta    = n->tx_sta;
    me->lp_note.tx_serial = n->tx_serial;
    me->lp_note.tx_hci    = n->tx_hci;
    me->lp_note_valid     = true;
    if ( (uint8_t)KODE_RADIO_WOKE_STOOD_DOWN == n->kind )
    {
        KLOG_W(TAG, "the sleep sentinel stood down: the bus is up, so the "
                    "board wakes");
        if ( me->lp_asleep )
        {
            lowpower_request(KODE_LP_AWAKE, RADIO_LP_SRC);
        }
    }
    else if ( (uint8_t)KODE_RADIO_WOKE_DEAF == n->kind )
    {
        KLOG_W(TAG, "the C5 did not answer a wake (%u knocks)",
               (unsigned)n->knocks);
    }
    else
    {
        const char *who = "the P4's traffic";

        if ( (uint8_t)KODE_RADIO_WOKE_C5 == n->kind )
        {
            who = "the C5";
        }
        KLOG_D(TAG, "woken by %s (%u knocks)", who, (unsigned)n->knocks);
    }
}

/**
 * @brief The link is gone from under a round or the sentinel.
 *
 * The entry of the link-less states. A round that waited on the link is
 * refused, and so is an OFF round whose stop of the sentinel is still with
 * the worker: answered over it, OFF would deep-sleep the P4 with the worker
 * inside the driver, and the stop's outcome is dropped with the link. Any
 * other OFF round has nothing left to rest, and is answered once AO_Net has
 * left.
 *
 * @param me The AO.
 */
static void radio_lp_link_gone(Radio * const me)
{
    bool const stop_out = ((uint8_t)RADIO_LPJ_STOP_OFF == me->lp_job)
                          || ((uint8_t)RADIO_LPJ_STOP_AWAKE == me->lp_job);

    me->lp_asleep    = false;
    me->lp_job       = (uint8_t)RADIO_LPJ_NONE;
    me->lp_wake_owed = false;
    radio_lp_radio_done(me);   /* no link, nothing left to wait for */
    if ( me->lp_owed )
    {
        if ( ((uint8_t)KODE_LP_OFF != me->lp_target) || stop_out )
        {
            radio_lp_answer(me, true, RADIO_LP_WHY_LOST);
        }
        else if ( radio_lp_net_clear(me, me->lp_round) )
        {
            radio_lp_answer(me, false, NULL);
        }
        else
        {
            /* AO_Net has not left yet; radio_net_left() answers. */
        }
    }
}

#else /* !CONFIG_KODE_LOWPOWER */

/**
 * @brief Built without low power there is no sentinel: the link is never kept for it.
 * @param me The AO.
 * @return false.
 */
static bool radio_lp_keeps_link(Radio const * const me)
{
    (void)me;
    return false;
}
#endif /* CONFIG_KODE_LOWPOWER */

/**
 * @brief Claim the C5, start the workers, read the stored wishes, and link if one is on.
 *
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to Radio_starting when WiFi or BLE is wanted and the
 *         connect was submitted; to Radio_off otherwise, or without the jobs'
 *         memory or the radio worker.
 */
static QState Radio_initial(Radio * const me, void const * const par)
{
    (void)par;

    kaos_claim(KBSP_RES_C5, me, "AO_Radio");
#if CONFIG_KODE_LOWPOWER
    me->lp_who = lowpower_join_radio(&me->super, "radio");
    if ( KODE_LOWPOWER_WHO_NONE != me->lp_who )
    {
        QActive_subscribe(&me->super, LOWPOWER_ROUND_SIG);
        QActive_subscribe(&me->super, LOWPOWER_STATE_SIG);
    }
#endif
    /* The amplifier's enable is the C5's IO4: radio_amp_request() says either way. */
    QActive_subscribe(&me->super, C5_RUN_REQ_SIG);
    QActive_subscribe(&me->super, C5_AMP_OFF_SIG);

    /* Nothing else to subscribe to: the open response and every chunk are
     * posted straight here (AO_Storage answers `who`), and the loan protocol is the
     * same one AO_AppMgr uses to write the apps region. */
#if CONFIG_KODEOS_BLE
    (void)kdrv_ble_set_cb(&radio_on_ble);
    (void)kdrv_ble_app_set_write_cb(&radio_on_ble_write);
#if CONFIG_KODEOS_BLE_PAIRING_PASSKEY
    (void)kdrv_ble_set_pair_cb(&radio_on_ble_pair);
#endif
#endif

    /* What the workers read lives in PSRAM, taken once and cleared at every
     * start. */
    if ( NULL == me->jobs )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; PSRAM storage aligned for any object, and sized for this one */
        me->jobs = (RadioJobs *)khal_alloc_psram(4U, sizeof(RadioJobs));
    }
    if ( NULL == me->jobs )
    {
        KLOG_E(TAG, "no room for the workers' jobs; the radio stays off");
        return Q_TRAN(&Radio_off);
    }
    (void)memset(me->jobs, 0, sizeof(*me->jobs));

    /* The radio worker's stack is INTERNAL: its bring-up initialises NVS and
     * its BLE jobs use NimBLE's bond store, which is flash through SPI1. */
    if ( kqp_worker_start(&me->worker, &me->super, RADIO_WORKER_NAME,
                         RADIO_WORKER_STACK, KODE_PRIO_RADIO,
                         RADIO_WORKER_DEPTH, false) != KODE_OK )
    {
        KLOG_E(TAG, "no worker; the radio stays off");
        return Q_TRAN(&Radio_off);
    }
    kdrv_radio_on_link_lost(&radio_on_link_lost);
    /* The KODE sends touch no flash, so their worker's stack is PSRAM. */
    me->svc_worker_up = (kqp_worker_start(&me->svc_worker, &me->super,
                                          RADIO_SVC_WORKER_NAME,
                                          RADIO_SVC_WORKER_STACK, KODE_PRIO_RADIO,
                                          RADIO_SVC_WORKER_DEPTH, true)
                         == KODE_OK);
    if ( !me->svc_worker_up )
    {
        KLOG_E(TAG, "no worker for the KODE services; vault, identity and BLE "
                    "keys are refused");
    }
#if CONFIG_KODEOS_WIFI
    kdrv_wifi_scan_progress(&radio_on_scan_more);
    kdrv_wifi_on_lost(&radio_on_wifi_lost);
#endif

    /* The stored desired state. A flag the store has never held takes its
     * build's default (KODEOS_WIFI_DEFAULT_ON, KODEOS_BLE_DEFAULT_ON), and
     * with both of those off a board with an empty NVS boots with the radio
     * off. */
#if CONFIG_KODEOS_WIFI
    me->wifi_want = radio_want(KSET_KEY_WIFI, RADIO_WIFI_DEFAULT);
    radio_hints_load(me);
    me->join_auto = radio_want(KSET_KEY_WJOIN, RADIO_JOIN_DEFAULT);
    me->ps_on     = radio_want(KSET_KEY_WIFI_PS, true);
#else
    /* Built without WiFi: a `wifi=1` another firmware left in the store is
     * not a reason to bring the link up for a service that is not here. */
    me->wifi_want = false;
#endif
#if defined(CONFIG_KODEOS_BLE_AT_BOOT)
    /* The store is not consulted: this firmware is on the air from boot. */
    me->ble_want  = true;
#else
    me->ble_want  = radio_want(KSET_KEY_BLE, RADIO_BLE_DEFAULT);
#endif
#if CONFIG_KODEOS_BLE
    ble_on_note(me->ble_want);
    me->ble_fast_owed = true;
#endif

    /* Builds before the vault kept every saved network, password and all,
     * here on the P4. Those are not used any more and must not stay. */
    {
        uint8_t probe = 0U;
        size_t  n     = sizeof(probe);

        /* DENIED is "not there"; any other answer, a short buffer included,
         * means the record is. */
        if ( (khal_nvs_get_blob(RADIO_OLD_WIFI_NS, RADIO_OLD_WIFI_KEY, &probe, &n)
             != KODE_DENIED)
            && (khal_nvs_erase(RADIO_OLD_WIFI_NS, RADIO_OLD_WIFI_KEY) == KODE_OK) )
        {
            KLOG_N(TAG, "the old Wi-Fi store on this chip is erased; join again "
                        "to save a network");
        }
    }

    KLOG_D(TAG, "boot: wifi_want=%d ble_want=%d",
           (int)me->wifi_want, (int)me->ble_want);
    if ( (me->wifi_want || me->ble_want) && radio_begin_connect(me) )
    {
        return Q_TRAN(&Radio_starting);
    }
    return Q_TRAN(&Radio_off);
}

/**
 * @brief What every link-less state answers.
 *
 * A request that needs the C5 is refused *with an answer*, never with
 * silence: the console closes its commands on these events, and a caller
 * that gets nothing back cannot tell "no link" from "still working".
 */
static QState Radio_down(Radio * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case STORAGE_CMD_RSP_SIG: {
            /* The link is down or on its way: an answer of AO_Storage is
             * the unmount a teardown was owed, or nobody's. */
            if ( radio_asked(me) == RADIO_ASK_TEARDOWN )
            {
                radio_teardown_answered(me);
            }
            status = Q_HANDLED();
            break;
        }
        case Q_ENTRY_SIG: {
#if CONFIG_KODE_LOWPOWER
            radio_lp_link_gone(me);
#endif
            /* The link went away under a park: nobody is left to answer it,
             * and AO_Boot is waiting for the answer. */
            if ( ((uint8_t)PARK_QUIET == me->park_phase)
                || ((uint8_t)PARK_MUTE == me->park_phase)
                || ((uint8_t)PARK_ASKED == me->park_phase) )
            {
                radio_park_end(me, false, "the link went down");
            }
            status = Q_HANDLED();
            break;
        }
        case RADIO_UNMOUNT_RETRY_SIG: {
            radio_teardown_retry(me);
            status = Q_HANDLED();
            break;
        }
        case RADIO_JOB_SIG:
        case RADIO_JOB_GUARD_SIG:
        case RADIO_LINK_LOST_SIG: {
            /* Radio_off has no job of its own to hear about: a done event
             * is read so that the worker is free again, a lost link is old
             * news, and a guard that fires is nobody's to answer. */
            (void)radio_outcome(me, e);
            status = Q_HANDLED();
            break;
        }
        case RADIO_SVC_JOB_SIG: {
            /* A send that was queued before the link went: nobody waits on
             * it any more (radio_svc_forget() saw to that). */
            (void)radio_svc_job_done(me, Q_EVT_CAST(KqpWorkerDoneEvt));
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODE_LOWPOWER
        case LOWPOWER_ROUND_SIG: {
            radio_lp_round(me, Q_EVT_CAST(LowPowerRoundEvt));
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_STATE_SIG: {
            radio_lp_state(me, Q_EVT_CAST(LowPowerStateEvt));
            status = Q_HANDLED();
            break;
        }
        case RADIO_SLEEP_NOTE_SIG: {
            radio_lp_note(me, Q_EVT_CAST(RadioSleepNoteEvt));
            status = Q_HANDLED();
            break;
        }
        case RADIO_NET_LEFT_SIG: {
            radio_lp_net_left(me, Q_EVT_CAST(RadioNetLeftEvt));
            status = Q_HANDLED();
            break;
        }
#endif
        case RADIO_PARK_REQ_SIG: {
            /* No link, so no station of ours to keep: the jump goes on and
             * the next firmware resets the C5. */
            KLOG_I(TAG, "no link to the C5: nothing to park");
            boot_radio_parked();
            status = Q_HANDLED();
            break;
        }
        case ID_SIGN_REQ_SIG:
        case ID_REQ_SIG: {
            /* The identity lives on the C5, and the link is down. */
            id_refuse(e, KODE_ST_REFUSED);
            status = Q_HANDLED();
            break;
        }
        case C5_WIPE_REQ_SIG: {
            /* So do the vault and the BLE keys. */
            wipe_reply(Q_EVT_CAST(C5WipeReqEvt)->who, (uint8_t)C5_WIPE_VAULT,
                       KODE_ST_REFUSED);
            status = Q_HANDLED();
            break;
        }
#if RADIO_ASKS
        case RADIO_SCAN_ASK_SIG: {
            /* The link is on its way down: the scan would bring it back up
             * against the request that took it away. */
            if ( !scan_ask_refuse_off(me, e) )
            {
                KLOG_N(TAG, "the link is going down; a scan is refused");
                scan_ask_refuse(e, KODE_NO_RESOURCES);
            }
            status = Q_HANDLED();
            break;
        }
        case RADIO_SCAN_OWED_SIG: {
            status = Q_HANDLED();   /* a kick from a link that is gone */
            break;
        }
#endif
        case RADIO_EXP_OWED_SIG: {
            /* A kick from a link that is gone: the no-link entry has already
             * answered the request, once. */
            status = Q_HANDLED();
            break;
        }
        case RADIO_PROBE_REQ_SIG: {
            /* The check never brings the link up: it measures a radio that
             * is already running. */
            KLOG_N(TAG, "radio check: no link to the C5; refused");
            probe_refuse(Q_EVT_CAST(RadioProbeReqEvt), KODE_DENIED);
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODEOS_WIFI
        case WIFI_SCAN_REQ_SIG: {
            KLOG_W(TAG, "no link to the C5; `radio_up` first");
            wifi_publish_scan_end(me, 0U, false);
            status = Q_HANDLED();
            break;
        }
        case WIFI_JOIN_REQ_SIG:
        case WIFI_LEAVE_REQ_SIG:
        case WIFI_AUTO_REQ_SIG: {
            if ( (QSignal)WIFI_LEAVE_REQ_SIG == e->sig )
            {
                wifi_owed_clear(me);
            }
            KLOG_W(TAG, "no link to the C5; `radio_up` first");
            wifi_publish_state(me);
            status = Q_HANDLED();
            break;
        }
        case WIFI_RSSI_REQ_SIG: {
            /* No warning, unlike the typed verbs above: this comes from the
             * settings screen's tick every couple of seconds. The published
             * state is the answer: rssi=0 with the link down. */
            wifi_publish_state(me);
            status = Q_HANDLED();
            break;
        }
        case WIFI_START_REQ_SIG: {
            /* The flag is the pending state, and Radio_up's entry reconciles
             * to it. Radio_off and Radio_failed override this to start the
             * link too; starting and stopping only record the flag. */
            radio_flag_set(me, SVC_WIFI, true);
            wifi_publish_state(me);
            status = Q_HANDLED();
            break;
        }
        case WIFI_STOP_REQ_SIG: {
            radio_flag_set(me, SVC_WIFI, false);
            wifi_publish_state(me);   /* already off: the truth, promptly */
            status = Q_HANDLED();
            break;
        }
#endif /* CONFIG_KODEOS_WIFI */
#if CONFIG_KODEOS_BLE
        case BLE_PAIR_OPEN_REQ_SIG:
        case BLE_UNPAIR_REQ_SIG: {
            KLOG_W(TAG, "no link to the C5; `radio_up` first");
            ble_publish_state(me);
            status = Q_HANDLED();
            break;
        }
        case BLE_START_REQ_SIG: {
            radio_flag_set(me, SVC_BLE, true);
            ble_publish_state(me);
            status = Q_HANDLED();
            break;
        }
        case BLE_STOP_REQ_SIG: {
            radio_flag_set(me, SVC_BLE, false);
            ble_publish_state(me);
            status = Q_HANDLED();
            break;
        }
        case BLE_SCAN_REQ_SIG: {
            KLOG_W(TAG, "no link to the C5; `radio_up` first");
            ble_publish_scan_end(me, false);
            status = Q_HANDLED();
            break;
        }
        case BLE_STATE_REQ_SIG:
        case BLE_LINK_SIG: {
            /* No link to the C5, so there is nothing to put back on the air
             * either: down here the two signals really are the same event. */
            ble_publish_state(me);
            status = Q_HANDLED();
            break;
        }
        case BLE_APP_WRITE_SIG: {
            /* Posted by the host task before the link went: still a write
             * the central made, so it goes out. */
            QACTIVE_PUBLISH(e, &me->super);
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODEOS_BLE_PAIRING_PASSKEY
        case BLE_PAIR_TAKE_SIG: {
            /* Handled down here as well, and not because a pairing is
             * plausible with the C5's controller unreachable: an untaken
             * record stays pending, and the next take - a boot's worth of
             * time later - would hand somebody a code that expired long ago.
             * Collecting it is what throws it away. */
            ble_publish_pair(me);
            status = Q_HANDLED();
            break;
        }
        case BLE_PAIR_REJECT_REQ_SIG: {
            (void)kdrv_ble_pair_reject();
            status = Q_HANDLED();
            break;
        }
#endif
#endif
#if CONFIG_KODEOS_WIFI
        case WIFI_LIST_REQ_SIG: {
            /* The vault is on the C5, and the C5 is not linked: what this
             * side last heard, marked not fresh. */
            wifi_publish_saved(me, false);
            status = Q_HANDLED();
            break;
        }
        case WIFI_FORGET_REQ_SIG: {
            WifiJoinEvt const * const f = Q_EVT_CAST(WifiJoinEvt);
            KLOG_W(TAG, "the vault is on the C5 and the link is down: "
                        "'%s' is not forgotten", f->ssid);
            status = Q_HANDLED();
            break;
        }
        case WIFI_STATE_REQ_SIG: {
            /* Answerable with no link at all: "not connected" is the answer,
             * and the driver's own view is already that. */
            wifi_publish_state(me);
            status = Q_HANDLED();
            break;
        }
#endif /* CONFIG_KODEOS_WIFI */
        case FLASH_REQ_SIG: {
            KLOG_E(TAG, "no link to the C5: it cannot be updated over a link "
                        "that is not there. `radio_up` first.");
            status = Q_HANDLED();
            break;
        }
        /* A terminal install needs no link to receive: the image lands in
         * PSRAM and asks for the link once it has verified (inst_go). */
        case RADIO_RECV_REQ_SIG: {
            status = inst_request(me, Q_EVT_CAST(AppRecvReqEvt));
            break;
        }
        case CONSOLE_RAW_RSP_SIG: {
            status = inst_armed(me, Q_EVT_CAST(ConsoleRawRspEvt));
            break;
        }
        case RECV_DATA_SIG: {
            status = inst_data(me, Q_EVT_CAST(RecvDataEvt));
            break;
        }
        case RADIO_RECV_IDLE_SIG: {
            status = inst_stall(me);
            break;
        }
        case RADIO_STATE_REQ_SIG: {
            radio_publish(me, (kradio_link_t)me->link);
            status = Q_HANDLED();
            break;
        }
        case RADIO_FIELD_REQ_SIG: {
            /* The snapshot answers with no link at all - "not valid" is an
             * answer - and the console is waiting on it. */
            radio_field_serve(Q_EVT_CAST(RadioFieldReqEvt));
            status = Q_HANDLED();
            break;
        }
        default: {
            if ( radio_exp_power(me, e) )
            {
                status = Q_HANDLED();
            }
            else if ( radio_amp(me, e) )
            {
                status = Q_HANDLED();
            }
#if CONFIG_KODEOS_BLE
            else if ( radio_ble_rsp(me, e) )
            {
                status = Q_HANDLED();
            }
            else if ( radio_ble_value(me, e) )
            {
                status = Q_HANDLED();
            }
#endif
#if defined(CONFIG_KODEOS_C5_HOLD)
            else if ( radio_c5_power(me, e) )
            {
                status = Q_HANDLED();
            }
#endif
            else if ( radio_config(me, e) )
            {
                status = Q_HANDLED();
            }
            else
            {
                status = Q_SUPER(&QHsm_top);
            }
            break;
        }
    }
    return status;
}

/**
 * @brief Switch a service on from a state that has no link to run it on.
 *
 * Records the desire and brings the link up for it; Radio_up's entry then
 * reconciles to the flags. Called from the two children that can act on it:
 * the superstate records the flag too, but cannot choose a transition target
 * without asking which child is active.
 *
 * @param me The AO.
 * @param e  The WIFI_START_REQ_SIG or BLE_START_REQ_SIG being handled.
 * @return What the calling state handler must return.
 */
#if CONFIG_KODEOS_WIFI || CONFIG_KODEOS_BLE
static QState radio_service_on(Radio * const me, QEvt const * const e)
{
    bool const wifi = ((QSignal)WIFI_START_REQ_SIG == e->sig);

#if CONFIG_KODEOS_WIFI
    if ( wifi )
    {
        radio_flag_set(me, SVC_WIFI, true);
    }
#endif
#if CONFIG_KODEOS_BLE
    if ( !wifi )
    {
        radio_flag_set(me, SVC_BLE, true);
    }
#endif

    if ( radio_begin_connect(me) )
    {
        return Q_TRAN(&Radio_starting);
    }

    /* The link could not even be asked for, so nothing further will answer:
     * say so here, or a console that disarmed its watchdog for this verb
     * waits on an event that is never coming. */
#if CONFIG_KODEOS_WIFI
    if ( wifi )
    {
        wifi_publish_state(me);
    }
#endif
#if CONFIG_KODEOS_BLE
    if ( !wifi )
    {
        ble_publish_state(me);
    }
#endif
    return Q_HANDLED();
}
#endif /* CONFIG_KODEOS_WIFI || CONFIG_KODEOS_BLE */

#if CONFIG_KODEOS_WIFI
/**
 * @brief A join or an autojoin with no link: keep it, and bring the link up.
 * @param me The AO.
 * @param e  The WIFI_JOIN_REQ_SIG or WIFI_AUTO_REQ_SIG.
 * @return What the calling state handler must return.
 */
static QState radio_wifi_request_on(Radio * const me, QEvt const * const e)
{
    wifi_owe(me, e);
    radio_flag_set(me, SVC_WIFI, true);
    if ( radio_begin_connect(me) )
    {
        return Q_TRAN(&Radio_starting);
    }
    wifi_owed_clear(me);
    wifi_publish_state(me);
    return Q_HANDLED();
}

#endif /* CONFIG_KODEOS_WIFI */

#if RADIO_ASKS
/**
 * @brief An asked scan with no link: hold it, and bring the link up.
 *
 * The link is asked for as `radio_up` asks for it - no service is switched
 * on, and nothing is stored - so a scan leaves the settings as it found them.
 *
 * @param me The AO.
 * @param e  The RADIO_SCAN_ASK_SIG.
 * @return What the calling state handler must return.
 */
static QState radio_scan_ask_on(Radio * const me, QEvt const * const e)
{
    if ( !scan_ask_take(me, e) )
    {
        return Q_HANDLED();
    }
    if ( radio_begin_connect(me) )
    {
        return Q_TRAN(&Radio_starting);
    }
    scan_ask_end(me, KODE_ERROR);
    return Q_HANDLED();
}
#endif /* RADIO_ASKS */

/**
 * @brief A request for J3's supply with no link: an "off" brings it up.
 *
 * The rail's switch is a pin of the C5, so an "off" the rail is not known to
 * be in needs the link. It is asked for as an asked scan asks (no service on,
 * nothing stored), its connect drives the pin, and the link is the asker's
 * for the rest of the boot. An "on" needs no link: a reset of the C5. Said
 * refused (KODE_DENIED) when the link cannot be asked for, and by
 * Radio_failed when it does not come up.
 *
 * @param me The AO.
 * @param e  The EXP_POWER_REQ_SIG.
 * @return What the calling state handler must return.
 */
static QState radio_exp_ask_link(Radio * const me, QEvt const * const e)
{
    (void)radio_exp_power(me, e);
    if ( me->exp_owed && !me->exp_want )
    {
        if ( radio_begin_connect(me) )
        {
            return Q_TRAN(&Radio_starting);
        }
        radio_exp_said(me, false, KODE_DENIED);
    }
    return Q_HANDLED();
}

/**
 * @brief No link and none on its way: a request that needs one brings it up.
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState Radio_off(Radio * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            me->busy = false;
            /* No link: an "on" that waited for one is told now, the amplifier
             * as the C5 left it, so a sound never waits past the hope. */
            radio_amp_answer(me);
            /* No link, so nobody's claim on it survives and no stop can
             * still be owed - the teardown took both services with it. */
            me->wifi_stop_pending  = false;
            me->ble_stop_pending   = false;
            me->wifi_start_pending = false;
            me->ble_start_pending  = false;
#if CONFIG_KODEOS_WIFI
            wifi_owed_clear(me);
#endif
#if RADIO_ASKS
            scan_ask_end(me, KODE_ERROR);   /* an asked scan waits for no link */
#endif
            /* A row that was waiting on this link was already answered
             * wherever the link failed; this only makes sure. */
            radio_test_clear(me);
            inst_link_gone(me, "the link is off");
            /* Whoever asked for the link by itself has had it taken away,
             * by `radio_down` or by the link failing; the next one asks
             * again. */
            me->link_asked = false;
            me->relink_dead = false;
            me->relink_wake = false;
#if defined(CONFIG_KODEOS_C5_HOLD)
            /* Nothing wants the link, so the C5 is put in reset, where it
             * draws nothing and its amplifier is shut down: the first thing a
             * boot with no radio does (C5_EN is pulled up, so the C5 started
             * with the board). The bring-up and C5_RUN_REQ_SIG let it go;
             * refused, harmlessly, while esp-hosted has the reset line. */
            if ( !me->wifi_want && !me->ble_want && !me->amp_asked )
            {
                (void)radio_hold_c5(me);
            }
#endif
            /* The header's supply, after the hold: a request that waits is
             * carried out now if it can be, or said owed to the next link. */
            radio_exp_no_link(me);
            radio_publish(me, KRADIO_OFF);
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODEOS_WIFI
        case WIFI_START_REQ_SIG:
#endif
#if CONFIG_KODEOS_BLE
        case BLE_START_REQ_SIG:
#endif
#if CONFIG_KODEOS_WIFI || CONFIG_KODEOS_BLE
        {
            status = radio_service_on(me, e);
            break;
        }
#endif
#if CONFIG_KODEOS_WIFI
        case WIFI_JOIN_REQ_SIG:
        case WIFI_AUTO_REQ_SIG: {
            status = radio_wifi_request_on(me, e);
            break;
        }
#endif
#if RADIO_ASKS
        case RADIO_SCAN_ASK_SIG: {
            status = radio_scan_ask_on(me, e);
            break;
        }
#endif
        case EXP_POWER_REQ_SIG: {
            status = radio_exp_ask_link(me, e);
            break;
        }
        case RADIO_UP_REQ_SIG: {
            status = radio_begin_connect(me) ? Q_TRAN(&Radio_starting)
                                             : Q_HANDLED();
            break;
        }
        case RADIO_TEST_REQ_SIG: {
            /* The row needs the link, so it brings it up exactly as
             * `radio_up` would - and like `radio_up`, it records nothing in
             * the settings store: a test that switched WiFi on for good
             * would be a mutation wearing a test's clothes. */
            me->test_phase = (uint8_t)TEST_LINK;
            me->test_user  = Q_EVT_CAST(RadioTestReqEvt)->user;
            if ( radio_begin_connect(me) )
            {
                status = Q_TRAN(&Radio_starting);
            }
            else
            {
                c5_test_end(me, false, "the worker refused the connect job");
                status = Q_HANDLED();
            }
            break;
        }
        case RADIO_DOWN_REQ_SIG: {
            radio_publish(me, KRADIO_OFF);   /* already there */
            status = Q_HANDLED();
            break;
        }
        default: {
            status = Q_SUPER(&Radio_down);
            break;
        }
    }
    return status;
}

/**
 * @brief The link is being built: Radio_up once it is, Radio_failed when it is not.
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState Radio_starting(Radio * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            radio_publish(me, KRADIO_STARTING);
            radio_svc_forget(me);
            me->linking = true;
            status = Q_HANDLED();
            break;
        }
        case Q_EXIT_SIG: {
            me->linking = false;
            status = Q_HANDLED();
            break;
        }
        case RADIO_UP_REQ_SIG: {
            /* Already on its way, so the bring-up already running is
             * what serves it. Answered rather than dropped: the console has
             * said `>ok` by the time this arrives, which is what would make
             * a silent drop invisible instead of merely unhelpful. */
            radio_publish(me, KRADIO_STARTING);
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODEOS_WIFI
        case WIFI_JOIN_REQ_SIG:
        case WIFI_AUTO_REQ_SIG: {
            /* Kept, and made once the link is up. */
            wifi_owe(me, e);
            radio_flag_set(me, SVC_WIFI, true);
            wifi_publish_state(me);
            status = Q_HANDLED();
            break;
        }
#endif
#if RADIO_ASKS
        case RADIO_SCAN_ASK_SIG: {
            (void)scan_ask_take(me, e);   /* held, and started once the link is up */
            status = Q_HANDLED();
            break;
        }
#endif
        case RADIO_TEST_REQ_SIG: {
            /* The link is on its way up for somebody; the row rides it
             * rather than asking for a second connect the worker would
             * refuse. */
            if ( (uint8_t)TEST_IDLE == me->test_phase )
            {
                me->test_phase = (uint8_t)TEST_LINK;
                me->test_user  = Q_EVT_CAST(RadioTestReqEvt)->user;
                KLOG_N(TAG, "test c5: the link is coming up; the row waits for it");
            }
            status = Q_HANDLED();
            break;
        }
        case RADIO_JOB_SIG:
        case RADIO_JOB_GUARD_SIG:
        case RADIO_LINK_LOST_SIG: {
            if ( !radio_outcome(me, e) )
            {
                status = Q_HANDLED();
                break;
            }
            RadioOutcome const * const job = &me->out;
            if ( ((uint8_t)RADIO_OUT_CONNECT_DONE == job->evt)
                && ((uint8_t)KODE_OK == job->status) )
            {
                /* A C5 the last firmware parked, kept for a boot that joins:
                 * its station is adopted before anything else is asked. */
                me->adopt = kdrv_radio_adopted() && me->wifi_want
                            && ((uint8_t)INST_LINKING != me->inst);
                me->wifi_adopted = false;
            }
            if ( me->card_wait
                && ((uint8_t)RADIO_OUT_LINK_LOST == job->evt) )
            {
                /* The link dropped while the card was coming back. What
                 * Radio_up does with a lost link, from one step earlier. */
                me->card_wait = false;
                if ( (uint8_t)TEST_IDLE != me->test_phase )
                {
                    c5_test_end(me, false, "the link dropped as it came up");
                }
                radio_unmount_for_teardown(me);
                radio_disconnect_after_loss(me);
                status = Q_TRAN(&Radio_failed);
            }
            else if ( (uint8_t)RADIO_OUT_CONNECT_DONE != job->evt )
            {
                status = Q_HANDLED();   /* not ours - a stale outcome */
            }
            else if ( (uint8_t)KODE_OK != job->status )
            {
                /* The driver deinitialised on every failure path, so the
                 * plain mount path is already safe again. */
                me->busy = false;
                c5_test_link_failed(me, (kode_status_t)job->status);
                radio_restore_plain_sd();
                status = Q_TRAN(&Radio_failed);
            }
            else if ( (uint8_t)INST_LINKING == me->inst )
            {
                /* Up for an image already verified in PSRAM: the card is
                 * not remounted (Radio_up's entry says why), so there is
                 * nothing to wait for. */
                me->busy = false;
                status = Q_TRAN(&Radio_up);
            }
            else if ( radio_ask(me, STORAGE_OP_MOUNT, RADIO_ASK_MOUNT) )
            {
                /* The link is up and the slave (plus the driver's belt) has
                 * the card powered again: give it back, through the hosted
                 * path, and hold here until that has run. The channel stays
                 * busy meanwhile - nothing goes to the C5 while the card is
                 * being asked who it is. */
                me->card_wait = true;
                status = Q_HANDLED();
            }
            else
            {
                /* No event left to carry the answer, so none will come:
                 * ask the plain way rather than wait for ever. */
                radio_storage_ask(STORAGE_OP_MOUNT);
                me->busy = false;
                status = Q_TRAN(&Radio_up);
            }
            break;
        }
        case STORAGE_CMD_RSP_SIG: {
            /* An answer of AO_Storage, in the order the asks went: the
             * unmount that gates the reset, the remount after the link, or
             * an unmount of a teardown that was owed. "Mounted" or "no card"
             * both mean the card has settled. */
            switch ( radio_asked(me) )
            {
                case RADIO_ASK_CONNECT: {
                    status = radio_connect_answer(me);
                    break;
                }
                case RADIO_ASK_MOUNT: {
                    if ( me->card_wait )
                    {
                        me->card_wait = false;
                        me->busy      = false;
                        status = Q_TRAN(&Radio_up);
                    }
                    else
                    {
                        status = Q_HANDLED();
                    }
                    break;
                }
                case RADIO_ASK_TEARDOWN: {
                    radio_teardown_answered(me);
                    status = Q_HANDLED();
                    break;
                }
                default: {
                    status = Q_HANDLED();
                    break;
                }
            }
            break;
        }
        case RADIO_UNMOUNT_RETRY_SIG: {
            if ( (uint8_t)RADIO_ASK_CONNECT == me->retry_kind )
            {
                /* The card was still mounted at the last answer: asked
                 * again, and when that cannot be done in the window left,
                 * the bring-up ends as the last answer would have ended it. */
                if ( radio_unmount_refire(me) )
                {
                    status = Q_HANDLED();
                }
                else
                {
                    status = radio_connect_answer(me);
                }
            }
            else
            {
                status = Q_SUPER(&Radio_down);
            }
            break;
        }
        default: {
            status = Q_SUPER(&Radio_down);
            break;
        }
    }
    return status;
}

/**
 * @brief The link is being taken down: Radio_off once the disconnect is done.
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState Radio_stopping(Radio * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            radio_publish(me, KRADIO_STOPPING);
            status = Q_HANDLED();
            break;
        }
        case RADIO_UP_REQ_SIG: {
            /* The teardown owns the C5 until it finishes, and claiming the
             * link here would not survive Radio_off's entry anyway. Say so
             * and answer with the state; asking again once it is off works. */
            KLOG_W(TAG, "the link is on its way down; `radio_up` again once "
                        "it is off");
            radio_publish(me, KRADIO_STOPPING);
            status = Q_HANDLED();
            break;
        }
        case RADIO_TEST_REQ_SIG: {
            /* The row brings the link up from `off`, not from half way
             * down: answered, and asking again once it is off works. */
            c5_test_end(me, false, "the link is on its way down; test again "
                                   "once it is off");
            status = Q_HANDLED();
            break;
        }
        case RADIO_RECV_REQ_SIG: {
            /* The same, for an image: the wire is not taken for it while
             * the link is half way down. Answered, because the console
             * waits on this with its watchdog disarmed. */
            KLOG_E(TAG, "install dst=c5 refused: the link is on its way down; "
                        "try again once it is off");
            QACTIVE_PUBLISH(&s_recv_fail_evt, &me->super);
            status = Q_HANDLED();
            break;
        }
        case RADIO_JOB_SIG:
        case RADIO_JOB_GUARD_SIG:
        case RADIO_LINK_LOST_SIG: {
            if ( !radio_outcome(me, e) )
            {
                status = Q_HANDLED();
                break;
            }
            RadioOutcome const * const job = &me->out;
            if ( (uint8_t)RADIO_OUT_DISCONNECT_DONE == job->evt )
            {
                me->busy = false;
                radio_restore_plain_sd();
                status = Q_TRAN(&Radio_off);
            }
            else
            {
                status = Q_HANDLED();
            }
            break;
        }
        default: {
            status = Q_SUPER(&Radio_down);
            break;
        }
    }
    return status;
}

/**
 * @brief The wait is over: build the link again, if it is still for somebody.
 *
 * @param me The AO, in `failed`.
 * @return What the calling state handler must return.
 */
static QState radio_relink_due(Radio * const me)
{
    if ( me->lp_rest )
    {
        me->relink_wake = true;
    }
    else if ( me->busy || me->winflight )
    {
        /* A teardown is still out: its end is not ours to wait on. */
        QTimeEvt_armX(&me->relink_tmo, KQP_TICKS_PER_MS(RADIO_RELINK_BUSY_MS), 0U);
    }
    else if ( (me->wifi_want || me->ble_want) && radio_relink_left(me) )
    {
        ++me->relink_n;
        me->relink_owed = true;
        if ( radio_begin_connect(me) )
        {
            return Q_TRAN(&Radio_starting);
        }
        me->relink_owed = false;
        radio_relink_plan(me);
        radio_publish(me, KRADIO_FAILED);
    }
    else
    {
        radio_relink_plan(me);          /* says so, if nothing is left */
        radio_publish(me, KRADIO_FAILED);
    }
    return Q_HANDLED();
}

/**
 * @brief The link could not be built, or was lost; a few more tries while a service wants it.
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState Radio_failed(Radio * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            radio_relink_plan(me);
            radio_publish(me, KRADIO_FAILED);
            /* No link to drive the amplifier's pin over: an "on" that waits
             * is told now, as the C5 left it. */
            radio_amp_answer(me);
            /* Every way in is a bring-up that failed - its reset of the C5
             * perhaps never driven - or a link lost, often to a C5 that
             * restarted by itself: what IO5 holds is not known any more. */
            me->exp_known = false;
            radio_exp_no_link(me);
            /* And in the services' own voices: `wifi_up` and `ble_up` close on
             * WIFI_STATE_SIG and BLE_STATE_SIG with their watchdog disarmed,
             * and without these the console waits until the board is reset. */
#if CONFIG_KODEOS_WIFI
            wifi_publish_state(me);
#endif
#if CONFIG_KODEOS_BLE
            ble_publish_state(me);
#endif
            /* The link that was claimed or owed things is gone with it. */
            me->wifi_stop_pending  = false;
            me->ble_stop_pending   = false;
            me->wifi_start_pending = false;
            me->ble_start_pending  = false;
#if CONFIG_KODEOS_WIFI
            wifi_owed_clear(me);
#endif
#if RADIO_ASKS
            scan_ask_end(me, KODE_ERROR);   /* an asked scan waits for no link */
#endif
            radio_test_clear(me);
            inst_link_gone(me, "the link could not be built, or was lost, "
                               "under the image");
            status = Q_HANDLED();
            break;
        }
        case Q_EXIT_SIG: {
            (void)QTimeEvt_disarm(&me->relink_tmo);
            status = Q_HANDLED();
            break;
        }
        case RADIO_RELINK_SIG: {
            status = radio_relink_due(me);
            break;
        }
#if CONFIG_KODEOS_WIFI
        case WIFI_START_REQ_SIG:
#endif
#if CONFIG_KODEOS_BLE
        case BLE_START_REQ_SIG:
#endif
#if CONFIG_KODEOS_WIFI || CONFIG_KODEOS_BLE
        {
            status = radio_service_on(me, e);
            break;
        }
#endif
#if CONFIG_KODEOS_WIFI
        case WIFI_JOIN_REQ_SIG:
        case WIFI_AUTO_REQ_SIG: {
            status = radio_wifi_request_on(me, e);
            break;
        }
#endif
#if RADIO_ASKS
        case RADIO_SCAN_ASK_SIG: {
            status = radio_scan_ask_on(me, e);
            break;
        }
#endif
        case EXP_POWER_REQ_SIG: {
            status = radio_exp_ask_link(me, e);
            break;
        }
        case RADIO_UP_REQ_SIG: {
            status = radio_begin_connect(me) ? Q_TRAN(&Radio_starting)
                                             : Q_HANDLED();
            break;
        }
        case RADIO_TEST_REQ_SIG: {
            /* The row needs the link, so it brings it up exactly as
             * `radio_up` would - and like `radio_up`, it records nothing in
             * the settings store: a test that switched WiFi on for good
             * would be a mutation wearing a test's clothes. */
            me->test_phase = (uint8_t)TEST_LINK;
            me->test_user  = Q_EVT_CAST(RadioTestReqEvt)->user;
            if ( radio_begin_connect(me) )
            {
                status = Q_TRAN(&Radio_starting);
            }
            else
            {
                c5_test_end(me, false, "the worker refused the connect job");
                status = Q_HANDLED();
            }
            break;
        }
        case RADIO_DOWN_REQ_SIG: {
            /* Nothing left to tear down - every path into this state
             * deinitialised (or has a DISCONNECT in flight); settle. */
            radio_restore_plain_sd();
            status = Q_TRAN(&Radio_off);
            break;
        }
        case RADIO_JOB_SIG:
        case RADIO_JOB_GUARD_SIG:
        case RADIO_LINK_LOST_SIG: {
            if ( !radio_outcome(me, e) )
            {
                status = Q_HANDLED();
                break;
            }
            RadioOutcome const * const job = &me->out;
            if ( (uint8_t)RADIO_OUT_DISCONNECT_DONE == job->evt )
            {
                /* The cleanup ordered by the LINK_LOST path finished: the
                 * controller is free. */
                me->busy = false;
                if ( me->relink_owed && me->lp_rest )
                {
                    /* Nothing resets the C5 under a board going to sleep. */
                    me->relink_wake = true;
                }
                else if ( me->relink_owed && radio_begin_connect(me) )
                {
                    /* The connect resets the C5, and Radio_up's entry
                     * reconciles to the flags as on any bring-up. */
                    status = Q_TRAN(&Radio_starting);
                    break;
                }
                else
                {
                    /* Nothing owed, or the card would not be asked: the next
                     * wait, if there is one, is what is left. */
                }
                me->relink_owed = false;
                /* The plain mount path is back. */
                radio_restore_plain_sd();
            }
            status = Q_HANDLED();
            break;
        }
        default: {
            status = Q_SUPER(&Radio_down);
            break;
        }
    }
    return status;
}

/** @brief Owed: the settle carries it out when the job in flight is done. */
#define RADIO_BUSY_OWED   "is owed and runs as soon as the channel is free"
/** @brief Answered on the spot, with nothing in it. */
#define RADIO_BUSY_EMPTY  "answered with an empty listing"
/** @brief A scan turned away: nothing was started, and the asker is told `busy`. */
#define RADIO_BUSY_ASKED  "was answered busy; ask again"
/** @brief Turned away: nothing was started and nothing is remembered. */
#define RADIO_BUSY_NO     "was not started; ask again"

/**
 * @brief Say that the one job in flight is not done, and what became of the
 *        request that arrived while it was running.
 *
 * `wifi_down` is written into the debts and carried out by radio_settle()
 * once the channel is free; `wifi_scan` is answered at once with an empty
 * listing, the event its asker waits on; the rest are turned away and say so.
 *
 * @param what The verb the request came in as.
 * @param how  What happened to it: one of the four RADIO_BUSY_* words. Owed is
 *             DEBUG, a scan answered busy INFO, a turned-away one a WARN.
 */
static void radio_say_busy(const char *what, const char *how)
{
    /* Owed: its outcome is told when it runs. A scan answered busy got the
     * answer it is built to take. Turned away or answered empty: a WARN. */
    if ( strcmp(how, RADIO_BUSY_OWED) == 0 )
    {
        KLOG_D(TAG, "the C5 is busy; %s %s", what, how);
    }
    else if ( strcmp(how, RADIO_BUSY_ASKED) == 0 )
    {
        KLOG_I(TAG, "the C5 is busy; %s %s", what, how);
    }
    else
    {
        KLOG_W(TAG, "the C5 is busy; %s %s", what, how);
    }
}

/**
 * @brief Radio_up's answer to a job outcome, or to the link being lost.
 *
 * A function of its own because the autojoin round, a child of Radio_up,
 * hands it everything it does not take: an event reaches one state's
 * handler once, and the outcome has already been read out of it by then.
 *
 * @param me  The AO.
 * @param job The outcome, from radio_outcome().
 * @return The QState the calling state handler must return.
 */
static QState Radio_up_outcome(Radio * const me, RadioOutcome const * const job)
{
    QState status = Q_HANDLED();

    /* A finished job is the moment the channel is free, so it is
     * also the moment to act on any flag that changed while it was
     * not. Every outcome below settles except the lost link, which
     * is on its way somewhere the flags cannot be served at all. */
    bool settle = true;

    radio_relink_judge(me, job);
#if CONFIG_KODEOS_WIFI
    if ( (uint8_t)RADIO_OUT_WIFI_PS_DONE != job->evt )
    {
        me->ps_held = false;   /* another job came through: retry */
    }
#endif

    switch ( (radio_out_t)job->evt )
    {
        case RADIO_OUT_LINK_LOST: {
            /* The transport died under us and took the hosted
             * mount's controller with it. Let go of the card, then
             * have the worker deinitialise so `failed` always means
             * "cleanly torn down, retry from scratch". */
            probe_lost(me);
            if ( (uint8_t)TEST_IDLE != me->test_phase )
            {
                c5_test_end(me, false, "the link dropped mid-test");
            }
            me->ble_scan_live = false;   /* its listing ends with the link */
#if CONFIG_KODEOS_WIFI
            if ( me->scan_live || scan_ask_running(me, (uint8_t)KQP_SCAN_WIFI) )
            {
                /* A listing in flight has a console waiting on its
                 * end marker with the watchdog disarmed, or an asker
                 * waiting on its end. Whatever channels got through
                 * are real; the rest never will, and only the marker
                 * says so. */
                KLOG_W(TAG, "the link dropped mid-scan");
                wifi_finish_scan(me, false);
            }
#endif
            radio_relink_note(me, (kode_status_t)job->status);
            radio_unmount_for_teardown(me);
            radio_disconnect_after_loss(me);
            settle = false;
            status = Q_TRAN(&Radio_failed);
            break;
        }
        case RADIO_OUT_C5_INFO_DONE: {
            me->busy = false;
            me->test_job = false;
            if ( probe_owns(me, (uint8_t)PB_CARD) )
            {
                probe_done(me, job);
            }
            else if ( (uint8_t)TEST_INFO == me->test_phase )
            {
                /* A failed job left the snapshot invalid, and the
                 * judge says so in its own words. */
                c5_test_judge_info(me);
            }
            status = Q_HANDLED();
            break;
        }
        case RADIO_OUT_AMP_DONE: {
            me->busy = false;
            if ( (uint8_t)KODE_OK != job->status )
            {
                /* Believed at submit, so forgotten - and the wish with it,
                 * or the settle that follows would ask again at RPC speed. */
                KLOG_E(TAG, "the C5 would not drive the amplifier's enable");
                me->amp_known  = false;
                me->amp_wished = false;
            }
            radio_amp_answer(me);
            if ( (uint8_t)PARK_MUTE == me->park_phase )
            {
                /* Silent, or as silent as it can be made: now the C5. */
                radio_park_send(me);
                settle = false;
            }
            status = Q_HANDLED();
            break;
        }
        case RADIO_OUT_EXP_POWER_DONE: {
            bool const on = me->exp_on;   /* the switch the job carried */

            me->busy    = false;
            me->exp_job = false;
            if ( (uint8_t)KODE_OK != job->status )
            {
                /* A request made since wants the rail the other way, which
                 * is where the refusal leaves it: the settle says so. */
                bool const later = me->exp_want != on;

                /* Believed at submit, so put back - and the wish is
                 * dropped with it, or the settle that follows would
                 * ask again at RPC speed. A new request asks again. */
                KLOG_E(TAG, "the C5 would not switch J3's supply; "
                            "ask again with exp_power");
                me->exp_on   = !me->exp_on;
                me->exp_want = me->exp_on;
                radio_exp_said(me, on, KODE_IO_ERROR);
                if ( !later )
                {
                    me->exp_owed = false;   /* the request is answered: refused */
                }
            }
            else
            {
                radio_exp_said(me, on, KODE_OK);
            }
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODEOS_WIFI
        case RADIO_OUT_WIFI_SCAN_DONE: {
            me->busy = false;
            /* The sweep the radio check cut short has ended: its request
             * is over, and the next sweep runs whole. */
            kdrv_wifi_scan_abort(false);
            if ( probe_owns(me, (uint8_t)PB_SWEEP) )
            {
                /* The check's own sweep: read for two names, never listed. */
                probe_done(me, job);
            }
            else if ( me->test_job )
            {
                /* The row's own scan: judged, never listed. Nobody
                 * typed `wifi_scan`, so nobody is waiting for the
                 * entries, and a listing would land on the wire in
                 * the middle of a test run. */
                me->test_job = false;
                c5_test_judge_scan(me, (uint8_t)KODE_OK == job->status);
            }
            else
            {
                /* Most of the listing is already on the wire; `ok` still
                 * means the sweep reached its end, so a cut-short one is a
                 * refusal behind the entries found, never a whole list. */
                wifi_finish_scan(me, (uint8_t)KODE_OK == job->status);
            }
            status = Q_HANDLED();
            break;
        }
        case RADIO_OUT_WIFI_PROBE_DONE: {
            me->busy = false;
            if ( probe_owns(me, (uint8_t)PB_W24) || probe_owns(me, (uint8_t)PB_W5) )
            {
                probe_done(me, job);
            }
            status = Q_HANDLED();
            break;
        }
        case RADIO_OUT_WIFI_JOIN_DONE: {
            me->busy = false;
            if ( (uint8_t)RECON_ADOPT == me->recon )
            {
                /* Not a join of ours: the kept station, taken over
                 * (or not). */
                radio_adopt_done(me, (kode_status_t)job->status);
                status = Q_HANDLED();
                break;
            }
            /* A new association starts on the default power save. */
            me->fast_want = false;
            if ( (uint8_t)KODE_OK == job->status )
            {
                uint32_t ip = 0U;
                (void)kdrv_wifi_state(&ip);
                KLOG_N(TAG, "joined '%s' as %u.%u.%u.%u", me->ssid,
                       (unsigned)(ip & 0xFFU),
                       (unsigned)((ip >> 8) & 0xFFU),
                       (unsigned)((ip >> 16) & 0xFFU),
                       (unsigned)((ip >> 24) & 0xFFU));
                radio_hint_learn(me);
                wifi_retry_stop(me);
                /* Only now: credentials that have not worked are
                 * not worth keeping, and one that failed must never
                 * overwrite one that works. */
                if ( me->remember )
                {
                    radio_vault_remember(me);
                }
            }
            (void)memset(me->pass, 0, sizeof(me->pass));
            me->remember = false;
            wifi_publish_state(me);
            status = Q_HANDLED();
            break;
        }
        case RADIO_OUT_WIFI_PREPARE_DONE: {
            /* This side is ready for a join the C5 makes: ask the
             * vault, and keep the channel until it answers. */
            me->busy = false;
            if ( ((uint8_t)KODE_OK == job->status)
                && radio_vault_join(me, me->ssid) )
            {
                me->busy = true;
            }
            else
            {
                wifi_publish_state(me);
            }
            status = Q_HANDLED();
            break;
        }
        case RADIO_OUT_WIFI_LEAVE_DONE: {
            me->busy = false;
            wifi_publish_state(me);
            status = Q_HANDLED();
            break;
        }
        case RADIO_OUT_WIFI_STOP_DONE: {
            me->busy = false;
            /* The wish ends with the station; what the C5 holds is
             * set back to the default at the next join. */
            me->fast_want = false;
            wifi_publish_state(me);
            status = Q_HANDLED();
            break;
        }
        case RADIO_OUT_WIFI_PS_DONE: {
            me->busy = false;
            if ( (uint8_t)KODE_OK != job->status )
            {
                /* Believed at submit, so put back: the C5 still
                 * holds what it held before. */
                KLOG_W(TAG, "the C5 would not change the power save");
                me->ps_fast = !me->ps_fast;
                me->ps_held = true;
            }
            status = Q_HANDLED();
            break;
        }
        case RADIO_OUT_WIFI_RSSI_DONE: {
            me->busy = false;
            wifi_publish_state(me);
            status = Q_HANDLED();
            break;
        }
#endif /* CONFIG_KODEOS_WIFI */
#if CONFIG_KODEOS_BLE
        case RADIO_OUT_BLE_START_DONE: {
            me->busy = false;
            ble_publish_state(me);
            if ( me->test_job )
            {
                /* The row's own start. Judged first: a pass submits
                 * the listen and holds the channel, so the invariant
                 * below finds it busy and waits for the next settled
                 * job, as it does behind any other. */
                me->test_job = false;
                c5_test_judge_ble_start(me, (uint8_t)KODE_OK == job->status);
            }
            if ( radio_ble_adv_reconcile(me) )
            {
                /* Up, so on the air: the flag already said this. The BLE
                 * step of a bring-up is not over (RECON_BLE is "start, then
                 * the advert"), and settling here would let the retry wave
                 * the chain past a job not yet with the worker. */
                settle = false;
            }
            else
            {
                /* The start failed, or somebody is already connected
                 * and there is nothing to advertise for: either way
                 * the BLE step ends here. */
                radio_reconcile_ble_done(me);
            }
            status = Q_HANDLED();
            break;
        }
        case RADIO_OUT_BLE_ADV_DONE: {
            me->busy = false;
            ble_publish_state(me);
            /* Whether it worked or not, the BLE step of a bring-up
             * is over: hand the baton on rather than let the chain
             * stall on a service that refused to come up. An advert
             * asked for by hand carries no baton to hand off - the
             * recon guard inside is what tells the two apart. */
            radio_reconcile_ble_done(me);
            if ( probe_owns(me, (uint8_t)PB_ADV) )
            {
                probe_done(me, job);
            }
            status = Q_HANDLED();
            break;
        }
        case RADIO_OUT_BLE_FIND_DONE: {
            me->busy = false;
            if ( probe_owns(me, (uint8_t)PB_FIND) )
            {
                probe_done(me, job);
            }
            status = Q_HANDLED();
            break;
        }
        case RADIO_OUT_BLE_UNPAIR_DONE: {
            me->busy = false;
            if ( me->wiping && ((uint8_t)C5_WIPE_BLE == me->wipe_step) )
            {
                if ( (uint8_t)KODE_OK == job->status )
                {
                    radio_wipe_end(me, (uint8_t)C5_WIPE_DONE, KODE_ST_OK);
                }
                else
                {
                    radio_wipe_end(me, (uint8_t)C5_WIPE_BLE, KODE_ST_STORAGE);
                }
            }
            ble_publish_state(me);
            status = Q_HANDLED();
            break;
        }
        case RADIO_OUT_BLE_QUIET_DONE:
        case RADIO_OUT_BLE_STOP_DONE: {
            me->busy = false;
            ble_publish_state(me);
            if ( (uint8_t)PARK_QUIET == me->park_phase )
            {
                /* The phone is hung up: now the amplifier and the C5.
                 * Nothing is settled meanwhile, the jump is on its way. */
                radio_park_mute(me);
                settle = false;
            }
            status = Q_HANDLED();
            break;
        }
        case RADIO_OUT_BLE_SCAN_DONE: {
            me->busy = false;
            if ( me->test_job )
            {
                /* The row's listen: judged, never listed, for the
                 * reason its WiFi scan is not. */
                me->test_job = false;
                c5_test_judge_ble_scan(me, (uint8_t)KODE_OK == job->status);
            }
            else if ( scan_ask_running(me, (uint8_t)KQP_SCAN_BLE) )
            {
                /* An asked listen: its devices and end go to the asker. */
                ble_ask_finish(me, (uint8_t)KODE_OK == job->status);
            }
            else if ( (uint8_t)KODE_OK == job->status )
            {
                me->ble_scan_live = false;
                ble_publish_scan(me);
            }
            else
            {
                me->ble_scan_live = false;
                ble_publish_scan_end(me, false);
            }
            status = Q_HANDLED();
            break;
        }
#endif
#if CONFIG_KODE_LOWPOWER
        case RADIO_OUT_SLEEP_DONE: {
            me->busy = false;
            radio_lp_sleep_done(me, (kode_status_t)job->status);
            status = Q_HANDLED();
            break;
        }
#endif
        default: {
            me->busy = false;
            status = Q_HANDLED();
            break;
        }
    }

#if CONFIG_KODE_LOWPOWER
    radio_lp_wake_pay(me);
#endif

    /* The channel just came free, so this is where both kinds of
     * unfinished business get served: a stop that was owed, and a
     * bring-up step that a busy refusal dropped. */
    if ( settle )
    {
        radio_settle(me);
#if CONFIG_KODEOS_BLE
        radio_reconcile_retry(me);
        /* Last, and after the debts on purpose: a stop that was owed
         * clears the flag this reads, so paying it first is what
         * stops `ble_down` from being answered with a fresh advert.
         * A scan ending is the ordinary case here - the radio comes
         * back and the board goes with it. */
        (void)radio_ble_adv_reconcile(me);
#endif
    }

    return status;
}

/**
 * @brief The link is up: the services are reconciled to their flags and requests served.
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState Radio_up(Radio * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            if ( (uint8_t)INST_LINKING == me->inst )
            {
                /* The link came up for an image already verified in PSRAM:
                 * the push goes first and nothing is reconciled, since a
                 * service would hold the channel, and once the image is
                 * activated the link is down for this boot. The card is not
                 * remounted: the push unmounts it again before the C5
                 * reboots, and the update's end mounts it. */
                me->recon = (uint8_t)RECON_DONE;
                me->inst  = (uint8_t)INST_READY;
                radio_publish(me, KRADIO_UP);
                inst_post_go(me);
                status = Q_HANDLED();
                break;
            }
            /* The card is back already: Radio_starting held until its
             * remount had run. */
            me->relink_dead = false;
            radio_publish(me, KRADIO_UP);
            radio_name_board();
            radio_svc_link(me);
            /* Reconcile the link to the desired state, one service at a time,
             * by posted requests: an entry action must not start a job. A
             * plain `radio_up` with both flags off reconciles to nothing and
             * keeps its link until `radio_down`. Nothing to settle on the way
             * in: the debts are incurred only with the link up, and both
             * no-link entries clear them. */
            /* The one debt incurred with no link: the header's supply. The
             * bring-up left the rail as the wish was at submit when its
             * switch went through; it is not known when it did not. A wish
             * changed since stays owed, and its kick is posted ahead of the
             * reconcile so its short job goes before the autojoin. Only
             * Radio_up applies the kick. */
            me->exp_on    = me->exp_link;
            me->exp_known = kdrv_radio_exp_switched();
            if ( (me->exp_want != me->exp_on) || (me->exp_owed && !me->exp_known) )
            {
                static const QEvt kick = QEVT_INITIALIZER(RADIO_EXP_OWED_SIG);

                me->exp_owed = true;
                QACTIVE_POST(&me->super, &kick, &me->super);
            }
            else
            {
                radio_exp_settled(me);   /* the bring-up carried the wish */
            }
            me->recon = (uint8_t)RECON_START;
            radio_reconcile_next(me);
            /* A C5 the bring-up reset raised IO4 at its own boot; one the
             * last firmware parked holds whatever it was left at. */
            me->amp_known = !kdrv_radio_adopted();
            me->amp_low   = false;
            if ( me->amp_known )
            {
                radio_amp_answer(me);
            }
            if ( me->amp_wished && ((!me->amp_known) || (me->amp_low != me->amp_off)) )
            {
                /* Posted again, as the header's supply is, to be applied once
                 * the channel is free. */
                radio_amp_request(!me->amp_off);
            }
            /* The row that brought this link up: its next step is posted
             * behind the reconcile's, so a service somebody switched on
             * gets the channel first and the row waits its turn. */
            if ( (uint8_t)TEST_LINK == me->test_phase )
            {
                static const QEvt step = QEVT_INITIALIZER(RADIO_TEST_STEP_SIG);
                me->test_phase = (uint8_t)TEST_INFO;
                QACTIVE_POST(&me->super, &step, &me->super);
            }
            status = Q_HANDLED();
            break;
        }
        case Q_EXIT_SIG: {
            probe_lost(me);
            /* Whatever the C5 was asked about the identity will not be
             * answered on this link: the asker hears so now. */
            (void)QTimeEvt_disarm(&me->svc_tmo[KODE_SVC_ID]);
            radio_svc_cancel(me, KODE_SVC_COUNT);
            radio_id_lost(me);
            radio_wipe_lost(me, KODE_SVC_COUNT);
#if CONFIG_KODEOS_WIFI
            /* The C5 boots on the default power save; nothing it held
             * survives the link. */
            me->fast_want = false;
            me->ps_fast   = false;
            wifi_retry_stop(me);    /* the bring-up has its own round */
#endif
            status = Q_HANDLED();
            break;
        }
        case RADIO_PROBE_REQ_SIG: {
            probe_take(me, e);
            status = Q_HANDLED();
            break;
        }
        case ID_SIGN_REQ_SIG:
        case ID_REQ_SIG: {
            radio_id_request(me, e);
            status = Q_HANDLED();
            break;
        }
        case C5_WIPE_REQ_SIG: {
            radio_wipe_start(me, Q_EVT_CAST(C5WipeReqEvt));
            status = Q_HANDLED();
            break;
        }
        case RADIO_TEST_REQ_SIG: {
            if ( (uint8_t)TEST_IDLE != me->test_phase )
            {
                KLOG_W(TAG, "test c5: already running; ignored");
            }
            else
            {
                me->test_phase = (uint8_t)TEST_INFO;
                me->test_user  = Q_EVT_CAST(RadioTestReqEvt)->user;
                if ( me->busy )
                {
                    KLOG_I(TAG, "test c5: waiting for the channel");
                    me->test_pending = true;
                }
                else
                {
                    c5_test_step(me);
                }
            }
            status = Q_HANDLED();
            break;
        }
        case RADIO_TEST_STEP_SIG: {
            if ( (uint8_t)TEST_IDLE == me->test_phase )
            {
                /* the row ended between the post and now */
            }
            else if ( me->busy )
            {
                KLOG_I(TAG, "test c5: waiting for the channel");
                me->test_pending = true;
            }
            else
            {
                c5_test_step(me);
            }
            status = Q_HANDLED();
            break;
        }
        case RADIO_FIELD_REQ_SIG: {
            radio_field_serve(Q_EVT_CAST(RadioFieldReqEvt));
            status = Q_HANDLED();
            break;
        }
        case RADIO_RECV_REQ_SIG: {
            status = inst_request(me, Q_EVT_CAST(AppRecvReqEvt));
            break;
        }
        case CONSOLE_RAW_RSP_SIG: {
            status = inst_armed(me, Q_EVT_CAST(ConsoleRawRspEvt));
            break;
        }
        case RECV_DATA_SIG: {
            status = inst_data(me, Q_EVT_CAST(RecvDataEvt));
            break;
        }
        case RADIO_RECV_IDLE_SIG: {
            status = inst_stall(me);
            break;
        }
        case RADIO_INSTALL_GO_SIG: {
            /* The verified image asks for the channel. Busy means owed, paid
             * by the settle; free means the start `install dst=c5` makes,
             * the card let go of before the C5 reboots. A row waiting its turn
             * is ended first: the update holds the channel for a minute and
             * reboots the chip the row was about to describe. */
            if ( (uint8_t)INST_READY != me->inst )
            {
                status = Q_HANDLED();   /* a stale ask: the install ended */
                break;
            }
            if ( me->busy )
            {
                KLOG_I(TAG, "install dst=c5: waiting for the channel");
                me->inst_pending = true;
                status = Q_HANDLED();
                break;
            }
            if ( (uint8_t)TEST_IDLE != me->test_phase )
            {
                c5_test_end(me, false, "a C5 firmware update started");
            }
            /* The activate reboots the C5 and the C5 switches the card's
             * supply: the card is let go of first, asked for WITH an answer
             * and waited for, and the session opens only when the card is
             * no longer mounted (radio_begin_after_unmount()). */
            if ( !radio_ask(me, STORAGE_OP_UNMOUNT, RADIO_ASK_BEGIN) )
            {
                c5_release(me);
                ota_fail(me, "the card could not be asked to let go; the C5 "
                             "was not touched");
                status = Q_HANDLED();
                break;
            }
            me->begin_wait = true;
            me->busy       = true;
            me->push_off   = 0U;
            me->written    = 0U;
            me->pct        = 0U;
            me->c5_open    = false;
            me->phase      = (uint8_t)OTA_BEGIN;
            me->inst       = (uint8_t)INST_PUSHING;
#if RADIO_ASKS
            scan_ask_refuse_owed(me);   /* never owed behind an update */
#endif
            status = Q_TRAN(&Radio_updating);
            break;
        }
        case RADIO_DOWN_REQ_SIG: {
            if ( me->busy || radio_lp_keeps_link(me) )
            {
                radio_say_busy("radio_down", RADIO_BUSY_NO);
                status = Q_HANDLED();
                break;
            }
            /* Whoever asked for the link by itself gives it back here.
             *
             * Unmount while esp-hosted still holds the controller (the
             * hosted unmount's deinit is a no-op), then end the
             * conversation. A refused job restores the card and stays up. */
            me->link_asked = false;
            KLOG_N(TAG, "taking the C5 link down");
            if ( radio_teardown_begin(me) )
            {
                status = Q_TRAN(&Radio_stopping);
            }
            else
            {
                status = Q_HANDLED();
            }
            break;
        }
        case RADIO_UNUSED_SIG: {
            /* radio_settle() saw nothing left that uses the link. Looked at
             * again here, because this was queued: a service switched back
             * on since then is a link that is wanted. The same teardown
             * `radio_down` makes; Radio_off's entry is what holds the C5. */
            if ( !radio_link_unused(me) )
            {
                status = Q_HANDLED();
                break;
            }
            KLOG_I(TAG, "nothing uses the C5 link any more; taking it down");
            if ( radio_teardown_begin(me) )
            {
                status = Q_TRAN(&Radio_stopping);
            }
            else
            {
                status = Q_HANDLED();
            }
            break;
        }
        case STORAGE_CMD_RSP_SIG: {
            /* An answer of AO_Storage: only a teardown's unmount means
             * anything here (the others belong to the states that asked). */
            if ( radio_asked(me) == RADIO_ASK_TEARDOWN )
            {
                radio_teardown_answered(me);
            }
            status = Q_HANDLED();
            break;
        }
        case RADIO_UNMOUNT_RETRY_SIG: {
            radio_teardown_retry(me);
            status = Q_HANDLED();
            break;
        }
        case RADIO_UP_REQ_SIG: {
            /* Already there - and from now on somebody has asked for the
             * link itself, so no service going off will take it down. */
            me->link_asked = true;
            radio_publish(me, KRADIO_UP);
            status = Q_HANDLED();
            break;
        }
        case RADIO_STATE_REQ_SIG: {
            radio_publish(me, (kradio_link_t)me->link);
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODEOS_WIFI
        case WIFI_STATE_REQ_SIG: {
            wifi_publish_state(me);
            status = Q_HANDLED();
            break;
        }
        case WIFI_START_REQ_SIG: {
            /* The flag is the desire, not the outcome, so the busy check
             * below guards only the action: a request that cannot act right
             * now still records that WiFi is wanted, and the next reconcile
             * or the next boot delivers it. */
            radio_flag_set(me, SVC_WIFI, true);
            if ( me->busy )
            {
                /* Refused now, owed from now - the mirror of `wifi_down`'s
                 * debt, and what makes an off-then-on tap land on. */
                radio_say_busy("wifi_up", RADIO_BUSY_OWED);
                radio_start_owed(me, SVC_WIFI);
                wifi_publish_state(me);
            }
            else
            {
                /* Already up: "on" means "get on a network", and the
                 * autojoin handler owns every rule about how (empty store,
                 * scan). */
                wifi_auto_self();
            }
            status = Q_HANDLED();
            break;
        }
        case WIFI_LOST_SIG: {
            /* Said at once: everything riding on the network has been told
             * nothing until now. Asleep too, because AO_Net would otherwise
             * keep asking for radio wakes on a network that is not there. */
            KLOG_N(TAG, "the network went away");
            me->retry_step = 0U;
            wifi_retry_arm(me);
            wifi_publish_state(me);
            status = Q_HANDLED();
            break;
        }
        case WIFI_RETRY_SIG: {
            me->retry_owed = true;
            (void)wifi_retry_pay(me);
            status = Q_HANDLED();
            break;
        }
        case WIFI_STOP_REQ_SIG: {
            radio_flag_set(me, SVC_WIFI, false);
            if ( me->busy )
            {
                /* Refused now, owed from now: radio_settle() carries it out
                 * the moment the job in flight is done, which is what stops
                 * a `wifi_down` typed during the boot autojoin from being
                 * overtaken by the join it was meant to prevent. */
                radio_say_busy("wifi_down", RADIO_BUSY_OWED);
                radio_stop_owed(me, SVC_WIFI);
                wifi_publish_state(me);
            }
            else if ( !radio_submit(me, KDRV_RADIO_JOB_WIFI_STOP) )
            {
                radio_stop_owed(me, SVC_WIFI);
                wifi_publish_state(me);
            }
            status = Q_HANDLED();
            break;
        }

        case WIFI_SCAN_REQ_SIG: {
            if ( me->busy )
            {
                radio_say_busy("wifi_scan", RADIO_BUSY_ASKED);
                wifi_publish_scan_busy(me);
            }
            else if ( !radio_submit(me, KDRV_RADIO_JOB_WIFI_SCAN) )
            {
                wifi_publish_scan_end(me, 0U, false);
            }
            else
            {
                scan_ask_refuse_owed(me);
                /* This one has a listener, so the sweep's knocks count. */
                me->scan_live = true;
                me->scan_sent = 0U;
                KLOG_I(TAG, "scanning; networks are listed as they are found");
            }
            status = Q_HANDLED();
            break;
        }
        case RADIO_SCAN_MORE_SIG: {
            /* A channel is in. The ring is emptied either way; whether that
             * becomes a listing is what `scan_live` decides, because the
             * production row's scan and the autojoin round's are judged from
             * the table and nobody is waiting for their entries. */
            wifi_drain_scan(me);
            status = Q_HANDLED();
            break;
        }
        case WIFI_JOIN_REQ_SIG: {
            WifiJoinEvt const * const j = Q_EVT_CAST(WifiJoinEvt);
            if ( me->busy )
            {
                if ( me->scan_live )
                {
                    /* The Wi-Fi page sweeps most of the time it is open,
                     * and a sweep is about 38 s: behind a whole one the
                     * join waited as long. Cut, it goes out when the step
                     * on the air ends, a few seconds at most; the sweep's
                     * end withdraws the cut (RADIO_OUT_WIFI_SCAN_DONE). */
                    kdrv_wifi_scan_abort(true);
                }
                radio_say_busy("wifi_join", RADIO_BUSY_OWED);
                wifi_owe(me, e);
                radio_flag_set(me, SVC_WIFI, true);
                wifi_publish_state(me);
            }
            else
            {
                kdrv_radio_req_t req;
                bool const by_vault = ('\0' == j->pass[0])
                                      && vault_knows(me, j->ssid);
                (void)memset(&req, 0, sizeof(req));
                wifi_owed_clear(me);
                (void)snprintf(req.ssid, sizeof(req.ssid), "%s", j->ssid);
                (void)snprintf(me->ssid, sizeof(me->ssid), "%s", j->ssid);
                me->autojoin  = false;
                me->wifi_left = false;
                if ( by_vault )
                {
                    /* A saved network: the C5 joins it from its vault, and
                     * this side only readies its netif and waits. */
                    req.type = KDRV_RADIO_JOB_WIFI_PREPARE;
                    me->pass[0]  = '\0';
                    me->remember = false;
                }
                else
                {
                    /* Kept so a success can be handed to the vault - the
                     * credentials are only worth keeping once they have
                     * actually worked. */
                    req.type = KDRV_RADIO_JOB_WIFI_JOIN;
                    (void)snprintf(req.pass, sizeof(req.pass), "%s", j->pass);
                    (void)snprintf(me->pass, sizeof(me->pass), "%s", j->pass);
                    me->remember = j->remember;
                }
                if ( !radio_submit_req(me, &req) )
                {
                    (void)memset(me->pass, 0, sizeof(me->pass));
                    wifi_publish_state(me);
                }
                else
                {
                    /* An accepted join is choosing WiFi, so it records the
                     * flag the same as `wifi_up` would - nobody asks for a
                     * network and means "but not after the next reboot". */
                    radio_flag_set(me, SVC_WIFI, true);
                    if ( by_vault )
                    {
                        KLOG_I(TAG, "joining '%s' from the C5's vault", j->ssid);
                    }
                    else
                    {
                        KLOG_I(TAG, "joining '%s'", j->ssid);
                    }
                }
                (void)memset(&req, 0, sizeof(req));
            }
            status = Q_HANDLED();
            break;
        }
        case WIFI_LEAVE_REQ_SIG: {
            wifi_owed_clear(me);
            if ( me->busy )
            {
                radio_say_busy("wifi_leave", RADIO_BUSY_NO);
                wifi_publish_state(me);
            }
            else
            {
                /* Leaving is not forgetting: `wifi_forget` is the verb for
                 * that, and conflating them meant you could not step off a
                 * network without losing it. And it is leaving: nothing goes
                 * back on one by itself until somebody asks. */
                me->wifi_left = true;
                wifi_retry_stop(me);
                if ( !radio_submit(me, KDRV_RADIO_JOB_WIFI_LEAVE) )
                {
                    wifi_publish_state(me);
                }
            }
            status = Q_HANDLED();
            break;
        }

        case WIFI_FAST_REQ_SIG: {
            /* The wish is kept whatever the channel is doing; the debt is
             * paid now if the worker is free, and by the settle if not. */
            me->fast_want = Q_EVT_CAST(WifiFastEvt)->on;
            me->ps_held   = false;
            (void)radio_fast_apply(me);
            status = Q_HANDLED();
            break;
        }

        case WIFI_RSSI_REQ_SIG: {
            if ( me->busy )
            {
                /* Quietly: the callers of this one are periodic, not typed
                 * (and "wifi_rssi" names no verb an operator could have
                 * typed anyway). The state event carries the last reading,
                 * which is what a refresh that lost the race deserves. */
                wifi_publish_state(me);
            }
            else if ( kdrv_wifi_state(NULL) != KDRV_WIFI_GOT_IP )
            {
                /* Nothing joined: nothing to measure, and no reason to wake
                 * a worker job just to learn what is already known. */
                wifi_publish_state(me);
            }
            else if ( !radio_submit(me, KDRV_RADIO_JOB_WIFI_RSSI) )
            {
                wifi_publish_state(me);
            }
            status = Q_HANDLED();
            break;
        }

        case WIFI_AUTO_REQ_SIG: {
            if ( me->busy )
            {
                radio_say_busy("wifi_auto", RADIO_BUSY_OWED);
                wifi_owe(me, e);
                radio_flag_set(me, SVC_WIFI, true);
                wifi_publish_state(me);
            }
            else if ( me->vlist_ok && (0U == me->vlist.count) )
            {
                /* Said once while the vault stays empty: every request for
                 * the autojoin finds the same fact. */
                if ( !me->nets_none_said )
                {
                    me->nets_none_said = true;
                    KLOG_I(TAG, "no networks remembered yet - `wifi_join` once "
                                "and this board will get back on it by itself");
                }
                wifi_retry_stop(me);
                wifi_publish_state(me);
            }
            else
            {
                me->nets_none_said = false;
                /* The hints first: one channel per network the Dot was last
                 * on, against a sweep of 35 s. A round with none, or none
                 * heard, sweeps. */
                me->hint_at = 0U;
                me->hint_on = auto_hint_next(me);
                if ( me->hint_on || radio_submit(me, KDRV_RADIO_JOB_WIFI_SCAN) )
                {
                    wifi_owed_clear(me);
                    scan_ask_refuse_owed(me);
                    me->tried    = 0UL;
                    me->autojoin = true;
                    /* Its end starts the next wait, if there is to be one. */
                    (void)QTimeEvt_disarm(&me->wifi_retry);
                    me->retry_owed = false;
                    me->wifi_left  = false;
                    me->retry_on   = me->wifi_want;
                    KLOG_I(TAG, "looking for a network we know");
                    status = Q_TRAN(&Radio_autojoin);
                    break;
                }
                wifi_retry_arm(me);
                wifi_publish_state(me);
            }
            status = Q_HANDLED();
            break;
        }
        case WIFI_LIST_REQ_SIG: {
            if ( radio_vault_send(me, KODE_VAULT_LIST, (uint8_t)VOP_LIST,
                                 NULL, 0U) )
            {
                me->vlist_publish = true;
            }
            else
            {
                /* The vault is busy with another request: what this side
                 * last heard, marked not fresh. */
                wifi_publish_saved(me, false);
            }
            status = Q_HANDLED();
            break;
        }
        case WIFI_FORGET_REQ_SIG: {
            WifiJoinEvt const * const f = Q_EVT_CAST(WifiJoinEvt);
            kode_vault_ssid_t v;
            (void)memset(&v, 0, sizeof(v));
            (void)snprintf(v.ssid, sizeof(v.ssid), "%s", f->ssid);
            if ( !radio_vault_send(me, KODE_VAULT_FORGET, (uint8_t)VOP_FORGET,
                                  &v, sizeof(v)) )
            {
                KLOG_W(TAG, "the C5's vault is busy; '%s' is not forgotten - "
                            "ask again", f->ssid);
            }
            status = Q_HANDLED();
            break;
        }
#endif /* CONFIG_KODEOS_WIFI */
#if RADIO_ASKS
        case RADIO_SCAN_ASK_SIG: {
            /* Started now if the channel is free; otherwise owed, and
             * radio_settle() starts it when the job holding it ends. */
            if ( scan_ask_take(me, e) )
            {
                (void)scan_ask_start(me);
            }
            status = Q_HANDLED();
            break;
        }
        case RADIO_SCAN_OWED_SIG: {
            /* Through the settle, so a switch or a sound owed meanwhile
             * goes first. */
            radio_settle(me);
            status = Q_HANDLED();
            break;
        }
#endif
        case RADIO_EXP_OWED_SIG: {
            /* The entry's kick: the switch the link-up did not carry, as
             * soon as the channel is free (radio_settle() pays it at the
             * next completion otherwise). */
            (void)radio_exp_apply(me);
            status = Q_HANDLED();
            break;
        }

#if CONFIG_KODEOS_BLE
        case BLE_START_REQ_SIG: {
            /* The flag first, and outside the busy check: it is the desire,
             * recorded even when this attempt is refused. */
            radio_flag_set(me, SVC_BLE, true);
            me->ble_fast_owed = true;
            if ( me->busy )
            {
                /* Refused, and answered anyway: the console's watchdog is
                 * disarmed for this command. The chain is neither advanced
                 * (the autojoin would meet an occupied channel) nor re-asked
                 * (it would spin on the busy job): radio_reconcile_retry()
                 * does it once the channel is free. */
                radio_say_busy("ble_up", RADIO_BUSY_OWED);
                radio_start_owed(me, SVC_BLE);
                ble_publish_state(me);
            }
            else if ( !radio_submit(me, KDRV_RADIO_JOB_BLE_START) )
            {
                /* Nothing will answer this one, so the debt keeps the start
                 * alive and the chain is told separately - ordering is moot
                 * once the job could not even be handed over. */
                radio_start_owed(me, SVC_BLE);
                ble_publish_state(me);
                radio_reconcile_ble_done(me);
            }
            else
            {
                /* the worker answers, and its outcome hands the baton on */
            }
            status = Q_HANDLED();
            break;
        }
        case BLE_SCAN_REQ_SIG: {
            if ( me->busy )
            {
                radio_say_busy("ble_scan", RADIO_BUSY_ASKED);
                ble_publish_scan_busy(me);
            }
            else
            {
                uint32_t ms = Q_EVT_CAST(BleScanReqEvt)->ms;
                if ( 0U == ms )
                {
                    ms = KODE_BLE_SCAN_MS;
                }
                if ( !ble_scan_ms_ok(ms) )
                {
                    KLOG_W(TAG, "a BLE scan of %lu ms is refused: %u..%u ms",
                           (unsigned long)ms, (unsigned)KODE_BLE_SCAN_MIN_MS,
                           (unsigned)KODE_BLE_SCAN_MAX_MS);
                    ble_publish_scan_end(me, false);
                }
                else
                {
                    kdrv_radio_req_t req;
                    (void)memset(&req, 0, sizeof(req));
                    req.type = KDRV_RADIO_JOB_BLE_SCAN;
                    req.len  = ms;
                    if ( !radio_submit_req(me, &req) )
                    {
                        ble_publish_scan_end(me, false);
                    }
                    else
                    {
                        scan_ask_refuse_owed(me);
                        me->ble_scan_live = true;
                        KLOG_I(TAG, "listening for BLE advertisers for %lu ms",
                               (unsigned long)ms);
                    }
                }
            }
            status = Q_HANDLED();
            break;
        }
        case BLE_STOP_REQ_SIG: {
            radio_flag_set(me, SVC_BLE, false);
            if ( me->busy )
            {
                /* Refused now, owed from now - the settle takes BLE down as
                 * soon as the channel is free. */
                radio_say_busy("ble_down", RADIO_BUSY_OWED);
                radio_stop_owed(me, SVC_BLE);
                ble_publish_state(me);
            }
            else if ( !radio_submit(me, KDRV_RADIO_JOB_BLE_QUIET) )
            {
                radio_stop_owed(me, SVC_BLE);
                ble_publish_state(me);
            }
            status = Q_HANDLED();
            break;
        }
        case BLE_PAIR_OPEN_REQ_SIG: {
            if ( !me->ble_want )
            {
                KLOG_W(TAG, "Bluetooth is off; `ble_up` first");
            }
            else
            {
                /* Visible now, and fast: whoever asked is about to look.
                 * An advert that hides the board comes off and the visible
                 * one follows (radio_ble_adv_reconcile()). */
                me->ble_pair_open  = true;
                me->ble_pair_until = khal_time_ms() + RADIO_BLE_PAIR_MS;
                me->ble_fast_owed  = true;
                me->ble_asked      = true;
                KLOG_N(TAG, "visible for %u s: a new device may pair",
                       (unsigned)RADIO_BLE_PAIR_MS / RADIO_MS_PER_S);
                (void)radio_ble_adv_reconcile(me);
            }
            ble_publish_state(me);
            status = Q_HANDLED();
            break;
        }
        case BLE_UNPAIR_REQ_SIG: {
            if ( kdrv_ble_console_attached() )
            {
                /* Clearing the store does not disconnect anybody, so the
                 * attached phone would keep a link whose key is gone.
                 * Disconnecting is what `ble_down` is for. */
                KLOG_W(TAG, "a console is attached; `ble_down` first");
                ble_publish_state(me);
            }
            else if ( me->busy )
            {
                radio_say_busy("ble_unpair", RADIO_BUSY_NO);
                ble_publish_state(me);
            }
            else if ( !radio_submit(me, KDRV_RADIO_JOB_BLE_UNPAIR) )
            {
                ble_publish_state(me);
            }
            else
            {
                /* the worker answers */
            }
            status = Q_HANDLED();
            break;
        }
        case BLE_STATE_REQ_SIG: {
            /* A question and nothing more. The invariant is restored where
             * it breaks, not where somebody asks about it. */
            ble_publish_state(me);
            status = Q_HANDLED();
            break;
        }
        case BLE_LINK_SIG: {
            /* A central arrived or left. Say so first - whoever is watching
             * the pill wants the colour now, not after a job - then put the
             * board back on the air if it has fallen off it. A refusal costs
             * nothing: the next completed job sweeps again. */
            radio_ble_link_note(me);
            ble_publish_state(me);
            (void)radio_ble_adv_reconcile(me);
            status = Q_HANDLED();
            break;
        }
        case BLE_APP_WRITE_SIG: {
            /* The host task's copy, published as it came: QP holds it for
             * every subscriber, and each one reads the same bytes. */
            QACTIVE_PUBLISH(e, &me->super);
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODEOS_BLE_PAIRING_PASSKEY
        case BLE_PAIR_TAKE_SIG: {
            ble_publish_pair(me);
            status = Q_HANDLED();
            break;
        }
        case BLE_PAIR_REJECT_REQ_SIG: {
            /* No advert sweep here. A refusal ends in a disconnect, and the
             * disconnect is already what restores the invariant. */
            (void)kdrv_ble_pair_reject();
            status = Q_HANDLED();
            break;
        }
#endif
#endif

        case FLASH_REQ_SIG: {
            FlashReqEvt const * const f = Q_EVT_CAST(FlashReqEvt);
            if ( (uint8_t)INST_IDLE != me->inst )
            {
                /* The staging buffer is the install's, whatever phase it is
                 * in. Log-only, as the busy refusal below: h_install answered
                 * the console on accept. */
                KLOG_E(TAG, "the coprocessor's staging buffer is taken: a "
                            "install dst=c5 is in progress");
                status = Q_HANDLED();
                break;
            }
            if ( me->busy )
            {
                radio_say_busy("install", RADIO_BUSY_NO);
                status = Q_HANDLED();
                break;
            }
            if ( (uint8_t)TEST_IDLE != me->test_phase )
            {
                /* Only a row waiting its turn can be here (a running one
                 * holds the channel and the busy check above refused).
                 * An update owns the channel for a minute or more and ends
                 * by rebooting the chip the row was about to describe. */
                c5_test_end(me, false, "a C5 firmware update started");
            }
            me->from_net = ((uint8_t)KFW_SRC_HTTP == f->src_kind);
            if ( !me->from_net
                && ((uint8_t)KFW_SRC_FS != f->src_kind)
                && ((uint8_t)KFW_SRC_SD != f->src_kind) )
            {
                /* No `pending` here, and none needed: the image is decoded
                 * whole into PSRAM and verified before the C5 is opened, so
                 * parking it first would only move the same bytes twice. */
                KLOG_E(TAG, "the coprocessor reads a file or a URL, nothing else");
                status = Q_HANDLED();
                break;
            }
            (void)snprintf(me->file, sizeof(me->file), "%.*s",
                           (int)(sizeof(me->file) - 1U), f->src);
            KLOG_N(TAG, "C5 update from '%s'", me->file);
#if CONFIG_KODEOS_NET
            if ( me->from_net )
            {
                net_stream_open_request(f->src, &me->super);
            }
            else
#endif
            {
                storage_open_request(me->file, &me->super);
            }
            /* The update's setup is the asker's, not the state's: the
             * install enters Radio_updating with its image already staged
             * and its phase at BEGIN, and an entry that reset both would
             * throw that image away. */
            me->phase       = (uint8_t)OTA_OPENING;
            me->total       = 0U;
            me->written     = 0U;
            me->pct         = 0U;
            me->first_chunk = true;
            c5_release(me);
#if RADIO_ASKS
            scan_ask_refuse_owed(me);   /* never owed behind an update */
#endif
            status = Q_TRAN(&Radio_updating);
            break;
        }

        case RADIO_JOB_SIG:
        case RADIO_JOB_GUARD_SIG:
        case RADIO_LINK_LOST_SIG: {
            if ( !radio_outcome(me, e) )
            {
                status = Q_HANDLED();
                break;
            }
            RadioOutcome const * const job = &me->out;
            status = Radio_up_outcome(me, job);
            break;
        }
        case RADIO_SVC_JOB_SIG: {
            radio_svc_lost(me, radio_svc_job_done(me, Q_EVT_CAST(KqpWorkerDoneEvt)));
            status = Q_HANDLED();
            break;
        }
        case RADIO_PARK_REQ_SIG: {
            radio_park_begin(me, Q_EVT_CAST(RadioParkEvt)->target);
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODE_LOWPOWER
        case LOWPOWER_ROUND_SIG: {
            radio_lp_round(me, Q_EVT_CAST(LowPowerRoundEvt));
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_STATE_SIG: {
            radio_lp_state(me, Q_EVT_CAST(LowPowerStateEvt));
            status = Q_HANDLED();
            break;
        }
        case RADIO_SLEEP_NOTE_SIG: {
            radio_lp_note(me, Q_EVT_CAST(RadioSleepNoteEvt));
            status = Q_HANDLED();
            break;
        }
        case RADIO_NET_LEFT_SIG: {
            radio_lp_net_left(me, Q_EVT_CAST(RadioNetLeftEvt));
            status = Q_HANDLED();
            break;
        }
#endif
        case KODE_SVC_RSP_SIG: {
            radio_svc_answer(me, (KodeSvcEvt const *)e);
            status = Q_HANDLED();
            break;
        }
        case KODE_SVC_TMO_SIG: {
            radio_svc_timeout(me, e);
            status = Q_HANDLED();
            break;
        }
        default: {
            if ( radio_exp_power(me, e) )
            {
                status = Q_HANDLED();
            }
            else if ( radio_amp(me, e) )
            {
                status = Q_HANDLED();
            }
#if CONFIG_KODEOS_BLE
            else if ( radio_ble_rsp(me, e) )
            {
                status = Q_HANDLED();
            }
            else if ( radio_ble_value(me, e) )
            {
                status = Q_HANDLED();
            }
#endif
#if defined(CONFIG_KODEOS_C5_HOLD)
            else if ( radio_c5_power(me, e) )
            {
                status = Q_HANDLED();
            }
#endif
            else if ( radio_config(me, e) )
            {
                status = Q_HANDLED();
            }
            else
            {
                status = Q_SUPER(&QHsm_top);
            }
            break;
        }
    }
    return status;
}

#if CONFIG_KODEOS_WIFI
/**
 * @brief The round is over, however it ended: say where WiFi stands, pay
 *        what was owed meanwhile, and hand the channel back.
 *
 * One exit for every way a round can end: a round is several jobs, and it
 * settles once, here, so a request refused as busy during the round (a
 * `wifi_down`, the production row) is paid when it ends.
 *
 * @param me The AO (its busy flag already cleared by the caller).
 * @return The QState the caller must return.
 */
static QState auto_finish(Radio * const me);

/**
 * @brief The next candidate of the round: ready this side for the C5's join
 *        of the strongest saved network still untried, or end the round.
 * @param me The AO (its busy flag already cleared by the caller).
 * @return The QState the caller must return.
 */
static QState auto_try(Radio * const me)
{
    if ( (uint8_t)WOWED_JOIN == me->wifi_owed )
    {
        return auto_finish(me);   /* somebody chose a network: it goes first */
    }
    if ( me->hint_on )
    {
        /* A hint that would not have us, or was not heard: the next, and when
         * they are spent the sweep the round would have begun with. */
        ++me->hint_at;
        if ( auto_hint_next(me) )
        {
            return Q_HANDLED();
        }
        me->hint_on = false;
        KLOG_I(TAG, "no network where it was; sweeping");
        if ( radio_submit(me, KDRV_RADIO_JOB_WIFI_SCAN) )
        {
            return Q_HANDLED();
        }
        return auto_finish(me);
    }
    if ( !auto_pick(me) )
    {
        KLOG_N(TAG, "none of the %u remembered networks in range would let "
                    "us on", (unsigned)me->vlist.count);
        return auto_finish(me);
    }
    kdrv_radio_req_t req;
    (void)memset(&req, 0, sizeof(req));
    req.type = KDRV_RADIO_JOB_WIFI_PREPARE;
    (void)snprintf(req.ssid, sizeof(req.ssid), "%s", me->ssid);
    if ( !radio_submit_req(me, &req) )
    {
        return auto_finish(me);
    }
    return Q_HANDLED();
}

static QState auto_finish(Radio * const me)
{
    me->hint_on = false;
    /* A round follows a round no sooner than the second wait. */
    if ( 0U == me->retry_step )
    {
        me->retry_step = 1U;
    }
    wifi_retry_arm(me);
    wifi_publish_state(me);
    radio_settle(me);
#if CONFIG_KODEOS_BLE
    /* The round's jobs complete here, not in Radio_up, so this is the sweep
     * every completed job owes: a central that left during the round found
     * this AO busy, and its BLE_LINK_SIG left the advert to the next job's
     * end. Without it the board stays off the air until the next reset. */
    (void)radio_ble_adv_reconcile(me);
#endif
    return Q_TRAN(&Radio_up);
}

/**
 * @brief WiFi was turned off during the round: stop it here and finish.
 *
 * A `wifi_down` typed inside a round is refused as busy and leaves only its
 * flag; carrying on would join a network for a service that is off. So each
 * outcome checks the flag first, and this is what it does instead.
 *
 * @param me The AO (its busy flag already cleared by the caller).
 * @return The QState the caller must return.
 */
static QState auto_abandon(Radio * const me)
{
    KLOG_I(TAG, "WiFi was turned off; the round stops here");
    /* radio_settle() stops the station - the same tidy-up every other
     * completed job gets. The link stays up regardless; it has to. */
    return auto_finish(me);
}

/**
 * @brief Getting onto a network we already know, without being asked twice.
 *
 * A child of Radio_up, like the update: it needs the link and owns the
 * channel while it runs. First the channels the last networks were on
 * (auto_hint_next()), then a sweep if none is heard: the strongest network
 * the store recognises is joined, and on a refusal the next one. A network
 * that refuses us is marked tried for this round only.
 */
static QState Radio_autojoin(Radio * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case RADIO_JOB_SIG:
        case RADIO_JOB_GUARD_SIG:
        case RADIO_LINK_LOST_SIG: {
            if ( !radio_outcome(me, e) )
            {
                status = Q_HANDLED();
                break;
            }
            RadioOutcome const * const job = &me->out;

            radio_relink_judge(me, job);
            switch ( (radio_out_t)job->evt )
            {
                case RADIO_OUT_WIFI_SCAN_DONE: {
                    me->busy = false;
                    if ( !me->wifi_want )
                    {
                        status = auto_abandon(me);
                        break;
                    }
                    if ( (uint8_t)WOWED_JOIN == me->wifi_owed )
                    {
                        /* Cut for the network somebody chose, or ended on its
                         * own: the join goes now, and the cut is withdrawn. */
                        kdrv_wifi_scan_abort(false);
                        status = auto_finish(me);
                        break;
                    }
                    if ( (uint8_t)KODE_DENIED == job->status )
                    {
                        /* Cut for the radio check: what it found is part of
                         * the air, so no network is judged absent, and the
                         * round is owed again once the check is done. */
                        KLOG_N(TAG, "the scan was cut for the radio check; "
                                    "the round is owed again");
                        radio_start_owed(me, SVC_WIFI);
                        status = auto_finish(me);
                        break;
                    }
                    if ( ((uint8_t)KODE_TIMEOUT == job->status)
                        && (0U < kdrv_wifi_scan_count()) )
                    {
                        /* A step of the sweep never answered, after others
                         * had: the networks heard are real, and joining one
                         * of them needs nothing from the part not swept. */
                        KLOG_N(TAG, "the scan stopped early; joining from the "
                                    "%u networks it heard",
                               (unsigned)kdrv_wifi_scan_count());
                    }
                    else if ( (uint8_t)KODE_OK != job->status )
                    {
                        KLOG_W(TAG, "the scan failed; nothing to join");
                        status = auto_finish(me);
                        break;
                    }
                    else
                    {
                        /* The sweep reached its end. */
                    }
                    if ( !me->vlist_ok )
                    {
                        KLOG_W(TAG, "the C5's vault never listed its networks; "
                                    "nothing to join");
                        status = auto_finish(me);
                        break;
                    }
                    status = auto_try(me);
                    break;
                }
                case RADIO_OUT_WIFI_PROBE_DONE: {
                    if ( !me->hint_on )
                    {
                        status = Radio_up_outcome(me, job);
                        break;
                    }
                    me->busy = false;
                    if ( !me->wifi_want )
                    {
                        status = auto_abandon(me);
                        break;
                    }
                    kdrv_radio_probe_result_t heard;

                    kdrv_radio_probe_result(&heard);
                    if ( ((uint8_t)WOWED_JOIN != me->wifi_owed)
                        && ((uint8_t)KODE_OK == job->status) && heard.net[0].seen )
                    {
                        kdrv_radio_req_t req;

                        (void)snprintf(me->ssid, sizeof(me->ssid), "%s",
                                       me->hints[me->hint_at].ssid);
                        KLOG_I(TAG, "trying '%s' (%ddBm) on channel %u, where it was",
                               me->ssid, (int)heard.net[0].rssi,
                               (unsigned)heard.net[0].channel);
                        (void)memset(&req, 0, sizeof(req));
                        req.type = KDRV_RADIO_JOB_WIFI_PREPARE;
                        (void)snprintf(req.ssid, sizeof(req.ssid), "%s", me->ssid);
                        if ( radio_submit_req(me, &req) )
                        {
                            status = Q_HANDLED();
                            break;
                        }
                    }
                    status = auto_try(me);
                    break;
                }
                case RADIO_OUT_WIFI_PREPARE_DONE: {
                    me->busy = false;
                    if ( !me->wifi_want )
                    {
                        status = auto_abandon(me);
                        break;
                    }
                    if ( ((uint8_t)WOWED_JOIN != me->wifi_owed)
                        && ((uint8_t)KODE_OK == job->status)
                        && radio_vault_join(me, me->ssid) )
                    {
                        me->busy = true;   /* the vault's answer is next */
                        status = Q_HANDLED();
                        break;
                    }
                    status = auto_try(me);
                    break;
                }
                case RADIO_OUT_WIFI_JOIN_DONE: {
                    me->busy = false;
                    me->fast_want = false;   /* a new association: the default */
                    if ( !me->wifi_want )
                    {
                        status = auto_abandon(me);
                        break;
                    }
                    if ( (uint8_t)KODE_OK == job->status )
                    {
                        uint32_t ip = 0U;
                        (void)kdrv_wifi_state(&ip);
                        KLOG_N(TAG, "joined '%s' as %u.%u.%u.%u", me->ssid,
                               (unsigned)(ip & 0xFFU),
                               (unsigned)((ip >> 8) & 0xFFU),
                               (unsigned)((ip >> 16) & 0xFFU),
                               (unsigned)((ip >> 24) & 0xFFU));
                        radio_hint_learn(me);
                        /* The vault moved it to the front of its list when
                         * it joined - which is where the next automatic
                         * round will look first. */
                        status = auto_finish(me);
                        break;
                    }

                    KLOG_W(TAG, "'%s' would not have us; trying the next one",
                           me->ssid);
                    status = auto_try(me);
                    break;
                }
                default: {
                    /* Anything else - a lost link, most of all - belongs to
                     * the parent, which knows how to tear things down. The
                     * outcome is read already: it is handed over as it is. */
                    status = Radio_up_outcome(me, job);
                    break;
                }
            }
            break;
        }
        case WIFI_SCAN_REQ_SIG: {
            /* Answered, not merely logged: `wifi_scan` closes on the
             * scan-end event with its watchdog disarmed, and this state
             * consumes its scan outcome without publishing a listing. */
            radio_say_busy("wifi_scan", RADIO_BUSY_ASKED);
            wifi_publish_scan_busy(me);
            status = Q_HANDLED();
            break;
        }
        case RADIO_SCAN_ASK_SIG: {
            /* The round is a sweep and a join: refused, never owed. */
            if ( !scan_ask_refuse_off(me, e) )
            {
                radio_say_busy("a scan", RADIO_BUSY_NO);
                scan_ask_refuse(e, KODE_NO_RESOURCES);
            }
            status = Q_HANDLED();
            break;
        }
        case RADIO_SCAN_OWED_SIG: {
            status = Q_HANDLED();   /* nothing is owed: the round refused it */
            break;
        }
        case RADIO_INSTALL_GO_SIG: {
            /* A round is several jobs and a state of its own; the push is
             * owed until auto_finish() settles, exactly as a row is. */
            if ( (uint8_t)INST_READY == me->inst )
            {
                KLOG_I(TAG, "install dst=c5: waiting for the autojoin round");
                me->inst_pending = true;
            }
            status = Q_HANDLED();
            break;
        }
        case RADIO_SVC_JOB_SIG: {
            /* A request that could not be sent ends the round's wait for the
             * vault the way a silence does; every other service's is the
             * parent's business, and the event was read here, once. */
            uint8_t const failed = radio_svc_job_done(me, Q_EVT_CAST(KqpWorkerDoneEvt));

            if ( (KODE_SVC_VAULT == failed) && ((uint8_t)VOP_JOIN == me->vault_op) )
            {
                me->vault_op = (uint8_t)VOP_NONE;
                me->busy     = false;
                status = auto_try(me);
            }
            else
            {
                radio_svc_lost(me, failed);
                status = Q_HANDLED();
            }
            break;
        }
        case KODE_SVC_RSP_SIG: {
            KodeSvcEvt const * const k = Q_EVT_CAST(KodeSvcEvt);
            if ( (KODE_SVC_VAULT != k->service)
                || ((uint8_t)VOP_JOIN != me->vault_op) )
            {
                status = Q_SUPER(&Radio_up);
                break;
            }
            /* The vault's answer to this round's join. */
            (void)QTimeEvt_disarm(&me->svc_tmo[KODE_SVC_VAULT]);
            me->svc_wait[KODE_SVC_VAULT] = false;
            me->vault_op = (uint8_t)VOP_NONE;
            me->busy     = false;
            if ( KODE_ST_OK == k->status )
            {
                kdrv_radio_req_t req;
                (void)memset(&req, 0, sizeof(req));
                req.type = KDRV_RADIO_JOB_WIFI_AWAIT;
                (void)snprintf(req.ssid, sizeof(req.ssid), "%s", me->ssid);
                if ( radio_submit_req(me, &req) )
                {
                    status = Q_HANDLED();
                    break;
                }
            }
            else
            {
                KLOG_W(TAG, "the C5's vault would not join '%s' (status %u)",
                       me->ssid, (unsigned)k->status);
            }
            status = auto_try(me);
            break;
        }
        case KODE_SVC_TMO_SIG: {
            if ( (e != &me->svc_tmo[KODE_SVC_VAULT].super)
                || ((uint8_t)VOP_JOIN != me->vault_op) )
            {
                status = Q_SUPER(&Radio_up);
                break;
            }
            me->svc_wait[KODE_SVC_VAULT] = false;
            kdrv_c5svc_abandon(KODE_SVC_VAULT);
            KLOG_W(TAG, "KODE service %u did not answer", (unsigned)KODE_SVC_VAULT);
            me->vault_op = (uint8_t)VOP_NONE;
            me->busy     = false;
            status = auto_try(me);
            break;
        }
        case WIFI_AUTO_REQ_SIG:
        case WIFI_JOIN_REQ_SIG: {
            radio_say_busy("the request", RADIO_BUSY_OWED);
            if ( ((QSignal)WIFI_AUTO_REQ_SIG == e->sig)
                && ((uint8_t)WOWED_JOIN == me->wifi_owed) )
            {
                /* The round is already what it would ask for; the network
                 * somebody chose is not given up for it. */
                status = Q_HANDLED();
                break;
            }
            wifi_owe(me, e);
            radio_flag_set(me, SVC_WIFI, true);
            if ( ((QSignal)WIFI_JOIN_REQ_SIG == e->sig) && me->winflight
                && (NULL != me->jobs)
                && (KDRV_RADIO_JOB_WIFI_SCAN == me->jobs->wreq.type) )
            {
                /* A round's sweep is the longest wait there is, and somebody
                 * is watching this join: it ends after the step on the air. */
                kdrv_wifi_scan_abort(true);
            }
            status = Q_HANDLED();
            break;
        }
        case WIFI_LEAVE_REQ_SIG: {
            wifi_owed_clear(me);
            radio_say_busy("the request", RADIO_BUSY_NO);
            status = Q_HANDLED();
            break;
        }
        case FLASH_REQ_SIG:
        case RADIO_DOWN_REQ_SIG: {
            radio_say_busy("the request", RADIO_BUSY_NO);
            status = Q_HANDLED();
            break;
        }
        default: {
            status = Q_SUPER(&Radio_up);
            break;
        }
    }
    return status;
}
#endif /* CONFIG_KODEOS_WIFI */

/**
 * @brief Ask the image's source for its next chunk: the network stream, or AO_Storage's file.
 * @param me The AO.
 */
static void c5_medium_read(Radio * const me)
{
#if CONFIG_KODEOS_NET
    if ( me->from_net )
    {
        net_stream_read_request();
        return;
    }
#endif
    storage_read_request();
}

/**
 * @brief Close the image's source: the network stream, or AO_Storage's file.
 * @param me The AO.
 */
static void c5_medium_close(Radio * const me)
{
#if CONFIG_KODEOS_NET
    if ( me->from_net )
    {
        net_stream_close_request();
        return;
    }
#endif
    storage_close_request();
}

/**
 * @brief The bytes of the chunk on loan from the image's source.
 *
 * Valid only until the chunk is handed back with c5_medium_return().
 *
 * @param me  The AO.
 * @param idx The buffer index the data event named.
 * @return The chunk's bytes.
 */
static const uint8_t *c5_medium_buf(Radio * const me, uint8_t idx)
{
#if CONFIG_KODEOS_NET
    if ( me->from_net )
    {
        return net_stream_buf(idx);
    }
#endif
    return storage_buf(idx);
}

/**
 * @brief Hand the chunk back to the image's source, so it can be refilled.
 * @param me  The AO.
 * @param idx The buffer index the data event named.
 */
static void c5_medium_return(Radio * const me, uint8_t idx)
{
#if CONFIG_KODEOS_NET
    if ( me->from_net )
    {
        net_stream_return(idx);
        return;
    }
#endif
    storage_next_request(idx);
}

/**
 * @brief Make room for the decoded image (kfw_hdr_cb_t).
 *
 * Called once the decoder knows how long the image really is - from the
 * container's header, or from the file's own size when it is a bare .bin -
 * and before a single byte is written anywhere.
 */
static kode_status_t c5_reserve(void *ctx, uint32_t img_len, const char *name)
{
    Radio * const me = (Radio *)ctx;
    (void)name;

    if ( (0U == img_len) || (C5_IMAGE_MAX < img_len) )
    {
        KLOG_E(TAG, "a %lu byte image is not one this board will send to the "
                    "coprocessor", (unsigned long)img_len);
        return KODE_ERROR;
    }
    me->kbuf.buf = khal_alloc_psram(64U, img_len);
    if ( NULL == me->kbuf.buf )
    {
        KLOG_E(TAG, "no PSRAM for a %lu byte image", (unsigned long)img_len);
        return KODE_NO_RESOURCES;
    }
    me->kbuf.cap = img_len;
    me->total    = img_len;
    return KODE_OK;
}

/** @brief Give the staging buffer back. Safe more than once. */
static void c5_release(Radio * const me)
{
    kfw_abort_owned(&me->kbuf);   /* AppMgr may hold the decoder */
    if ( NULL != me->kbuf.buf )
    {
        khal_free(me->kbuf.buf);
        me->kbuf.buf = NULL;
    }
    me->kbuf.cap  = 0U;
    me->kbuf.fill = 0U;
    me->push_off  = 0U;
}

/**
 * @brief Send the next slice of the decoded image, or close the session.
 * @return false when the worker refused; the caller fails the transfer.
 */
static bool c5_push_next(Radio * const me)
{
    if ( me->push_off >= me->kbuf.fill )
    {
        me->phase = (uint8_t)OTA_FINISH;
        return radio_submit(me, KDRV_RADIO_JOB_C5_FINISH);
    }
    uint32_t const left = me->kbuf.fill - me->push_off;
    uint32_t const n    = (C5_PUSH_CHUNK > left) ? left : C5_PUSH_CHUNK;

    kdrv_radio_req_t req;
    (void)memset(&req, 0, sizeof(req));
    req.type  = KDRV_RADIO_JOB_C5_WRITE;
    req.data  = &me->kbuf.buf[me->push_off];
    req.len   = n;
    me->buf_len = n;
    me->phase   = (uint8_t)OTA_WRITING;
    return radio_submit_req(me, &req);
}

/**
 * @brief Writing a new firmware into the C5, from an image decoded and verified in PSRAM.
 *
 * A child of Radio_up, because the link carries the writes. The last write
 * reboots the C5, so the state ends by tearing the link down, in Radio_off.
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState Radio_updating(Radio * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        /* No entry action: whoever transitions here has set the phase and
         * the counters - Radio_up's FLASH_REQ_SIG arm at OPENING with the
         * buffer released, RADIO_INSTALL_GO_SIG at BEGIN with the image
         * already staged in it. */
        case STORAGE_CMD_RSP_SIG: {
            switch ( radio_asked(me) )
            {
                case RADIO_ASK_BEGIN: {
                    status = radio_begin_answer(me);
                    break;
                }
                case RADIO_ASK_TEARDOWN: {
                    radio_teardown_answered(me);
                    status = Q_HANDLED();
                    break;
                }
                default: {
                    status = Q_HANDLED();
                    break;
                }
            }
            break;
        }
        case STORAGE_OPEN_RSP_SIG:
        case NET_STREAM_OPEN_RSP_SIG: {
            StorageOpenRspEvt const * const r = Q_EVT_CAST(StorageOpenRspEvt);
            if ( (uint8_t)OTA_OPENING != me->phase )
            {
                status = Q_HANDLED();   /* someone else's file */
            }
            else if ( (uint8_t)KODE_OK != r->status )
            {
                KLOG_E(TAG, "'%s' not found on any medium", me->file);
                me->phase = (uint8_t)OTA_IDLE;
                status = Q_TRAN(&Radio_up);
            }
            else
            {
                /* Decode first, send second. The C5's session is not opened
                 * until the whole image is in PSRAM and its digest has been
                 * checked, so a bad download never reaches its flash. */
                KLOG_I(TAG, "reading %luKB for the C5",
                       (unsigned long)(r->size / 1024U));
                if ( kfw_begin(kfw_buf_sink(), &me->kbuf, KFW_TARGET_C5,
                              r->size, c5_reserve, me) != KODE_OK )
                {
                    ota_fail(me, "another transfer is already decoding");
                    status = Q_TRAN(&Radio_up);
                }
                else
                {
                    me->phase = (uint8_t)OTA_DECODING;
                    c5_report(me, 0U);
                    c5_medium_read(me);
                    c5_medium_read(me);
                    status = Q_HANDLED();
                }
            }
            break;
        }
        case STORAGE_DATA_SIG:
        case NET_STREAM_DATA_SIG: {
            StorageDataEvt const * const d = Q_EVT_CAST(StorageDataEvt);

            /* Two buffers are primed at the start, so two reads are in
             * flight and both report eof at the end. Only the first one is
             * the end of anything; the second would call kfw_end() on a
             * session that has already been closed and read the refusal as a
             * verification failure - which is exactly what it did. */
            if ( (uint8_t)OTA_DECODING != me->phase )
            {
                status = Q_HANDLED();
                break;
            }
            if ( (uint8_t)KODE_OK != d->status )
            {
                ota_fail(me, "the medium stopped answering");
                status = Q_TRAN(&Radio_up);
                break;
            }
            if ( d->eof )
            {
                /* The card's power is switched by the C5, and activate
                 * reboots it - so let go of the medium before, not after. */
                c5_medium_close(me);
                if ( !radio_ask(me, STORAGE_OP_UNMOUNT, RADIO_ASK_BEGIN) )
                {
                    c5_release(me);
                    ota_fail(me, "the card could not be asked to let go; the "
                                 "C5 was not touched");
                    status = Q_TRAN(&Radio_up);
                    break;
                }

                if ( kfw_end() != KODE_OK )
                {
                    /* Nothing has reached the coprocessor yet: this is the
                     * whole point of decoding first. */
                    c5_release(me);
                    ota_fail(me, "the image did not verify; the C5 was not "
                                 "touched");
                    status = Q_TRAN(&Radio_up);
                    break;
                }
                if ( !kdrv_radio_image_header_ok(me->kbuf.buf, me->kbuf.fill) )
                {
                    c5_release(me);
                    ota_fail(me, "that is not an ESP firmware image");
                    status = Q_TRAN(&Radio_up);
                    break;
                }
                me->push_off   = 0U;
                me->written    = 0U;
                me->phase      = (uint8_t)OTA_BEGIN;
                me->begin_wait = true;
                me->busy       = true;
                status = Q_HANDLED();
                break;
            }

            const uint8_t *buf = c5_medium_buf(me, d->idx);
            kode_status_t const fst = kfw_feed(buf, d->len);
            c5_medium_return(me, d->idx);
            if ( KODE_OK != fst )
            {
                c5_release(me);
                ota_fail(me, "the image could not be decoded");
                status = Q_TRAN(&Radio_up);
            }
            else
            {
                me->first_chunk = false;
                c5_progress(me, me->kbuf.fill, me->total, 0U);
                status = Q_HANDLED();
            }
            break;
        }
        case RADIO_JOB_SIG:
        case RADIO_JOB_GUARD_SIG:
        case RADIO_LINK_LOST_SIG: {
            if ( !radio_outcome(me, e) )
            {
                status = Q_HANDLED();
                break;
            }
            RadioOutcome const * const job = &me->out;
            me->busy = false;
            status   = Q_HANDLED();

            switch ( (radio_out_t)job->evt )
            {
                case RADIO_OUT_C5_BEGIN_DONE: {
                    if ( (uint8_t)KODE_OK != job->status )
                    {
                        ota_fail(me, "the C5 refused to open an OTA session");
                        status = Q_TRAN(&Radio_up);
                    }
                    else
                    {
                        me->c5_open = true;
                        if ( !c5_push_next(me) )
                        {
                            ota_fail(me, "the worker refused the first chunk");
                            c5_release(me);
                            status = Q_TRAN(&Radio_up);
                        }
                    }
                    break;
                }
                case RADIO_OUT_C5_WRITE_DONE: {
                    if ( (uint8_t)KODE_OK != job->status )
                    {
                        ota_fail(me, "a chunk did not reach the C5");
                        status = Q_TRAN(&Radio_up);
                    }
                    else
                    {
                        me->written  += me->buf_len;
                        me->push_off += me->buf_len;
                        c5_progress(me, me->push_off, me->kbuf.fill, 50U);
                        if ( 0U != me->total )
                        {
                            uint8_t const pct =
                                (uint8_t)((me->written * 100U) / me->total);
                            if ( (uint8_t)(me->pct + 10U) <= pct )
                            {
                                me->pct = (uint8_t)(pct - (pct % 10U));
                                KLOG_N(TAG, "C5 update %u%%", (unsigned)me->pct);
                            }
                        }
                        if ( !c5_push_next(me) )
                        {
                            c5_release(me);
                            ota_fail(me, "the worker refused a chunk");
                            status = Q_TRAN(&Radio_up);
                        }
                    }
                    break;
                }
                case RADIO_OUT_C5_FINISH_DONE: {
                    if ( (uint8_t)KODE_OK != job->status )
                    {
                        ota_fail(me, "the C5 refused to activate the image");
                        status = Q_TRAN(&Radio_up);
                    }
                    else
                    {
                        /* The C5 is rebooting into the new image and the
                         * transport it was holding is already gone. Take the
                         * host side down for good: esp-hosted cannot be
                         * initialised twice in one boot, so there is no
                         * relink to wait for - the update ends as `radio_down`
                         * does, with the link off until the board resets. */
                        me->c5_open = false;
                        c5_release(me);
                        /* The new image raises IO4 at its own boot: what was
                         * believed of the pin is the old image's. */
                        me->amp_known = false;
                        me->phase = (uint8_t)OTA_TEARDOWN;
                        if ( (uint8_t)INST_PUSHING == me->inst )
                        {
                            /* Answered once the teardown is done, so the
                             * host's `>ok` finds the radio settled. */
                            me->inst = (uint8_t)INST_ACTIVATED;
                        }
                        (void)radio_submit(me, KDRV_RADIO_JOB_DISCONNECT);
                    }
                    break;
                }
                case RADIO_OUT_DISCONNECT_DONE: {
                    if ( (uint8_t)OTA_TEARDOWN == me->phase )
                    {
                        /* The same ending as Radio_stopping's: nothing failed,
                         * there is simply no link this boot, and the C5 runs
                         * its firmware unbothered - which is Radio_off. The
                         * card goes back to the plain path it takes whenever
                         * there is no link to carry it. */
                        me->phase = (uint8_t)OTA_IDLE;
                        c5_update_done(me);
                        radio_restore_plain_sd();
                        /* The C5 restarted into the new image: what IO5
                         * holds is that image's, not known here. */
                        me->exp_known = false;
                        status = Q_TRAN(&Radio_off);
                    }
                    break;
                }
                case RADIO_OUT_LINK_LOST: {
                    if ( (uint8_t)OTA_TEARDOWN == me->phase )
                    {
                        /* Expected: the C5 rebooted when we told it to. */
                        break;
                    }
                    ota_fail(me, "the link dropped mid-transfer");
                    radio_unmount_for_teardown(me);
                    radio_disconnect_after_loss(me);
                    status = Q_TRAN(&Radio_failed);
                    break;
                }
                default: {
                    break;
                }
            }
            break;
        }
        /* An update owns the channel until it ends, so refusals are an
         * explicit list, not a busy check: while downloading me->busy is
         * false, and anything left off reaches Radio_up and submits a job
         * mid-write. Each refusal publishes an answer, since those verbs
         * disarm the console's watchdog; FLASH_REQ_SIG and RADIO_DOWN_REQ_SIG
         * stay log-only, as their verbs answer the console at once. */
        case RADIO_PROBE_REQ_SIG: {
            radio_say_busy("the radio check", RADIO_BUSY_NO);
            probe_refuse(Q_EVT_CAST(RadioProbeReqEvt), KODE_NO_RESOURCES);
            status = Q_HANDLED();
            break;
        }
#if RADIO_ASKS
        case RADIO_SCAN_ASK_SIG: {
            /* An update holds the channel and may end with no link. */
            if ( !scan_ask_refuse_off(me, e) )
            {
                radio_say_busy("a scan", RADIO_BUSY_NO);
                scan_ask_refuse(e, KODE_NO_RESOURCES);
            }
            status = Q_HANDLED();
            break;
        }
#endif
#if CONFIG_KODEOS_WIFI
        case WIFI_SCAN_REQ_SIG: {
            radio_say_busy("wifi_scan", RADIO_BUSY_EMPTY);
            wifi_publish_scan_end(me, 0U, false);
            status = Q_HANDLED();
            break;
        }
        case WIFI_JOIN_REQ_SIG:
        case WIFI_LEAVE_REQ_SIG:
        case WIFI_AUTO_REQ_SIG:
        case WIFI_START_REQ_SIG:
        case WIFI_STOP_REQ_SIG: {
            if ( (QSignal)WIFI_LEAVE_REQ_SIG == e->sig )
            {
                wifi_owed_clear(me);
            }
            radio_say_busy("the request", RADIO_BUSY_NO);
            wifi_publish_state(me);
            status = Q_HANDLED();
            break;
        }
#endif /* CONFIG_KODEOS_WIFI */
#if CONFIG_KODEOS_BLE
        case BLE_START_REQ_SIG:
        case BLE_STOP_REQ_SIG:
        case BLE_PAIR_OPEN_REQ_SIG:
        case BLE_UNPAIR_REQ_SIG: {
            radio_say_busy("the request", RADIO_BUSY_NO);
            ble_publish_state(me);
            status = Q_HANDLED();
            break;
        }
        case BLE_SCAN_REQ_SIG: {
            /* Apart from the others because it closes on a different event:
             * `ble_scan` waits for the scan's end marker, exactly as
             * `wifi_scan` does above, and a state record would not close it.
             * Radio_down answers these two the same two ways. */
            radio_say_busy("ble_scan", RADIO_BUSY_EMPTY);
            ble_publish_scan_end(me, false);
            status = Q_HANDLED();
            break;
        }
#endif
        case FLASH_REQ_SIG:
        case RADIO_DOWN_REQ_SIG: {
            radio_say_busy("the request", RADIO_BUSY_NO);
            status = Q_HANDLED();
            break;
        }
        /* RADIO_RECV_REQ_SIG is not listed: it falls to Radio_up, where
         * inst_request() refuses it with an answer for as long as the phase
         * is not idle - which, in here, it never is. */
        case RADIO_TEST_REQ_SIG: {
            /* A verdict, not a busy line: the console waits on it with the
             * row's own budget, and an update can outlast that. */
            c5_test_end(me, false, "a C5 firmware update is in progress");
            status = Q_HANDLED();
            break;
        }
        case RADIO_TEST_STEP_SIG: {
            status = Q_HANDLED();   /* no row survives into an update */
            break;
        }
        case RADIO_PARK_REQ_SIG: {
            /* The update owns the C5 and reboots it when it ends: parking
             * it would be asking a chip mid-write for something else. */
            KLOG_N(TAG, "C5 was not parked (a C5 update is in progress): "
                        "the next firmware resets it");
            boot_radio_parked();
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODEOS_WIFI
        case WIFI_FAST_REQ_SIG: {
            /* Kept, not applied: the update owns the channel, and the C5
             * reboots into its new image when it ends. */
            me->fast_want = Q_EVT_CAST(WifiFastEvt)->on;
            status = Q_HANDLED();
            break;
        }
        case WIFI_RSSI_REQ_SIG: {
            /* The one request here that does not need the channel: the
             * update owns it, but the last reading is already in hand, so
             * this can be answered from the cache instead of joining the
             * refusal above - nothing is submitted to a worker mid-update. */
            wifi_publish_state(me);
            status = Q_HANDLED();
            break;
        }
#endif
        case RADIO_UNMOUNT_RETRY_SIG: {
            if ( ((uint8_t)RADIO_ASK_BEGIN == me->retry_kind) && me->begin_wait )
            {
                if ( radio_unmount_refire(me) )
                {
                    status = Q_HANDLED();
                }
                else
                {
                    status = radio_begin_answer(me);
                }
            }
            else
            {
                status = Q_SUPER(&Radio_up);
            }
            break;
        }
        default: {
            status = Q_SUPER(&Radio_up);
            break;
        }
    }
    return status;
}
