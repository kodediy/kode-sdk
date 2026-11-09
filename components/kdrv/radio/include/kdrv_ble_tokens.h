/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_ble_tokens.h
 * @brief What the P4's BLE bond store keeps in place of a key.
 */
#ifndef KDRV_BLE_TOKENS_H_
#define KDRV_BLE_TOKENS_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode_defs.h"
#include "kode_wire.h"   /* KODE_TOKEN_LTK, KODE_TOKEN_IRK: the magics */

#ifdef __cplusplus
extern "C" {
#endif

/* cppcheck-suppress misra-c2012-2.5 ; used by kdrv_ble.c, under CONFIG_KODEOS_BLE_CONSOLE_PAIRING */
#define KBT_ROLE_OUR    0U       /**< A key this board distributed.           */
/* cppcheck-suppress misra-c2012-2.5 ; used by kdrv_ble.c, under CONFIG_KODEOS_BLE_CONSOLE_PAIRING */
#define KBT_ROLE_PEER   1U       /**< A key the peer distributed.             */

/**
 * @brief Sends one key to the C5; KODE_OK once the C5 holds it.
 * @param tok The token that stands for it.
 * @param key The key.
 */
typedef kode_status_t (*kbt_put_t)(const uint8_t tok[16], const uint8_t key[16]);

/**
 * @brief The token for one key of one bond.
 * @param magic KODE_TOKEN_LTK or KODE_TOKEN_IRK.
 * @param role  KBT_ROLE_*.
 * @param addr  The peer's identity address.
 * @param type  Its type.
 * @param out   Out: 16 bytes.
 */
void kbt_token(const char magic[4], uint8_t role, const uint8_t addr[6],
               uint8_t type, uint8_t out[16]);

/**
 * @brief Whether 16 bytes are one of kodeOS's tokens.
 * @param key The bytes, or NULL.
 * @return true for either magic.
 */
bool kbt_is_token(const uint8_t key[16]);

/**
 * @brief Move a bond record's LTK and IRK to the C5 and leave tokens.
 *
 * Nothing is replaced until every put succeeded, so a refusal leaves the
 * record as it came and the caller refuses the write.
 *
 * @param ltk  In/out: the LTK, or NULL when absent.
 * @param irk  In/out: the IRK, or NULL when absent (or not to be moved).
 * @param role KBT_ROLE_*.
 * @param addr Peer identity address.
 * @param type Its type.
 * @param put  Sends one key to the C5.
 * @return KODE_OK (keys replaced, or already tokens), or put's error
 *         (nothing replaced).
 */
kode_status_t kbt_tokenize(uint8_t *ltk, uint8_t *irk, uint8_t role,
                           const uint8_t addr[6], uint8_t type, kbt_put_t put);

/**
 * @brief Whether a link that ended means the bond's key is gone - the C5
 *        does not know its token (after a wipe, or a C5 swapped in repair),
 *        or the peer lost its half - so the bond is forgotten and the next
 *        attempt pairs again.
 * @param status BLE_GAP_EVENT_DISCONNECT's reason (a ble_hs code; the
 *               encryption change only says ENOTCONN by then).
 * @param bonded Whether the store holds a bond for the peer.
 * @return true for PIN or key missing and MIC failure on a bonded peer.
 */
bool kbt_forget_on_enc_fail(int status, bool bonded);

/**
 * @brief Whether the C5's answer to KODE_BTKEYS_HAS says the bond's key is
 *        gone, which is the one case its bond is forgotten for.
 *
 * A device spoofing a bonded peer's address can end a link with either
 * reason kbt_forget_on_enc_fail() takes, but it cannot make the C5 lose a
 * key it holds. So a key the C5 still has keeps the bond, and so does any
 * answer that is not a clear "not here": a C5 that did not answer, or one
 * too old to know the question.
 *
 * @param rc     How the call went (kdrv_c5svc_call()).
 * @param status The C5's KODE_ST_* byte when @p rc is KODE_OK.
 * @return true only for KODE_OK with KODE_ST_NOT_FOUND.
 */
bool kbt_c5_lost_key(kode_status_t rc, uint8_t status);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_BLE_TOKENS_H_ */
