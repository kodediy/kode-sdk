/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_gate.c
 * @brief The gate at the top of kode_init(): boot from OFF only on BOT, and only on a fit cell.
 */
#include "kode_gate.h"

#if CONFIG_KODE_LOWPOWER

#include <stdbool.h>
#include <stdint.h>

#include "kbsp.h"
#include "kdrv_bq25896.h"
#include "kdrv_cw2217.h"
#include "kdrv_led.h"
#include "kdrv_slot.h"
#include "kdrv_tca6408a.h"
#include "khal_power.h"
#include "khal_time.h"
#include "kode_log.h"

#define TAG "kode.gate"

/** @brief The expander's input byte with BOT's bit alone set. */
#define BOT_MASK  (1U << KBSP_IOEXP_BIT_BTN_BOT)
/** @brief The rest of the buttons on the expander's line. */
#define PAD_MASK  ((1U << KBSP_IOEXP_BIT_PAD_UP) | (1U << KBSP_IOEXP_BIT_PAD_LEFT) \
                   | (1U << KBSP_IOEXP_BIT_PAD_DOWN) | (1U << KBSP_IOEXP_BIT_PAD_RIGHT))

#define TAP_POLL_MS      20U    /**< Between two looks at the buttons after a short press. */
#define TAP_POLLS        50U    /**< How long BOT is watched: a second. */
#define TAP_LIT_POLLS    10U    /**< How long the pulse stays lit: 200 ms. */
#define TAP_BOT_READS    2U     /**< Reads in a row that make a press, not a glitch. */
#define TAP_GREEN        0x03U, 0x40U, 0x14U   /**< A dim green, about 1.3 mA: the wake costs far more. */
#define NOTE_PAD_DOWN    0x4B50ADU   /**< Went back to OFF with a pad button still down. */

#define GAUGE_MIN_MV     2000   /**< Below this the gauge has no cell to measure. */
#define PMIC_ADC_ZERO_MV 2304U  /**< BATV's code 0: a conversion that never ran, not a cell. */
#define ADC_POLL_MS      20U    /**< Between two looks at CONV_START. */
#define ADC_POLLS        55U    /**< SLUSC76C: tCONV is 8 ms to 1 s. */
#define SHIP_SETTLE_MS   100U   /**< Past this with the write done, something else holds VSYS up. */

#define FLAT_FLASHES     3U     /**< Three red: the battery is empty. */
#define FLAT_FLASH_MS    150U   /**< Each one lit, and the gap after it. */
#define FLAT_RED         0x40U  /**< About 1.3 mA of the sink's 5 mA. */

/**
 * @brief Read the expander and say which buttons are down.
 * @param[out] down BOT_MASK and PAD_MASK bits of the buttons pressed; untouched
 *                  unless true is returned.
 * @return true when the expander answered; false when it could not be read.
 */
static bool read_buttons(uint32_t *down)
{
    int32_t levels = 0;
    bool    read   = false;

    if ( (KODE_OK == kdrv_tca6408a_init())
        && (KODE_OK == kdrv_tca6408a_get(KDRV_TCA_INPUT, &levels)) )
    {
        /* Active low: the button pulls its pin to ground. */
        *down = (~(uint32_t)levels) & (BOT_MASK | PAD_MASK);
        read  = true;
    }
    return read;
}

/**
 * @brief Read the gauge and say what a flat cell would have to be under.
 *
 * @param[out] cut_mv The bar for this boot; untouched unless true is returned.
 * @param[out] mv     What the gauge read; untouched unless true is returned.
 * @return true when the gauge answered, is ready and reads under the bar.
 */
static bool gauge_low(uint16_t *cut_mv, int32_t *mv)
{
    int32_t ready = 0;
    int32_t read  = 0;
    bool    low   = false;

    if ( (KODE_OK == kdrv_cw2217_init())
        && (KODE_OK == kdrv_cw2217_get(KDRV_CW_READY, &ready)) && (0 != ready)
        && (KODE_OK == kdrv_cw2217_get(KDRV_CW_VCELL_MV, &read))
        && (GAUGE_MIN_MV < read) && ((int32_t)KBSP_CELL_DIP_CUT_MV > read) )
    {
        uint16_t bar = KBSP_CELL_CUT_MV;

        if ( (int)KDRV_RESET_BROWNOUT == kdrv_reset_reason() )
        {
            bar = KBSP_CELL_DIP_CUT_MV;
        }
        if ( (int32_t)bar > read )
        {
            *cut_mv = bar;
            *mv     = read;
            low     = true;
        }
    }
    return low;
}

/**
 * @brief Whether the PMIC says, in every way it can, that no cable is in.
 *
 * @return true only when every read landed and none of them saw an input, the
 *         chip's own boost or a charge.
 */
static bool pmic_no_cable(void)
{
    kdrv_bq25896_adc_t adc;
    int32_t            pg    = 0;
    int32_t            vstat = 0;
    int32_t            chrg  = 0;

    return (KODE_OK == kdrv_bq25896_get(KDRV_BQ_PG_STAT, &pg))
           && (KODE_OK == kdrv_bq25896_get(KDRV_BQ_VBUS_STAT, &vstat))
           && (KODE_OK == kdrv_bq25896_get(KDRV_BQ_CHRG_STAT, &chrg))
           && (KODE_OK == kdrv_bq25896_adc_read(&adc))
           && (0 == pg)
           && ((int32_t)KDRV_BQ_VBUS_NONE == vstat)
           && ((int32_t)KDRV_BQ_CHRG_NONE == chrg)
           && (!adc.vbus_gd);
}

/**
 * @brief Run one conversion of the PMIC's ADC to its end.
 *
 * @return true when CONV_START was seen to clear. Nothing older than this
 *         conversion may be read as a cell: the ADC at rest reads as 2304 mV.
 */
static bool pmic_convert(void)
{
    int32_t busy = 1;
    bool    ok   = (KODE_OK == kdrv_bq25896_set(KDRV_BQ_CONV_START, 1));

    for ( uint32_t i = 0U; ok && (0 != busy) && (ADC_POLLS > i); ++i )
    {
        khal_delay_ms(ADC_POLL_MS);
        ok = (KODE_OK == kdrv_bq25896_get(KDRV_BQ_CONV_START, &busy));
    }
    return ok && (0 == busy);
}

/**
 * @brief Whether this boot is on a cell that cannot carry it, with no cable.
 *
 * @param[out] mv The gauge's reading, for the line that says so.
 * @return true only when the gauge and a fresh conversion of the PMIC agree.
 */
static bool cell_flat(int32_t *mv)
{
    uint16_t cut = 0U;
    bool     flat = false;

    if ( gauge_low(&cut, mv) && (KODE_OK == kdrv_bq25896_init()) && pmic_no_cable()
        && pmic_convert() )
    {
        kdrv_bq25896_adc_t adc;

        /* Asked again: a cable may have come in during the conversion. */
        flat = pmic_no_cable() && (KODE_OK == kdrv_bq25896_adc_read(&adc))
               && (PMIC_ADC_ZERO_MV < adc.vbat_mv) && (cut > adc.vbat_mv);
    }
    return flat;
}

/** @brief Three red flashes, the only thing a person sees of an empty battery. */
static void flat_say(void)
{
    for ( uint32_t i = 0U; FLAT_FLASHES > i; ++i )
    {
        (void)kdrv_led_rgb(FLAT_RED, 0U, 0U);
        khal_delay_ms(FLAT_FLASH_MS);
        (void)kdrv_led_rgb(0U, 0U, 0U);
        khal_delay_ms(FLAT_FLASH_MS);
    }
}

/**
 * @brief Go to ship mode instead of booting, when the cell says so.
 *
 * Does not return on battery. A board still running after the write has a cable
 * that came in late, and a cable always boots: the battery path goes back, or
 * the cell would not charge.
 */
static void cell_gate(void)
{
    int32_t mv = 0;

    if ( cell_flat(&mv) )
    {
        KLOG_E(TAG, "the cell is at %ld mV with no cable: ship mode instead of a boot",
               (long)mv);
        flat_say();
        kode_status_t const rc = kdrv_bq25896_ship_mode(false);

        khal_delay_ms(SHIP_SETTLE_MS);
        KLOG_W(TAG, "still up after ship mode (%d): booting", (int)rc);
        (void)kdrv_bq25896_set(KDRV_BQ_BATFET_DIS, 0);
    }
}

/**
 * @brief Answer a press that was over before the gate could read it.
 *
 * The pulse, and a second of watching BOT with the holds still held. The last
 * look counts with one read: a person holding BOT from there on would otherwise
 * press a board that has just gone to sleep on a quiet line.
 *
 * @return true when the boot goes on: BOT came down, or the expander stopped
 *         answering, which is the gate's own rule.
 */
static bool tap_answer(void)
{
    uint32_t bot_reads = 0U;
    bool     go_on     = false;
    bool     stop      = false;

    (void)kdrv_led_rgb(TAP_GREEN);
    for ( uint32_t i = 0U; (!stop) && (TAP_POLLS > i); ++i )
    {
        uint32_t down = 0U;

        khal_delay_ms(TAP_POLL_MS);
        if ( TAP_LIT_POLLS == i )
        {
            (void)kdrv_led_rgb(0U, 0U, 0U);
        }
        if ( !read_buttons(&down) )
        {
            go_on = true;
            stop  = true;
        }
        else if ( 0U != (down & BOT_MASK) )
        {
            bot_reads++;
            if ( (TAP_BOT_READS <= bot_reads) || ((TAP_POLLS - 1U) == i) )
            {
                go_on = true;
                stop  = true;
            }
        }
        else
        {
            bot_reads = 0U;
            if ( 0U != down )
            {
                khal_power_off_note_set(NOTE_PAD_DOWN);
                stop = true;
            }
        }
    }
    (void)kdrv_led_rgb(0U, 0U, 0U);
    return go_on;
}

/**
 * @brief Decide a wake from OFF.
 * @return true when the boot goes on; false when the board goes back to OFF.
 */
static bool wake_decide(void)
{
    uint32_t   down  = 0U;
    bool       go_on = false;
    bool const noted = (NOTE_PAD_DOWN == khal_power_off_note());

    khal_power_off_note_set(0U);
    if ( !read_buttons(&down) )
    {
        KLOG_W(TAG, "woke from OFF and the expander would not answer: "
                    "booting anyway");
        go_on = true;
    }
    else if ( 0U != (down & BOT_MASK) )
    {
        /* Nothing to say: kaos.boot logs the cause of a boot from OFF. */
        go_on = true;
    }
    else if ( 0U != down )
    {
        khal_power_off_note_set(NOTE_PAD_DOWN);
    }
    else if ( !noted )
    {
        /* Before the light: a flat cell has no second to spend on it. */
        cell_gate();
        go_on = tap_answer();
    }
    else
    {
        /* The release of the pad button the last wake saw. */
    }
    return go_on;
}

void kode_boot_gate(void)
{
    bool go_on = true;

    if ( khal_power_woke_from_off() )
    {
        go_on = wake_decide();
    }

    if ( go_on )
    {
        cell_gate();
        khal_power_holds_release();
    }
    else
    {
        KLOG_N(TAG, "woke from OFF without BOT: back to OFF");
        khal_power_off_enter();   /* does not return on the board */
    }
}

#endif /* CONFIG_KODE_LOWPOWER */
