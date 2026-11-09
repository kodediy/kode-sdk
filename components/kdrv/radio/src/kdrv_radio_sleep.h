/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_radio_sleep.h
 * @brief The rules of the radio's sleep sentinel, free of ESP-IDF.
 */
#ifndef KDRV_RADIO_SLEEP_H_
#define KDRV_RADIO_SLEEP_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Timing ---- */

#define KRADIO_SLEEP_PS_WAIT_MS      20U     /**< After announcing power save, before the SDIO tasks stop. */
#define KRADIO_SLEEP_OFF_RETRIES     100U    /**< Knocks after the first one, while the C5 wakes up.       */
#define KRADIO_SLEEP_OFF_RETRY_MS    10U     /**< Between two knocks.                                      */
#define KRADIO_SLEEP_RESUME_WAIT_MS  50U     /**< After the C5 answered, before the SDIO tasks run again.  */
#define KRADIO_SLEEP_AWAKE_MS        100U    /**< Awake after a wake, for the traffic to land.             */
#define KRADIO_SLEEP_GAP_MS          3000U   /**< Awake again after BLE GAP activity.                      */
#define KRADIO_SLEEP_SETTLE_TRIES    20U     /**< Looks at the SDIO tasks before giving up on the way down. */
#define KRADIO_SLEEP_SETTLE_MS       5U      /**< Between two looks.                                       */

/* ---- esp-hosted's interface numbers ---- */

/* esp_hosted_if_type_t in the component's common/esp_hosted_interface.h counts
 * ESP_INVALID_IF as 0, then ESP_STA_IF, ESP_AP_IF, ESP_SERIAL_IF, ESP_HCI_IF.
 * That header is not on the include path of any consumer, so the numbers are
 * pinned here; the spike that proved the sentinel first had them wrong, and
 * an esp-hosted that renumbers them breaks the counters, not the link. */
#define KRADIO_SLEEP_IF_STA      1U   /**< ESP_STA_IF: station frames.        */
#define KRADIO_SLEEP_IF_SERIAL   3U   /**< ESP_SERIAL_IF: the RPC channel.    */
#define KRADIO_SLEEP_IF_HCI      4U   /**< ESP_HCI_IF: BLE.                   */

/** @brief The counter a frame of this interface type adds to. */
typedef enum {
    KRADIO_SLEEP_TX_STA = 0,   /**< Station frames.                      */
    KRADIO_SLEEP_TX_SERIAL,    /**< RPC frames.                          */
    KRADIO_SLEEP_TX_HCI,       /**< BLE frames.                          */
    KRADIO_SLEEP_TX_OTHER      /**< Private and control frames; not counted. */
} kradio_sleep_tx_t;

/** @brief The calls the rules make; kdrv_radio_sleep.c fills it with the real ones. */
typedef struct {
    int      (*power_save)(bool on);   /**< Tell the C5 the host enters (true) or leaves (false) power save; 0 = taken. */
    void     (*sdio_tasks)(bool run);  /**< Resume (true) or suspend (false) esp-hosted's SDIO tasks.                   */
    bool     (*sdio_settled)(void);    /**< Every SDIO task is blocked, none in the middle of a transaction.            */
    bool     (*bus_down)(bool down);   /**< Mark the bus silent (true: drain old events, ring on any frame or line edge,
                                        *   arm the C5's line; false: unmark). false answer from true: the line would not arm. */
    void     (*delay_ms)(uint32_t ms); /**< Wait.                                                                       */
    uint32_t (*gap_activity)(void);    /**< The BLE GAP event count; changes when the link moves.                       */
    bool     (*cmd_waiting)(void);     /**< A command (start or stop) waits for the sentinel's task.                    */
} kradio_sleep_seam_t;

/** @brief How the way down ended. Any answer but KRADIO_DOWN_OK leaves the bus up. */
typedef enum {
    KRADIO_DOWN_OK = 0,   /**< The bus is silent, the C5 told, the line armed.              */
    KRADIO_DOWN_NO_LINE,  /**< The C5's wake line would not arm; nothing else was touched.   */
    KRADIO_DOWN_NO_PS,    /**< The C5 did not take the announcement.                         */
    KRADIO_DOWN_BUSY      /**< The SDIO tasks never settled; the C5 was told power save is off again. */
} kradio_down_t;

/**
 * @brief The way down: mark the bus silent, announce power save, wait, suspend the SDIO tasks.
 *
 * The mark comes first, before the C5 is told anything: a frame the host
 * queues from then on must wake the sentinel, or its caller waits for an
 * answer that cannot come. The tasks are suspended only when all of them are
 * blocked - one stopped in the middle of an SDIO transaction would hold the
 * bus's mutex, and the way back up would wait for it for ever. If they do not
 * settle in KRADIO_SLEEP_SETTLE_TRIES looks, or the C5 does not take the
 * announcement, the bus is left as it was (a C5 already told is told again that
 * power save is off) and the mark is taken back. A bus nobody told the C5
 * about must not go quiet.
 *
 * @param seam The calls to make.
 * @return How it ended.
 */
kradio_down_t kradio_sleep_down(const kradio_sleep_seam_t *seam);

/**
 * @brief The way up: knock the C5 awake, then resume the SDIO tasks.
 *
 * The first power-save-off message only wakes the C5 (it wakes on its CMD
 * line), so it is repeated, up to KRADIO_SLEEP_OFF_RETRIES more times
 * KRADIO_SLEEP_OFF_RETRY_MS apart, until the C5 answers. Only then, and
 * after KRADIO_SLEEP_RESUME_WAIT_MS, do the SDIO tasks run: a write that
 * reaches a sleeping C5 times out and esp-hosted restarts the board. A C5
 * that never answers leaves the tasks suspended.
 *
 * @param seam   The calls to make.
 * @param knocks Receives the messages sent, the one that was answered included.
 * @return true when the C5 answered and the bus is up.
 */
bool kradio_sleep_up(const kradio_sleep_seam_t *seam, uint16_t *knocks);

/**
 * @brief One wake from end to end: the way up, then the awake window.
 *
 * The window is KRADIO_SLEEP_AWAKE_MS, and it is repeated for
 * KRADIO_SLEEP_GAP_MS for as long as the BLE GAP count moves during it: a
 * connect and its encryption need the board up for longer than the traffic
 * that announced them. A command that waits for the sentinel's task ends the
 * repeated window: the console's own output over BLE moves the count too, and
 * a stop would otherwise wait for the link to fall silent. A deaf C5 ends the
 * wake at once, with the bus down.
 *
 * @param seam   The calls to make.
 * @param knocks Receives the messages the way up sent.
 * @return true when the C5 answered; false when it is deaf.
 */
bool kradio_sleep_wake_once(const kradio_sleep_seam_t *seam, uint16_t *knocks);

/**
 * @brief Which counter a frame queued for the C5 belongs to.
 * @param if_type esp-hosted's interface type of the frame.
 * @return The counter; KRADIO_SLEEP_TX_OTHER for any type that is not counted.
 */
kradio_sleep_tx_t kradio_sleep_tx_slot(uint8_t if_type);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_RADIO_SLEEP_H_ */
