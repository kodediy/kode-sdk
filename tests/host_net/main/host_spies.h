/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file host_spies.h
 * @brief What the stubs beside the cases saw: klog's lines and holds, and
 *        the PSRAM blocks kdrv_net took.
 */
#ifndef HOST_SPIES_H_
#define HOST_SPIES_H_

#include <stdbool.h>
#include <stddef.h>

#include "kode_log.h"

/**
 * @brief Whether @p tag is held now, and at what floor.
 *
 * @param tag The tag asked about.
 * @param min Out, when held: the floor. May be NULL.
 * @return true while a kode_log_hold() on @p tag stands.
 */
bool log_stub_held(const char *tag, kode_log_lvl_t *min);

/**
 * @brief Whether one of the last lines logged holds @p needle.
 *
 * @param needle The text looked for.
 * @return true when a kept line contains it.
 */
bool log_stub_saw(const char *needle);

/** @brief Forget the lines kept so far. */
void log_stub_forget(void);

/**
 * @brief Make every later kode_log_hold() fail, or work again.
 *
 * @param refuse true to refuse.
 */
void log_stub_refuse_holds(bool refuse);

/**
 * @brief The first PSRAM block taken of at least @p min_size bytes.
 *
 * @param min_size The smallest size that counts.
 * @param size     Out: the block's size. May be NULL.
 * @return The block, or NULL when none was taken.
 */
const unsigned char *psram_stub_block(size_t min_size, size_t *size);

#endif /* HOST_SPIES_H_ */
