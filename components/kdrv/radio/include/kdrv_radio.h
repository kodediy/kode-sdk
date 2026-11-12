/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_radio.h
 * @brief The ESP32-C5 radio coprocessor, behind the esp-hosted link.
 */
#ifndef KDRV_RADIO_H_
#define KDRV_RADIO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"
#include "sdkconfig.h"   /* CONFIG_KODE_LOWPOWER, for the sleep sentinel below */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What kdrv_radio_run() can be asked to do. One job at a time -
 *         that is the honest shape of a channel that is sequential on the
 *         far end, and AO_Radio's state machine enforces it. */
typedef enum {
    KDRV_RADIO_JOB_CONNECT = 0,  /**< Bring the link up: reset the C5, SDIO
                                  *   transport, wait for CP_INIT, read the
                                  *   coprocessor version, and drive J3's
                                  *   supply to the wish as it is then
                                  *   (kdrv_radio_exp_wish()).              */
    KDRV_RADIO_JOB_DISCONNECT,   /**< Tear the link down (esp-hosted deinit).
                                  *   The C5 keeps running - only the
                                  *   conversation ends.                    */
    KDRV_RADIO_JOB_WIFI_SCAN,    /**< Scan both bands. Fills kdrv_wifi's
                                  *   result table; AO_Radio publishes it.  */
    KDRV_RADIO_JOB_WIFI_JOIN,    /**< Join req.ssid with req.pass and wait
                                  *   for an address.                       */
    KDRV_RADIO_JOB_WIFI_LEAVE,   /**< Leave the network.                    */
    KDRV_RADIO_JOB_WIFI_STOP,    /**< Stop the station entirely - off, not
                                  *   merely disassociated.                 */
    KDRV_RADIO_JOB_WIFI_RSSI,    /**< Refresh kdrv_wifi's cached signal
                                  *   strength of the current association.  */
    KDRV_RADIO_JOB_WIFI_PREPARE, /**< Ready this side for a join the C5
                                  *   makes from its vault: station and
                                  *   netif up, the old network left, the
                                  *   radio held to the band of req.channel,
                                  *   the bits cleared
                                  *   (kdrv_wifi_prepare()).               */
    KDRV_RADIO_JOB_WIFI_AWAIT,   /**< Wait for that join's lease
                                  *   (kdrv_wifi_await()); ends as
                                  *   a join this side made does, with the
                                  *   status of the wait.                 */
    KDRV_RADIO_JOB_C5_BEGIN,     /**< Open an OTA session on the C5.        */
    KDRV_RADIO_JOB_C5_WRITE,     /**< Push req.data/req.len into it.        */
    KDRV_RADIO_JOB_C5_FINISH,    /**< Close it and activate: the C5 reboots
                                  *   into the slot just written.           */
    KDRV_RADIO_JOB_C5_ABORT,     /**< Close it without activating.          */
    KDRV_RADIO_JOB_C5_INFO,      /**< Take a fresh kdrv_c5 snapshot: chip,
                                  *   version, descriptor, MAC, heap, SDIO,
                                  *   the card pins. What the production
                                  *   row judges.                            */
    KDRV_RADIO_JOB_BLE_START,    /**< Bring the BLE stack up, no more. With
                                  *   req.on, first the scan response a
                                  *   BLE_RSP would set, from the same
                                  *   fields.                               */
    KDRV_RADIO_JOB_BLE_ADV,      /**< Start or stop advertising (req.on), as
                                  *   req.itvl, req.len, req.hidden and
                                  *   req.pulsed plan it. With no req.len an
                                  *   advert lasts until it is taken or stopped. */
    KDRV_RADIO_JOB_BLE_SCAN,     /**< Listen for advertisers for req.len ms. */
    KDRV_RADIO_JOB_BLE_QUIET,    /**< Stop advertising and hang up, leaving
                                  *   the host running. What `ble_down` uses;
                                  *   see kdrv_ble_quiet().                 */
    KDRV_RADIO_JOB_BLE_STOP,     /**< Take the BLE stack down. Only where the
                                  *   C5 goes with it.                      */
    KDRV_RADIO_JOB_BLE_UNPAIR,   /**< Clear the bond store.                 */
    KDRV_RADIO_JOB_WIFI_PS,      /**< Set the station's power save: req.on
                                  *   true is the default (modem save),
                                  *   false is none. See
                                  *   kdrv_wifi_power_save().               */
    KDRV_RADIO_JOB_EXP_POWER,    /**< Switch J3's +3.3V_EXT: req.on true is
                                  *   on. See kdrv_c5_exp_power().          */
    KDRV_RADIO_JOB_AMP,          /**< Drive the speaker amplifier's enable,
                                  *   the C5's IO4: req.on true is on. See
                                  *   kdrv_c5_amp().                        */
    KDRV_RADIO_JOB_WIFI_ADOPT,   /**< Take over the station a parked C5 still
                                  *   holds (kdrv_wifi_adopt()): netif and
                                  *   Wi-Fi up with no stop, no disconnect,
                                  *   then wait for the lease. Ends as a
                                  *   join does, with the adopt's status.   */
    KDRV_RADIO_JOB_BLE_RSP,      /**< Set the scan response
                                  *   (kdrv_ble_set_scan_rsp()): req.ssid the
                                  *   name, "" for the serial; req.data and
                                  *   req.len the manufacturer data.        */
    KDRV_RADIO_JOB_WIFI_PROBE,   /**< Listen on req.channel for req.dwell_ms
                                  *   and note the strongest reading of
                                  *   req.ssid and req.ssid2 (""= not
                                  *   wanted); read it with
                                  *   kdrv_radio_probe_result().            */
    KDRV_RADIO_JOB_BLE_FIND,     /**< Listen up to req.len ms for an advertiser
                                  *   whose name starts with req.ssid and stop
                                  *   at the first; read it with
                                  *   kdrv_radio_probe_result().            */
#if CONFIG_KODE_LOWPOWER
    KDRV_RADIO_JOB_SLEEP,        /**< Start (req.on) or stop the sleep
                                  *   sentinel. The owner's worker runs it
                                  *   through kdrv_radio_sleep(), because the
                                  *   wake note must reach the owner's own
                                  *   post; kdrv_radio_run() refuses it.    */
#endif
} kdrv_radio_job_t;

/** @brief Bytes of a network name in a job: 32 characters and the terminator. */
#define KDRV_RADIO_SSID_BYTES   33U

/** @brief Networks one WIFI_PROBE job looks for. */
#define KDRV_RADIO_PROBE_NETS   2U

/** @brief Bytes of a BLE device address in a result. */
#define KDRV_RADIO_ADDR_BYTES   6U

/** @brief What a WIFI_PROBE job heard of one wanted network. */
typedef struct {
    bool    seen;      /**< Heard at least once.        */
    int8_t  rssi;      /**< The strongest reading, dBm. */
    uint8_t channel;   /**< Its primary channel.        */
} kdrv_radio_heard_t;

/** @brief What the last WIFI_PROBE or BLE_FIND job heard. */
typedef struct {
    kdrv_radio_heard_t net[KDRV_RADIO_PROBE_NETS]; /**< WIFI_PROBE: req.ssid, req.ssid2, in that order. */
    bool    passive;                               /**< WIFI_PROBE: the channel was listened to passively. */
    bool    found;                                 /**< BLE_FIND: the name was heard.        */
    int8_t  ble_rssi;                              /**< BLE_FIND: its reading, dBm.          */
    uint8_t ble_addr[KDRV_RADIO_ADDR_BYTES];       /**< BLE_FIND: its address, as printed.   */
} kdrv_radio_probe_result_t;

/**
 * @brief One job, with whatever that job needs.
 *
 * @c data is a *borrowed* pointer and the one place this driver takes one:
 * an OTA chunk lives in AO_Storage's loan buffer, and the loan protocol
 * already guarantees it stays put until the borrower hands the index back -
 * which AO_Radio does only after this job's outcome arrives. The alternative
 * was copying 1.3 MB through the event bus a chunk at a time to say
 * something the loan already says.
 */
typedef struct {
    kdrv_radio_job_t type;
    char             ssid[KDRV_RADIO_SSID_BYTES]; /**< WIFI_PROBE: the first network wanted.
                              *   BLE_FIND: the name. BLE_RSP, BLE_START:
                              *   the board's name, "" for the serial.       */
    char             ssid2[KDRV_RADIO_SSID_BYTES]; /**< WIFI_PROBE: the second network, "" for none. */
    uint8_t          channel;  /**< WIFI_PROBE: the channel to listen on.
                              *   WIFI_PREPARE: the channel of the access
                              *   point the join is aimed at, 0 for none
                              *   (KDRV_WIFI_CHANNEL_ANY).                   */
    uint16_t         dwell_ms; /**< WIFI_PROBE: the time on it.                 */
    char             pass[65];
    const uint8_t   *data;   /**< C5_WRITE, BLE_RSP, BLE_START: borrowed until the
                              *   outcome.                                  */
    uint32_t         len;    /**< C5_WRITE, BLE_RSP, BLE_START: bytes. BLE_SCAN, BLE_FIND: ms.
                              *   BLE_ADV: the time on the air in ms, 0 for no end. */
    uint16_t         itvl;   /**< BLE_ADV: the interval (KDRV_BLE_ITVL_*); 0 for slow. */
    bool             hidden; /**< BLE_ADV: hide the board (kdrv_ble_adv_plan()). */
    bool             pulsed; /**< BLE_ADV: send the advert in pulses (kdrv_ble_adv_plan()). */
    bool             on;     /**< BLE_ADV: start or stop. WIFI_PS: modem save
                              *   (true) or none (false). EXP_POWER:
                              *   J3's supply on (true) or off.
                              *   AMP: the amplifier on (true) or off.
                              *   SLEEP: start the sentinel (true) or stop it.
                              *   BLE_START: set the scan response first. */
    bool             wants_wifi; /**< CONNECT: this boot wants Wi-Fi, so a C5
                              *   parked by the last firmware may be kept
                              *   running instead of reset.                  */
} kdrv_radio_req_t;

/** @brief The C5 slave firmware version, read over RPC at link-up.
 *
 * The slave answers with esp-hosted's own version (2.12.9 today) and
 * hardcodes everything finer to -1, so this RPC alone cannot tell a Kode
 * build of the slave from stock. What can is the image descriptor's project
 * name, KDRV_C5_PROJECT_NAME, which kdrv_c5 reads in the same bring-up. */
typedef struct {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
} kdrv_radio_fwver_t;

/**
 * @brief Run one job, and return when it has ended.
 *
 * Blocking: a job brings the link up, scans, joins or pushes a firmware
 * chunk and takes as long as the C5 takes. It is meant for the owner's
 * worker task, never for an Active Object's step, and one at a time - the
 * link, the C5's RPC channel and the result tables of kdrv_wifi, kdrv_ble
 * and kdrv_c5 are shared by every job, so the owner runs the next only
 * after the last has returned.
 *
 * A service job - a scan, probe, join, prepare, await, leave or power save,
 * J3's supply, the snapshot, and the BLE start, advert, scan, find, scan
 * response and unpair - that fails while the link reads up asks the C5 for its version
 * before it returns. An answer means the failure was the job's own and the status is
 * returned as it is; silence means the link is lost in everything but name,
 * and the notification of kdrv_radio_on_link_lost() fires with KODE_TIMEOUT
 * before the job returns its status.
 *
 * @param req The job. @c data of a C5_WRITE is borrowed until this returns.
 * @return What the job came to: KODE_OK, or the status of the step that
 *         failed. KODE_DENIED for a NULL @p req or a job this build does
 *         not have.
 */
kode_status_t kdrv_radio_run(const kdrv_radio_req_t *req);

/**
 * @brief What the last WIFI_PROBE or BLE_FIND job heard.
 *
 * Cleared when either job starts. It is the owner that reads it, after the
 * job's done event, while the worker is idle (the next job is submitted only
 * after the read), like the scan tables of kdrv_wifi.
 *
 * @param[out] out Receives a copy.
 */
void kdrv_radio_probe_result(kdrv_radio_probe_result_t *out);

/**
 * @brief What the notification is told: why the link is gone.
 * @param why KODE_ERROR when the transport dropped on its own, in answer to
 *            no job; KODE_TIMEOUT when a job failed and the C5 stopped
 *            answering.
 */
typedef void (*kdrv_radio_lost_cb_t)(kode_status_t why);

/**
 * @brief Install the notification that the link is gone.
 *
 * Called from the worker (a mute C5) or from esp-hosted's own event task (a
 * dropped transport): post an event and return, as the console reader's
 * on_byte does. A teardown the owner asked for does not call it. Call it
 * once, before the first job.
 *
 * @param cb Who to tell; NULL removes it.
 */
void kdrv_radio_on_link_lost(kdrv_radio_lost_cb_t cb);

/** @brief Whether the link is up (CP_INIT seen, no drop since). */
bool kdrv_radio_link_up(void);

/**
 * @brief Hold the C5 in reset, or let it go.
 *
 * C5_EN is the C5's enable pin, pulled up on the board, so a C5 nobody holds
 * starts by itself. Holding drives the pin low: the chip is off, and the
 * speaker amplifier its firmware enables is shut down with it. Letting go
 * drives the pin high, and the C5 boots.
 *
 * Only while esp-hosted is not running. It takes the pin when it starts - it
 * pulses it to reset the C5 - and until its DISCONNECT has finished the pin
 * is its own: a hold under a live link would drop it.
 *
 * Compiled in every build: without KODEOS_RADIO, kdrv_c5_hold.c drives the
 * pin alone, so the core can still hold the C5 off.
 *
 * @param hold true to hold the C5 in reset, false to let it run.
 * @return KODE_OK; KODE_DENIED between a CONNECT and the DISCONNECT that
 *         ends it;
 *         KODE_ERROR when the pin would not configure.
 */
kode_status_t kdrv_radio_c5_hold(bool hold);

/** @brief Whether kdrv_radio_c5_hold() is holding the C5 in reset now. */
bool kdrv_radio_c5_held(void);

/** @brief How long a join may take before the worker gives up on it, ms.
 *
 * Association plus DHCP. A wrong password answers much sooner than this (the stack
 * reports the refusal and the wait ends early). An autojoin round waits it
 * once for each remembered network that will not have the board. */
#define KDRV_RADIO_JOIN_MS      20000U

/** How long C5_EN is held low to reset the C5: esp-hosted's own pulse
 * (transport_gpio_reset). kdrv_radio.c pulses with it, and so does the core
 * for a C5 kept running for the amplifier or the app (kaos_park_c5()). */
#define KDRV_RADIO_C5_PULSE_MS  10U

/** How long the C5 is given to boot after its reset line is released:
 * esp-hosted's own wait (CONFIG_ESP_HOSTED_SDIO_RESET_DELAY_MS), asserted
 * against it in kdrv_radio.c. */
#define KDRV_RADIO_C5_BOOT_MS   1500U

/** How long the C5 waits after answering an activate before it restarts into
 * the new image: esp-hosted's slave arms a one-shot of this length
 * (RESTART_TIMEOUT, slave_control.c) and then calls esp_restart(). */
#define KDRV_RADIO_C5_ACTIVATE_MS 2000U

/**
 * @brief What a deliberate restart tells the next firmware about the C5.
 *
 * A jump names the role of the firmware it goes to, and a firmware keeps a
 * parked C5 only when the note names its own role: the only firmware that can
 * read a note meant for another is one built before the note existed, which
 * ignores it. An update names no role.
 */
typedef enum {
    KDRV_RADIO_NOTE_OS     = 1, /**< A jump home: kodeOS.                     */
    KDRV_RADIO_NOTE_APP    = 2, /**< A jump to an app.                        */
    KDRV_RADIO_NOTE_UPDATE = 3, /**< The C5 runs an update it has not
                                 *   confirmed: a closed C5 confirms it at the
                                 *   first KODE request, and its bootloader
                                 *   goes back to the old image if a reset
                                 *   comes first.                            */
} kdrv_radio_note_t;

/**
 * @brief Leave the handover note for the next firmware.
 *
 * Call it right before the restart: for a jump, only after the C5 has
 * answered the park with OK; for an update, once the C5 has restarted into
 * the new image. The note is one word in a register of the chip's
 * low-power domain: it survives esp_restart() and every reset but a power-on,
 * whichever firmware runs next. The next firmware's first link reads it and erases it,
 * and keeps the C5 running only when every condition of the rule holds;
 * otherwise it resets the C5 as it always did.
 *
 * @param target What the restart leaves the C5 doing.
 */
void kdrv_radio_note_leave(kdrv_radio_note_t target);

/**
 * @brief Whether this boot found an update note its first link has not read
 *        yet.
 *
 * Reads the note without erasing it, so a boot that wants neither Wi-Fi nor
 * BLE can still bring the link up once, and the new image is confirmed
 * instead of held in reset and given up.
 *
 * @return true for a KDRV_RADIO_NOTE_UPDATE note after a software reset.
 */
bool kdrv_radio_note_update_waits(void);

/**
 * @brief Make the lock that gives synchronous RPCs to the C5 their turns
 *        (kdrv_radio_rpc.c). Once is enough, more is harmless; called by the
 *        link's bring-up, before the first request.
 */
void kdrv_radio_rpc_init(void);

/** How long the gate holds the link's first words at most (kdrv_radio_gate()). */
/* cppcheck-suppress misra-c2012-2.5 ; kdrv_radio.c uses it; named here because kdrv_radio_gate()'s contract quotes it */
#define KDRV_RADIO_GATE_MS      1000U

/**
 * @brief Hold the link's first words on the SDIO bus back, or let them go.
 *
 * The card and the C5 share one SDMMC controller, and the interface's first
 * read of the card is what the first screen is made of. The C5's reset and its
 * boot are not held, only esp_hosted_connect_to_slave(), and for
 * KDRV_RADIO_GATE_MS at most.
 *
 * @param closed true holds it back.
 */
void kdrv_radio_gate(bool closed);

/**
 * @brief Whether the last CONNECT kept a parked C5 instead of resetting it.
 *
 * True after a CONNECT_DONE whose link found the C5 parked by the last
 * firmware; false otherwise. Every CONNECT clears it first.
 */
bool kdrv_radio_adopted(void);

/* ---- The sleep sentinel (CONFIG_KODE_LOWPOWER) ----------------------------
 * While the P4 light-sleeps, the esp-hosted link to the C5 stays alive: the
 * C5 is told the host is in power save, esp-hosted's SDIO tasks are
 * suspended, and the C5's wake line (KBSP_PIN_C5_WAKE_P4) or the P4's own
 * outgoing traffic ends the sleep. The sentinel is a worker in the sense of
 * design rule 9: it runs the handshake with the C5 and reports what
 * happened; it decides no state of the system.
 */

/* The kinds of a wake note. They are the values of kode_radio_wake_t in
 * kqp_signals.h (KODE_RADIO_WOKE_C5, _TX, _DEAF, _STOOD_DOWN), one for one: this header
 * may not include kqp, so a host case pins the two together. */
#define KDRV_RADIO_WOKE_C5    0U   /**< The C5 raised its line: traffic for the P4.   */
#define KDRV_RADIO_WOKE_TX    1U   /**< The P4 had a frame to send to the C5.         */
#define KDRV_RADIO_WOKE_DEAF  2U   /**< The C5 never answered the wake; the bus stays down. */
#define KDRV_RADIO_WOKE_STOOD_DOWN 3U   /**< The sentinel could not go back down: the bus is up and the
                                         *   sentinel holds the P4 awake until it is started or stopped. */

/** @brief What a wake note says. Counters are totals since boot. */
typedef struct {
    uint8_t  kind;        /**< KDRV_RADIO_WOKE_C5, _TX, _DEAF or _STOOD_DOWN.            */
    uint16_t knocks;      /**< Attempts the wake needed to make the C5 answer.            */
    uint32_t tx_sta;      /**< Station frames queued while the bus was down.              */
    uint32_t tx_serial;   /**< RPC (serial) frames queued while the bus was down.         */
    uint32_t tx_hci;      /**< BLE (HCI) frames queued while the bus was down.            */
} kdrv_radio_sleep_note_t;

/** @brief What the sentinel has done since boot. */
typedef struct {
    uint32_t wakes_c5;     /**< Wakes by the C5's line.                              */
    uint32_t wakes_tx;     /**< Wakes by the P4's own traffic.                       */
    uint32_t deaf;         /**< Wakes the C5 never answered.                         */
    uint32_t stood_down;   /**< Times the sentinel could not go back down.           */
    uint32_t knocks;       /**< Attempts spent on all wakes together.                */
    uint16_t knocks_max;   /**< The most attempts one wake needed.                   */
} kdrv_radio_sleep_stats_t;

/**
 * @brief Called in the sentinel's task after every wake, once the bus is back
 *        (or given up on), and once more with KDRV_RADIO_WOKE_STOOD_DOWN when
 *        the way back down failed. The note decides nothing; it must be short.
 * @param n   What happened.
 * @param arg What kdrv_radio_sleep() was given.
 */
typedef void (*kdrv_radio_sleep_note_cb_t)(const kdrv_radio_sleep_note_t *n, void *arg);

#if CONFIG_KODE_LOWPOWER

/**
 * @brief Start or stop the sentinel.
 *
 * true: tell the C5 the host is entering power save, wait, suspend
 * esp-hosted's SDIO tasks and arm the C5's wake line. From then on every wake
 * is served by the sentinel's own task: the C5 is knocked awake (retrying),
 * the bus is resumed only once it answered, a short awake window follows
 * (longer while BLE GAP events keep arriving), @p note is called, and the bus
 * goes down again. A C5 that never answers leaves the bus down and is
 * reported as KDRV_RADIO_WOKE_DEAF. If the bus cannot go down again (the C5
 * refuses power save, or the SDIO tasks never settle) the sentinel stands
 * down: the bus is left up, its power-management lock stays held so the P4 does
 * not light-sleep over a live bus, and KDRV_RADIO_WOKE_STOOD_DOWN is noted;
 * the next start or stop of the sentinel lets the lock go.
 *
 * false: serve the C5 one last time and leave the bus awake. @p note and
 * @p arg are ignored.
 *
 * Blocks until the sentinel's task has done it, so it belongs to a worker,
 * and one caller at a time. The sentinel holds a power-management lock of
 * its own while it works; the caller's lock is the caller's to let go.
 *
 * @param sleep true to start, false to stop.
 * @param note  Called after every wake; may be NULL.
 * @param arg   Passed through to @p note.
 * @return KODE_OK; KODE_NO_RESOURCES when the task, its semaphore or its
 *         lock could not be made; for a start that left the bus up (the
 *         caller must not let the P4 light-sleep): KODE_IO_ERROR when the C5
 *         did not take the announcement, KODE_DENIED when esp-hosted's SDIO
 *         tasks never settled, KODE_ERROR when the C5's wake line would not
 *         arm; for a stop, KODE_IO_ERROR when the C5 did not answer (the bus
 *         stays down and the sentinel stays armed); KODE_TIMEOUT when the
 *         task did not finish in time.
 */
kode_status_t kdrv_radio_sleep(bool sleep, kdrv_radio_sleep_note_cb_t note, void *arg);

/**
 * @brief The sentinel's counters.
 * @param out Receives them; zeros before the first start.
 */
void kdrv_radio_sleep_stats(kdrv_radio_sleep_stats_t *out);

#endif /* CONFIG_KODE_LOWPOWER */

/**
 * @brief Set the wish for J3's supply: what the next CONNECT drives the pin to.
 *
 * Written by AO_Radio alone, whenever its own wish changes, and read by the
 * CONNECT worker once, just before it drives the enable. The constructor's
 * write runs on the boot thread before the worker exists, so nothing reads it
 * then. It is one byte, so the read is atomic on this chip and needs no lock;
 * the worker drives what it reads and decides nothing. A wish set after that read is not
 * the one driven, and AO_Radio pays it with a job of its own.
 *
 * @param on true for the supply on.
 */
void kdrv_radio_exp_wish(bool on);

/**
 * @brief Whether the last CONNECT drove J3's supply to the wish it read.
 *
 * True after a CONNECT_DONE whose link-up switched the C5's IO5; false when
 * that step failed (the link works all the same) or the CONNECT failed. Every
 * CONNECT clears it first.
 */
bool kdrv_radio_exp_switched(void);

/**
 * @brief The level the last CONNECT drove J3's supply enable to.
 *
 * The wish it read, whether or not the drive went through. Meaningful only
 * after a CONNECT that succeeded; AO_Radio reads it once that CONNECT is done.
 *
 * @return true for the supply on.
 */
bool kdrv_radio_exp_level(void);

/**
 * @brief Called when the C5 sends peer data with the id that was listened for.
 *
 * Runs on esp-hosted's own thread, not an interrupt and not an Active Object:
 * copy what is needed, signal, return. @p data is only valid for the call.
 *
 * @param data The payload.
 * @param len  Its length.
 * @param ctx  What kdrv_radio_peer_listen() was given.
 */
typedef void (*kdrv_radio_peer_cb_t)(const uint8_t *data, size_t len, void *ctx);

/** @brief Called on the link's bring-up thread each time the link comes up. */
typedef void (*kdrv_radio_peer_up_t)(void *ctx);

/**
 * @brief Listen for peer data the C5 sends under @p msg_id.
 *
 * The link's owner is AO_Radio; this is the one seam through which another
 * driver may talk to a service of its own on the C5 - today the infrared
 * block, which is wired to that chip - without reaching for esp-hosted
 * itself. May be called before the link is up: the listener is kept and
 * handed to esp-hosted when the link comes up, and once per boot is enough.
 *
 * @param msg_id The id the C5 sends under.
 * @param cb     Who to tell.
 * @param up     Called every time the link comes up, which is every time the
 *               C5 has just been reset: whatever the listener believed about
 *               the far end's state is no longer true. May be NULL.
 * @param ctx    Handed back to @p cb and @p up untouched.
 * @return KODE_OK, KODE_DENIED for a NULL callback, KODE_NO_RESOURCES when
 *         every listener slot is taken.
 */
kode_status_t kdrv_radio_peer_listen(uint32_t msg_id, kdrv_radio_peer_cb_t cb,
                                     kdrv_radio_peer_up_t up, void *ctx);

/**
 * @brief Send peer data to the C5 under @p msg_id.
 *
 * Synchronous: it returns when the C5 has acknowledged the message, which is
 * one RPC round trip, and it waits its turn behind whatever RPC is in flight
 * - a scan's thirteen seconds included. It belongs on a worker task, never
 * on an Active Object's step.
 *
 * @param msg_id The id the C5 listens for.
 * @param data   The payload.
 * @param len    Its length, at least 1.
 * @return KODE_OK, KODE_DENIED for a bad argument or while the link is down,
 *         KODE_IO_ERROR when the C5 did not take it.
 */
kode_status_t kdrv_radio_peer_send(uint32_t msg_id, const uint8_t *data,
                                   size_t len);

/**
 * @brief Does @p head look like the start of an ESP firmware image?
 *
 * The cheap gate before 1.3 MB goes over the RPC one chunk at a time: a
 * file that is not an image at all should cost one chunk to find out, not a
 * whole transfer and a rebooted coprocessor. It is not a verification -
 * the C5's own bootloader does that - it is a typo check, and since the
 * console learned to stream images straight at the coprocessor it also
 * reads the header's chip id: a kodeOS or app image, built for the P4,
 * shares the file format and is refused here by name.
 */
bool kdrv_radio_image_header_ok(const uint8_t *head, uint32_t len);

/**
 * @brief The coprocessor version cached by the last successful CONNECT.
 * @return KODE_DENIED before any link-up has read it.
 */
kode_status_t kdrv_radio_fwversion(kdrv_radio_fwver_t *ver);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_RADIO_H_ */
