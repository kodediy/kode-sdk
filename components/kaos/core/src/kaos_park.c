/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kaos_park.c
 * @brief The parks: an owner the app did not link puts its part to sleep.
 */
#include "kaos.h"
#include "kaos_claim.h"

#include "kdrv_aw86233.h"
#include "kdrv_es8311.h"
#include "kdrv_led.h"
#include "kdrv_qmc5883p.h"
#include "kdrv_qmi8658.h"
#include "kdrv_cst820.h"
#include "kdrv_rfid_lf.h"
#include "kdrv_sd.h"
#include "kdrv_st25r3916.h"
#include "kdrv_tca6408a.h"
#include "khal_time.h"
#include "kode_log.h"
#include "sdkconfig.h"

#define TAG "kaos.park"

/* cppcheck-suppress misra-c2012-2.5 ; used under CONFIG_KODEOS_C5_RUN */
/** @brief The C5's claim while it runs for the app (CONFIG_KODEOS_C5_RUN). */
#define PARK_C5_FOR_APP "running for the app"
/** @brief The C5's claim while it runs for the speaker amplifier, its IO4. */
#define PARK_C5_FOR_AMP "running for the amp"

const uint8_t kaos_parked = 0U;

/* The owners the parks ask about, named weakly (KAOS_WEAK, kaos.h): naming
 * them here would link them into every app. */
KAOS_WEAK void kaos_audio_start(void);
KAOS_WEAK void kaos_display_start(void);
#if CONFIG_KODEOS_RADIO
KAOS_WEAK void kaos_radio_start(void);
KAOS_WEAK void radio_amp_request(bool on);
#endif

/**
 * @brief Claim @p res for an absent owner.
 * @param res  The resource.
 * @param name What `ps` and a double claim call it, `parked(AO_<x>)`.
 */
static void park_claim(kbsp_res_t res, const char *name)
{
    kaos_claim(res, &kaos_parked, name);
}

/**
 * @brief Say how a park went: a part that did not answer is not fitted, not a fault.
 * @param rc   What the driver answered.
 * @param part The part, for the line.
 */
static void park_said(kode_status_t rc, const char *part)
{
    if ( KODE_OK == rc )
    {
        KLOG_I(TAG, "%s parked", part);
    }
    else
    {
        KLOG_I(TAG, "%s not parked: no such part answered", part);
    }
}

void kaos_park_storage(void)
{
    park_claim(KBSP_RES_SDMMC, "parked(AO_Storage)");
    park_claim(KBSP_RES_FATFS, "parked(AO_Storage)");
    park_said(kdrv_sd_park(), "the card's supply");
}

void kaos_park_c5(void)
{
    const char *why = NULL;

/* cppcheck-suppress misra-c2012-20.9 ; sdkconfig.h leaves an option that is off undefined, and #if reads it as 0 */
#if CONFIG_KODEOS_C5_RUN
    /* The app talks to the C5 by a road of its own, from boot. */
    why = PARK_C5_FOR_APP;
#else
    if ( kaos_linked(&kaos_audio_start) )
    {
        /* The speaker amplifier's enable is the C5's IO4, which its firmware
         * raises: an app with sound and no AO_Radio keeps the C5 running. */
        why = PARK_C5_FOR_AMP;
    }
#endif
    if ( NULL != why )
    {
        park_claim(KBSP_RES_C5, why);
        /* A C5 kept from the last firmware may be in any state, IO4 low
         * included: reset it, so its firmware starts afresh. The pulse is
         * esp-hosted's own reset pulse, KDRV_RADIO_C5_PULSE_MS; the boot after
         * it takes KDRV_RADIO_C5_BOOT_MS (esp-hosted's SDIO reset delay), not
         * waited here: a sound inside it starts before the amplifier does. */
        if ( kdrv_radio_c5_hold(true) == KODE_OK )
        {
            khal_delay_ms(KDRV_RADIO_C5_PULSE_MS);
            (void)kdrv_radio_c5_hold(false);
            KLOG_I(TAG, "the C5 reset and left %s", why);
        }
        else
        {
            KLOG_W(TAG, "the C5 would not reset; it is as the last firmware "
                        "left it, %s", why);
        }
    }
    else
    {
        park_claim(KBSP_RES_C5, "parked(AO_Radio)");
        if ( kdrv_radio_c5_hold(true) != KODE_OK )
        {
            KLOG_W(TAG, "the C5 would not hold in reset; it runs on its own firmware");
        }
        else
        {
            KLOG_I(TAG, "the C5 parked: held in reset, the amplifier with it");
        }
    }
}

void kaos_park_led(void)
{
    park_claim(KBSP_RES_LED, "parked(AO_LedMgr)");
    park_said(kdrv_led_park(), "the KTD2026");
}

void kaos_park_ir(void)
{
    park_claim(KBSP_RES_IR, "parked(AO_Ir)");
}

void kaos_park_nfc(void)
{
    park_claim(KBSP_RES_NFC, "parked(AO_Nfc)");
    park_said(kdrv_st25r3916_park(), "the ST25R3916B");
}

void kaos_park_rfid(void)
{
    park_claim(KBSP_RES_RFID, "parked(AO_Rfid)");
    /* Its pins at rest, RFID_OUT idle low. Nothing on them answers, so a
     * front end that is not fitted cannot be told from one that is. */
    (void)kdrv_rfid_lf_init();
    KLOG_I(TAG, "the LF front end parked (fitted or not)");
}

void kaos_park_audio(void)
{
    park_claim(KBSP_RES_AUDIO, "parked(AO_Audio)");
    park_said(kdrv_es8311_park(), "the ES8311");
#if CONFIG_KODEOS_RADIO
    /* The amplifier's enable is the C5's IO4, which its firmware raises: with
     * AO_Radio here the C5 runs, so ask for it low. Without it the C5 is
     * parked in reset and the amplifier goes down with it. */
    if ( kaos_linked(&kaos_radio_start) )
    {
        radio_amp_request(false);
    }
#endif
}

void kaos_park_imu(void)
{
    park_claim(KBSP_RES_IMU, "parked(AO_Imu)");
    park_said(kdrv_qmi8658_park(), "the QMI8658A");
    park_said(kdrv_qmc5883p_park(), "the QMC5883P");
}

void kaos_park_haptic(void)
{
    park_claim(KBSP_RES_HAPTIC, "parked(AO_Haptic)");
    park_said(kdrv_aw86233_park(), "the AW86233");
}

void kaos_park_appmgr(void)
{
    park_claim(KBSP_RES_APPS_REGION, "parked(AO_AppMgr)");
    park_claim(KBSP_RES_APP_PENDING, "parked(AO_AppMgr)");
    park_claim(KBSP_RES_OTA_SLOT,    "parked(AO_AppMgr)");
}

void kaos_park_touch(void)
{
    park_claim(KBSP_RES_TOUCH, "parked(AO_Touch)");
    /* With AO_Display linked, its bring-up resets the touch on GPIO7 on its
     * own task, racing anything sent here: the chip is left to the auto-sleep
     * a reset gives it. Deep sleep only with no panel owner to undo it. */
    if ( !kaos_linked(&kaos_display_start) )
    {
        park_said(kdrv_cst820_park(), "the CST820");
    }
}

void kaos_park_display(void)
{
    park_claim(KBSP_RES_PANEL, "parked(AO_Display)");
#if CONFIG_KODEOS_MIRROR
    park_claim(KBSP_RES_JPEG, "parked(AO_Display)");
    park_claim(KBSP_RES_PPA,  "parked(AO_Display)");
#endif
    /* VCI_EN is P7, AO_IoExp's to drive: the level first, then the direction,
     * so a pin a cold power-on left an input never drives high on its way. */
    ioexp_field_request(NULL, FIELD_OP_SET, (uint16_t)KDRV_TCA_O7, 0);
    ioexp_field_request(NULL, FIELD_OP_SET, (uint16_t)KDRV_TCA_C7, 0);
    KLOG_I(TAG, "the panel parked: its supply asked off");
}
