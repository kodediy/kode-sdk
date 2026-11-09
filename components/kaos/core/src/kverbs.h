/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kverbs.h
 * @brief The console verbs a device's owner registers from its start, so an owner the app does not link leaves none.
 */
#ifndef KVERBS_H_
#define KVERBS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "console_grammar.h"
#include "kode_defs.h"
#include "kqp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KVERBS_TABLES_MAX  19U  /**< Owner tables the registry holds: one per owner with verbs (fifteen) and four for an app. */
#define KVERB_NAME_MAX     24U  /**< The longest verb, in characters. */

/** @brief The keys a spec array declares, as a kverb_t row counts them. */
#define KVERB_KEYS(a_)  ((uint8_t)(sizeof(a_) / sizeof((a_)[0])))

struct kverb;

/** @brief A handler: the line's values, already validated against the row. */
typedef void (*kverb_fn)(const struct kverb *c, const karg_val_t *v);

/** @brief One verb: its name, its keys and its handler. */
typedef struct kverb {
    const char        *verb;    /**< The line's first token. */
    const karg_spec_t *args;    /**< Its keys; NULL when none. */
    uint8_t            n_args;  /**< How many, at most KARG_KEYS_MAX. */
    kverb_fn           fn;      /**< Runs on AO_Console's thread. */
} kverb_t;

/** @brief One owner's table. */
typedef struct {
    const kverb_t *verbs;                 /**< Its rows. */
    size_t         n;                     /**< How many. */
    bool (*answer)(QEvt const *e);        /**< An event while one of its verbs holds the console; true when it was the owner's. NULL: none. */
    bool (*timeout)(void);                /**< The watchdog fired on one of its verbs; true when the owner closed it. NULL: the timeout error. */
    void (*info)(void);                   /**< Its records in `info`. NULL: none. */
} kverbs_t;

/**
 * @brief Register an owner's table. The same table twice is one registration.
 * @param table The table; it must outlive the console.
 * @return KODE_OK; KODE_DENIED for a malformed row; KODE_EXISTS for a verb
 *         already taken; KODE_NO_RESOURCES past KVERBS_TABLES_MAX. A refusal
 *         logs why and registers nothing.
 */
kode_status_t kverbs_add(const kverbs_t *table);

/** @brief How many verbs are registered, over every table. @return The total. */
size_t kverbs_count(void);

/**
 * @brief Verb @p i in registration order.
 * @param i 0..kverbs_count()-1.
 * @return The row, or NULL past the end.
 */
const kverb_t *kverbs_at(size_t i);

/**
 * @brief The registered verb called @p verb.
 * @param verb  The word.
 * @param owner Out, when not NULL: the table that holds it.
 * @return The row, or NULL.
 */
const kverb_t *kverbs_find(const char *verb, const kverbs_t **owner);

/**
 * @brief Table @p i in registration order.
 * @param i 0..KVERBS_TABLES_MAX-1.
 * @return The table, or NULL past the last one.
 */
const kverbs_t *kverbs_table(size_t i);

/** @brief Forget every table. For the host suite. */
void kverbs_reset(void);

/* ---- The owners' tables, registered by their starts ---- */

extern const kverbs_t led_verbs;      /**< AO_LedMgr's `led_*`. */
extern const kverbs_t touch_verbs;    /**< AO_Touch's `tp`. */
extern const kverbs_t audio_verbs;    /**< AO_Audio's `audio*`. */
extern const kverbs_t haptic_verbs;   /**< AO_Haptic's `haptic*`. */
extern const kverbs_t ir_verbs;       /**< AO_Ir's `ir*`. */
extern const kverbs_t nfc_verbs;      /**< AO_Nfc's `nfc*`. */
extern const kverbs_t rfid_verbs;     /**< AO_Rfid's `rfid*`. */
extern const kverbs_t display_verbs;  /**< AO_Display's `display_info`. */
extern const kverbs_t radio_verbs;    /**< AO_Radio's link, C5, identity, Wi-Fi and BLE verbs. */
extern const kverbs_t net_verbs;      /**< AO_Net's network and MQTT verbs. */
extern const kverbs_t storage_verbs;  /**< AO_Storage's `sd_*`, `fs_*` and card table. */
extern const kverbs_t appmgr_verbs;   /**< AO_AppMgr's `app_*`. */
extern const kverbs_t lowpower_verbs; /**< AO_LowPower's `sleep`, `power_off` and `lowpower_info`. */
extern const kverbs_t scheduler_verbs; /**< AO_Scheduler's `cron`. */
extern const kverbs_t test_dot_verbs;  /**< AO_TestDot's `test_dot_*`, and `test component=all` handed over by the console. */

#ifdef __cplusplus
}
#endif

#endif /* KVERBS_H_ */
