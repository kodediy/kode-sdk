/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_banner.c
 * @brief The boot banner: gathers the image, SDK, chip and reset facts and prints them as four klog lines.
 */
#include "kode_banner.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "kbsp_board.h"
#include "kdrv_apps.h"
#include "kdrv_slot.h"
#include "khal_system.h"
#include "kode_log.h"
#include "kode_sdk_version.h"   /* KODE_SDK_VERSION, written by the build */

#define TAG  "kode"

#define BANNER_DUMP_MAX   96U             /**< Room for the dump's panic reason.      */
#define BANNER_LINE_MAX   160U            /**< Room for the reset line.               */
#define BANNER_UNKNOWN    "?"             /**< Said when a fact cannot be read.       */
#define BANNER_MB_BYTES   (1024U * 1024U) /**< Bytes in the megabyte the sizes use.   */

/**
 * @brief Compose the role and place half of line 1.
 * @param out Receives the text.
 * @param len Room in @p out.
 */
static void role_text(char *out, size_t len)
{
#if CONFIG_KODEOS_BOOT_ROLE_OS
    char const *label = BANNER_UNKNOWN;
    kdrv_running_info(&label, NULL, NULL);
    (void)snprintf(out, len, "the OS, from %s", label);
#else
    (void)snprintf(out, len, "an app");
#endif
}

#if CONFIG_KODEOS_BOOT_ROLE_OS
/**
 * @brief The app this boot came back from, read off the TRIED request (which it leaves for AO_Boot).
 * @param name Receives the app's project name, or "?" when the directory no longer holds the image.
 * @param len  Room in @p name.
 * @return @p name, or NULL when the board did not come back from an app.
 */
static char const *back_from_app(char *name, size_t len)
{
    uint32_t offset = 0U;
    if ( (kdrv_apps_init() != KODE_OK) || !kdrv_apps_req_tried(&offset) )
    {
        return NULL;
    }

    (void)snprintf(name, len, "%s", BANNER_UNKNOWN);
    kapps_dir_t const * const dir = kdrv_apps_dir();
    if ( NULL == dir )
    {
        return name;
    }
    for ( uint32_t i = 0U; (i < dir->count) && (KAPPS_MAX_ENTRIES > i); ++i )
    {
        if ( dir->e[i].offset == offset )
        {
            kdrv_slot_desc_t desc;
            if ( kdrv_apps_desc(&dir->e[i], &desc, NULL) == KODE_OK )
            {
                (void)snprintf(name, len, "%s", desc.project);
            }
            break;
        }
    }
    return name;
}
#endif

void kode_banner_print(void)
{
    /* Every fact first: what the gathering logs must not land between the lines. */
    kdrv_slot_desc_t img;
    if ( kdrv_running_desc(&img) != KODE_OK )
    {
        (void)memset(&img, 0, sizeof(img));
        (void)snprintf(img.project, sizeof(img.project), "%s", BANNER_UNKNOWN);
        (void)snprintf(img.version, sizeof(img.version), "%s", BANNER_UNKNOWN);
        (void)snprintf(img.idf_ver, sizeof(img.idf_ver), "%s", BANNER_UNKNOWN);
    }
    char role[KDRV_SLOT_PROJECT_MAX];
    role_text(role, sizeof(role));

    /* Only the flash size can fail; khal_chip_info() fills the rest first. */
    khal_chip_info_t chip;
    (void)memset(&chip, 0, sizeof(chip));
    (void)khal_chip_info(&chip);

    int32_t const reason = kdrv_reset_reason();

    char const *app = NULL;
#if CONFIG_KODEOS_BOOT_ROLE_OS
    char app_name[KDRV_SLOT_PROJECT_MAX];
    app = back_from_app(app_name, sizeof(app_name));
#endif

    /* After a return from an app the dump in flash is an old kodeOS one: not read. */
    char dump[BANNER_DUMP_MAX];
    char const *dump_reason = NULL;
    if ( (NULL == app) && kode_banner_dump_explains(reason)
        && (kdrv_slot_coredump_reason(dump, sizeof(dump)) == KODE_OK) )
    {
        dump_reason = dump;
    }

    char line[BANNER_LINE_MAX];
    (void)kode_banner_reset_line(line, sizeof(line), reason, dump_reason, app);

    /* Line 1: the running image, then what it runs as and from where. */
    kode_log_always(KODE_LOG_NOTICE, TAG, "%s %s (%s %s, ESP-IDF %s), %s",
                    img.project, img.version, img.date, img.time, img.idf_ver, role);

    /* Line 2: the SDK, as the build recorded it. */
    kode_log_always(KODE_LOG_NOTICE, TAG, "sdk %s", KODE_SDK_VERSION);

    /* Line 3: the board and the chip under it. */
    kode_log_always(KODE_LOG_NOTICE, TAG,
                    "hardware %s %s, %s v%" PRIu32 ".%" PRIu32 ", %" PRIu32
                    " MB flash, %" PRIu32 " MB PSRAM",
                    KBSP_PRODUCT_NAME, KBSP_BOARD_REV, chip.model,
                    (uint32_t)chip.rev_major, (uint32_t)chip.rev_minor,
                    chip.flash_bytes / BANNER_MB_BYTES,
                    chip.psram_bytes / BANNER_MB_BYTES);

    /* Line 4: why the board reset. */
    kode_log_always(KODE_LOG_NOTICE, TAG, "%s", line);
}
