/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_image_check.c
 * @brief An ESP image judged as its bytes arrive: the header chain against the file's length, then the SHA-256 it carries.
 */
#include "kdrv_image_check.h"

#include <stdbool.h>
#include <string.h>

#include "esp_image_format.h"
#include "psa/crypto.h"

#include "kode_log.h"

#define TAG "kdrv.imgchk"   /**< This file's log tag. */

#define CHK_HDR_LEN     ((uint32_t)sizeof(esp_image_header_t))          /**< Bytes of the image header.   */
#define CHK_SEG_LEN     ((uint32_t)sizeof(esp_image_segment_header_t))  /**< Bytes of a segment header.   */
#define CHK_PAD_MASK    15U   /**< The checksum byte pads the image out to 16 bytes, less one. */
#define CHK_HASH_LEN    32U   /**< Bytes of the SHA-256 an image may carry after its checksum. */

/** @brief Where the check is in the image. */
typedef enum {
    CHK_IDLE = 0,   /**< No check open.                                      */
    CHK_HEADER,     /**< Collecting the image header.                        */
    CHK_SEG_HDR,    /**< Collecting a segment header.                        */
    CHK_SEG_DATA,   /**< Inside a segment's data.                            */
    CHK_PAD,        /**< In the padding that ends with the checksum byte.    */
    CHK_HASH,       /**< Collecting the SHA-256 the image carries.           */
    CHK_DONE,       /**< The image is whole; what follows is not its.        */
    CHK_BAD         /**< It cannot be a whole image.                         */
} chk_state_t;

/** @brief The check in hand: one at a time, so one set of statics. */
typedef struct {
    chk_state_t          st;        /**< Where it is.                                  */
    uint32_t             limit;     /**< The file's length.                            */
    uint32_t             taken;     /**< Bytes of the image taken so far.              */
    uint32_t             left;      /**< Bytes the part in hand still needs.           */
    uint8_t              segs;      /**< Segments still to come, the one in hand not counted. */
    bool                 carries;   /**< The image carries a SHA-256.                  */
    bool                 hashing;   /**< sha is open.                                  */
    uint8_t              fill;      /**< Bytes of head[] in hand.                      */
    uint8_t              head[sizeof(esp_image_header_t)];   /**< The header being collected. */
    uint8_t              want[CHK_HASH_LEN];   /**< The SHA-256 the image carries.     */
    uint32_t             want_fill; /**< Bytes of want[] in hand.                      */
    psa_hash_operation_t sha;       /**< Over every byte before the hash.              */
} chk_t;

static chk_t s_chk;   /**< The check in hand. */

/** @brief Spend the check: it cannot be a whole image. */
static void chk_bad(const char *why)
{
    KLOG_E(TAG, "the file is not a whole image: %s", why);
    if ( s_chk.hashing )
    {
        (void)psa_hash_abort(&s_chk.sha);
        s_chk.hashing = false;
    }
    s_chk.st = CHK_BAD;
}

/** @brief Whether @p more more bytes of the image would still lie inside the file. */
static bool chk_room(uint32_t more)
{
    return (s_chk.taken <= s_chk.limit) && (more <= (s_chk.limit - s_chk.taken));
}

/** @brief The image header is in: read it and start on the first segment. */
static void chk_header_done(void)
{
    esp_image_header_t header;

    (void)memcpy(&header, s_chk.head, sizeof(header));
    if ( (ESP_IMAGE_HEADER_MAGIC != header.magic)
        || (0U == header.segment_count)
        || (ESP_IMAGE_MAX_SEGMENTS < header.segment_count) )
    {
        chk_bad("bad magic or segment count");
        return;
    }
    s_chk.carries = (0U != header.hash_appended);
    if ( s_chk.carries )
    {
        (void)psa_crypto_init();
        s_chk.sha = (psa_hash_operation_t)PSA_HASH_OPERATION_INIT;
        if ( (psa_hash_setup(&s_chk.sha, PSA_ALG_SHA_256) != PSA_SUCCESS)
            || (psa_hash_update(&s_chk.sha, s_chk.head, sizeof(header)) != PSA_SUCCESS) )
        {
            chk_bad("the hash would not start");
            return;
        }
        s_chk.hashing = true;
    }
    s_chk.segs = header.segment_count;
    s_chk.fill = 0U;
    s_chk.left = CHK_SEG_LEN;
    s_chk.st   = CHK_SEG_HDR;
}

/** @brief A segment header is in: the segment's data is next, if it fits the file. */
static void chk_seg_hdr_done(void)
{
    esp_image_segment_header_t seg;

    (void)memcpy(&seg, s_chk.head, sizeof(seg));
    if ( s_chk.hashing
        && (psa_hash_update(&s_chk.sha, s_chk.head, sizeof(seg)) != PSA_SUCCESS) )
    {
        chk_bad("the hash would not take a segment header");
        return;
    }
    if ( !chk_room(seg.data_len) )
    {
        chk_bad("a segment leaves the file");
        return;
    }
    --s_chk.segs;
    s_chk.fill = 0U;
    s_chk.left = seg.data_len;
    s_chk.st   = CHK_SEG_DATA;
}

/** @brief The part in hand is whole: go on to the next. */
static void chk_advance(void)
{
    switch ( s_chk.st )
    {
        case CHK_HEADER:
        {
            chk_header_done();
            break;
        }
        case CHK_SEG_HDR:
        {
            chk_seg_hdr_done();
            break;
        }
        case CHK_SEG_DATA:
        {
            if ( 0U < s_chk.segs )
            {
                s_chk.fill = 0U;
                s_chk.left = CHK_SEG_LEN;
                s_chk.st   = CHK_SEG_HDR;
            }
            else
            {
                s_chk.left = ((s_chk.taken | CHK_PAD_MASK) + 1U) - s_chk.taken;
                s_chk.st   = CHK_PAD;
                if ( !chk_room(s_chk.left) )
                {
                    chk_bad("the checksum leaves the file");
                }
            }
            break;
        }
        case CHK_PAD:
        {
            if ( s_chk.carries )
            {
                s_chk.left      = CHK_HASH_LEN;
                s_chk.want_fill = 0U;
                s_chk.st        = CHK_HASH;
                if ( !chk_room(s_chk.left) )
                {
                    chk_bad("the hash leaves the file");
                }
            }
            else
            {
                s_chk.st = CHK_DONE;
            }
            break;
        }
        case CHK_HASH:
        {
            uint8_t got[CHK_HASH_LEN];
            size_t  got_len = 0U;

            if ( (psa_hash_finish(&s_chk.sha, got, sizeof(got), &got_len) != PSA_SUCCESS)
                || (sizeof(got) != got_len) )
            {
                s_chk.hashing = false;
                chk_bad("the hash would not finish");
                break;
            }
            s_chk.hashing = false;
            if ( memcmp(got, s_chk.want, sizeof(got)) != 0 )
            {
                chk_bad("its SHA-256 does not match");
            }
            else
            {
                s_chk.st = CHK_DONE;
            }
            break;
        }
        default:
        {
            break;
        }
    }
}

/**
 * @brief Take @p n bytes of the part in hand.
 * @param p The bytes; @p n of them lie in the part.
 * @param n How many.
 */
static void chk_take(const uint8_t *p, uint32_t n)
{
    switch ( s_chk.st )
    {
        case CHK_HEADER:
        case CHK_SEG_HDR:
        {
            (void)memcpy(&s_chk.head[s_chk.fill], p, n);
            s_chk.fill = (uint8_t)(s_chk.fill + n);
            break;
        }
        case CHK_SEG_DATA:
        case CHK_PAD:
        {
            if ( s_chk.hashing && (psa_hash_update(&s_chk.sha, p, n) != PSA_SUCCESS) )
            {
                chk_bad("the hash would not take the data");
            }
            break;
        }
        case CHK_HASH:
        {
            (void)memcpy(&s_chk.want[s_chk.want_fill], p, n);
            s_chk.want_fill += n;
            break;
        }
        default:
        {
            break;
        }
    }
    s_chk.taken += n;
    s_chk.left  -= n;
}

kode_status_t kdrv_image_check_begin(uint32_t file_len)
{
    if ( CHK_IDLE != s_chk.st )
    {
        return KODE_DENIED;
    }
    (void)memset(&s_chk, 0, sizeof(s_chk));
    s_chk.limit = file_len;
    s_chk.left  = CHK_HDR_LEN;
    s_chk.st    = CHK_HEADER;
    return KODE_OK;
}

kode_status_t kdrv_image_check_feed(const void *data, size_t len)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the bytes of a buffer a caller lent as a void * */
    const uint8_t *p = (const uint8_t *)data;
    size_t         n = len;

    if ( (CHK_IDLE == s_chk.st) || (NULL == data) )
    {
        return KODE_DENIED;
    }
    while ( (CHK_BAD != s_chk.st) && (CHK_DONE != s_chk.st) )
    {
        if ( 0U == s_chk.left )
        {
            chk_advance();
        }
        else if ( 0U < n )
        {
            uint32_t take = s_chk.left;

            if ( (size_t)take > n )
            {
                take = (uint32_t)n;
            }
            chk_take(p, take);
            p = &p[take];
            n -= take;
        }
        else
        {
            break;
        }
    }
    if ( CHK_BAD == s_chk.st )
    {
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t kdrv_image_check_end(void)
{
    kode_status_t rc = KODE_OK;

    if ( CHK_IDLE == s_chk.st )
    {
        return KODE_DENIED;
    }
    if ( CHK_DONE != s_chk.st )
    {
        if ( CHK_BAD != s_chk.st )
        {
            chk_bad("the file ends inside its image");
        }
        rc = KODE_ERROR;
    }
    kdrv_image_check_abort();
    return rc;
}

void kdrv_image_check_abort(void)
{
    if ( s_chk.hashing )
    {
        (void)psa_hash_abort(&s_chk.sha);
    }
    (void)memset(&s_chk, 0, sizeof(s_chk));
}
