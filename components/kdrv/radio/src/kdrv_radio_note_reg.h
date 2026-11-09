/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_radio_note_reg.h
 * @brief Where the handover note lives, and its two accesses; private to kdrv/radio.
 */
#ifndef KDRV_RADIO_NOTE_REG_H_
#define KDRV_RADIO_NOTE_REG_H_

#include <stdint.h>

#include "soc/lp_system_reg.h"
#include "soc/soc.h"

/** The register that holds the handover note: one of the P4's LP store
 * registers, which keep their value through esp_restart() and every reset but
 * a power-on, whatever image runs next. Where a variable would sit in RTC
 * memory depends on the image's own RTC sections, so two firmwares would
 * disagree about it; a register is the same address for all of them. IDF
 * reserves STORE0 to STORE11 on the P4 (esp_rom rtc.h), and nothing in the
 * IDF or in this tree touches 15. After a power-on its content is noise, and
 * the reset reason, read with it, is what makes that harmless. */
#define RADIO_NOTE_REG  LP_SYSTEM_REG_LP_STORE15_REG

/** @brief The stored word of the handover note. @return The word. */
static inline uint32_t kdrv_radio_note_reg_read(void)
{
    return REG_READ(RADIO_NOTE_REG);
}

/** @brief Store a word in the handover note's register. @param word The word; 0 erases it. */
static inline void kdrv_radio_note_reg_write(uint32_t word)
{
    REG_WRITE(RADIO_NOTE_REG, word);
}

#endif /* KDRV_RADIO_NOTE_REG_H_ */
