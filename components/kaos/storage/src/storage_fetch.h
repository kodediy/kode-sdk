/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file storage_fetch.h
 * @brief What AO_Storage calls of its download module, and what the module
 *        calls of AO_Storage. AO_Storage names the module's two weakly
 *        (KAOS_WEAK, storage_ao.c): they are NULL in an image whose app asks
 *        for no download, where none of the download's events exist.
 */
#ifndef STORAGE_FETCH_H_
#define STORAGE_FETCH_H_

#include <stdbool.h>
#include <stddef.h>

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

/* ---- What the download calls of AO_Storage -------------------------------- */

/**
 * @brief Where a file with no volume named goes, as a virtual path: where a
 *        write of that name goes (storage_write_all_request()), and said so.
 *
 * AO_Storage's thread only.
 *
 * @param name The bare name.
 * @param[out] out The path.
 * @param cap  The size of @p out.
 * @return false when the path would not fit @p cap.
 */
bool storage_bare_path(const char *name, char *out, size_t cap);

/**
 * @brief A file AO_Storage put on the card for a bare name failed a write
 *        there: the card is not chosen again this boot, and it is mounted
 *        again once nothing is in flight. AO_Storage's thread only.
 */
void storage_bare_failed(void);

#endif /* STORAGE_FETCH_H_ */
