/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_ble.h
 * @brief BLE: a host on the P4, a controller on the C5, HCI in between.
 */
#ifndef KDRV_BLE_H_
#define KDRV_BLE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"
/* For the pairing block below, which exists only where there is a screen to
 * put a code on. kdrv_net.h carries this for the same reason. */
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief How many devices one scan may report - the event bus's cap. */
#define KDRV_BLE_SCAN_MAX   24U

/** @brief Most manufacturer-specific bytes one device keeps. An Apple Find
 *         My advert is 29 (company id included), so 30 holds it whole. */
#define KDRV_BLE_MFG_MAX      30U

/** @brief Most 16-bit service UUIDs one device keeps. */
#define KDRV_BLE_SVC_MAX      4U

/** @brief An advertising packet's payload, and a scan response's: 31 bytes. */
#define KDRV_BLE_ADV_MAX      31U

/** @brief Bytes of the company id that opens manufacturer data, as BLE defines it. */
#define KDRV_BLE_MFG_ID_LEN   2U

/** @brief kdrv_ble_dev_t.addr_type: a public address (or a public identity). */
#define KDRV_BLE_ADDR_PUBLIC  0U

/** @brief kdrv_ble_dev_t.addr_type: a random one, static or private. */
/* cppcheck-suppress misra-c2012-2.5 ; used by kdrv_ble.c, which sets it, and by the host cases that read it */
#define KDRV_BLE_ADDR_RANDOM  1U

/**
 * @brief One device seen while scanning.
 *
 * Past the name, what the advert says about the device, for an app that tells
 * kinds of device apart (a tracker is known by its maker data or its service,
 * not by a name). The advert and its scan response arrive as separate
 * packets: the first packet that carries a field fills it, as for the name.
 */
typedef struct {
    uint8_t  addr[6];    /**< Big-endian as printed, not NimBLE's order.   */
    int8_t   rssi;
    char     name[32];   /**< Empty when the advert carried no name.       */
    uint8_t  addr_type;  /**< KDRV_BLE_ADDR_PUBLIC or KDRV_BLE_ADDR_RANDOM. */
    uint8_t  mfg_len;    /**< Bytes in @c mfg; 0 when there was none.      */
    uint8_t  mfg[KDRV_BLE_MFG_MAX]; /**< Manufacturer data as on the air,
                                     *   the little-endian company id
                                     *   first; a longer one keeps its
                                     *   first KDRV_BLE_MFG_MAX bytes.   */
    uint8_t  svc_n;      /**< Entries in @c svc16.                         */
    uint16_t svc16[KDRV_BLE_SVC_MAX]; /**< The 16-bit service UUIDs the
                                       *   advert lists, then the one its
                                       *   16-bit service data is for.    */
} kdrv_ble_dev_t;

/** @brief What the host half is doing. */
typedef enum {
    KDRV_BLE_OFF = 0,      /**< The stack is not up.                       */
    KDRV_BLE_IDLE,         /**< Up, neither advertising nor connected.     */
    KDRV_BLE_ADVERTISING,
    KDRV_BLE_CONNECTED,
} kdrv_ble_state_t;

/**
 * @brief Told when a connection comes or goes.
 *
 * Called from NimBLE's host task. Post an event and return - the same
 * contract the console reader's byte callback has.
 */
typedef void (*kdrv_ble_cb_t)(kdrv_ble_state_t state);

/** @brief Install the callback. Before the first start; NULL is refused. */
kode_status_t kdrv_ble_set_cb(kdrv_ble_cb_t cb);

#if CONFIG_KODEOS_BLE_PAIRING_PASSKEY
/** @brief Where a pairing is in its life. */
typedef enum {
    KDRV_BLE_PAIR_NONE = 0,  /**< Nothing pending.                          */
    KDRV_BLE_PAIR_SHOW,      /**< A code is on offer; passkey is valid.     */
    KDRV_BLE_PAIR_DONE       /**< It ended; outcome says how.               */
} kdrv_ble_pair_phase_t;

/** @brief How a pairing ended. */
typedef enum {
    KDRV_BLE_PAIR_OK = 0,    /**< Bonded.                                   */
    KDRV_BLE_PAIR_GONE,      /**< The peer left with the pairing in flight. */
    KDRV_BLE_PAIR_FAILED,    /**< Encryption failed, or the SM gave up.     */
    KDRV_BLE_PAIR_REJECTED   /**< This board hung up on it.                 */
} kdrv_ble_pair_outcome_t;

/** @brief One moment in a pairing's life, as kdrv_ble_pair_take() reports it. */
typedef struct {
    kdrv_ble_pair_phase_t   phase;
    uint32_t                passkey;  /**< 0..999999. Valid in SHOW.        */
    kdrv_ble_pair_outcome_t outcome;  /**< Valid in DONE.                   */
} kdrv_ble_pair_t;

/**
 * @brief Told that a pairing wants a code shown, or has ended.
 *
 * Carries nothing on purpose: it runs on NimBLE's host task, so it posts and
 * returns, and the reader collects the record with kdrv_ble_pair_take() on
 * its own thread. The same contract kdrv_ble_cb_t has.
 */
typedef void (*kdrv_ble_pair_cb_t)(void);

/** @brief Install the pairing callback. Before the first start; NULL refused. */
kode_status_t kdrv_ble_set_pair_cb(kdrv_ble_pair_cb_t cb);

/**
 * @brief Collect what the last nudge was about, clearing it.
 *
 * @param out Filled when this answers true; untouched otherwise.
 * @return true when there was something pending.
 */
bool kdrv_ble_pair_take(kdrv_ble_pair_t *out);

/**
 * @brief Refuse the pairing in flight: hang up on the peer that asked.
 *
 * Records KDRV_BLE_PAIR_REJECTED and terminates the connection, which is
 * what kdrv_ble_quiet() already does from a thread that is not NimBLE's.
 *
 * @return KODE_OK when a pairing was there to refuse, KODE_DENIED otherwise.
 */
kode_status_t kdrv_ble_pair_reject(void);
#endif /* CONFIG_KODEOS_BLE_PAIRING_PASSKEY */

/**
 * @brief Bring the stack up: the C5's controller, then NimBLE. Idempotent.
 *
 * Needs the link. Blocks for the handshake, so the worker calls it.
 */
kode_status_t kdrv_ble_start(void);

/**
 * @brief Go quiet without going down: stop advertising, hang up on whoever
 *        is attached, and LEAVE the host running.
 *
 * This is what "switch Bluetooth off" means while the link stays up: the
 * service is what the owner switches, AO_Radio's stored flag is where that
 * lives, and a host left running is back on the air in one step. The host
 * itself goes down with the link, in kdrv_ble_stop(), and comes up again
 * with the next start.
 *
 * Idempotent, needs the link, and belongs to the kdrv_radio worker like
 * every other call here.
 */
kode_status_t kdrv_ble_quiet(void);

/**
 * @brief Take it down for real: stop the host, disable the controller.
 *
 * NOT what a switch calls - that is kdrv_ble_quiet(). This is for the paths
 * that take the link away (a link teardown, a coprocessor update): the
 * controller is the C5's, so a host with no link has nothing to talk to. The
 * next kdrv_ble_start() builds the host again.
 *
 * Idempotent, and needs the link for the controller half.
 */
kode_status_t kdrv_ble_stop(void);

/** @brief The fast advert's interval, 152.5 ms, in BLE's 0.625 ms units: a central
 *         that is looking has it within a few of these. On Apple's list of
 *         intervals for an accessory (QA1931), as the slow one is. */
#define KDRV_BLE_ITVL_FAST  244U
/** @brief The slow advert's interval, 1022.5 ms: what an advert that may stay up
 *         for minutes costs little at. */
#define KDRV_BLE_ITVL_SLOW  1636U

/** @brief One pulse's time enabled, ms: the first event leaves in a few ms; the station is deaf while enabled. */
/* cppcheck-suppress misra-c2012-2.5 ; used by kdrv_ble.c, which is a file this check does not join to this one, and by the host cases */
#define KDRV_BLE_PULSE_ON_MS         100U

/** @brief Restarts of a pulsed advert NimBLE may refuse in a row before the state goes to IDLE. */
#define KDRV_BLE_PULSE_REFUSALS_MAX  3U

/** @brief The shortest wait for the next pulse, ms: a pulse that ended late is followed at once. */
#define KDRV_BLE_PULSE_WAIT_MIN_MS   1U

/**
 * @brief Say how the next kdrv_ble_advertise(true) goes on the air.
 *
 * @param itvl   The interval, KDRV_BLE_ITVL_FAST or KDRV_BLE_ITVL_SLOW.
 * @param ms     How long it stays there, in ms; 0 for no end. When it runs
 *               out the advert stops and the state goes to IDLE.
 * @param hidden Hide the board: the advert stays connectable for the devices
 *               bonded to it, with no discoverable flag and no name, and no
 *               new device may pair until an advert is planned visible.
 * @param pulsed Send the advert in pulses: enabled for KDRV_BLE_PULSE_ON_MS
 *               at the start of each interval and disabled until the interval
 *               is over, so the air still sees one advertising event per
 *               interval. With the advert enabled, the C5's station on a 5 GHz
 *               channel does not fetch the frames its access point holds for
 *               it; with the advert enabled only for the event, it does.
 *               Meant for the slow interval. @p ms counts across the pulses.
 */
void kdrv_ble_adv_plan(uint16_t itvl, uint32_t ms, bool hidden, bool pulsed);

/** @brief What the driver does at the end of a pulse, or when a pause is over. */
typedef enum {
    KDRV_BLE_PULSE_DONE = 0,  /**< Nothing: a stop or a connection ended the train, and whoever did said so. */
    KDRV_BLE_PULSE_NEXT,      /**< Another pulse is owed, after the wait kdrv_ble_pulse_next() gives. */
    KDRV_BLE_PULSE_IDLE       /**< The train is over: the state goes to IDLE, once. */
} kdrv_ble_pulse_do_t;

/** @brief What kdrv_ble_pulse_next() judges, as the driver knows it. */
typedef struct {
    bool     live;          /**< No stop and no connection has ended the train.   */
    bool     timed_out;     /**< The pulse ended on its own time; false when NimBLE
                             *   or the controller ended it.                      */
    uint16_t itvl;          /**< The advert's interval, 0.625 ms units.           */
    uint32_t leg_ms;        /**< The plan's time on the air, ms; 0 for no end.    */
    uint32_t leg_run_ms;    /**< Since the first pulse began, ms.                 */
    uint32_t pulse_run_ms;  /**< Since the last pulse (or its try) began, ms.     */
    uint8_t  refused;       /**< Restarts NimBLE refused in a row.                */
} kdrv_ble_pulse_t;

/**
 * @brief The period of an advert of interval @p itvl, in ms: the time between
 *        the starts of two pulses of a pulsed one.
 * @param itvl The interval, 0.625 ms units.
 * @return The period, ms, rounded down.
 */
uint32_t kdrv_ble_pulse_period_ms(uint16_t itvl);

/**
 * @brief How long to wait for the next pulse after one that has run @p run_ms.
 * @param itvl   The interval, 0.625 ms units.
 * @param run_ms Since the pulse began, ms.
 * @return The period less @p run_ms, and at least KDRV_BLE_PULSE_WAIT_MIN_MS.
 */
uint32_t kdrv_ble_pulse_pause_ms(uint16_t itvl, uint32_t run_ms);

/**
 * @brief Judge what follows a pulse's end, or the end of its pause.
 *
 * A train that a stop or a connection ended is DONE. One that NimBLE or the
 * controller ended, that ran out its leg, or whose restarts were refused
 * KDRV_BLE_PULSE_REFUSALS_MAX times in a row is IDLE. Any other has a next
 * pulse after the wait: the rest of the period, or of the leg when that ends
 * sooner.
 *
 * @param in      What the driver knows.
 * @param wait_ms Receives the wait, ms, for KDRV_BLE_PULSE_NEXT; 0 otherwise.
 * @return What follows.
 */
kdrv_ble_pulse_do_t kdrv_ble_pulse_next(const kdrv_ble_pulse_t *in, uint32_t *wait_ms);

/**
 * @brief Whether the last link got encrypted: somebody this board knows, or who
 *        has just paired with it. Still true after that link ended, until the
 *        next one opens.
 *
 * @return true when it did.
 */
bool kdrv_ble_link_secure(void);

/**
 * @brief Start or stop advertising as this board.
 *
 * Connectable, and general-discoverable unless kdrv_ble_adv_plan() hid it, at
 * the interval and for the time that plan said: the advert stays on the air
 * until a central takes it, somebody stops it or its time runs out.
 *
 * A pulsed plan goes on the air as a train of pulses: the driver enables the
 * advert for KDRV_BLE_PULSE_ON_MS, disables it for the rest of the interval
 * and starts the next pulse by itself, from NimBLE's host task. The state is
 * ADVERTISING from the first pulse to the end of the train, and the plan's time
 * counts across the pulses. A stop, a connection, a lost link or a host reset
 * ends the train and nothing restarts it; a leg that runs out, or restarts
 * NimBLE keeps refusing, take the state to IDLE once.
 *
 * When the console service is built in, the advert carries its UUID so a
 * client can filter for Kode Dots and nothing else, and the device name
 * travels in the scan response - the two together do not fit in one 31-byte
 * advertising packet. The scan response is kdrv_ble_set_scan_rsp()'s.
 *
 * Starts the stack if it is not up. This is mechanism only: WHEN the board
 * should be on the air is AO_Radio's invariant, and the driver does not
 * re-arm itself after a disconnect.
 *
 * @param on true to go on the air, false to come off it.
 * @return KODE_OK, or KODE_DENIED while a central is already connected.
 */
kode_status_t kdrv_ble_advertise(bool on);

/**
 * @brief A fast advert on the air goes on at the slow interval; anything else
 *        is left as it is. A board that goes to rest must not pay for a fast one.
 *
 * @param ms     The time the slow one stays on the air, in ms; 0 for no end.
 * @param pulsed Whether the slow one is pulsed (kdrv_ble_adv_plan()).
 * @return KODE_OK, or what kdrv_ble_advertise() answered.
 */
kode_status_t kdrv_ble_adv_calm(uint32_t ms, bool pulsed);

/** @brief A connection's parameters, in the units BLE gives them. */
typedef struct {
    uint16_t itvl_min;   /**< The shortest interval, in 1.25 ms units. */
    uint16_t itvl_max;   /**< The longest interval, in 1.25 ms units.  */
    uint16_t latency;    /**< Connection events the board may skip.     */
    uint16_t timeout;    /**< The supervision timeout, in 10 ms units.  */
} kdrv_ble_conn_params_t;

/**
 * @brief The parameters this board asks a central for.
 *
 * Awake, 15 ms with no latency: the console's bytes move at the speed of the
 * link. At rest, 300-330 ms with a latency of 4 and a 6 s timeout: the C5's
 * radio wakes about three times a second instead of 66, and the board can
 * still answer at once, because the latency only lets it skip events. Both
 * sets are inside Apple's rules for an accessory (kdrv_ble_conn_params_ok()).
 *
 * @param rest true for the set of a resting board.
 * @param out  Receives the set; nothing happens when NULL.
 */
void kdrv_ble_conn_params(bool rest, kdrv_ble_conn_params_t *out);

/**
 * @brief Whether a set keeps Apple's rules for an accessory's connection.
 *
 * Interval min at least 15 ms; max at least min + 15 ms unless both are
 * 15 ms; latency at most 30; max x (latency + 1) at most 2 s; a timeout of
 * 2 to 6 s, longer than three times max x (latency + 1). A central outside
 * Apple's world takes any set that keeps these.
 *
 * @param p The set; NULL is not kept.
 * @return true when it keeps every rule.
 */
bool kdrv_ble_conn_params_ok(const kdrv_ble_conn_params_t *p);

/**
 * @brief Ask the connected central for the rest set (@p rest) or the awake one.
 *
 * Nothing happens with no central connected. The central decides: one that
 * declines keeps its interval, which is what the board had before.
 *
 * @param rest true for the rest set.
 * @return KODE_OK when asked or when there was nobody to ask; KODE_ERROR when
 *         NimBLE refused to send the request.
 */
kode_status_t kdrv_ble_conn_calm(bool rest);

/**
 * @brief Pack a name and manufacturer data the way a scan response carries them.
 *
 * The name first, as a complete local name (AD type 0x09), then the data as
 * manufacturer-specific data (AD type 0xFF); each AD structure is its
 * length, its type and its bytes. "Dot-7" and `FF FF 01 02` pack to
 * `06 09 'D' 'o' 't' '-' '7' 05 FF FF FF 01 02`, 13 bytes. The company id is
 * the data's first two bytes, as BLE defines it: nothing is added. What does
 * not fit is refused whole, never cut. Pure: no stack, no lock, any thread.
 *
 * @param name    The name; NULL or "" for none.
 * @param data    The manufacturer data, its company id first; NULL when @p len is 0.
 * @param len     Its bytes: 0 for none, else at least KDRV_BLE_MFG_ID_LEN.
 * @param out     Where the packet goes.
 * @param cap     Bytes @p out holds.
 * @param out_len Receives the packet's length.
 * @return KODE_OK; KODE_DENIED for a NULL @p out or @p out_len, NULL data with
 *         a length, or a length below KDRV_BLE_MFG_ID_LEN; KODE_NO_RESOURCES
 *         when the two do not fit @p cap or KDRV_BLE_ADV_MAX. On a refusal
 *         @p out and @p out_len are left as they were.
 */
kode_status_t kdrv_ble_adv_pack(const char *name, const uint8_t *data, size_t len,
                                uint8_t *out, size_t cap, size_t *out_len);

/**
 * @brief The name this board advertises when nobody names it: its serial.
 *
 * The canonical serial, not the printed one: an advert is a transport, not a
 * display. A board that does not know its serial answers "KODE-DOT". Any
 * thread.
 *
 * @return A string that stays valid for the boot.
 */
const char *kdrv_ble_serial_name(void);

/**
 * @brief Set what the scan response carries: a name and manufacturer data.
 *
 * The advert is untouched: with the console service it carries the
 * service's UUID and has 7 of its 31 bytes left, so whatever names the board
 * rides the scan response, which has a packet of its own, and is the Device
 * Name a connected central reads. Applied at once when the host is up, and
 * kept for every advert after, the host's restarts included. Until this is called, the scan response carries the serial alone
 * with the console service, and nothing without it.
 *
 * Belongs to the kdrv_radio worker, like every call here that talks to the
 * stack.
 *
 * @param name The name; NULL for the serial (kdrv_ble_serial_name()).
 * @param data The manufacturer data, its company id first; NULL when @p len is 0.
 * @param len  Its bytes.
 * @return KODE_OK; what kdrv_ble_adv_pack() refuses, with the scan response
 *         left as it was; KODE_ERROR when the host refused the packet.
 */
kode_status_t kdrv_ble_set_scan_rsp(const char *name, const uint8_t *data, size_t len);

/**
 * @brief The name the board goes by: the scan response's and its Device Name.
 *
 * The one kdrv_ble_set_scan_rsp() last set, and the serial until then. Written
 * by the kdrv_radio worker, so it is read while no job is with it.
 *
 * @return The name; never NULL.
 */
const char *kdrv_ble_name(void);

/**
 * @brief Scan for @p ms milliseconds. Blocks for about that long.
 *
 * Fills this driver's result table; read it with kdrv_ble_scan_count() and
 * kdrv_ble_scan_get() afterwards. A device that advertises repeatedly is
 * reported once, with the strongest signal seen.
 */
kode_status_t kdrv_ble_scan(uint32_t ms);

/**
 * @brief Listen for ONE advertiser by its local name and stop as soon as it
 *        is heard.
 *
 * The radio check's receive test: a beacon the station emits, found by the
 * name in its advert or scan response (its address rotates, so a MAC would
 * not do). Active, like kdrv_ble_scan(). Does not touch the device table, so
 * a crowded room cannot push the beacon out of it. Blocks: worker only.
 * Brings the BLE host up if it is not (kdrv_ble_start()) and leaves it up;
 * a scan takes an advert that was on the air off it, and putting it back is
 * the caller's.
 *
 * @param name   The name to look for; a prefix of the advertised one matches.
 * @param max_ms Longest listen.
 * @param[out] dev   The advertiser (address, rssi, name) when found.
 * @param[out] found Whether it was heard.
 * @return KODE_OK when the listen ran (@p found says what it heard);
 *         KODE_DENIED for a name that is empty or does not fit an advert;
 *         the stack's refusal otherwise.
 */
kode_status_t kdrv_ble_scan_find(const char *name, uint32_t max_ms,
                                 kdrv_ble_dev_t *dev, bool *found);

/** @brief How many devices the last scan kept. */
uint8_t kdrv_ble_scan_count(void);

/** @brief Read one result. @return false when @p idx is past the count. */
bool kdrv_ble_scan_get(uint8_t idx, kdrv_ble_dev_t *out);

/** @brief What the host half is doing right now. */
kdrv_ble_state_t kdrv_ble_state(void);

/**
 * @brief This board's own BLE address, as the controller reports it.
 * @return false before the stack has synchronised with the controller.
 */
bool kdrv_ble_addr(uint8_t out[6]);

/* ---- The app's own service ------------------------------------------------
 * One GATT service an app declares at file scope (KODE_BLE_SERVICE in kode.h,
 * KODE_SYS_BLE_SERVICE in kode_sys.h), which defines one constant,
 * kode_ble_app_service. kdrv_ble.c finds it through a weak reference each
 * time it builds the attribute table, at every start of the host, and serves
 * it after the console's service. Its characteristics take the console's
 * permissions, whatever those are in the build: with console pairing and
 * the passkey (both the default) every access needs an encrypted,
 * authenticated, bonded link, so only a phone bonded in kodeOS reaches it;
 * with Just Works pairing the link is encrypted and bonded but not
 * authenticated; with console pairing off it is as open as the console. It
 * rides beside the console's service, so a build without
 * CONFIG_KODEOS_BLE_CONSOLE leaves it out.
 *
 * A table that cannot be served (kdrv_ble_app_fault()) is left out whole,
 * with one log line at the build; the console's service stays. A value is
 * what a read gets; a notification goes into a ring that NimBLE's host task
 * drains. A characteristic with KDRV_BLE_APP_QUEUED has a queue instead:
 * each read takes the oldest value pushed, a read of an empty queue gets 0
 * bytes, and the take is told through the taken callback. The values, the
 * ring and every NimBLE structure of the service live in one PSRAM block,
 * taken when a service is first served or a value first set, so an app that
 * declares none pays no internal RAM for it; each queue is a PSRAM block of
 * its own, taken for a queued characteristic alone.
 *
 * AO_Radio owns the service the way it owns the radio: it alone calls
 * kdrv_ble_app_set_value(), kdrv_ble_app_notify(), kdrv_ble_app_queue_push(),
 * kdrv_ble_app_queue_clear() and kdrv_ble_app_kick(), and kode_sys asks it by
 * event. kdrv_ble_app_check() and kdrv_ble_app_queue_room() are the calls any
 * task may make, reads of the declared constant and of a queue's count. A
 * write arrives as a copy through the write callback, and a take through the
 * taken callback, both on NimBLE's host task.
 */

#define KDRV_BLE_APP_CHARS_MAX  4U    /**< Characteristics one app's service may have.          */
#define KDRV_BLE_APP_VALUE_DEFAULT 244U  /**< Bytes of a value by default: one notification at an MTU of 247. */
#ifdef CONFIG_KODE_BLE_APP_VALUE_MAX
#define KDRV_BLE_APP_VALUE_MAX  ((uint32_t)CONFIG_KODE_BLE_APP_VALUE_MAX) /**< Bytes of a value: the app's CONFIG_KODE_BLE_APP_VALUE_MAX. */
#else
#define KDRV_BLE_APP_VALUE_MAX  KDRV_BLE_APP_VALUE_DEFAULT                /**< Bytes of a value, with BLE off. */
#endif
#define KDRV_BLE_APP_NAME_MAX   16U   /**< Bytes of a characteristic's name, NUL included.      */
#define KDRV_BLE_APP_WHY_MAX    80U   /**< Bytes of the reason a service is left out, NUL included. */
#define KDRV_BLE_APP_READ       0x01U /**< A central may read the value.                       */
#define KDRV_BLE_APP_WRITE      0x02U /**< A central may write it.                             */
#define KDRV_BLE_APP_NOTIFY     0x04U /**< A central may subscribe to its notifications.       */
#define KDRV_BLE_APP_QUEUED     0x08U /**< Reads take the values pushed, oldest first; a read. */
#define KDRV_BLE_APP_QUEUE_SLOTS 8U   /**< Values one queued characteristic holds.             */

#define KDRV_BLE_APP_OP_SET     0U    /**< kdrv_ble_app_check(): set the value.               */
#define KDRV_BLE_APP_OP_NOTIFY  1U    /**< kdrv_ble_app_check(): set it and notify.           */
#define KDRV_BLE_APP_OP_PUSH    2U    /**< kdrv_ble_app_check(): push a value on the queue.   */
#define KDRV_BLE_APP_OP_CLEAR   3U    /**< kdrv_ble_app_check(): empty the queue.             */

/** @brief One characteristic of the app's service, as the app declares it. */
typedef struct {
    const char *name;   /**< 1 to KDRV_BLE_APP_NAME_MAX - 1 bytes; unique in the service. */
    const char *uuid;   /**< 128 bits as text, "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx".    */
    uint8_t     props;  /**< KDRV_BLE_APP_READ, _WRITE, _NOTIFY and _QUEUED, at least one. */
} kdrv_ble_app_char_t;

/** @brief The app's service, as the app declares it: kode_ble_app_service. */
typedef struct {
    const char                *uuid;   /**< 128 bits as text, as a characteristic's. */
    const kdrv_ble_app_char_t *chars;  /**< Its characteristics, in table order.     */
    size_t                     count;  /**< How many; 1 to KDRV_BLE_APP_CHARS_MAX.    */
} kdrv_ble_app_service_t;

/**
 * @brief The service the app serves: the one kdrv_ble_app_select() chose,
 *        else the one the app declared, found by name.
 * @return It; NULL when the app chose none, or declares none. Any thread.
 */
const kdrv_ble_app_service_t *kdrv_ble_app_service(void);

/**
 * @brief The service the app declared with KODE_BLE_SERVICE(), found by name.
 *
 * Defined by kdrv_ble.c (by the host suite's fake there). Only
 * kdrv_ble_app_service() reads it: everything else asks that.
 *
 * @return kode_ble_app_service; NULL when the app declares none. Any thread.
 */
const kdrv_ble_app_service_t *kdrv_ble_app_declared(void);

/**
 * @brief Choose the service the table is built from at the host's start, in
 *        place of the one declared; NULL serves none of the app's.
 *
 * A choice, not a table: it is read once, when kdrv_ble_start() registers
 * the GATT table. Once that table is registered the choice is fixed for the
 * boot, and this refuses and changes nothing (NimBLE's table is registered
 * once per boot). Any thread.
 *
 * @param svc The service, which must outlive the boot; NULL for none.
 * @return true when the choice is kept; false once the table is registered.
 */
bool kdrv_ble_app_select(const kdrv_ble_app_service_t *svc);

/**
 * @brief Whether the service @p svc is declared but cannot be served, and why.
 *
 * Judged by the rules the table build applies: the UUIDs, the count, the
 * names, the properties, and whether this build has the console's service
 * to ride beside. Pure: no stack, no lock, any thread.
 *
 * @param svc The service; NULL for none.
 * @param why Receives the reason as a sentence when this answers true; may
 *            be NULL.
 * @param cap Bytes @p why holds; KDRV_BLE_APP_WHY_MAX holds every reason.
 * @return true when @p svc is left out; false when it is served, or NULL.
 */
bool kdrv_ble_app_fault(const kdrv_ble_app_service_t *svc, char *why, size_t cap);

/**
 * @brief Whether a set, a notify, a push or a clear of characteristic @p chr
 *        would be refused before any value is touched.
 *
 * Judged against the declared constant alone: the service served, the name,
 * NOTIFY for a notify, QUEUED for a push or a clear, the length and the data.
 * Pure: no lock, no PSRAM, any thread.
 *
 * @param chr  The characteristic's name.
 * @param op   KDRV_BLE_APP_OP_SET, _NOTIFY, _PUSH or _CLEAR.
 * @param data The bytes; NULL when @p len is 0.
 * @param len  Their count: 1 or more for a push, 0 for a clear.
 * @return KODE_OK when the call @p op names would go on; KODE_DENIED as it
 *         refuses.
 */
kode_status_t kdrv_ble_app_check(const char *chr, uint8_t op, const void *data, size_t len);

/**
 * @brief Set what a read of characteristic @p chr gets.
 *
 * Copied under the lock NimBLE's host task takes for a read; it works before
 * the host has started. Nothing is sent. AO_Radio's alone.
 *
 * @param chr  The characteristic's name.
 * @param data The value; NULL when @p len is 0.
 * @param len  Its bytes, 0 to KDRV_BLE_APP_VALUE_MAX.
 * @return KODE_OK; KODE_DENIED when the app's service is not served, no
 *         characteristic has that name, @p len is too long or @p data is
 *         NULL with a length; KODE_ERROR when PSRAM refused the values.
 */
kode_status_t kdrv_ble_app_set_value(const char *chr, const void *data, size_t len);

/**
 * @brief Set characteristic @p chr's value and notify a subscribed central.
 *
 * The value is set as kdrv_ble_app_set_value() sets it. With a central
 * subscribed, the bytes go into the ring, and NimBLE's host task sends them
 * once kdrv_ble_app_kick() wakes it; one longer than the link's MTU less 3
 * is not sent, and the value stays for a read. AO_Radio's alone.
 *
 * @param chr  The characteristic's name; it carries KDRV_BLE_APP_NOTIFY.
 * @param data The bytes; NULL when @p len is 0.
 * @param len  Their count, 0 to KDRV_BLE_APP_VALUE_MAX.
 * @return KODE_OK, queued or nobody subscribed; KODE_DENIED as
 *         kdrv_ble_app_set_value(), or for a characteristic without
 *         KDRV_BLE_APP_NOTIFY; KODE_NO_RESOURCES when the ring is full: the
 *         value is set, the notification is not queued; KODE_ERROR when PSRAM
 *         refused the ring.
 */
kode_status_t kdrv_ble_app_notify(const char *chr, const void *data, size_t len);

/**
 * @brief Told about one write a central made to the app's service.
 *
 * Called from NimBLE's host task with a copy the callback may not keep. Post
 * an event and return, as kdrv_ble_cb_t does.
 *
 * @param chr  The characteristic's place in the table.
 * @param name Its name.
 * @param data The bytes written.
 * @param len  Their count, 0 to KDRV_BLE_APP_VALUE_MAX.
 * @return true when the write was taken; false makes the central's write
 *         fail with "insufficient resources", and the value stays as it was.
 */
typedef bool (*kdrv_ble_app_write_cb_t)(uint8_t chr, const char *name,
                                        const uint8_t *data, uint16_t len);

/** @brief Install the write callback. Before the first start; NULL is refused. */
kode_status_t kdrv_ble_app_set_write_cb(kdrv_ble_app_write_cb_t cb);

/**
 * @brief Push one value on characteristic @p chr's queue, behind the others.
 *
 * Copied under the lock NimBLE's host task takes for a read; it works before
 * the host has started, though a link that begins or ends empties the queue.
 * AO_Radio's alone.
 *
 * @param chr  The characteristic's name; it carries KDRV_BLE_APP_QUEUED.
 * @param data The bytes.
 * @param len  Their count, 1 to KDRV_BLE_APP_VALUE_MAX.
 * @return KODE_OK; KODE_DENIED as kdrv_ble_app_check() refuses a push;
 *         KODE_NO_RESOURCES when the queue holds KDRV_BLE_APP_QUEUE_SLOTS
 *         values; KODE_ERROR when PSRAM refused the queue.
 */
kode_status_t kdrv_ble_app_queue_push(const char *chr, const void *data, size_t len);

/**
 * @brief Empty characteristic @p chr's queue. AO_Radio's alone.
 * @param chr The characteristic's name; it carries KDRV_BLE_APP_QUEUED.
 * @return KODE_OK; KODE_DENIED as kdrv_ble_app_check() refuses a clear.
 */
kode_status_t kdrv_ble_app_queue_clear(const char *chr);

/**
 * @brief How many more values characteristic @p chr's queue takes now.
 *
 * A read of the count under the queue's lock: any task.
 *
 * @param chr  The characteristic's name; it carries KDRV_BLE_APP_QUEUED.
 * @param room Receives the count, 0 to KDRV_BLE_APP_QUEUE_SLOTS.
 * @return KODE_OK; KODE_DENIED as kdrv_ble_app_check() refuses a clear, or
 *         for a NULL @p room.
 */
kode_status_t kdrv_ble_app_queue_room(const char *chr, size_t *room);

/**
 * @brief Told that a read took the oldest value of a queued characteristic.
 *
 * Called from NimBLE's host task once the read reached the value's last
 * byte. Post an event and return, as kdrv_ble_cb_t does.
 *
 * @param chr  The characteristic's place in the table.
 * @param name Its name.
 * @param left Values still in its queue.
 */
typedef void (*kdrv_ble_app_taken_cb_t)(uint8_t chr, const char *name, uint8_t left);

/**
 * @brief Install the taken callback. Before the first start; NULL is refused.
 * @param cb The callback.
 * @return KODE_OK, or KODE_DENIED for a NULL @p cb.
 */
kode_status_t kdrv_ble_app_set_taken_cb(kdrv_ble_app_taken_cb_t cb);

/**
 * @brief Wake NimBLE's host task to send what the ring holds.
 *
 * Does nothing while the host is not running the app's service or the ring
 * is empty. AO_Radio's alone, and never while a job of its worker runs: a
 * start or a stop of the host there is what this would race.
 */
void kdrv_ble_app_kick(void);

/**
 * @brief Whether the app asked for the Battery service (KODE_BLE_BATTERY_SERVICE()).
 *
 * Served beside the console's and the app's services: the Bluetooth SIG's
 * Battery (0x180F) with Battery Level (0x2A19), read and notify, behind the
 * app's service's permissions. Any thread.
 *
 * @return true when the app declared it.
 */
bool kdrv_ble_battery_declared(void);

/**
 * @brief The charge Battery Level reports, in percent.
 *
 * A level that moved is notified to a subscribed central from NimBLE's host
 * task. AO_Radio's alone, from AO_Power's readings, and never while a job of
 * its worker runs, for kdrv_ble_app_kick()'s reason.
 *
 * @param pct 0 to 100.
 */
void kdrv_ble_battery_set(uint8_t pct);

/**
 * @brief How many connections a central has opened since boot.
 *
 * Counted on NimBLE's host task as each one opens, so an owner that reads
 * the state after a connection that came and went at once still sees that
 * there was one. Any thread.
 *
 * @return The count; it wraps.
 */
uint32_t kdrv_ble_links(void);

/* ---- The console service -------------------------------------------------
 * A GATT service carrying the same console text the USB-C cable carries: one
 * characteristic written to, one notified from. With
 * KODEOS_BLE_CONSOLE_PAIRING both refuse a peer that has not paired, so what
 * travels here is what travels on a cable somebody had to be holding.
 * Pairing is required, so a peer that has not read the passkey off the cable
 * gets no console at all - which is what lets the board advertise without a
 * window around it.
 *
 * AO_Log owns this the way it owns the cable's transport - it is a console
 * port that happens to be wireless, not a radio.
 */

/**
 * @brief Told about one complete line typed by the connected central.
 *
 * Called from NimBLE's host task with a NUL-terminated line, its terminator
 * already stripped. Post an event and return - the same contract the console
 * reader's byte callback has.
 */
typedef void (*kdrv_ble_console_rx_t)(const char *line);

/** @brief Install the line callback. NULL uninstalls. */
kode_status_t kdrv_ble_console_set_rx_cb(kdrv_ble_console_rx_t cb);

/**
 * @brief Told about raw bytes while the payload of a terminal install arrives.
 *
 * Called from NimBLE's host task, once per GATT write, with the bytes
 * exactly as they came. The line assembler is bypassed for the length of the
 * transfer - what follows the command is an image, not text.
 *
 * This is a PUSH where the cable's raw mode is a pull: there is no reader
 * task on this port to divert, so arrival drives it. The stop-and-wait of
 * the `>|ack` protocol is what keeps the host from outrunning the board,
 * exactly as it does on the cable.
 */
typedef void (*kdrv_ble_console_raw_t)(const uint8_t *data, uint16_t len);

/** @brief Install the raw callback. NULL uninstalls. */
kode_status_t kdrv_ble_console_set_raw_cb(kdrv_ble_console_raw_t cb);

/**
 * @brief Send what arrives to the raw callback instead of the line assembler.
 *
 * @param on true to divert, false to go back to lines.
 */
void kdrv_ble_console_raw(bool on);

/** @brief What the console half is doing, for `ble_info`. */
bool kdrv_ble_console_attached(void);

/**
 * @brief Bytes still queued for the central, 0 when nobody is attached.
 *
 * The link is orders of magnitude slower than the console can produce - a
 * connection interval carries a handful of notifications, while a listing
 * emitted at full speed is a thousand records a second. A producer of many
 * records asks this and waits, rather than filling the ring and losing the
 * tail. See kaos.h's console_congested().
 */
uint16_t kdrv_ble_console_backlog(void);

/** @brief Times the drain found NimBLE out of buffers and had to wait. */
uint16_t kdrv_ble_console_retry(void);

/** @brief Lines the ring has refused since the central attached. */
uint16_t kdrv_ble_console_tx_drop(void);

/** @brief How many peers the bond store holds. */
uint8_t kdrv_ble_bonds(void);

/**
 * @brief Forget every bonded peer.
 *
 * Does not disconnect anybody: a central still connected would keep a link
 * whose key no longer exists. Refusing this while one is attached is the
 * caller's job - see AO_Radio.
 */
kode_status_t kdrv_ble_unpair(void);

/**
 * @brief Put one finished console line on the wire, if anyone is listening.
 *
 * Called from AO_Log's thread, the way khal_console_write() is: AO_Log owns
 * the console transport, and this is that transport's second port.
 *
 * It does no BLE work itself. It copies the line into a ring and posts one
 * event to NimBLE's own queue; the notifications happen on NimBLE's host
 * task. That matters: ble_gatts_notify_custom() waits on the host mutex with
 * portMAX_DELAY and esp-hosted's SDIO transport waits on a 20-deep queue
 * under it, so a stalled C5 link would otherwise block AO_Log inside its own
 * step - and AO_Log is the only writer of BOTH consoles.
 *
 * @param buf One complete line, newline included. Not NUL-terminated.
 * @param len Its length in bytes.
 * @return KODE_OK queued; KODE_DENIED nobody is subscribed;
 *         KODE_NO_RESOURCES the ring is full and the line was dropped whole.
 */
kode_status_t kdrv_ble_console_write(const char *buf, size_t len);

/**
 * @brief The console ring, checked against itself: `test component=ble_console`.
 *
 * Exercises a ring of its own rather than the live one, so it can run with a
 * central attached and change nothing. Judges what the transport cannot: that
 * a line goes in whole or not at all, that order survives, and that a full
 * ring refuses rather than overwrites.
 *
 * @return KODE_OK when every case held, KODE_ERROR otherwise.
 */
kode_status_t kdrv_ble_console_selftest(void);

/* ---- The mirror characteristic --------------------------------------------
 * A fourth, notify-only characteristic in the same Kode service: the panel
 * stream, binary, on a pipe of its own. Console text and mirror frames must
 * never interleave on one wire - a base64 record and a length-prefixed JPEG
 * chunk are not distinguishable from each other by anyone reading the
 * stream - so this rides its own ring and its own drain, the same shape as
 * the console's, rather than sharing either.
 *
 * There is no base64 here. The console pays that 33% because it is a
 * line-based protocol and a stray control byte from klog's rescue path has
 * to stay recognisable as text; neither reason survives on a pipe that
 * carries nothing but frame bytes to a program that already knows their
 * shape. What goes out is the wire format kmirror_proto.h defines, raw:
 * header, then JPEG, chunked only by however many bytes fit one notification
 * - never re-framed, never re-encoded.
 *
 * The acknowledgement travels the other way over the console's existing RX
 * pipe as an ordinary `mirror_ack` command, not over this characteristic -
 * notify-only is the whole design, and it is what saves a second parser.
 */

/** @brief Whether a central is subscribed to the mirror characteristic. */
bool kdrv_ble_mirror_attached(void);

/**
 * @brief Queue @p len bytes of mirror wire data for the central, if anyone
 *        is listening.
 *
 * Called from AO_Display's thread - a different one from AO_Log's, which is
 * exactly why this cannot share the console's ring: two producers, one on
 * each side of a lock, would still serialise correctly, but a console line
 * landing between two halves of a frame would not, and there is no way for
 * a reader of raw bytes to notice that happened. It does no BLE work itself:
 * the bytes are copied into a ring of their own and one event is posted to
 * NimBLE's queue, exactly as kdrv_ble_console_write() does for a line.
 *
 * @param data Header bytes or JPEG bytes - the caller decides where the cut
 *             falls; this function does not parse the frame, it only moves
 *             bytes.
 * @param len  How many. Zero, or more than this ring can ever hold whole
 *             (0xFFFF), is refused.
 * @return KODE_OK queued; KODE_DENIED nobody is subscribed;
 *         KODE_NO_RESOURCES the ring is full and nothing was queued - the
 *         caller's own bytes are untouched and it is expected to retry the
 *         same @p data later rather than treat this as a drop, the same
 *         contract mirror_send_slice() keeps with a congested console.
 */
kode_status_t kdrv_ble_mirror_write(const uint8_t *data, size_t len);

/** @brief Forget everything locally, without talking to a link that is gone. */
void kdrv_ble_forget(void);

#if CONFIG_KODE_LOWPOWER
/**
 * @brief How many GAP events NimBLE has delivered since boot.
 *
 * A connect, its encryption and a disconnect each add one. The radio's sleep
 * sentinel watches it change to keep the board awake while the link moves.
 *
 * @return The count; it wraps.
 */
uint32_t kdrv_ble_activity(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* KDRV_BLE_H_ */
