/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file knfc_ndef.c
 * @brief The NDEF record codec.
 */
#include "knfc_ndef.h"

#include <string.h>

/*
 * The header of a short, single, well-known record:
 *   MB 1  it is the first record
 *   ME 1  and the last
 *   CF 0  not chunked
 *   SR 1  the payload length fits one byte
 *   IL 0  no ID field
 *   TNF 1 NFC Forum well-known type
 */
#define HDR_SR_WKT     0xD1U
#define TYPE_URI       'U'
#define TYPE_TEXT      'T'

#define HDR_SR_BIT     0x10U   /**< Short record.                        */
#define HDR_IL_BIT     0x08U   /**< An ID field follows the type.        */

#define TLV_NULL       0x00U
#define TLV_LONG_LEN   0xFFU   /**< The length is the next two bytes.    */

#define SHORT_MAX      255U    /**< The largest payload a short record has. */
#define TLV_SHORT_MAX  254U    /**< The largest message a one-byte TLV has. */
#define LANG_MAX       63U     /**< Six bits of the status byte.         */
#define TEXT_UTF16     0x80U   /**< Status byte bit 7.                   */
#define TEXT_LANG_MASK 0x3FU

/** @brief The prefixes worth spending a code on, longest first. */
static const struct {
    uint8_t     code;
    const char *txt;
} WRITE_PREFIXES[] = {
    { 0x02U, "https://www." },
    { 0x01U, "http://www."  },
    { 0x04U, "https://"     },
    { 0x03U, "http://"      },
    { 0x06U, "mailto:"      },
    { 0x05U, "tel:"         },
};

/*
 * The NFC Forum's whole table. Writing uses the six above; reading needs all
 * of them, because the tag in your hand may have been written by anything.
 * An unknown code is not an error - the URI simply comes back without a
 * prefix, which is closer to the truth than refusing to read it.
 */
static const char *const READ_PREFIXES[] = {
    "",                    "http://www.",  "https://www.",
    "http://",             "https://",     "tel:",
    "mailto:",             "ftp://anonymous:anonymous@",
    "ftp://ftp.",          "ftps://",      "sftp://",
    "smb://",              "nfs://",       "ftp://",
    "dav://",              "news:",        "telnet://",
    "imap:",               "rtsp://",      "urn:",
    "pop:",                "sip:",         "sips:",
    "tftp:",               "btspp://",     "btl2cap://",
    "btgoep://",           "tcpobex://",   "irdaobex://",
    "file://",             "urn:epc:id:",  "urn:epc:tag:",
    "urn:epc:pat:",        "urn:epc:raw:", "urn:epc:",
    "urn:nfc:",
};

/**
 * @brief Case-insensitive comparison of @p n bytes, ASCII only.
 *
 * Written out rather than taken from strings.h: strncasecmp is a POSIX
 * function and this file is compiled for the board as well as for the host.
 *
 * @param a First string.
 * @param b Second string.
 * @param n How many bytes to compare.
 * @return true when they match.
 */
static bool same_ci(const char *a, const char *b, size_t n)
{
    for ( size_t i = 0U; i < n; ++i )
    {
        char ca = a[i];
        char cb = b[i];
        if ( ('A' <= ca) && ('Z' >= ca) )
        {
            ca = (char)(ca + ('a' - 'A'));
        }
        if ( ('A' <= cb) && ('Z' >= cb) )
        {
            cb = (char)(cb + ('a' - 'A'));
        }
        if ( ca != cb )
        {
            return false;
        }
        if ( '\0' == ca )
        {
            return true;
        }
    }
    return true;
}

/**
 * @brief Split @p uri into a prefix code and the rest of it.
 *
 * @param uri  The link.
 * @param rest Out: what follows the prefix. Never NULL on return.
 * @return The prefix code, 0 when none matched.
 */
static uint8_t uri_split(const char *uri, const char **rest)
{
    for ( size_t i = 0U;
         (sizeof(WRITE_PREFIXES) / sizeof(WRITE_PREFIXES[0])) > i; ++i )
    {
        size_t n = strlen(WRITE_PREFIXES[i].txt);
        if ( same_ci(uri, WRITE_PREFIXES[i].txt, n) )
        {
            *rest = uri + n;
            return WRITE_PREFIXES[i].code;
        }
    }
    *rest = uri;
    return 0U;
}

kode_status_t knfc_ndef_make_uri(const char *uri, uint8_t *out, size_t cap,
                                 size_t *used)
{
    if ( (NULL == uri) || (NULL == out) || (NULL == used) )
    {
        return KODE_DENIED;
    }

    const char *rest = NULL;
    uint8_t     code = uri_split(uri, &rest);
    size_t      len  = strlen(rest);
    size_t      pay  = 1U + len;

    if ( SHORT_MAX < pay )
    {
        return KODE_NO_RESOURCES;
    }
    if ( (4U + pay) > cap )
    {
        return KODE_NO_RESOURCES;
    }

    out[0] = HDR_SR_WKT;
    out[1] = 1U;
    out[2] = (uint8_t)pay;
    out[3] = (uint8_t)TYPE_URI;
    out[4] = code;
    if ( 0U < len )
    {
        (void)memcpy(&out[5], rest, len);
    }
    *used = 5U + len;
    return KODE_OK;
}

kode_status_t knfc_ndef_make_text(const char *text, const char *lang,
                                  uint8_t *out, size_t cap, size_t *used)
{
    if ( (NULL == text) || (NULL == out) || (NULL == used) )
    {
        return KODE_DENIED;
    }
    if ( (NULL == lang) || ('\0' == lang[0]) )
    {
        lang = "en";
    }

    size_t lang_len = strlen(lang);
    size_t txt_len  = strlen(text);
    if ( LANG_MAX < lang_len )
    {
        return KODE_DENIED;
    }

    size_t pay = 1U + lang_len + txt_len;
    if ( SHORT_MAX < pay )
    {
        return KODE_NO_RESOURCES;
    }
    if ( (4U + pay) > cap )
    {
        return KODE_NO_RESOURCES;
    }

    size_t k = 0U;
    out[k] = HDR_SR_WKT;
    ++k;
    out[k] = 1U;
    ++k;
    out[k] = (uint8_t)pay;
    ++k;
    out[k] = (uint8_t)TYPE_TEXT;
    ++k;
    /* Bit 7 clear is UTF-8; the low six bits are the language's length. */
    out[k] = (uint8_t)(lang_len & TEXT_LANG_MASK);
    ++k;
    (void)memcpy(&out[k], lang, lang_len);
    k += lang_len;
    if ( 0U < txt_len )
    {
        (void)memcpy(&out[k], text, txt_len);
        k += txt_len;
    }
    *used = k;
    return KODE_OK;
}

kode_status_t knfc_ndef_wrap_tlv(const uint8_t *msg, size_t msg_len,
                                 uint8_t *out, size_t cap, size_t *used)
{
    if ( (NULL == msg) || (NULL == out) || (NULL == used) )
    {
        return KODE_DENIED;
    }
    if ( TLV_SHORT_MAX < msg_len )
    {
        return KODE_NO_RESOURCES;
    }
    if ( (msg_len + KNFC_TLV_WRAP_BYTES) > cap )
    {
        return KODE_NO_RESOURCES;
    }

    out[0] = (uint8_t)KNFC_TLV_NDEF;
    out[1] = (uint8_t)msg_len;
    (void)memcpy(&out[2], msg, msg_len);
    out[2U + msg_len] = (uint8_t)KNFC_TLV_TERMINATOR;
    *used = msg_len + KNFC_TLV_WRAP_BYTES;
    return KODE_OK;
}

kode_status_t knfc_ndef_find_tlv(const uint8_t *data, size_t len, size_t *off,
                                 size_t *msg_len)
{
    if ( (NULL == data) || (NULL == off) || (NULL == msg_len) )
    {
        return KODE_DENIED;
    }

    size_t i = 0U;
    while ( i < len )
    {
        uint8_t t = data[i];
        if ( TLV_NULL == t )
        {
            ++i;
            continue;
        }
        if ( (uint8_t)KNFC_TLV_TERMINATOR == t )
        {
            return KODE_ERROR;
        }
        if ( (i + 1U) >= len )
        {
            return KODE_ERROR;
        }

        size_t l   = data[i + 1U];
        size_t hdr = 2U;
        if ( TLV_LONG_LEN == l )
        {
            if ( (i + 3U) >= len )
            {
                return KODE_ERROR;
            }
            l   = ((size_t)data[i + 2U] << 8) | (size_t)data[i + 3U];
            hdr = 4U;
        }

        if ( (uint8_t)KNFC_TLV_NDEF == t )
        {
            i += hdr;
            if ( (0U == l) || (i >= len) )
            {
                return KODE_ERROR;
            }
            /* A tag that claims more than it holds is truncated, not a
             * reason to give up: what is there is still readable. */
            if ( (len - i) < l )
            {
                l = len - i;
            }
            *off     = i;
            *msg_len = l;
            return KODE_OK;
        }
        i += hdr + l;
    }
    return KODE_ERROR;
}

/**
 * @brief Copy a URI record's payload out, prefix expanded.
 *
 * @param pay     The payload: one prefix code, then the rest.
 * @param pay_len How long it is.
 * @param out     Out: the URI, NUL-terminated.
 * @param cap     How many bytes @p out holds.
 * @return KODE_OK, or KODE_NO_RESOURCES when it does not fit.
 */
static kode_status_t take_uri(const uint8_t *pay, size_t pay_len, char *out,
                              size_t cap)
{
    uint8_t     code = pay[0];
    const char *pre  = "";
    if ( (uint8_t)(sizeof(READ_PREFIXES) / sizeof(READ_PREFIXES[0])) > code )
    {
        pre = READ_PREFIXES[code];
    }

    size_t pre_len  = strlen(pre);
    size_t body_len = pay_len - 1U;
    if ( (pre_len + body_len + 1U) > cap )
    {
        return KODE_NO_RESOURCES;
    }
    (void)memcpy(out, pre, pre_len);
    if ( 0U < body_len )
    {
        (void)memcpy(&out[pre_len], &pay[1], body_len);
    }
    out[pre_len + body_len] = '\0';
    return KODE_OK;
}

/**
 * @brief Copy a Text record's payload out, language stripped.
 *
 * @param pay     The payload: a status byte, the language, the text.
 * @param pay_len How long it is.
 * @param out     Out: the text, NUL-terminated.
 * @param cap     How many bytes @p out holds.
 * @return KODE_OK, KODE_NO_RESOURCES when it does not fit, KODE_ERROR when
 *         the record is UTF-16 or its language length is a lie.
 */
static kode_status_t take_text(const uint8_t *pay, size_t pay_len, char *out,
                               size_t cap)
{
    uint8_t status = pay[0];
    size_t  lang   = (size_t)(status & TEXT_LANG_MASK);

    /* UTF-16 is stepped over rather than dumped: raw bytes would produce a
     * string that looks like a result and is not. */
    if ( (status & TEXT_UTF16) != 0U )
    {
        return KODE_ERROR;
    }
    if ( (lang + 1U) > pay_len )
    {
        return KODE_ERROR;
    }

    size_t body_len = pay_len - 1U - lang;
    if ( (body_len + 1U) > cap )
    {
        return KODE_NO_RESOURCES;
    }
    if ( 0U < body_len )
    {
        (void)memcpy(out, &pay[1U + lang], body_len);
    }
    out[body_len] = '\0';
    return KODE_OK;
}

kode_status_t knfc_ndef_parse(const uint8_t *data, size_t len, char *out,
                              size_t cap, knfc_ndef_kind_t *kind)
{
    if ( (NULL == data) || (NULL == out) || (0U == cap) )
    {
        return KODE_DENIED;
    }

    size_t        off = 0U;
    size_t        msg = 0U;
    kode_status_t rc  = knfc_ndef_find_tlv(data, len, &off, &msg);
    if ( KODE_OK != rc )
    {
        return rc;
    }

    size_t p   = off;
    size_t end = off + msg;
    while ( p < end )
    {
        uint8_t hdr = data[p];
        bool    sr  = (hdr & HDR_SR_BIT) != 0U;
        bool    il  = (hdr & HDR_IL_BIT) != 0U;
        size_t  q   = p + 1U;
        if ( q >= end )
        {
            break;
        }

        size_t type_len = data[q];
        ++q;

        size_t pay_len = 0U;
        if ( sr )
        {
            if ( q >= end )
            {
                break;
            }
            pay_len = data[q];
            ++q;
        }
        else
        {
            if ( (q + 3U) >= end )
            {
                break;
            }
            pay_len = ((size_t)data[q] << 24) | ((size_t)data[q + 1U] << 16)
                    | ((size_t)data[q + 2U] << 8) | (size_t)data[q + 3U];
            q += 4U;
        }

        size_t id_len = 0U;
        if ( il )
        {
            if ( q >= end )
            {
                break;
            }
            id_len = data[q];
            ++q;
        }
        if ( (q + type_len + id_len) > end )
        {
            break;
        }

        const uint8_t *type = &data[q];
        q += type_len + id_len;
        if ( (q + pay_len) > end )
        {
            pay_len = end - q;
        }

        if ( (1U == type_len) && (1U <= pay_len) && (TYPE_URI == type[0]) )
        {
            rc = take_uri(&data[q], pay_len, out, cap);
            if ( KODE_OK == rc )
            {
                if ( NULL != kind )
                {
                    *kind = KNFC_NDEF_URI;
                }
            }
            return rc;
        }

        if ( (1U == type_len) && (1U <= pay_len) && (TYPE_TEXT == type[0]) )
        {
            rc = take_text(&data[q], pay_len, out, cap);
            if ( KODE_OK == rc )
            {
                if ( NULL != kind )
                {
                    *kind = KNFC_NDEF_TEXT;
                }
                return KODE_OK;
            }
            if ( KODE_NO_RESOURCES == rc )
            {
                return rc;
            }
            /* KODE_ERROR here means "not one this can show" - keep looking. */
        }

        p = q + pay_len;
    }
    return KODE_ERROR;
}

bool knfc_ndef_looks_like_uri(const char *s)
{
    if ( NULL == s )
    {
        return false;
    }
    if ( '\0' == s[0] )
    {
        return false;
    }

    /* An explicit prefix settles it; there is nothing to guess. */
    const char *rest = NULL;
    if ( uri_split(s, &rest) != 0U )
    {
        return true;
    }
    if ( same_ci(s, "www.", 4U) )
    {
        return true;
    }

    /* A domain has no spaces in it. */
    if ( (strchr(s, ' ') != NULL) || (strchr(s, '\t') != NULL) )
    {
        return false;
    }

    /* Two or more letters must follow the LAST dot. The last rather than the
     * first, so that "kode.diy/v30jul" is still a link; counting only the
     * leading letters is what keeps "3.14" and "192.168.1.1" out. */
    const char *dot = strrchr(s, '.');
    if ( (NULL == dot) || (dot == s) )
    {
        return false;
    }

    int letters = 0;
    for ( const char *p = dot + 1; '\0' != *p; ++p )
    {
        bool lower = ('a' <= *p) && ('z' >= *p);
        bool upper = ('A' <= *p) && ('Z' >= *p);
        if ( !lower && !upper )
        {
            break;   /* The TLD ends at a slash, a query or a digit. */
        }
        ++letters;
    }
    return 2 <= letters;
}
