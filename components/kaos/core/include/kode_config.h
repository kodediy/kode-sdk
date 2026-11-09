/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_config.h
 * @brief Settings: the stored values a host reads and changes with `config`.
 */
#ifndef KODE_CONFIG_H_
#define KODE_CONFIG_H_

#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"
#include "kode_functions.h"
#include "kqp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KODE_CONFIG_MAX        32U    /**< Settings per firmware, all tables.  */
#define KODE_CONFIG_TABLES     15U    /**< kode_config_add() calls accepted:
                                       *   one per SDK owner with settings
                                       *   (Log, Power, Boot, Display, Radio,
                                       *   Audio, Haptic, Imu, Net, LowPower),
                                       *   kgui's, and four of an app's.      */
#define KODE_CONFIG_ANSWER_MS  1000U  /**< How long the console waits for an owner. */
#define KODE_CONFIG_MARGIN     KQP_CMD_MARGIN
                                      /**< The room a request leaves in an
                                       *   app's own owner's queue: a fuller
                                       *   queue answers busy.                */

/** @brief One setting. Every field but name and type has a default. */
typedef struct {
    const char        *name;    /**< [a-z0-9_], 1..KODE_ROW_NAME_MAX, unique
                                 *   over every setting.                     */
    kode_type_t        type;    /**< INT, BOOL, ENUM or STRING.              */
    int32_t            min;     /**< KODE_TYPE_INT only. min == max: unbounded. */
    int32_t            max;     /**< KODE_TYPE_INT only.                     */
    const char *const *enums;   /**< KODE_TYPE_ENUM: the words, each a valid name. */
    uint8_t            n_enums; /**< 1..KODE_VALUE_ENUMS_MAX.                */
} kode_config_t;

/**
 * @brief Register one table of settings, answered by @p owner.
 *
 * Call it once @p owner is started (after its QACTIVE_START, or from its
 * initial transition): every request for one of these keys is posted to it,
 * and a post needs its queue. The table is kept by pointer, so it must be
 * `static const`. A table with one bad row is refused whole, with a log line
 * naming the row.
 *
 * @param owner  The AO that answers for every row of the table.
 * @param margin The slots a request leaves free in @p owner's queue, or it
 *               answers busy; KODE_CONFIG_MARGIN for an app's own.
 * @param table  The rows.
 * @param n      How many.
 * @return KODE_OK, also for the same table again; KODE_EXISTS for a key
 *         already taken; KODE_NO_RESOURCES past the caps; KODE_DENIED for
 *         any other rule, and always without CONFIG_KODEOS_CONSOLE.
 */
kode_status_t kode_config_add(QActive *owner, uint16_t margin,
                              const kode_config_t *table, size_t n);

/** @brief kode_config_add() over a whole array. */
#define KODE_CONFIG_ADD(owner, margin, t) \
    kode_config_add((owner), (margin), (t), sizeof(t) / sizeof((t)[0]))

/**
 * @brief Post a request to the owner of its key, leaving that owner's margin.
 *
 * The one way a CONFIG_REQ_SIG reaches an owner. The answer is
 * CONFIG_ANS_SIG, published, with the request's `seq` and key.
 *
 * @param req    A request from Q_NEW_X(), key and value filled in. Consumed
 *               whether or not it is posted.
 * @param sender The asking AO: the post's sender.
 * @return true when posted; false for a NULL request, a key no table holds,
 *         or an owner whose queue is too full, which the asker treats as busy.
 */
bool kode_config_post(KodeConfigReqEvt *req, void const *sender);

/* ---- A setting's owner ---------------------------------------------------
 * The owner of a setting answers every request for one of its keys, a read
 * or a set, once. The requests are posted to it: it subscribes to nothing.
 */

/**
 * @brief Answer a CONFIG_REQ_SIG for one of @p owner's settings.
 *
 * Call it from the owner's own step. For a set, apply and store the value
 * first, then answer with what is now in force.
 *
 * @param owner  The AO answering: the publish's sender.
 * @param req    The request.
 * @param status KODE_OK; KODE_DENIED for a value the owner refuses.
 * @param stored The value is in NVS, not the owner's default.
 * @param value  The value as its word, unquoted, at most
 *               KODE_CONFIG_VALUE_MAX characters; NULL for none.
 * @return true when the answer is published; false for a NULL request, a
 *         longer value, or an empty pool, and the console times out.
 */
bool kode_config_answer(QActive *owner, const KodeConfigReqEvt *req,
                        kode_status_t status, bool stored, const char *value);

#ifdef __cplusplus
}
#endif

#endif /* KODE_CONFIG_H_ */
