/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_rfid_lf.c
 * @brief The 125 kHz LF analog front end (see kdrv_rfid_lf.h).
 */
#include "kdrv_rfid_lf.h"

#include <inttypes.h>
#include <string.h>

#include "sdkconfig.h"

#include "driver/gpio.h"
#include "driver/parlio_tx.h"
#include "driver/pulse_cnt.h"
#include "esp_adc/adc_continuous.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "kbsp.h"
#include "khal_pwm.h"
#include "khal_system.h"
#include "krfid_mod.h"
#include "soc/soc_caps.h"
#include "kode_log.h"

/* kbsp names the demod line's ADC in IDF's numbers; this holds it to the
 * conversion mode, which names its unit outright. */
_Static_assert(KBSP_ADC_RFID == (uint32_t)ADC_UNIT_2,
               "KDRV_RFID_LF_ADC_CONV converts on ADC2 alone");

#define TAG "kdrv.rfid"

/** @brief The LF carrier frequency, in hertz. */
#define RFID_CARRIER_HZ       125000U

/** @brief Carrier duty resolution: the 8 bits the reference drives it at. */
#define RFID_CARRIER_BITS     8U

/** @brief Carrier duty, tenths of a percent: half, 128 of 256, a square wave. */
#define RFID_CARRIER_DUTY_X10 500U

/** @brief ADC sample rate: 80 kSPS, the P4 ADC's practical maximum. */
#define RFID_SAMPLE_HZ        80000U

/* One window is 12000 samples: 150 ms at 80 kSPS, which holds well over two
 * EM4100 frames (a frame is 64 bits, ~32.8 ms at RF/64). It is also the most
 * the decoder reads in one pass, so nothing sampled here is thrown away. */
#define RFID_WINDOW_SAMPLES   KRFID_EM4100_MAX_SAMPLES

/** @brief Samples per millisecond at the sample rate: 80. */
#define RFID_SAMPLES_PER_MS   (RFID_SAMPLE_HZ / 1000U)

/* The exported capture ceiling is written as a number in the header, so the
 * build checks it against the window it stands for: 150 ms x 80 = 12000. */
_Static_assert((KDRV_RFID_LF_CAPTURE_MS_MAX * RFID_SAMPLES_PER_MS)
               == (uint32_t)RFID_WINDOW_SAMPLES,
               "a capture is exactly one read window");

/** @brief The DMA read buffer, in bytes, drained per adc_continuous_read(). */
#define RFID_READ_BYTES       1024U

/** @brief Per-read DMA wait, in milliseconds, before the read is retried. */
#define RFID_READ_TIMEOUT_MS  1000U

/* A bound on the fill loop so a dead ADC cannot hang the job forever.
 * A healthy stream fills 12000 samples in ~50 reads; this stands many times
 * clear of that, and is only reached when the DMA delivers nothing - in which
 * case the window comes back short and decodes as flat (a timeout). */
#define RFID_READ_ATTEMPTS_MAX 512

/* ---- T5577 downlink timings (Flipper field-clocks x 8 us) ---------------- */

/** @brief Idle before and after a block program (field-clocks x 8 us). */
#define T5_WAIT_US            (400U * 8U)
/** @brief Start gap that opens a downlink burst. */
#define T5_STARTGAP_US        (30U  * 8U)
/** @brief Gap that separates two data bits. */
#define T5_WRITEGAP_US        (18U  * 8U)
/** @brief Carrier-on time that encodes a 0 bit. */
#define T5_DATA0_US           (24U  * 8U)
/** @brief Carrier-on time that encodes a 1 bit. */
#define T5_DATA1_US           (56U  * 8U)
/** @brief EEPROM program time the T5577 needs after a block's bits. */
#define T5_PROGRAM_US         (700U * 8U)
/** @brief T5577 config word for EM4100: Manchester | RF/64 | maxblock 2. */
#define T5_CONFIG_EM4100      0x00148040U

/**
 * @brief What one read needs and nothing else does: the demod window and the
 *        decoder's scratch space, 108 KB together.
 */
typedef struct {
    uint16_t            win[RFID_WINDOW_SAMPLES]; /**< The demod window.        */
    krfid_em4100_work_t work;                     /**< The decoder's scratch.   */
} rfid_bufs_t;

/** @brief The read buffers, in PSRAM, taken on demand and released with the
 *         field. Internal RAM cannot spare them: as statics they cost the boot
 *         its DMA pool. */
static rfid_bufs_t *s_bufs;

/** @brief The adc_continuous handle, created on demand, released with the field. */
static adc_continuous_handle_t s_adc;

/** @brief The VO4 channel handle, held only while a boosted field is up. */
static esp_ldo_channel_handle_t s_ldo;

/** @brief The carrier's channel from the first field to the next rest, else KHAL_PWM_NONE. */
static khal_pwm_t s_carrier = KHAL_PWM_NONE;

/** @brief Whether the carrier is currently driving RFID_OUT. */
static bool s_field;

/* ===== The receiver's pins ================================================ */

/** @brief RFID_PULL's level while the receiver is in use: Q2 off. */
#define PULL_LISTEN  0U
/** @brief RFID_PULL's level at rest: Q2 on, and no current through R11. */
#define PULL_REST    1U
/** @brief RFID_OUT's level with no carrier: the half-bridge's input low. */
#define OUT_IDLE     0U

/**
 * @brief Drive RFID_PULL to @p level.
 * @param level PULL_LISTEN or PULL_REST.
 */
static void pull_set(uint32_t level)
{
    if ( gpio_set_level((gpio_num_t)KBSP_PIN_RFID_PULL, level) != ESP_OK )
    {
        KLOG_W(TAG, "RFID_PULL would not move to %u", (unsigned)level);
    }
}

/**
 * @brief Make RFID_PULL a plain GPIO output, taking the pad back from any
 *        peripheral that routed it.
 *
 * A call that fails is logged: gpio_config() refuses only bad arguments.
 */
static void pull_pin_config(void)
{
    gpio_config_t const pull = {
        .pin_bit_mask = (uint64_t)1U << KBSP_PIN_RFID_PULL,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    if ( gpio_config(&pull) != ESP_OK )
    {
        KLOG_W(TAG, "RFID_PULL would not configure; the receiver is shunted");
    }
}

/**
 * @brief Make RFID_PULL an output at rest, RFID_OUT an output at OUT_IDLE and
 *        RFID_CARRIER a plain input.
 *
 * R11 (10 k, main PCB) pulls RFID_PULL up, and high turns Q2 (a MOSFET on the
 * antenna PCB) on, which shunts the envelope detector to ground: the receiver
 * hears nothing. LOW is the read and clone state, and it costs about 0.33 mA
 * through R11; HIGH costs nothing. So the pin rests high and goes low only
 * while something uses the receiver (pull_set(), kdrv_rfid_lf_rest()).
 * RFID_CARRIER is the coil's zero-crossing sense, pulled up on the board by R1:
 * the P4 reads it and never drives it.
 *
 * A call that fails is logged and the bring-up goes on: the pins belong to
 * this block alone, and gpio_config() refuses only bad arguments.
 */
static void pins_read_state(void)
{
    gpio_config_t const out = {
        .pin_bit_mask = (uint64_t)1U << KBSP_PIN_RFID_OUT,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    pull_pin_config();
    pull_set(PULL_REST);

    /* The level first, so the pad never drives anything else on its way. */
    if ( gpio_set_level((gpio_num_t)KBSP_PIN_RFID_OUT, OUT_IDLE) != ESP_OK )
    {
        KLOG_W(TAG, "RFID_OUT would not take its idle level");
    }
    if ( gpio_config(&out) != ESP_OK )
    {
        KLOG_W(TAG, "RFID_OUT would not configure; the half-bridge input floats");
    }

    gpio_config_t const sense = {
        .pin_bit_mask = (uint64_t)1U << KBSP_PIN_RFID_CARRIER,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    if ( gpio_config(&sense) != ESP_OK )
    {
        KLOG_W(TAG, "RFID_CARRIER would not configure as an input");
    }
}

/* ===== Carrier ============================================================ */

/**
 * @brief Take RFID_OUT out of the GPIO driver's reservation, still driven low.
 *
 * gpio_config() reserves an output pin, and LEDC warns when it is handed a
 * pin that is reserved. gpio_reset_pin() gives the reservation up and leaves
 * the pad undriven with its pull-up on; the pad is made an output again at
 * once by its direction alone, which reserves nothing. It is undriven for the
 * few instructions in between, and low from there until LEDC takes it.
 */
static void out_unreserve(void)
{
    (void)gpio_set_level((gpio_num_t)KBSP_PIN_RFID_OUT, OUT_IDLE);
    (void)gpio_reset_pin((gpio_num_t)KBSP_PIN_RFID_OUT);
    (void)gpio_set_direction((gpio_num_t)KBSP_PIN_RFID_OUT, GPIO_MODE_OUTPUT);
    (void)gpio_set_pull_mode((gpio_num_t)KBSP_PIN_RFID_OUT, GPIO_FLOATING);
}

/**
 * @brief Bring the 125 kHz carrier up on RFID_OUT, on a timer of its own.
 *
 * The drive strength is capped high so the half-bridge gets a clean edge.
 *
 * @return KODE_OK with the carrier running; KODE_NO_RESOURCES when the
 *         expansion header holds every LEDC timer; KODE_ERROR if LEDC refused.
 */
static kode_status_t carrier_start(void)
{
    const khal_pwm_cfg_t cfg = {
        .gpio      = (uint8_t)KBSP_PIN_RFID_OUT,
        .hz        = RFID_CARRIER_HZ,
        .bits      = RFID_CARRIER_BITS,
        .duty_x10  = RFID_CARRIER_DUTY_X10,
        .own_timer = true,
    };
    kode_status_t rc;

    if ( KHAL_PWM_NONE != s_carrier )
    {
        /* The channel stays with LEDC, silent, between the fields of a read:
         * only its duty comes back. */
        return khal_pwm_set(s_carrier, RFID_CARRIER_HZ, RFID_CARRIER_DUTY_X10);
    }
    /* At rest the pin is a GPIO output, reserved: LEDC takes it unreserved. */
    out_unreserve();
    rc = khal_pwm_open(&cfg, &s_carrier);
    if ( KODE_NO_RESOURCES == rc )
    {
        KLOG_E(TAG, "no LEDC timer free for the carrier: the expansion header holds them");
        return rc;
    }
    if ( KODE_OK != rc )
    {
        KLOG_E(TAG, "the carrier would not start (status %u)", (unsigned)rc);
        return rc;
    }
    (void)gpio_set_drive_capability((gpio_num_t)KBSP_PIN_RFID_OUT,
                                    GPIO_DRIVE_CAP_3);
    return KODE_OK;
}

/**
 * @brief Give the carrier's channel and its timer back, the pin to the GPIO driver.
 */
static void carrier_release(void)
{
    if ( KHAL_PWM_NONE != s_carrier )
    {
        khal_pwm_close(s_carrier);
        s_carrier = KHAL_PWM_NONE;
    }
}

/**
 * @brief Silence the carrier: RFID_OUT held idle low by its channel, which stays open.
 *
 * A channel that will not go quiet is let go instead; the pin is then idle low
 * as a GPIO.
 */
static void carrier_stop(void)
{
    if ( (KHAL_PWM_NONE != s_carrier)
        && (khal_pwm_set(s_carrier, RFID_CARRIER_HZ, 0U) != KODE_OK) )
    {
        carrier_release();
    }
}

/* ===== VO4 (write boost) ================================================== */

/**
 * @brief Ensure VDD_IO_5 is at 3.3 V through VO4 for a clone.
 *
 * VO4 is `esp_ldo` channel 4, and the SDMMC IO domain owns that same channel
 * (`KBSP_SD_LDO_CHAN` = 4) as an *adjustable* reference for as long as a card is
 * mounted. `esp_ldo` allows no second acquirer of an adjustable channel at all,
 * so on a running kodeOS the clone cannot own VO4 - and does not need to: the SD
 * already holds VDD_IO_5 at the SDMMC IO voltage (3.3 V for a standard card), so
 * `RFID_OUT`'s bank is already at full strength. Two cases, and the acquire
 * result tells them apart:
 *   - it succeeds  -> no card held the channel; this brought the rail up fresh
 *     at 3300 mV and holds a reference (released in ldo_release()).
 *   - it fails     -> the SDMMC domain holds the rail; the clone shares it as it
 *     stands and owns nothing (s_ldo stays NULL, ldo_release() a no-op).
 * Either way the field is boosted, so this never fails the clone. A card driven
 * below 3.3 V (a UHS 1.8 V rail) would leave the field weak; that is not a
 * configuration this board ships, and a write that does not take is already
 * caught by the clone's read-back.
 *
 * @return KODE_OK. The rail is at 3.3 V, whether this raised it or the SD did.
 */
static kode_status_t ldo_acquire(void)
{
    if ( NULL != s_ldo )
    {
        return KODE_OK;
    }

    esp_ldo_channel_config_t const cfg = {
        .chan_id          = KDRV_RFID_LF_LDO_CHANNEL,
        .voltage_mv       = KDRV_RFID_LF_LDO_MV,
        .flags.adjustable = false,
    };
    if ( esp_ldo_acquire_channel(&cfg, &s_ldo) != ESP_OK )
    {
        /* The SDMMC IO domain already holds VO4 at 3.3 V; share it as it is. */
        KLOG_I(TAG, "VO4 held by the SDMMC domain; clone shares the 3.3 V rail");
        s_ldo = NULL;
    }
    return KODE_OK;
}

/**
 * @brief Release VO4 if it is held, dropping VDD_IO_5 back to its boot level.
 */
static void ldo_release(void)
{
    if ( NULL != s_ldo )
    {
        (void)esp_ldo_release_channel(s_ldo);
        s_ldo = NULL;
    }
}

/* ===== Reader (adc_continuous DMA) ======================================== */

/**
 * @brief Create the adc_continuous handle on the demod channel, once.
 *
 * @return KODE_OK with the handle ready; KODE_ERROR if it would not create or
 *         configure.
 */
static kode_status_t adc_ensure(void)
{
    if ( NULL != s_adc )
    {
        return KODE_OK;
    }

    adc_continuous_handle_cfg_t const hcfg = {
        .max_store_buf_size = 16384,
        .conv_frame_size    = RFID_READ_BYTES,
    };
    if ( adc_continuous_new_handle(&hcfg, &s_adc) != ESP_OK )
    {
        KLOG_E(TAG, "the ADC-continuous handle would not create");
        s_adc = NULL;
        return KODE_ERROR;
    }

    /* Not const: adc_continuous_config_t.adc_pattern is a non-const pointer. */
    adc_digi_pattern_config_t pat = {
        .atten     = ADC_ATTEN_DB_12,
        .channel   = KDRV_RFID_LF_ADC_CHANNEL,
        .unit      = KDRV_RFID_LF_ADC_UNIT,
        .bit_width = ADC_BITWIDTH_12,
    };
    adc_continuous_config_t const ccfg = {
        .sample_freq_hz = RFID_SAMPLE_HZ,
        .conv_mode      = KDRV_RFID_LF_ADC_CONV,
        .format         = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
        .pattern_num    = 1,
        .adc_pattern    = &pat,
    };
    if ( adc_continuous_config(s_adc, &ccfg) != ESP_OK )
    {
        KLOG_E(TAG, "the ADC-continuous config was refused");
        (void)adc_continuous_deinit(s_adc);
        s_adc = NULL;
        return KODE_ERROR;
    }
    return KODE_OK;
}

/**
 * @brief Delete the adc_continuous handle on ADC2, so nothing of the ADC is
 *        held between requests.
 */
static void adc_release(void)
{
    if ( NULL != s_adc )
    {
        (void)adc_continuous_deinit(s_adc);
        s_adc = NULL;
    }
}

/**
 * @brief Take the read buffers from PSRAM, once.
 *
 * @return KODE_OK with the buffers ready; KODE_NO_RESOURCES if PSRAM refused.
 */
static kode_status_t bufs_ensure(void)
{
    if ( NULL != s_bufs )
    {
        return KODE_OK;
    }

    s_bufs = (rfid_bufs_t *)khal_alloc_psram(sizeof(uint32_t), sizeof(*s_bufs));
    if ( NULL == s_bufs )
    {
        KLOG_E(TAG, "no PSRAM for the read buffers (%u bytes)",
               (unsigned)sizeof(*s_bufs));
        return KODE_NO_RESOURCES;
    }
    return KODE_OK;
}

/**
 * @brief Hand the read buffers back.
 */
static void bufs_release(void)
{
    if ( NULL != s_bufs )
    {
        khal_free(s_bufs);
        s_bufs = NULL;
    }
}

/**
 * @brief Whether one DMA result is a valid conversion of RFID_RF_IN, and its
 *        count when it is.
 *
 * IDF's continuous driver tags every result with the unit and channel that
 * produced it, because its pattern table interleaves as many channels as it
 * is given, and it documents a channel field out of range as an invalid
 * result. The pattern here has one entry, so anything that is not a valid
 * RFID_RF_IN conversion is not a reading of the demod line, and taken into
 * the window it would pass for one - in the statistics, the waveform and the
 * decoder alike. The result is decoded by adc_continuous_parse_data() rather
 * than by its bit fields, because the raw field is not the channel number on
 * this chip: ADC2's channels come back offset by two, which the parser
 * removes.
 *
 * @param raw   One result, SOC_ADC_DIGI_RESULT_BYTES long, as the DMA wrote it.
 * @param count Out: its 12-bit count, written only when it is kept.
 * @return true when the result is valid and is KDRV_RFID_LF_ADC_UNIT's
 *         KDRV_RFID_LF_ADC_CHANNEL.
 */
static bool adc_keep(const uint8_t *raw, uint16_t *count)
{
    adc_continuous_data_t r;
    uint32_t              parsed = 0U;

    if ( adc_continuous_parse_data(s_adc, raw, SOC_ADC_DIGI_RESULT_BYTES, &r,
                                  &parsed) != ESP_OK )
    {
        return false;
    }
    if ( (1U != parsed) || !r.valid || (KDRV_RFID_LF_ADC_UNIT != r.unit)
        || (KDRV_RFID_LF_ADC_CHANNEL != r.channel) )
    {
        return false;
    }
    *count = (uint16_t)r.raw_data;
    return true;
}

/**
 * @brief Fill the start of the demod window from the demod line by DMA.
 *
 * Starts the conversion, drains the results into the window until @p want
 * samples are in or the attempt bound is reached, then stops. Only valid
 * conversions of RFID_RF_IN are kept (adc_keep()); the rest are dropped and
 * do not count toward the fill, and how many were dropped is logged at debug
 * level. The bound only bites when the DMA delivers nothing the window can
 * keep; a healthy stream fills the request and returns the full count.
 *
 * @param want How many samples to take. More than the window holds takes the
 *             whole window; 0 takes none and starts nothing.
 * @return The number of samples written to the window (0 on a failed start).
 */
static size_t adc_fill(size_t want)
{
    static uint8_t rd[RFID_READ_BYTES];
    size_t         limit    = want;
    size_t         filled   = 0U;
    size_t         dropped  = 0U;
    int            attempts = 0;

    if ( (size_t)RFID_WINDOW_SAMPLES < limit )
    {
        limit = (size_t)RFID_WINDOW_SAMPLES;
    }
    if ( 0U == limit )
    {
        return 0U;
    }
    if ( adc_continuous_start(s_adc) != ESP_OK )
    {
        return 0U;
    }

    while ( (filled < limit) && (RFID_READ_ATTEMPTS_MAX > attempts) )
    {
        uint32_t got = 0;

        ++attempts;
        if ( adc_continuous_read(s_adc, rd, sizeof rd, &got,
                                RFID_READ_TIMEOUT_MS) != ESP_OK )
        {
            continue;
        }

        for ( uint32_t i = 0U;
             ((i + (uint32_t)SOC_ADC_DIGI_RESULT_BYTES) <= got)
             && (filled < limit);
             i += (uint32_t)SOC_ADC_DIGI_RESULT_BYTES )
        {
            uint16_t count = 0U;

            if ( adc_keep(&rd[i], &count) )
            {
                s_bufs->win[filled] = count;
                ++filled;
            }
            else
            {
                ++dropped;
            }
        }
    }

    (void)adc_continuous_stop(s_adc);
    if ( 0U < dropped )
    {
        KLOG_D(TAG, "window: %u results kept, %u dropped (invalid or not "
               "RFID_RF_IN)", (unsigned)filled, (unsigned)dropped);
    }
    return filled;
}

/* ===== T5577 writer (field gapping) ======================================= */

/**
 * @brief Open a timed gap in the carrier by pausing its timer.
 *
 * @param us How long the field is down, in microseconds.
 */
static void field_gap(uint32_t us)
{
    (void)khal_pwm_pause(s_carrier);
    esp_rom_delay_us(us);
    (void)khal_pwm_resume(s_carrier);
}

/**
 * @brief Send one T5577 data bit: carrier on for its width, then a write gap.
 *
 * @param v true for a 1 bit, false for a 0 bit.
 */
static void t5_bit(bool v)
{
    if ( v )
    {
        esp_rom_delay_us(T5_DATA1_US);
    }
    else
    {
        esp_rom_delay_us(T5_DATA0_US);
    }
    field_gap(T5_WRITEGAP_US);
}

/**
 * @brief Send a 2-bit T5577 opcode, most-significant bit first.
 *
 * @param op The opcode in its low two bits.
 */
static void t5_opcode(uint8_t op)
{
    t5_bit(((op >> 1) & 1U) != 0U);
    t5_bit((op & 1U) != 0U);
}

/**
 * @brief Send the T5577 reset sequence.
 */
static void t5_reset(void)
{
    field_gap(T5_STARTGAP_US);
    t5_bit(true);
    t5_bit(false);
}

/**
 * @brief Program one 32-bit T5577 block on page 0.
 *
 * @param block The block index (0..7).
 * @param data  The 32-bit word to write.
 */
static void t5_write_block(uint8_t block, uint32_t data)
{
    esp_rom_delay_us(T5_WAIT_US);
    field_gap(T5_STARTGAP_US);
    t5_opcode(0x2U);                 /* opcode 0b10: page 0 */
    t5_bit(false);                   /* lock bit */
    for ( int i = 0; 32 > i; ++i )
    {
        t5_bit(((data >> (31 - i)) & 1U) != 0U);
    }
    t5_bit(((block >> 2) & 1U) != 0U);
    t5_bit(((block >> 1) & 1U) != 0U);
    t5_bit((block & 1U) != 0U);
    esp_rom_delay_us(T5_PROGRAM_US);
    esp_rom_delay_us(T5_WAIT_US);
    t5_reset();
}

/* ===== The certification carrier (FCC 15.209, cert app only) ============== *
 * A held maximum-drive carrier for the FCC bench. Unlike a read or a clone it
 * carries no window and no worker job: AO_Rfid brings it up and holds it until
 * a stop. The modulated variant OOK-gates the carrier from a driver-internal
 * task - the front end has no modulation primitive of its own, so this is the
 * fixed-pattern fallback, exactly as the NFC reader's cert carrier does.
 */

#if CONFIG_KODEOS_NFC_CERT

/** @brief The OOK task runs low: its gating need not be crisp, and the glass
 *         and the console must stay responsive while the field is held. */
#define RFID_CERT_MOD_PRIO     4U
/** @brief One gating loop; it needs no depth. */
#define RFID_CERT_MOD_STACK    2048U
/** @brief Half an OOK period, in microseconds: ~1.25 kHz gating, in the EM4100
 *         bit-rate ballpark, so the sidebands land where a 125 kHz occupied-
 *         bandwidth measurement expects them. */
#define RFID_CERT_OOK_HALF_US  400U
/** @brief Gate cycles per burst before the task yields a tick. Bounds the
 *         busy-wait so lower priorities and the idle task still run. */
#define RFID_CERT_OOK_BURST    4U
/** @brief Steps of 2 ms field_off() waits for the OOK task to exit. */
#define RFID_CERT_JOIN_STEPS   100U

/** @brief The OOK loop's on switch. */
static volatile bool s_cert_mod_run;
/** @brief The OOK task, NULL when no loop is running. */
static TaskHandle_t  s_cert_mod_task;

/**
 * @brief OOK-gate the carrier until told to stop.
 *
 * Each burst pauses and resumes the LEDC timer a few times - carrier off for
 * one half-period, on for the next - then yields a tick, across which the
 * carrier stays up at full drive. That leaves the OOK sidebands on the spectrum
 * between bursts, the fixed-pattern fallback this front end allows since it has
 * no modulation primitive of its own.
 *
 * @param arg Unused.
 */
static void cert_mod_task(void *arg)
{
    (void)arg;

    while ( s_cert_mod_run )
    {
        for ( uint32_t i = 0U; (RFID_CERT_OOK_BURST > i) && s_cert_mod_run; ++i )
        {
            (void)khal_pwm_pause(s_carrier);
            esp_rom_delay_us(RFID_CERT_OOK_HALF_US);
            (void)khal_pwm_resume(s_carrier);
            esp_rom_delay_us(RFID_CERT_OOK_HALF_US);
        }
        vTaskDelay(1);
    }

    s_cert_mod_task = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Stop the OOK loop, if one is running. A no-op otherwise.
 *
 * Called from kdrv_rfid_lf_field_off() before the carrier is taken down, so the
 * task has stopped touching the LEDC timer by the time the carrier stops.
 */
static void cert_carrier_stop(void)
{
    if ( NULL != s_cert_mod_task )
    {
        s_cert_mod_run = false;
        uint32_t step = 0U;
        while ( (NULL != s_cert_mod_task) && (RFID_CERT_JOIN_STEPS > step) )
        {
            vTaskDelay(pdMS_TO_TICKS(2));
            step++;
        }
    }
}

kode_status_t kdrv_rfid_lf_cert_carrier(bool modulated)
{
    /* Start from a clean field: a prior OOK loop is joined and the carrier
     * dropped, so switching carrier<->modulated leaves no task behind. */
    (void)kdrv_rfid_lf_field_off();

    /* Boosted, for the most drive the half-bridge can give (VDD_IO_5 at 3.3 V
     * where the SDMMC domain does not already hold it). */
    kode_status_t rc = kdrv_rfid_lf_field_on(true);
    if ( KODE_OK != rc )
    {
        return rc;
    }

    if ( modulated )
    {
        s_cert_mod_run = true;
        if ( xTaskCreate(&cert_mod_task, "rfid_cert", RFID_CERT_MOD_STACK, NULL,
                        RFID_CERT_MOD_PRIO, &s_cert_mod_task) != pdPASS )
        {
            KLOG_E(TAG, "no task for the RFID certification modulation loop");
            s_cert_mod_run = false;
            (void)kdrv_rfid_lf_field_off();
            return KODE_ERROR;
        }
        KLOG_N(TAG, "certification carrier up: 125 kHz, max drive, OOK modulated");
    }
    else
    {
        KLOG_N(TAG, "certification carrier up: 125 kHz, max drive, unmodulated");
    }
    return KODE_OK;
}

#endif /* CONFIG_KODEOS_NFC_CERT */

/* ===== Public API ========================================================= */

kode_status_t kdrv_rfid_lf_init(void)
{
    carrier_release();   /* the pin is about to be a GPIO output again */
    pins_read_state();
    s_field = false;
    return KODE_OK;
}

kode_status_t kdrv_rfid_lf_field_on(bool boost)
{
    pull_set(PULL_LISTEN);
    kode_status_t rc = carrier_start();
    if ( KODE_OK != rc )
    {
        return rc;
    }

    if ( boost )
    {
        rc = ldo_acquire();
        if ( KODE_OK != rc )
        {
            carrier_stop();
            return rc;
        }
    }

    s_field = true;
    return KODE_OK;
}

kode_status_t kdrv_rfid_lf_field_off(void)
{
#if CONFIG_KODEOS_NFC_CERT
    /* Join any held certification OOK loop before the carrier stops, so the
     * task is not left pausing a timer that is going away. A no-op for every
     * ordinary read or clone. */
    cert_carrier_stop();
#endif
    carrier_stop();
    adc_release();
    bufs_release();
    ldo_release();
    s_field = false;
    return KODE_OK;
}

kode_status_t kdrv_rfid_lf_read_window(uint8_t id[5], krfid_em4100_diag_t *d)
{
    if ( NULL == id )
    {
        return KODE_ERROR;
    }

    kode_status_t rc = bufs_ensure();
    if ( KODE_OK != rc )
    {
        return rc;
    }
    rc = adc_ensure();
    if ( KODE_OK != rc )
    {
        return rc;
    }

    /* At most the window's 12000 samples, so the count fits the decoder's int. */
    int const n = (int)adc_fill((size_t)RFID_WINDOW_SAMPLES);
    krfid_em4100_diag_t local_d;
    krfid_em4100_diag_t *dd = d;
    if ( NULL == dd )
    {
        dd = &local_d;
    }
    int const r = krfid_em4100_decode(s_bufs->win, n, RFID_SAMPLE_HZ,
                                      &s_bufs->work, id, dd);
    if ( 0 == r )
    {
        return KODE_OK;
    }
    /* -1 (flat) and -2 (signal, no clean frame) are both "no tag this window"
     * to the caller, which retries or lets the deadline end it. */
    return KODE_TIMEOUT;
}

kode_status_t kdrv_rfid_lf_clone(const uint8_t id[5])
{
    if ( NULL == id )
    {
        return KODE_ERROR;
    }
    if ( !s_field )
    {
        return KODE_DENIED;
    }

    uint64_t const frame = krfid_em4100_frame(id);
    uint32_t const b1 = (uint32_t)(frame >> 32);
    uint32_t const b2 = (uint32_t)frame;

    KLOG_N(TAG, "clone EM4100 %02X %02X %02X %02X %02X (cfg=%08lX b1=%08lX b2=%08lX)",
           id[0], id[1], id[2], id[3], id[4],
           (unsigned long)T5_CONFIG_EM4100, (unsigned long)b1, (unsigned long)b2);

    /* One tick between blocks so priorities beneath the worker breathe; the
     * per-block burst itself is the irreducible real-time part. */
    t5_write_block(0, T5_CONFIG_EM4100);
    vTaskDelay(pdMS_TO_TICKS(1));
    t5_write_block(1, b1);
    vTaskDelay(pdMS_TO_TICKS(1));
    t5_write_block(2, b2);
    vTaskDelay(pdMS_TO_TICKS(1));
    t5_reset();

    return KODE_OK;
}

/* ===== The jobs ============================================================ *
 * The blocking primitives above run here, off the AO's thread: AO_Rfid hands
 * kdrv_rfid_lf_run() to its worker as a job. The driver keeps no task, queue
 * or running flag of its own - one job in flight is the AO's rule, and the
 * only thing the driver must know about the worker is which task it is, so
 * that a capture is refused anywhere else.
 */

/** @brief Whether the caller is the AO's worker; set by the AO, NULL before. */
static bool (*s_is_worker)(void);

void kdrv_rfid_lf_set_worker_check(bool (*is_worker)(void))
{
    s_is_worker = is_worker;
}

/**
 * @brief Run one READ job: energise, read windows until a decode or @p ms, drop.
 *
 * @param ms     How long to keep looking.
 * @param id     Out: the decoded ID on success.
 * @param rf_div Out: the bit rate it was read at, 0 when none.
 * @return KODE_OK with @p id; KODE_TIMEOUT when nothing decoded in @p ms; the
 *         driver's own error when the carrier or a window could not be had.
 */
static kode_status_t job_read(uint32_t ms, uint8_t id[5], uint8_t *rf_div)
{
    krfid_em4100_diag_t d;
    kode_status_t       rc = KODE_TIMEOUT;
    TickType_t const    start = xTaskGetTickCount();
    TickType_t const    span  = pdMS_TO_TICKS(ms);

    rc = kdrv_rfid_lf_field_on(false);
    if ( KODE_OK != rc )
    {
        /* No carrier: a window would only sample a dead coil until the deadline. */
        return rc;
    }
    do
    {
        (void)memset(&d, 0, sizeof(d));
        rc = kdrv_rfid_lf_read_window(id, &d);
        if ( KODE_OK == rc )
        {
            *rf_div = (uint8_t)d.bitrate_div;
            break;
        }
        if ( KODE_TIMEOUT != rc )
        {
            break;   /* no buffers or no ADC: another window would fail the same */
        }
    } while ( (xTaskGetTickCount() - start) < span );
    (void)kdrv_rfid_lf_field_off();
    return rc;
}

/**
 * @brief Run one CLONE job: boost, write, read one window back to verify, drop.
 *
 * @param src    The 5-byte ID to program.
 * @param id     Out: what the verify read decoded, for the AO to compare.
 * @param rf_div Out: the bit rate the read-back came at, 0 when none.
 * @return KODE_OK when the read-back decoded; the carrier's error when the
 *         field would not come up; the clone's error otherwise.
 */
static kode_status_t job_clone(const uint8_t src[5], uint8_t id[5],
                               uint8_t *rf_div)
{
    krfid_em4100_diag_t d;
    kode_status_t       rc;

    rc = kdrv_rfid_lf_field_on(true);
    if ( KODE_OK != rc )
    {
        return rc;
    }
    rc = kdrv_rfid_lf_clone(src);
    if ( KODE_OK == rc )
    {
        (void)memset(&d, 0, sizeof(d));
        rc = kdrv_rfid_lf_read_window(id, &d);
        if ( KODE_OK == rc )
        {
            *rf_div = (uint8_t)d.bitrate_div;
        }
    }
    (void)kdrv_rfid_lf_field_off();
    return rc;
}

/* ===== Emulation: an EM4100 tag looped on RFID_PULL by the PARLIO ======== */

/** @brief Reader class: the 125 kHz field, in hertz. */
#define RFID_EMU_CLASS_125K_HZ   125000U
/** @brief Reader class: the 134.2 kHz field, in hertz. */
#define RFID_EMU_CLASS_134K_HZ   134200U
/** @brief Reader class: the 62.5 kHz field, in hertz. */
#define RFID_EMU_CLASS_62K_HZ    62500U
/** @brief Lowest field counted as the 125 kHz class, in hertz. */
#define RFID_EMU_125K_MIN_HZ     120000U
/** @brief First field above the 125 kHz class, in hertz. */
#define RFID_EMU_125K_END_HZ     130000U
/** @brief Highest field counted as the 134.2 kHz class, in hertz. */
#define RFID_EMU_134K_MAX_HZ     138000U
/** @brief Lowest field counted as the 62.5 kHz class, in hertz. */
#define RFID_EMU_62K_MIN_HZ      60000U
/** @brief Highest field counted as the 62.5 kHz class, in hertz. */
#define RFID_EMU_62K_MAX_HZ      65000U
/** @brief The window RFID_CARRIER's edges are counted over, in milliseconds. */
#define RFID_EMU_COUNT_MS        10U
/** @brief Milliseconds in a second. */
#define RFID_EMU_MS_PER_S        1000U
/** @brief PCNT counter ceiling; 10 ms of the fastest class is 1380 edges. */
#define RFID_EMU_PCNT_HIGH       32767
/** @brief PCNT counter floor; IDF wants a negative one. */
#define RFID_EMU_PCNT_LOW        (-1)
/** @brief Bits per carrier period on the internal clock: XTAL/160 is 250 kHz. */
#define RFID_EMU_INT_REP         2U
/** @brief Periods per bit-clock tick on the 62.5 kHz class. */
#define RFID_EMU_HALF_DIV        2U
/** @brief Bits per byte of the DMA buffer. */
#define RFID_EMU_BYTE_BITS       8U
/** @brief The most copies of the loop that make it whole bytes. */
#define RFID_EMU_COPIES_MAX      8U
/** @brief PARLIO transfers that may queue. */
#define RFID_EMU_QUEUE_DEPTH     4U
/** @brief PARLIO data lines in use. */
#define RFID_EMU_DATA_WIDTH      1U
/** @brief The modulator level that sets a DMA bit (EM4100 polarity). */
#define RFID_EMU_BIT_LEVEL       1U
/** @brief The most bits the DMA buffer holds: the loop on the internal clock,
 *         two bits a period. */
#define RFID_EMU_MAX_BITS        (KRFID_MOD_EM4100_LEVELS * RFID_EMU_INT_REP)

/** @brief The PARLIO unit while a tag plays, else NULL. */
static parlio_tx_unit_handle_t s_emu_tx;
/** @brief The DMA bit buffer while a tag plays, else NULL. */
static uint8_t *s_emu_buf;
/** @brief The PARLIO is clocking a tag out. */
static bool s_emu_on;

/**
 * @brief The reader class an RFID_CARRIER reading falls in.
 *
 * @param ext_hz What the squared field shows on RFID_CARRIER, in hertz.
 * @return 125000, 134200 or 62500; 0 when it is no reader (use the internal clock).
 */
static uint32_t emu_class_hz(uint32_t ext_hz)
{
    if ( (RFID_EMU_125K_MIN_HZ <= ext_hz) && (RFID_EMU_125K_END_HZ > ext_hz) )
    {
        return RFID_EMU_CLASS_125K_HZ;
    }
    if ( (RFID_EMU_125K_END_HZ <= ext_hz) && (RFID_EMU_134K_MAX_HZ >= ext_hz) )
    {
        return RFID_EMU_CLASS_134K_HZ;
    }
    if ( (RFID_EMU_62K_MIN_HZ <= ext_hz) && (RFID_EMU_62K_MAX_HZ >= ext_hz) )
    {
        return RFID_EMU_CLASS_62K_HZ;
    }
    return 0U;
}

/**
 * @brief Count RFID_CARRIER's edges over RFID_EMU_COUNT_MS.
 *
 * @return The frequency in hertz; 0 without a PCNT unit.
 */
static uint32_t emu_field_hz(void)
{
    pcnt_unit_config_t const pc = {
        .high_limit        = RFID_EMU_PCNT_HIGH,
        .low_limit         = RFID_EMU_PCNT_LOW,
        .flags.accum_count = 1,
    };
    pcnt_chan_config_t const cc = {
        .edge_gpio_num  = (int)KBSP_PIN_RFID_CARRIER,
        .level_gpio_num = -1,
    };
    pcnt_unit_handle_t    unit  = NULL;
    pcnt_channel_handle_t ch    = NULL;
    uint32_t              edges = 0U;
    int                   raw   = 0;   /* IDF's type; converted below */

    if ( pcnt_new_unit(&pc, &unit) != ESP_OK )
    {
        KLOG_W(TAG, "no PCNT unit: the reader's field is not measured");
        return 0U;
    }
    if ( pcnt_new_channel(unit, &cc, &ch) != ESP_OK )
    {
        KLOG_W(TAG, "no PCNT channel on RFID_CARRIER");
        (void)pcnt_del_unit(unit);
        return 0U;
    }
    (void)pcnt_channel_set_edge_action(ch, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                       PCNT_CHANNEL_EDGE_ACTION_HOLD);
    (void)pcnt_unit_enable(unit);
    (void)pcnt_unit_clear_count(unit);
    (void)pcnt_unit_start(unit);
    vTaskDelay(pdMS_TO_TICKS(RFID_EMU_COUNT_MS));
    (void)pcnt_unit_get_count(unit, &raw);
    (void)pcnt_unit_stop(unit);
    (void)pcnt_unit_disable(unit);
    (void)pcnt_del_channel(ch);
    (void)pcnt_del_unit(unit);

    if ( 0 < raw )
    {
        edges = (uint32_t)raw;
    }
    return edges * (RFID_EMU_MS_PER_S / RFID_EMU_COUNT_MS);
}

/** @brief Delete the unit and buffer, drive RFID_PULL low, release VO4. */
static void emu_release(void)
{
    if ( NULL != s_emu_tx )
    {
        (void)parlio_del_tx_unit(s_emu_tx);
        s_emu_tx = NULL;
    }
    if ( NULL != s_emu_buf )
    {
        heap_caps_free(s_emu_buf);
        s_emu_buf = NULL;
    }
    pull_pin_config();
    pull_set(PULL_LISTEN);
    ldo_release();   /* hand VO4 back; the SD keeps its own hold */
}

/**
 * @brief Loop @c s_emu_buf out on RFID_PULL.
 *
 * @param ext   true to clock from RFID_CARRIER, false from XTAL.
 * @param hz    The carrier rate, in hertz.
 * @param rep   Bits per carrier period.
 * @param nbits The bits in @c s_emu_buf.
 * @return KODE_OK with the PARLIO looping; KODE_ERROR, all released.
 */
static kode_status_t emu_play(bool ext, uint32_t hz, uint32_t rep,
                              uint32_t nbits)
{
    parlio_tx_unit_config_t cfg = {
        .data_width        = RFID_EMU_DATA_WIDTH,
        .data_gpio_nums    = { (int)KBSP_PIN_RFID_PULL, -1, -1, -1, -1, -1,
                               -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
        .clk_out_gpio_num  = -1,
        .valid_gpio_num    = -1,
        .trans_queue_depth = RFID_EMU_QUEUE_DEPTH,
        .max_transfer_size = nbits / RFID_EMU_BYTE_BITS,
        .shift_edge        = PARLIO_SHIFT_EDGE_NEG,
        .bit_pack_order    = PARLIO_BIT_PACK_ORDER_LSB,
    };
    parlio_transmit_config_t const tc = {
        .idle_value              = 0U,
        .flags.loop_transmission = 1U,
    };
    esp_err_t err;

    /* Drive RFID_PULL at 3.3 V: take VO4, or share the SD's hold. Every path
     * from here down rests through emu_release(), which hands it back. */
    (void)ldo_acquire();

    if ( ext )
    {
        cfg.clk_src               = PARLIO_CLK_SRC_EXTERNAL;
        cfg.clk_in_gpio_num       = (int)KBSP_PIN_RFID_CARRIER;
        cfg.input_clk_src_freq_hz = hz;
        cfg.output_clk_freq_hz    = hz * rep;
    }
    else
    {
        cfg.clk_src               = PARLIO_CLK_SRC_XTAL;
        cfg.clk_in_gpio_num       = -1;
        cfg.input_clk_src_freq_hz = 0U;
        cfg.output_clk_freq_hz    = hz * rep;
    }
    err = parlio_new_tx_unit(&cfg, &s_emu_tx);
    if ( ESP_OK != err )
    {
        s_emu_tx = NULL;
        KLOG_E(TAG, "parlio_new_tx_unit: %s", esp_err_to_name(err));
        emu_release();
        return KODE_ERROR;
    }
    err = parlio_tx_unit_enable(s_emu_tx);
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "parlio_tx_unit_enable: %s", esp_err_to_name(err));
        emu_release();
        return KODE_ERROR;
    }
    err = parlio_tx_unit_transmit(s_emu_tx, s_emu_buf, nbits, &tc);
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "parlio_tx_unit_transmit: %s", esp_err_to_name(err));
        (void)parlio_tx_unit_disable(s_emu_tx);
        emu_release();
        return KODE_ERROR;
    }
    s_emu_on = true;
    return KODE_OK;
}

/**
 * @brief Halve a loop of even runs in place, for the 62.5 kHz class.
 *
 * @param lv   The levels.
 * @param loop Their count.
 * @return The new count; 0 when a run is odd.
 */
static uint32_t emu_halve(uint8_t *lv, uint32_t loop)
{
    uint32_t i;

    if ( (loop % RFID_EMU_HALF_DIV) != 0U )
    {
        return 0U;
    }
    for ( i = 0U; (i + 1U) < loop; i += RFID_EMU_HALF_DIV )
    {
        if ( lv[i] != lv[i + 1U] )
        {
            return 0U;
        }
    }
    for ( i = 0U; (loop / RFID_EMU_HALF_DIV) > i; ++i )
    {
        lv[i] = lv[RFID_EMU_HALF_DIV * i];
    }
    return loop / RFID_EMU_HALF_DIV;
}

/**
 * @brief The clock's name for the log.
 *
 * @param ext true for the external clock.
 * @return "external" or "internal".
 */
static const char *emu_clk_name(bool ext)
{
    if ( ext )
    {
        return "external";
    }
    return "internal";
}

/**
 * @brief Start the PARLIO loop of an EM4100 tag.
 *
 * @param id The 5-byte ID.
 * @return KODE_OK with the loop running; KODE_DENIED while one runs or a
 *         certification carrier is up; KODE_NO_RESOURCES without PSRAM or DMA
 *         memory; KODE_ERROR on a PARLIO failure, all released.
 */
static kode_status_t emu_start(const uint8_t id[5])
{
    uint32_t hz    = RFID_EMU_CLASS_125K_HZ;
    uint32_t rep   = 1U;
    uint32_t copy  = 1U;
    uint32_t loop;
    uint32_t nbits;
    uint32_t klass;
    uint32_t i;
    bool     ext   = true;
    uint8_t *lv;
    kode_status_t rc;

    if ( s_emu_on || s_field )
    {
        return KODE_DENIED;
    }
    /* cppcheck-suppress misra-c2012-11.5 ; khal_alloc_psram() hands back untyped memory */
    lv = (uint8_t *)khal_alloc_psram(sizeof(uint32_t), KRFID_MOD_EM4100_LEVELS);
    if ( NULL == lv )
    {
        KLOG_E(TAG, "no PSRAM for the emulation levels (%" PRIu32 " bytes)",
               (uint32_t)KRFID_MOD_EM4100_LEVELS);
        return KODE_NO_RESOURCES;
    }
    loop = krfid_mod_em4100(id, lv, KRFID_MOD_EM4100_LEVELS);
    if ( 0U == loop )
    {
        khal_free(lv);
        return KODE_ERROR;
    }

    klass = emu_class_hz(emu_field_hz());
    if ( 0U == klass )
    {
        ext = false;
        rep = RFID_EMU_INT_REP;   /* XTAL/160 = 250 kHz: two bits per period */
    }
    else
    {
        hz = klass;
        if ( RFID_EMU_CLASS_62K_HZ == klass )
        {
            uint32_t const half = emu_halve(lv, loop);

            if ( 0U != half )
            {
                loop = half;
            }
            else
            {
                KLOG_W(TAG, "runs not all even; internal clock");
                ext = false;
                hz  = RFID_EMU_CLASS_125K_HZ;
                rep = RFID_EMU_INT_REP;
            }
        }
    }
    while ( (((loop * rep * copy) % RFID_EMU_BYTE_BITS) != 0U)
           && (RFID_EMU_COPIES_MAX > copy) )
    {
        ++copy;                   /* whole bytes for the DMA buffer */
    }
    nbits = loop * rep * copy;
    if ( (RFID_EMU_MAX_BITS < nbits) || ((nbits % RFID_EMU_BYTE_BITS) != 0U) )
    {
        khal_free(lv);
        KLOG_E(TAG, "emulation loop does not fit the DMA buffer");
        return KODE_ERROR;
    }

    /* The bit buffer exists for this play alone: emu_release() gives it back
     * on every path out, a stop included. */
    /* cppcheck-suppress misra-c2012-11.5 ; heap_caps_calloc() hands back untyped memory */
    s_emu_buf = (uint8_t *)heap_caps_calloc(1U, nbits / RFID_EMU_BYTE_BITS,
                                            /* cppcheck-suppress misra-c2012-10.1 ; IDF's MALLOC_CAP_* are shifts of a signed constant */
                                            /* cppcheck-suppress misra-c2012-12.2 ; the same shifts */
                                            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if ( NULL == s_emu_buf )
    {
        khal_free(lv);
        KLOG_E(TAG, "no DMA memory for the emulation buffer");
        return KODE_NO_RESOURCES;
    }
    for ( i = 0U; i < nbits; ++i )
    {
        if ( RFID_EMU_BIT_LEVEL == lv[(i / rep) % loop] )
        {
            s_emu_buf[i / RFID_EMU_BYTE_BITS] |=
                (uint8_t)(1U << (i % RFID_EMU_BYTE_BITS));   /* LSB-first */
        }
    }
    khal_free(lv);

    rc = emu_play(ext, hz, rep, nbits);
    if ( KODE_OK == rc )
    {
        KLOG_N(TAG, "emulating EM4100 %02X %02X %02X %02X %02X: %" PRIu32
                    " periods/loop, %s clock %" PRIu32 " Hz",
               id[0], id[1], id[2], id[3], id[4], loop,
               emu_clk_name(ext), hz);
    }
    return rc;
}

/**
 * @brief Stop the loop and release it; harmless with none.
 * @return KODE_OK.
 */
static kode_status_t emu_stop(void)
{
    if ( s_emu_on )
    {
        (void)parlio_tx_unit_disable(s_emu_tx);
        s_emu_on = false;
        emu_release();
        KLOG_N(TAG, "emulation stopped");
    }
    return KODE_OK;
}

kode_status_t kdrv_rfid_lf_run(kdrv_rfid_lf_job_t const *job, uint8_t id[5],
                               uint8_t *rf_div)
{
    if ( (NULL == job) || (NULL == id) || (NULL == rf_div) )
    {
        return KODE_DENIED;
    }
    (void)memset(id, 0, KDRV_RFID_LF_ID_BYTES);
    *rf_div = 0U;
    /* The emulation drives RFID_PULL through the PARLIO and a stop gives the
     * pad back itself: neither lets the receiver listen first. */
    if ( KDRV_RFID_JOB_EMULATE == job->type )
    {
        return emu_start(job->id);
    }
    if ( KDRV_RFID_JOB_EMU_STOP == job->type )
    {
        return emu_stop();
    }
    /* The receiver listens from here: the job's window, or its field
     * capture, which measures with the carrier down. */
    pull_set(PULL_LISTEN);

    if ( KDRV_RFID_JOB_CLONE == job->type )
    {
        return job_clone(job->id, id, rf_div);
    }
    if ( KDRV_RFID_JOB_CAPTURE == job->type )
    {
        return kdrv_rfid_lf_field_capture(job->ms, job->field, job->wave,
                                          job->cap, job->n);
    }
    return job_read(job->ms, id, rf_div);
}

kode_status_t kdrv_rfid_lf_rest(void)
{
    /* No job is checked for here: AO_Rfid asks only with none in flight, and
     * the job is its to know (rule 9) - the driver keeps no running flag. */
    if ( s_field )
    {
        return KODE_DENIED;
    }
    carrier_release();   /* the timer goes back to the header while nothing reads */
    pull_set(PULL_REST);
    return KODE_OK;
}

kode_status_t kdrv_rfid_lf_field_capture(uint32_t ms, krfid_field_t *out,
                                         uint16_t *wave, size_t cap, size_t *n)
{
    uint32_t      span = ms;
    kode_status_t rc;

    /* The worker is the one task a capture runs on. Its jobs are serialised,
     * so the window buffers and the ADC handle are never shared, and it is not
     * app_main, where the DMA returns zeros. Until the AO has named its worker
     * there is none, and no task qualifies. */
    if ( (NULL == s_is_worker) || !s_is_worker() )
    {
        return KODE_DENIED;
    }
    /* Only a held certification carrier can be up here, since a read or a
     * clone drops its field before its job ends; a capture under it would
     * measure the Dot's own field. */
    if ( s_field )
    {
        return KODE_DENIED;
    }
    if ( 0U == span )
    {
        return KODE_ERROR;
    }
    if ( KDRV_RFID_LF_CAPTURE_MS_MAX < span )
    {
        span = KDRV_RFID_LF_CAPTURE_MS_MAX;
    }

    /* The read's window without its field: the buffers and the ADC handle are
     * taken, the window is filled and never energised, and both are handed
     * back when it ends - in the order kdrv_rfid_lf_field_off() releases them
     * after a read, so between requests the driver still holds nothing. */
    rc = bufs_ensure();
    if ( KODE_OK == rc )
    {
        rc = adc_ensure();
    }
    if ( KODE_OK == rc )
    {
        size_t const got =
            adc_fill((size_t)span * (size_t)RFID_SAMPLES_PER_MS);

        if ( 0U == got )
        {
            /* An ADC that delivered nothing answers an error and writes
             * nothing: the lent pointers carry a window or stay as they were. */
            rc = KODE_ERROR;
        }
        else
        {
            size_t points = 0U;

            krfid_wave_stats(s_bufs->win, got, out);
            if ( NULL != wave )
            {
                points = krfid_wave_decimate(s_bufs->win, got, wave, cap);
            }
            if ( NULL != n )
            {
                *n = points;
            }
        }
    }
    adc_release();
    bufs_release();
    return rc;
}
