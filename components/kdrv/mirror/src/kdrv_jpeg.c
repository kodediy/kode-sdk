/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_jpeg.c
 * @brief The P4's JPEG codec.
 */
#include "kdrv_jpeg.h"

#include "driver/jpeg_encode.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "kode_log.h"

#define TAG "kdrv.jpeg"

/* A full frame at quality 90 measured 29 KB, and quality is an operator's
 * choice; 128 KB is four times the worst case anyone should ask for and
 * still a rounding error against 32 MB of PSRAM. */
#define JPEG_OUT_CAP    (128U * 1024U)

/* The engine's own deadline. A full frame takes 3 ms, so two seconds is not
 * a tuning parameter - it is the difference between "slow" and "the
 * peripheral is wedged", and the caller wants to hear about the second. */
#define JPEG_TIMEOUT_MS 2000

static jpeg_encoder_handle_t s_enc;
static uint8_t              *s_out;
static size_t                s_out_cap;
/* A try-lock: khal_mutex_t only blocks, and AO_Display must never wait out another encode. */
static SemaphoreHandle_t     s_busy;   /**< Try-lock.                       */

kode_status_t kdrv_jpeg_init(void)
{
    if ( NULL != s_enc )
    {
        return KODE_OK;
    }

    /* Guarded on s_busy rather than created unconditionally: a caller that
     * retries after the engine or the buffer failed to allocate would
     * otherwise get a second mutex every time, leaking the first's control
     * block. Created unlocked, which is what makes the first
     * kdrv_jpeg_encode()'s xSemaphoreTake() succeed without anyone having to
     * give it first. */
    if ( NULL == s_busy )
    {
        s_busy = xSemaphoreCreateMutex();
        if ( NULL == s_busy )
        {
            KLOG_E(TAG, "no memory for the encode guard");
            return KODE_NO_RESOURCES;
        }
    }

    jpeg_encode_engine_cfg_t const cfg = { .timeout_ms = JPEG_TIMEOUT_MS };
    if ( jpeg_new_encoder_engine(&cfg, &s_enc) != ESP_OK )
    {
        KLOG_E(TAG, "the JPEG engine would not allocate");
        s_enc = NULL;
        return KODE_NO_RESOURCES;
    }

    jpeg_encode_memory_alloc_cfg_t const mcfg = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    /* The engine refuses output that does not start on a cache line; its allocator ensures it. */
    s_out = (uint8_t *)jpeg_alloc_encoder_mem(JPEG_OUT_CAP, &mcfg, &s_out_cap);
    if ( NULL == s_out )
    {
        KLOG_E(TAG, "no room for a %u KB output buffer",
               (unsigned)(JPEG_OUT_CAP / 1024U));
        (void)jpeg_del_encoder_engine(s_enc);
        s_enc = NULL;
        return KODE_NO_RESOURCES;
    }
    return KODE_OK;
}

/**
 * @brief The actual encode, assuming the caller already holds s_busy.
 *
 * Split out of kdrv_jpeg_encode() so kdrv_jpeg_selftest() can call it while
 * keeping the guard held across its own read of the result - see that
 * function's comment for why the public entry point cannot be reused
 * as-is. Never called with s_enc/s_out NULL or with s_busy unheld; both
 * callers already answer for that.
 */
static kode_status_t jpeg_encode_locked(const uint8_t *rgb565, uint32_t w,
                                        uint32_t h, uint32_t quality,
                                        uint8_t **out, uint32_t *out_len)
{
    jpeg_encode_cfg_t const cfg = {
        .width         = w,
        .height        = h,
        .src_type      = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample    = JPEG_DOWN_SAMPLING_YUV420,
        .image_quality = quality,
    };
    uint32_t n = 0U;
    esp_err_t const err = jpeg_encoder_process(
        s_enc, &cfg, rgb565, (uint32_t)((size_t)w * h * 2U),
        s_out, (uint32_t)s_out_cap, &n);

    if ( ESP_OK != err )
    {
        KLOG_W(TAG, "encode refused: %s", esp_err_to_name(err));
        return (ESP_ERR_NO_MEM == err) ? KODE_NO_RESOURCES : KODE_ERROR;
    }

    *out     = s_out;
    *out_len = n;
    return KODE_OK;
}

kode_status_t kdrv_jpeg_encode(const uint8_t *rgb565, uint32_t w, uint32_t h,
                               uint32_t quality, uint8_t **out,
                               uint32_t *out_len)
{
    if ( (NULL == s_enc) || (NULL == s_out) )
    {
        return KODE_DENIED;
    }
    if ( (NULL == rgb565) || (NULL == out) || (NULL == out_len)
        || (0U == w) || (0U == h) || (0U == quality) || (100U < quality) )
    {
        return KODE_DENIED;
    }

    /* Zero timeout: a refusal, not a wait. One engine and one output buffer
     * mean a second caller in here at the same time would read s_out as the
     * first is still writing it. */
    if ( xSemaphoreTake(s_busy, 0U) != pdTRUE )
    {
        KLOG_W(TAG, "an encode is already running; refusing the second caller");
        return KODE_DENIED;
    }

    kode_status_t const st = jpeg_encode_locked(rgb565, w, h, quality,
                                                out, out_len);
    /* Given back here, only after out and out_len are both set - this
     * caller is the general-purpose one, whose contract (kdrv_jpeg.h) is
     * that s_out is borrowed only until the NEXT call, not any longer. A
     * caller that needs to read it more than that one instant longer -
     * kdrv_jpeg_selftest() - does not come through here; it holds s_busy
     * itself instead. */
    (void)xSemaphoreGive(s_busy);
    return st;
}

kode_status_t kdrv_jpeg_selftest(void)
{
    if ( NULL == s_enc )
    {
        return KODE_NO_RESOURCES;
    }

    /* Same zero-timeout try-lock kdrv_jpeg_encode() uses, taken here
     * directly rather than through that function: kdrv_jpeg_encode() gives
     * s_busy back before it returns, and s_out is borrowed only until the
     * NEXT encode call - not until this function is done reading it. This
     * selftest runs on AO_Autotest's thread (inline, TEST_SYNC) and AO_
     * Display on its own, and with two cores AO_Display can run between
     * that give and the marker check
     * below, take the lock itself for a live frame, and overwrite s_out
     * mid-read: a codec that is fine would then report itself broken. Held
     * across jpeg_encode_locked() AND the read that follows, this window
     * does not exist. */
    if ( xSemaphoreTake(s_busy, 0U) != pdTRUE )
    {
        KLOG_W(TAG, "an encode is already running; refusing the second caller");
        return KODE_DENIED;
    }

    /* A 16x16 mid-grey square: small enough to cost nothing, large enough to
     * be a whole MCU at YUV420, which is where a misconfigured engine would
     * fall over. */
    static uint16_t square[16 * 16];
    for ( uint16_t i = 0U; (uint16_t)(16 * 16) > i; ++i )
    {
        square[i] = 0x7BEFU;
    }

    uint8_t *out = NULL;
    uint32_t len = 0U;
    kode_status_t const st =
        jpeg_encode_locked((const uint8_t *)square, 16U, 16U, 80U, &out, &len);
    if ( KODE_OK != st )
    {
        (void)xSemaphoreGive(s_busy);
        return st;
    }
    /* SOI at the front, EOI at the back: the two markers that say the engine
     * produced a whole file rather than a truncated one. Read while s_busy
     * is still ours, so nothing else can have touched s_out since. */
    bool const whole = (4U <= len) && (0xFFU == out[0]) && (0xD8U == out[1])
                     && (0xFFU == out[len - 2U]) && (0xD9U == out[len - 1U]);
    (void)xSemaphoreGive(s_busy);
    return whole ? KODE_OK : KODE_ERROR;
}
