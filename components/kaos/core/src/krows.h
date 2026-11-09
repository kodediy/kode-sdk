/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file krows.h
 * @brief The self-test rows each owner registers from its start, and the order AO_Autotest walks them in.
 */
#ifndef KROWS_H_
#define KROWS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KROWS_TABLES_MAX  16U  /**< Owner tables the registry holds: one per owner with rows. */
#define KROWS_ROWS_MAX    64U  /**< Rows over every table: an index fits a byte, below a walk's markers. */
#define KROW_LABEL_MAX    17U  /**< Characters of a row's label: what the self-test screen's line holds. */
#define KROW_PROBLEM_MAX  22U  /**< Characters of a row's problem: what the result screen's line holds. */

#define KROW_F_SKIP_NO_CARD  0x01U  /**< A fail for an empty slot is a skip: nothing to judge. */

/* ---- The walk's order ------------------------------------------------------
 * A group runs its rows by these keys, whatever order the owners registered
 * in: the automatic rows first, the four operator rows last. Keep c5 after
 * sd (bringing the link up resets the C5, which switches the card's supply),
 * audio after c5 (its boot drives the amplifier's enable), buttons first of
 * the operator rows (led and display are judged on the pad) and mem last.
 * The rows after mem answer by name only.
 */
#define AUTOTEST_ORDER_SCREENSHOT   1U   /**< Bench: the capture buffer. */
#define AUTOTEST_ORDER_JPEG         2U   /**< Bench: the encoder. */
#define AUTOTEST_ORDER_FS           3U   /**< Bench: the internal volume. */
#define AUTOTEST_ORDER_PMIC         4U   /**< The charger. */
#define AUTOTEST_ORDER_GAUGE        5U   /**< The gauge. */
#define AUTOTEST_ORDER_IOEXP        6U   /**< The expander. */
#define AUTOTEST_ORDER_CLOCKS       7U   /**< The crystals. */
#define AUTOTEST_ORDER_IMU          8U   /**< The IMU. */
#define AUTOTEST_ORDER_MAG          9U   /**< The magnetometer. */
#define AUTOTEST_ORDER_HAPTIC      10U   /**< The motor. */
#define AUTOTEST_ORDER_SD          11U   /**< The card slot. */
#define AUTOTEST_ORDER_C5          12U   /**< The coprocessor. */
#define AUTOTEST_ORDER_IR          13U   /**< The infrared block. */
#define AUTOTEST_ORDER_NFC         14U   /**< The NFC reader. */
#define AUTOTEST_ORDER_RFID        15U   /**< The 125 kHz reader. */
#define AUTOTEST_ORDER_AUDIO       16U   /**< The speaker and the microphone. */
#define AUTOTEST_ORDER_BUTTONS     17U   /**< Operator: the pad. */
#define AUTOTEST_ORDER_LED         18U   /**< Operator: the light. */
#define AUTOTEST_ORDER_DISPLAY     19U   /**< Operator: the screen. */
#define AUTOTEST_ORDER_TP          20U   /**< Operator: the glass. */
#define AUTOTEST_ORDER_PSRAM       21U   /**< The free PSRAM. */
#define AUTOTEST_ORDER_MEM         22U   /**< The internal pool, last. */
#define AUTOTEST_ORDER_BOOT        23U   /**< Bench: the boot record. */
#define AUTOTEST_ORDER_APPS        24U   /**< Bench: the category store. */
#define AUTOTEST_ORDER_KFW         25U   /**< Bench: the unpacker. */
#define AUTOTEST_ORDER_ID          26U   /**< Bench: the serial. */
#define AUTOTEST_ORDER_BLE_CONSOLE 27U   /**< Bench: the BLE console's ring. */
#define AUTOTEST_ORDER_NET         28U   /**< Bench: the gateway echo. */
#define AUTOTEST_ORDER_SD_USER     29U   /**< `user`: the card slot. */
#define AUTOTEST_ORDER_C5_USER     30U   /**< `user`: the coprocessor. */
#define AUTOTEST_ORDER_NFC_FIELD   31U   /**< `user`: the NFC field. */
#define AUTOTEST_ORDER_RFID_FIELD  32U   /**< `user`: the 125 kHz carrier. */
#define AUTOTEST_ORDER_POWER_PLUG  33U   /**< `user`: the charger plugged. */
#define AUTOTEST_ORDER_NFC_CARD    34U   /**< `user`: an NFC card read. */
#define AUTOTEST_ORDER_RFID_CARD   35U   /**< `user`: a 125 kHz card read. */

/** @brief Whether a row answers on the spot or later, on the bus. */
typedef enum {
    KROW_SYNC,   /**< fn returns the verdict. */
    KROW_ASYNC   /**< fn returns false; the owner answers TEST_RESULT_SIG. */
} krow_kind_t;

/** @brief One self-test: runs on AO_Autotest's thread. */
typedef bool (*krow_fn)(void);

/** @brief One row. */
typedef struct {
    const char *name;      /**< The `component=` word that selects it. */
    uint8_t     id;        /**< AUTOTEST_ID_*: what the kept report stores. */
    krow_fn     fn;        /**< The request, or the verdict. */
    krow_kind_t kind;      /**< SYNC or ASYNC. */
    uint8_t     groups;    /**< AUTOTEST_G_* mask. */
    uint32_t    budget_ms; /**< ASYNC: how long its owner has to answer. */
    const char *answer;    /**< The name its owner answers under, when not @c name. */
    uint8_t     flags;     /**< KROW_F_*. */
    const char *label;     /**< What it is, KROW_LABEL_MAX characters at most. */
    const char *problem;   /**< What to say when it fails, KROW_PROBLEM_MAX at most. */
    uint8_t     order;     /**< AUTOTEST_ORDER_*: its place in a group's walk. */
} krow_t;

/**
 * @brief Register an owner's rows. The same table twice is one registration.
 * @param rows The rows; they must outlive AO_Autotest.
 * @param n    How many.
 * @return KODE_OK; KODE_DENIED for a malformed row; KODE_EXISTS for a name,
 *         an id or an order key already taken; KODE_NO_RESOURCES past
 *         KROWS_TABLES_MAX tables or KROWS_ROWS_MAX rows. A refusal logs why
 *         and registers nothing.
 */
kode_status_t krows_add(const krow_t *rows, size_t n);

/** @brief How many rows are registered. @return The total. */
size_t krows_count(void);

/**
 * @brief Row @p i in registration order.
 * @param i 0..krows_count()-1.
 * @return The row, or NULL past the end.
 */
const krow_t *krows_at(size_t i);

/**
 * @brief The row that carries @p id.
 * @param id AUTOTEST_ID_*.
 * @return The row, or NULL when no owner registered it.
 */
const krow_t *krows_by_id(uint8_t id);

/** @brief Forget every table. For the host suite. */
void krows_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* KROWS_H_ */
