/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_banner.h
 * @brief The four-line boot banner kode_init() prints once the log is up; private to this component.
 */
#ifndef KODE_BANNER_H_
#define KODE_BANNER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The reset reason in words.
 * @param reason One of the KDRV_RESET_ values, as kdrv_reset_reason() answers.
 * @return A static, non-empty string; "unknown" for a number the table does not hold.
 */
char const *kode_reset_words(int32_t reason);

/**
 * @brief Whether the core dump's reason can explain this reset (a panic or one of the two watchdogs).
 * @param reason One of the KDRV_RESET_ values.
 * @return true when the dump may speak for this reset.
 */
bool kode_banner_dump_explains(int32_t reason);

/**
 * @brief Compose the banner's reset line, "reset ...".
 *
 * The dump's panic reason replaces the words only where the dump explains the
 * reset. After a return from an app the line names the app and the dump is
 * ignored: apps keep none.
 * @param out         Receives the line, NUL-terminated (cut to fit).
 * @param len         Room in @p out; 0 writes nothing.
 * @param reason      One of the KDRV_RESET_ values.
 * @param dump_reason The dump's panic reason, or NULL.
 * @param app_name    The app the board came back from, or NULL.
 * @return The length written, terminator excluded.
 */
size_t kode_banner_reset_line(char *out, size_t len, int32_t reason,
                              char const *dump_reason, char const *app_name);

/**
 * @brief Print the four banner lines through klog, at every level but off.
 * @note Call once, right after AO_Log starts. In the OS role it reads the TRIED
 *       boot request to name the app the board came back from, and never clears it.
 */
void kode_banner_print(void);

#ifdef __cplusplus
}
#endif

#endif /* KODE_BANNER_H_ */
