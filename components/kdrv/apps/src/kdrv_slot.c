/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_slot.c
 * @brief The only file in kodeOS that calls esp_ota_*: the two kodeOS OTA slots and otadata.
 */
#include "kdrv_slot.h"
#include "kdrv_desc_fill.h"
#include "kdrv_bounce.h"

#include "kdrv_kfw.h"

#include <string.h>

#include "kode_log.h"

#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
/* espcoredump registers no component at all for the `linux` target (its
 * CMakeLists returns before declaring an include dir) - guard the header the
 * same way the body below is guarded, or the host `slot` suite, which links
 * this file for real, fails to find it regardless of the Kconfig value. */
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#include "esp_core_dump.h"
#endif
#if CONFIG_KODEOS_BOOT_ROLE_APP
#include "esp_image_format.h"
#include "esp_flash.h"
#include "spi_flash_mmap.h"
#include "kapps_fmt.h"
#endif

#define TAG            "kdrv.slot"
#define SLOT_ERASE_HDR (64U * 1024U)  /* enough to kill an image header */

static esp_ota_handle_t s_ota;

/**
 * @brief The slot this driver writes is whichever kodeOS slot is NOT running: the OTA ping-pong.
 *
 * Apps no longer live in ota partitions at all - they live in the apps region
 * (kdrv_apps.c).
 *
 * @return ota_1 while ota_0 runs, ota_0 otherwise; NULL when that partition
 *         is not in the table.
 */
static const esp_partition_t *slot_part(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    if ( (NULL != run) && (ESP_PARTITION_SUBTYPE_APP_OTA_0 == run->subtype) )
    {
        return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                        ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    }
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                    ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
}

bool kdrv_slot_confirm_self(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if ( (NULL == run)
        || (esp_ota_get_state_partition(run, &st) != ESP_OK)
        || (ESP_OTA_IMG_PENDING_VERIFY != st) )
    {
        return false;
    }
    return (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK);
}

kode_status_t kdrv_slot_reject_self(void)
{
    esp_err_t const err = esp_ota_mark_app_invalid_rollback();

    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "esp_ota_mark_app_invalid_rollback: %s", esp_err_to_name(err));
        return KODE_ERROR;
    }
    return KODE_OK;
}

/**
 * @brief Find the otadata partition.
 * @return The partition, or NULL when the partition table has none.
 */
static const esp_partition_t *otadata_part(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
}

/* ---- The slot ------------------------------------------------------------ */

bool kdrv_slot_present(void)
{
    return (slot_part() != NULL);
}

uint32_t kdrv_slot_size(void)
{
    const esp_partition_t *p = slot_part();
    return (NULL != p) ? (uint32_t)p->size : 0U;
}

uint32_t kdrv_slot_address(void)
{
    const esp_partition_t *p = slot_part();
    return (NULL != p) ? (uint32_t)p->address : 0U;
}

kode_status_t kdrv_slot_desc(kdrv_slot_desc_t *out)
{
    const esp_partition_t *p = slot_part();
    if ( (NULL == p) || (NULL == out) )
    {
        return KODE_DENIED;
    }
    esp_app_desc_t desc;
    if ( esp_ota_get_partition_description(p, &desc) != ESP_OK )
    {
        return KODE_ERROR;
    }
    kdrv_desc_fill(out, &desc);
    return KODE_OK;
}

bool kdrv_slot_has_image(void)
{
    kdrv_slot_desc_t desc;
    return (kdrv_slot_desc(&desc) == KODE_OK);
}

kode_status_t kdrv_slot_begin(void)
{
    const esp_partition_t *p = slot_part();
    if ( NULL == p )
    {
        return KODE_DENIED;
    }
    if ( 0 != s_ota )
    {
        return KODE_DENIED;   /* a session is already open */
    }
    /* esp_ota_begin() refuses a session from a slot still unconfirmed. A board
     * asked for its next update in the moments after the last one has answered
     * for this one, so it does not make whoever asked wait for AO_Boot. */
    (void)kdrv_slot_confirm_self();
    esp_err_t const err = esp_ota_begin(p, OTA_WITH_SEQUENTIAL_WRITES, &s_ota);
    if ( ESP_OK != err )
    {
        s_ota = 0;
        KLOG_E(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        return KODE_ERROR;
    }
    return KODE_OK;
}

/** @brief One bounce into the open session; esp_ota_write() keeps the place. */
static esp_err_t slot_put(void *ctx, size_t at, const void *src, size_t len)
{
    (void)ctx;
    (void)at;
    return esp_ota_write(s_ota, src, len);
}

kode_status_t kdrv_slot_write(const void *data, size_t len)
{
    if ( 0 == s_ota )
    {
        return KODE_DENIED;
    }
    esp_err_t const err = kdrv_bounce_write(&slot_put, NULL, data, len);
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "esp_ota_write: %s", esp_err_to_name(err));
        return KODE_ERROR;
    }
    return KODE_OK;
}

/**
 * @brief Tell whether the image in the slot is the project that is running.
 *
 * The slot is where this firmware's own update goes, and the next boot hands
 * the board to whatever it holds. esp_ota_end() accepts any image built for
 * the chip, an app's included, so the project's name is what tells an update
 * from something else.
 *
 * @return KODE_OK; KODE_ERROR when the slot's description cannot be read or
 *         names another project.
 */
static kode_status_t slot_is_ours(void)
{
    const esp_partition_t * const p   = slot_part();
    const esp_app_desc_t  * const run = esp_app_get_description();
    esp_app_desc_t                desc;

    if ( (NULL == p) || (NULL == run)
        || (esp_ota_get_partition_description(p, &desc) != ESP_OK) )
    {
        KLOG_E(TAG, "the image written has no description to read; "
                    "it is not an update");
        return KODE_ERROR;
    }
    if ( strncmp(desc.project_name, run->project_name,
                 sizeof(desc.project_name)) != 0 )
    {
        KLOG_E(TAG, "the image written is '%.*s', not %s; it is not an update",
               (int)sizeof(desc.project_name), desc.project_name,
               run->project_name);
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t kdrv_slot_end(void)
{
    if ( 0 == s_ota )
    {
        return KODE_DENIED;
    }
    esp_err_t const err = esp_ota_end(s_ota);
    s_ota = 0;                      /* consumed by esp_ota_end either way */
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        return KODE_ERROR;
    }
    return slot_is_ours();
}

void kdrv_slot_abort(void)
{
    if ( 0 != s_ota )
    {
        (void)esp_ota_abort(s_ota);
        s_ota = 0;
    }
}

/* ---- The slot as a kfw_sink_t ---------------------------------------------
 * The slot is a singleton, so the context is unused. begin() gains the one
 * thing the bare call never had: a length check. An image too big for the
 * slot is now refused before the first byte instead of discovered at the end,
 * after two minutes of writing.
 */

/**
 * @brief Refuse an image larger than the slot, then open an OTA session on it.
 * @param ctx     Unused.
 * @param img_len Decompressed length, or 0 when the source could not say;
 *                0 skips the size check.
 * @return KODE_OK; KODE_ERROR for an image larger than the slot; otherwise
 *         kdrv_slot_begin()'s answer.
 */
static kode_status_t slot_sink_begin(void *ctx, uint32_t img_len)
{
    (void)ctx;
    uint32_t const room = kdrv_slot_size();
    if ( (0U != img_len) && (0U != room) && (img_len > room) )
    {
        KLOG_E(TAG, "image is %u bytes, the slot holds %u",
               (unsigned)img_len, (unsigned)room);
        return KODE_ERROR;
    }
    return kdrv_slot_begin();
}

/**
 * @brief Append one chunk to the slot's OTA session.
 * @param ctx  Unused.
 * @param data The chunk.
 * @param len  Its length in bytes.
 * @return KODE_OK; KODE_DENIED with no session open; KODE_ERROR when the write fails.
 */
static kode_status_t slot_sink_write(void *ctx, const void *data, size_t len)
{
    (void)ctx;
    return kdrv_slot_write(data, len);
}

/**
 * @brief Close the slot's OTA session and verify what was written.
 * @param ctx Unused.
 * @return KODE_OK; KODE_DENIED with no session open; KODE_ERROR when
 *         kdrv_slot_end() refuses the image.
 */
static kode_status_t slot_sink_end(void *ctx)
{
    (void)ctx;
    return kdrv_slot_end();
}

/**
 * @brief Abandon the slot's OTA session, leaving the slot invalid.
 * @param ctx Unused.
 */
static void slot_sink_abort(void *ctx)
{
    (void)ctx;
    kdrv_slot_abort();
}

static const kfw_sink_t SLOT_SINK = {
    slot_sink_begin, slot_sink_write, slot_sink_end, slot_sink_abort
};

const kfw_sink_t *kdrv_slot_sink(void)
{
    return &SLOT_SINK;
}

kode_status_t kdrv_slot_erase(void)
{
    const esp_partition_t *p = slot_part();
    if ( NULL == p )
    {
        return KODE_DENIED;
    }
    size_t const len = (SLOT_ERASE_HDR > p->size) ? p->size : SLOT_ERASE_HDR;
    esp_err_t const err = esp_partition_erase_range(p, 0, len);
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "erase failed (%s)", esp_err_to_name(err));
        return KODE_ERROR;
    }
    KLOG_I(TAG, "user slot invalidated (first %uKB erased)",
           (unsigned)(len / 1024U));
    return KODE_OK;
}

/* ---- otadata ------------------------------------------------------------- */

bool kdrv_otadata_blank(void)
{
    const esp_partition_t *ota = otadata_part();
    if ( NULL == ota )
    {
        return true;
    }
    uint8_t buf[256];
    for ( size_t off = 0U; off < ota->size; off += sizeof(buf) )
    {
        size_t const n = ((ota->size - off) < sizeof(buf))
                       ? (ota->size - off) : sizeof(buf);
        if ( esp_partition_read(ota, off, buf, n) != ESP_OK )
        {
            return false;   /* cannot prove it is blank: treat as dirty */
        }
        for ( size_t i = 0U; i < n; ++i )
        {
            if ( 0xFFU != buf[i] )
            {
                return false;
            }
        }
    }
    return true;
}

kode_status_t kdrv_otadata_clear(void)
{
    const esp_partition_t *ota = otadata_part();
    if ( NULL == ota )
    {
        return KODE_DENIED;
    }
    esp_err_t const err = esp_partition_erase_range(ota, 0, ota->size);
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "could not clear otadata (%s)", esp_err_to_name(err));
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t kdrv_otadata_select_slot(void)
{
    const esp_partition_t *p = slot_part();
    if ( NULL == p )
    {
        return KODE_DENIED;
    }
    /* esp_ota_set_boot_partition verifies the image before writing otadata,
     * so a corrupt slot is refused here rather than at the next boot. */
    esp_err_t const err = esp_ota_set_boot_partition(p);
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "cannot select the user slot (%s)", esp_err_to_name(err));
        return KODE_ERROR;
    }
    return KODE_OK;
}

kdrv_slot_state_t kdrv_slot_state(void)
{
    const esp_partition_t *p = slot_part();
    if ( NULL == p )
    {
        return KDRV_SLOT_STATE_UNDEFINED;
    }
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if ( esp_ota_get_state_partition(p, &state) != ESP_OK )
    {
        return KDRV_SLOT_STATE_UNDEFINED;
    }
    switch ( state )
    {
        case ESP_OTA_IMG_ABORTED:   return KDRV_SLOT_STATE_ABORTED;
        case ESP_OTA_IMG_UNDEFINED: return KDRV_SLOT_STATE_UNDEFINED;
        default:                    return KDRV_SLOT_STATE_OTHER;
    }
}

/* ---- Every app partition, by id ------------------------------------------ */

kode_status_t kdrv_slot_info(kdrv_slot_id_t id, kdrv_slot_info_t *out)
{
    static const esp_partition_subtype_t SUBTYPES[KDRV_SLOT_ID_COUNT] = {
        ESP_PARTITION_SUBTYPE_APP_FACTORY,
        ESP_PARTITION_SUBTYPE_APP_OTA_0,
        ESP_PARTITION_SUBTYPE_APP_OTA_1,
    };

    if ( (NULL == out) || ((unsigned)id >= (unsigned)KDRV_SLOT_ID_COUNT) )
    {
        return KODE_DENIED;
    }
    const esp_partition_t *p =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, SUBTYPES[id], NULL);
    if ( NULL == p )
    {
        return KODE_DENIED;
    }

    (void)memset(out, 0, sizeof(*out));
    out->label   = p->label;
    out->address = (uint32_t)p->address;
    out->size    = (uint32_t)p->size;

    /* By address, not by pointer: the app-role wrap answers the running
     * question with a synthetic partition of its own, and two descriptions
     * of one place must still agree. */
    const esp_partition_t *run = esp_ota_get_running_partition();
    out->running = (NULL != run) && (run->address == p->address);

    esp_app_desc_t desc;
    if ( esp_ota_get_partition_description(p, &desc) == ESP_OK )
    {
        kdrv_desc_fill(&out->desc, &desc);
        out->has_image = true;
    }
    return KODE_OK;
}

/* ---- The crash the board kept --------------------------------------------- */

kode_status_t kdrv_slot_coredump(kdrv_coredump_t *out)
{
    if ( NULL == out )
    {
        return KODE_DENIED;
    }
    (void)memset(out, 0, sizeof(*out));

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    /* image_check() proves the checksum before anything else reads the
     * partition. ESP_ERR_NOT_FOUND is the one code that means the partition
     * really holds nothing - answer "none". Every other error (this build
     * verifies a SHA-256 image; a sketch's dump, from the Arduino core's IDF
     * v5.5.5 libraries, carries a CRC32 checksum instead, so image_check()
     * answers ESP_ERR_INVALID_CRC or ESP_ERR_INVALID_SIZE for it) means a
     * dump sits there that this build cannot decode: answer "present" and
     * stop, with nothing else claimed - get_summary() would be reading the
     * same bytes image_check() just refused. */
    esp_err_t const chk = esp_core_dump_image_check();
    if ( ESP_ERR_NOT_FOUND == chk )
    {
        return KODE_OK;
    }
    if ( ESP_OK != chk )
    {
        out->present = true;
        return KODE_OK;
    }

    esp_core_dump_summary_t summary = { 0 };
    if ( esp_core_dump_get_summary(&summary) != ESP_OK )
    {
        /* The checksum is good but the summary could not be built (no
         * crashed TCB found) - still a dump, still not claimable. */
        out->present = true;
        return KODE_OK;
    }
    out->present = true;
    (void)snprintf(out->task, sizeof(out->task), "%s", summary.exc_task);
    out->pc = summary.exc_pc;

    /* app_elf_sha256 is a hex string, not raw digest bytes - both here and
     * from esp_app_get_elf_sha256(), which is why a plain memcmp over the
     * summary's own field width is enough: same encoding, same length. */
    char running[sizeof(summary.app_elf_sha256)];
    (void)esp_app_get_elf_sha256(running, sizeof(running));
    out->mine = (memcmp(summary.app_elf_sha256, running, sizeof(running)) == 0);
    return KODE_OK;
#else
    /* Apps built from the SDK do not turn this on: whether a firmware keeps
     * its crashes is kodeOS's call, not an app's. */
    return KODE_DENIED;
#endif
}

kode_status_t kdrv_slot_coredump_reason(char *buf, size_t len)
{
    if ( (NULL == buf) || (0U == len) )
    {
        return KODE_DENIED;
    }
    buf[0] = '\0';

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    /* The same ownership check `info` makes: only a dump whose ELF SHA-256
     * is this image's can say anything about this image's last panic. */
    kdrv_coredump_t cd;
    if ( (kdrv_slot_coredump(&cd) != KODE_OK) || !cd.present || !cd.mine )
    {
        return KODE_DENIED;
    }
    if ( (esp_core_dump_get_panic_reason(buf, len) != ESP_OK) || ('\0' == buf[0]) )
    {
        buf[0] = '\0';
        return KODE_DENIED;
    }
    return KODE_OK;
#else
    return KODE_DENIED;
#endif
}

/* ---- Where we are running ------------------------------------------------ */

void kdrv_running_info(const char **label, uint32_t *address, uint32_t *size)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    if ( NULL != label )   { *label   = (NULL != p) ? p->label : "?"; }
    if ( NULL != address ) { *address = (NULL != p) ? (uint32_t)p->address : 0U; }
    if ( NULL != size )    { *size    = (NULL != p) ? (uint32_t)p->size : 0U; }
}

kode_status_t kdrv_running_desc(kdrv_slot_desc_t *out)
{
    if ( NULL == out )
    {
        return KODE_DENIED;
    }
    /* Unlike kdrv_slot_desc() (the inactive slot, read off flash by
     * partition), esp_app_get_description() answers for the image already
     * running - no partition lookup needed, and it cannot fail for a boot
     * that got this far. */
    const esp_app_desc_t *desc = esp_app_get_description();
    if ( NULL == desc )
    {
        return KODE_ERROR;
    }
    kdrv_desc_fill(out, desc);
    return KODE_OK;
}

bool kdrv_running_is_factory(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    return (NULL != p) && (ESP_PARTITION_SUBTYPE_APP_FACTORY == p->subtype);
}

/* The KDRV_RESET_ values are spelt out in the header so nothing above this layer has to
 * include esp_system.h; these keep the two numberings one. */
_Static_assert((int)KDRV_RESET_UNKNOWN == (int)ESP_RST_UNKNOWN, "reset UNKNOWN");
_Static_assert((int)KDRV_RESET_POWERON == (int)ESP_RST_POWERON, "reset POWERON");
_Static_assert((int)KDRV_RESET_EXTERNAL == (int)ESP_RST_EXT, "reset EXT");
_Static_assert((int)KDRV_RESET_SOFTWARE == (int)ESP_RST_SW, "reset SW");
_Static_assert((int)KDRV_RESET_PANIC == (int)ESP_RST_PANIC, "reset PANIC");
_Static_assert((int)KDRV_RESET_INT_WDT == (int)ESP_RST_INT_WDT, "reset INT_WDT");
_Static_assert((int)KDRV_RESET_TASK_WDT == (int)ESP_RST_TASK_WDT, "reset TASK_WDT");
_Static_assert((int)KDRV_RESET_WDT == (int)ESP_RST_WDT, "reset WDT");
_Static_assert((int)KDRV_RESET_DEEPSLEEP == (int)ESP_RST_DEEPSLEEP, "reset DEEPSLEEP");
_Static_assert((int)KDRV_RESET_BROWNOUT == (int)ESP_RST_BROWNOUT, "reset BROWNOUT");
_Static_assert((int)KDRV_RESET_SDIO == (int)ESP_RST_SDIO, "reset SDIO");
_Static_assert((int)KDRV_RESET_USB == (int)ESP_RST_USB, "reset USB");
_Static_assert((int)KDRV_RESET_JTAG == (int)ESP_RST_JTAG, "reset JTAG");
_Static_assert((int)KDRV_RESET_EFUSE == (int)ESP_RST_EFUSE, "reset EFUSE");
_Static_assert((int)KDRV_RESET_PWR_GLITCH == (int)ESP_RST_PWR_GLITCH, "reset PWR_GLITCH");
_Static_assert((int)KDRV_RESET_CPU_LOCKUP == (int)ESP_RST_CPU_LOCKUP, "reset CPU_LOCKUP");

int kdrv_reset_reason(void)
{
    return (int)esp_reset_reason();
}

#if CONFIG_KODEOS_BOOT_ROLE_APP
/* ---- Flash-write guard for an app running in place from the region ---------
 *
 * IDF ships CONFIG_SPI_FLASH_DANGEROUS_WRITE_ABORTS, so every esp_flash_write
 * passes through esp_partition_main_flash_region_safe(), which asks
 * esp_ota_get_running_partition() which region the caller must not overwrite.
 * That lookup searches only APP-type partitions. A kodeOS app runs in place
 * from the `apps` region, a DATA partition, so the lookup finds nothing and
 * aborts ("Partition table is invalid or corrupt") - and with a sticky boot
 * request an abort at boot (NVS recovery, the autojoin promote) is a panic
 * loop. Every persistent write an app makes - kset, the WiFi store,
 * the FAT - and even boot_info, which does not write, hit it.
 *
 * These two wraps answer for an app. They exist only under ROLE_APP: kodeOS
 * itself (ROLE_OS) runs from a real APP partition, needs neither, and is left
 * byte-for-byte unchanged - the CMakeLists gates the --wrap flags the same
 * way. This is the SDK side of the Arduino variant's kode_flash_guard.cpp,
 * one mechanism for the one problem. The known limit is the same too: --wrap
 * redirects only undefined references, so a direct esp_ota_begin() inside an
 * app still reaches the real symbol and aborts; an app has no OTA slot to
 * write anyway.
 */

/** @brief The real lookup, still linked, used when the image cannot be found. */
extern const esp_partition_t *__real_esp_ota_get_running_partition(void);

/**
 * @brief Answer the running-partition question with this app's own image.
 * @return A synthetic APP partition covering the running image, or the stock
 *         answer if the image cannot be located.
 */
const esp_partition_t *__wrap_esp_ota_get_running_partition(void)
{
    static esp_partition_t part;
    static bool            ready = false;
    if ( ready )
    {
        return &part;
    }

    const esp_app_desc_t *desc = esp_app_get_description();
    if ( NULL == desc )
    {
        return __real_esp_ota_get_running_partition();
    }
    size_t const phys = spi_flash_cache2phys((const void *)desc);
    if ( SPI_FLASH_CACHE2PHYS_FAIL == phys )
    {
        return __real_esp_ota_get_running_partition();
    }
    /* The descriptor sits at image offset 0x20 (24-byte image header + one
     * 8-byte segment header), so the image starts 0x20 below it. */
    uint32_t const start = (uint32_t)phys - 0x20U;

    const esp_partition_t *apps =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                 (esp_partition_subtype_t)KAPPS_PART_SUBTYPE, NULL);
    if ( (NULL == apps) || (start < apps->address)
        || ((apps->address + apps->size) <= start) )
    {
        return __real_esp_ota_get_running_partition();
    }

    /* Bound the synthetic partition to the region, then tighten it to the
     * image's own length so the guard refuses writes into THIS app's image
     * while allowing nvs, storage and the rest. The metadata read is headers
     * only - no SHA - because all this needs is the length. */
    uint32_t size = (apps->address + apps->size) - start;
    esp_partition_pos_t const pos = { .offset = start, .size = size };
    esp_image_metadata_t meta;
    if ( esp_image_get_metadata(&pos, &meta) == ESP_OK )
    {
        uint32_t const img = (meta.image_len + 0xFFFU) & ~0xFFFU;
        if ( img < size )
        {
            size = img;
        }
    }

    (void)memset(&part, 0, sizeof(part));
    part.flash_chip = esp_flash_default_chip;
    part.type       = ESP_PARTITION_TYPE_APP;
    part.subtype    = ESP_PARTITION_SUBTYPE_APP_TEST;   /* not FACTORY: an app is
                                                         * not the rescue kodeOS */
    part.address    = start;
    part.size       = size;
    part.erase_size = 0x1000U;
    part.readonly   = false;
    (void)snprintf(part.label, sizeof(part.label), "%s", "app");
    ready = true;
    return &part;
}

/**
 * @brief An app running from the region has no OTA slot of its own.
 * @param from Ignored.
 * @return Always NULL, so esp_ota_get_next_update_partition callers fail
 *         cleanly instead of aborting, and none reaches kodeOS's ota slots.
 */
const esp_partition_t *__wrap_esp_ota_get_next_update_partition(const esp_partition_t *from)
{
    (void)from;
    return NULL;
}
#endif /* CONFIG_KODEOS_BOOT_ROLE_APP */
