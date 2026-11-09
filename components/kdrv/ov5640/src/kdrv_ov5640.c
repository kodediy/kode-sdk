/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_ov5640.c
 * @brief The OV5640 camera module on J3: plug side and version, SCCB, DVP capture, three buffers.
 */
#include "kdrv_ov5640.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_cache.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_dvp.h"
#include "esp_cam_sensor.h"
#include "esp_err.h"
#include "esp_rom_sys.h"
#include "esp_sccb_i2c.h"
#include "esp_sccb_intf.h"
#include "ov5640.h"
#include "sdkconfig.h"

#include "kbsp_exp.h"
#include "kbsp_station.h"
#include "khal_exp.h"
#include "khal_sync.h"
#include "khal_system.h"
#include "khal_time.h"
#include "kode_log.h"

/* The field is the sensor's own RGB565 big-endian 800x600 table, loaded as its
 * default format (esp_cam_sensor_set_format() with NULL also loads the reset list). */
#if !defined(CONFIG_CAMERA_OV5640) || !defined(CONFIG_CAMERA_OV5640_DVP_RGB565_BE_800X600_10FPS) \
    || !defined(CONFIG_CAMERA_OV5640_DVP_DEFAULT_FMT_RGB565_BE_800X600_10FPS)
#error "kdrv_ov5640 needs CONFIG_CAMERA_OV5640, CONFIG_CAMERA_OV5640_DVP_RGB565_BE_800X600_10FPS and CONFIG_CAMERA_OV5640_DVP_DEFAULT_FMT_RGB565_BE_800X600_10FPS"
#endif

#define TAG "kdrv.ov5640"

/* ---- Finding the module ----------------------------------------------------- */
#define SIDE_UPRIGHT        0U     /**< Index of the upright side in SIDE_MAPS.            */
#define SIDE_TURNED         1U     /**< Index of the turned side in SIDE_MAPS.             */
#define VERSION_V10         0U     /**< Index of v1.0 in SIDE_MAPS.                        */
#define VERSION_V11         1U     /**< Index of v1.1 in SIDE_MAPS.                        */
#define PAIR_SETTLE_MS      2U     /**< After the pulls are set, before the pair is read.  */
#define PAIR_READS          20U    /**< Reads, PAIR_READ_GAP_MS apart, that must agree.    */
#define PAIR_READ_GAP_MS    1U     /**< Between two reads of a pair.                       */
#define PROBE_ROUND_GAP_MS  200U   /**< Between two looks at both pairs.                   */
#define BUS_FREE_CLOCKS     9U     /**< Clocks that let a slave held mid-byte finish it.   */
#define BUS_FREE_HALF_US    10U    /**< Half a clock of that release, in us.               */
#define VERSION_SETTLE_MS   10U    /**< After the stream stops, before PWDN's pull is read. */
#define PULL_SETTLE_MS      2U     /**< After a pull-up is set, before the pin is read.    */

/* ---- J3's I2C bus -------------------------------------------------------------- */
#define I2C_GLITCH_CYCLES   7U     /**< Pulses shorter than this many clock cycles are ignored. */
#define I2C_STOP_MS         50     /**< The stream-off write's wait, in ms (IDF takes an int). */

/* ---- The DVP and the buffers ------------------------------------------------- */
#define DVP_CTLR_ID         0      /**< The P4's one DVP controller.                       */
#define DVP_BURST_BYTES     64U    /**< The DVP's DMA burst.                               */
#define DVP_DEL_TRIES       10U    /**< Tries to delete a controller that is still busy.   */
#define DVP_DEL_GAP_MS      60U    /**< Before each of those tries.                        */
#define STREAM_STOP_MS      50U    /**< After the sensor stops streaming, before the DVP stops. */
#define OPEN_TRIES          3U     /**< Builds of the capture before an open gives up.     */
#define OPEN_RETRY_MS       300U   /**< Between two of those builds.                       */
#define CFG_LOST_TRIES      2U     /**< Frames a grab re-takes after the settings were lost. */
#define BYTES_RGB565        2U     /**< Bytes a pixel in RGB565.                           */
#define BYTES_RAW8          1U     /**< Bytes a pixel in RAW8.                             */
#define BUF_FLOOR_W         1056U  /**< Smallest buffer, the test's biggest window: width. */
#define BUF_FLOOR_H         768U   /**< Smallest buffer, the test's biggest window: height. */
#define BUF_ALIGN           ((size_t)CONFIG_CACHE_L2_CACHE_LINE_SIZE) /**< PSRAM's DMA and cache line. */
#define NO_BUF              (-1)   /**< No buffer: none done, none held.                   */
#define VCM_UNKNOWN         (-1)   /**< The lens motor's code is not known since the last reset. */

_Static_assert(KBSP_CAM_DATA_LINES <= ESP_CAM_CTLR_DVP_DATA_SIG_NUM, "the DVP takes eight data lines");

/* ---- OV5640 registers ------------------------------------------------------- */
#define BYTE_BITS           8U      /**< A register's width.                               */
#define OV_REG_STREAM       0x4202U /**< Frame control: OV_STREAM_OFF stops the stream, no reset. */
#define OV_STREAM_OFF       0x0FU   /**< Stream off.                                        */
#define OV_REG_X_START      0x3800U /**< Array window, first column (16 bits).              */
#define OV_REG_Y_START      0x3802U /**< Array window, first row (16 bits).                 */
#define OV_REG_X_END        0x3804U /**< Array window, last column (16 bits).               */
#define OV_REG_Y_END        0x3806U /**< Array window, last row (16 bits).                  */
#define OV_REG_X_OUT        0x3808U /**< Output width (16 bits).                            */
#define OV_REG_Y_OUT        0x380AU /**< Output height (16 bits).                           */
#define OV_WIN_X_MARGIN     31U     /**< The ISP's 32 columns of margin, less one.          */
#define OV_WIN_Y_MARGIN     7U      /**< The ISP's 8 rows of margin, less one.              */
#define OV_REG_TC20         0x3820U /**< Timing control 20: vertical flip and binning.      */
#define OV_REG_TC21         0x3821U /**< Timing control 21: mirror and binning.             */
#define OV_TC_FLIP_BITS     0x06U   /**< The flip (TC20) or mirror (TC21) bits.             */
#define OV_TC20_NO_VBIN     0x40U   /**< TC20: no vertical binning (windows).               */
#define OV_REG_BLC_4514     0x4514U /**< Black level: follows the flips in a window.        */
#define OV_4514_VFLIP       0x00U   /**< 0x4514 with the vertical flip on.                  */
#define OV_4514_MIRROR      0xBBU   /**< 0x4514 with the mirror alone.                      */
#define OV_4514_NONE        0x88U   /**< 0x4514 with neither.                               */
#define OV_REG_ISP0         0x5000U /**< ISP control 00.                                    */
#define OV_ISP0_LENC        0x80U   /**< Lens correction.                                   */
#define OV_ISP0_GMA         0x20U   /**< RAW gamma.                                         */
#define OV_ISP0_BPC         0x04U   /**< Black pixel cancelling.                            */
#define OV_ISP0_WPC         0x02U   /**< White pixel cancelling.                            */
#define OV_REG_ISP1         0x5001U /**< ISP control 01.                                    */
#define OV_ISP1_SDE         0x80U   /**< Special digital effects.                           */
#define OV_ISP1_SCALE       0x20U   /**< The scaler.                                        */
#define OV_REG_FMT_MUX      0x501FU /**< ISP format mux.                                    */
#define OV_FMT_MUX_RAW      0x03U   /**< The mux on RAW.                                    */
#define OV_REG_FORMAT       0x4300U /**< Output format.                                     */
#define OV_FORMAT_RAW       0x00U   /**< RAW out.                                           */
#define OV_REG_CIP          0x5308U /**< CIP control.                                       */
#define OV_CIP_DNS_MANUAL   0x40U   /**< Manual de-noise.                                   */
#define OV_REG_DNS_OFFSET   0x5306U /**< Manual de-noise offset.                            */
#define OV_REG_SHARP_MT1    0x5302U /**< Manual sharpen, first value.                       */
#define OV_REG_SHARP_MT2    0x5303U /**< Manual sharpen, second value.                      */
#define OV_REG_AEC0         0x3A00U /**< AEC control 00.                                    */
#define OV_AEC0_BANDING     0x20U   /**< The banding filter.                                */
#define OV_REG_AWB_MANUAL   0x3406U /**< AWB manual control.                                */
#define OV_AWB_MANUAL       0x01U   /**< White balance from the gains below.                */
#define OV_REG_AWB_R        0x3400U /**< Red gain (16 bits).                                */
#define OV_REG_AWB_G        0x3402U /**< Green gain (16 bits).                              */
#define OV_REG_AWB_B        0x3404U /**< Blue gain (16 bits).                               */
#define OV_REG_AEC_MANUAL   0x3503U /**< AEC/AGC manual control.                            */
#define OV_AEC_MANUAL_EXP   0x01U   /**< Exposure from OV_REG_EXP.                          */
#define OV_AEC_MANUAL_GAIN  0x02U   /**< Gain from OV_REG_GAIN.                             */
#define OV_REG_EXP          0x3500U /**< Exposure, three registers, in 1/16 lines.          */
#define OV_EXP_FRAC_BITS    4U      /**< Exposure's fraction bits.                          */
#define OV_EXP_TOP_SHIFT    16U     /**< The top exposure register's shift.                 */
#define OV_EXP_TOP_MASK     0x0FU   /**< The top exposure register's bits.                  */
#define OV_REG_GAIN         0x350AU /**< Gain, two registers.                               */
#define OV_GAIN_TOP_MASK    0x03U   /**< The top gain register's bits.                      */
#define OV_REG_VCM_LOW      0x3602U /**< Lens DAC, low four bits in the top half.           */
#define OV_REG_VCM_HIGH     0x3603U /**< Lens DAC, high six bits.                           */
#define OV_VCM_SHIFT        4U      /**< Where the DAC splits between the two.              */
#define OV_VCM_HIGH_MASK    0x3FU   /**< The high register's bits.                          */
#define OV_VCM_LOW_MASK     0x0FU   /**< The low register's DAC bits and the bits it keeps. */

/** @brief One register and the value written to it. */
typedef struct {
    uint16_t reg;  /**< The register. */
    uint8_t  val;  /**< Its value.    */
} ov_reg_t;

/* The native window: camera_lab's QSXGA table (the full array, ISP offset
 * 16/4, HTS 2844, VTS 1968, no binning, no scaler). */
static const ov_reg_t OV_WINDOW_TABLE[] = {
    { 0x380CU, 0x0BU }, { 0x380DU, 0x1CU },   /* HTS                    */
    { 0x380EU, 0x07U }, { 0x380FU, 0xB0U },   /* VTS                    */
    { 0x3810U, 0x00U }, { 0x3811U, 0x10U },   /* ISP horizontal offset  */
    { 0x3812U, 0x00U }, { 0x3813U, 0x04U },   /* ISP vertical offset    */
    { 0x3814U, 0x11U }, { 0x3815U, 0x11U },   /* no subsampling         */
    { 0x4520U, 0x10U },                       /* BLC                    */
    { 0x3618U, 0x04U }, { 0x3612U, 0x2BU },   /* analog, full array     */
    { 0x3708U, 0x63U }, { 0x3709U, 0x12U }, { 0x370CU, 0x00U },
    { 0x4001U, 0x02U }, { 0x4004U, 0x06U },   /* BLC start line, lines  */
};

/** @brief What a pair of lines reads with pull-downs on. */
typedef enum {
    PAIR_NONE = 0,  /**< Nothing pulls them up.                       */
    PAIR_PULLUPS,   /**< Both held high: a module's pull-ups.         */
    PAIR_SDA_LOW,   /**< SCL high, SDA held low: a slave stuck mid-byte. */
} pair_t;

/** @brief The maps of each side, by version; the SCCB pair is the same in both. */
static const uint8_t SIDE_MAPS[KBSP_CAM_SIDES][KBSP_CAM_VERSIONS] = {
    [SIDE_UPRIGHT] = { [VERSION_V10] = KBSP_CAM_MAP_V10_UPRIGHT, [VERSION_V11] = KBSP_CAM_MAP_V11_UPRIGHT },
    [SIDE_TURNED]  = { [VERSION_V10] = KBSP_CAM_MAP_V10_TURNED,  [VERSION_V11] = KBSP_CAM_MAP_V11_TURNED  },
};

static bool                     s_lent;                    /**< J3 and its I2C controller are ours.  */
static uint8_t                  s_gpio[KBSP_EXP_COUNT];    /**< The GPIO behind each position.        */
static uint8_t                  s_i2c_id;                  /**< The controller the loan gave.         */
static i2c_master_bus_handle_t  s_bus;                     /**< J3's bus; NULL until a probe found the camera. */
static uint8_t                  s_map;                     /**< The map found; valid while s_bus is.  */
static esp_cam_sensor_device_t *s_sensor;                  /**< The sensor, from the first open.      */
static esp_cam_ctlr_handle_t    s_ctlr;                    /**< The DVP controller.                   */
static bool                     s_cbs;                     /**< Its callbacks are registered.         */
static bool                     s_enabled;                 /**< It is enabled.                        */
static bool                     s_capturing;               /**< It is capturing.                      */
static kdrv_ov5640_mode_t       s_mode;                    /**< The capture asked for.                */
static kdrv_ov5640_cfg_t        s_cfg = { .vflip = true }; /**< The settings asked for.               */
static bool                     s_prog_win;                /**< The sensor holds a window since its last reset. */
static kdrv_ov5640_fmt_t        s_prog_fmt;                /**< And in this format.                   */
static int32_t                  s_vcm_hw = VCM_UNKNOWN;    /**< The lens code written since the last reset. */
static esp_cam_ctlr_trans_t     s_trans;                   /**< The transfer the DVP is handed.       */
static khal_sem_t               s_frame;                   /**< Signalled by the DVP at each frame.   */
static void                    *s_bufs[KDRV_OV5640_BUFS];  /**< The frame buffers, in PSRAM.          */
static size_t                   s_buflen;                  /**< Bytes a frame of the current mode.    */
static size_t                   s_bufcap;                  /**< Bytes each buffer holds.              */
static volatile int32_t         s_filling;                 /**< The buffer the DMA fills.             */
static volatile int32_t         s_last = NO_BUF;           /**< The newest finished frame.            */
static volatile int32_t         s_cam_held = NO_BUF;       /**< The frame the caller holds.           */

/** @brief The GPIO behind J3 position @p pos, from the loan. */
static gpio_num_t gpio_of(uint8_t pos)
{
    return (gpio_num_t)s_gpio[pos - KHAL_EXP_POS_MIN];
}

/** @brief @p n rounded up to BUF_ALIGN. */
static size_t align_up(size_t n)
{
    return ((n + BUF_ALIGN) - 1U) & ~(BUF_ALIGN - 1U);
}

/* ---- DVP interrupts --------------------------------------------------------- */

/** @brief A buffer that is neither @p done nor @p held. */
static int32_t free_buffer(int32_t done, int32_t held)
{
    int32_t pick = NO_BUF;

    for ( int32_t i = 0; ((int32_t)KDRV_OV5640_BUFS > i) && (NO_BUF == pick); i++ )
    {
        if ( (i != done) && (i != held) )
        {
            pick = i;
        }
    }
    if ( NO_BUF == pick )
    {
        pick = (done + 1) % (int32_t)KDRV_OV5640_BUFS;
    }
    return pick;
}

/** @brief The DVP wants the next buffer: one the caller does not hold. */
static bool on_new(esp_cam_ctlr_handle_t h, esp_cam_ctlr_trans_t *trans, void *ud)
{
    (void)h;
    (void)ud;
    s_filling      = free_buffer(s_filling, s_cam_held);
    s_trans.buffer = s_bufs[s_filling];
    s_trans.buflen = s_buflen;
    *trans         = s_trans;
    return false;
}

/** @brief The DVP finished a frame: note which and wake the grab. */
static bool on_done(esp_cam_ctlr_handle_t h, esp_cam_ctlr_trans_t *trans, void *ud)
{
    int32_t hit = NO_BUF;

    (void)h;
    (void)ud;
    for ( int32_t i = 0; (int32_t)KDRV_OV5640_BUFS > i; i++ )
    {
        if ( (NULL != trans) && (trans->buffer == s_bufs[i]) )
        {
            hit = i;
        }
    }
    if ( NO_BUF != hit )
    {
        s_last = hit;
        khal_sem_signal(s_frame);
    }
    return false;
}

/* ---- SCCB --------------------------------------------------------------------- */

/** @brief Read one sensor register. */
static bool rd(uint16_t reg, uint8_t *v)
{
    bool ok = false;

    if ( (NULL != s_sensor) && (NULL != s_sensor->sccb_handle) )
    {
        ok = (ESP_OK == esp_sccb_transmit_receive_reg_a16v8(s_sensor->sccb_handle, reg, v));
    }
    return ok;
}

/** @brief Write one sensor register. */
static bool wr(uint16_t reg, uint8_t v)
{
    bool ok = false;

    if ( (NULL != s_sensor) && (NULL != s_sensor->sccb_handle) )
    {
        ok = (ESP_OK == esp_sccb_transmit_reg_a16v8(s_sensor->sccb_handle, reg, v));
    }
    return ok;
}

/** @brief Clear @p clear and set @p set in one register. */
static bool rmw(uint16_t reg, uint8_t clear, uint8_t set)
{
    uint8_t v  = 0U;
    bool    ok = rd(reg, &v);

    if ( ok )
    {
        ok = wr(reg, (uint8_t)((v & (uint8_t)~clear) | set));
    }
    return ok;
}

/** @brief Write a 16-bit value to a register pair, high byte first. */
static bool wr16(uint16_t reg, uint16_t v)
{
    return wr(reg, (uint8_t)(v >> BYTE_BITS)) && wr((uint16_t)(reg + 1U), (uint8_t)v);
}

/* ---- J3's bus and the loan ---------------------------------------------------- */

/** @brief Bring J3's bus up on @p scl / @p sda. */
static kode_status_t bus_up(gpio_num_t scl, gpio_num_t sda)
{
    kode_status_t                 st  = KODE_OK;
    const i2c_master_bus_config_t cfg = {
        .i2c_port                     = (i2c_port_num_t)s_i2c_id,
        .sda_io_num                   = sda,
        .scl_io_num                   = scl,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = I2C_GLITCH_CYCLES,
        .flags.enable_internal_pullup = false, /* the module and the station have them */
    };
    const esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);

    if ( ESP_OK != err )
    {
        s_bus = NULL;
        st    = KODE_ERROR;
        KLOG_W(TAG, "J3's I2C bus: %s", esp_err_to_name(err));
    }
    return st;
}

/** @brief Take J3's bus down. */
static void bus_down(void)
{
    if ( NULL != s_bus )
    {
        (void)i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
}

/** @brief Lend J3's fourteen positions and take its I2C controller, both under the tag. */
static kode_status_t j3_take(void)
{
    uint8_t       pos[KBSP_EXP_COUNT];
    kode_status_t st = KODE_OK;

    for ( uint8_t i = 0U; KBSP_EXP_COUNT > i; i++ )
    {
        pos[i] = (uint8_t)(KHAL_EXP_POS_MIN + i);
    }
    if ( KODE_OK != khal_exp_lend(pos, KBSP_EXP_COUNT, KDRV_OV5640_TAG, s_gpio) )
    {
        st = KODE_DENIED;
    }
    else
    {
        st = khal_exp_ctrl(KBSP_EXP_I2C, KDRV_OV5640_TAG, &s_i2c_id);
        if ( KODE_OK == st )
        {
            s_lent = true;
        }
        else
        {
            (void)khal_exp_return(KDRV_OV5640_TAG);
        }
    }
    return st;
}

/** @brief Give J3 and its controller back. */
static void j3_give(void)
{
    if ( s_lent )
    {
        (void)khal_exp_return(KDRV_OV5640_TAG);
        s_lent = false;
    }
}

/* ---- Plug side and module version --------------------------------------------- */

/**
 * @brief Read a pair with pull-downs on, PAIR_READS times.
 *
 * A pair counts as pull-ups only when it reads high every time (a data line
 * with video moves), and as a stuck slave only when SDA reads low every time
 * with SCL high.
 */
static pair_t read_pair(gpio_num_t scl, gpio_num_t sda)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << (uint32_t)scl) | (1ULL << (uint32_t)sda),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    uint32_t up      = 0U;
    uint32_t sda_low = 0U;
    pair_t   pair    = PAIR_NONE;

    (void)gpio_config(&cfg);
    khal_delay_ms(PAIR_SETTLE_MS);
    for ( uint32_t i = 0U; PAIR_READS > i; i++ )
    {
        const bool c = (0 != gpio_get_level(scl));
        const bool d = (0 != gpio_get_level(sda));

        if ( c && d )
        {
            up++;
        }
        if ( c && !d )
        {
            sda_low++;
        }
        khal_delay_ms(PAIR_READ_GAP_MS);
    }
    cfg.mode         = GPIO_MODE_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    (void)gpio_config(&cfg);
    if ( PAIR_READS == up )
    {
        pair = PAIR_PULLUPS;
    }
    else if ( PAIR_READS == sda_low )
    {
        pair = PAIR_SDA_LOW;
    }
    else
    {
        pair = PAIR_NONE;
    }
    return pair;
}

/** @brief Free a slave holding SDA low: clocks until it lets go, then a STOP. */
static void free_bus(gpio_num_t scl, gpio_num_t sda)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << (uint32_t)scl) | (1ULL << (uint32_t)sda),
        .mode         = GPIO_MODE_INPUT_OUTPUT_OD,
    };

    (void)gpio_set_level(scl, 1U);
    (void)gpio_set_level(sda, 1U);
    (void)gpio_config(&cfg);
    esp_rom_delay_us(BUS_FREE_HALF_US);
    for ( uint32_t i = 0U; (BUS_FREE_CLOCKS > i) && (0 == gpio_get_level(sda)); i++ )
    {
        (void)gpio_set_level(scl, 0U);
        esp_rom_delay_us(BUS_FREE_HALF_US);
        (void)gpio_set_level(scl, 1U);
        esp_rom_delay_us(BUS_FREE_HALF_US);
    }
    (void)gpio_set_level(sda, 0U);
    esp_rom_delay_us(BUS_FREE_HALF_US);
    (void)gpio_set_level(scl, 1U);
    esp_rom_delay_us(BUS_FREE_HALF_US);
    (void)gpio_set_level(sda, 1U);
    esp_rom_delay_us(BUS_FREE_HALF_US);
    cfg.mode = GPIO_MODE_DISABLE;
    (void)gpio_config(&cfg);
}

/** @brief Whether the sensor answers on J3's bus; the write that asks also stops its stream. */
static bool sensor_answers(void)
{
    const i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = KBSP_CAM_SCCB_ADDR,
        .scl_speed_hz    = KBSP_STATION_I2C_HZ,
    };
    const uint8_t stop[] = { (uint8_t)(OV_REG_STREAM >> BYTE_BITS), (uint8_t)OV_REG_STREAM, OV_STREAM_OFF };
    i2c_master_dev_handle_t dev = NULL;
    bool                    ok  = false;

    if ( ESP_OK == i2c_master_bus_add_device(s_bus, &dc, &dev) )
    {
        ok = (ESP_OK == i2c_master_transmit(dev, stop, sizeof(stop), I2C_STOP_MS));
        (void)i2c_master_bus_rm_device(dev);
    }
    return ok;
}

/**
 * @brief Look at both sides until a sensor answers on one or @p budget_ms have passed.
 * @param budget_ms The longest the search may take.
 * @param side      Out: SIDE_UPRIGHT or SIDE_TURNED.
 * @return KODE_OK with J3's bus up on that side; KODE_TIMEOUT; KODE_ERROR when IDF refused the bus.
 */
static kode_status_t find_side(uint32_t budget_ms, uint8_t *side)
{
    const uint32_t start = khal_time_ms();
    kode_status_t  st    = KODE_TIMEOUT;
    bool           freed = false;
    bool           more  = true;

    while ( more )
    {
        for ( uint8_t s = 0U; (KBSP_CAM_SIDES > s) && (KODE_TIMEOUT == st); s++ )
        {
            const kbsp_cam_map_t *const m    = &KBSP_CAM_MAPS[SIDE_MAPS[s][VERSION_V10]];
            const gpio_num_t            scl  = gpio_of(m->scl);
            const gpio_num_t            sda  = gpio_of(m->sda);
            const pair_t                pair = read_pair(scl, sda);

            if ( PAIR_PULLUPS == pair )
            {
                st = bus_up(scl, sda);
                if ( KODE_OK == st )
                {
                    if ( sensor_answers() )
                    {
                        *side = s;
                    }
                    else
                    {
                        bus_down();
                        st = KODE_TIMEOUT;
                    }
                }
            }
            else if ( (PAIR_SDA_LOW == pair) && !freed )
            {
                KLOG_W(TAG, "SDA held low on GPIO%u: freeing the bus", (unsigned)sda);
                free_bus(scl, sda);
                freed = true;
            }
            else
            {
                /* Nothing on this side yet. */
            }
        }
        more = (KODE_TIMEOUT == st) && ((khal_time_ms() - start) < budget_ms);
        if ( more )
        {
            khal_delay_ms(PROBE_ROUND_GAP_MS);
        }
    }
    return st;
}

/** @brief Whether @p g is pulled down outside the chip: it reads low with the chip's pull-up on. */
static bool has_pulldown(gpio_num_t g)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << (uint32_t)g,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    bool low = false;

    (void)gpio_config(&cfg);
    khal_delay_ms(PULL_SETTLE_MS);
    low = (0 == gpio_get_level(g));
    (void)gpio_set_pull_mode(g, GPIO_FLOATING);
    return low;
}

/** @brief The module's version on @p side, from which PWDN is pulled down; v1.0 when unsure. */
static uint8_t map_of_side(uint8_t side)
{
    const kbsp_cam_map_t *const v10 = &KBSP_CAM_MAPS[SIDE_MAPS[side][VERSION_V10]];
    const kbsp_cam_map_t *const v11 = &KBSP_CAM_MAPS[SIDE_MAPS[side][VERSION_V11]];
    bool                        in_v10;
    bool                        in_v11;
    uint8_t                     map = SIDE_MAPS[side][VERSION_V10];

    khal_delay_ms(VERSION_SETTLE_MS);
    in_v10 = has_pulldown(gpio_of(v10->pwdn));
    in_v11 = has_pulldown(gpio_of(v11->pwdn));
    if ( in_v11 && !in_v10 )
    {
        map = SIDE_MAPS[side][VERSION_V11];
    }
    return map;
}

/* ---- DVP and capture --------------------------------------------------------- */

/** @brief Stop the DVP and delete it, trying again while it is still busy. */
static void ctlr_del(void)
{
    bool gone = (NULL == s_ctlr);

    if ( !gone )
    {
        if ( s_capturing )
        {
            (void)esp_cam_ctlr_stop(s_ctlr);
            s_capturing = false;
        }
        if ( s_enabled )
        {
            (void)esp_cam_ctlr_disable(s_ctlr);
            s_enabled = false;
        }
    }
    /* A controller not deleted stays allocated and the next one finds none free. */
    for ( uint32_t i = 0U; (DVP_DEL_TRIES > i) && !gone; i++ )
    {
        esp_err_t err;

        khal_delay_ms(DVP_DEL_GAP_MS);
        err = esp_cam_ctlr_del(s_ctlr);
        if ( ESP_OK == err )
        {
            s_ctlr = NULL;
            s_cbs  = false;
            gone   = true;
        }
        else
        {
            KLOG_W(TAG, "dvp del: %s (try %u)", esp_err_to_name(err), (unsigned)(i + 1U));
            (void)esp_cam_ctlr_disable(s_ctlr);
        }
    }
}

/** @brief Free the frame buffers. */
static void bufs_free(void)
{
    for ( uint32_t i = 0U; KDRV_OV5640_BUFS > i; i++ )
    {
        khal_free(s_bufs[i]);
        s_bufs[i] = NULL;
    }
    s_bufcap = 0U;
}

/**
 * @brief Buffers for frames of @p need bytes: kept when they fit, taken again when not.
 *
 * Never smaller than the test's biggest window, so a change of window keeps
 * them and PSRAM does not fragment with each pass.
 */
static kode_status_t bufs_fit(size_t need)
{
    const size_t  floor_bytes = (size_t)BUF_FLOOR_W * BUF_FLOOR_H * BYTES_RGB565;
    size_t        want        = need;
    kode_status_t st          = KODE_OK;

    if ( floor_bytes > want )
    {
        want = floor_bytes;
    }
    want = align_up(want);
    if ( (NULL == s_bufs[0]) || (align_up(need) > s_bufcap) )
    {
        bufs_free();
        for ( uint32_t i = 0U; (KDRV_OV5640_BUFS > i) && (KODE_OK == st); i++ )
        {
            s_bufs[i] = khal_alloc_psram(BUF_ALIGN, want);
            if ( NULL == s_bufs[i] )
            {
                st = KODE_NO_RESOURCES;
            }
        }
        if ( KODE_OK == st )
        {
            s_bufcap = want;
        }
        else
        {
            KLOG_W(TAG, "no PSRAM for %u frame buffers of %u bytes", (unsigned)KDRV_OV5640_BUFS,
                   (unsigned)want);
            bufs_free();
        }
    }
    return st;
}

/** @brief A new DVP controller for the current mode, on the map found, and its buffers. */
static kode_status_t dvp_new(void)
{
    const kbsp_cam_map_t *const   m   = &KBSP_CAM_MAPS[s_map];
    esp_cam_ctlr_dvp_pin_config_t pins;
    cam_ctlr_color_t              color = CAM_CTLR_COLOR_RGB565;
    size_t                        bpp   = BYTES_RGB565;
    kode_status_t                 st    = KODE_OK;
    esp_err_t                     err;

    ctlr_del();
    if ( KDRV_OV5640_RAW8 == s_mode.fmt )
    {
        color = CAM_CTLR_COLOR_RAW8;
        bpp   = BYTES_RAW8;
    }
    (void)memset(&pins, 0, sizeof(pins));
    pins.data_width = CAM_CTLR_DATA_WIDTH_8;
    for ( uint32_t i = 0U; KBSP_CAM_DATA_LINES > i; i++ )
    {
        pins.data_io[i] = gpio_of(m->d[i]);
    }
    pins.vsync_io = gpio_of(m->vsync);
    pins.de_io    = gpio_of(m->href);
    pins.pclk_io  = gpio_of(m->pclk);
    pins.xclk_io  = GPIO_NUM_NC; /* the module has its own crystal */

    const esp_cam_ctlr_dvp_config_t dvp = {
        .ctlr_id                = DVP_CTLR_ID,
        .clk_src                = CAM_CLK_SRC_XTAL,
        .h_res                  = s_mode.w,
        .v_res                  = s_mode.h,
        .input_data_color_type  = color,
        .output_data_color_type = color,
        .dma_burst_size         = DVP_BURST_BYTES,
        .bk_buffer_dis          = 1U,
        .external_xtal          = 1U,
        .pin                    = &pins,
    };

    err = esp_cam_new_dvp_ctlr(&dvp, &s_ctlr);
    if ( ESP_OK != err )
    {
        s_ctlr = NULL;
        st     = KODE_ERROR;
        KLOG_W(TAG, "dvp: %s", esp_err_to_name(err));
    }
    else
    {
        const size_t need = (size_t)s_mode.w * s_mode.h * bpp;

        st = bufs_fit(need);
        if ( KODE_OK == st )
        {
            s_buflen   = need;
            s_filling  = 0;
            s_last     = NO_BUF;
            s_cam_held = NO_BUF;
        }
        else
        {
            ctlr_del();
        }
    }
    return st;
}

/** @brief Start the DVP and the sensor's stream. */
static kode_status_t capture_start(void)
{
    kode_status_t st  = KODE_OK;
    esp_err_t     err = ESP_OK;

    if ( !s_cbs )
    {
        const esp_cam_ctlr_evt_cbs_t cbs = { .on_get_new_trans = on_new, .on_trans_finished = on_done };

        s_trans.buffer = s_bufs[0];
        s_trans.buflen = s_buflen;
        err            = esp_cam_ctlr_register_event_callbacks(s_ctlr, &cbs, NULL);
        s_cbs          = (ESP_OK == err);
    }
    if ( ESP_OK == err )
    {
        err = esp_cam_ctlr_enable(s_ctlr);
        if ( ESP_ERR_INVALID_STATE == err )
        {
            err = ESP_OK; /* already enabled */
        }
    }
    if ( ESP_OK == err )
    {
        s_enabled = true;
        err       = esp_cam_ctlr_start(s_ctlr);
        if ( ESP_OK != err )
        {
            (void)esp_cam_ctlr_disable(s_ctlr);
            s_enabled = false;
        }
    }
    if ( ESP_OK == err )
    {
        int32_t on = 1;

        (void)esp_cam_sensor_ioctl(s_sensor, ESP_CAM_SENSOR_IOC_S_STREAM, &on);
        s_capturing = true;
    }
    else
    {
        st = KODE_ERROR;
        KLOG_W(TAG, "dvp start: %s", esp_err_to_name(err));
    }
    return st;
}

/** @brief Stop the sensor's stream, then the DVP. */
static void capture_stop(void)
{
    if ( s_capturing )
    {
        int32_t off = 0;

        if ( (NULL != s_sensor) && (NULL != s_sensor->sccb_handle) )
        {
            (void)esp_cam_sensor_ioctl(s_sensor, ESP_CAM_SENSOR_IOC_S_STREAM, &off);
        }
        khal_delay_ms(STREAM_STOP_MS);
        (void)esp_cam_ctlr_stop(s_ctlr);
        (void)esp_cam_ctlr_disable(s_ctlr);
        s_enabled   = false;
        s_capturing = false;
    }
}

/* ---- Sensor registers: mode, orientation, settings ------------------------------ */

/** @brief The native window @p m of the 2592x1944 array, no scaling. */
static bool apply_window(const kdrv_ov5640_mode_t *m)
{
    bool ok = wr16(OV_REG_X_START, m->x) && wr16(OV_REG_Y_START, m->y)
              && wr16(OV_REG_X_END, (uint16_t)(m->x + m->w + OV_WIN_X_MARGIN))
              && wr16(OV_REG_Y_END, (uint16_t)(m->y + m->h + OV_WIN_Y_MARGIN))
              && wr16(OV_REG_X_OUT, m->w) && wr16(OV_REG_Y_OUT, m->h);

    for ( size_t i = 0U; ok && ((sizeof(OV_WINDOW_TABLE) / sizeof(OV_WINDOW_TABLE[0])) > i); i++ )
    {
        ok = wr(OV_WINDOW_TABLE[i].reg, OV_WINDOW_TABLE[i].val);
    }
    return ok && rmw(OV_REG_ISP1, OV_ISP1_SCALE, 0U);
}

/** @brief RAW8: the ISP's format mux and the output format to RAW. */
static bool apply_raw(void)
{
    return wr(OV_REG_FMT_MUX, OV_FMT_MUX_RAW) && wr(OV_REG_FORMAT, OV_FORMAT_RAW);
}

/** @brief The flips; with the stream stopped only. */
static bool apply_orientation(void)
{
    uint8_t vf = 0U;
    uint8_t hm = 0U;
    bool    ok = false;

    if ( s_cfg.vflip )
    {
        vf = OV_TC_FLIP_BITS;
    }
    if ( s_cfg.mirror )
    {
        hm = OV_TC_FLIP_BITS;
    }
    if ( !s_mode.window )
    {
        ok = rmw(OV_REG_TC20, OV_TC_FLIP_BITS, vf) && rmw(OV_REG_TC21, OV_TC_FLIP_BITS, hm);
    }
    else
    {
        /* Whole bytes: they also clear the binning bits the field's table leaves set. */
        uint8_t blc = OV_4514_NONE;

        if ( s_cfg.vflip )
        {
            blc = OV_4514_VFLIP;
        }
        else if ( s_cfg.mirror )
        {
            blc = OV_4514_MIRROR;
        }
        else
        {
            blc = OV_4514_NONE;
        }
        ok = wr(OV_REG_TC20, (uint8_t)(OV_TC20_NO_VBIN | vf)) && wr(OV_REG_TC21, hm)
             && wr(OV_REG_BLC_4514, blc);
    }
    return ok;
}

/** @brief Write the lens motor's DAC code. */
static bool vcm_write(uint16_t dac)
{
    uint8_t low = 0U;
    bool    ok;

    s_vcm_hw = VCM_UNKNOWN;
    ok = wr(OV_REG_VCM_HIGH, (uint8_t)((dac >> OV_VCM_SHIFT) & OV_VCM_HIGH_MASK))
         && rd(OV_REG_VCM_LOW, &low)
         && wr(OV_REG_VCM_LOW, (uint8_t)(((dac & OV_VCM_LOW_MASK) << OV_VCM_SHIFT) | (low & OV_VCM_LOW_MASK)));
    if ( ok )
    {
        s_vcm_hw = (int32_t)dac;
    }
    return ok;
}

/** @brief The ISP's corrections off; colour interpolation and the colour matrix stay on. */
static bool isp_off(void)
{
    bool ok = rmw(OV_REG_ISP0, (uint8_t)(OV_ISP0_LENC | OV_ISP0_GMA | OV_ISP0_BPC | OV_ISP0_WPC), 0U);

    ok = rmw(OV_REG_ISP1, OV_ISP1_SDE, 0U) && ok;
    ok = rmw(OV_REG_CIP, 0U, OV_CIP_DNS_MANUAL) && ok;
    ok = wr(OV_REG_DNS_OFFSET, 0U) && ok;
    ok = wr(OV_REG_SHARP_MT1, 0U) && ok;
    ok = wr(OV_REG_SHARP_MT2, 0U) && ok;
    ok = rmw(OV_REG_AEC0, OV_AEC0_BANDING, 0U) && ok;
    return ok;
}

/** @brief The manual-control bits s_cfg asks for in OV_REG_AEC_MANUAL. */
static uint8_t aec_manual_bits(void)
{
    uint8_t bits = 0U;

    if ( s_cfg.exp_set )
    {
        bits |= OV_AEC_MANUAL_EXP;
    }
    if ( s_cfg.gain_set )
    {
        bits |= OV_AEC_MANUAL_GAIN;
    }
    return bits;
}

/** @brief Write every setting s_cfg asks for; the lens only when it moved since the last reset. */
static bool cfg_write(void)
{
    const kdrv_ov5640_cfg_t *const c  = &s_cfg;
    bool                           ok = true;

    if ( c->isp_off )
    {
        ok = isp_off() && ok;
    }
    if ( c->awb_set )
    {
        ok = wr(OV_REG_AWB_MANUAL, OV_AWB_MANUAL) && wr16(OV_REG_AWB_R, c->awb[0])
             && wr16(OV_REG_AWB_G, c->awb[1]) && wr16(OV_REG_AWB_B, c->awb[2]) && ok;
    }
    if ( c->exp_set || c->gain_set )
    {
        ok = wr(OV_REG_AEC_MANUAL, aec_manual_bits()) && ok;
    }
    if ( c->exp_set )
    {
        const uint32_t e = c->exp_lines << OV_EXP_FRAC_BITS;

        ok = wr(OV_REG_EXP, (uint8_t)((e >> OV_EXP_TOP_SHIFT) & OV_EXP_TOP_MASK))
             && wr((uint16_t)(OV_REG_EXP + 1U), (uint8_t)(e >> BYTE_BITS))
             && wr((uint16_t)(OV_REG_EXP + 2U), (uint8_t)e) && ok;
    }
    if ( c->gain_set )
    {
        ok = wr(OV_REG_GAIN, (uint8_t)((c->gain >> BYTE_BITS) & OV_GAIN_TOP_MASK))
             && wr((uint16_t)(OV_REG_GAIN + 1U), (uint8_t)c->gain) && ok;
    }
    if ( c->vcm_set && ((int32_t)c->vcm != s_vcm_hw) )
    {
        ok = vcm_write(c->vcm) && ok;
    }
    return ok;
}

/** @brief Whether the sensor still holds s_cfg: a reset puts its defaults back. */
static bool cfg_holds(void)
{
    uint8_t v     = 0U;
    bool    holds = true;

    if ( s_cfg.exp_set || s_cfg.gain_set )
    {
        holds = rd(OV_REG_AEC_MANUAL, &v)
                && (aec_manual_bits() == (uint8_t)(v & (OV_AEC_MANUAL_EXP | OV_AEC_MANUAL_GAIN)));
    }
    if ( holds && s_cfg.awb_set )
    {
        holds = rd(OV_REG_AWB_MANUAL, &v) && (0U != (v & OV_AWB_MANUAL));
    }
    if ( holds && s_cfg.isp_off )
    {
        holds = rd(OV_REG_ISP0, &v) && (0U == (v & (OV_ISP0_LENC | OV_ISP0_BPC | OV_ISP0_WPC)));
    }
    if ( holds && s_cfg.vcm_set )
    {
        holds = rd(OV_REG_VCM_HIGH, &v) && ((uint8_t)((s_cfg.vcm >> OV_VCM_SHIFT) & OV_VCM_HIGH_MASK) == v);
    }
    return holds;
}

/**
 * @brief Program the sensor for s_mode, with the stream stopped.
 *
 * A window that only moves (same format, the sensor still in window mode) is
 * written over the last one: a reset sends the lens motor to rest and back,
 * which is heard. The first mode after an open, a format change and a sensor
 * that lost its settings get the reset.
 */
static kode_status_t sensor_program(void)
{
    const bool    keep = s_prog_win && s_mode.window && (s_prog_fmt == s_mode.fmt);
    kode_status_t st   = KODE_OK;
    bool          ok   = true;

    if ( !keep )
    {
        esp_err_t err;

        s_prog_win = false;
        s_vcm_hw   = VCM_UNKNOWN;
        err        = esp_cam_sensor_set_format(s_sensor, NULL);
        if ( ESP_OK != err )
        {
            KLOG_W(TAG, "set_format: %s", esp_err_to_name(err));
            ok = false;
        }
    }
    if ( ok && s_mode.window )
    {
        ok = apply_window(&s_mode);
    }
    if ( ok && (KDRV_OV5640_RAW8 == s_mode.fmt) )
    {
        ok = apply_raw();
    }
    ok = ok && apply_orientation();
    /* What the caller asked for goes in after the mode and before the stream. */
    ok = ok && cfg_write();
    if ( ok )
    {
        s_prog_win = s_mode.window;
        s_prog_fmt = s_mode.fmt;
    }
    else
    {
        s_prog_win = false;
        st         = KODE_IO_ERROR;
    }
    return st;
}

/** @brief Stop, rebuild the DVP, program the sensor, start: once. */
static kode_status_t pipeline_build(void)
{
    kode_status_t st;

    capture_stop();
    st = dvp_new();
    if ( KODE_OK == st )
    {
        st = sensor_program();
    }
    if ( KODE_OK == st )
    {
        st = capture_start();
    }
    return st;
}

/** @brief pipeline_build() up to OPEN_TRIES times: a rebuild fails now and then. */
static kode_status_t pipeline_build_tries(void)
{
    kode_status_t st = KODE_ERROR;

    for ( uint32_t i = 0U; (OPEN_TRIES > i) && (KODE_OK != st); i++ )
    {
        if ( 0U < i )
        {
            khal_delay_ms(OPEN_RETRY_MS);
        }
        st = pipeline_build();
        if ( KODE_OK != st )
        {
            KLOG_W(TAG, "capture build failed (%d), try %u", (int)st, (unsigned)(i + 1U));
        }
    }
    if ( KODE_OK != st )
    {
        capture_stop();
    }
    return st;
}

/** @brief The sensor on the SCCB, powered: PWDN driven low. */
static kode_status_t sensor_up(void)
{
    const kbsp_cam_map_t *const m    = &KBSP_CAM_MAPS[s_map];
    const gpio_num_t            pwdn = gpio_of(m->pwdn);
    const gpio_config_t         pw   = { .pin_bit_mask = 1ULL << (uint32_t)pwdn, .mode = GPIO_MODE_OUTPUT };
    const sccb_i2c_config_t     icfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = KBSP_CAM_SCCB_ADDR,
        .scl_speed_hz    = KBSP_STATION_I2C_HZ,
    };
    esp_sccb_io_handle_t io = NULL;
    kode_status_t        st = KODE_OK;

    (void)gpio_config(&pw);
    (void)gpio_set_level(pwdn, 0U); /* PWDN is active high */
    if ( ESP_OK != sccb_new_i2c_io(s_bus, &icfg, &io) )
    {
        st = KODE_ERROR;
    }
    else
    {
        esp_cam_sensor_config_t cfg = {
            .sccb_handle = io,
            .reset_pin   = GPIO_NUM_NC,
            .pwdn_pin    = GPIO_NUM_NC,
            .xclk_pin    = GPIO_NUM_NC,
            .sensor_port = ESP_CAM_SENSOR_DVP,
        };

        s_sensor = ov5640_detect(&cfg);
        if ( NULL == s_sensor )
        {
            (void)esp_sccb_del_i2c_io(io);
            st = KODE_IO_ERROR;
            KLOG_W(TAG, "no OV5640 at 0x%02x", (unsigned)KBSP_CAM_SCCB_ADDR);
        }
    }
    return st;
}

/** @brief The sensor off the SCCB and freed. */
static void sensor_down(void)
{
    if ( NULL != s_sensor )
    {
        if ( NULL != s_sensor->sccb_handle )
        {
            (void)esp_sccb_del_i2c_io(s_sensor->sccb_handle);
            s_sensor->sccb_handle = NULL;
        }
        (void)esp_cam_sensor_del_dev(s_sensor);
        s_sensor = NULL;
        (void)gpio_set_direction(gpio_of(KBSP_CAM_MAPS[s_map].pwdn), GPIO_MODE_INPUT);
    }
}

/** @brief Whether @p m is a capture this driver can make. */
static bool mode_ok(const kdrv_ov5640_mode_t *m)
{
    bool ok = true;

    if ( m->window )
    {
        ok = (0U < m->w) && (0U < m->h) && (KDRV_OV5640_SENSOR_W >= ((uint32_t)m->x + m->w))
             && (KDRV_OV5640_SENSOR_H >= ((uint32_t)m->y + m->h));
    }
    return ok && ((KDRV_OV5640_RGB565 == m->fmt) || (KDRV_OV5640_RAW8 == m->fmt));
}

/* ---- The calls ------------------------------------------------------------------ */

kode_status_t kdrv_ov5640_probe(uint32_t budget_ms, uint8_t *map)
{
    kode_status_t st = KODE_DENIED;

    if ( NULL == map )
    {
        st = KODE_DENIED;
    }
    else if ( NULL != s_bus )
    {
        *map = s_map;
        st   = KODE_OK;
    }
    else
    {
        uint8_t side = SIDE_UPRIGHT;

        st = j3_take();
        if ( KODE_OK == st )
        {
            st = find_side(budget_ms, &side);
        }
        if ( KODE_OK == st )
        {
            s_map = map_of_side(side);
            *map  = s_map;
            KLOG_I(TAG, "camera on map %u (I2C%u)", (unsigned)s_map, (unsigned)s_i2c_id);
        }
        else
        {
            bus_down();
            j3_give();
        }
    }
    return st;
}

kode_status_t kdrv_ov5640_open(const kdrv_ov5640_mode_t *mode)
{
    kode_status_t st = KODE_OK;

    if ( (NULL == mode) || (NULL == s_bus) || !mode_ok(mode) )
    {
        st = KODE_DENIED;
    }
    if ( (KODE_OK == st) && (NULL == s_frame) )
    {
        s_frame = khal_sem_create();
        if ( NULL == s_frame )
        {
            st = KODE_NO_RESOURCES;
        }
    }
    if ( (KODE_OK == st) && (NULL == s_sensor) )
    {
        st = sensor_up();
    }
    if ( KODE_OK == st )
    {
        s_mode = *mode;
        if ( !s_mode.window )
        {
            s_mode.x = 0U;
            s_mode.y = 0U;
            s_mode.w = KDRV_OV5640_FIELD_W;
            s_mode.h = KDRV_OV5640_FIELD_H;
        }
        st = pipeline_build_tries();
    }
    return st;
}

void kdrv_ov5640_close(void)
{
    capture_stop();
    ctlr_del();
    sensor_down();
    bufs_free();
    bus_down();
    j3_give();
    khal_sem_delete(s_frame);
    s_frame    = NULL;
    s_prog_win = false;
    s_vcm_hw   = VCM_UNKNOWN;
    s_cam_held = NO_BUF;
    s_last     = NO_BUF;
}

kode_status_t kdrv_ov5640_grab(uint32_t skip, uint32_t timeout_ms, const uint8_t **frame,
                               size_t *len)
{
    kode_status_t st     = KODE_TIMEOUT;
    uint32_t      need   = skip + 1U;
    uint32_t      lost   = 0U;
    bool          wanted = true;

    if ( (NULL == frame) || (NULL == len) || !s_capturing )
    {
        st     = KODE_DENIED;
        wanted = false;
    }
    else
    {
        s_cam_held = NO_BUF;
        while ( khal_sem_wait_timeout(s_frame, 0U) )
        {
            /* Drop the frames that finished before the call. */
        }
    }
    while ( wanted )
    {
        if ( !khal_sem_wait_timeout(s_frame, timeout_ms) )
        {
            wanted = false;
        }
        else if ( 1U < need )
        {
            need--;
        }
        else
        {
            const int32_t mine = s_last;

            s_cam_held = mine;
            if ( (NO_BUF == mine) || (s_filling == mine) )
            {
                /* The interrupt gave it to the DMA meanwhile: take the next one. */
                s_cam_held = NO_BUF;
            }
            else
            {
                (void)esp_cache_msync(s_bufs[mine], align_up(s_buflen), ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                if ( !cfg_holds() && (CFG_LOST_TRIES > lost) )
                {
                    /* The sensor lost its settings: put them back, take a fresh frame. */
                    lost++;
                    s_cam_held = NO_BUF;
                    s_prog_win = false;
                    s_vcm_hw   = VCM_UNKNOWN;
                    KLOG_W(TAG, "sensor settings lost; written again");
                    (void)cfg_write();
                    need = skip + 2U;
                    while ( khal_sem_wait_timeout(s_frame, 0U) )
                    {
                        /* Drop what came under the old settings. */
                    }
                }
                else
                {
                    *frame = (const uint8_t *)s_bufs[mine];
                    *len   = s_buflen;
                    st     = KODE_OK;
                    wanted = false;
                }
            }
        }
    }
    return st;
}

void kdrv_ov5640_release(void)
{
    s_cam_held = NO_BUF;
}

kode_status_t kdrv_ov5640_apply(const kdrv_ov5640_cfg_t *cfg)
{
    kode_status_t st = KODE_OK;

    if ( NULL == cfg )
    {
        st = KODE_DENIED;
    }
    else
    {
        const kdrv_ov5640_cfg_t before = s_cfg;
        /* A correction going back to automatic needs the mode's own registers. */
        const bool back_to_auto = (before.isp_off && !cfg->isp_off) || (before.awb_set && !cfg->awb_set)
                                  || (before.exp_set && !cfg->exp_set)
                                  || (before.gain_set && !cfg->gain_set);
        /* The flips are written with the stream stopped. */
        const bool flips = (before.vflip != cfg->vflip) || (before.mirror != cfg->mirror);

        s_cfg = *cfg;
        if ( !s_capturing )
        {
            st = KODE_OK;
        }
        else if ( back_to_auto || flips )
        {
            if ( back_to_auto )
            {
                s_prog_win = false;
            }
            st = pipeline_build_tries();
        }
        else if ( !cfg_write() )
        {
            st = KODE_IO_ERROR;
        }
        else
        {
            st = KODE_OK;
        }
    }
    return st;
}

void *kdrv_ov5640_bus(void)
{
    return (void *)s_bus;
}
