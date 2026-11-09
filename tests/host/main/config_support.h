/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file config_support.h
 * @brief One `config` request handed straight to its owner, and its answer.
 */
#ifndef CONFIG_SUPPORT_H_
#define CONFIG_SUPPORT_H_

#include <stdbool.h>
#include <stdint.h>

#include "kqp.h"

/** @brief What a case asks: a key, and with @c set a value of its kind. */
typedef struct {
    const char *key;  /**< The setting.                              */
    bool        set;  /**< A set rather than a read.                 */
    int32_t     i;    /**< An INT value.                             */
    bool        b;    /**< A BOOL value.                             */
    uint8_t     e;    /**< An ENUM value: the word's index.          */
    const char *s;    /**< A STRING value; NULL is none.             */
} config_ask_t;

/**
 * @brief Dispatch @p ask into @p ao and take the answer it published.
 *
 * Every event the spy holds ahead of the answer is taken and passed over.
 *
 * @param ao  The owner.
 * @param ask The request.
 * @return The answer, valid until the next ao_spy_drain(); NULL when the
 *         owner published none.
 */
const KodeConfigAnsEvt *config_ask(QActive *ao, const config_ask_t *ask);

#endif /* CONFIG_SUPPORT_H_ */
