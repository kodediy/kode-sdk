/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_clk_esp.c
 * @brief khal_clk on ESP-IDF: the RTC clock mux and the calibration counter.
 */
#include "khal_clk.h"

#include "esp_clk_tree.h"
#include "esp_err.h"
#include "esp_idf_version.h"
#include "soc/clk_tree_defs.h"
#include "soc/rtc.h"

/* v6 renamed the counter's selector; v5.5 (the Arduino core's) gets the new names. */
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
typedef rtc_cal_sel_t soc_clk_freq_calculation_src_t;
#define CLK_CAL_32K_XTAL  RTC_CAL_32K_XTAL
#define CLK_CAL_RC_SLOW   RTC_CAL_RC_SLOW
#endif

#define HZ_PER_MHZ  1000000U  /**< Hertz in a megahertz. */
/** Over rtc_clk_cal()'s period (us, Q13.19), the frequency in milli-hertz. */
#define CLK_MILLIHZ_DIVIDEND  (1000000000ULL << RTC_CLK_CAL_FRACT)

/**
 * @brief Map a khal source onto the calibration counter's selector.
 *
 * @param[in]  src Source to map.
 * @param[out] sel The selector, untouched when the source is not known.
 * @return true when @p src is a source this port can count.
 */
static bool cal_sel(khal_clk_src_t src, soc_clk_freq_calculation_src_t *sel)
{
    bool known = true;

    switch ( src )
    {
    case KHAL_CLK_SRC_XTAL32K:
        *sel = CLK_CAL_32K_XTAL;
        break;
    case KHAL_CLK_SRC_RC_SLOW:
        *sel = CLK_CAL_RC_SLOW;
        break;
    default:
        known = false;
        break;
    }

    return known;
}

/**
 * @brief Map a khal derived clock onto the clock tree's module identifier.
 *
 * @param[in]  clk Clock to map.
 * @param[out] mod The identifier, untouched when the clock is not known.
 * @return true when @p clk is a clock this port can read.
 */
static bool tree_mod(khal_clk_tree_t clk, soc_module_clk_t *mod)
{
    bool known = true;

    switch ( clk )
    {
    case KHAL_CLK_TREE_CPU:
        *mod = SOC_MOD_CLK_CPU;
        break;
    case KHAL_CLK_TREE_APB:
        *mod = SOC_MOD_CLK_APB;
        break;
    case KHAL_CLK_TREE_RTC_SLOW:
        *mod = SOC_MOD_CLK_RTC_SLOW;
        break;
    case KHAL_CLK_TREE_RTC_FAST:
        *mod = SOC_MOD_CLK_RTC_FAST;
        break;
    default:
        known = false;
        break;
    }

    return known;
}

uint32_t khal_clk_main_hz(void)
{
    return (uint32_t)rtc_clk_xtal_freq_get() * HZ_PER_MHZ;
}

khal_clk_slow_t khal_clk_slow_src(void)
{
    khal_clk_slow_t out;

    switch ( rtc_clk_slow_src_get() )
    {
    case SOC_RTC_SLOW_CLK_SRC_RC_SLOW:
        out = KHAL_CLK_SLOW_RC;
        break;
    case SOC_RTC_SLOW_CLK_SRC_XTAL32K:
        out = KHAL_CLK_SLOW_XTAL32K;
        break;
    case SOC_RTC_SLOW_CLK_SRC_RC32K:
        out = KHAL_CLK_SLOW_RC32K;
        break;
    default:
        out = KHAL_CLK_SLOW_UNKNOWN;
        break;
    }

    return out;
}

kode_status_t khal_clk_measure(khal_clk_src_t src, uint32_t cycles,
                               uint32_t *milli_hz)
{
    soc_clk_freq_calculation_src_t sel = CLK_CAL_RC_SLOW;

    if ( (NULL == milli_hz) || (0U == cycles) || !cal_sel(src, &sel) )
    {
        return KODE_DENIED;
    }

    /* Zero is the counter timing out: no edges, an absent or stopped source. */
    uint32_t const cal = rtc_clk_cal(sel, cycles);
    if ( 0U == cal )
    {
        return KODE_TIMEOUT;
    }

    *milli_hz = (uint32_t)(CLK_MILLIHZ_DIVIDEND / (uint64_t)cal);

    return KODE_OK;
}

kode_status_t khal_clk_tree_hz(khal_clk_tree_t clk, uint32_t *hz)
{
    soc_module_clk_t mod = SOC_MOD_CLK_CPU;

    if ( (NULL == hz) || !tree_mod(clk, &mod) )
    {
        return KODE_DENIED;
    }

    uint32_t freq = 0U;
    if ( esp_clk_tree_src_get_freq_hz(mod, ESP_CLK_TREE_SRC_FREQ_PRECISION_EXACT,
                                     &freq) != ESP_OK )
    {
        return KODE_ERROR;
    }

    *hz = freq;

    return KODE_OK;
}
