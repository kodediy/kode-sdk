/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kes_fields.c
 * @brief The codec's field table.
 */
#include "kes_fields.h"

#include <stdio.h>
#include <string.h>

#include "kode_log.h"

#define TAG "kdrv.audio"

/* ---- The chip's registers, by the datasheet's numbers --------------------- */

#define REG_RESET       0x00U  /* reset and the clock state machine          */
#define REG_CLK_01      0x01U  /* clock manager: which clocks are on         */
#define REG_CLK_02      0x02U  /* clock manager: pre-divider and multiplier  */
#define REG_CLK_03      0x03U  /* clock manager: fs mode and ADC oversample  */
#define REG_CLK_04      0x04U  /* clock manager: DAC oversample              */
#define REG_CLK_05      0x05U  /* clock manager: ADC and DAC dividers        */
#define REG_CLK_06      0x06U  /* clock manager: BCLK divider                */
#define REG_CLK_07      0x07U  /* clock manager: LRCK high byte              */
#define REG_CLK_08      0x08U  /* clock manager: LRCK low byte               */
#define REG_SDP_IN      0x09U  /* serial port in: format, word length, mute  */
#define REG_SDP_OUT     0x0AU  /* serial port out: format, word length       */
#define REG_SYS_0D      0x0DU  /* system: analog power up and down           */
#define REG_SYS_0E      0x0EU  /* system: analog power up and down           */
#define REG_SYS_12      0x12U  /* system: enable the DAC                     */
#define REG_SYS_13      0x13U  /* system: the headphone switch is bit 4      */
#define REG_SYS_14      0x14U  /* system: DMIC select (bit 6), analog PGA    */
#define REG_ADC_SCALE   0x16U  /* ADC scale: 0 to 7, six decibels a step     */
#define REG_ADC_VOL     0x17U  /* ADC volume                                 */
#define REG_DAC_MUTE    0x31U  /* DAC mute                                   */
#define REG_DAC_VOL     0x32U  /* DAC volume                                 */
#define REG_DRC         0x34U  /* DAC dynamic range control                  */
#define REG_RAMPRATE    0x37U  /* DAC volume ramp rate                       */
#define REG_DAC2ADC     0x44U  /* the internal loopback                      */
#define REG_GP          0x45U  /* general purpose                            */
#define REG_CHD1        0xFDU  /* chip identity 1                            */
#define REG_CHD2        0xFEU  /* chip identity 2                            */
#define REG_CHVER       0xFFU  /* chip version                               */

/** @brief A field the driver answers from its own state, with no register. */
#define REG_NONE      0xFFFFU

/** @brief One step of that scale. */
#define ES_MIC_DB_STEP      6U

/* What an open applies. Held here rather than in the device because they are
 * what this table sets, and because they survive a close - a close is
 * something the owner does on a timer rather than something a caller asked
 * for, so a beep after an idle close must come back at the volume the last
 * caller set and not at the boot default. */
static uint8_t s_vol    = KDRV_ES8311_VOL_MAX;
static uint8_t s_mic_db = 24U;
static bool    s_out_mute;
static bool    s_in_mute;

void kes_settings(kes_settings_t *out)
{
    if ( NULL == out )
    {
        return;
    }
    out->vol      = s_vol;
    out->mic_db   = s_mic_db;
    out->out_mute = s_out_mute;
    out->in_mute  = s_in_mute;
}

/* ---- The field table ----------------------------------------------------- */

#define F_RW    0x00U
#define F_RO    0x01U   /**< set() refuses it.                              */
#define F_DRV   0x02U   /**< Answered from driver state; no bus access.     */

/** @brief One field: what the outside sees, then where it lives. */
typedef struct {
    kdrv_es8311_field_info_t info;
    uint16_t reg;
    uint8_t  shift;
    uint8_t  width;
    uint8_t  flags;
} row_t;

#define ROW(nm, un, lo, hi, st, r, sh, w, fl) \
    { { (nm), (un), (lo), (hi), (st), (((fl) & F_RO) == 0U) }, (r), (sh), (w), (fl) }
#define BYTE(nm, r)      ROW(nm, "bits", 0, 255, 1U, r, 0U, 8U, F_RW)
#define BYTE_RO(nm, r)   ROW(nm, "bits", 0, 255, 1U, r, 0U, 8U, F_RO)
/* FBIT and not BIT: ESP-IDF defines a BIT(nr) of its own, and
 * i2s_std.h is what drags it in here. */
#define FBIT(nm, r, b, f) ROW(nm, "bool", 0, 1, 1U, r, b, 1U, f)
#define DRV(nm, un, lo, hi, st, f) \
    ROW(nm, un, lo, hi, st, REG_NONE, 0U, 0U, (f) | F_DRV)

/**
 * @brief What the volume registers read, in tenth-decibels.
 *
 * The component's own law: register / 2 - 95.5 decibels, over the full byte.
 * So 0xBF is exactly zero, and anything above it is digital gain the DAC does
 * not have the headroom for. Volume 100 is the speaker's cap
 * (KDRV_ES8311_OUT_DB_X10_MAX, full scale), which the driver's volume curve
 * applies.
 */
#define VOL_DB_X10_MIN   (-955)
#define VOL_DB_X10_MAX     320
#define VOL_DB_X10_STEP      5U

static const row_t FIELDS[KDRV_ES_FIELD_MAX] = {
    [KDRV_ES_PRESENT]      = DRV("present",   "bool", 0, 1, 1U, F_RO),
    [KDRV_ES_OPEN]         = DRV("open",      "bool", 0, 1, 1U, F_RO),
    [KDRV_ES_BUSY]         = DRV("busy",      "bool", 0, 1, 1U, F_RO),
    [KDRV_ES_RATE]         = DRV("rate",      "Hz", (int32_t)KDRV_ES8311_RATE_HZ,
                                 (int32_t)KDRV_ES8311_RATE_HZ, 1U, F_RO),
    [KDRV_ES_BITS]         = DRV("bits",      "bits", (int32_t)KDRV_ES8311_BITS,
                                 (int32_t)KDRV_ES8311_BITS, 1U, F_RO),
    [KDRV_ES_MCLK_MULT]    = DRV("mclk_mult", "x", (int32_t)KDRV_ES8311_MCLK_MULT,
                                 (int32_t)KDRV_ES8311_MCLK_MULT, 1U, F_RO),

    [KDRV_ES_VOL]          = DRV("vol",      "%",  0,
                                 (int32_t)KDRV_ES8311_VOL_MAX, 1U, F_RW),
    [KDRV_ES_MIC_DB]       = DRV("mic_db",   "dB", 0, (int32_t)KDRV_ES8311_MIC_DB_MAX,
                                 ES_MIC_DB_STEP, F_RW),
    [KDRV_ES_OUT_MUTE]     = DRV("out_mute", "bool", 0, 1, 1U, F_RW),
    [KDRV_ES_IN_MUTE]      = DRV("in_mute",  "bool", 0, 1, 1U, F_RW),

    [KDRV_ES_DAC_VOL_DB]   = ROW("dac_vol_db", "x10dB", VOL_DB_X10_MIN,
                                 VOL_DB_X10_MAX, VOL_DB_X10_STEP,
                                 REG_DAC_VOL, 0U, 8U, F_RO),
    [KDRV_ES_ADC_VOL_DB]   = ROW("adc_vol_db", "x10dB", VOL_DB_X10_MIN,
                                 VOL_DB_X10_MAX, VOL_DB_X10_STEP,
                                 REG_ADC_VOL, 0U, 8U, F_RO),
    [KDRV_ES_ADC_SCALE_DB] = ROW("adc_scale_db", "dB", 0, (int32_t)KDRV_ES8311_MIC_DB_MAX,
                                 ES_MIC_DB_STEP, REG_ADC_SCALE, 0U, 3U, F_RW),
    [KDRV_ES_PGA]          = ROW("pga", "bits", 0, 15, 1U, REG_SYS_14, 0U, 4U, F_RW),
    [KDRV_ES_DMIC]         = FBIT("dmic",   REG_SYS_14,  6U, F_RO),
    [KDRV_ES_HPSW]         = FBIT("hpsw",   REG_SYS_13,  4U, F_RW),
    [KDRV_ES_DAC2ADC]      = BYTE("dac2adc", REG_DAC2ADC),
    /* Read only, like 0x32 and 0x34 below: the compressor levels toward
     * -6 dBFS whatever the volume is, and the driver's volume
     * curve is the one path to the output that is allowed to move it. */
    [KDRV_ES_DRC_EN]       = FBIT("drc_en", REG_DRC,     7U, F_RO),
    [KDRV_ES_RAMPRATE]     = ROW("ramprate", "steps", 0, 15, 1U,
                                 REG_RAMPRATE, 4U, 4U, F_RW),

    [KDRV_ES_REG00] = BYTE("reg00", REG_RESET),
    [KDRV_ES_REG01] = BYTE("reg01", REG_CLK_01),
    [KDRV_ES_REG02] = BYTE("reg02", REG_CLK_02),
    [KDRV_ES_REG03] = BYTE("reg03", REG_CLK_03),
    [KDRV_ES_REG04] = BYTE("reg04", REG_CLK_04),
    [KDRV_ES_REG05] = BYTE("reg05", REG_CLK_05),
    [KDRV_ES_REG06] = BYTE("reg06", REG_CLK_06),
    [KDRV_ES_REG07] = BYTE("reg07", REG_CLK_07),
    [KDRV_ES_REG08] = BYTE("reg08", REG_CLK_08),
    [KDRV_ES_REG09] = BYTE("reg09", REG_SDP_IN),
    [KDRV_ES_REG0A] = BYTE("reg0a", REG_SDP_OUT),
    [KDRV_ES_REG0D] = BYTE("reg0d", REG_SYS_0D),
    [KDRV_ES_REG0E] = BYTE("reg0e", REG_SYS_0E),
    [KDRV_ES_REG12] = BYTE("reg12", REG_SYS_12),
    [KDRV_ES_REG13] = BYTE("reg13", REG_SYS_13),
    [KDRV_ES_REG14] = BYTE("reg14", REG_SYS_14),
    [KDRV_ES_REG16] = BYTE("reg16", REG_ADC_SCALE),
    [KDRV_ES_REG17] = BYTE("reg17", REG_ADC_VOL),
    [KDRV_ES_REG31] = BYTE("reg31", REG_DAC_MUTE),
    [KDRV_ES_REG32] = BYTE_RO("reg32", REG_DAC_VOL),  /* the speaker's cap */
    [KDRV_ES_REG34] = BYTE_RO("reg34", REG_DRC),      /* the speaker's cap */
    [KDRV_ES_REG37] = BYTE("reg37", REG_RAMPRATE),
    [KDRV_ES_REG44] = BYTE("reg44", REG_DAC2ADC),
    [KDRV_ES_REG45] = BYTE("reg45", REG_GP),
    [KDRV_ES_CHD1]  = BYTE_RO("chd1",  REG_CHD1),
    [KDRV_ES_CHD2]  = BYTE_RO("chd2",  REG_CHD2),
    [KDRV_ES_CHVER] = BYTE_RO("chver", REG_CHVER),
};

/**
 * @brief The mask a row's bits occupy in its register.
 *
 * @param r The row.
 * @return The mask.
 */
static uint8_t field_mask(const row_t *r)
{
    uint32_t m = ((1U << r->width) - 1U) << r->shift;
    return (uint8_t)m;
}

/* ---- Fields -------------------------------------------------------------- */

/**
 * @brief Turn a register's bits into what the field means.
 *
 * @param f    Which field.
 * @param bits What the register held, already shifted and masked.
 * @return The value the outside sees.
 */
static int32_t decode(kdrv_es8311_field_t f, int32_t bits)
{
    if ( (KDRV_ES_DAC_VOL_DB == f) || (KDRV_ES_ADC_VOL_DB == f) )
    {
        /* The component's own law, in tenth-decibels so it stays an integer:
         * register / 2 - 95.5 dB. 0xBF is exactly zero. */
        return (bits * 5) + VOL_DB_X10_MIN;
    }
    if ( KDRV_ES_ADC_SCALE_DB == f )
    {
        return bits * (int32_t)ES_MIC_DB_STEP;
    }
    return bits;
}

/**
 * @brief Turn what the outside means into a register's bits.
 *
 * @param f     Which field.
 * @param value What was asked for.
 * @return The bits to write.
 */
static int32_t encode(kdrv_es8311_field_t f, int32_t value)
{
    if ( KDRV_ES_ADC_SCALE_DB == f )
    {
        /* Rounded down: the chip has eight steps and there is no honest way
         * to give somebody the 21 dB they asked for. */
        return value / (int32_t)ES_MIC_DB_STEP;
    }
    return value;
}

/**
 * @brief Answer a field the driver keeps rather than reads.
 *
 * @param f     Which field.
 * @param value Out.
 * @return KODE_OK, or KODE_DENIED for a field that is not one of these.
 */
static kode_status_t drv_get(kdrv_es8311_field_t f, int32_t *value)
{
    switch ( f )
    {
        /* The four the device knows about itself are asked for one at a
         * time rather than through one snapshot: a snapshot would mean
         * reaching for all of them to answer any of them. */
        case KDRV_ES_PRESENT:   *value = (int32_t)kdrv_es8311_present(); break;
        case KDRV_ES_OPEN:      *value = (int32_t)kdrv_es8311_is_open(); break;
        case KDRV_ES_RATE:      *value = (int32_t)KDRV_ES8311_RATE_HZ; break;
        case KDRV_ES_BITS:      *value = (int32_t)KDRV_ES8311_BITS; break;
        case KDRV_ES_MCLK_MULT: *value = (int32_t)KDRV_ES8311_MCLK_MULT; break;
        case KDRV_ES_VOL:       *value = (int32_t)s_vol; break;
        case KDRV_ES_MIC_DB:    *value = (int32_t)s_mic_db; break;
        case KDRV_ES_OUT_MUTE:  *value = (int32_t)s_out_mute; break;
        case KDRV_ES_IN_MUTE:   *value = (int32_t)s_in_mute; break;
        default:                return KODE_DENIED;
    }
    return KODE_OK;
}

/**
 * @brief Set a field the driver keeps, and push it at the chip if it is open.
 *
 * Stored either way. A setting made while the device is closed is applied at
 * the next open rather than refused, because the device being closed is the
 * owner's timer rather than anything the caller did.
 *
 * @param f     Which field.
 * @param value What to set it to.
 * @return KODE_OK, or KODE_DENIED for a read-only one.
 */
static kode_status_t drv_set(kdrv_es8311_field_t f, int32_t value)
{
    switch ( f )
    {
        case KDRV_ES_VOL:
            s_vol = (uint8_t)value;
            break;
        case KDRV_ES_MIC_DB:
            /* Rounded down to a step the chip has, and stored rounded, so
             * that reading it back says what the chip will actually do. */
            s_mic_db = (uint8_t)((value / (int32_t)ES_MIC_DB_STEP)
                                 * (int32_t)ES_MIC_DB_STEP);
            break;
        case KDRV_ES_OUT_MUTE:
            s_out_mute = (0 != value);
            break;
        case KDRV_ES_IN_MUTE:
            s_in_mute = (0 != value);
            break;
        default:
            return KODE_DENIED;
    }
    /* Pushed at the chip now when it is open, and at the next open when it
     * is not: a setting made while the device is closed is the owner's timer
     * rather than anything the caller did. */
    kdrv_es8311_apply_settings();
    return KODE_OK;
}

kode_status_t kdrv_es8311_get(kdrv_es8311_field_t f, int32_t *value)
{
    const row_t *r;
    uint8_t      raw  = 0U;
    int32_t      bits;

    if ( (NULL == value) || (KDRV_ES_FIELD_MAX <= f) )
    {
        return KODE_DENIED;
    }
    r = &FIELDS[f];
    if ( (r->flags & F_DRV) != 0U )
    {
        return drv_get(f, value);
    }
    if ( !kdrv_es8311_present() )
    {
        return KODE_DENIED;
    }
    if ( kdrv_es8311_reg_read((uint8_t)r->reg, &raw) != KODE_OK )
    {
        return KODE_IO_ERROR;
    }
    bits = (int32_t)((uint32_t)(raw & field_mask(r)) >> r->shift);
    *value = decode(f, bits);
    return KODE_OK;
}

kode_status_t kdrv_es8311_set(kdrv_es8311_field_t f, int32_t value)
{
    const row_t *r;
    uint8_t      raw  = 0U;
    uint8_t      back = 0U;
    uint8_t      bits;
    uint8_t      next;

    if ( KDRV_ES_FIELD_MAX <= f )
    {
        return KODE_DENIED;
    }
    r = &FIELDS[f];
    if ( (r->flags & F_RO) != 0U )
    {
        return KODE_DENIED;
    }
    if ( (value < r->info.min) || (value > r->info.max) )
    {
        return KODE_DENIED;
    }
    if ( (r->flags & F_DRV) != 0U )
    {
        return drv_set(f, value);
    }
    if ( !kdrv_es8311_present() )
    {
        return KODE_DENIED;
    }

    bits = (uint8_t)(((uint32_t)encode(f, value) << r->shift) & field_mask(r));
    if ( kdrv_es8311_reg_read((uint8_t)r->reg, &raw) != KODE_OK )
    {
        return KODE_IO_ERROR;
    }
    next = (uint8_t)((raw & (uint8_t)~field_mask(r)) | bits);
    if ( kdrv_es8311_reg_write((uint8_t)r->reg, next) != KODE_OK )
    {
        return KODE_IO_ERROR;
    }
    /* Only this field's bits are compared: the rest of the byte belongs to
     * other things, and the chip's own state machine may have moved one of
     * them between the two accesses. */
    if ( kdrv_es8311_reg_read((uint8_t)r->reg, &back) != KODE_OK )
    {
        return KODE_IO_ERROR;
    }
    if ( (uint8_t)(back & field_mask(r)) != bits )
    {
        KLOG_W(TAG, "%s: wrote %ld, chip kept 0x%02X", r->info.name,
               (long)value, (unsigned)back);
        return KODE_IO_ERROR;
    }
    return KODE_OK;
}

const kdrv_es8311_field_info_t *kdrv_es8311_field_info(kdrv_es8311_field_t f)
{
    if ( KDRV_ES_FIELD_MAX <= f )
    {
        return NULL;
    }
    return &FIELDS[f].info;
}

kode_status_t kdrv_es8311_field_by_name(const char *name,
                                        kdrv_es8311_field_t *out)
{
    uint32_t i;

    if ( (NULL == name) || (NULL == out) )
    {
        return KODE_DENIED;
    }
    for ( i = 0U; (uint32_t)KDRV_ES_FIELD_MAX > i; ++i )
    {
        if ( strcmp(FIELDS[i].info.name, name) == 0 )
        {
            *out = (kdrv_es8311_field_t)i;
            return KODE_OK;
        }
    }
    return KODE_DENIED;
}

void kdrv_es8311_dump(void)
{
    char     line[96];
    size_t   n = 0U;
    uint32_t i;

    line[0] = '\0';
    for ( i = 0U; (uint32_t)KDRV_ES_FIELD_MAX > i; ++i )
    {
        int32_t v = 0;
        char    one[32];
        int     w;

        if ( kdrv_es8311_get((kdrv_es8311_field_t)i, &v) != KODE_OK )
        {
            continue;
        }
        w = snprintf(one, sizeof(one), "%s=%ld ", FIELDS[i].info.name, (long)v);
        if ( 0 >= w )
        {
            continue;
        }
        if ( (n + (size_t)w) >= sizeof(line) )
        {
            KLOG_N(TAG, "%s", line);
            line[0] = '\0';
            n = 0U;
        }
        (void)memcpy(&line[n], one, (size_t)w + 1U);
        n += (size_t)w;
    }
    if ( 0U < n )
    {
        KLOG_N(TAG, "%s", line);
    }
}
