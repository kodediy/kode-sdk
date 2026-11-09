/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kbsp_owners.h
 * @brief Who owns what: one owner per device, checked at startup.
 */
#ifndef KBSP_OWNERS_H_
#define KBSP_OWNERS_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Everything in kodeOS that can have exactly one owner. */
typedef enum {
    KBSP_RES_LED = 0,     /**< KTD2026 @0x30                                 */
    KBSP_RES_PANEL,       /**< MIPI-DSI + CO5300                             */
    KBSP_RES_TOUCH,       /**< CST820 @0x15                                  */
    KBSP_RES_IOEXP,       /**< TCA6408A @0x20 and its INT line (GPIO6)       */
    KBSP_RES_BTN_TOP,     /**< GPIO35: the only button on a P4 pin           */
    KBSP_RES_SDMMC,       /**< The microSD slot                              */
    KBSP_RES_FATFS,       /**< The internal `storage` partition              */
    KBSP_RES_OTA_SLOT,    /**< The inactive kodeOS slot (ota_0/ota_1)        */
    KBSP_RES_OTADATA,     /**< The otadata partition: who boots              */
    KBSP_RES_APPS_REGION, /**< The `apps` partition: directory and extents   */
    KBSP_RES_APPS_BOOTREQ, /**< The boot-request sector: which app boots next */
    KBSP_RES_APP_PENDING, /**< The `app_pending` mailbox partition           */
    KBSP_RES_PMIC,        /**< BQ25896 @0x6B                                 */
    KBSP_RES_GAUGE,       /**< CW2217 @0x64                                  */
    KBSP_RES_CONSOLE,     /**< The console transport                         */
    KBSP_RES_C5,          /**< The ESP32-C5: EN line, link, its GPIOs, flash */
    KBSP_RES_JPEG,        /**< The P4's JPEG codec                           */
    KBSP_RES_PPA,         /**< The P4's pixel accelerator                    */
    KBSP_RES_IR,          /**< The infrared emitters and receiver, on the C5 */
    KBSP_RES_NFC,         /**< ST25R3916B, its IRQ (GPIO29) and the SPI2 bus */
    KBSP_RES_AUDIO,       /**< ES8311 @0x18, mic, amp, speaker and the I2S   */
    KBSP_RES_IMU,         /**< QMI8658A @0x6A and QMC5883P @0x2C             */
    KBSP_RES_HAPTIC,      /**< AW86233 @0x5A on the antenna PCB              */
    KBSP_RES_RFID,        /**< The 125 kHz front end and its ADC             */
    KBSP_RES_EXP,         /**< The expansion header J3, all positions        */
    KBSP_RES_MAX
} kbsp_res_t;

/**
 * @brief Claim @p res for @p owner, atomically.
 *
 * @param res   The resource.
 * @param owner A stable pointer that identifies the owner, usually the AO.
 * @param name  The owner's name for `ps` and error messages.
 * @return true when @p owner holds @p res, now or already; false when another
 *         owner holds it or the arguments are invalid.
 */
bool kbsp_claim(kbsp_res_t res, const void *owner, const char *name);

/**
 * @brief Give @p res back. Ignored unless @p owner holds it.
 *
 * @param res   The resource.
 * @param owner The owner that claimed it.
 */
void kbsp_release(kbsp_res_t res, const void *owner);

/**
 * @brief Who holds @p res.
 *
 * @param res The resource.
 * @return The owner's name, "?" while it is being named, or NULL when free.
 */
const char *kbsp_owner_name(kbsp_res_t res);

/**
 * @brief The name of @p res itself.
 *
 * @param res The resource.
 * @return Its name, or "?" when @p res is out of range. Never NULL.
 */
const char *kbsp_res_name(kbsp_res_t res);

#ifdef __cplusplus
}
#endif

#endif /* KBSP_OWNERS_H_ */
