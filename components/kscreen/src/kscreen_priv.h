/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kscreen_priv.h
 * @brief What kscreen.c tells the rest of the component about the display it
 *        opened; private to this component.
 */
#ifndef KSCREEN_PRIV_H_
#define KSCREEN_PRIV_H_

#include "sdkconfig.h"

#include "kscreen.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(CONFIG_KODE_COMPOSE)
/**
 * @brief The renderer that opened a display.
 *
 * Asks kscreen's own record, never the display's user data: a display
 * kscreen did not open may carry anything there.
 *
 * @param disp The display; may be NULL.
 * @return The kscreen_t kscreen_open() bound to @p disp, or NULL.
 */
kscreen_t *kscreen_of(lv_display_t const *disp);
#endif

#ifdef __cplusplus
}
#endif

#endif /* KSCREEN_PRIV_H_ */
