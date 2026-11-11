/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_wire.h
 * @brief The KODE services between the P4 and the ESP32-C5: the wire.
 *
 * One message id each way over esp-hosted's peer data, with the service and
 * the operation in a header, because esp-hosted's table of peer-data handlers
 * is three entries long and the infrared service already holds one of them:
 * the services share the other entry, and a new service takes none. Four
 * services ride it: the Wi-Fi vault, the unit's identity, the BLE bond keys
 * and the Wi-Fi frames. The first three each hold a secret on the C5 and
 * perform the one operation that needs it; no operation hands a secret back.
 * The fourth holds no secret: it listens on one channel when asked and sends
 * what it hears as events, unasked, in batches.
 *
 * Every multi-byte field is little-endian, which is what both chips are; the
 * structs are packed and their sizes asserted. THE SAME FILE LIVES IN TWO
 * PLACES - sdk/components/kdrv/radio/include/ and sdk/c5/main/ - and the two
 * copies must be identical. KODE_WIRE_VERSION goes up with a change an older
 * peer would misread: a field that moves or changes meaning, a size that
 * changes. A new service or operation is not one, because a peer that does
 * not know it answers KODE_ST_BAD_REQUEST. The P4 refuses a C5 that answers
 * with another version.
 */
#ifndef KODE_WIRE_H_
#define KODE_WIRE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Bumped on a change an older peer would misread; see the top of the file. */
#define KODE_WIRE_VERSION    3U

/** @brief P4 to C5: a request. 'K','O','D', 0x01. */
#define KODE_MSG_REQ         0x4B4F4401UL
/** @brief C5 to P4: the answer to one request. */
#define KODE_MSG_RSP         0x4B4F4481UL

/** @brief Largest payload either way. The certificate pair is 982 B DER. */
#define KODE_MAX_PAYLOAD     2048U

/* ---- Services ------------------------------------------------------------ */
#define KODE_SVC_SELF        0U  /**< The services themselves: INFO.         */
#define KODE_SVC_VAULT       1U  /**< Wi-Fi credentials.                     */
#define KODE_SVC_ID          2U  /**< The unit's identity.                   */
#define KODE_SVC_BTKEYS      3U  /**< BLE bond keys behind tokens.           */
#define KODE_SVC_SNIFF       4U  /**< Wi-Fi frames heard, on request.        */
#define KODE_SVC_COUNT       5U  /**< One past the last service.             */

/** @brief An op with this bit is an event: sent by the C5 unasked, seq 0. */
#define KODE_OP_EVENT        0x80U

/* ---- Operations, per service --------------------------------------------- */
#define KODE_SELF_INFO       1U  /**< -> kode_info_t.                        */
/* cppcheck-suppress misra-c2012-2.5 ; the wire is shared with the C5, which answers it; the P4 asks it from AO_Radio */
#define KODE_SELF_PARK       2U  /**< ->; keep the station, drop the host.   */

#define KODE_VAULT_REMEMBER  1U  /**< kode_vault_cred_t ->.                  */
#define KODE_VAULT_JOIN      2U  /**< kode_vault_ssid_t ->.                  */
#define KODE_VAULT_LIST      3U  /**< -> kode_vault_list_t.                  */
#define KODE_VAULT_FORGET    4U  /**< kode_vault_ssid_t ->.                  */
#define KODE_VAULT_WIPE      5U  /**< ->.                                    */

#define KODE_ID_CSR          1U  /**< -> CSR, DER.                           */
#define KODE_ID_CERT_SET     2U  /**< kode_id_cert_t ->.                     */
#define KODE_ID_CERT_GET     3U  /**< -> kode_id_cert_t.                     */
#define KODE_ID_SIGN_AUTH    4U  /**< kode_id_nonce_t -> signature, DER.     */
#define KODE_ID_LOCK         5U  /**< ->.                                    */
/* cppcheck-suppress misra-c2012-2.5 ; the wire is shared with the C5, which answers it */
#define KODE_ID_STATE        6U  /**< -> one byte of KODE_ID_F_* flags.      */

/* cppcheck-suppress misra-c2012-2.5 ; the wire is shared with the C5, which uses it; on the P4 only kdrv_ble.c does, under CONFIG_KODEOS_BLE_CONSOLE_PAIRING */
#define KODE_BTKEYS_PUT      1U  /**< kode_btkey_t ->.                       */
/* cppcheck-suppress misra-c2012-2.5 ; the wire is shared with the C5, which uses it; on the P4 only kdrv_ble.c does, under CONFIG_KODEOS_BLE_CONSOLE_PAIRING */
#define KODE_BTKEYS_DEL      2U  /**< A 16-byte token ->.                    */
#define KODE_BTKEYS_WIPE     3U  /**< ->.                                    */
/* cppcheck-suppress misra-c2012-2.5 ; the wire is shared with the C5, which uses it; on the P4 only kdrv_ble.c does, under CONFIG_KODEOS_BLE_CONSOLE_PAIRING */
#define KODE_BTKEYS_HAS      4U  /**< A 16-byte token -> OK or NOT_FOUND.     */

#define KODE_SNIFF_START     1U  /**< kode_sniff_start_t -> kode_sniff_state_t. */
#define KODE_SNIFF_STOP      2U  /**< -> kode_sniff_state_t.                 */
#define KODE_SNIFF_STATE     3U  /**< -> kode_sniff_state_t.                 */
#define KODE_SNIFF_EVT_BATCH 0x81U /**< kode_sniff_batch_t, then its records. */

/* ---- Status bytes --------------------------------------------------------- */
#define KODE_ST_OK           0U  /**< Done.                                  */
#define KODE_ST_REFUSED      1U  /**< By policy, or the service is busy.     */
#define KODE_ST_BAD_REQUEST  2U  /**< Malformed, or a field out of range.    */
#define KODE_ST_NOT_FOUND    3U  /**< Nothing stored under that name.        */
#define KODE_ST_STORAGE      4U  /**< NVS or the Key Manager failed.         */
#define KODE_ST_LOCKED       5U  /**< Identity is locked.                    */

/* ---- Identity state flags ------------------------------------------------- */
#define KODE_ID_F_KEY        0x01U  /**< A Key Manager key exists.           */
#define KODE_ID_F_CERT       0x02U  /**< A certificate for it is stored.     */
#define KODE_ID_F_LOCKED     0x04U  /**< CSR and CERT_SET are closed.        */

/* ---- BLE key tokens ------------------------------------------------------- */
#define KODE_TOKEN_LEN       16U      /**< Same length as the key it hides. */
#define KODE_TOKEN_LTK       "KBTL"   /**< 4-byte magic of an LTK token.    */
#define KODE_TOKEN_IRK       "KBTI"   /**< 4-byte magic of an IRK token.    */

/* ---- Wi-Fi frames: why the service is not listening ------------------------ */
#define KODE_SNIFF_WHY_NONE     0U  /**< Listening, or stopped as asked.     */
#define KODE_SNIFF_WHY_WIFI_OFF 1U  /**< Wi-Fi is not started on the C5.     */
#define KODE_SNIFF_WHY_JOINED   2U  /**< Joined: only its own channel.      */
#define KODE_SNIFF_WHY_CHANNEL  3U  /**< The radio refused the channel.      */
#define KODE_SNIFF_WHY_MEMORY   4U  /**< No room for the queue or the task.  */

#define KODE_SNIFF_BATCH_MAX    8U  /**< Records in one batch, at most.      */
#define KODE_SNIFF_HEAD_LEN     8U  /**< Bytes kept after the MAC header.    */
#define KODE_SNIFF_RSSI_NONE    (-128) /**< rssi_max of a period with no frame. */

/** @brief Every message starts with this. */
typedef struct __attribute__((packed)) {
    uint8_t  version;   /**< KODE_WIRE_VERSION.                             */
    uint8_t  service;   /**< KODE_SVC_*.                                    */
    uint8_t  op;        /**< Per service.                                   */
    uint8_t  seq;       /**< Echoed in the answer.                          */
    uint8_t  status;    /**< KODE_ST_*; 0 in a request.                     */
    uint8_t  reserved;  /**< 0.                                             */
    uint16_t len;       /**< Payload bytes after the header.                */
} kode_hdr_t;
_Static_assert(sizeof(kode_hdr_t) == 8U, "kode_hdr_t is 8 bytes on the wire");

/** @brief KODE_SELF_INFO's answer. */
typedef struct __attribute__((packed)) {
    uint8_t  wire_version;   /**< The C5's KODE_WIRE_VERSION.               */
    uint8_t  vault_count;    /**< Networks saved.                           */
    uint8_t  btkeys_count;   /**< BLE keys held.                            */
    uint8_t  id_flags;       /**< KODE_ID_F_*.                              */
    uint32_t heap_free;      /**< Internal heap, bytes.                     */
    uint32_t heap_low;       /**< Lowest since boot.                        */
    uint32_t heap_largest;   /**< Largest free block.                       */
    uint32_t swaps;          /**< Tokens swapped since boot.                */
    uint32_t misses;         /**< Tokens seen and not known.                */
} kode_info_t;
_Static_assert(sizeof(kode_info_t) == 24U, "kode_info_t layout");

#define KODE_SSID_MAX        32U  /**< Bytes of an SSID, at most.               */
#define KODE_PASS_MAX        64U  /**< Bytes of a Wi-Fi password, at most.      */
#define KODE_VAULT_NETS_MAX  8U   /**< Networks the vault keeps.                */
#define KODE_CERT_DER_MAX    1536U /**< Bytes of the unit cert and its chain.  */
#define KODE_BTKEY_LEN       16U  /**< Bytes of an LTK or an IRK.              */

/** @brief An SSID, NUL-terminated, 1..32 bytes. */
typedef struct __attribute__((packed)) {
    char ssid[KODE_SSID_MAX + 1U];   /**< The network's name.               */
} kode_vault_ssid_t;
_Static_assert(sizeof(kode_vault_ssid_t) == (KODE_SSID_MAX + 1U), "kode_vault_ssid_t layout");

/** @brief REMEMBER's request. pass is empty for an open network. */
typedef struct __attribute__((packed)) {
    char ssid[KODE_SSID_MAX + 1U];   /**< 1..32 bytes and a NUL.            */
    char pass[KODE_PASS_MAX + 1U];   /**< Empty, or 8..64 bytes, and a NUL. */
} kode_vault_cred_t;
_Static_assert(sizeof(kode_vault_cred_t) == (KODE_SSID_MAX + KODE_PASS_MAX + 2U),
               "kode_vault_cred_t layout");

/** @brief LIST's answer, most recently used first. */
typedef struct __attribute__((packed)) {
    uint8_t count;                                     /**< Entries used, 0..8. */
    char    ssid[KODE_VAULT_NETS_MAX][KODE_SSID_MAX + 1U]; /**< Names only.     */
} kode_vault_list_t;
_Static_assert(sizeof(kode_vault_list_t)
               == (1U + (KODE_VAULT_NETS_MAX * (KODE_SSID_MAX + 1U))),
               "kode_vault_list_t layout");

/** @brief Longest nonce SIGN_AUTH accepts. */
#define KODE_NONCE_MAX       200U

/** @brief SIGN_AUTH's request. */
typedef struct __attribute__((packed)) {
    uint8_t len;                     /**< 1..KODE_NONCE_MAX.                */
    char    nonce[KODE_NONCE_MAX];   /**< [A-Za-z0-9._-], no NUL.           */
} kode_id_nonce_t;
_Static_assert(sizeof(kode_id_nonce_t) == (1U + KODE_NONCE_MAX), "kode_id_nonce_t layout");

/**
 * @brief Whether a nonce is one the cloud's challenge can issue, and so one
 *        the unit's key signs: 1..KODE_NONCE_MAX characters of [A-Za-z0-9._-].
 *
 * Checked on both chips: on the P4 for a quick answer, on the C5 because the
 * P4 is not trusted with the key.
 *
 * @param nonce The characters, not necessarily NUL-terminated.
 * @param len   Their count.
 * @return true when every character is in the alphabet and the count fits.
 */
static inline bool kode_nonce_ok(const char *nonce, size_t len)
{
    bool ok = (NULL != nonce) && (1U <= len) && ((size_t)KODE_NONCE_MAX >= len);

    for ( size_t i = 0U; ok && (i < len); i++ )
    {
        char const c = nonce[i];
        ok = (('A' <= c) && ('Z' >= c)) || (('a' <= c) && ('z' >= c))
             || (('0' <= c) && ('9' >= c)) || ('.' == c) || ('_' == c)
             || ('-' == c);
    }
    return ok;
}

/** @brief CERT_SET's request and CERT_GET's answer: unit cert, then chain. */
typedef struct __attribute__((packed)) {
    uint16_t unit_len;    /**< Bytes of the unit certificate.               */
    uint16_t chain_len;   /**< Bytes of the intermediate after it.          */
    uint8_t  der[KODE_CERT_DER_MAX]; /**< unit_len bytes, then chain_len.  */
} kode_id_cert_t;
_Static_assert(sizeof(kode_id_cert_t) == (4U + KODE_CERT_DER_MAX), "kode_id_cert_t layout");

/** @brief BTKEYS_PUT's request. */
/* cppcheck-suppress misra-c2012-2.4 ; the wire is shared with the C5, which uses it; on the P4 only kdrv_ble.c does, under CONFIG_KODEOS_BLE_CONSOLE_PAIRING */
typedef struct __attribute__((packed)) {
    uint8_t token[KODE_TOKEN_LEN];   /**< What the P4's bond store keeps.   */
    uint8_t key[KODE_BTKEY_LEN];     /**< What the controller needs.        */
/* cppcheck-suppress misra-c2012-2.3 ; the wire is shared with the C5, which uses it; on the P4 only kdrv_ble.c does, under CONFIG_KODEOS_BLE_CONSOLE_PAIRING */
} kode_btkey_t;
_Static_assert(sizeof(kode_btkey_t) == (KODE_TOKEN_LEN + KODE_BTKEY_LEN), "kode_btkey_t layout");

#define KODE_SNIFF_CHANNEL_MAX  177U  /**< Highest channel a SNIFF_START names (5 GHz 177). */

/** @brief SNIFF_START's request. */
typedef struct __attribute__((packed)) {
    uint8_t channel;    /**< 1..KODE_SNIFF_CHANNEL_MAX, or 0: stay where the radio is. */
    uint8_t reserved;   /**< 0.                                                */
} kode_sniff_start_t;
_Static_assert(sizeof(kode_sniff_start_t) == 2U, "kode_sniff_start_t layout");

/** @brief The answer to every SNIFF operation. */
typedef struct __attribute__((packed)) {
    uint8_t on;         /**< 1: listening.                                     */
    uint8_t channel;    /**< The radio's primary channel now.                  */
    uint8_t joined;     /**< 1: the station holds an association.              */
    uint8_t why;        /**< KODE_SNIFF_WHY_*.                                 */
} kode_sniff_state_t;
_Static_assert(sizeof(kode_sniff_state_t) == 4U, "kode_sniff_state_t layout");

/** @brief One frame heard: its header and the first bytes after it. */
typedef struct __attribute__((packed)) {
    int8_t   rssi;      /**< dBm.                                              */
    uint8_t  fc[2];     /**< Frame control, as on the air.                     */
    uint8_t  addr1[6];  /**< Address 1, as on the air.                         */
    uint8_t  addr2[6];  /**< Zeros when the frame has none.                    */
    uint16_t len;       /**< The frame's length on the air.                    */
    uint8_t  head[KODE_SNIFF_HEAD_LEN]; /**< After the MAC header; zeros past the end. */
} kode_sniff_rec_t;
_Static_assert(sizeof(kode_sniff_rec_t) == 25U, "kode_sniff_rec_t layout");

/** @brief KODE_SNIFF_EVT_BATCH's payload: this header, then count records. */
typedef struct __attribute__((packed)) {
    uint8_t  channel;   /**< The radio's primary channel in this period.       */
    uint8_t  count;     /**< Records after this header, 0..KODE_SNIFF_BATCH_MAX. */
    int8_t   rssi_max;  /**< Strongest frame of the period, or KODE_SNIFF_RSSI_NONE. */
    uint8_t  joined;    /**< 1: the station holds an association.              */
    uint32_t heard;     /**< Frames heard since START.                         */
    uint16_t mgmt;      /**< Management frames heard in this period.           */
    uint16_t data;      /**< Data frames heard in this period.                 */
} kode_sniff_batch_t;
_Static_assert(sizeof(kode_sniff_batch_t) == 12U, "kode_sniff_batch_t layout");
/* A full batch is one message, and a message holds KODE_MAX_PAYLOAD. */
_Static_assert((sizeof(kode_sniff_batch_t)
                + (KODE_SNIFF_BATCH_MAX * sizeof(kode_sniff_rec_t))) <= KODE_MAX_PAYLOAD,
               "a full kode_sniff_batch_t fits KODE_MAX_PAYLOAD");

#endif /* KODE_WIRE_H_ */
