/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_kfw.h
 * @brief Where an image is written, behind one contract.
 */
#ifndef KDRV_KFW_H_
#define KDRV_KFW_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"
#include "kapp_data.h"   /* KAPP_DATA_JSON_MAX, KAPP_ICON_BYTES: the block's own limits */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- The sink ------------------------------------------------------------ */

/** @brief One destination for a decoded image. */
typedef struct {
    /**
     * @brief Open a session for an image of @p img_len bytes.
     * @param ctx      Whatever the sink's constructor said it needs.
     * @param img_len  Decompressed length, or 0 when the source could not
     *                 say. A sink that cannot work without it refuses here.
     * @return KODE_OK, or a reason.
     */
    kode_status_t (*begin)(void *ctx, uint32_t img_len);
    /** @brief Append one chunk. May block for its duration. */
    kode_status_t (*write)(void *ctx, const void *data, size_t len);
    /** @brief Close the session and verify what was written. */
    kode_status_t (*end)(void *ctx);
    /** @brief Abandon the session. Always safe, even before begin(). */
    void          (*abort)(void *ctx);
} kfw_sink_t;

/* ---- The buffer sink -----------------------------------------------------
 * For an owner that cannot be blocked: the decoder fills the buffer, the
 * owner drains it. Not a flash destination - an adapter between a
 * synchronous decoder and an asynchronous transport.
 */

/** @brief State for kfw_buf_sink(). The owner allocates and drains it. */
typedef struct {
    uint8_t *buf;       /**< Where decoded bytes accumulate.              */
    uint32_t cap;       /**< How many bytes @p buf holds.                 */
    uint32_t fill;      /**< How many are valid right now.                */
    uint32_t total;     /**< How many have passed through since begin().  */
    uint32_t img_len;   /**< What begin() was told, 0 when unknown.       */
    bool     full;      /**< Set when a write could not fit: drain first. */
} kfw_buf_ctx_t;

/**
 * @brief A sink that accumulates into a caller-owned buffer.
 *
 * write() refuses with KODE_NO_RESOURCES rather than overflowing when the
 * buffer is full, which is the signal for the owner to drain it and feed
 * the same chunk again. Refusing is the whole point: dropping bytes here
 * would corrupt an image that the digest would then correctly refuse, which
 * is an expensive way to discover a bug in this file.
 */
const kfw_sink_t *kfw_buf_sink(void);

/* ---- The container -------------------------------------------------------
 * 72 bytes, little-endian, and everything in it describes the DECOMPRESSED
 * image rather than the payload carrying it. The length, because the region
 * allocator needs it before the first byte is written and Content-Length
 * describes the wrong number. The digest, because the image is the thing
 * that has to be right - a digest of the payload would be satisfied by a
 * correct transfer of a corrupt file.
 */

#define KFW_MAGIC        0x3157464BU   /**< "KFW1" read little-endian.      */
#define KFW_ESP_MAGIC    0xE9U         /**< First byte of a raw ESP image.  */
#define KFW_HDR_VERSION  1U
#define KFW_HDR_LEN      72U
#define KFW_NAME_MAX     16U
#define KFW_SHA_LEN      32U

/** @brief Header flag bit 0: an app-data block follows the header (target app only). */
/* cppcheck-suppress misra-c2012-2.5 ; used by kdrv_kfw.c and kdrv_apps.c; the addon reads the header from a file that does not use it */
#define KFW_FLAG_APPDATA   0x0001U

/** @brief Which destination an image declares itself for. */
typedef enum {
    KFW_TARGET_OS = 0,   /**< The inactive kodeOS slot.  */
    KFW_TARGET_APP,      /**< The apps region.           */
    KFW_TARGET_C5,       /**< The coprocessor.           */
    KFW_TARGET_COUNT
} kfw_target_t;

/** @brief How the payload is encoded. */
typedef enum {
    KFW_COMP_NONE = 0,
    KFW_COMP_DEFLATE,
    KFW_COMP_COUNT
} kfw_comp_t;

/** @brief Where the bytes come from. Not a mirror of kfw_target_t. */
typedef enum {
    KFW_SRC_FS = 0,     /**< The internal FAT partition. */
    KFW_SRC_SD,         /**< The microSD card.           */
    KFW_SRC_HTTP,       /**< http:// or https://         */
    KFW_SRC_PENDING,    /**< The app_pending mailbox.    */
    KFW_SRC_SERIAL,     /**< The console wire.           */
    KFW_SRC_COUNT
} kfw_src_t;

/** @brief The container header, exactly as it sits in the stream. */
typedef struct {
    uint32_t magic;
    uint16_t hdr_version;
    uint16_t hdr_len;
    uint8_t  target;
    uint8_t  comp;
    uint16_t flags;
    uint32_t payload_len;             /**< Bytes following the header.      */
    uint32_t image_len;               /**< Bytes after decompression.       */
    uint8_t  sha256[KFW_SHA_LEN];     /**< Of the decompressed image.       */
    uint32_t crc;                     /**< CRC32 of the 52 bytes before it. */
    char     name[KFW_NAME_MAX];
} kfw_hdr_t;

/* ---- The app-data block ---------------------------------------------------
 * With KFW_FLAG_APPDATA a container for an app carries the app's app.json and
 * its icon between the header and the payload, uncompressed:
 *
 *   0x00  4   magic "KAD1"
 *   0x04  2   json_len   1..KFW_AD_JSON_MAX
 *   0x06  2   icon_len   0, KFW_AD_ICON_LEN or KFW_AD_ICON_LEN_HALF
 *   0x08  4   CRC32 (kfw_crc32) of the json_len + icon_len bytes that follow
 *   0x0C      app.json, then icon.k64
 *
 * payload_len does not count the block, which states its own length. The
 * decoder checks the block and keeps it for the session's owner; it never
 * writes it anywhere.
 */

/* cppcheck-suppress misra-c2012-2.5 ; used by kdrv_kfw.c and kdrv_apps.c; the addon reads the header from a file that does not use it */
#define KFW_AD_MAGIC       0x3144414BU   /**< "KAD1" read little-endian.            */
#define KFW_AD_HDR_LEN     12U           /**< The block's own header.               */
#define KFW_AD_OFS_MAGIC   0U            /**< Offset of the magic in the block header.    */
#define KFW_AD_OFS_JSON_LEN 4U           /**< Offset of json_len in the block header.     */
#define KFW_AD_OFS_ICON_LEN 6U           /**< Offset of icon_len in the block header.     */
#define KFW_AD_OFS_CRC     8U            /**< Offset of the CRC in the block header.      */
/* cppcheck-suppress misra-c2012-2.5 ; used by kdrv_kfw.c; the addon reads the header from a file that does not use it */
#define KFW_AD_JSON_MAX    KAPP_DATA_JSON_MAX   /**< Longest app.json a block carries. */
/* cppcheck-suppress misra-c2012-2.5 ; used by kdrv_kfw.c; the addon reads the header from a file that does not use it */
#define KFW_AD_ICON_LEN    KAPP_ICON_BYTES      /**< The icon the board holds: 128x128, RGB565 + A8. */
/* cppcheck-suppress misra-c2012-2.5 ; used by kdrv_kfw.c; the addon reads the header from a file that does not use it */
#define KFW_AD_ICON_LEN_HALF KAPP_ICON_HALF_BYTES  /**< The smaller icon a block may carry: 64x64, RGB565 + A8. */

/** @brief The block's header, exactly as it sits in the stream. */
typedef struct {
    uint32_t magic;       /**< KFW_AD_MAGIC.                                    */
    uint16_t json_len;    /**< Bytes of app.json: 1..KFW_AD_JSON_MAX.           */
    uint16_t icon_len;    /**< Bytes of icon.k64: 0, KFW_AD_ICON_LEN or KFW_AD_ICON_LEN_HALF. */
    uint32_t crc;         /**< CRC32 of the json_len + icon_len bytes after it. */
} kfw_ad_hdr_t;

_Static_assert(sizeof(kfw_ad_hdr_t) == KFW_AD_HDR_LEN,
               "the app-data block's header must be 12 bytes");
_Static_assert(offsetof(kfw_ad_hdr_t, magic) == KFW_AD_OFS_MAGIC,
               "the block's magic sits at 0x00");
_Static_assert(offsetof(kfw_ad_hdr_t, json_len) == KFW_AD_OFS_JSON_LEN,
               "the block's json_len sits at 0x04");
_Static_assert(offsetof(kfw_ad_hdr_t, icon_len) == KFW_AD_OFS_ICON_LEN,
               "the block's icon_len sits at 0x06");
_Static_assert(offsetof(kfw_ad_hdr_t, crc) == KFW_AD_OFS_CRC,
               "the block's crc sits at 0x08");

/** @brief What kfw_ad_check() found. */
typedef enum {
    KFW_AD_OK = 0,        /**< Every check passed.                              */
    KFW_AD_BAD_MAGIC,     /**< Not "KAD1".                                      */
    KFW_AD_BAD_SIZE,      /**< A length outside what a block may carry.         */
    KFW_AD_BAD_CRC,       /**< The bytes do not match the block's CRC.          */
    KFW_AD_BAD_SYNTAX,    /**< app.json is outside the subset kapp_data reads.  */
    KFW_AD_BAD_REVISION   /**< app.json's `kode` is missing or not "1".         */
} kfw_ad_status_t;

/** @brief What a finished session has for the folder. */
typedef enum {
    KFW_AD_RAW = 0,       /**< A bare image: the folder is left as it is.       */
    KFW_AD_NONE,          /**< A container with no block, or a dropped one: the folder goes. */
    KFW_AD_BLOCK          /**< A checked block: the folder becomes this.        */
} kfw_ad_kind_t;

/** @brief The block a session kept, and who owns it now. */
typedef struct {
    kfw_ad_kind_t kind;       /**< Which of the three answers this is.          */
    uint8_t      *buf;        /**< PSRAM: json_len bytes of app.json, then icon_len of icon.k64; NULL unless BLOCK. */
    uint16_t      json_len;   /**< Bytes of app.json at the head of @p buf.     */
    uint16_t      icon_len;   /**< Bytes of icon.k64 after it: 0, KFW_AD_ICON_LEN or KFW_AD_ICON_LEN_HALF. */
} kfw_appdata_t;

/**
 * @brief Judge one block: magic, lengths, CRC, then app.json itself.
 * @param h     The block's header.
 * @param body  h->json_len bytes of app.json, then h->icon_len of icon.k64.
 *              Read only after the lengths pass.
 * @return KFW_AD_OK, or the first check that failed, in the order above.
 */
kfw_ad_status_t kfw_ad_check(const kfw_ad_hdr_t *h, const uint8_t *body);

/**
 * @brief The one-word name of a status, for the log.
 * @param st  A status.
 * @return "ok", "magic", "size", "crc", "syntax" or "revision".
 */
const char *kfw_ad_status_name(kfw_ad_status_t st);

/**
 * @brief Take the session's block, once kfw_end() has answered KODE_OK.
 *
 * Ownership of the PSRAM buffer moves to the caller, who gives it back with
 * kfw_appdata_release(). Called again, or after a session that failed, it
 * answers KFW_AD_NONE with no buffer.
 *
 * @param[out] out  The answer.
 */
void kfw_appdata_take(kfw_appdata_t *out);

/**
 * @brief Free a block taken with kfw_appdata_take().
 * @param ad  The block; its buf becomes NULL and its kind stays.
 */
void kfw_appdata_release(kfw_appdata_t *ad);

/**
 * @brief Parse and validate a container header.
 * @param buf  At least KFW_HDR_LEN bytes from the head of a stream.
 * @param len  How many bytes @p buf holds.
 * @param[out] out  Filled only on KODE_OK.
 * @return KODE_OK, or KODE_ERROR with the reason logged.
 */
kode_status_t kfw_hdr_parse(const void *buf, size_t len, kfw_hdr_t *out);

/** @brief SHA-256 of @p data into the KFW_SHA_LEN bytes at @p out. */
void kfw_sha256(const void *data, size_t len, uint8_t *out);

/** @brief CRC32 over @p len bytes - the one the header seals itself with. */
uint32_t kfw_crc32(const void *data, size_t len);

/* ---- The decoder ---------------------------------------------------------
 * One session at a time, deliberately: the only callers are owner AOs
 * writing to a device they own, and a second concurrent session would mean
 * a second writer.
 */

/**
 * @brief Called once the image length is known, before the sink is opened.
 * @param ctx      Whatever was handed to kfw_begin().
 * @param img_len  The decompressed length, from the header or the source.
 * @param name     The container's name; for a raw image the project name in
 *                 its first chunk, or "" when that chunk is too short to hold
 *                 it. The app the image replaces, when it is installed.
 * @return KODE_OK to proceed.
 */
typedef kode_status_t (*kfw_hdr_cb_t)(void *ctx, uint32_t img_len,
                                      const char *name);

/**
 * @brief Open a decode session.
 * @param sink     Where the decoded image goes.
 * @param ctx      Passed to every sink call.
 * @param want     The destination the operator asked for. A container
 *                 declaring a different one is refused the moment its header
 *                 completes: the command line decides where an image goes,
 *                 and the file only agrees or does not.
 * @param src_len  Total bytes the source will deliver, or 0 when unknown.
 *                 Only used for a raw image, which has no header to ask.
 * @param on_hdr   Called once the image's true length is known and BEFORE
 *                 the sink is opened, so a caller that must place the image
 *                 first - the apps region and its allocator - gets to. May
 *                 be NULL. Returning anything but KODE_OK aborts the
 *                 transfer before a byte is written.
 * @param hdr_ctx  Passed to @p on_hdr.
 * @return KODE_OK, or KODE_DENIED when a session is already open.
 */
kode_status_t kfw_begin(const kfw_sink_t *sink, void *ctx, kfw_target_t want,
                        uint32_t src_len, kfw_hdr_cb_t on_hdr, void *hdr_ctx);

/** @brief Push the next chunk of the source stream. */
kode_status_t kfw_feed(const void *data, size_t len);

/** @brief Close: verify the digest, then close the sink. */
kode_status_t kfw_end(void);

/**
 * @brief Abandon the session and the sink, whoever opened it. Safe at any
 *        point.
 *
 * For a caller that owns the decoder outright - the host tests, between
 * cases. An AO ends only its own session, with kfw_abort_owned().
 */
void kfw_abort(void);

/**
 * @brief Abandon the session only if @p ctx opened it. Safe at any point.
 *
 * AO_AppMgr and AO_Radio both open sessions on the one decoder. Either one's
 * cleanup may run while the other holds the decoder - a failed kfw_begin()
 * is one such moment - and must not end the other's session under it.
 *
 * @param ctx The ctx handed to kfw_begin().
 */
void kfw_abort_owned(const void *ctx);

/** @brief Whether a session is open right now. */
bool kfw_busy(void);

/**
 * @brief Image bytes handed to the sink so far.
 *
 * Not the same as the bytes fed in: a compressed container produces more
 * than it consumes, and every progress report and end-of-stream check has
 * to count the image rather than what carried it.
 */
uint32_t kfw_produced(void);

/**
 * @brief Parse, refuse and decode, checked by hand: the console's `test kfw`.
 *
 * It lives here rather than in the console for the reason
 * kdrv_storage_selftest() does: building a valid header means computing the
 * CRC, and esp_rom_crc.h stops at this layer.
 */
kode_status_t kfw_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_KFW_H_ */
