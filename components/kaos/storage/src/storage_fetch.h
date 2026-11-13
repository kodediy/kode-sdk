/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file storage_fetch.h
 * @brief What AO_Storage calls of its download module. AO_Storage names both
 *        weakly (KAOS_WEAK, storage_ao.c): they are NULL in an image whose app
 *        asks for no download, where none of the download's events exist.
 */
#ifndef STORAGE_FETCH_H_
#define STORAGE_FETCH_H_

#include <stdbool.h>

#include "kaos.h"
#include "kqp.h"

/**
 * @brief Give the download an event of AO_Storage's queue.
 *
 * AO_Storage's thread only.
 *
 * @param e    The event.
 * @param busy Whether another transfer holds AO_Storage's one file handle, or
 *             the card is out on a call: a chunk is not written then.
 * @return true when @p e was the download's and has been dealt with.
 */
bool storage_fetch_event(QEvt const * const e, bool busy);

/**
 * @brief Whether a download is under way. AO_Storage's thread only.
 * @return true from the ask AO_Storage took to the end's answer.
 */
bool storage_fetch_running(void);

#endif /* STORAGE_FETCH_H_ */
