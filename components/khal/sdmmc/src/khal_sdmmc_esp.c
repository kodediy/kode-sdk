/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_sdmmc_esp.c
 * @brief The microSD on ESP-IDF: slot 0, its FAT volume, plain and hosted mount paths.
 */
#include "khal_sdmmc.h"

#include <string.h>

#include "sdkconfig.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sd_protocol_defs.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "kbsp.h"
#include "khal_time.h"
#include "kode_log.h"

#define TAG                  "khal.sdmmc"   /**< This file's log tag. */
#define SD_MOUNT_POINT       "/sdcard"      /**< Where the volume's files appear. */
#define SD_LDO_SETTLE_MS     10U            /**< Wait for the IO rail before any clock. */
#define SD_MAX_FILES         5              /**< Files the volume keeps open at once. */
#define SD_ALLOC_UNIT_BYTES  (64U * 1024U)  /**< Cluster size a mount and a format ask for. */
#define HZ_PER_KHZ           1000           /**< The CSD's rate is in Hz; the snapshot's in kHz. */
#define SD_HOST_LOG_TAG      "SD_HOST"      /**< ESP-IDF's tag for the SDMMC host driver. */
#define SD_LINK_SLOT         SDMMC_HOST_SLOT_1  /**< The slot esp-hosted takes for the C5's link. */

static sdmmc_card_t        *s_card;          /**< The card under the volume. */
static sd_pwr_ctrl_handle_t s_pwr;           /**< The IO domain's LDO, once it is up. */
static StaticSemaphore_t    s_ctlr_lock_mem; /**< Storage for s_ctlr_lock. */
static SemaphoreHandle_t    s_ctlr_lock;     /**< One controller change at a time. */

/** @brief Whether the next mount reuses esp-hosted's controller; it takes it before app_main. */
#if CONFIG_ESP_HOSTED_ENABLED
static bool s_hosted = true;
static bool s_link_slot;   /**< The link's slot is on the controller: it is there for the card's slot to join. */
static bool s_link_heard;  /**< The C5 answered the last refresh of the link's slot, or none ran yet. */
#else
static bool s_hosted;
#endif

/* ---- The controller's lifetime ------------------------------------------ */

/* The card's slot 0 and esp-hosted's slot 1 share one controller, and both
 * create, join, leave and delete it through ESP-IDF's sdmmc_host, whose
 * controller and slot handles have no lock. In hosted mode the linker routes
 * those four calls through this file (`--wrap`, khal's CMakeLists), and one
 * recursive mutex makes them take turns. Transfers need nothing here: the
 * controller's own mutex already serialises them. */

/**
 * @brief Take the controller's lock.
 *
 * esp-hosted's constructor brings the link up before the scheduler starts, so
 * the first call of a boot creates the mutex while one thread runs. Until the
 * scheduler starts nothing can race, and nothing is taken.
 *
 * @return True when the lock was taken, for ctlr_unlock().
 */
static bool ctlr_lock(void)
{
    if ( NULL == s_ctlr_lock )
    {
        s_ctlr_lock = xSemaphoreCreateRecursiveMutexStatic(&s_ctlr_lock_mem);
    }
    if ( taskSCHEDULER_NOT_STARTED == xTaskGetSchedulerState() )
    {
        return false;
    }
    (void)xSemaphoreTakeRecursive(s_ctlr_lock, portMAX_DELAY);
    return true;
}

/**
 * @brief Give back what ctlr_lock() took.
 * @param taken What ctlr_lock() returned.
 */
static void ctlr_unlock(bool taken)
{
    if ( taken )
    {
        (void)xSemaphoreGiveRecursive(s_ctlr_lock);
    }
}

/* Hosted mode's host hooks: the controller is esp-hosted's, the slot is ours.
 * Init does nothing; deinit releases slot 0 and leaves the controller. */

/** @brief Hosted mode's init: the controller is already up. */
static esp_err_t sd_host_init_noop(void)
{
    return ESP_OK;
}

/**
 * @brief Release one slot of the shared controller, quietly when the other
 *        slot keeps it.
 *
 * While the other slot is registered the driver logs an error for the
 * controller it may not delete; that is the expected outcome, so its tag is
 * held quiet for the one call.
 *
 * @param slot SDMMC_HOST_SLOT_0 (the card) or SD_LINK_SLOT (the link).
 * @return What sdmmc_host_deinit_slot() said.
 */
static esp_err_t deinit_slot_shared(int slot)
{
    bool const            taken = ctlr_lock();
    esp_log_level_t const was   = esp_log_level_get(SD_HOST_LOG_TAG);

    esp_log_level_set(SD_HOST_LOG_TAG, ESP_LOG_NONE);
    esp_err_t const err = sdmmc_host_deinit_slot(slot);
    esp_log_level_set(SD_HOST_LOG_TAG, was);
    ctlr_unlock(taken);
    return err;
}

/**
 * @brief Hosted mode's deinit: release the card's slot only.
 * @return What deinit_slot_shared() said for slot 0.
 */
static esp_err_t sd_host_deinit_slot(void)
{
    return deinit_slot_shared(SDMMC_HOST_SLOT_0);
}

#if CONFIG_ESP_HOSTED_ENABLED
/* MISRA C:2012 Rule 21.2 deviation, for every name below: the leading double
 * underscore is the name the linker's --wrap contract requires. */

esp_err_t __real_sdmmc_host_init(void);
esp_err_t __real_sdmmc_host_init_slot(int slot,
                                      const sdmmc_slot_config_t *slot_config);
esp_err_t __real_sdmmc_host_deinit_slot(int slot);

/**
 * @brief sdmmc_host_init(), under the controller's lock.
 * @return What sdmmc_host_init() said.
 */
esp_err_t __wrap_sdmmc_host_init(void);
esp_err_t __wrap_sdmmc_host_init(void)
{
    bool const      taken = ctlr_lock();
    esp_err_t const err   = __real_sdmmc_host_init();

    ctlr_unlock(taken);
    return err;
}

/**
 * @brief sdmmc_host_init_slot(), under the controller's lock.
 * @param slot        The slot to add to the controller.
 * @param slot_config Its pins and width.
 * @return What sdmmc_host_init_slot() said.
 */
esp_err_t __wrap_sdmmc_host_init_slot(int slot,
                                      const sdmmc_slot_config_t *slot_config);
esp_err_t __wrap_sdmmc_host_init_slot(int slot,
                                      const sdmmc_slot_config_t *slot_config)
{
    bool const      taken = ctlr_lock();
    esp_err_t const err   = __real_sdmmc_host_init_slot(slot, slot_config);

    if ( (ESP_OK == err) && (SD_LINK_SLOT == slot) )
    {
        s_link_slot  = true;
        s_link_heard = true;
    }
    ctlr_unlock(taken);
    return err;
}

/**
 * @brief sdmmc_host_deinit_slot(), under the controller's lock.
 * @param slot The slot to remove; the controller goes with the last one.
 * @return What sdmmc_host_deinit_slot() said.
 */
esp_err_t __wrap_sdmmc_host_deinit_slot(int slot);
esp_err_t __wrap_sdmmc_host_deinit_slot(int slot)
{
    bool const      taken = ctlr_lock();
    esp_err_t const err   = __real_sdmmc_host_deinit_slot(slot);

    if ( (ESP_OK == err) && (SD_LINK_SLOT == slot) )
    {
        s_link_slot = false;
    }
    ctlr_unlock(taken);
    return err;
}

/**
 * @brief esp-hosted's sdmmc_host_deinit(), narrowed to the link's slot 1.
 *
 * The linker routes the link's teardown here (`--wrap=sdmmc_host_deinit`,
 * khal's CMakeLists), so the card's slot 0 survives it.
 *
 * @return What deinit_slot_shared() said for the link's slot.
 */
esp_err_t __wrap_sdmmc_host_deinit(void);
esp_err_t __wrap_sdmmc_host_deinit(void)
{
    return deinit_slot_shared(SD_LINK_SLOT);
}
#endif /* CONFIG_ESP_HOSTED_ENABLED */

#if CONFIG_ESP_HOSTED_ENABLED
#define SD_LINK_REFRESH_REG  0U     /**< Function 0 register the refresh reads: the CCCR revision. */
#define SD_LINK_REFRESH_MS   50U    /**< How long the refresh waits for the C5 to answer, in ms. */

/**
 * @brief Put the link's slot back in order after a command of the card's slot.
 *
 * ESP-IDF programs the controller's shared clock generator for whichever slot
 * runs a command and leaves it there. After a card command the link's slot has
 * neither its clock nor its interrupt sensing back until its own next command,
 * and an answer the C5 raises in between is not heard. One register read on the
 * link's slot, under the controller's lock, programs its clock again and runs
 * it. A read the C5 does not answer ends the refreshes until the link's slot is
 * created again: a C5 that is gone must not slow every card command.
 */
static void link_slot_refresh(void)
{
    bool const taken = ctlr_lock();

    if ( s_link_slot && s_link_heard )
    {
        sdmmc_command_t cmd = {
            .opcode     = SD_IO_RW_DIRECT,
            .flags      = SCF_CMD_AC | SCF_RSP_R5,
            .timeout_ms = SD_LINK_REFRESH_MS,
            .arg        = SD_ARG_CMD52_READ
                          | (SD_LINK_REFRESH_REG << SD_ARG_CMD52_REG_SHIFT),
        };

        if ( ESP_OK != sdmmc_host_do_transaction(SD_LINK_SLOT, &cmd) )
        {
            s_link_heard = false;
        }
    }
    ctlr_unlock(taken);
}

/**
 * @brief The card's slot-0 command, followed by the link slot's refresh.
 * @param slot The card's slot.
 * @param cmd  The command.
 * @return What sdmmc_host_do_transaction() said for the card's command.
 */
static esp_err_t sd_host_do_transaction(int slot, sdmmc_command_t *cmd)
{
    esp_err_t const err = sdmmc_host_do_transaction(slot, cmd);

    link_slot_refresh();
    return err;
}
#endif /* CONFIG_ESP_HOSTED_ENABLED */

/* ---- The snapshot -------------------------------------------------------- */

/**
 * @brief Copy the identified card's registers out of ESP-IDF's live handle.
 *
 * @param[in]  card The handle the mount filled in.
 * @param[out] out  This seam's copy.
 */
static void snapshot(const sdmmc_card_t *card, khal_sdmmc_card_t *out)
{
    (void)memset(out, 0, sizeof(*out));

    out->mfg_id   = (uint8_t)card->cid.mfg_id;
    out->oem_id   = (uint16_t)card->cid.oem_id;
    (void)memcpy(out->name, card->cid.name, sizeof(out->name) - 1U);
    out->name[sizeof(out->name) - 1U] = '\0';
    out->revision = (uint8_t)card->cid.revision;
    out->serial   = (uint32_t)card->cid.serial;
    out->date     = (uint16_t)card->cid.date;

    out->csd_ver        = (uint8_t)card->csd.csd_ver;
    out->sectors        = (uint32_t)card->csd.capacity;
    out->sector_size    = (uint16_t)card->csd.sector_size;
    out->read_block_len = (uint16_t)card->csd.read_block_len;
    out->ccc            = (uint16_t)card->csd.card_command_class;
    out->tr_speed_khz   = (uint32_t)(card->csd.tr_speed / HZ_PER_KHZ);

    out->sd_spec     = (uint8_t)card->scr.sd_spec;
    out->bus_widths  = (uint8_t)card->scr.bus_width;
    out->erase_state = (0U != card->scr.erase_mem_state);

    out->au_kb           = (uint16_t)card->ssr.alloc_unit_kb;
    out->erase_size_au   = (uint16_t)card->ssr.erase_size_au;
    out->erase_timeout_s = (uint8_t)card->ssr.erase_timeout;
    out->erase_offset_s  = (uint8_t)card->ssr.erase_offset;
    out->discard         = (0U != card->ssr.discard_support);
    out->fule            = (0U != card->ssr.fule_support);

    out->high_capacity = (0U != (card->ocr & SD_OCR_SDHC_CAP));
    /* log_bus_width is the log2 of the width, not the width. */
    out->bus_width     = (uint8_t)(1U << card->log_bus_width);
    out->freq_khz      = (uint32_t)card->real_freq_khz;
    out->max_freq_khz  = card->max_freq_khz;
    out->rca           = card->rca;
    out->is_mmc        = (0U != card->is_mmc);
    out->is_uhs1       = (0U != card->is_uhs1);
}

/* ---- Mounting ------------------------------------------------------------ */

void khal_sdmmc_set_hosted(bool hosted)
{
    s_hosted = hosted;
}

bool khal_sdmmc_hosted(void)
{
    return s_hosted;
}

bool khal_sdmmc_mounted(void)
{
    return (NULL != s_card);
}

bool khal_sdmmc_ready(void)
{
#if CONFIG_ESP_HOSTED_ENABLED
    return (!s_hosted) || s_link_slot;
#else
    return true;
#endif
}

/**
 * @brief Bring up the LDO that feeds the SDMMC IO domain, once per boot.
 * @return KODE_OK, or KODE_ERROR with the reason logged.
 */
static kode_status_t ldo_up(void)
{
    if ( NULL != s_pwr )
    {
        return KODE_OK;
    }
    const sd_pwr_ctrl_ldo_config_t ldo_cfg = {
        .ldo_chan_id = KBSP_SD_LDO_CHAN,
    };
    esp_err_t const err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &s_pwr);
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "on-chip LDO ch%d: %s",
               KBSP_SD_LDO_CHAN, esp_err_to_name(err));
        return KODE_ERROR;
    }
    khal_delay_ms(SD_LDO_SETTLE_MS);
    return KODE_OK;
}

const char *khal_sdmmc_mount_point(void)
{
    return SD_MOUNT_POINT;
}

/**
 * @brief Fill in the host and the slot the way both ways onto the card need.
 *
 * @param[in]  width   Data lines: 1 or 4.
 * @param[in]  max_khz Ceiling for the host clock, in kHz.
 * @param[out] host    Slot 0's host, with hosted mode's hooks when it applies.
 * @param[out] slot    Slot 0 on its IOMUX pins.
 */
static void host_config(uint8_t width, uint32_t max_khz, sdmmc_host_t *host,
                        sdmmc_slot_config_t *slot)
{
    sdmmc_host_t const        h = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t const s = SDMMC_SLOT_CONFIG_DEFAULT();

    *host = h;
    host->slot            = SDMMC_HOST_SLOT_0;
    host->max_freq_khz    = (int)max_khz;
    host->pwr_ctrl_handle = s_pwr;
    if ( s_hosted )
    {
        /* deinit shares a union with deinit_p, so its flag goes with it. */
        host->init   = &sd_host_init_noop;
        host->deinit = &sd_host_deinit_slot;
#if CONFIG_ESP_HOSTED_ENABLED
        host->do_transaction = &sd_host_do_transaction;
#endif
        host->flags &= ~(uint32_t)SDMMC_HOST_FLAG_DEINIT_ARG;
    }

    *slot = s;
    /* Slot 0 is on its IOMUX pins: no routing to give. */
    slot->width = (uint8_t)width;
}

/**
 * @brief Check a mount may start, power the IO domain, and fill in the host.
 *
 * @param[in]  width   Data lines: 1 or 4.
 * @param[in]  max_khz Ceiling for the host clock, in kHz.
 * @param[out] host    Slot 0's host.
 * @param[out] slot    Slot 0's configuration.
 * @return KODE_OK; KODE_DENIED for a bad argument, a card already mounted or
 *         a hosted mount with no controller to join; KODE_ERROR when the LDO
 *         will not come up.
 */
static kode_status_t card_prepare(uint8_t width, uint32_t max_khz,
                                  sdmmc_host_t *host, sdmmc_slot_config_t *slot)
{
    if ( ((1U != width) && (4U != width)) || (0U == max_khz) )
    {
        return KODE_DENIED;
    }
    if ( NULL != s_card )
    {
        return KODE_DENIED;
    }
    if ( !khal_sdmmc_ready() )
    {
        return KODE_DENIED;
    }
    if ( ldo_up() != KODE_OK )
    {
        return KODE_ERROR;
    }
    host_config(width, max_khz, host, slot);
    return KODE_OK;
}

kode_status_t khal_sdmmc_mount(uint8_t width, uint32_t max_khz,
                               khal_sdmmc_card_t *out)
{
    sdmmc_host_t        host;
    sdmmc_slot_config_t slot;
    kode_status_t const st = card_prepare(width, max_khz, &host, &slot);

    if ( KODE_OK != st )
    {
        return st;
    }

    /* Never reformat someone's card. */
    const esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = SD_MAX_FILES,
        .allocation_unit_size   = SD_ALLOC_UNIT_BYTES,
    };

    esp_err_t const err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot,
                                                  &mount_cfg, &s_card);
    if ( ESP_OK != err )
    {
        s_card = NULL;
        /* kdrv_sd tells the outcome; this is only the try. ESP_FAIL is the
         * one failure that comes after the card answered: its volume. */
        KLOG_D(TAG, "%d-bit mount at %lukHz: %s",
               (int)width, (unsigned long)max_khz, esp_err_to_name(err));
        if ( ESP_ERR_TIMEOUT == err )
        {
            return KODE_TIMEOUT;
        }
        if ( ESP_FAIL == err )
        {
            return KODE_IO_ERROR;
        }
        return KODE_ERROR;
    }
    if ( NULL != out )
    {
        snapshot(s_card, out);
    }
    return KODE_OK;
}

void khal_sdmmc_unmount(void)
{
    if ( NULL == s_card )
    {
        return;
    }
    (void)esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    s_card = NULL;
}

kode_status_t khal_sdmmc_info(uint64_t *total, uint64_t *free_bytes)
{
    if ( NULL == s_card )
    {
        return KODE_DENIED;
    }
    uint64_t t = 0U;
    uint64_t f = 0U;
    if ( esp_vfs_fat_info(SD_MOUNT_POINT, &t, &f) != ESP_OK )
    {
        return KODE_ERROR;
    }
    if ( NULL != total )
    {
        *total = t;
    }
    if ( NULL != free_bytes )
    {
        *free_bytes = f;
    }
    return KODE_OK;
}

kode_status_t khal_sdmmc_format(void)
{
    if ( NULL == s_card )
    {
        return KODE_DENIED;
    }
    /* NULL config: keep the cluster size the mount asked for. */
    esp_err_t const err = esp_vfs_fat_sdcard_format(SD_MOUNT_POINT, s_card);
    if ( ESP_OK != err )
    {
        /* mkfs failed: the card is still ours to release, so release it. */
        KLOG_E(TAG, "format: %s", esp_err_to_name(err));
        (void)esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        s_card = NULL;
        return KODE_ERROR;
    }

    /* ESP_OK is a claim: a failed remount frees the card and still says OK.
     * Ask the filesystem, and never hand the freed pointer to unmount(). */
    if ( esp_vfs_fat_info(SD_MOUNT_POINT, NULL, NULL) != ESP_OK )
    {
        KLOG_E(TAG, "format: the volume did not come back; the card is gone");
        s_card = NULL;
        return KODE_ERROR;
    }
    return KODE_OK;
}

/* ---- Raw sectors --------------------------------------------------------- */

kode_status_t khal_sdmmc_read_sectors(uint32_t first, uint32_t count, void *dst)
{
    if ( (NULL == s_card) || (NULL == dst) )
    {
        return KODE_DENIED;
    }
    if ( 0U == count )
    {
        return KODE_OK;
    }
    esp_err_t const err = sdmmc_read_sectors(s_card, dst, (size_t)first,
                                             (size_t)count);
    if ( ESP_OK != err )
    {
        KLOG_W(TAG, "read %lu sectors at %lu: %s",
               (unsigned long)count, (unsigned long)first,
               esp_err_to_name(err));
        return KODE_ERROR;
    }
    return KODE_OK;
}

kode_status_t khal_sdmmc_write_sectors(uint32_t first, uint32_t count,
                                       const void *src)
{
    if ( (NULL == s_card) || (NULL == src) )
    {
        return KODE_DENIED;
    }
    if ( 0U == count )
    {
        return KODE_OK;
    }
    esp_err_t const err = sdmmc_write_sectors(s_card, src, (size_t)first,
                                              (size_t)count);
    if ( ESP_OK != err )
    {
        KLOG_W(TAG, "write %lu sectors at %lu: %s",
               (unsigned long)count, (unsigned long)first,
               esp_err_to_name(err));
        return KODE_ERROR;
    }
    return KODE_OK;
}
