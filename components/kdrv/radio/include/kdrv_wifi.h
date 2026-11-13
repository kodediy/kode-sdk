/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_wifi.h
 * @brief WiFi, which on this board means the C5's radio over the RPC link.
 */
#ifndef KDRV_WIFI_H_
#define KDRV_WIFI_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief How many networks the results table keeps: the strongest ones.
 *
 * This bounds what is REMEMBERED, not what is reported. A listing goes out
 * through the hand-over ring below as the sweep finds it, so a room with
 * forty networks lists forty; the table is what autojoin and the production
 * row read afterwards, and for both of those the strongest twenty-four are
 * the whole question. It must also stay under 32, because AO_Radio's
 * autojoin holds one bit per slot in a uint32_t. */
#define KDRV_WIFI_SCAN_MAX   24U

/** @brief How many networks can wait to be handed over.
 *
 * The sweep produces a step's worth at a time - up to five channels of the
 * lower band or seven slots of the upper one - and the owner drains them on
 * the knock, so this only has to cover one busy step plus slack: forty on a
 * step is an ordinary city street. Overflowing costs networks off the
 * listing, which is why kdrv_wifi_scan_dropped() exists rather than the ring
 * quietly swallowing them. */
#define KDRV_WIFI_LIST_MAX   96U

/** @brief The whole sweep's budget, checked between steps.
 *
 * Measured at 20.6 s for the seven steps on this board (36.7 s at the 200-500
 * ms dwell it used to have) - the same as asking
 * the coprocessor for one sweep, since the dwell per channel is what costs
 * and the steps only add a round trip each. This is the ceiling on a sweep
 * that has stopped making progress, not a deadline anybody should meet. */
#define KDRV_WIFI_SCAN_TIMEOUT_MS  90000U

/** @brief How long one step gets to answer with its SCAN_DONE.
 *
 * A step is at most seven channels at the dwell (about 500 ms each, measured) plus
 * an RPC round trip, so twelve seconds is comfortably above what it costs
 * and still far inside esp-hosted's 30 s RPC timeout - which the wait never
 * meets anyway, since the start is asked for without blocking. */
#define KDRV_WIFI_SCAN_STEP_TIMEOUT_MS  12000U

/** @brief The lower band is swept in this many steps, the upper in
 *         KDRV_WIFI_SCAN_5G_STEPS - seven in all, however many channels the
 *         country opens.
 *
 * Why steps of several channels and not one channel per step: every answer
 * from the coprocessor is a DMA receive, and on the P4 each of those
 * invalidates both cores' L1 instruction caches by address
 * (cache_hal_invalidate_addr uses CACHE_TYPE_ALL for internal memory). At
 * one step per channel that ran forty times per sweep, and the tick ISR on
 * the other core twice fetched a line of zeros from intact IRAM - an
 * "Illegal instruction" with mtval 0, read from the halted chip over JTAG
 * on 2026-09-07. Seven steps keep the listing arriving as the air is heard
 * while cutting that traffic sixfold. */
#define KDRV_WIFI_SCAN_24_STEPS  3U

/** @brief The upper band's 28 bitmap slots, swept seven at a time. */
#define KDRV_WIFI_SCAN_5G_STEPS  4U

/** @brief The highest 5 GHz channel bit wifi_scan_channel_bitmap_t carries.
 *
 * Bit 0 of that field is the band bypass and bits 1 to 28 are the channels,
 * so a sweep of the upper band covers 1..28 and lets the stack drop whatever
 * the country does not open. */
#define KDRV_WIFI_SCAN_5G_BITS  28U

/** @brief A network with no password, as kdrv_wifi_ap_t's @c authmode says
 *         it (esp_wifi's WIFI_AUTH_OPEN). */
#define KDRV_WIFI_AUTH_OPEN  0U

/** @brief One network, as the C5 saw it. */
typedef struct {
    char    ssid[33];   /**< 32 bytes plus the terminator (802.11).        */
    int8_t  rssi;
    uint8_t channel;
    uint8_t authmode;   /**< KDRV_WIFI_AUTH_OPEN, or esp_wifi's own order. */
    uint8_t bssid[6];   /**< The access point's MAC, as on the air.        */
} kdrv_wifi_ap_t;

/** @brief Where a connection attempt got to. */
typedef enum {
    KDRV_WIFI_IDLE = 0,
    KDRV_WIFI_CONNECTING,
    KDRV_WIFI_GOT_IP,
    KDRV_WIFI_DISCONNECTED,
} kdrv_wifi_state_t;

/**
 * @brief Why the station last left a network or was refused one, in the three
 *        answers a person can act on. The stack's own codes stay in this
 *        driver: every other layer reads this.
 */
typedef enum {
    KDRV_WIFI_FAIL_NONE = 0,    /**< Nothing has gone wrong since the last attempt. */
    KDRV_WIFI_FAIL_PASSWORD,    /**< The network turned the password down.          */
    KDRV_WIFI_FAIL_NOT_FOUND,   /**< Not heard: out of range, or no such name.      */
    KDRV_WIFI_FAIL_OTHER,       /**< Anything else: full, incompatible, a fault.    */
} kdrv_wifi_fail_t;

/**
 * @brief Bring the remote WiFi stack up (idempotent).
 *
 * STA mode, started, no netif: enough to scan. Connecting adds the netif.
 */
kode_status_t kdrv_wifi_start(void);

/**
 * @brief Stop the remote stack. Idempotent.
 *
 * Talks to the C5, so it needs a live link: call it *before* the transport
 * goes away, never after.
 */
kode_status_t kdrv_wifi_stop(void);

/**
 * @brief Forget everything this driver believes, without saying a word.
 *
 * For when the link has gone or is going: there is nothing to negotiate with
 * any more, and the only thing left to get right is not lying about it. A
 * `wifi_info` that still reported an address after the radio was torn down
 * was the bug this exists to prevent.
 */
void kdrv_wifi_forget(void);

/**
 * @brief What a sweep calls when a step has networks waiting to hand over.
 *
 * Runs on the kdrv_radio worker, so it is under every rule this file states:
 * post an event and return. It carries no payload because a step can hold
 * forty networks and forty posts in one breath is how an owner's queue
 * overflows - one knock, and the owner drains the ring with
 * kdrv_wifi_scan_next() on its own thread.
 */
typedef void (*kdrv_wifi_progress_cb_t)(void);

/**
 * @brief Install the hook a sweep knocks on. Pass NULL to remove it.
 *
 * Set once, at bring-up, by whoever owns the radio. Without it a scan is
 * silent until it returns, which is correct but makes the caller wait for
 * the last channel to hear about the first.
 *
 * @param cb The hook, or NULL.
 */
void kdrv_wifi_scan_progress(kdrv_wifi_progress_cb_t cb);

/**
 * @brief Scan every channel, both bands (the C5 is dual-band). Blocks.
 *
 * In seven steps of a few channels each, every step its own non-blocking
 * start and its own wait on the stack's SCAN_DONE, and the results of a step
 * land in the ring before the next one is asked for. Two things follow from
 * that. The caller hears about a network a few seconds after the radio did
 * instead of at the end of the sweep - that is what the progress hook is
 * for. And no single request to the coprocessor is long, which is what
 * keeps the sweep clear of esp-hosted's 30 s RPC timeout: asking for the
 * blocking form put the whole sweep under it, and a timed-out RPC leaves the
 * channel expecting an answer that arrives later, into somebody else's
 * request. Why a few channels per step rather than one is under
 * KDRV_WIFI_SCAN_24_STEPS.
 *
 * Both halves go through the channel bitmap. The 2.4 GHz half splits the
 * channels the slave's country opens into KDRV_WIFI_SCAN_24_STEPS groups;
 * the 5 GHz half covers the bitmap's 28 slots seven at a time and lets the
 * stack ignore the ones the country closes. Every step is swept whatever the
 * table already holds: cutting the sweep short at a full table would mean a
 * crowded low band hid the upper one entirely, which on a busy street is
 * every scan.
 *
 * Two places take the results, and they are not the same place. Everything
 * found goes into the hand-over ring for kdrv_wifi_scan_next(), in the order
 * it was heard. The table behind kdrv_wifi_scan_count() and
 * kdrv_wifi_scan_get() keeps the strongest KDRV_WIFI_SCAN_MAX of them, which
 * is what a caller reads once the sweep is over.
 *
 * @return KODE_OK when the sweep ran to its end; KODE_TIMEOUT when a step
 *         or the sweep's budget ran out, with whatever was found kept;
 *         KODE_DENIED when kdrv_wifi_scan_abort() cut it short, with whatever
 *         it had found kept: it did not reach its end, so a caller must not
 *         conclude from it that a network is absent.
 */
kode_status_t kdrv_wifi_scan(void);

/**
 * @brief Ask the sweep that is running (if any) to end before its next step,
 *        or take the request back.
 *
 * For whoever cannot wait a sweep out: the radio check, which needs the C5's
 * channel, a join somebody is watching, and `wifi_scan_stop`. It only sets a
 * flag: the sweep reads it between its steps, so the step on the air finishes
 * first, and the sweep then returns KODE_DENIED (cut) with what it had found.
 * While the flag is set every sweep that
 * starts ends before its first step, so whoever set it clears it (@p on
 * false) once the sweep it was for has ended. AO_Radio only.
 *
 * @param on true to ask for the stop, false to take the request back.
 */
void kdrv_wifi_scan_abort(bool on);

/** @brief Beacon interval of an access point, ms: 100 TU of 1.024 ms,
 *         rounded up. */
#define KDRV_WIFI_BEACON_TU_MS     103U

/** @brief Shortest dwell on a radar channel, ms: a beacon interval and a half
 *         of margin, so one beacon is certain to fall inside it. */
#define KDRV_WIFI_RADAR_DWELL_MS   150U

_Static_assert(KDRV_WIFI_RADAR_DWELL_MS > KDRV_WIFI_BEACON_TU_MS,
               "a radar dwell shorter than a beacon interval hears nothing");

/** @brief Characters of a network name (802.11). */
#define KDRV_WIFI_SSID_LEN         32U

/** @brief One network the probe looks for, and what it heard. */
typedef struct {
    const char *ssid;     /**< In: the name to look for (exact match).       */
    int8_t      rssi;     /**< Out: the strongest reading, dBm.              */
    uint8_t     channel;  /**< Out: its primary channel.                     */
    bool        seen;     /**< Out: heard at least once.                     */
} kdrv_wifi_probe_t;

/**
 * @brief Listen on ONE channel and note the strongest reading of each wanted
 *        network. Does not touch the sweep's tables or ring.
 *
 * Active (a probe request) where transmitting is free; passive on the radar
 * channels (5 GHz 52-64 and 100-144), where the radio may not transmit before
 * it has heard the channel and a beacon is all it can wait for. The dwell
 * there must outlast a beacon interval, so it is raised to
 * KDRV_WIFI_RADAR_DWELL_MS when the caller asks for less. Blocks: worker only.
 * Starts the station if it is not started (kdrv_wifi_start()) and leaves it
 * started, with no netif and no association.
 *
 * @param channel    The channel number (1..14, 36..177).
 * @param dwell_ms   Time on the channel, active or passive.
 * @param want       The networks; the readings are merged into them, so one
 *                   array can be passed over many channels.
 * @param n          How many entries.
 * @param[out] passive Whether the channel was listened to passively; may be NULL.
 * @return KODE_OK when the channel was heard; KODE_DENIED when the stack
 *         refused it (a channel the country closes); KODE_TIMEOUT when the
 *         C5 never reported the end; KODE_ERROR when it would not say what
 *         it heard.
 */
kode_status_t kdrv_wifi_probe(uint8_t channel, uint16_t dwell_ms,
                              kdrv_wifi_probe_t *want, uint8_t n,
                              bool *passive);

/** @brief How many of the strongest networks the last scan kept. */
uint8_t kdrv_wifi_scan_count(void);

/** @brief Read one result. @return false when @p idx is past the count. */
bool kdrv_wifi_scan_get(uint8_t idx, kdrv_wifi_ap_t *out);

/**
 * @brief Take the next network the sweep found and has not handed over yet.
 *
 * The listing's half of the results, in the order the air gave them and with
 * no cap of its own. Drained by the hook's owner on the owner's own thread,
 * one knock at a time; draining it even when nobody is listening is what
 * keeps a scan run for autojoin from reporting drops it was never going to
 * avoid.
 *
 * @param out Receives the network.
 * @return false when there is nothing waiting.
 */
bool kdrv_wifi_scan_next(kdrv_wifi_ap_t *out);

/**
 * @brief How many networks the last sweep could not hand over.
 *
 * Non-zero means the ring filled because nobody drained it fast enough, and
 * that many networks are missing from the listing. Said out loud rather than
 * swallowed: a listing that is quietly short is a listing that lies.
 */
uint16_t kdrv_wifi_scan_dropped(void);

/**
 * @brief Join @p ssid and wait for an address. Blocks up to @p timeout_ms.
 *
 * @return KODE_OK once DHCP has answered; KODE_TIMEOUT if the association
 *         or the lease did not arrive in time; KODE_ERROR if the stack
 *         refused the attempt outright.
 */
kode_status_t kdrv_wifi_connect(const char *ssid, const char *pass,
                                uint32_t timeout_ms);

/** @brief The last channel of the lower band: 1 to 14 are 2.4 GHz and every
 *         channel above is 5 GHz. */
#define KDRV_WIFI_LAST_24_CHANNEL  14U

/** @brief The channel of a join whose access point is not known: the radio
 *         may use both bands. */
#define KDRV_WIFI_CHANNEL_ANY      0U

/**
 * @brief Ready this side for a join somebody else makes: the C5, from its
 *        vault. Blocks briefly; the worker's.
 *
 * Starts the station and its netif if needed (so DHCP runs here when the C5
 * associates), steps off any current network and clears what the last join
 * left. The state stays IDLE: the join may still be refused, and then
 * nothing has to be undone. kdrv_wifi_await() then waits for the outcome.
 *
 * The C5 joins the first access point of the name its own scan finds, which
 * is the 2.4 GHz one when the network is on both bands. So the radio is held
 * to the band of the access point that was chosen: the lower band for a
 * channel up to KDRV_WIFI_LAST_24_CHANNEL, the upper band above it, both
 * bands for KDRV_WIFI_CHANNEL_ANY. kdrv_wifi_await() gives both back, and so
 * does every scan. A C5 that refuses the band does not fail the join: it
 * goes on as it would without the aim, and the first refusal of a boot is
 * logged.
 *
 * @param ssid    The network the join is for.
 * @param channel The channel of the access point the join is aimed at, or
 *                KDRV_WIFI_CHANNEL_ANY when it is not known.
 * @return KODE_OK; KODE_ERROR for no name or a stack that would not start.
 */
kode_status_t kdrv_wifi_prepare(const char *ssid, uint8_t channel);

/**
 * @brief Wait for the join kdrv_wifi_prepare() readied. Blocks up to
 *        @p timeout_ms; the worker's.
 *
 * The state is CONNECTING with @p ssid named from here, GOT_IP on success,
 * and back to IDLE (or DISCONNECTED, as the network said) on a failure.
 * Whatever the outcome, the radio is given both bands back before this
 * returns.
 *
 * @param ssid       The network, named in the state and in the log.
 * @param timeout_ms How long the lease may take.
 * @return KODE_OK once DHCP has answered; KODE_DENIED if the network turned
 *         us away; KODE_TIMEOUT if nothing came in time.
 */
kode_status_t kdrv_wifi_await(const char *ssid, uint32_t timeout_ms);

/**
 * @brief Take over a station the C5 already holds, without dropping it.
 *        Blocks up to @p timeout_ms; the worker's.
 *
 * For a C5 that was parked across a deliberate P4 reset and is still
 * associated. From a stack that is not yet started (the boot after the
 * reset) the station netif is created before the Wi-Fi starts, so the stack
 * is never stopped and the association survives; esp_wifi_connect() then
 * makes the C5 replay its association, and this netif's DHCP gets the
 * lease. No disconnect comes first. If the stack is already started and has
 * no netif, the netif is made the join way, with a stop and a start around
 * it, which a held association does not survive (a warning says so).
 * Afterwards the state, the address, the SSID and the signal read exactly
 * as after a join.
 *
 * @param timeout_ms How long the lease may take.
 * @return KODE_OK once DHCP has answered; KODE_DENIED if the C5 reported a
 *         disconnect; KODE_TIMEOUT if nothing came in time; KODE_ERROR if the
 *         stack or its netif would not start.
 */
kode_status_t kdrv_wifi_adopt(uint32_t timeout_ms);

/** @brief Leave the network. Idempotent. */
kode_status_t kdrv_wifi_disconnect(void);

/** @brief Where the connection stands, and the address if there is one. */
kdrv_wifi_state_t kdrv_wifi_state(uint32_t *ip4);

/**
 * @brief Copy the SSID joined or being joined, whole, from any task or core.
 * @param out Where the name goes, always NUL-terminated; "" for none.
 * @param cap Bytes of @p out; 0 writes nothing. A name longer than it is cut.
 */
void kdrv_wifi_ssid_copy(char *out, size_t cap);

/**
 * @brief Signal strength of the current association. Crosses the RPC link.
 *
 * A short question to the remote stack, not a scan - but still a call over
 * the link: the kdrv_radio worker is the only caller, never an AO
 * step. What it learns is cached; kdrv_wifi_rssi_last() is how the rest of
 * the system reads it back.
 * @return dBm (negative) while associated; 0 in any other state.
 */
int8_t kdrv_wifi_rssi(void);

/** @brief The hook of kdrv_wifi_on_lost(): carries nothing, the driver's state is the news. */
typedef void (*kdrv_wifi_lost_cb_t)(void);

/**
 * @brief Be told when the network in use goes away by itself.
 *
 * A router restarting, the access point out of range, an AP that expels the
 * station: an address was held and nobody here asked to leave it. A refused
 * join is not this (its waiter hears that), and neither is a step off the
 * network this driver was asked for. The driver reconnects nothing: whoever
 * installs the hook decides. Survives stop() and forget(); installed once at
 * bring-up.
 *
 * @param cb Called on the system event task, so it only posts. NULL for none.
 */
void kdrv_wifi_on_lost(kdrv_wifi_lost_cb_t cb);

/**
 * @brief Sort a disconnect's reason into what a person can act on.
 *
 * A timed-out handshake counts as the password because that is what a wrong
 * WPA2 password looks like from here: the access point simply never finishes
 * it. A weak signal can do the same, and nothing on this side tells them apart.
 *
 * @param reason The stack's wifi_err_reason_t, as the event carried it.
 * @return The answer it belongs to; never KDRV_WIFI_FAIL_NONE.
 */
kdrv_wifi_fail_t kdrv_wifi_fail_of(uint16_t reason);

/**
 * @brief Why the last attempt failed, or the network last in use was lost. No
 *        RPC: safe anywhere.
 *
 * The first disconnect after an attempt began is the one that says: the
 * station's own step off the network follows a refusal and would otherwise
 * overwrite it. An attempt starting and an address arriving clear it.
 *
 * @return The reason; KDRV_WIFI_FAIL_NONE when there is none to give.
 */
kdrv_wifi_fail_t kdrv_wifi_fail_last(void);

/**
 * @brief The strength kdrv_wifi_rssi() last cached. No RPC: safe anywhere.
 *
 * @return dBm (negative) while still associated; 0 once the driver has
 *         cleared its association state (disconnect, stop, forget).
 */
int8_t kdrv_wifi_rssi_last(void);

/**
 * @brief The primary channel of the network the station is on, as of the last
 *        kdrv_wifi_rssi(): a join's own success takes that sample. No RPC.
 * @return The channel, or 0 before any sample and once the association is gone.
 */
uint8_t kdrv_wifi_channel_last(void);

/**
 * @brief Whether the station is up at all. No RPC: safe anywhere.
 *
 * The question kdrv_wifi_state() cannot answer, because KDRV_WIFI_IDLE means
 * both "never started" and "started, on no network" - and the difference is
 * what decides whether stopping the station would do anything. A scan starts
 * it without ever joining, so the two cases are genuinely reachable.
 *
 * @return true between kdrv_wifi_start() and kdrv_wifi_stop()/forget().
 */
bool kdrv_wifi_started(void);

/**
 * @brief Choose the station's power save. Crosses the RPC link.
 *
 * The C5 sleeps the station between beacons by default (modem save), which
 * is cheap and makes an idle board answer a frame that was sent to it a few
 * seconds late. Off, the radio listens all the time: every frame is answered
 * at once, for about 27 mA more. A blocking RPC like the rest of this file:
 * the kdrv_radio worker is the only caller, never an AO step.
 *
 * @param on true for the default (minimum modem save), false for no save.
 * @return KODE_OK; KODE_ERROR when the stack is not up or refused.
 */
kode_status_t kdrv_wifi_power_save(bool on);

/**
 * @brief Keep the C5's association through the coming esp_restart().
 *
 * IDF registers esp_wifi_stop as a shutdown handler when the station netif is
 * made, and esp_restart() runs it. Here that stop is an RPC to the C5, so a
 * restart would end the very association the C5 is parked to keep (or stall
 * on the RPC). Call it once the C5 has answered the park with OK, right before
 * the restart; it takes the handler off the list. Harmless when the handler
 * is not there. No RPC: safe in an AO step.
 */
void kdrv_wifi_keep_station_on_restart(void);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_WIFI_H_ */
