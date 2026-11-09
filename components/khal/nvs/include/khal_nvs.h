/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_nvs.h
 * @brief Small persistent key/value storage, behind a seam.
 */
#ifndef KHAL_NVS_H_
#define KHAL_NVS_H_

#include <stdint.h>
#include <stddef.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Prepare the backing store. Idempotent; call before the rest.
 *
 * A partition of another NVS version, or one out of pages, is erased whole:
 * settings and app data go back to their defaults rather than fail the boot.
 * Every call here answers KODE_DENIED from a task whose stack is in PSRAM; to a
 * getter that reads as a missing key, and only the error log line tells them apart.
 * @return KODE_OK, or KODE_ERROR when the store cannot be used.
 */
kode_status_t khal_nvs_init(void);

/**
 * @brief Read a string.
 *
 * @param ns   Namespace, at most 15 characters.
 * @param key  Key, at most 15 characters.
 * @param out  Destination, always NUL-terminated on KODE_OK.
 * @param cap  Size of @p out.
 * @return KODE_DENIED when the key is not there (not an error - it is the
 *         answer "nothing was stored").
 */
kode_status_t khal_nvs_get_str(const char *ns, const char *key,
                               char *out, size_t cap);

/**
 * @brief Write a string and commit it.
 * @param ns    Namespace, at most 15 characters.
 * @param key   Key, at most 15 characters.
 * @param value The string; NULL is refused.
 * @return KODE_OK; KODE_DENIED when the caller's stack is in PSRAM; KODE_ERROR
 *         when the store refused.
 */
kode_status_t khal_nvs_set_str(const char *ns, const char *key,
                               const char *value);

/**
 * @brief Read one byte - the scalar a settings record is made of.
 *
 * @param ns   Namespace, at most 15 characters.
 * @param key  Key, at most 15 characters.
 * @param out  Where the value goes on KODE_OK.
 * @return KODE_DENIED when the key is not there (not an error - it is the
 *         answer "nothing was stored", and the caller's default applies).
 */
kode_status_t khal_nvs_get_u8(const char *ns, const char *key, uint8_t *out);

/**
 * @brief Write one byte and commit it.
 * @param ns    Namespace, at most 15 characters.
 * @param key   Key, at most 15 characters.
 * @param value The byte.
 * @return KODE_OK; KODE_DENIED when the caller's stack is in PSRAM; KODE_ERROR
 *         when the store refused.
 */
kode_status_t khal_nvs_set_u8(const char *ns, const char *key, uint8_t value);

/**
 * @brief Read a 64-bit value: one NVS entry, where a blob of 8 bytes takes
 *        three.
 *
 * @param ns   Namespace, at most 15 characters.
 * @param key  Key, at most 15 characters.
 * @param out  Where the value goes on KODE_OK.
 * @return KODE_DENIED when the key is not there, KODE_ERROR when @p out is
 *         NULL or the read failed.
 */
kode_status_t khal_nvs_get_u64(const char *ns, const char *key, uint64_t *out);

/**
 * @brief Write a 64-bit value and commit it.
 * @param ns    Namespace, at most 15 characters.
 * @param key   Key, at most 15 characters.
 * @param value The value.
 * @return KODE_OK, or KODE_ERROR when the store refused.
 */
kode_status_t khal_nvs_set_u64(const char *ns, const char *key, uint64_t value);

/**
 * @brief Read a fixed-size record.
 *
 * @param ns  Namespace, at most 15 characters.
 * @param key Key, at most 15 characters.
 * @param out Where the record goes on KODE_OK.
 * @param len On entry the size of @p out; on return what was actually
 *            stored. A record of a different size than expected is the
 *            caller's to judge - that is what version fields are for.
 * @return KODE_DENIED when the key is not there.
 */
kode_status_t khal_nvs_get_blob(const char *ns, const char *key,
                                void *out, size_t *len);

/**
 * @brief Write a record and commit it.
 *
 * One record rather than a key per field, when the fields have to agree with
 * each other: a single commit either lands or does not, so a power cut
 * cannot leave a half-updated list behind.
 *
 * @param ns    Namespace, at most 15 characters.
 * @param key   Key, at most 15 characters.
 * @param value The record; NULL is refused.
 * @param len   Its size; 0 is refused.
 * @return KODE_OK; KODE_DENIED when the caller's stack is in PSRAM; KODE_ERROR
 *         when the store refused.
 */
kode_status_t khal_nvs_set_blob(const char *ns, const char *key,
                                const void *value, size_t len);

/**
 * @brief Forget @p key. Missing is success: the postcondition holds.
 * @param ns  Namespace, at most 15 characters.
 * @param key Key, at most 15 characters.
 * @return KODE_OK, also when the key was not there; KODE_DENIED when the
 *         caller's stack is in PSRAM; KODE_ERROR when the store refused.
 */
kode_status_t khal_nvs_erase(const char *ns, const char *key);

/**
 * @brief The name of stored key @p idx in @p ns, so a store can be listed.
 *
 * @param ns   Namespace, at most 15 characters.
 * @param idx  Which key, counting from zero.
 * @param out  Destination for the name, always NUL-terminated on KODE_OK.
 * @param cap  Size of @p out. A name that does not fit is refused, never
 *             shortened: a truncated key opens nothing.
 * @return KODE_DENIED when @p idx is past the last key, or when the
 *         namespace has never been written - the same "nothing is stored"
 *         every other reader here answers, so a caller can loop on KODE_OK
 *         without asking for a count first.
 */
kode_status_t khal_nvs_key(const char *ns, uint16_t idx,
                           char *out, size_t cap);

/** @brief Room for a namespace name: the store's 15 characters and the NUL. */
#define KHAL_NVS_NS_SIZE  16U

/**
 * @brief The name of namespace @p idx among those that hold at least one key,
 *        so a store can be listed whole.
 *
 * Each name counts once, however many keys it holds, and no order is
 * promised: while nothing is written in between, indices 0..N-1 give every
 * such namespace exactly once.
 *
 * @param idx  Which namespace, counting from zero.
 * @param out  Destination for the name, always NUL-terminated on KODE_OK.
 * @param cap  Size of @p out. A name that does not fit is refused, never
 *             shortened: a truncated name opens nothing.
 * @return KODE_OK; KODE_DENIED when @p idx is past the last namespace (so a
 *         caller can loop on KODE_OK without a count), or when the caller's
 *         stack is in PSRAM; KODE_ERROR when @p out is NULL, @p cap is 0, the
 *         name does not fit or the store refused.
 */
kode_status_t khal_nvs_namespace(uint16_t idx, char *out, size_t cap);

/**
 * @brief Forget every key of @p ns and commit.
 *
 * @param ns  Namespace, at most 15 characters.
 * @return KODE_OK, also when @p ns holds no keys; KODE_DENIED when the
 *         caller's stack is in PSRAM; KODE_ERROR when @p ns is NULL or the
 *         store refused.
 */
kode_status_t khal_nvs_erase_namespace(const char *ns);

#ifdef __cplusplus
}
#endif

#endif /* KHAL_NVS_H_ */
