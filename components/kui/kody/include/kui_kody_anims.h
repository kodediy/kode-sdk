/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Written by tools/kodygen.py from kody/assets/: change the sources there and run the script, never this file. */
/**
 * @file kui_kody_anims.h
 * @brief Kody's built-in scene, one descriptor per animation, for the player in kui_kody.h.
 */
#ifndef KUI_KODY_ANIMS_H
#define KUI_KODY_ANIMS_H

#include "kui_kody.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const kui_kody_anim_t kody_reloj;           /**< From the apocrifa's kody_anims: reloj.. 410 x 334, 540 frames at 30 fps, an anchor every 45. */
extern const kui_kody_anim_t kody_despierta_reloj; /**< From the apocrifa's kody_anims: despierta_reloj.. 410 x 334, 57 frames at 30 fps. */
extern const kui_kody_anim_t kody_reloj_te_mira;   /**< From the apocrifa's kody_anims: reloj_te_mira.. 410 x 334, 45 frames at 30 fps. */
extern const kui_kody_anim_t kody_reloj_mira_hora; /**< From the apocrifa's kody_anims: reloj_mira_hora.. 410 x 334, 45 frames at 30 fps. */
extern const kui_kody_anim_t kody_reloj_lee_aviso; /**< From the apocrifa's kody_anims: reloj_lee_aviso.. 410 x 334, 90 frames at 30 fps. */
extern const kui_kody_anim_t kody_reloj_enchufado; /**< From the apocrifa's kody_anims: reloj_enchufado.. 410 x 334, 90 frames at 30 fps. */

#ifdef __cplusplus
}
#endif

#endif /* KUI_KODY_ANIMS_H */
