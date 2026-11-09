/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kapp_desc.h
 * @brief What an app says about itself, in its own image.
 */
#ifndef KAPP_DESC_H_
#define KAPP_DESC_H_

#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"   /* kode_status_t */
#include "kapp_cat.h"    /* KAPP_CATEGORY_MAX: the field's size; KAPP_CATEGORY_ITEM_MAX: v1's */

#ifdef __cplusplus
extern "C" {
#endif

#define KAPP_DESC_MAGIC     0x4450414BU   /**< "KAPD".                        */
#define KAPP_DESC_VERSION   2U            /**< Of this struct, not of the app. */
/* Twinned in kqp_config.h as KODE_APP_DISPLAY_MAX, because kqp sits below
 * kdrv and its events carry this string. Change one, change the other. */
#define KAPP_NAME_MAX       32U           /**< Display name, terminator included. */

/**
 * @brief Size of a version-1 manifest, which had a 16-byte category.
 *
 * The reader bounds a v1 image by THIS, never by sizeof(kapp_desc_t): a v1
 * image has nothing meaningful in the 48 bytes a v2 reader would otherwise
 * walk into. Every app installed before revision 2 is a v1 image.
 */
#define KAPP_DESC_V1_SIZE   88U

/** @brief Image offset of IDF's own app descriptor (esp_app_desc_t). */
#define KAPP_IDF_DESC_OFF   0x20U

/** @brief Image offset of the manifest: 0x20 + sizeof(esp_app_desc_t). */
#define KAPP_DESC_OFF       0x120U

/** @brief Bytes of esp_app_desc_t.project_name, terminator included. */
#define KAPP_IDF_NAME_MAX   32U
/** @brief Bytes of esp_app_desc_t.date, terminator included: "Oct 24 2026". */
#define KAPP_IDF_DATE_MAX   16U
/** @brief Bytes of esp_app_desc_t.time, terminator included: "21:14:03". */
#define KAPP_IDF_TIME_MAX   16U

/**
 * @brief What an app declares about itself.
 *
 * @a version plus @a reserved is why this should not need editing again: a
 * reader that meets a version it does not know reports "unmanifested"
 * rather than guessing at fields it has never seen.
 */
typedef struct {
    uint32_t magic;                         /**< KAPP_DESC_MAGIC.            */
    uint32_t version;                       /**< KAPP_DESC_VERSION.          */
    char     name[KAPP_NAME_MAX];           /**< Display name, e.g. "Kode Runner". */
    char     category[KAPP_CATEGORY_MAX];   /**< Comma-separated list; lowercased on read. */
    uint32_t reserved[8];                   /**< Zero. Icon, author, flags.  */
} kapp_desc_t;                              /* 136 bytes; v1 was 88 */

/** @brief Bytes of an image a reader must hold to find the manifest. */
#define KAPP_PREFIX_LEN     (KAPP_DESC_OFF + sizeof(kapp_desc_t))

/**
 * @brief Validate and normalise a manifest found in an image prefix.
 *
 * @param prefix The first bytes of an image; at least KAPP_PREFIX_LEN of them.
 * @param len    How many bytes @p prefix actually holds.
 * @param out    Filled only on KODE_OK; untouched otherwise.
 * @return KODE_OK when a manifest of a known version is present and both
 *         strings terminate inside their arrays. KODE_ERROR means
 *         "unmanifested" - a normal outcome, never a failure to report.
 */
kode_status_t kapp_desc_parse(const uint8_t *prefix, size_t len,
                              kapp_desc_t *out);

/**
 * @brief The project name IDF stamped into an image prefix.
 * @param prefix The first bytes of an image; at least KAPP_PREFIX_LEN.
 * @param len    How many bytes @p prefix holds.
 * @param out    Buffer for the name; filled with "" when there is none.
 * @param max    Its size in bytes.
 * @return KODE_OK when a name was found.
 */
kode_status_t kapp_project_name(const uint8_t *prefix, size_t len,
                                char *out, size_t max);

/**
 * @brief What IDF stamped into an image prefix: the project name, the
 *        version, and the date and time of the build.
 *
 * kapp_project_name() with the rest beside it: the version is what a
 * read-back after a coprocessor update compares the running image against,
 * and the date and time say when the image was built. They are the `__DATE__`
 * and `__TIME__` strings the compiler gave the descriptor ("Oct 24 2026",
 * "21:14:03"), copied out and always terminated.
 * A field that fills its array with no terminator is cut at the array's end,
 * never read past. Any output may be NULL when it is not wanted.
 *
 * @param prefix         The first bytes of an image; at least KAPP_PREFIX_LEN.
 * @param len            How many bytes @p prefix holds.
 * @param project        Buffer for the project name, or NULL; "" when none.
 * @param project_max    Its size in bytes.
 * @param version        Buffer for the version, or NULL; "" when none.
 * @param version_max    Its size in bytes.
 * @param build_date     Buffer for the build date, or NULL; "" when none.
 *                       KAPP_IDF_DATE_MAX bytes hold any date.
 * @param build_date_max Its size in bytes.
 * @param build_time     Buffer for the build time, or NULL; "" when none.
 *                       KAPP_IDF_TIME_MAX bytes hold any time.
 * @param build_time_max Its size in bytes.
 * @return KODE_OK when IDF's descriptor was found at its fixed offset.
 */
kode_status_t kapp_idf_desc(const uint8_t *prefix, size_t len,
                            char *project, size_t project_max,
                            char *version, size_t version_max,
                            char *build_date, size_t build_date_max,
                            char *build_time, size_t build_time_max);

/**
 * @brief The chip an ESP image was built for, from its header.
 *
 * A bare `.bin` says which silicon it runs on in its first sixteen bytes,
 * and that is the one fact that keeps a P4 image out of the C5's flash - the
 * two chips share a file format and nothing else. Read by offset rather than
 * through esp_image_header_t, for the reason kapp_project_name() reads IDF's
 * descriptor by offset: this header must stay free of ESP-IDF's, and the
 * layout is pinned by a static assertion where that header is available
 * (kdrv_radio.c).
 *
 * @param prefix  The first bytes of an image; at least KAPP_IMAGE_HDR_LEN.
 * @param len     How many bytes @p prefix holds.
 * @param chip_id The header's chip id (esp_chip_id_t as a number); left
 *                alone when the read is refused.
 * @return KODE_OK when the prefix starts with the ESP image magic;
 *         KODE_ERROR for anything else, a short prefix included.
 */
kode_status_t kapp_image_chip(const uint8_t *prefix, size_t len,
                              uint16_t *chip_id);

/** @brief Bytes of image header kapp_image_chip() needs: sizeof(esp_image_header_t). */
#define KAPP_IMAGE_HDR_LEN   24U
/** @brief Offset of esp_image_header_t.chip_id inside the header. */
#define KAPP_IMAGE_CHIP_OFF  12U
/** @brief ESP_IMAGE_HEADER_MAGIC, the first byte of every ESP image. */
#define KAPP_IMAGE_MAGIC     0xE9U

/**
 * @brief Declare this firmware's manifest. Optional; omit it and the app is
 *        unmanifested, which is exactly what every app in the tree is today.
 *
 * Place it at file scope, once per firmware:
 * @code
 *     KODE_APP_MANIFEST("Kode Runner", "game,demo");
 * @endcode
 *
 * `retain` is not decoration and must not be "cleaned up": `used` binds the
 * compiler, while the linker is a separate reader and --gc-sections collects
 * an unreferenced section whatever the compiler decided. Neither descriptor
 * section carries a KEEP() in ld.flash.sections, and IDF's own descriptor
 * survives only because code that actually runs - the boot banner logged at
 * startup - reads esp_app_desc; nothing anywhere reads this one. `retain`
 * emits SHF_GNU_RETAIN, which --gc-sections honours. Measured: without it
 * .flash.appdesc is exactly 0x100 and the manifest is simply absent from
 * the image.
 *
 * kode_app_desc is deliberately external linkage with no extern declaration
 * in scope, which MISRA C Rule 8.4 otherwise forbids: that deviation is what
 * turns two firmwares - or one firmware including this macro twice - into a
 * duplicate-symbol link error instead of two structs silently competing for
 * the same bytes of .rodata_custom_desc with only one of them winning.
 *
 * @param display_  String literal: what a user reads on the pill.
 * @param category_ String literal: a free-form group, e.g. "game", "audio" -
 *                   up to four, comma-separated, e.g. "game,demo".
 */
#define KODE_APP_MANIFEST(display_, category_)                                \
    _Static_assert(sizeof(display_) <= KAPP_NAME_MAX,                         \
                   "KODE_APP_MANIFEST: display name too long");               \
    _Static_assert(sizeof(category_) <= KAPP_CATEGORY_MAX,                    \
                   "KODE_APP_MANIFEST: category too long");                   \
    const __attribute__((section(".rodata_custom_desc")))                     \
    __attribute__((used, retain)) kapp_desc_t kode_app_desc = {               \
        .magic    = KAPP_DESC_MAGIC,                                          \
        .version  = KAPP_DESC_VERSION,                                        \
        .name     = (display_),                                               \
        .category = (category_),                                              \
        .reserved = { 0U }                                                    \
    }

#ifdef __cplusplus
}
#endif

#endif /* KAPP_DESC_H_ */
