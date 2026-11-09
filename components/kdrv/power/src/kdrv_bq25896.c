/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_bq25896.c
 * @brief BQ25896: the register map as a table.
 */
#include "kdrv_bq25896.h"

#include <stdio.h>
#include <string.h>

#include "kbsp.h"
#include "khal_i2c.h"
#include "kode_log.h"

#define TAG "kdrv.bq25896"

#define BQ_PROBE_MS         100U
#define BQ_REG_FAULT        0x0CU
#define BQ_REG_ADC_FIRST    0x0EU   /* REG0E..REG12: one conversion       */
#define BQ_REG_ADC_COUNT    5U
#define BQ_REG_BATFET       0x09U
#define BQ_REG_ID           0x14U
#define BQ_REG_COUNT        0x15U   /* REG00..REG14                       */
#define BQ_PN_BQ25896       0U      /* REG14[5:3]                         */
#define BQ_DUMP_LINE        96U

/* ---- The table ----------------------------------------------------------- */

#define F_RW    0U
#define F_RO    1U      /* status or ADC: set() refuses it                */
#define F_SELF  2U      /* returns to 0 by itself: written, not verified  */

/** @brief One field: what the outside sees, then where it lives. */
typedef struct {
    kdrv_bq25896_field_info_t info;
    uint8_t  reg;
    uint8_t  shift;
    uint8_t  width;
    uint8_t  min_code;
    uint8_t  max_code;
    uint16_t offset;
    uint8_t  flags;
} field_t;

/* min and max are spelled out from the codes here so that field_info() can
 * hand back a pointer into the table rather than into a scratch buffer. */
#define ROW(nm, un, r, sh, w, off, st, minc, maxc, fl)                      \
    { { (nm), (un),                                                          \
        (int32_t)(off) + ((int32_t)(minc) * (int32_t)(st)),                 \
        (int32_t)(off) + ((int32_t)(maxc) * (int32_t)(st)),                 \
        (uint16_t)(st), (((fl) & F_RO) == 0U) },                             \
      (r), (sh), (w), (minc), (maxc), (off), (fl) }
#define BOOL(nm, r, bit, fl)           ROW(nm, "bool", r, bit, 1U, 0U, 1U, 0U, 1U, fl)
#define CODE(nm, r, sh, w, maxc, fl)   ROW(nm, "code", r, sh, w, 0U, 1U, 0U, maxc, fl)
#define SCALED(nm, un, r, sh, w, off, st, maxc, fl) \
                                       ROW(nm, un, r, sh, w, off, st, 0U, maxc, fl)

static const field_t FIELDS[KDRV_BQ_FIELD_MAX] = {
    [KDRV_BQ_EN_HIZ]        = BOOL("en_hiz",        0x00U, 7U, F_RW),
    [KDRV_BQ_EN_ILIM]       = BOOL("en_ilim",       0x00U, 6U, F_RW),
    [KDRV_BQ_IINLIM_MA]     = SCALED("iinlim_ma", "mA", 0x00U, 0U, 6U, 100U, 50U, 0x3FU, F_RW),
    [KDRV_BQ_BHOT]          = CODE("bhot",          0x01U, 6U, 2U, 3U, F_RW),
    [KDRV_BQ_BCOLD]         = CODE("bcold",         0x01U, 5U, 1U, 1U, F_RW),
    [KDRV_BQ_VINDPM_OS_MV]  = SCALED("vindpm_os_mv", "mV", 0x01U, 0U, 5U, 0U, 100U, 0x1FU, F_RW),
    [KDRV_BQ_CONV_START]    = BOOL("conv_start",    0x02U, 7U, F_SELF),
    [KDRV_BQ_CONV_RATE]     = BOOL("conv_rate",     0x02U, 6U, F_RW),
    [KDRV_BQ_BOOST_FREQ]    = BOOL("boost_freq",    0x02U, 5U, F_RW),
    [KDRV_BQ_ICO_EN]        = BOOL("ico_en",        0x02U, 4U, F_RW),
    [KDRV_BQ_FORCE_DPDM]    = BOOL("force_dpdm",    0x02U, 1U, F_SELF),
    [KDRV_BQ_AUTO_DPDM_EN]  = BOOL("auto_dpdm_en",  0x02U, 0U, F_RW),
    [KDRV_BQ_BAT_LOADEN]    = BOOL("bat_loaden",    0x03U, 7U, F_RW),
    [KDRV_BQ_WD_RST]        = BOOL("wd_rst",        0x03U, 6U, F_SELF),
    [KDRV_BQ_OTG_CONFIG]    = BOOL("otg_config",    0x03U, 5U, F_RW),
    [KDRV_BQ_CHG_CONFIG]    = BOOL("chg_config",    0x03U, 4U, F_RW),
    [KDRV_BQ_SYS_MIN_MV]    = SCALED("sys_min_mv", "mV", 0x03U, 1U, 3U, 3000U, 100U, 7U, F_RW),
    [KDRV_BQ_MIN_VBAT_SEL]  = BOOL("min_vbat_sel",  0x03U, 0U, F_RW),
    [KDRV_BQ_EN_PUMPX]      = BOOL("en_pumpx",      0x04U, 7U, F_RW),
    [KDRV_BQ_ICHG_MA]       = SCALED("ichg_ma", "mA", 0x04U, 0U, 7U, 0U, 64U, 0x2FU, F_RW),
    [KDRV_BQ_IPRECHG_MA]    = SCALED("iprechg_ma", "mA", 0x05U, 4U, 4U, 64U, 64U, 0x0FU, F_RW),
    [KDRV_BQ_ITERM_MA]      = SCALED("iterm_ma", "mA", 0x05U, 0U, 4U, 64U, 64U, 0x0FU, F_RW),
    [KDRV_BQ_VREG_MV]       = SCALED("vreg_mv", "mV", 0x06U, 2U, 6U, 3840U, 16U, 0x30U, F_RW),
    [KDRV_BQ_BATLOWV]       = BOOL("batlowv",       0x06U, 1U, F_RW),
    [KDRV_BQ_VRECHG]        = BOOL("vrechg",        0x06U, 0U, F_RW),
    [KDRV_BQ_EN_TERM]       = BOOL("en_term",       0x07U, 7U, F_RW),
    [KDRV_BQ_STAT_DIS]      = BOOL("stat_dis",      0x07U, 6U, F_RW),
    [KDRV_BQ_WATCHDOG]      = CODE("watchdog",      0x07U, 4U, 2U, 3U, F_RW),
    [KDRV_BQ_EN_TIMER]      = BOOL("en_timer",      0x07U, 3U, F_RW),
    [KDRV_BQ_CHG_TIMER]     = CODE("chg_timer",     0x07U, 1U, 2U, 3U, F_RW),
    [KDRV_BQ_JEITA_ISET]    = BOOL("jeita_iset",    0x07U, 0U, F_RW),
    [KDRV_BQ_BAT_COMP_MOHM] = SCALED("bat_comp_mohm", "mohm", 0x08U, 5U, 3U, 0U, 20U, 7U, F_RW),
    [KDRV_BQ_VCLAMP_MV]     = SCALED("vclamp_mv", "mV", 0x08U, 2U, 3U, 0U, 32U, 7U, F_RW),
    [KDRV_BQ_TREG_C]        = SCALED("treg_c", "C", 0x08U, 0U, 2U, 60U, 20U, 3U, F_RW),
    [KDRV_BQ_FORCE_ICO]     = BOOL("force_ico",     0x09U, 7U, F_SELF),
    [KDRV_BQ_TMR2X_EN]      = BOOL("tmr2x_en",      0x09U, 6U, F_RW),
    [KDRV_BQ_BATFET_DIS]    = BOOL("batfet_dis",    0x09U, 5U, F_RW),
    [KDRV_BQ_JEITA_VSET]    = BOOL("jeita_vset",    0x09U, 4U, F_RW),
    [KDRV_BQ_BATFET_DLY]    = BOOL("batfet_dly",    0x09U, 3U, F_RW),
    [KDRV_BQ_BATFET_RST_EN] = BOOL("batfet_rst_en", 0x09U, 2U, F_RW),
    [KDRV_BQ_PUMPX_UP]      = BOOL("pumpx_up",      0x09U, 1U, F_SELF),
    [KDRV_BQ_PUMPX_DN]      = BOOL("pumpx_dn",      0x09U, 0U, F_SELF),
    [KDRV_BQ_BOOSTV_MV]     = SCALED("boostv_mv", "mV", 0x0AU, 4U, 4U, 4550U, 64U, 0x0FU, F_RW),
    [KDRV_BQ_PFM_OTG_DIS]   = BOOL("pfm_otg_dis",   0x0AU, 3U, F_RW),
    [KDRV_BQ_BOOST_LIM]     = CODE("boost_lim",     0x0AU, 0U, 3U, 6U, F_RW),
    [KDRV_BQ_VBUS_STAT]     = CODE("vbus_stat",     0x0BU, 5U, 3U, 7U, F_RO),
    [KDRV_BQ_CHRG_STAT]     = CODE("chrg_stat",     0x0BU, 3U, 2U, 3U, F_RO),
    [KDRV_BQ_PG_STAT]       = BOOL("pg_stat",       0x0BU, 2U, F_RO),
    [KDRV_BQ_VSYS_STAT]     = BOOL("vsys_stat",     0x0BU, 0U, F_RO),
    [KDRV_BQ_FORCE_VINDPM]  = BOOL("force_vindpm",  0x0DU, 7U, F_RW),
    [KDRV_BQ_VINDPM_MV]     = ROW("vindpm_mv", "mV", 0x0DU, 0U, 7U, 2600U, 100U, 13U, 0x7FU, F_RW),
    [KDRV_BQ_THERM_STAT]    = BOOL("therm_stat",    0x0EU, 7U, F_RO),
    [KDRV_BQ_BATV_MV]       = SCALED("batv_mv", "mV", 0x0EU, 0U, 7U, 2304U, 20U, 0x7FU, F_RO),
    [KDRV_BQ_SYSV_MV]       = SCALED("sysv_mv", "mV", 0x0FU, 0U, 7U, 2304U, 20U, 0x7FU, F_RO),
    [KDRV_BQ_TS_PCT_X1000]  = SCALED("ts_pct_x1000", "0.001%", 0x10U, 0U, 7U, 21000U, 465U, 0x7FU, F_RO),
    [KDRV_BQ_VBUS_GD]       = BOOL("vbus_gd",       0x11U, 7U, F_RO),
    [KDRV_BQ_VBUSV_MV]      = SCALED("vbusv_mv", "mV", 0x11U, 0U, 7U, 2600U, 100U, 0x7FU, F_RO),
    [KDRV_BQ_ICHGR_MA]      = SCALED("ichgr_ma", "mA", 0x12U, 0U, 7U, 0U, 50U, 0x7FU, F_RO),
    [KDRV_BQ_VDPM_STAT]     = BOOL("vdpm_stat",     0x13U, 7U, F_RO),
    [KDRV_BQ_IDPM_STAT]     = BOOL("idpm_stat",     0x13U, 6U, F_RO),
    [KDRV_BQ_IDPM_LIM_MA]   = SCALED("idpm_lim_ma", "mA", 0x13U, 0U, 6U, 100U, 50U, 0x3FU, F_RO),
    [KDRV_BQ_REG_RST]       = BOOL("reg_rst",       0x14U, 7U, F_SELF),
    [KDRV_BQ_ICO_OPTIMIZED] = BOOL("ico_optimized", 0x14U, 6U, F_RO),
    [KDRV_BQ_PN]            = CODE("pn",            0x14U, 3U, 3U, 7U, F_RO),
    [KDRV_BQ_TS_PROFILE]    = BOOL("ts_profile",    0x14U, 2U, F_RO),
    [KDRV_BQ_DEV_REV]       = CODE("dev_rev",       0x14U, 0U, 2U, 3U, F_RO),
};

static khal_i2c_dev_t s_dev;
static bool           s_bus_ok = true;   /* edge state for the log lines */

/* ---- Register access ----------------------------------------------------- */

/**
 * @brief Say once when the bus stops answering, and once when it returns.
 *
 * Every poll comes through here. A line per failed read would bury the
 * console this is trying to warn.
 */
static void note_bus(bool ok)
{
    if ( ok != s_bus_ok )
    {
        s_bus_ok = ok;
        if ( ok )
        {
            KLOG_N(TAG, "PMIC answering again");
        }
        else
        {
            KLOG_E(TAG, "PMIC stopped answering; readings withheld rather "
                        "than reported as a flat battery");
        }
    }
}

/** @brief Read @p len registers from @p reg. */
static bool reg_read(uint8_t reg, uint8_t *buf, size_t len)
{
    bool const ok = (NULL != s_dev)
                 && (khal_i2c_read_reg(s_dev, reg, buf, len) == KODE_OK);
    note_bus(ok);
    return ok;
}

/** @brief Write one register. */
static bool reg_write(uint8_t reg, uint8_t val)
{
    bool const ok = (NULL != s_dev)
                 && (khal_i2c_write_reg(s_dev, reg, val) == KODE_OK);
    note_bus(ok);
    return ok;
}

/** @brief The mask of a field's bits, in place. */
static uint8_t field_mask(const field_t *f)
{
    uint8_t const ones = (uint8_t)((1U << f->width) - 1U);
    return (uint8_t)(ones << f->shift);
}

/** @brief The code a raw register byte holds for @p f. */
static uint8_t field_code(const field_t *f, uint8_t raw)
{
    return (uint8_t)((raw & field_mask(f)) >> f->shift);
}

/** @brief The value, in its unit, a raw register byte holds for @p f. */
static int32_t field_value(const field_t *f, uint8_t raw)
{
    return (int32_t)f->offset
         + ((int32_t)field_code(f, raw) * (int32_t)f->info.step);
}

/** @brief REG0C, taken apart. */
static void faults_decode(uint8_t raw, kdrv_bq25896_faults_t *f)
{
    f->watchdog = ((raw & 0x80U) != 0U);
    f->boost    = ((raw & 0x40U) != 0U);
    f->chrg     = (uint8_t)((raw >> 4) & 0x03U);
    f->bat_ovp  = ((raw & 0x08U) != 0U);
    f->ntc      = (uint8_t)(raw & 0x07U);
    f->raw      = raw;
}

/* ---- Bring-up ------------------------------------------------------------ */

/**
 * @brief Hand the device back after a bring-up that failed past add().
 *
 * `s_dev != NULL` is the "already up" guard, so forgetting the handle is
 * what lets the next call try again - and removing it first is what keeps
 * that retry from leaving a second device at the same address.
 */
static void release(void)
{
    (void)khal_i2c_remove(s_dev);
    s_dev = NULL;
}

kode_status_t kdrv_bq25896_init(void)
{
    uint8_t id = 0U;

    if ( NULL != s_dev )
    {
        return KODE_OK;
    }
    if ( khal_i2c_init() != KODE_OK )
    {
        return KODE_ERROR;
    }
    if ( !khal_i2c_probe(KBSP_I2C_ADDR_CHARGER, BQ_PROBE_MS) )
    {
        KLOG_E(TAG, "no ACK at 0x%02X: no PMIC", (unsigned)KBSP_I2C_ADDR_CHARGER);
        return KODE_ERROR;
    }
    if ( khal_i2c_add(KBSP_I2C_ADDR_CHARGER, &s_dev) != KODE_OK )
    {
        s_dev = NULL;
        return KODE_ERROR;
    }
    if ( !reg_read(BQ_REG_ID, &id, 1U) )
    {
        release();
        return KODE_ERROR;
    }
    if ( field_code(&FIELDS[KDRV_BQ_PN], id) != BQ_PN_BQ25896 )
    {
        KLOG_E(TAG, "PN=%u at 0x%02X is not a bq25896; refusing to drive it",
               (unsigned)field_code(&FIELDS[KDRV_BQ_PN], id),
               (unsigned)KBSP_I2C_ADDR_CHARGER);
        release();
        return KODE_ERROR;
    }
    KLOG_D(TAG, "BQ25896 at 0x%02X, silicon rev %u; nothing written",
           (unsigned)KBSP_I2C_ADDR_CHARGER,
           (unsigned)field_code(&FIELDS[KDRV_BQ_DEV_REV], id));
    return KODE_OK;
}

bool kdrv_bq25896_present(void)
{
    return NULL != s_dev;
}

/* ---- Fields -------------------------------------------------------------- */

kode_status_t kdrv_bq25896_get(kdrv_bq25896_field_t f, int32_t *value)
{
    uint8_t raw = 0U;

    if ( (NULL == value) || (KDRV_BQ_FIELD_MAX <= f) || (NULL == s_dev) )
    {
        return KODE_DENIED;
    }
    if ( !reg_read(FIELDS[f].reg, &raw, 1U) )
    {
        return KODE_ERROR;
    }
    *value = field_value(&FIELDS[f], raw);
    return KODE_OK;
}

kode_status_t kdrv_bq25896_set(kdrv_bq25896_field_t f, int32_t value)
{
    uint8_t raw  = 0U;
    uint8_t back = 0U;

    if ( (KDRV_BQ_FIELD_MAX <= f) || (NULL == s_dev) )
    {
        return KODE_DENIED;
    }
    const field_t * const row = &FIELDS[f];
    if ( (row->flags & F_RO) != 0U )
    {
        return KODE_DENIED;
    }
    if ( (value < row->info.min) || (value > row->info.max) )
    {
        return KODE_DENIED;
    }

    /* The nearest code. min and max are multiples of the step away from the
     * offset, so rounding can neither leave the range nor need a clamp. */
    int32_t const step = (int32_t)row->info.step;
    int32_t const rel  = value - (int32_t)row->offset;
    uint8_t const code = (uint8_t)((rel + (step / 2)) / step);

    if ( !reg_read(row->reg, &raw, 1U) )
    {
        return KODE_ERROR;
    }
    uint8_t const bits = (uint8_t)(code << row->shift);
    uint8_t const next = (uint8_t)((raw & (uint8_t)~field_mask(row)) | bits);
    if ( !reg_write(row->reg, next) )
    {
        return KODE_ERROR;
    }
    if ( (row->flags & F_SELF) != 0U )
    {
        return KODE_OK;      /* it is already on its way back to 0 */
    }
    /* Only this field's bits are compared: a self-clearing neighbour in the
     * same register may legitimately have moved between the two reads. */
    if ( !reg_read(row->reg, &back, 1U) )
    {
        return KODE_ERROR;
    }
    if ( (back & field_mask(row)) != bits )
    {
        KLOG_W(TAG, "%s: wrote %ld, chip kept 0x%02X", row->info.name,
               (long)value, (unsigned)back);
        return KODE_ERROR;
    }
    return KODE_OK;
}

const kdrv_bq25896_field_info_t *kdrv_bq25896_field_info(kdrv_bq25896_field_t f)
{
    if ( KDRV_BQ_FIELD_MAX <= f )
    {
        return NULL;
    }
    return &FIELDS[f].info;
}

kode_status_t kdrv_bq25896_field_by_name(const char *name,
                                         kdrv_bq25896_field_t *out)
{
    if ( (NULL == name) || (NULL == out) )
    {
        return KODE_DENIED;
    }
    for ( uint32_t i = 0U; (uint32_t)KDRV_BQ_FIELD_MAX > i; ++i )
    {
        if ( strcmp(FIELDS[i].info.name, name) == 0 )
        {
            *out = (kdrv_bq25896_field_t)i;
            return KODE_OK;
        }
    }
    return KODE_DENIED;
}

/* ---- What the table cannot say ------------------------------------------- */

kode_status_t kdrv_bq25896_adc_read(kdrv_bq25896_adc_t *out)
{
    uint8_t b[BQ_REG_ADC_COUNT];

    if ( (NULL == out) || (NULL == s_dev) )
    {
        return KODE_DENIED;
    }
    if ( !reg_read(BQ_REG_ADC_FIRST, b, BQ_REG_ADC_COUNT) )
    {
        return KODE_ERROR;
    }
    out->therm_stat   = (field_code(&FIELDS[KDRV_BQ_THERM_STAT], b[0]) != 0U);
    out->vbat_mv      = (uint16_t)field_value(&FIELDS[KDRV_BQ_BATV_MV], b[0]);
    out->vsys_mv      = (uint16_t)field_value(&FIELDS[KDRV_BQ_SYSV_MV], b[1]);
    out->ts_pct_x1000 = (uint16_t)field_value(&FIELDS[KDRV_BQ_TS_PCT_X1000], b[2]);
    out->vbus_gd      = (field_code(&FIELDS[KDRV_BQ_VBUS_GD], b[3]) != 0U);
    out->vbus_mv      = (uint16_t)field_value(&FIELDS[KDRV_BQ_VBUSV_MV], b[3]);
    out->ichg_ma      = (uint16_t)field_value(&FIELDS[KDRV_BQ_ICHGR_MA], b[4]);
    return KODE_OK;
}

kode_status_t kdrv_bq25896_faults(kdrv_bq25896_faults_t *latched,
                                  kdrv_bq25896_faults_t *live)
{
    uint8_t raw = 0U;

    if ( ((NULL == latched) && (NULL == live)) || (NULL == s_dev) )
    {
        return KODE_DENIED;
    }
    /* The first read is the history whether or not anyone wants it: it is
     * what clears the latch, and only the read after it is the present. */
    if ( !reg_read(BQ_REG_FAULT, &raw, 1U) )
    {
        return KODE_ERROR;
    }
    if ( NULL != latched )
    {
        faults_decode(raw, latched);
    }
    if ( NULL != live )
    {
        if ( !reg_read(BQ_REG_FAULT, &raw, 1U) )
        {
            return KODE_ERROR;
        }
        faults_decode(raw, live);
    }
    return KODE_OK;
}

kode_status_t kdrv_bq25896_ship_mode(bool delayed)
{
    uint8_t raw = 0U;

    if ( NULL == s_dev )
    {
        return KODE_DENIED;
    }
    /* A read first, because the register also holds BATFET_RST_EN and the
     * JEITA voltage choice, and the board must not be switched off on a
     * guess about them. */
    if ( !reg_read(BQ_REG_BATFET, &raw, 1U) )
    {
        return KODE_ERROR;
    }
    uint8_t next = (uint8_t)(raw | field_mask(&FIELDS[KDRV_BQ_BATFET_DIS]));
    if ( delayed )
    {
        next = (uint8_t)(next | field_mask(&FIELDS[KDRV_BQ_BATFET_DLY]));
    }
    else
    {
        next = (uint8_t)(next & (uint8_t)~field_mask(&FIELDS[KDRV_BQ_BATFET_DLY]));
    }
    if ( !reg_write(BQ_REG_BATFET, next) )
    {
        return KODE_ERROR;
    }
    /* On battery this line is never reached: VSYS is already gone. */
    return KODE_OK;
}

/** @brief Append @p text to @p line, flushing it as a log line when full. */
static void dump_append(char *line, size_t *n, const char *text)
{
    size_t const len = strlen(text);

    if ( (*n + len) >= BQ_DUMP_LINE )
    {
        KLOG_N(TAG, "%s", line);
        *n = 0U;
        line[0] = '\0';
    }
    (void)memcpy(&line[*n], text, len + 1U);
    *n += len;
}

void kdrv_bq25896_dump(void)
{
    uint8_t regs[BQ_REG_COUNT];
    uint8_t live = 0U;
    char    line[BQ_DUMP_LINE];
    char    item[32];
    size_t  n = 0U;

    if ( NULL == s_dev )
    {
        KLOG_N(TAG, "no PMIC; nothing to dump");
        return;
    }
    /* Three transactions around REG0C, which takes no part in a burst, and
     * REG0C itself twice: the first read is the history, the second the
     * present. */
    bool ok = reg_read(0x00U, &regs[0], (size_t)BQ_REG_FAULT);
    ok = ok && reg_read(BQ_REG_FAULT, &regs[BQ_REG_FAULT], 1U);
    ok = ok && reg_read(BQ_REG_FAULT, &live, 1U);
    ok = ok && reg_read((uint8_t)(BQ_REG_FAULT + 1U), &regs[BQ_REG_FAULT + 1U],
                        (size_t)(BQ_REG_COUNT - BQ_REG_FAULT - 1U));
    if ( !ok )
    {
        KLOG_E(TAG, "dump: the PMIC did not answer");
        return;
    }

    line[0] = '\0';
    for ( uint32_t r = 0U; BQ_REG_COUNT > r; ++r )
    {
        (void)snprintf(item, sizeof(item), "REG%02X=0x%02X ", (unsigned)r,
                       (unsigned)regs[r]);
        dump_append(line, &n, item);
    }
    KLOG_N(TAG, "%s", line);
    KLOG_N(TAG, "REG0C=0x%02X since the last read, 0x%02X now",
           (unsigned)regs[BQ_REG_FAULT], (unsigned)live);

    n = 0U;
    line[0] = '\0';
    for ( uint32_t f = 0U; (uint32_t)KDRV_BQ_FIELD_MAX > f; ++f )
    {
        const field_t * const row = &FIELDS[f];
        (void)snprintf(item, sizeof(item), "%s=%ld ", row->info.name,
                       (long)field_value(row, regs[row->reg]));
        dump_append(line, &n, item);
    }
    KLOG_N(TAG, "%s", line);
}
