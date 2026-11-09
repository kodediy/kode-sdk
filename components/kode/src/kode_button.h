/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_button.h
 * @brief The buttons' names, and the shared last-seen press count behind
 *        kode_button_was_pressed() and kode_button_get_presses(). Private to
 *        this component; never installed.
 */
#ifndef KODE_BUTTON_H_
#define KODE_BUTTON_H_

#include <stdint.h>

/**
 * @brief Where kode_button_take() reads a button's press count from.
 *
 * @param id A button id, a kdrv_button_t.
 * @return The presses since boot, as kode_sys_button_read_presses() answers.
 */
typedef uint32_t (*kode_button_reader_t)(uint8_t id);

/**
 * @brief The name of button @p id, the one the kode.h calls and the event
 *        names spell: "up", "down", "left", "right", "top" or "bottom".
 *
 * The only table of the six names; the events built from them read it here.
 *
 * @param id A button id, a kdrv_button_t.
 * @return The name, a string that lives for the life of the firmware; NULL
 *         for an id out of range.
 */
const char *kode_button_name(uint8_t id);

/**
 * @brief The presses of button @p id since the last time anyone asked, and
 *        mark them as asked.
 *
 * One last-seen count per button, shared by every task and every kode.h call
 * that reports news, moved with a compare-and-swap so two tasks asking at
 * once never report one press twice and never lose one. The count is read
 * again after a lost swap, so a reading that went stale cannot move the
 * last-seen count backwards. The difference is taken modulo 2^32.
 *
 * @param id   A button id; must be below the number of buttons.
 * @param read Where the press count comes from.
 * @return The presses since the last call for this button; 0 for an id out of
 *         range.
 */
uint32_t kode_button_take(uint8_t id, kode_button_reader_t read);

#endif /* KODE_BUTTON_H_ */
