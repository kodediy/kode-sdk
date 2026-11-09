/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_ble_tokens.c
 * @brief Tokens in the P4's bond store, and when a key moves to the C5.
 */
#include "kdrv_ble_tokens.h"

#include <string.h>

/* ble_hs reports an HCI error as BLE_HS_ERR_HCI_BASE + the HCI code. */
#define KBT_HS_HCI_BASE        0x200
#define KBT_HCI_KEY_MISSING    0x06   /**< PIN or Key Missing.                */
#define KBT_HCI_MIC_FAILURE    0x3D   /**< Connection Terminated due to MIC.  */

void kbt_token(const char magic[4], uint8_t role, const uint8_t addr[6],
               uint8_t type, uint8_t out[16])
{
    (void)memset(out, 0, 16U);
    (void)memcpy(out, magic, 4U);
    (void)memcpy(&out[4], addr, 6U);
    out[10] = type;
    out[11] = role;
}

bool kbt_is_token(const uint8_t key[16])
{
    bool is = false;

    if ( NULL != key )
    {
        /* Four bytes of a magic, compared as bytes and never as a C string:
         * the literal is the magic's bytes. */
        /* cppcheck-suppress [misra-c2012-21.14,misra-c2012-21.16] */
        bool const ltk = (memcmp(key, KODE_TOKEN_LTK, 4U) == 0);
        /* cppcheck-suppress [misra-c2012-21.14,misra-c2012-21.16] */
        bool const irk = (memcmp(key, KODE_TOKEN_IRK, 4U) == 0);
        is = ltk || irk;
    }
    return is;
}

/* cppcheck-suppress misra-c2012-8.7 ; called by kdrv_ble.c, under CONFIG_KODEOS_BLE_CONSOLE_PAIRING */
kode_status_t kbt_tokenize(uint8_t *ltk, uint8_t *irk, uint8_t role,
                           const uint8_t addr[6], uint8_t type, kbt_put_t put)
{
    uint8_t       ltk_tok[16];
    uint8_t       irk_tok[16];
    bool const    move_ltk = (NULL != ltk) && !kbt_is_token(ltk);
    bool const    move_irk = (NULL != irk) && !kbt_is_token(irk);
    kode_status_t rc       = KODE_OK;

    kbt_token(KODE_TOKEN_LTK, role, addr, type, ltk_tok);
    kbt_token(KODE_TOKEN_IRK, role, addr, type, irk_tok);
    if ( move_ltk )
    {
        rc = put(ltk_tok, ltk);
    }
    if ( (KODE_OK == rc) && move_irk )
    {
        rc = put(irk_tok, irk);
    }
    if ( KODE_OK == rc )
    {
        if ( move_ltk )
        {
            (void)memcpy(ltk, ltk_tok, 16U);
        }
        if ( move_irk )
        {
            (void)memcpy(irk, irk_tok, 16U);
        }
    }
    return rc;
}

/* cppcheck-suppress misra-c2012-8.7 ; called by kdrv_ble.c, under CONFIG_KODEOS_BLE_CONSOLE_PAIRING */
bool kbt_c5_lost_key(kode_status_t rc, uint8_t status)
{
    return (KODE_OK == rc) && ((uint8_t)KODE_ST_NOT_FOUND == status);
}

/* cppcheck-suppress misra-c2012-8.7 ; called by kdrv_ble.c, under CONFIG_KODEOS_BLE_CONSOLE_PAIRING */
bool kbt_forget_on_enc_fail(int status, bool bonded)
{
    return bonded
           && (((KBT_HS_HCI_BASE + KBT_HCI_KEY_MISSING) == status)
               || ((KBT_HS_HCI_BASE + KBT_HCI_MIC_FAILURE) == status));
}
