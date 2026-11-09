/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_buttons.c
 * @brief The buttons' doors: AO_Buttons' atomic copies, for a polling app.
 */
#include "kode_sys.h"

#include "kaos.h"
#include "kdrv_buttons.h"

_Static_assert((uint32_t)KDRV_BTN_UP == KODE_SYS_BUTTON_UP, "button ids follow the driver's");
_Static_assert((uint32_t)KDRV_BTN_DOWN == KODE_SYS_BUTTON_DOWN, "button ids follow the driver's");
_Static_assert((uint32_t)KDRV_BTN_LEFT == KODE_SYS_BUTTON_LEFT, "button ids follow the driver's");
_Static_assert((uint32_t)KDRV_BTN_RIGHT == KODE_SYS_BUTTON_RIGHT, "button ids follow the driver's");
_Static_assert((uint32_t)KDRV_BTN_TOP == KODE_SYS_BUTTON_TOP, "button ids follow the driver's");
_Static_assert((uint32_t)KDRV_BTN_BOT == KODE_SYS_BUTTON_BOT, "button ids follow the driver's");
_Static_assert((uint32_t)KDRV_BTN_COUNT == KODE_SYS_BUTTON_COUNT, "button ids follow the driver's");

uint32_t kode_sys_button_read_mask(void)
{
    return (uint32_t)buttons_down();
}

uint32_t kode_sys_button_read_presses(uint8_t id)
{
    return buttons_presses(id);
}
