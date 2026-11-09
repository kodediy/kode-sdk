/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_defs.h
 * @brief kode_status_t: what every fallible call of the SDK returns.
 */
#ifndef KODE_DEFS_H_
#define KODE_DEFS_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Status of every call that can fail. There is no BUSY: a busy driver
 *        waits or answers KODE_DENIED.
 */
typedef enum {
    KODE_OK = 0,          /**< The call did what it says.                    */
    KODE_ERROR,           /**< It failed, and no other code says more.       */
    KODE_TIMEOUT,         /**< The far end did not answer in time.           */
    KODE_DENIED,          /**< Refused: bad argument, wrong state, no right. */
    KODE_NO_RESOURCES,    /**< No memory, slot or handle left.               */
    KODE_IO_ERROR,        /**< The device itself refused the operation.      */
    KODE_EXISTS,          /**< The name is already taken.                    */
    KODE_NOT_DIR,         /**< A component of the path is not a directory.   */
    KODE_IS_DIR,          /**< The path is a directory; a file was wanted.   */
} kode_status_t;

#ifdef __cplusplus
}
#endif

#endif /* KODE_DEFS_H_ */
