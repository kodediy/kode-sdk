/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_es8311.c
 * @brief The ES8311 codec and the I2S controller under it.
 */
#include "kdrv_es8311.h"

#include <stdio.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "kbsp.h"
#include "khal_i2c.h"
#include "khal_sync.h"
#include "khal_system.h"
#include "khal_time.h"
#include "kode_log.h"
#include "kes_fields.h"
#include "kring.h"
#include "kwav.h"

#include "es8311_codec.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

/* IDF 6.1 numbers the I2S controllers as plain integers, I2S_NUM_0 to the
 * chip's last; the codec's must be one of them. */
_Static_assert((int)KBSP_I2S_CODEC <= I2S_NUM_2,
               "KBSP_I2S_CODEC must name one of the chip's I2S controllers");

#define TAG "kdrv.audio"

_Static_assert(KDRV_ES8311_RATE_HZ == KTONE_RATE_HZ,
               "the detector and the codec must agree on the sample rate");
_Static_assert(KDRV_ES8311_RATE_HZ == KWAV_RATE_HZ,
               "the file format and the codec must agree on the sample rate");
_Static_assert((KTONE_WIN_N % KTONE_BLOCK_FRAMES) == 0U,
               "a window must be a whole number of I2S blocks");

/* ---- The three registers this file reads itself --------------------------- */
/* The rest of the map belongs to the field table, in kes_fields.c: these
 * three are the ones the bring-up and the open check for themselves. */

#define REG_DAC2ADC     0x44U  /* the DAC-to-ADC loopback, kept off          */
#define REG_CHD1        0xFDU  /* chip identity 1                            */
#define REG_CHD2        0xFEU  /* chip identity 2                            */

/* ---- The park: the codec layer's suspend, written straight to the bus ------
 * kdrv_es8311_park() is for a board with no audio owner, and it must not link
 * the codec component or the I2S set-up: so it writes, over the shared bus,
 * the same steps esp_codec_dev's es8311_suspend() writes on a close. */

#define PARK_PROBE_MS       20U    /* one probe of 0x18                       */
#define PARK_REG_RESET      0x00U  /* reset and CSM                           */
#define PARK_REG_CLK1       0x01U  /* clock enables                           */
#define PARK_REG_CLK2       0x02U  /* clock dividers                          */
#define PARK_REG_SYS0D      0x0DU  /* analog power                            */
#define PARK_REG_SYS0E      0x0EU  /* ADC analog power                        */
#define PARK_REG_SYS12      0x12U  /* DAC power                               */
#define PARK_REG_SYS14      0x14U  /* microphone input and PGA                */
#define PARK_REG_ADC15      0x15U  /* ADC ramp                                */
#define PARK_REG_ADC17      0x17U  /* ADC volume                              */
#define PARK_REG_DAC32      0x32U  /* DAC volume                              */
#define PARK_REG_GP45       0x45U  /* general purpose                         */
#define PARK_VOL_OFF        0x00U  /* DAC and ADC volume at their floor       */
#define PARK_ADC_DOWN       0xFFU  /* every ADC analog block off              */
#define PARK_DAC_DOWN       0x02U  /* the DAC powered down                    */
#define PARK_MIC_OFF        0x00U  /* no input selected, the PGA off          */
#define PARK_ANALOG_HALF    0xFAU  /* analog off but the reference            */
#define PARK_RAMP_OFF       0x00U  /* no ADC ramp                             */
#define PARK_CLK_DIV        0x10U  /* the divider the suspend leaves          */
#define PARK_RESET_HOLD     0x00U  /* out of reset, CSM off                   */
#define PARK_RESET_ALL      0x1FU  /* every block held in reset               */
#define PARK_CLK_GATED      0x30U  /* the clocks gated                        */
#define PARK_CLK_OFF        0x00U  /* every clock off                         */
#define PARK_GP_OFF         0x00U  /* the general purpose register cleared    */
#define PARK_ANALOG_OFF     0xFCU  /* every analog block off                  */
#define PARK_CLK_DIV_OFF    0x00U  /* the dividers cleared                    */

/* ---- Sizes and timings --------------------------------------------------- */

/** @brief Two slots on the wire: the codec component refuses an odd channel count. */
#define ES_SLOTS            2U
/** @brief One I2S transfer, in bytes. */
#define ES_BLOCK_BYTES  (KTONE_BLOCK_FRAMES * ES_SLOTS * 2U)

/* ---- The transmit ring, spelled out rather than inherited -----------------
 * These two are ESP-IDF's own defaults for an I2S channel, and they are set
 * explicitly below because the drain constant under them has to be right: a
 * ring that grew in a later IDF would leave the tail of every sound un-played
 * and nothing would say so. One place, three uses.
 */
/** @brief DMA descriptors in the transmit ring. */
#define ES_DMA_DESCS         6U
/** @brief Frames in one DMA descriptor. */
#define ES_DMA_FRAMES      240U
/** @brief What the ring holds when it is full, in bytes. */
#define ES_DMA_RING_BYTES  (ES_DMA_DESCS * ES_DMA_FRAMES * ES_SLOTS * 2U)

/**
 * @brief Blocks of silence written after a sound, to push it out of the ring.
 *
 * `esp_codec_dev_write()` returns when the bytes are IN the ring, not when
 * they have left it - so a play that stops writing at its last sample has
 * about thirty milliseconds of sound still queued and hands the DAC whatever
 * the ring holds behind it. Two things follow, and this constant answers both.
 *
 * The samples after a sound must be zeros the driver PUT there. The ring
 * clears itself behind the DMA (auto_clear, see i2s_up()), but that is a
 * property of a channel configuration this file does not want to be
 * load-bearing: an explicit tail of silence is, and it is also what a partial
 * last DMA buffer needs to be padded with.
 *
 * And the end of a play has to mean the speaker has heard it. That is a
 * promise - AUDIO_DONE_SIG carries it, and an Active Object that played a
 * sound can close on it before asking for the write that opens the BATFET -
 * so a job that answered with its last thirty milliseconds still in a ring
 * would have the board switch off mid-note.
 *
 * A whole ring, rounded up, plus one block for the buffer a write was in the
 * middle of filling. About 37 ms per sound, paid on the worker.
 */
#define ES_DRAIN_BLOCKS  (((ES_DMA_RING_BYTES + ES_BLOCK_BYTES - 1U) \
                           / ES_BLOCK_BYTES) + 1U)

/* Both of the drain's promises stop being true the moment it no longer
 * covers the whole ring, and neither would fail loudly on a board - every
 * sound would simply lose its tail. Checked here instead. */
_Static_assert((ES_DRAIN_BLOCKS * ES_BLOCK_BYTES) > ES_DMA_RING_BYTES,
               "the drain must push a whole transmit ring out, and then some");
/** @brief A whole window, in blocks. */
#define ES_WINDOW_BLOCKS (KTONE_WIN_N / KTONE_BLOCK_FRAMES)
/**
 * @brief Blocks emitted and discarded before a window is captured.
 *
 * The transmit and receive DMA rings have to fill, the amplifier has to start,
 * and the DAC's own volume ramp has to finish. Six blocks is 32 ms. A first
 * attempt that fails where a second passes is this being too short, and the
 * log says which attempt it was so that is visible rather than mysterious.
 */
#define ES_SETTLE_BLOCKS    6U
/**
 * @brief Blocks more before the first window after an open.
 *
 * The microphone input starts with a step: the window from 32 to 118 ms
 * after an open reads -12 dBFS RMS and full-scale peaks in a quiet room, the
 * next one -56 dBFS. A floor measured over that step refuses to judge ("too
 * loud") a board that is fine. 36 blocks is 192 ms, so the first window
 * starts 256 ms after the open, twice the time the step takes.
 * KODE_AUDIO_TEST_FRESH_MS is this in milliseconds.
 */
#define ES_FRESH_RX_BLOCKS 36U
/** @brief Blocks emitted after the window, so a block of slip cannot truncate
 *         the tone the detector was measuring. */
#define ES_TAIL_BLOCKS      4U

/**
 * @brief How long the converter's mute is given before the analog goes down.
 *
 * The DAC does not jump to silence: register 0x37 sets a volume ramp, and the
 * mute follows it. Twelve milliseconds is the longest that ramp takes at this
 * board's rate with the rate the codec layer leaves in 0x37 - long enough for
 * the output to have reached zero, short enough that an idle close is not a
 * pause anybody notices. Paid once per close, on the worker's thread.
 */
#define ES_MUTE_SETTLE_MS  12U

/* ---- State --------------------------------------------------------------- */

static const audio_codec_ctrl_if_t *s_ctrl;
static const audio_codec_data_if_t *s_data;
static const audio_codec_gpio_if_t *s_gpio;
static const audio_codec_if_t      *s_codec;
static esp_codec_dev_handle_t       s_dev;
static i2s_chan_handle_t            s_tx;
static i2s_chan_handle_t            s_rx;

static kdrv_es8311_result_t s_result;
static ktone_det_t          s_det;

static bool s_present;            /**< The chip answered and identified.    */
static bool s_open;               /**< The device is open.                  */
static bool s_fresh;              /**< Opened and nothing emitted since: the
                                   *   amplifier is still starting.          */
static bool s_hold_mute;          /**< The driver is holding the DAC muted
                                   *   because the analog is not up. Its own,
                                   *   never the caller's out_mute.          */
static bool s_started;            /**< kdrv_es8311_init() has run.          */
static uint8_t s_id1;
static uint8_t s_id2;

/* The buffers. Static, never on the worker's stack: two kilobytes of stereo
 * blocks and half of one for the de-interleaved left slot. */
static int16_t s_txb[KTONE_BLOCK_FRAMES * ES_SLOTS];
static int16_t s_rxb[KTONE_BLOCK_FRAMES * ES_SLOTS];
static int16_t s_mono[KTONE_BLOCK_FRAMES];

/* ---- Registers, through the control interface ---------------------------- */

kode_status_t kdrv_es8311_reg_read(uint8_t reg, uint8_t *val)
{
    uint8_t v = 0U;

    if ( (NULL == val) || (NULL == s_ctrl) )
    {
        return KODE_DENIED;
    }
    if ( s_ctrl->read_reg(s_ctrl, (int)reg, 1, &v, 1) != 0 )
    {
        return KODE_IO_ERROR;
    }
    /* Written only on success: a failed bus read must never become a
     * plausible reading of the chip. */
    *val = v;
    return KODE_OK;
}

kode_status_t kdrv_es8311_reg_write(uint8_t reg, uint8_t val)
{
    uint8_t v = val;

    if ( NULL == s_ctrl )
    {
        return KODE_DENIED;
    }
    if ( s_ctrl->write_reg(s_ctrl, (int)reg, 1, &v, 1) != 0 )
    {
        return KODE_IO_ERROR;
    }
    return KODE_OK;
}

/* ---- Bring-up ------------------------------------------------------------ */

/**
 * @brief Build the I2S channel pair: one controller, both directions.
 *
 * Initialised and deliberately NOT enabled. The codec layer disables,
 * reconfigures and enables both channels itself inside its open(), so
 * enabling here would only be undone a moment later.
 *
 * @return KODE_OK, or KODE_ERROR when the controller would not be made.
 */
static kode_status_t i2s_up(void)
{
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(
        (int)KBSP_I2S_CODEC, I2S_ROLE_MASTER);
    /* Silence rather than the last block again when the writer falls behind:
     * an underrun should be a gap, not a repeated fragment that a detector
     * would happily measure. Not what the tail of a sound rests on, though -
     * see ES_DRAIN_BLOCKS. */
    chan.auto_clear = true;
    /* IDF's own defaults, spelled out because ES_DRAIN_BLOCKS is computed
     * from them and a ring that changed size underneath it would truncate
     * every sound by however much it grew. */
    chan.dma_desc_num  = ES_DMA_DESCS;
    chan.dma_frame_num = ES_DMA_FRAMES;

    /* One controller for the speaker and the microphone: ESP-IDF warns that
     * the RX half turns slave for full duplex, which is the design. */
    kode_log_set_level("i2s_common", KODE_LOG_ERROR);
    if ( i2s_new_channel(&chan, &s_tx, &s_rx) != ESP_OK )
    {
        KLOG_E(TAG, "no I2S controller");
        return KODE_ERROR;
    }

    i2s_std_config_t std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(KDRV_ES8311_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)KBSP_PIN_I2S_MCLK,
            .bclk = (gpio_num_t)KBSP_PIN_I2S_SCLK,
            .ws   = (gpio_num_t)KBSP_PIN_I2S_LRCK,
            .dout = (gpio_num_t)KBSP_PIN_I2S_DSDIN,
            .din  = (gpio_num_t)KBSP_PIN_I2S_ASDOUT,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    std.clk_cfg.mclk_multiple = (i2s_mclk_multiple_t)KDRV_ES8311_MCLK_MULT;

    if ( (i2s_channel_init_std_mode(s_tx, &std) != ESP_OK)
        || (i2s_channel_init_std_mode(s_rx, &std) != ESP_OK) )
    {
        KLOG_E(TAG, "the I2S channels would not take the board's format");
        return KODE_ERROR;
    }
    return KODE_OK;
}

static kode_status_t do_open(void);
static kode_status_t do_close(void);

/** @brief The codec layer's own span from volume 1 to volume 100, kept. */
#define ES_VOL_SPAN_DB  50.0f

/**
 * @brief Give the codec layer the volume curve that caps the speaker.
 *
 * Every volume anybody sets passes through this curve - the field table, a
 * tone, an app - so it is the one place the cap can live and be obeyed by
 * all of them. Two points: the layer's own fifty-decibel span, moved down so
 * that its top is KDRV_ES8311_OUT_DB_X10_MAX. Volume 100 is the cap, and
 * every volume under it sits as far under the cap as the same volume sits
 * under full scale on the layer's own curve. The layer interpolates in floats
 * and truncates to a register step, so nothing in between rounds up past the
 * top.
 *
 * @return KODE_OK, or KODE_ERROR when the layer refused it.
 */
static kode_status_t install_cap(void)
{
    float top = (float)KDRV_ES8311_OUT_DB_X10_MAX / 10.0f;
    esp_codec_dev_vol_map_t points[2] = {
        { .vol = 0,                        .db_value = top - ES_VOL_SPAN_DB },
        { .vol = (int)KDRV_ES8311_VOL_MAX, .db_value = top },
    };
    esp_codec_dev_vol_curve_t curve = {
        .vol_map = points,
        .count   = 2,
    };

    if ( esp_codec_dev_set_vol_curve(s_dev, &curve) != ESP_CODEC_DEV_OK )
    {
        return KODE_ERROR;
    }
    return KODE_OK;
}

/**
 * @brief Walk the codec straight to its resting registers, at bring-up only.
 *
 * Building the codec object (es8311_codec_new(), inside es8311_open()) left
 * the chip configured but not suspended: references powered, internal clocks
 * enabled. The codec layer only suspends a chip it has ENABLED, so reaching
 * rest needs one enable(true)/enable(false) cycle - and this calls it on
 * s_codec directly, never through esp_codec_dev_open()/close(). That path
 * also walks the I2S data interface, whose own set_fmt() disables both
 * channels unconditionally before it enables them, to force a known state.
 * At bring-up the channels are fresh from i2s_new_channel() and have never
 * been enabled once, so that unconditional disable logs "the channel has not
 * been enabled yet" on both, at IDF's own error level - true, and read as a
 * boot failure it is not (design rule 6). This never reaches I2S at all, so
 * it cannot happen. enable() is the interface's own low-power entry point
 * (audio_codec_if.h: "when codec disabled it can use less power"), not a
 * layer being gone around.
 *
 * @return KODE_OK, or KODE_ERROR when the codec would not walk the cycle.
 */
static kode_status_t rest_at_bringup(void)
{
    if ( (NULL == s_codec->enable)
        || (s_codec->enable(s_codec, true) != ESP_CODEC_DEV_OK)
        || (s_codec->enable(s_codec, false) != ESP_CODEC_DEV_OK) )
    {
        return KODE_ERROR;
    }
    return KODE_OK;
}

/** @brief One write of the park: a register and what goes into it. */
typedef struct {
    uint8_t reg;  /**< The register. */
    uint8_t val;  /**< Its value.    */
} park_step_t;

kode_status_t kdrv_es8311_park(void)
{
    static const park_step_t STEPS[] = {
        { PARK_REG_DAC32, PARK_VOL_OFF      }, { PARK_REG_ADC17, PARK_VOL_OFF     },
        { PARK_REG_SYS0E, PARK_ADC_DOWN     }, { PARK_REG_SYS12, PARK_DAC_DOWN    },
        { PARK_REG_SYS14, PARK_MIC_OFF      }, { PARK_REG_SYS0D, PARK_ANALOG_HALF },
        { PARK_REG_ADC15, PARK_RAMP_OFF     }, { PARK_REG_CLK2,  PARK_CLK_DIV     },
        { PARK_REG_RESET, PARK_RESET_HOLD   }, { PARK_REG_RESET, PARK_RESET_ALL   },
        { PARK_REG_CLK1,  PARK_CLK_GATED    }, { PARK_REG_CLK1,  PARK_CLK_OFF     },
        { PARK_REG_GP45,  PARK_GP_OFF       }, { PARK_REG_SYS0D, PARK_ANALOG_OFF  },
        { PARK_REG_CLK2,  PARK_CLK_DIV_OFF  },
    };
    khal_i2c_dev_t dev = NULL;
    uint8_t        id1 = 0U;
    uint8_t        id2 = 0U;
    kode_status_t  rc  = KODE_ERROR;

    if ( (khal_i2c_init() != KODE_OK) || !khal_i2c_probe(KBSP_I2C_ADDR_CODEC, PARK_PROBE_MS)
        || (khal_i2c_add(KBSP_I2C_ADDR_CODEC, &dev) != KODE_OK) )
    {
        return KODE_ERROR;
    }
    if ( (khal_i2c_read_reg(dev, REG_CHD1, &id1, 1U) == KODE_OK)
        && (khal_i2c_read_reg(dev, REG_CHD2, &id2, 1U) == KODE_OK)
        && ((uint8_t)KDRV_ES8311_ID1 == id1) && ((uint8_t)KDRV_ES8311_ID2 == id2) )
    {
        rc = KODE_OK;
        for ( size_t i = 0U; (sizeof(STEPS) / sizeof(STEPS[0])) > i; ++i )
        {
            if ( khal_i2c_write_reg(dev, STEPS[i].reg, STEPS[i].val) != KODE_OK )
            {
                rc = KODE_IO_ERROR;
            }
        }
    }
    (void)khal_i2c_remove(dev);
    return rc;
}

kode_status_t kdrv_es8311_init(void)
{
    audio_codec_i2c_cfg_t i2c = {
        .port = 0U,
        /* The component's I2C layer shifts this right by one, so it wants the
         * eight-bit form. kbsp keeps the seven-bit one, like every other
         * address on the bus. Unshifted, it addresses 0x0C, where nothing answers. */
        .addr = (uint8_t)(KBSP_I2C_ADDR_CODEC << 1U),
        .bus_handle = NULL,
    };

    if ( s_started )
    {
        return KODE_DENIED;
    }
    s_started = true;

    if ( khal_i2c_init() != KODE_OK )
    {
        KLOG_E(TAG, "no I2C bus; the codec stays down");
        return KODE_ERROR;
    }
    i2c.bus_handle = (void *)khal_i2c_bus();

    s_ctrl = audio_codec_new_i2c_ctrl(&i2c);
    if ( NULL == s_ctrl )
    {
        KLOG_E(TAG, "no control interface for the codec");
        return KODE_ERROR;
    }

    /* The identity check is ours: the codec component never reads these, so
     * without it a board with no codec fitted would fail later and less
     * clearly. Absence is a state - said once here, and every call after this
     * refuses rather than retrying a chip that is not there. */
    if ( (kdrv_es8311_reg_read(REG_CHD1, &s_id1) != KODE_OK)
        || (kdrv_es8311_reg_read(REG_CHD2, &s_id2) != KODE_OK) )
    {
        KLOG_W(TAG, "nothing answers at 0x%02X; this board has no codec",
               (unsigned)KBSP_I2C_ADDR_CODEC);
        return KODE_ERROR;
    }
    if ( ((uint8_t)KDRV_ES8311_ID1 != s_id1) || ((uint8_t)KDRV_ES8311_ID2 != s_id2) )
    {
        KLOG_E(TAG, "0x%02X answers but reads 0x%02X%02X, not an ES8311",
               (unsigned)KBSP_I2C_ADDR_CODEC, (unsigned)s_id1, (unsigned)s_id2);
        return KODE_ERROR;
    }

    if ( i2s_up() != KODE_OK )
    {
        return KODE_ERROR;
    }

    audio_codec_i2s_cfg_t i2s = {
        .port      = 0U,
        .rx_handle = s_rx,
        .tx_handle = s_tx,
        .clk_src   = 0,
    };
    s_data = audio_codec_new_i2s_data(&i2s);
    s_gpio = audio_codec_new_gpio();
    if ( (NULL == s_data) || (NULL == s_gpio) )
    {
        KLOG_E(TAG, "no data interface for the codec");
        return KODE_ERROR;
    }

    es8311_codec_cfg_t cfg = {
        .ctrl_if     = s_ctrl,
        .gpio_if     = s_gpio,
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_BOTH,
        /* The amplifier's enable is the C5's IO4, driven by the C5's own
         * firmware. Nothing on this side owns it, so the component is told
         * there is no pin rather than given one it would drive wrongly. */
        .pa_pin      = -1,
        .pa_reverted = false,
        /* The P4 is the I2S master and the codec is the slave. */
        .master_mode = false,
        .use_mclk    = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = {
            /* Both equal, so the component computes no hardware gain and adds
             * no digital gain to compensate for one. The amplifier runs from
             * +VSYS: the stock five volts makes volume 100 write 0xC6 and clip. */
            .pa_voltage        = 3.3f,
            .codec_dac_voltage = 3.3f,
            .pa_gain           = 0.0f,
        },
        /* The right capture slot stays empty, never the DAC's own output:
         * with it, an acoustic test passes with a dead mic and speaker. */
        .no_dac_ref  = true,
        .mclk_div    = (uint16_t)KDRV_ES8311_MCLK_MULT,
    };
    s_codec = es8311_codec_new(&cfg);
    if ( NULL == s_codec )
    {
        KLOG_E(TAG, "the codec would not initialise");
        return KODE_ERROR;
    }

    esp_codec_dev_cfg_t dev = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = s_codec,
        .data_if  = s_data,
    };
    s_dev = esp_codec_dev_new(&dev);
    if ( NULL == s_dev )
    {
        KLOG_E(TAG, "no codec device");
        return KODE_ERROR;
    }
    /* The analog blocks go down on close. Without this the amplifier - which
     * the C5 has already enabled - would be fed a live DAC for as long as the
     * board is up, which is not the resting state of a battery product. */
    (void)esp_codec_set_disable_when_closed(s_dev, true);

    /* The speaker's cap, before anything can open the device. A codec that
     * cannot be capped is not presented, so it never makes a sound. */
    if ( install_cap() != KODE_OK )
    {
        KLOG_E(TAG, "the speaker's cap would not install; the codec stays down");
        return KODE_ERROR;
    }
    s_present = true;

    /* Rest from here, not from the first idle close. A board where this
     * fails still presents the chip: the first sound's own open meets the
     * same failure and says so, and the production row judges it. */
    if ( rest_at_bringup() != KODE_OK )
    {
        KLOG_W(TAG, "the codec would not rest at bring-up; it idles configured "
                    "until its first sound");
    }
    KLOG_D(TAG, "ES8311 up, identity 0x%02X%02X, resting until somebody asks",
           (unsigned)s_id1, (unsigned)s_id2);
    return KODE_OK;
}

void kdrv_es8311_info(kdrv_es8311_info_t *out)
{
    if ( NULL == out )
    {
        return;
    }
    out->rate    = KDRV_ES8311_RATE_HZ;
    out->present = s_present;
    out->open    = s_open;
    out->busy    = false;
    out->bits    = (uint8_t)KDRV_ES8311_BITS;
    {
        kes_settings_t s;
        kes_settings(&s);
        out->volume = s.vol;
        out->mic_db = s.mic_db;
        out->mute   = s.out_mute;
    }
}

bool kdrv_es8311_present(void)
{
    return s_present;
}

bool kdrv_es8311_is_open(void)
{
    return s_open;
}

/* ---- The owner's buffer, for a recording on its way out ------------------ */

/**
 * @brief Where the samples begin inside the allocation.
 *
 * A whole 64-byte block rather than the header's 44, so the samples stay
 * aligned and the header still fits in front of them: the file image starts
 * at this offset minus KWAV_HEADER_BYTES.
 */
#define ES_BUF_HEAD    64U

static uint8_t *s_buf;   /**< The allocation. NULL until somebody asks.     */

int16_t *kdrv_es8311_buf(uint32_t *frames)
{
    if ( NULL != frames )
    {
        *frames = (uint32_t)KDRV_ES8311_BUF_FRAMES;
    }
    if ( NULL == s_buf )
    {
        s_buf = (uint8_t *)khal_alloc_psram(
            64U, (size_t)ES_BUF_HEAD
                 + ((size_t)KDRV_ES8311_BUF_FRAMES * sizeof(int16_t)));
        if ( NULL == s_buf )
        {
            KLOG_E(TAG, "no PSRAM for a sound of %u seconds",
                   (unsigned)KDRV_ES8311_BUF_SECONDS);
            return NULL;
        }
    }
    return (int16_t *)(void *)&s_buf[ES_BUF_HEAD];
}

uint8_t *kdrv_es8311_buf_file(uint32_t frames, uint32_t *bytes)
{
    uint8_t *image;

    if ( kdrv_es8311_buf(NULL) == NULL )
    {
        return NULL;
    }
    if ( (uint32_t)KDRV_ES8311_BUF_FRAMES < frames )
    {
        return NULL;
    }
    image = &s_buf[ES_BUF_HEAD - KWAV_HEADER_BYTES];
    if ( kwav_build(frames, image, (size_t)KWAV_HEADER_BYTES) != KODE_OK )
    {
        return NULL;
    }
    if ( NULL != bytes )
    {
        *bytes = (uint32_t)KWAV_HEADER_BYTES
                 + (frames * (uint32_t)sizeof(int16_t));
    }
    return image;
}

void kdrv_es8311_result(kdrv_es8311_result_t *out)
{
    if ( NULL != out )
    {
        *out = s_result;
    }
}

/* ---- Ending a recording early ---------------------------------------------- */

static volatile bool s_record_end;   /**< The recording is to stop at its next block. */

void kdrv_es8311_record_reset(void)
{
    s_record_end = false;
}

void kdrv_es8311_record_end(void)
{
    s_record_end = true;
}

/* ---- The stream ------------------------------------------------------------ */

static kring_t       s_stream;        /**< The writer's frames on their way.     */
static int16_t      *s_stream_buf;    /**< Its storage. NULL until first opened. */
static khal_sem_t    s_stream_room;   /**< Given each time the job takes frames. */
static volatile bool s_stream_open;   /**< Writes are taken: reset to end.       */
static volatile bool s_stream_end;    /**< No more frames are coming.            */

kode_status_t kdrv_es8311_stream_reset(void)
{
    if ( NULL == s_stream_buf )
    {
        s_stream_buf = (int16_t *)khal_alloc_psram(
            64U, (size_t)KDRV_ES8311_STREAM_FRAMES * sizeof(int16_t));
        if ( NULL == s_stream_buf )
        {
            KLOG_E(TAG, "no PSRAM for the stream's ring");
            return KODE_NO_RESOURCES;
        }
    }
    if ( NULL == s_stream_room )
    {
        s_stream_room = khal_sem_create();
        if ( NULL == s_stream_room )
        {
            return KODE_NO_RESOURCES;
        }
    }
    kring_init(&s_stream, s_stream_buf, (uint32_t)KDRV_ES8311_STREAM_FRAMES);
    s_stream_end  = false;
    s_stream_open = true;
    return KODE_OK;
}

kode_status_t kdrv_es8311_stream_write(const int16_t *frames, size_t n,
                                       uint8_t channels, uint32_t ms,
                                       size_t *taken)
{
    size_t           done  = 0U;
    kode_status_t    rc    = KODE_OK;
    TickType_t const start = xTaskGetTickCount();
    TickType_t const limit = pdMS_TO_TICKS(ms);

    if ( NULL != taken )
    {
        *taken = 0U;
    }
    if ( (NULL == frames) || ((1U != channels) && (2U != channels)) )
    {
        return KODE_DENIED;
    }
    if ( !s_stream_open )
    {
        return KODE_DENIED;
    }
    for ( ;; )
    {
        TickType_t spent;

        done += kring_put(&s_stream, &frames[done * (size_t)channels],
                          n - done, channels);
        if ( done >= n )
        {
            break;
        }
        if ( !s_stream_open )
        {
            rc = KODE_DENIED;
            break;
        }
        spent = xTaskGetTickCount() - start;
        if ( spent >= limit )
        {
            rc = KODE_TIMEOUT;
            break;
        }
        /* The ring is full. The job gives this semaphore every time it takes
         * a block, so the wait ends as soon as there is room for one. */
        (void)khal_sem_wait_timeout(s_stream_room,
                                    (uint32_t)pdTICKS_TO_MS(limit - spent));
    }
    if ( NULL != taken )
    {
        *taken = done;
    }
    return rc;
}

void kdrv_es8311_stream_end(void)
{
    s_stream_end  = true;
    s_stream_open = false;
    /* A writer asleep on a full ring is let go, and finds the stream shut. */
    if ( NULL != s_stream_room )
    {
        khal_sem_signal(s_stream_room);
    }
}

/* ---- The jobs, all of which run on the owner's worker --------------------- */

void kdrv_es8311_apply_settings(void)
{
    kes_settings_t s;

    if ( !s_open )
    {
        return;
    }
    kes_settings(&s);
    (void)esp_codec_dev_set_out_vol(s_dev, (int)s.vol);
    (void)esp_codec_dev_set_in_gain(s_dev, (float)s.mic_db);
    /* The driver's hold wins over the caller's setting while it is on, and it
     * is never stored: un-muting is the hold going away, not somebody's
     * out_mute changing. The amplifier is always live, so the DAC stays
     * muted whenever nothing is being written to it. */
    (void)esp_codec_dev_set_out_mute(s_dev, s.out_mute || s_hold_mute);
    (void)esp_codec_dev_set_in_mute(s_dev, s.in_mute);
}

/**
 * @brief Hold the DAC muted, or hand the decision back to the stored setting.
 *
 * One register write rather than the whole settings push: this is on the path
 * of every sound now, twice, and the volume and the microphone gain have not
 * moved. The caller's own @c out_mute is still obeyed - the hold only ever
 * adds silence, never takes it away.
 *
 * @param on Whether the driver is holding it muted.
 */
static void hold_mute(bool on)
{
    kes_settings_t s;

    s_hold_mute = on;
    if ( !s_open )
    {
        return;
    }
    kes_settings(&s);
    (void)esp_codec_dev_set_out_mute(s_dev, s.out_mute || s_hold_mute);
}

/**
 * @brief Mute the converter and let its ramp reach silence. Idempotent.
 *
 * The ES8311's own soft mute - register 0x31 through the codec layer's
 * `set_mute`, which is a masked write of bits 5 and 6, NOT the volume
 * register and not the volume curve - followed by the ramp at 0x37. A mute
 * that is already held costs nothing and waits for nothing: the ramp was paid
 * when it was taken.
 */
static void mute_and_settle(void)
{
    if ( s_hold_mute )
    {
        return;
    }
    hold_mute(true);
    khal_delay_ms(ES_MUTE_SETTLE_MS);
    KLOG_D(TAG, "the DAC is muted; %u ms of ramp paid",
           (unsigned)ES_MUTE_SETTLE_MS);
}

void kdrv_es8311_hush(void)
{
    hold_mute(true);
}

/**
 * @brief Let the converter speak again. Idempotent.
 *
 * Called with the first block of real samples already IN the ring, so the
 * ramp back up rides on signal rather than on the idle level - which is the
 * whole reason it is here and not at the top of the job.
 */
static void speak_up(void)
{
    if ( !s_hold_mute )
    {
        return;
    }
    hold_mute(false);
}

/** @brief ESP-IDF's own tag for the I2S channel driver. */
#define ES_I2S_TAG  "i2s_common"

/**
 * @brief Open the device at the board's one format.
 *
 * @return KODE_OK, KODE_DENIED when there is no chip, KODE_ERROR otherwise.
 */
static kode_status_t do_open(void)
{
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = (uint8_t)KDRV_ES8311_BITS,
        .channel         = (uint8_t)ES_SLOTS,
        .channel_mask    = 0x03U,
        .sample_rate     = KDRV_ES8311_RATE_HZ,
        .mclk_multiple   = (int)KDRV_ES8311_MCLK_MULT,
    };
    uint8_t loop = 0U;

    if ( !s_present )
    {
        return KODE_DENIED;
    }
    if ( s_open )
    {
        return KODE_OK;
    }
    /* TWO FALSE ERRORS PER OPEN, and they are ESP-IDF's, not this driver's.
     * The codec component's I2S data interface disables both channels before
     * it configures them, and i2s_channel_disable() logs "the channel has
     * not been enabled yet" at ERROR when they were not - which is always
     * true here, because i2s_up() leaves them initialised and idle on
     * purpose. The open then succeeds. The two lines appear on every open,
     * which would make them the most common ERROR line in the log, and an
     * operator cannot tell them from a fault.
     *
     * Silenced for the length of the call and no longer: the two lines are
     * about a state this file chose, and an I2S error raised anywhere else -
     * including by the next open - still reaches the log at its own level.
     * Restored to the build's own default rather than to a number of ours,
     * so `log_level` and the Kconfig still decide what i2s_common may say. */
    esp_log_level_set(ES_I2S_TAG, ESP_LOG_NONE);
    int const rc = esp_codec_dev_open(s_dev, &fs);
    esp_log_level_set(ES_I2S_TAG, (esp_log_level_t)CONFIG_LOG_DEFAULT_LEVEL);
    if ( ESP_CODEC_DEV_OK != rc )
    {
        KLOG_E(TAG, "the codec would not open at %u Hz",
               (unsigned)KDRV_ES8311_RATE_HZ);
        return KODE_ERROR;
    }
    s_open = true;
    s_fresh = true;
    /* Muted before anything else the open applies. The amplifier is live
     * throughout and the analog blocks have just come up, so this is the
     * earliest the converter can be silenced - and the first job to write
     * real samples is what lets it speak again (speak_up()). */
    s_hold_mute = true;
    kdrv_es8311_apply_settings();

    /* The loopback, checked rather than assumed. Warned about here and judged by
     * the production row, because a firmware that reached this state would
     * measure its own DAC and call it a microphone. */
    if ( kdrv_es8311_reg_read(REG_DAC2ADC, &loop) == KODE_OK )
    {
        if ( (uint8_t)KDRV_ES8311_DAC2ADC_OFF != loop )
        {
            KLOG_E(TAG, "register 0x44 reads 0x%02X: the DAC is wired into the "
                        "ADC and nothing captured here means anything",
                   (unsigned)loop);
        }
    }
    return KODE_OK;
}

/**
 * @brief Mute the DAC, let it reach silence, then close; the analog goes down.
 *
 * The order is the whole point, and it is the fix for the pop at the end of
 * every sound. The amplifier's enable is the C5's IO4 and stays high, so
 * esp_codec_dev_close() - which powers the converter's analog blocks down,
 * because esp_codec_set_disable_when_closed() asked it to - lands on a live
 * class-D whatever this driver does. What it must not land on is a converter
 * still holding a bias: that step IS the pop. So the mute goes first and is
 * given ES_MUTE_SETTLE_MS of the ramp's own time before the power comes off.
 *
 * The hold is left ON, which is what the next open wants: it comes up muted
 * and the settle blocks are what let it speak.
 *
 * @return KODE_OK.
 */
static kode_status_t do_close(void)
{
    if ( s_open )
    {
        /* Normally a no-op by now: every sound leaves the converter muted,
         * and this is the idle timer arriving on top of that. It still has to
         * be here, because a close can also follow a recording, which never
         * drove the converter at all. */
        mute_and_settle();
        (void)esp_codec_dev_close(s_dev);
        s_open = false;
        s_fresh = false;
    }
    return KODE_OK;
}

/**
 * @brief Fill the transmit block with one candidate's tone, both slots.
 *
 * @param bin Which candidate.
 * @param n0  The frame index the first sample carries.
 * @param amp Peak amplitude.
 */
static void tx_tone(uint8_t bin, uint32_t n0, int16_t amp)
{
    size_t f;

    for ( f = 0U; (size_t)KTONE_BLOCK_FRAMES > f; ++f )
    {
        int16_t v = ktone_sample(bin, n0 + (uint32_t)f, amp);
        s_txb[f * ES_SLOTS]      = v;
        s_txb[(f * ES_SLOTS) + 1U] = v;
    }
}

/** @brief Fill the transmit block with silence. */
static void tx_silence(void)
{
    (void)memset(s_txb, 0, sizeof(s_txb));
}

/**
 * @brief End a sound: write silence until the ring has pushed it all out.
 *
 * The far end of every job that emits. It returns only once the last sample
 * of the sound has actually left the controller, because `esp_codec_dev_write()`
 * blocks for a free DMA buffer and there are none until the ones in front of
 * it have been sent - so writing a whole ring's worth of zeros IS the wait.
 *
 * What the DAC is given after a sound is therefore silence this file put
 * there, for long enough that no partial DMA buffer and no ring behind it can
 * hand it anything else. A converter fed zeros holds its bias; a converter
 * handed the tail of somebody else's buffer steps, and a step into an
 * amplifier that is always live is a click.
 *
 * @return KODE_OK, or KODE_IO_ERROR when the bus refused.
 */
static kode_status_t drain_silence(void)
{
    uint32_t b;

    tx_silence();
    for ( b = 0U; (uint32_t)ES_DRAIN_BLOCKS > b; ++b )
    {
        if ( esp_codec_dev_write(s_dev, s_txb, (int)ES_BLOCK_BYTES)
            != ESP_CODEC_DEV_OK )
        {
            return KODE_IO_ERROR;
        }
    }
    return KODE_OK;
}

/**
 * @brief The far end of every job that drove the converter.
 *
 * Two steps, and the order between them is the whole of it.
 *
 * The drain goes FIRST. It is the wait for the sound to leave the controller,
 * so muting before it would ramp down over about thirty milliseconds of audio
 * that has not been heard yet - every sound cut short by its own tail. After
 * it, what the ring still holds is the drain's own silence, and the mute's
 * ramp runs over that: inaudible by construction.
 *
 * The mute then goes SECOND and stays on for the whole idle hold. The channel
 * keeps running and keeps clocking; what changes is that the converter is
 * soft-muted between sounds instead of sitting at its idle level with a live
 * amplifier on the end of it.
 *
 * @return KODE_OK, or KODE_IO_ERROR when the bus refused mid-drain.
 */
static kode_status_t sound_end(void)
{
    kode_status_t const rc = drain_silence();

    if ( KODE_OK != rc )
    {
        return rc;
    }
    mute_and_settle();
    return KODE_OK;
}

/** @brief Copy the left slot of the receive block into the mono buffer. */
static void rx_left(void)
{
    size_t f;

    for ( f = 0U; (size_t)KTONE_BLOCK_FRAMES > f; ++f )
    {
        s_mono[f] = s_rxb[f * ES_SLOTS];
    }
}

/**
 * @brief Move one block each way.
 *
 * Written before read, every block, so the transmit ring stays ahead of the
 * receive one and the two cannot drift apart over a long capture.
 *
 * @return KODE_OK, or KODE_IO_ERROR when either direction refused.
 */
static kode_status_t block_exchange(void)
{
    if ( esp_codec_dev_write(s_dev, s_txb, (int)ES_BLOCK_BYTES)
        != ESP_CODEC_DEV_OK )
    {
        return KODE_IO_ERROR;
    }
    if ( esp_codec_dev_read(s_dev, s_rxb, (int)ES_BLOCK_BYTES)
        != ESP_CODEC_DEV_OK )
    {
        return KODE_IO_ERROR;
    }
    return KODE_OK;
}

static kode_status_t settle_if_fresh(void);

/**
 * @brief Capture one window, emitting @p bin or emitting silence.
 *
 * A fresh open is settled through settle_if_fresh() like every other job,
 * rather than by a rule of its own: that call is also what un-mutes the DAC,
 * and a window that skipped it would measure a converter the driver is still
 * holding silent. Its own settle blocks below stay, and are for the other
 * half of the job - the DMA rings filling and the tone reaching the detector.
 *
 * Those blocks are emitted and thrown away. The tail blocks exist only when
 * something is being emitted, so that a block of slip cannot truncate the
 * tone the detector was measuring.
 *
 * Silence is emitted with the device OPEN and the DAC writing zeros rather
 * than by not writing at all, so that the amplifier is in the same electrical
 * state it will be in when a tone is measured against this.
 *
 * @param emit Whether to emit @p bin at all.
 * @param bin  Which candidate.
 * @param amp  Peak amplitude.
 * @return KODE_OK, KODE_DENIED when the device is closed, KODE_IO_ERROR when
 *         the bus refused.
 */
static kode_status_t do_window(bool emit, uint8_t bin, int16_t amp)
{
    uint32_t settle = (uint32_t)ES_SETTLE_BLOCKS;
    uint32_t blocks;
    uint32_t b;
    uint32_t n = 0U;

    if ( !s_open )
    {
        return KODE_DENIED;
    }
    if ( s_fresh )
    {
        /* The first capture of an open waits out the input's start-up
         * step; any later one finds it long settled. */
        settle += (uint32_t)ES_FRESH_RX_BLOCKS;
    }
    if ( settle_if_fresh() != KODE_OK )
    {
        return KODE_IO_ERROR;
    }

    blocks = settle + (uint32_t)ES_WINDOW_BLOCKS;
    if ( emit )
    {
        blocks += (uint32_t)ES_TAIL_BLOCKS;
    }

    ktone_reset(&s_det);
    (void)memset(&s_result, 0, sizeof(s_result));
    s_result.bin = bin;

    for ( b = 0U; b < blocks; ++b )
    {
        if ( emit )
        {
            tx_tone(bin, n, amp);
        }
        else
        {
            tx_silence();
        }
        n += (uint32_t)KTONE_BLOCK_FRAMES;

        if ( block_exchange() != KODE_OK )
        {
            return KODE_IO_ERROR;
        }
        /* Un-muted after the first block, whether this window emits or not:
         * step 1's floor has to be measured with the amplifier in exactly the
         * state step 2 will be measured in, and a converter the driver was
         * still holding silent is not that state. */
        speak_up();
        if ( (settle <= b) && ((settle + (uint32_t)ES_WINDOW_BLOCKS) > b) )
        {
            rx_left();
            ktone_feed(&s_det, s_mono, (size_t)KTONE_BLOCK_FRAMES);
        }
    }
    s_result.frames = KTONE_WIN_N;
    /* A probe stops at full amplitude - the tail blocks above are still the
     * tone - so it ends on a step like any other sound, and the row is what
     * an operator hears most of. Ended here, after the window has been fed,
     * so the measurement is exactly what it was. The floor window comes
     * through too: it drove the converter with zeros, and leaving it the one
     * job that walks away un-muted would be a second rule. */
    if ( sound_end() != KODE_OK )
    {
        return KODE_IO_ERROR;
    }
    return ktone_result(&s_det, &s_result.win);
}

/**
 * @brief Emit the settle blocks of silence a fresh open needs, and un-mute.
 *
 * The amplifier starts and the DAC's volume ramps over the first blocks
 * after an open, so anything emitted there is swallowed. Every job that
 * touches the converter after an open comes through here first - a tone, a
 * play and a window alike - so the settle is paid once per open and a second
 * sound inside the idle window pays nothing. The cost is about 32 ms of
 * latency on the first sound after a pause, which is cheaper than a sound
 * nobody hears.
 *
 * It does NOT un-mute. The converter is muted between sounds, not merely
 * between opens, so speak_up() is called by each job once its first block of
 * real samples is in the ring - one rule, and this one would be a second.
 *
 * @return KODE_OK, or KODE_IO_ERROR when the bus refused.
 */
static kode_status_t settle_if_fresh(void)
{
    uint32_t b;

    if ( !s_fresh )
    {
        return KODE_OK;
    }
    s_fresh = false;
    tx_silence();
    for ( b = 0U; (uint32_t)ES_SETTLE_BLOCKS > b; ++b )
    {
        if ( esp_codec_dev_write(s_dev, s_txb, (int)ES_BLOCK_BYTES)
            != ESP_CODEC_DEV_OK )
        {
            return KODE_IO_ERROR;
        }
    }
    return KODE_OK;
}

/**
 * @brief Emit one candidate for a while and capture nothing.
 *
 * The console's verb. The receive ring keeps running underneath and is
 * ignored; a ring nobody drains overwrites itself, which costs nothing.
 *
 * @param req The job.
 * @return KODE_OK, KODE_DENIED when closed, KODE_IO_ERROR when refused.
 */
static kode_status_t do_tone(const kdrv_es8311_req_t *req)
{
    uint32_t frames = (uint32_t)req->ms * (KDRV_ES8311_RATE_HZ / 1000U);
    uint32_t done   = 0U;

    if ( !s_open )
    {
        return KODE_DENIED;
    }
    (void)memset(&s_result, 0, sizeof(s_result));
    s_result.bin = req->bin;

    if ( settle_if_fresh() != KODE_OK )
    {
        return KODE_IO_ERROR;
    }

    while ( done < frames )
    {
        tx_tone(req->bin, done, req->amp);
        if ( esp_codec_dev_write(s_dev, s_txb, (int)ES_BLOCK_BYTES)
            != ESP_CODEC_DEV_OK )
        {
            return KODE_IO_ERROR;
        }
        done += (uint32_t)KTONE_BLOCK_FRAMES;
        /* With the first block already in the ring, so the ramp back up has
         * signal to ride on rather than the idle level. */
        speak_up();
    }
    s_result.frames = done;
    return sound_end();
}

/**
 * @brief Write @p req->frames mono frames from @p req->buf.
 *
 * One continuous feed with no gap between blocks, which is why a caller hands
 * over the whole sound rather than a piece of it: a seam between two submits
 * would be an audible click.
 *
 * @param req The job.
 * @return KODE_OK, KODE_DENIED when closed or @c buf is NULL, KODE_IO_ERROR.
 */
static kode_status_t do_play(const kdrv_es8311_req_t *req)
{
    uint32_t done = 0U;

    if ( !s_open )
    {
        return KODE_DENIED;
    }
    if ( NULL == req->buf )
    {
        return KODE_DENIED;
    }
    (void)memset(&s_result, 0, sizeof(s_result));

    if ( settle_if_fresh() != KODE_OK )
    {
        return KODE_IO_ERROR;
    }

    while ( done < req->frames )
    {
        uint32_t n = req->frames - done;
        size_t   f;

        if ( (uint32_t)KTONE_BLOCK_FRAMES < n )
        {
            n = (uint32_t)KTONE_BLOCK_FRAMES;
        }
        /* The last block is short, so the tail of it is cleared rather than
         * left holding the previous block: a repeated fragment at the end of
         * a sound is exactly the artefact auto_clear exists to prevent. */
        tx_silence();
        for ( f = 0U; (size_t)n > f; ++f )
        {
            int16_t v = req->buf[done + (uint32_t)f];
            s_txb[f * ES_SLOTS]        = v;
            s_txb[(f * ES_SLOTS) + 1U] = v;
        }
        if ( esp_codec_dev_write(s_dev, s_txb, (int)ES_BLOCK_BYTES)
            != ESP_CODEC_DEV_OK )
        {
            return KODE_IO_ERROR;
        }
        done += n;
        /* With the first block already in the ring, so the ramp back up has
         * signal to ride on rather than the idle level. */
        speak_up();
    }
    s_result.frames = done;
    /* The sound is not over when the last block is written; it is over when
     * the ring has played it and the converter has gone quiet. PLAY_DONE
     * means both - which lets a caller that waits on the end of its sound
     * ask for the BATFET write on that end instead of on a clock. */
    return sound_end();
}

/**
 * @brief Read up to @p req->frames mono frames into @p req->buf.
 *
 * The first blocks are kept, not discarded: unlike a window, a recording is
 * whatever the microphone heard and trimming its start would be a decision
 * this layer has no business making.
 *
 * @param req The job.
 * @return KODE_OK, KODE_DENIED when closed or @c buf is NULL, KODE_IO_ERROR.
 */
static kode_status_t do_record(const kdrv_es8311_req_t *req)
{
    uint32_t done = 0U;

    if ( !s_open )
    {
        return KODE_DENIED;
    }
    if ( NULL == req->buf )
    {
        return KODE_DENIED;
    }
    (void)memset(&s_result, 0, sizeof(s_result));
    tx_silence();

    while ( (done < req->frames) && !s_record_end )
    {
        uint32_t n = req->frames - done;

        if ( (uint32_t)KTONE_BLOCK_FRAMES < n )
        {
            n = (uint32_t)KTONE_BLOCK_FRAMES;
        }
        if ( block_exchange() != KODE_OK )
        {
            return KODE_IO_ERROR;
        }
        rx_left();
        (void)memcpy(&req->buf[done], s_mono, (size_t)n * sizeof(int16_t));
        done += n;
    }
    s_result.frames = done;
    return KODE_OK;
}

/**
 * @brief Play the stream's ring until its writer is done with it.
 *
 * One block per pass, as PLAY writes them: whatever the ring holds, up to a
 * block, and silence after it. A dry ring is a whole block of silence rather
 * than a pause, because the controller has to be fed either way and a gap in
 * what it is fed is a click; the write blocks for a free DMA buffer, so a dry
 * ring costs real time and never a spin. The converter speaks from the first
 * block with frames in it, and the job ends with the usual drain once the
 * writer has said it is done and the ring is empty.
 *
 * @return KODE_OK, KODE_DENIED when closed, KODE_IO_ERROR when refused.
 */
static kode_status_t do_stream(void)
{
    uint32_t      moved = 0U;
    kode_status_t rc    = KODE_OK;

    (void)memset(&s_result, 0, sizeof(s_result));
    if ( !s_open )
    {
        rc = KODE_DENIED;
    }
    else if ( settle_if_fresh() != KODE_OK )
    {
        rc = KODE_IO_ERROR;
    }
    else
    {
        for ( ;; )
        {
            /* The end is read before the take: a last write and the end
             * landing between the two would otherwise lose that write. */
            bool const     ending = s_stream_end;
            uint32_t const n      = kring_take(&s_stream, s_mono,
                                               (uint32_t)KTONE_BLOCK_FRAMES);
            size_t f;

            if ( (0U == n) && ending )
            {
                break;
            }
            tx_silence();
            for ( f = 0U; (size_t)n > f; ++f )
            {
                s_txb[f * ES_SLOTS]        = s_mono[f];
                s_txb[(f * ES_SLOTS) + 1U] = s_mono[f];
            }
            if ( esp_codec_dev_write(s_dev, s_txb, (int)ES_BLOCK_BYTES)
                != ESP_CODEC_DEV_OK )
            {
                rc = KODE_IO_ERROR;
                break;
            }
            if ( 0U < n )
            {
                moved += n;
                khal_sem_signal(s_stream_room);
                speak_up();
            }
        }
    }
    /* However it ended, nothing more goes in: a writer still feeding a stream
     * the job has left would be filling a ring nobody plays. A writer asleep
     * on a full ring is let go, and finds the stream shut. */
    s_stream_open = false;
    khal_sem_signal(s_stream_room);
    s_result.frames = moved;
    if ( KODE_OK == rc )
    {
        rc = sound_end();
    }
    return rc;
}

/* ---- Running a job --------------------------------------------------------- */

kode_status_t kdrv_es8311_run(const kdrv_es8311_req_t *req)
{
    kode_status_t rc = KODE_DENIED;

    if ( (NULL == req) || !s_present )
    {
        return KODE_DENIED;
    }
    switch ( req->type )
    {
        case KDRV_ES8311_JOB_OPEN:
            rc = do_open();
            break;
        case KDRV_ES8311_JOB_CLOSE:
            rc = do_close();
            break;
        case KDRV_ES8311_JOB_TONE:
            rc = do_tone(req);
            break;
        case KDRV_ES8311_JOB_FLOOR:
            rc = do_window(false, 0U, 0);
            break;
        case KDRV_ES8311_JOB_PROBE:
            rc = do_window(true, req->bin, req->amp);
            break;
        case KDRV_ES8311_JOB_PLAY:
            rc = do_play(req);
            break;
        case KDRV_ES8311_JOB_RECORD:
            rc = do_record(req);
            break;
        case KDRV_ES8311_JOB_STREAM:
            rc = do_stream();
            break;
        default:
            KLOG_E(TAG, "job %d is not one of ours", (int)req->type);
            break;
    }
    return rc;
}
