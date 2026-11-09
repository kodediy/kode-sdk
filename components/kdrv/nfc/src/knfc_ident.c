/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file knfc_ident.c
 * @brief The reader's identity judgement. See knfc_ident.h.
 */
#include "knfc_ident.h"

#include "kdrv_st25r3916.h"

bool knfc_ident_supported(uint8_t identity)
{
    return ((identity & KDRV_ST25R3916_IC_TYPE_MASK) == KDRV_ST25R3916_IC_TYPE_B)
           && ((identity & KDRV_ST25R3916_IC_REV_MASK) >= KDRV_ST25R3916_IC_REV_MIN);
}
