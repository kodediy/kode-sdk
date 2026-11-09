/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_identity_esp.c
 * @brief khal_identity: the serial from the C5's MAC, cached in NVS.
 */
#include <string.h>

#include "kbsp_identity.h"
#include "khal_identity.h"
#include "khal_nvs.h"
#include "kode_log.h"

#define TAG "khal.id"

#define ID_KEY  "c5mac"   /**< NVS key of the cached MAC. */

#define ID_BYTE_BITS    8U    /**< Bits in a byte of the MAC. */
#define ID_SYMBOL_BITS  5U    /**< Bits in one Base32 symbol. */
#define ID_SYMBOLS      10U   /**< Symbols of the body: 48 bits, left-padded to 50. */
#define ID_PADDED_BITS  (ID_SYMBOLS * ID_SYMBOL_BITS) /**< The MAC's bits, padded. */
#define ID_SYMBOL_MASK  ((1U << ID_SYMBOL_BITS) - 1U) /**< One symbol's bits. */
#define ID_RADIX        32U   /**< Symbols in the alphabet: Luhn mod 32 for the check. */
#define ID_GROUP_DASHES 3U    /**< The dashes of the printed 4-4-3 grouping. */

/** Crockford Base32: no I, L, O or U, so nothing reads as a 0 or a 1. */
static const char ID_ALPHABET[ID_RADIX + 1U] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/** How many characters KBSP_ID_PREFIX contributes, its NUL excluded. */
#define ID_PREFIX_LEN   (sizeof(KBSP_ID_PREFIX) - 1U)

/* The public lengths follow the prefix in kbsp, so a new prefix cannot truncate. */
_Static_assert(KHAL_ID_LEN == (ID_PREFIX_LEN + ID_SYMBOLS + 2U),
               "KHAL_ID_LEN must hold KBSP_ID_PREFIX, ten body symbols, the "
               "check symbol and a NUL");
_Static_assert(KHAL_ID_PRETTY_LEN == (KHAL_ID_LEN + ID_GROUP_DASHES),
               "KHAL_ID_PRETTY_LEN must be KHAL_ID_LEN plus the three group "
               "separators");

static struct {
    char    serial[KHAL_ID_LEN];
    char    pretty[KHAL_ID_PRETTY_LEN];
    uint8_t mac[KHAL_ID_MAC_LEN];
    bool    valid;
} s_id;

/**
 * @brief The Luhn mod 32 check over @p n symbol values.
 * @param v The values, 0..31, most significant first.
 * @param n How many.
 * @return The check value, 0..31.
 */
static uint8_t id_luhn32(const uint8_t *v, uint8_t n)
{
    uint32_t sum    = 0U;
    uint32_t factor = 2U;

    /* From the rightmost value leftwards, the factor alternating 2, 1, 2, ... */
    for ( uint8_t k = n; 0U < k; --k )
    {
        uint32_t const p = factor * (uint32_t)v[k - 1U];

        sum += (p / ID_RADIX) + (p % ID_RADIX);
        if ( 2U == factor )
        {
            factor = 1U;
        }
        else
        {
            factor = 2U;
        }
    }
    return (uint8_t)((ID_RADIX - (sum % ID_RADIX)) % ID_RADIX);
}

kode_status_t khal_identity_encode(const uint8_t mac[KHAL_ID_MAC_LEN], char *out, size_t cap)
{
    if ( (NULL == mac) || (NULL == out) || (KHAL_ID_LEN > cap) )
    {
        return KODE_ERROR;
    }

    uint64_t value = 0U;
    for ( uint8_t i = 0U; KHAL_ID_MAC_LEN > i; ++i )
    {
        value = (value << ID_BYTE_BITS) | (uint64_t)mac[i];
    }

    (void)memcpy(out, KBSP_ID_PREFIX, ID_PREFIX_LEN);

    /* Most significant symbol first; the padding makes the first one 0..7. */
    uint8_t sym[ID_SYMBOLS];
    for ( uint8_t i = 0U; ID_SYMBOLS > i; ++i )
    {
        uint8_t const shift = (uint8_t)(ID_PADDED_BITS - (((uint32_t)i + 1U) * ID_SYMBOL_BITS));
        sym[i] = (uint8_t)((value >> shift) & ID_SYMBOL_MASK);
        out[ID_PREFIX_LEN + i] = ID_ALPHABET[sym[i]];
    }

    out[ID_PREFIX_LEN + ID_SYMBOLS] = ID_ALPHABET[id_luhn32(sym, ID_SYMBOLS)];
    out[ID_PREFIX_LEN + ID_SYMBOLS + 1U] = '\0';
    return KODE_OK;
}

/** @brief Render the printed form, KODE-XXXX-XXXX-XXX, from the canonical one. */
static void id_pretty(const char *canon, char *out)
{
    static const uint8_t GROUPS[ID_GROUP_DASHES] = { 4U, 4U, 3U };

    size_t src = ID_PREFIX_LEN;
    size_t dst = ID_PREFIX_LEN;

    (void)memcpy(out, canon, ID_PREFIX_LEN);

    for ( uint8_t g = 0U; (sizeof(GROUPS) / sizeof(GROUPS[0])) > g; ++g )
    {
        out[dst] = '-';
        ++dst;
        (void)memcpy(&out[dst], &canon[src], GROUPS[g]);
        dst += GROUPS[g];
        src += GROUPS[g];
    }

    out[dst] = '\0';
}

/** @brief Whether six bytes are all zero: an unburned eFuse, not an address. */
static bool mac_is_zero(const uint8_t mac[KHAL_ID_MAC_LEN])
{
    bool zero = true;
    for ( uint8_t i = 0U; KHAL_ID_MAC_LEN > i; ++i )
    {
        if ( 0U != mac[i] )
        {
            zero = false;
        }
    }
    return zero;
}

/** @brief Build both forms from a MAC already checked. */
static kode_status_t id_build(const uint8_t mac[KHAL_ID_MAC_LEN])
{
    if ( khal_identity_encode(mac, s_id.serial, sizeof(s_id.serial)) != KODE_OK )
    {
        s_id.valid = false;
        return KODE_ERROR;
    }
    id_pretty(s_id.serial, s_id.pretty);
    (void)memcpy(s_id.mac, mac, sizeof(s_id.mac));
    s_id.valid = true;
    return KODE_OK;
}

kode_status_t khal_identity_init(void)
{
    uint8_t mac[KHAL_ID_MAC_LEN];
    size_t  len = sizeof(mac);

    if ( (khal_nvs_get_blob(KHAL_IDENTITY_NVS_NS, ID_KEY, mac, &len) != KODE_OK)
        || (sizeof(mac) != len) || mac_is_zero(mac) )
    {
        s_id.valid = false;
        KLOG_N(TAG, "this board has no name yet: its C5 has not been seen");
        return KODE_ERROR;
    }
    /* Called by the dock's console and by the boot, in either order: a name
     * already said is not said again. */
    bool const said = s_id.valid && (memcmp(s_id.mac, mac, sizeof(s_id.mac)) == 0);

    if ( id_build(mac) != KODE_OK )
    {
        return KODE_ERROR;
    }
    if ( !said )
    {
        KLOG_N(TAG, "this board is %s (C5 %02x:%02x:%02x:%02x:%02x:%02x)",
               s_id.pretty, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    return KODE_OK;
}

kode_status_t khal_identity_set_mac(const uint8_t mac[KHAL_ID_MAC_LEN])
{
    char old[KHAL_ID_PRETTY_LEN];
    bool had;

    if ( (NULL == mac) || mac_is_zero(mac) )
    {
        return KODE_DENIED;
    }
    if ( s_id.valid && (memcmp(s_id.mac, mac, sizeof(s_id.mac)) == 0) )
    {
        return KODE_OK;
    }
    had = s_id.valid;
    (void)memcpy(old, s_id.pretty, sizeof(old));
    if ( id_build(mac) != KODE_OK )
    {
        return KODE_ERROR;
    }
    if ( had )
    {
        KLOG_N(TAG, "serial changed: %s -> %s (new C5)", old, s_id.pretty);
    }
    else
    {
        KLOG_N(TAG, "this board is %s (from its C5 %02x:%02x:%02x:%02x:%02x:%02x)",
               s_id.pretty, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    if ( khal_nvs_set_blob(KHAL_IDENTITY_NVS_NS, ID_KEY, mac, KHAL_ID_MAC_LEN) != KODE_OK )
    {
        KLOG_W(TAG, "the C5's MAC could not be cached; the name lasts this boot");
        return KODE_IO_ERROR;
    }
    return KODE_OK;
}

const char *khal_identity_serial(void)
{
    const char *serial = "";

    if ( s_id.valid )
    {
        serial = s_id.serial;
    }
    return serial;
}

const char *khal_identity_pretty(void)
{
    const char *pretty = "";

    if ( s_id.valid )
    {
        pretty = s_id.pretty;
    }
    return pretty;
}

bool khal_identity_valid(void)
{
    return s_id.valid;
}

kode_status_t khal_identity_mac(uint8_t mac[KHAL_ID_MAC_LEN])
{
    if ( (NULL == mac) || !s_id.valid )
    {
        return KODE_ERROR;
    }
    (void)memcpy(mac, s_id.mac, sizeof(s_id.mac));
    return KODE_OK;
}
