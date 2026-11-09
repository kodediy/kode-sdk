/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_file.h
 * @brief Shared by kode.h's file calls. Private to this component.
 */
#ifndef KODE_FILE_H_
#define KODE_FILE_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode.h"
#include "kode_sys.h"

/** @brief A task's last read text, read bytes and listing. */
struct kode_file_state;

/**
 * @brief Turn a typed path ("sd/a.txt") into a volume path ("/sdcard/a.txt").
 * @param user What the caller typed; one trailing "/" is dropped.
 * @param out  Receives the volume path.
 * @param cap  Bytes of @p out, NUL included.
 * @return true on success; false with the task's error set for a NULL or empty
 *         path, a leading "/", another volume, an empty, "." or ".." part, or
 *         a path too long for KODE_FILE_PATH_MAX or @p cap.
 */
bool kode_file_path(const char *user, char *out, uint32_t cap);

/**
 * @brief The calling task's files state, taken from PSRAM on first use.
 * @return The state; NULL if PSRAM has none to give.
 */
struct kode_file_state *kode_file_state_ensure(void);

/**
 * @brief Free a files state and its blocks.
 * @param s The state; NULL does nothing.
 */
void kode_file_state_free(struct kode_file_state *s);

/**
 * @brief Run a storage call, again while it answers KODE_DENIED (busy).
 * @param op  One try.
 * @param ctx Handed to @p op.
 * @return The last answer, after at most KODE_FILE_BUSY_MS; KODE_ERROR if
 *         @p op is NULL.
 */
kode_status_t kode_file_sys(kode_status_t (*op)(void *ctx), void *ctx);

/**
 * @brief What a volume path is, in its turn among the file calls, retrying while storage is busy.
 * @param vol The volume path.
 * @param st  Out: the answer.
 * @return The last storage answer, after at most KODE_FILE_BUSY_MS; KODE_NO_RESOURCES if the lock cannot be made.
 */
kode_status_t kode_file_stat(const char *vol, kode_sys_file_status_t *st);

/**
 * @brief One window of a file, in its turn among the file calls, retrying while storage is busy.
 * @param vol    The volume path.
 * @param size   What the file held when it was asked about.
 * @param offset Where the window starts.
 * @param buf    Out: the bytes.
 * @param cap    Bytes asked for, at most KODE_SYS_FILE_READ_MAX.
 * @param got    Out: bytes read; less than @p cap at the end.
 * @return The last storage answer, after at most KODE_FILE_BUSY_MS; KODE_NO_RESOURCES if the lock cannot be made.
 */
kode_status_t kode_file_window(const char *vol, uint32_t size, uint32_t offset, void *buf,
                               uint32_t cap, uint32_t *got);

/**
 * @brief Write or append bytes to a typed path, in parts, in its turn among the file calls.
 * @param user   The path as typed.
 * @param data   The bytes; NULL is refused.
 * @param len    How many; 0 creates or empties the file.
 * @param append Add at the end rather than replace.
 * @return true when all of it is written; false with the task's error set.
 */
bool kode_file_put_bytes(const char *user, const uint8_t *data, uint32_t len, bool append);

/**
 * @brief A short phrase for a failed status.
 * @param rc The status.
 * @return The phrase; never NULL.
 */
const char *kode_file_why(kode_status_t rc);

#endif /* KODE_FILE_H_ */
