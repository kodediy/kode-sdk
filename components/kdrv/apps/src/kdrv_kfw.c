/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_kfw.c
 * @brief The Kode firmware container: what it is, whether to believe it, and its sink.
 */
#include "kdrv_kfw.h"
#include "kapp_desc.h"      /* kapp_image_chip(): a raw image's chip id */
#include "kapps_fmt.h"

#include <stddef.h>
#include <string.h>

#include "kode_log.h"
#include "khal_system.h"   /* khal_alloc_psram(), khal_free(): the block's buffer */

#include "esp_rom_crc.h"
#include "psa/crypto.h"

/* The inflater lives in the P4's ROM (tinfl_decompress at 0x4fc000f8, the
 * miniz group in esp32p4.rom.ld), so a compressed container costs this image
 * nothing but the dictionary it borrows from PSRAM while a transfer runs.
 * That is also why there is no KODEOS_FOTA_COMPRESS: a switch that saves no
 * flash is a second mechanism doing no work. */
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "miniz.h"

#define TAG            "kdrv.kfw"

_Static_assert(sizeof(kfw_hdr_t) == KFW_HDR_LEN, "the header must be 72 bytes");
_Static_assert(offsetof(kfw_hdr_t, sha256) == 20U, "sha256 sits at 0x14");
_Static_assert(offsetof(kfw_hdr_t, crc)    == 52U, "crc sits at 0x34");
_Static_assert(offsetof(kfw_hdr_t, name)   == 56U, "name sits at 0x38");

/* The bootloader reads the container's magic through its own copy of this
 * number; the two headers must agree or a mailbox image stops being seen. */
_Static_assert(KAPPS_KFW_MAGIC == KFW_MAGIC, "kapps_fmt.h and kdrv_kfw.h must name the same container magic");

/* ---- The buffer sink ----------------------------------------------------- */

/**
 * @brief Empty the buffer and zero its counts for a new image of @p img_len bytes.
 * @param ctx     A kfw_buf_ctx_t whose buf and cap the owner has set.
 * @param img_len Decompressed length, or 0 when the source could not say;
 *                end() checks the byte count against it when it is known.
 * @return KODE_OK; KODE_DENIED with no context, no buffer or a 0 cap.
 */
static kode_status_t buf_begin(void *ctx, uint32_t img_len)
{
    kfw_buf_ctx_t *c = (kfw_buf_ctx_t *)ctx;
    if ( (NULL == c) || (NULL == c->buf) || (0U == c->cap) )
    {
        return KODE_DENIED;
    }
    c->fill    = 0U;
    c->total   = 0U;
    c->img_len = img_len;
    c->full    = false;
    return KODE_OK;
}

/**
 * @brief Copy one chunk into the buffer, or refuse it whole when it does not fit.
 * @param ctx  The kfw_buf_ctx_t.
 * @param data The chunk.
 * @param len  Its length in bytes.
 * @return KODE_OK; KODE_DENIED with no context, buffer or data;
 *         KODE_NO_RESOURCES when the chunk does not fit: drain, then offer it again.
 */
static kode_status_t buf_write(void *ctx, const void *data, size_t len)
{
    kfw_buf_ctx_t *c = (kfw_buf_ctx_t *)ctx;
    if ( (NULL == c) || (NULL == c->buf) || (NULL == data) )
    {
        return KODE_DENIED;
    }
    if ( ((uint64_t)c->fill + (uint64_t)len) > (uint64_t)c->cap )
    {
        c->full = true;
        return KODE_NO_RESOURCES;   /* drain, then offer this chunk again */
    }
    (void)memcpy(&c->buf[c->fill], data, len);
    c->fill  += (uint32_t)len;
    c->total += (uint32_t)len;
    c->full   = (c->fill == c->cap);
    return KODE_OK;
}

/**
 * @brief Close the session, checking the bytes seen against begin()'s length when known.
 * @param ctx The kfw_buf_ctx_t.
 * @return KODE_OK; KODE_DENIED with no context; KODE_ERROR when a known
 *         length and the bytes seen differ.
 */
static kode_status_t buf_end(void *ctx)
{
    kfw_buf_ctx_t *c = (kfw_buf_ctx_t *)ctx;
    if ( NULL == c )
    {
        return KODE_DENIED;
    }
    if ( (0U != c->img_len) && (c->total != c->img_len) )
    {
        KLOG_E(TAG, "buffer sink saw %u bytes of a %u byte image",
               (unsigned)c->total, (unsigned)c->img_len);
        return KODE_ERROR;
    }
    return KODE_OK;
}

/**
 * @brief Empty the buffer and clear its full flag.
 * @param ctx The kfw_buf_ctx_t, or NULL.
 */
static void buf_abort(void *ctx)
{
    kfw_buf_ctx_t *c = (kfw_buf_ctx_t *)ctx;
    if ( NULL != c )
    {
        c->fill = 0U;
        c->full = false;
    }
}

static const kfw_sink_t BUF_SINK = { buf_begin, buf_write, buf_end, buf_abort };

const kfw_sink_t *kfw_buf_sink(void)
{
    return &BUF_SINK;
}

/* ---- Primitives ---------------------------------------------------------- */

uint32_t kfw_crc32(const void *data, size_t len)
{
    return esp_rom_crc32_le(UINT32_MAX, (const uint8_t *)data, len);
}

/* PSA Crypto rather than mbedtls/sha256.h: in IDF v6 mbedTLS moved under
 * tf-psa-crypto and its own sha256.h became private. psa_crypto_init() is
 * idempotent, so calling it here costs nothing and removes an ordering
 * dependency on whoever else in the image happens to use crypto. */
void kfw_sha256(const void *data, size_t len, uint8_t *out)
{
    size_t got = 0U;
    (void)psa_crypto_init();
    (void)psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)data, len,
                           out, (size_t)KFW_SHA_LEN, &got);
}

/* ---- The header ---------------------------------------------------------- */

/**
 * @brief Whether a header's flags are ones this firmware obeys.
 *
 * Nothing set is always fine; the app-data bit is fine on an app and on
 * nothing else, because the block describes an app and an OS slot or the
 * coprocessor has nowhere to keep it. Any other bit is a container this
 * firmware would half understand.
 *
 * @param h  The header as read.
 * @return true when the flags are legal for the header's target.
 */
static bool flags_known(const kfw_hdr_t *h)
{
    bool ok = (0U == h->flags);

    if ( (KFW_FLAG_APPDATA == h->flags)
        && ((uint8_t)KFW_TARGET_APP == h->target) )
    {
        ok = true;
    }
    return ok;
}

kode_status_t kfw_hdr_parse(const void *buf, size_t len, kfw_hdr_t *out)
{
    if ( (NULL == buf) || (NULL == out) || ((size_t)KFW_HDR_LEN > len) )
    {
        return KODE_ERROR;
    }

    kfw_hdr_t h;
    (void)memcpy(&h, buf, sizeof(h));

    if ( KFW_MAGIC != h.magic )
    {
        return KODE_ERROR;      /* not a container; the caller decides */
    }
    uint32_t const want = kfw_crc32(buf, offsetof(kfw_hdr_t, crc));
    if ( h.crc != want )
    {
        KLOG_E(TAG, "header crc is %08x, computed %08x",
               (unsigned)h.crc, (unsigned)want);
        return KODE_ERROR;
    }
    if ( ((uint16_t)KFW_HDR_VERSION != h.hdr_version)
        || ((uint16_t)KFW_HDR_LEN != h.hdr_len) )
    {
        KLOG_E(TAG, "header v%u len %u is not one this firmware reads",
               (unsigned)h.hdr_version, (unsigned)h.hdr_len);
        return KODE_ERROR;
    }
    if ( ((uint8_t)KFW_TARGET_COUNT <= h.target)
        || ((uint8_t)KFW_COMP_COUNT <= h.comp) || !flags_known(&h) )
    {
        KLOG_E(TAG, "target %u comp %u flags %u: not a header this firmware "
                    "knows how to obey", (unsigned)h.target, (unsigned)h.comp,
               (unsigned)h.flags);
        return KODE_ERROR;
    }
    if ( (0U == h.image_len) || (0U == h.payload_len) )
    {
        KLOG_E(TAG, "an image of %u bytes from a payload of %u",
               (unsigned)h.image_len, (unsigned)h.payload_len);
        return KODE_ERROR;
    }
    if ( ((uint8_t)KFW_COMP_NONE == h.comp) && (h.payload_len != h.image_len) )
    {
        KLOG_E(TAG, "uncompressed, but %u bytes claim to carry a %u byte image",
               (unsigned)h.payload_len, (unsigned)h.image_len);
        return KODE_ERROR;
    }
    if ( '\0' != h.name[KFW_NAME_MAX - 1U] )
    {
        KLOG_E(TAG, "the name is not terminated");
        return KODE_ERROR;
    }

    *out = h;
    return KODE_OK;
}

/* ---- The app-data block --------------------------------------------------- */

/**
 * @brief Whether a block's two lengths are the ones a block may carry.
 * @param h  The block's header.
 * @return true when json_len is 1..KFW_AD_JSON_MAX and icon_len is 0, KFW_AD_ICON_LEN
 *         or KFW_AD_ICON_LEN_HALF.
 */
static bool ad_sizes_ok(const kfw_ad_hdr_t *h)
{
    bool ok = true;

    if ( (0U == h->json_len) || ((uint16_t)KFW_AD_JSON_MAX < h->json_len) )
    {
        ok = false;
    }
    if ( (0U != h->icon_len) && ((uint16_t)KFW_AD_ICON_LEN != h->icon_len)
        && ((uint16_t)KFW_AD_ICON_LEN_HALF != h->icon_len) )
    {
        ok = false;
    }
    return ok;
}

kfw_ad_status_t kfw_ad_check(const kfw_ad_hdr_t *h, const uint8_t *body)
{
    kfw_ad_status_t st = KFW_AD_OK;

    if ( (NULL == h) || (KFW_AD_MAGIC != h->magic) )
    {
        st = KFW_AD_BAD_MAGIC;
    }
    else if ( !ad_sizes_ok(h) || (NULL == body) )
    {
        st = KFW_AD_BAD_SIZE;
    }
    else if ( kfw_crc32(body, (size_t)h->json_len + (size_t)h->icon_len)
               != h->crc )
    {
        st = KFW_AD_BAD_CRC;
    }
    else
    {
        kapp_data_t parsed;
        switch ( kapp_data_parse((const char *)body, (size_t)h->json_len,
                                &parsed) )
        {
            case KAPP_JSON_OK:
                break;
            case KAPP_JSON_REVISION:
                st = KFW_AD_BAD_REVISION;
                break;
            case KAPP_JSON_SIZE:
                st = KFW_AD_BAD_SIZE;
                break;
            default:
                st = KFW_AD_BAD_SYNTAX;
                break;
        }
    }
    return st;
}

const char *kfw_ad_status_name(kfw_ad_status_t st)
{
    const char *name = "syntax";

    switch ( st )
    {
        case KFW_AD_OK:           name = "ok";       break;
        case KFW_AD_BAD_MAGIC:    name = "magic";    break;
        case KFW_AD_BAD_SIZE:     name = "size";     break;
        case KFW_AD_BAD_CRC:      name = "crc";      break;
        case KFW_AD_BAD_SYNTAX:   name = "syntax";   break;
        case KFW_AD_BAD_REVISION: name = "revision"; break;
        default:                  break;
    }
    return name;
}

/* The block a session kept. It is not part of s_kfw because kfw_end() resets
 * the session before it returns, and the owner reads the block after that:
 * kfw_begin(), kfw_abort() and a failed kfw_end() free it, kfw_appdata_take()
 * moves it out. While a session runs, buf holds a checked block waiting for
 * the digest and kind is still NONE. */
static kfw_appdata_t s_ad = { KFW_AD_NONE, NULL, 0U, 0U };

/** @brief Free whatever block is held and forget it. */
static void ad_drop(void)
{
    if ( NULL != s_ad.buf )
    {
        khal_free(s_ad.buf);
    }
    s_ad.buf      = NULL;
    s_ad.json_len = 0U;
    s_ad.icon_len = 0U;
    s_ad.kind     = KFW_AD_NONE;
}

void kfw_appdata_take(kfw_appdata_t *out)
{
    if ( NULL != out )
    {
        *out = s_ad;
        s_ad.buf      = NULL;     /* ownership moved: nothing to free here */
        s_ad.json_len = 0U;
        s_ad.icon_len = 0U;
        s_ad.kind     = KFW_AD_NONE;
    }
}

void kfw_appdata_release(kfw_appdata_t *ad)
{
    if ( NULL != ad )
    {
        if ( NULL != ad->buf )
        {
            khal_free(ad->buf);
        }
        ad->buf      = NULL;
        ad->json_len = 0U;
        ad->icon_len = 0U;
    }
}

/* ---- The decoder --------------------------------------------------------- */

typedef enum {
    KFW_ST_IDLE = 0,
    KFW_ST_SNIFF,    /**< Fewer than KFW_HDR_LEN bytes seen, still deciding. */
    KFW_ST_RAW,      /**< A bare ESP image: pass it through untouched.       */
    KFW_ST_ADHDR,    /**< Header consumed; collecting the block's header.    */
    KFW_ST_ADBODY,   /**< Collecting (or stepping over) the block's bytes.   */
    KFW_ST_BODY,     /**< Header consumed; payload streaming to the sink.    */
    KFW_ST_DEAD      /**< Something was refused; swallow the rest quietly.   */
} kfw_state_t;

static struct {
    kfw_state_t             st;
    const kfw_sink_t       *sink;
    void                   *ctx;
    kfw_target_t            want;
    uint32_t                src_len;
    kfw_hdr_t               hdr;
    uint8_t                 hdr_buf[KFW_HDR_LEN];
    uint32_t                hdr_fill;
    uint32_t                produced;   /**< Image bytes given to the sink. */
    psa_hash_operation_t    sha;
    bool                    sha_open;
    kfw_hdr_cb_t            on_hdr;
    void                   *hdr_ctx;
    tinfl_decompressor     *inf;      /**< PSRAM; only for a deflate payload. */
    uint8_t                *dict;     /**< TINFL_LZ_DICT_SIZE ring buffer.    */
    size_t                  dict_ofs;
    uint8_t                 ad_hdr_buf[KFW_AD_HDR_LEN];   /**< The block's header as it arrives. */
    kfw_ad_hdr_t            ad_hdr;     /**< The block's header, once read.        */
    uint32_t                ad_fill;    /**< Block header or body bytes seen so far. */
    bool                    ad_skip;    /**< The block's bytes are stepped over, not kept. */
} s_kfw;

bool kfw_busy(void)
{
    return (KFW_ST_IDLE != s_kfw.st);
}

uint32_t kfw_produced(void)
{
    return s_kfw.produced;
}

/**
 * @brief Return the decoder to idle: abort its hash, free the inflater and dictionary.
 *
 * Neither the sink nor the app-data block is touched: kfw_end() and
 * kfw_abort() settle both before they call it.
 */
static void kfw_reset(void)
{
    if ( s_kfw.sha_open )
    {
        (void)psa_hash_abort(&s_kfw.sha);
        s_kfw.sha_open = false;
    }
    if ( NULL != s_kfw.inf )
    {
        heap_caps_free(s_kfw.inf);
        s_kfw.inf = NULL;
    }
    if ( NULL != s_kfw.dict )
    {
        heap_caps_free(s_kfw.dict);
        s_kfw.dict = NULL;
    }
    s_kfw.st       = KFW_ST_IDLE;
    s_kfw.sink     = NULL;
    s_kfw.ctx      = NULL;
    s_kfw.hdr_fill = 0U;
    s_kfw.produced = 0U;
    s_kfw.dict_ofs = 0U;
}

kode_status_t kfw_begin(const kfw_sink_t *sink, void *ctx, kfw_target_t want,
                        uint32_t src_len, kfw_hdr_cb_t on_hdr, void *hdr_ctx)
{
    if ( NULL == sink )
    {
        return KODE_DENIED;
    }
    if ( KFW_ST_IDLE != s_kfw.st )
    {
        KLOG_E(TAG, "a decode session is already open");
        return KODE_DENIED;
    }
    ad_drop();                  /* a block nobody took */
    (void)memset(&s_kfw, 0, sizeof(s_kfw));
    s_kfw.st      = KFW_ST_SNIFF;
    s_kfw.sink    = sink;
    s_kfw.ctx     = ctx;
    s_kfw.want    = want;
    s_kfw.src_len = src_len;
    s_kfw.on_hdr  = on_hdr;
    s_kfw.hdr_ctx = hdr_ctx;
    return KODE_OK;
}

/** @brief Hand @p len decoded image bytes to the sink, counting them. */
static kode_status_t emit(const uint8_t *data, size_t len)
{
    if ( (0U != s_kfw.hdr.image_len)
        && (((uint64_t)s_kfw.produced + (uint64_t)len)
            > (uint64_t)s_kfw.hdr.image_len) )
    {
        /* A payload that produces more than its header declared is corrupt at
         * best. Stop it before it runs past the sink's session, not after. */
        KLOG_E(TAG, "the payload produces more than the %u bytes it declared",
               (unsigned)s_kfw.hdr.image_len);
        return KODE_ERROR;
    }
    if ( s_kfw.sha_open )
    {
        (void)psa_hash_update(&s_kfw.sha, data, len);
    }
    kode_status_t const st = s_kfw.sink->write(s_kfw.ctx, data, len);
    if ( KODE_OK == st )
    {
        s_kfw.produced += (uint32_t)len;
    }
    return st;
}

/**
 * @brief Inflate @p len payload bytes, emitting whatever comes out.
 *
 * The dictionary doubles as the output ring buffer, which is what makes this
 * streaming: tinfl writes into it, we hand each produced run to the sink, and
 * nothing ever holds more than 32 KB of the image.
 */
static kode_status_t inflate_feed(const uint8_t *in, size_t len)
{
    for ( ;; )
    {
        size_t in_bytes  = len;
        size_t out_bytes = (size_t)TINFL_LZ_DICT_SIZE - s_kfw.dict_ofs;

        tinfl_status const ts = tinfl_decompress(
            s_kfw.inf, in, &in_bytes,
            s_kfw.dict, &s_kfw.dict[s_kfw.dict_ofs], &out_bytes,
            (mz_uint32)(TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_HAS_MORE_INPUT));

        if ( 0U < out_bytes )
        {
            kode_status_t const st = emit(&s_kfw.dict[s_kfw.dict_ofs], out_bytes);
            if ( KODE_OK != st )
            {
                return st;
            }
            s_kfw.dict_ofs = (s_kfw.dict_ofs + out_bytes)
                           & ((size_t)TINFL_LZ_DICT_SIZE - 1U);
        }

        in  += in_bytes;
        len -= in_bytes;

        if ( TINFL_STATUS_DONE == ts )
        {
            return KODE_OK;
        }
        if ( (int)ts < 0 )
        {
            KLOG_E(TAG, "the deflate payload is corrupt (tinfl %d)", (int)ts);
            return KODE_ERROR;
        }
        if ( (0U == len) && (0U == out_bytes) )
        {
            return KODE_OK;     /* needs more input: wait for the next chunk */
        }
    }
}

/**
 * @brief The chip a target's image must be built for: the P4 for kodeOS and
 *        its apps, the C5 for the coprocessor.
 *
 * @param t The target the session was opened for.
 * @return An esp_chip_id_t, as the image header stores it.
 */
static uint16_t target_chip(kfw_target_t t)
{
    uint16_t chip = (uint16_t)ESP_CHIP_ID_ESP32P4;

    if ( KFW_TARGET_C5 == t )
    {
        chip = (uint16_t)ESP_CHIP_ID_ESP32C5;
    }
    return chip;
}

kode_status_t kfw_feed(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    if ( (NULL == p) && (0U != len) )
    {
        return KODE_DENIED;
    }
    if ( KFW_ST_DEAD == s_kfw.st )
    {
        return KODE_ERROR;
    }
    if ( KFW_ST_IDLE == s_kfw.st )
    {
        return KODE_DENIED;
    }

    while ( 0U < len )
    {
        switch ( s_kfw.st )
        {
            case KFW_ST_SNIFF: {
                /* One byte settles the raw case, and it must, because a raw
                 * image has no length of its own to wait for. */
                if ( (0U == s_kfw.hdr_fill) && (KFW_ESP_MAGIC == p[0]) )
                {
                    /* A raw image names its chip in its header, and another
                     * chip's is refused here, before on_hdr places it:
                     * placing an app may evict others, and the
                     * esp_image_verify() at the end would only refuse the
                     * image after they were gone. A first chunk shorter than
                     * the header - no medium hands one over - leaves the
                     * chip to that verify. */
                    uint16_t chip = 0U;
                    if ( (kapp_image_chip(p, len, &chip) == KODE_OK)
                        && (target_chip(s_kfw.want) != chip) )
                    {
                        KLOG_E(TAG, "that image was built for chip id 0x%04x, "
                                    "not this target's 0x%04x", (unsigned)chip,
                               (unsigned)target_chip(s_kfw.want));
                        s_kfw.st = KFW_ST_DEAD;
                        return KODE_ERROR;
                    }
                    /* Its project name, when the first chunk holds IDF's
                     * descriptor (every medium's does): the app it replaces. */
                    char project[KAPP_IDF_NAME_MAX];
                    (void)kapp_project_name(p, len, project, sizeof(project));
                    if ( (NULL != s_kfw.on_hdr)
                        && (s_kfw.on_hdr(s_kfw.hdr_ctx, s_kfw.src_len, project)
                            != KODE_OK) )
                    {
                        s_kfw.st = KFW_ST_DEAD;
                        return KODE_ERROR;
                    }
                    if ( s_kfw.sink->begin(s_kfw.ctx, s_kfw.src_len) != KODE_OK )
                    {
                        s_kfw.st = KFW_ST_DEAD;
                        return KODE_ERROR;
                    }
                    s_kfw.st = KFW_ST_RAW;
                    break;      /* re-enter the loop in KFW_ST_RAW */
                }

                uint32_t const room = (uint32_t)KFW_HDR_LEN - s_kfw.hdr_fill;
                uint32_t const take = ((size_t)room > len) ? (uint32_t)len : room;
                (void)memcpy(&s_kfw.hdr_buf[s_kfw.hdr_fill], p, take);
                s_kfw.hdr_fill += take;
                p              += take;
                len            -= take;

                if ( (uint32_t)KFW_HDR_LEN > s_kfw.hdr_fill )
                {
                    return KODE_OK;     /* the header spans more chunks */
                }
                if ( kfw_hdr_parse(s_kfw.hdr_buf, KFW_HDR_LEN, &s_kfw.hdr)
                    != KODE_OK )
                {
                    KLOG_E(TAG, "this is neither a container nor an ESP image");
                    s_kfw.st = KFW_ST_DEAD;
                    return KODE_ERROR;
                }
                if ( (kfw_target_t)s_kfw.hdr.target != s_kfw.want )
                {
                    KLOG_E(TAG, "the container is for target %u, but %u was "
                                "asked for", (unsigned)s_kfw.hdr.target,
                           (unsigned)s_kfw.want);
                    s_kfw.st = KFW_ST_DEAD;
                    return KODE_ERROR;
                }
                if ( (uint8_t)KFW_COMP_DEFLATE == s_kfw.hdr.comp )
                {
                    s_kfw.inf = heap_caps_malloc(sizeof(tinfl_decompressor),
                                                 MALLOC_CAP_SPIRAM);
                    s_kfw.dict = heap_caps_malloc((size_t)TINFL_LZ_DICT_SIZE,
                                                  MALLOC_CAP_SPIRAM);
                    if ( (NULL == s_kfw.inf) || (NULL == s_kfw.dict) )
                    {
                        /* Free whichever half succeeded. kfw_reset() would
                         * do it, but it is only reached from kfw_end() and
                         * kfw_abort() - and KODE_NO_RESOURCES means "drain
                         * and retry" everywhere else in this file, so a
                         * caller following that reading never calls either
                         * and the surviving allocation is never freed. */
                        heap_caps_free(s_kfw.inf);
                        heap_caps_free(s_kfw.dict);
                        s_kfw.inf  = NULL;
                        s_kfw.dict = NULL;
                        KLOG_E(TAG, "no PSRAM for the inflate dictionary");
                        s_kfw.st = KFW_ST_DEAD;
                        return KODE_NO_RESOURCES;
                    }
                    tinfl_init(s_kfw.inf);
                    s_kfw.dict_ofs = 0U;
                }
                (void)psa_crypto_init();
                s_kfw.sha = (psa_hash_operation_t)PSA_HASH_OPERATION_INIT;
                if ( psa_hash_setup(&s_kfw.sha, PSA_ALG_SHA_256) != PSA_SUCCESS )
                {
                    KLOG_E(TAG, "cannot start a SHA-256");
                    s_kfw.st = KFW_ST_DEAD;
                    return KODE_ERROR;
                }
                s_kfw.sha_open = true;
                if ( (NULL != s_kfw.on_hdr)
                    && (s_kfw.on_hdr(s_kfw.hdr_ctx, s_kfw.hdr.image_len,
                                     s_kfw.hdr.name) != KODE_OK) )
                {
                    s_kfw.st = KFW_ST_DEAD;
                    return KODE_ERROR;
                }
                if ( s_kfw.sink->begin(s_kfw.ctx, s_kfw.hdr.image_len) != KODE_OK )
                {
                    s_kfw.st = KFW_ST_DEAD;
                    return KODE_ERROR;
                }
                if ( (s_kfw.hdr.flags & KFW_FLAG_APPDATA) != 0U )
                {
                    s_kfw.st      = KFW_ST_ADHDR;
                    s_kfw.ad_fill = 0U;
                }
                else
                {
                    s_kfw.st = KFW_ST_BODY;
                }
                break;
            }

            case KFW_ST_ADHDR: {
                uint32_t const room = (uint32_t)KFW_AD_HDR_LEN - s_kfw.ad_fill;
                uint32_t       take = room;
                if ( (size_t)room > len )
                {
                    take = (uint32_t)len;
                }
                (void)memcpy(&s_kfw.ad_hdr_buf[s_kfw.ad_fill], p, take);
                s_kfw.ad_fill += take;
                p             += take;
                len           -= take;

                if ( (uint32_t)KFW_AD_HDR_LEN > s_kfw.ad_fill )
                {
                    return KODE_OK;     /* the block's header spans more chunks */
                }
                (void)memcpy(&s_kfw.ad_hdr, s_kfw.ad_hdr_buf,
                             sizeof(s_kfw.ad_hdr));
                if ( KFW_AD_MAGIC != s_kfw.ad_hdr.magic )
                {
                    /* Without its lengths the block cannot be stepped over,
                     * and the payload starts after it: the stream cannot be
                     * followed any further. */
                    KLOG_E(TAG, "app data: %s - the payload cannot be found",
                           kfw_ad_status_name(KFW_AD_BAD_MAGIC));
                    s_kfw.st = KFW_ST_DEAD;
                    return KODE_ERROR;
                }
                s_kfw.ad_fill = 0U;
                s_kfw.ad_skip = true;
                if ( !ad_sizes_ok(&s_kfw.ad_hdr) )
                {
                    KLOG_W(TAG, "app data dropped: %s",
                           kfw_ad_status_name(KFW_AD_BAD_SIZE));
                }
                else
                {
                    /* The sizes were judged first, so this is the biggest
                     * request a block can make. */
                    s_ad.buf = (uint8_t *)khal_alloc_psram(
                        1U, (size_t)s_kfw.ad_hdr.json_len
                            + (size_t)s_kfw.ad_hdr.icon_len);
                    if ( NULL == s_ad.buf )
                    {
                        KLOG_W(TAG, "app data dropped: no PSRAM for it");
                    }
                    else
                    {
                        s_kfw.ad_skip = false;
                    }
                }
                s_kfw.st = KFW_ST_ADBODY;
                break;
            }

            case KFW_ST_ADBODY: {
                uint32_t const total = (uint32_t)s_kfw.ad_hdr.json_len
                                     + (uint32_t)s_kfw.ad_hdr.icon_len;
                uint32_t const room  = total - s_kfw.ad_fill;
                uint32_t       take  = room;
                if ( (size_t)room > len )
                {
                    take = (uint32_t)len;
                }

                if ( !s_kfw.ad_skip )
                {
                    (void)memcpy(&s_ad.buf[s_kfw.ad_fill], p, take);
                }
                s_kfw.ad_fill += take;
                p             += take;
                len           -= take;

                if ( s_kfw.ad_fill < total )
                {
                    return KODE_OK;     /* the block spans more chunks */
                }
                if ( !s_kfw.ad_skip )
                {
                    kfw_ad_status_t const ast = kfw_ad_check(&s_kfw.ad_hdr,
                                                             s_ad.buf);
                    if ( KFW_AD_OK != ast )
                    {
                        KLOG_W(TAG, "app data dropped: %s",
                               kfw_ad_status_name(ast));
                        ad_drop();
                    }
                    else
                    {
                        s_ad.json_len = s_kfw.ad_hdr.json_len;
                        s_ad.icon_len = s_kfw.ad_hdr.icon_len;
                    }
                }
                s_kfw.st = KFW_ST_BODY;
                break;
            }

            case KFW_ST_RAW:
            case KFW_ST_BODY: {
                kode_status_t const st = (NULL != s_kfw.inf)
                                       ? inflate_feed(p, len)
                                       : emit(p, len);
                if ( KODE_OK != st )
                {
                    if ( KODE_NO_RESOURCES != st )
                    {
                        s_kfw.st = KFW_ST_DEAD;
                    }
                    return st;      /* NO_RESOURCES: drain and offer again */
                }
                len = 0U;
                break;
            }

            default:
                return KODE_ERROR;
        }
    }
    return KODE_OK;
}

kode_status_t kfw_end(void)
{
    kode_status_t rc = KODE_OK;

    switch ( s_kfw.st )
    {
        case KFW_ST_RAW:
            rc = s_kfw.sink->end(s_kfw.ctx);
            break;

        case KFW_ST_BODY: {
            if ( s_kfw.produced != s_kfw.hdr.image_len )
            {
                KLOG_E(TAG, "the stream ended at %u of %u bytes",
                       (unsigned)s_kfw.produced, (unsigned)s_kfw.hdr.image_len);
                s_kfw.sink->abort(s_kfw.ctx);
                rc = KODE_ERROR;
                break;
            }
            uint8_t got[KFW_SHA_LEN];
            size_t  got_len = 0U;
            psa_status_t const hrc = psa_hash_finish(&s_kfw.sha, got,
                                                     sizeof(got), &got_len);
            s_kfw.sha_open = false;
            if ( (PSA_SUCCESS != hrc) || (sizeof(got) != got_len) )
            {
                /* Say which thing went wrong. The comparison below would run
                 * against indeterminate stack bytes and almost certainly
                 * fail - the safe direction, but it would blame the image,
                 * and whoever read that line would go looking at a file that
                 * is fine. */
                KLOG_E(TAG, "the SHA-256 did not finish; the image was not judged");
                s_kfw.sink->abort(s_kfw.ctx);
                rc = KODE_ERROR;
                break;
            }
            if ( memcmp(got, s_kfw.hdr.sha256, sizeof(got)) != 0 )
            {
                /* Before end(), never after: the destination is abandoned
                 * rather than committed, which is the whole reason nothing
                 * needed staging in the first place. */
                KLOG_E(TAG, "the image does not match the digest it carries");
                s_kfw.sink->abort(s_kfw.ctx);
                rc = KODE_ERROR;
                break;
            }
            rc = s_kfw.sink->end(s_kfw.ctx);
            break;
        }

        case KFW_ST_SNIFF:
            KLOG_E(TAG, "the stream ended inside the header");
            rc = KODE_ERROR;
            break;

        case KFW_ST_ADHDR:
        case KFW_ST_ADBODY:
            KLOG_E(TAG, "the stream ended inside the app data block");
            s_kfw.sink->abort(s_kfw.ctx);
            rc = KODE_ERROR;
            break;

        case KFW_ST_DEAD:
            rc = KODE_ERROR;
            break;

        default:
            return KODE_DENIED;
    }

    /* What the owner may take: only from an image that went in whole. A raw
     * image says so, so the folder is left alone; a container says block or
     * none by whether a checked block survived. Anything else frees it. */
    if ( KODE_OK != rc )
    {
        ad_drop();
    }
    else if ( KFW_ST_RAW == s_kfw.st )
    {
        ad_drop();
        s_ad.kind = KFW_AD_RAW;
    }
    else if ( NULL != s_ad.buf )
    {
        s_ad.kind = KFW_AD_BLOCK;
    }
    else
    {
        s_ad.kind = KFW_AD_NONE;
    }

    kfw_reset();
    return rc;
}

void kfw_abort(void)
{
    ad_drop();
    if ( (KFW_ST_IDLE != s_kfw.st) && (NULL != s_kfw.sink) )
    {
        s_kfw.sink->abort(s_kfw.ctx);
    }
    kfw_reset();
}

void kfw_abort_owned(const void *ctx)
{
    if ( (KFW_ST_IDLE != s_kfw.st) && (s_kfw.ctx == ctx) )
    {
        kfw_abort();
    }
}

/* ---- The self-test -------------------------------------------------------
 * Headers built byte by byte in RAM, so the arithmetic is visible, then
 * broken one field at a time with the CRC recomputed each time - so that
 * exactly one thing is wrong per case and a pass means what it says.
 */

/** @brief A counting sink, so the decoder can be exercised without flash. */
static uint32_t s_stub_len;
static uint8_t  s_stub_last;
static bool     s_stub_ended;

/**
 * @brief Zero the stub's count, last byte and ended flag.
 * @param ctx Unused.
 * @param n   Unused.
 * @return KODE_OK.
 */
static kode_status_t stub_begin(void *ctx, uint32_t n)
{
    (void)ctx; (void)n;
    s_stub_len = 0U; s_stub_last = 0U; s_stub_ended = false;
    return KODE_OK;
}
/**
 * @brief Count one chunk's bytes and keep its last byte.
 * @param ctx Unused.
 * @param d   The chunk.
 * @param n   Its length in bytes.
 * @return KODE_OK.
 */
static kode_status_t stub_write(void *ctx, const void *d, size_t n)
{
    (void)ctx;
    s_stub_len += (uint32_t)n;
    if ( 0U < n )
    {
        s_stub_last = ((const uint8_t *)d)[n - 1U];
    }
    return KODE_OK;
}
static kode_status_t stub_end(void *ctx)   { (void)ctx; s_stub_ended = true; return KODE_OK; }
static void          stub_abort(void *ctx) { (void)ctx; }

static const kfw_sink_t STUB_SINK = { stub_begin, stub_write, stub_end, stub_abort };

/** @brief Seal a 72-byte header buffer with the CRC the parser will check. */
static void hdr_seal(uint8_t *h)
{
    uint32_t const crc = kfw_crc32(h, offsetof(kfw_hdr_t, crc));
    (void)memcpy(&h[offsetof(kfw_hdr_t, crc)], &crc, sizeof(crc));
}

/** @brief Lay out a valid uncompressed header for an image of @p n bytes. */
static void hdr_build(uint8_t *h, uint8_t target, uint32_t n)
{
    (void)memset(h, 0, KFW_HDR_LEN);
    h[0] = 'K'; h[1] = 'F'; h[2] = 'W'; h[3] = '1';
    h[4] = (uint8_t)KFW_HDR_VERSION;
    h[6] = (uint8_t)KFW_HDR_LEN;
    h[8] = target;
    h[9] = (uint8_t)KFW_COMP_NONE;
    (void)memcpy(&h[offsetof(kfw_hdr_t, payload_len)], &n, sizeof(n));
    (void)memcpy(&h[offsetof(kfw_hdr_t, image_len)],   &n, sizeof(n));
    hdr_seal(h);
}

/* Generated on the host by tools/mkkfw.py's own zlib: a 1024-byte image and
 * the deflate stream it compresses to. Embedded because a deflate stream
 * cannot be built by hand in RAM the way a header can. */
#define KFW_TV_IMG_LEN   1021U
#define KFW_TV_COMP_LEN  286U
static const uint8_t KFW_TV_COMP[KFW_TV_COMP_LEN] = {
    0x78, 0xDA, 0x7B, 0xC9, 0xC8, 0xC4, 0xCC, 0xC2, 0xCA, 0xC6, 0xCE, 0xC1,
    0xC9, 0xC5, 0xCD, 0xC3, 0xCB, 0xC7, 0x2F, 0x20, 0x28, 0x24, 0x2C, 0x22,
    0x2A, 0x26, 0x2E, 0x21, 0x29, 0x25, 0x2D, 0x23, 0x2B, 0x27, 0xAF, 0xA0,
    0xA8, 0xA4, 0xAC, 0xA2, 0xAA, 0xA6, 0xAE, 0xA1, 0xA9, 0xA5, 0xAD, 0xA3,
    0xAB, 0xA7, 0x6F, 0x60, 0x68, 0x64, 0x6C, 0x62, 0x6A, 0x66, 0x6E, 0x61,
    0x69, 0x65, 0x6D, 0x63, 0x6B, 0x67, 0xEF, 0xE0, 0xE8, 0xE4, 0xEC, 0xE2,
    0xEA, 0xE6, 0xEE, 0xE1, 0xE9, 0xE5, 0xED, 0xE3, 0xEB, 0xE7, 0x1F, 0x10,
    0x18, 0x14, 0x1C, 0x12, 0x1A, 0x16, 0x1E, 0x11, 0x19, 0x15, 0x1D, 0x13,
    0x1B, 0x17, 0x9F, 0x90, 0x98, 0x94, 0x9C, 0x92, 0x9A, 0x96, 0x9E, 0x91,
    0x99, 0x95, 0x9D, 0x93, 0x9B, 0x97, 0x5F, 0x50, 0x58, 0x54, 0x5C, 0x52,
    0x5A, 0x56, 0x5E, 0x51, 0x59, 0x55, 0x5D, 0x53, 0x5B, 0x57, 0xDF, 0xD0,
    0xD8, 0xD4, 0xDC, 0xD2, 0xDA, 0xD6, 0xDE, 0xD1, 0xD9, 0xD5, 0xDD, 0xD3,
    0xDB, 0xD7, 0x3F, 0x61, 0xE2, 0xA4, 0xC9, 0x53, 0xA6, 0x4E, 0x9B, 0x3E,
    0x63, 0xE6, 0xAC, 0xD9, 0x73, 0xE6, 0xCE, 0x9B, 0xBF, 0x60, 0xE1, 0xA2,
    0xC5, 0x4B, 0x96, 0x2E, 0x5B, 0xBE, 0x62, 0xE5, 0xAA, 0xD5, 0x6B, 0xD6,
    0xAE, 0x5B, 0xBF, 0x61, 0xE3, 0xA6, 0xCD, 0x5B, 0xB6, 0x6E, 0xDB, 0xBE,
    0x63, 0xE7, 0xAE, 0xDD, 0x7B, 0xF6, 0xEE, 0xDB, 0x7F, 0xE0, 0xE0, 0xA1,
    0xC3, 0x47, 0x8E, 0x1E, 0x3B, 0x7E, 0xE2, 0xE4, 0xA9, 0xD3, 0x67, 0xCE,
    0x9E, 0x3B, 0x7F, 0xE1, 0xE2, 0xA5, 0xCB, 0x57, 0xAE, 0x5E, 0xBB, 0x7E,
    0xE3, 0xE6, 0xAD, 0xDB, 0x77, 0xEE, 0xDE, 0xBB, 0xFF, 0xE0, 0xE1, 0xA3,
    0xC7, 0x4F, 0x9E, 0x3E, 0x7B, 0xFE, 0xE2, 0xE5, 0xAB, 0xD7, 0x6F, 0xDE,
    0xBE, 0x7B, 0xFF, 0xE1, 0xE3, 0xA7, 0xCF, 0x5F, 0xBE, 0x7E, 0xFB, 0xFE,
    0xE3, 0xE7, 0xAF, 0xDF, 0x7F, 0xFE, 0xFE, 0xFB, 0x3F, 0xEA, 0xFD, 0x51,
    0xEF, 0x8F, 0x58, 0xEF, 0x03, 0x00, 0x89, 0x1A, 0xFE, 0xF9,
};

/**
 * @brief A deflated container, decoded one small chunk at a time.
 *
 * The image is rebuilt from the test vector rather than stored twice, so the
 * two halves cannot drift: whatever the host compressed must inflate back to
 * exactly this.
 */
static bool selftest_inflate(void)
{
    static uint8_t img[KFW_TV_IMG_LEN];
    uint8_t  hdr[KFW_HDR_LEN];
    uint32_t const ilen = (uint32_t)KFW_TV_IMG_LEN;
    uint32_t const clen = (uint32_t)KFW_TV_COMP_LEN;
    bool ok;

    img[0] = KFW_ESP_MAGIC;
    for ( uint32_t i = 1U; i < ilen; i++ )
    {
        img[i] = (uint8_t)(((i - 1U) % 255U) + 1U);
    }

    hdr_build(hdr, (uint8_t)KFW_TARGET_OS, ilen);
    hdr[9] = (uint8_t)KFW_COMP_DEFLATE;
    (void)memcpy(&hdr[offsetof(kfw_hdr_t, payload_len)], &clen, sizeof(clen));
    kfw_sha256(img, ilen, &hdr[offsetof(kfw_hdr_t, sha256)]);
    hdr_seal(hdr);

    ok = (kfw_begin(&STUB_SINK, NULL, KFW_TARGET_OS, 0U, NULL, NULL) == KODE_OK)
      && (kfw_feed(hdr, sizeof(hdr)) == KODE_OK);
    /* 7 bytes at a time: a size that divides neither the header nor the
     * payload, so every boundary case gets exercised on the way through. */
    for ( uint32_t i = 0U; ok && (i < clen); i += 7U )
    {
        uint32_t const n = ((clen - i) < 7U) ? (clen - i) : 7U;
        ok = ok && (kfw_feed(&KFW_TV_COMP[i], n) == KODE_OK);
    }
    ok = ok && (kfw_end() == KODE_OK) && s_stub_ended && (s_stub_len == ilen)
            && (s_stub_last == img[ilen - 1U]);

    /* A corrupted deflate stream must be refused, not decoded into noise. */
    static uint8_t bad[KFW_TV_COMP_LEN];
    (void)memcpy(bad, KFW_TV_COMP, clen);
    bad[clen / 2U] ^= 0xFFU;
    ok = ok && (kfw_begin(&STUB_SINK, NULL, KFW_TARGET_OS, 0U, NULL, NULL) == KODE_OK)
            && (kfw_feed(hdr, sizeof(hdr)) == KODE_OK);
    (void)kfw_feed(bad, clen);
    ok = ok && (kfw_end() == KODE_ERROR) && !s_stub_ended;

    return ok;
}

/** @brief The image a block's container carries in the self-test. */
#define KFW_ST_AD_IMG_LEN   16U

/** @brief The app.json a block carries in the self-test. */
#define KFW_ST_AD_JSON      "{\"kode\":\"1\"}"

/**
 * @brief A container with an app-data block, decoded, and what it hands over.
 *
 * The block has no icon, so the whole stream is a few hundred bytes of stack.
 * With @p bad_crc the block is sealed wrong: the image must still go in and
 * the owner must be told there is no block.
 *
 * @param bad_crc  Seal a CRC the block's bytes do not match.
 * @param want     The kind the owner must be told.
 * @return true when the session and the answer are what they should be.
 */
static bool selftest_block(bool bad_crc, kfw_ad_kind_t want)
{
    uint8_t       img[KFW_ST_AD_IMG_LEN];
    uint8_t       hdr[KFW_HDR_LEN];
    uint8_t       blk[KFW_AD_HDR_LEN + sizeof(KFW_ST_AD_JSON)];
    kfw_ad_hdr_t  ad;
    uint16_t const jlen  = (uint16_t)(sizeof(KFW_ST_AD_JSON) - 1U);
    uint16_t const flags = KFW_FLAG_APPDATA;
    kfw_appdata_t got;
    bool          ok;

    (void)memset(img, 0x5A, sizeof(img));
    hdr_build(hdr, (uint8_t)KFW_TARGET_APP, (uint32_t)sizeof(img));
    (void)memcpy(&hdr[offsetof(kfw_hdr_t, flags)], &flags, sizeof(flags));
    kfw_sha256(img, sizeof(img), &hdr[offsetof(kfw_hdr_t, sha256)]);
    hdr_seal(hdr);

    (void)memcpy(&blk[KFW_AD_HDR_LEN], KFW_ST_AD_JSON, jlen);
    ad.magic    = KFW_AD_MAGIC;
    ad.json_len = jlen;
    ad.icon_len = 0U;
    ad.crc      = kfw_crc32(&blk[KFW_AD_HDR_LEN], jlen);
    if ( bad_crc )
    {
        ad.crc ^= 1U;
    }
    (void)memcpy(blk, &ad, sizeof(ad));

    ok = (kfw_begin(&STUB_SINK, NULL, KFW_TARGET_APP, 0U, NULL, NULL) == KODE_OK)
      && (kfw_feed(hdr, sizeof(hdr)) == KODE_OK)
      && (kfw_feed(blk, (size_t)KFW_AD_HDR_LEN + jlen) == KODE_OK)
      && (kfw_feed(img, sizeof(img)) == KODE_OK)
      && (kfw_end() == KODE_OK) && s_stub_ended
      && ((uint32_t)sizeof(img) == s_stub_len);

    /* A feed that failed leaves the session open, and an open session
     * refuses every install after it until the next reboot. */
    if ( kfw_busy() )
    {
        kfw_abort();
    }

    kfw_appdata_take(&got);
    ok = ok && (got.kind == want);
    if ( KFW_AD_BLOCK == want )
    {
        ok = ok && (NULL != got.buf) && (got.json_len == jlen)
                && (memcmp(got.buf, KFW_ST_AD_JSON, jlen) == 0);
    }
    else
    {
        ok = ok && (NULL == got.buf);
    }
    kfw_appdata_release(&got);
    return ok;
}

/**
 * @brief What is left of the on-target selftest: the inflate round trip, and
 *        the app-data block.
 *
 * It used to check four things. Three of them - the header parser, the
 * streaming decoder and the buffer sink - are covered by the host suite now,
 * in 43 named cases that can fail one at a time and be checked by restoring
 * the defect they describe. Keeping a second, coarser copy of that on the
 * board bought nothing and cost a `bool ok` chain that reports which of
 * thirty conditions failed by not saying.
 *
 * This one cannot move. It carries a real 286-byte deflate stream produced by
 * tools/mkkfw.py's zlib, and inflating it needs a real tinfl_decompress: on
 * the P4 that comes from ROM, and on a host it does not exist at all. The
 * suite supplies a controllable stand-in instead, which makes the plumbing
 * around inflate assertable but leaves a true round trip out of reach.
 * Vendoring a decompressor into the tree to test a path the board already
 * exercises on every `test component=kfw` is the wrong trade.
 */
kode_status_t kfw_selftest(void)
{
    kode_status_t rc = KODE_ERROR;

    if ( selftest_inflate()
        && selftest_block(false, KFW_AD_BLOCK)
        && selftest_block(true, KFW_AD_NONE) )
    {
        rc = KODE_OK;
    }
    return rc;
}
