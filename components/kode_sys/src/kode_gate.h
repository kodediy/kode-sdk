/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_gate.h
 * @brief The gate at the top of kode_init(): does this boot go on?
 */
#ifndef KODE_GATE_H_
#define KODE_GATE_H_

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_KODE_LOWPOWER

/**
 * @brief Let this boot go on, or send the board back to OFF.
 *
 * Any pad button, and the expander's own interrupt, pulls the line that wakes
 * OFF, but OFF is left by BOT alone. The gate therefore reads the expander
 * before it touches anything else, with every pin deep sleep held still held,
 * and:
 *  - a cold boot, or any boot that is not a wake from OFF: lets go of the
 *    holds (they are harmless when none were set) and returns;
 *  - a wake from OFF with BOT pressed: lets go of the holds and returns;
 *  - a wake from OFF with a pad button down, and the wake its release brings:
 *    back to OFF at once and with no light;
 *  - a wake from OFF with nothing down, a press too short to be read: a dim green
 *    pulse, then BOT is watched for a second. BOT turns it on from there with no
 *    hold; otherwise it goes back to OFF.
 *
 * Whenever the board goes back to OFF the holds were never released, so the C5
 * stays in reset, the card unpowered and the glass dark. It does not return on
 * the board.
 *
 * An expander that will not answer cannot say what woke the board, and a
 * board that cannot be told apart from a dead one is worse than a boot that
 * was not asked for: the boot goes on, with a warning.
 *
 * Every boot that goes on is asked one more thing: a cell under
 * KBSP_CELL_CUT_MV with no cable, on the gauge's word and a fresh conversion of
 * the PMIC's together, gives three red flashes and ship mode. A boot that follows
 * a brownout is held to KBSP_CELL_DIP_CUT_MV instead. Any reading that fails, any
 * sign of a cable, and any boot the gauge calls healthy go on; on that last path
 * the cost is three reads of the gauge.
 *
 * Call it first in kode_init(), before any Active Object starts. It needs
 * the I2C bus and nothing else.
 */
void kode_boot_gate(void);

#endif /* CONFIG_KODE_LOWPOWER */

#ifdef __cplusplus
}
#endif

#endif /* KODE_GATE_H_ */
