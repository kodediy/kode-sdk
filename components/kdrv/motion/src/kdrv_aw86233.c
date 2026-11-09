/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_aw86233.c
 * @brief The AW86233's register map, written by hand, and the RTP stream over it.
 */
#include "kdrv_aw86233.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "kbsp.h"
#include "khal_i2c.h"
#include "khal_time.h"
#include "kode_log.h"

#define TAG "kdrv.haptic"

/* ---- The register map ---------------------------------------------------- */

#define AW_REG_RSTCFG    0x00U   /* W 0xAA: soft reset.                        */
#define AW_REG_SYSST     0x01U   /* bit 3 FF_AFS: the RTP FIFO is almost full. */
#define AW_REG_PLAYCFG2  0x07U   /* The gain, 0 to 255.                        */
#define AW_REG_PLAYCFG3  0x08U   /* bits [1:0]: the play mode.                 */
#define AW_REG_PLAYCFG4  0x09U   /* bit 0 GO, bit 1 STOP.                      */
#define AW_REG_RTPDATA   0x32U   /* The RTP FIFO. Does not auto-increment.     */
#define AW_REG_GLBRD5    0x3FU   /* bits [3:0] GLB_STATE: what the chip is doing. */
#define AW_REG_SYSCTRL2  0x46U   /* bit 6 STANDBY, bits [1:0] the rate.        */
#define AW_REG_CHIPID_H  0x57U   /* 0x23: the family.                          */
#define AW_REG_CHIPID_L  0x69U   /* 0x30: this member of it.                   */

#define AW_SRST          0xAAU   /* Written to AW_REG_RSTCFG.                  */
#define AW_PLAYMODE_RTP  0x01U
#define AW_GO            0x01U
#define AW_STOP          0x02U
#define AW_FF_AFS        0x08U
#define AW_SYSCTRL2_RUN  0x02U   /* Out of standby, WAVDAT_MODE 10: 12 kHz.
                                  * Skip it and the chip acks everything and
                                  * never drives; leave the rate at its reset
                                  * value and it plays at 24 kHz, an octave up. */
#define AW_STANDBY_BIT   0x40U   /* SYSCTRL2 bit 6: the driver stage parked.  */
#define AW_GLB_STATE_MASK 0x0FU
#define AW_GLB_RTP       0x08U   /* GLB_STATE: playing from the RTP FIFO.     */
#define AW_FAMILY        0x23U   /* CHIPID_H on every AW8623X.                */

#define AW_PROBE_MS      50U
#define AW_RESET_MS      3U
#define AW_GO_READS      16U     /* Status reads GO is given to show in
                                  * GLB_STATE: about a millisecond of bus,
                                  * the datasheet's own wait.                */
#define AW_FULL_SCALE    255U    /* What PLAYCFG2 holds at 100 per cent.       */
#define AW_PCT_FULL      100U
#define AW_INT8_FULL     127.0f  /* Full scale of one signed sample.           */
#define AW_TWO_PI_F      6.2831853f /* One turn. Single precision: the P4 has
                                     * no double unit and this is a waveform. */
#define AW_DUMP_LINE     96U

/* ---- The table ----------------------------------------------------------- */

/** @brief Where a field's value comes from. */
typedef enum {
    KIND_DRIVER = 0,   /**< The driver's own cache; no bus at all. */
    KIND_REG           /**< Bits of one register.                  */
} kind_t;

/** @brief One field: what the outside sees, then where it lives. */
typedef struct {
    kdrv_aw86233_field_info_t info;
    uint8_t reg;      /**< The register, for KIND_REG. */
    uint8_t shift;
    uint8_t width;
    uint8_t kind;
} field_t;

/* Every row spells its own range out so that field_info() can hand back a
 * pointer straight into the table. */
#define ROW(nm, un, r, sh, w, wr, kd) \
    { { (nm), (un), 0, (int32_t)((1UL << (w)) - 1UL), 1U, (wr) }, \
      (r), (sh), (w), (uint8_t)(kd) }
#define BYTE(nm, un, r, wr)   ROW(nm, un, r, 0U, 8U, wr, KIND_REG)
#define BIT(nm, r, bit, wr)   ROW(nm, "bool", r, bit, 1U, wr, KIND_REG)

static const field_t FIELDS[KDRV_AW_FIELD_MAX] = {
    [KDRV_AW_PRESENT]      = ROW("present", "bool", 0x00U, 0U, 1U, false, KIND_DRIVER),
    [KDRV_AW_CHIPID_FIELD] = ROW("chipid", "code", 0x00U, 0U, 16U, false, KIND_DRIVER),
    [KDRV_AW_SYSST]        = BYTE("sysst", "bits", AW_REG_SYSST, false),
    [KDRV_AW_FF_AFS]       = BIT("ff_afs", AW_REG_SYSST, 3U, false),
    [KDRV_AW_GAIN]         = BYTE("gain", "code", AW_REG_PLAYCFG2, true),
    [KDRV_AW_PLAY_MODE]    = ROW("play_mode", "code", AW_REG_PLAYCFG3, 0U, 2U, true, KIND_REG),
    [KDRV_AW_GLB_STATE]    = ROW("glb_state", "code", AW_REG_GLBRD5, 0U, 4U, false, KIND_REG),
    [KDRV_AW_SYSCTRL2]     = BYTE("sysctrl2", "bits", AW_REG_SYSCTRL2, true),
    [KDRV_AW_STANDBY]      = BIT("standby", AW_REG_SYSCTRL2, 6U, true),
};

/* ---- State --------------------------------------------------------------- */

static khal_i2c_dev_t s_dev;       /**< The device on the shared bus.        */
static bool           s_added;     /**< khal_i2c_add() has succeeded.        */
static bool           s_present;   /**< A chip answered and came up.         */
static uint16_t       s_chipid;    /**< CHIPID_H:CHIPID_L, read at bring-up. */

/* ---- Register access ----------------------------------------------------- */

/**
 * @brief Read one register.
 * @return true when the bus answered.
 */
static bool reg_read(uint8_t reg, uint8_t *out)
{
    return khal_i2c_read_reg(s_dev, reg, out, 1U) == KODE_OK;
}

/**
 * @brief Write one register.
 * @return true when the bus took it.
 */
static bool reg_write(uint8_t reg, uint8_t val)
{
    return khal_i2c_write_reg(s_dev, reg, val) == KODE_OK;
}

/** @brief The bits @p row occupies in its register. */
static uint8_t field_mask(const field_t *row)
{
    return (uint8_t)(((1U << row->width) - 1U) << row->shift);
}

/* ---- Bring-up ------------------------------------------------------------ */

kode_status_t kdrv_aw86233_park(void)
{
    /* One quiet probe first: a part that is not fitted is a fact here, said
     * once by the caller, and no warning of the bring-up's. */
    if ( (khal_i2c_init() != KODE_OK) || !khal_i2c_probe(KBSP_I2C_ADDR_HAPTIC, AW_PROBE_MS) )
    {
        return KODE_ERROR;
    }
    return kdrv_aw86233_create();
}

kode_status_t kdrv_aw86233_create(void)
{
    uint8_t hi = 0U;
    uint8_t lo = 0U;

    /* Cleared first, and not restored on the way out: a create() that does
     * not finish leaves a driver that says it has no chip, which is what
     * every other call here checks. */
    s_present = false;

    if ( khal_i2c_init() != KODE_OK )
    {
        return KODE_IO_ERROR;
    }
    if ( !khal_i2c_probe(KBSP_I2C_ADDR_HAPTIC, AW_PROBE_MS) )
    {
        KLOG_W(TAG, "no ACK at 0x%02X: no haptic driver fitted",
               (unsigned)KBSP_I2C_ADDR_HAPTIC);
        return KODE_IO_ERROR;
    }
    if ( !s_added )
    {
        if ( khal_i2c_add(KBSP_I2C_ADDR_HAPTIC, &s_dev) != KODE_OK )
        {
            KLOG_E(TAG, "the bus would not take 0x%02X",
                   (unsigned)KBSP_I2C_ADDR_HAPTIC);
            return KODE_IO_ERROR;
        }
        s_added = true;
    }

    if ( !reg_read(AW_REG_CHIPID_H, &hi) || !reg_read(AW_REG_CHIPID_L, &lo) )
    {
        KLOG_E(TAG, "0x%02X acked the probe and then would not be read",
               (unsigned)KBSP_I2C_ADDR_HAPTIC);
        return KODE_IO_ERROR;
    }
    s_chipid = (uint16_t)(((uint16_t)hi << 8) | lo);
    /* The family decides, not the member: the three AW8623X parts share this
     * register map, and what must be kept off the motor is something else
     * entirely answering at this address. */
    if ( AW_FAMILY != hi )
    {
        KLOG_E(TAG, "what answers at 0x%02X reads CHIPID 0x%04X, not an "
                    "AW8623X; not driven",
               (unsigned)KBSP_I2C_ADDR_HAPTIC, (unsigned)s_chipid);
        return KODE_IO_ERROR;
    }

    if ( !reg_write(AW_REG_RSTCFG, AW_SRST) )
    {
        KLOG_E(TAG, "the soft reset would not write");
        return KODE_IO_ERROR;
    }
    khal_delay_ms(AW_RESET_MS);
    if ( !reg_write(AW_REG_SYSCTRL2, AW_SYSCTRL2_RUN) )
    {
        KLOG_E(TAG, "the chip would not leave standby; it would ack and never drive");
        return KODE_IO_ERROR;
    }

    s_present = true;

    /* Out of standby and straight back into it. The run write above is what
     * proves the chip can leave standby at all - a part that cannot would
     * otherwise be discovered at the first buzz, which reports the fault in
     * the wrong place - and parked is where a board nobody has asked to buzz
     * belongs. s_present is set before the call because that is what
     * standby() checks. */
    if ( kdrv_aw86233_standby(true) != KODE_OK )
    {
        KLOG_E(TAG, "the chip would not go back into standby");
        s_present = false;
        return KODE_IO_ERROR;
    }

    KLOG_D(TAG, "AW86233 at 0x%02X: chipid=0x%04X, RTP at %u Hz, amplitude "
                "capped at %u%%, resting in standby until something asks for "
                "a buzz",
           (unsigned)KBSP_I2C_ADDR_HAPTIC, (unsigned)s_chipid,
           (unsigned)KDRV_AW_RATE_HZ, (unsigned)KDRV_AW_GAIN_MAX_PCT);
    return KODE_OK;
}

bool kdrv_aw86233_present(void)
{
    return s_present;
}

kode_status_t kdrv_aw86233_standby(bool on)
{
    uint8_t raw = 0U;
    uint8_t next;

    if ( !s_present )
    {
        return KODE_DENIED;
    }
    /* Read-modify-write rather than a whole byte: the sample rate under bit 6
     * may be a user's, and a rest is not a reconfiguration. */
    if ( !reg_read(AW_REG_SYSCTRL2, &raw) )
    {
        return KODE_IO_ERROR;
    }
    next = (uint8_t)(raw & (uint8_t)~AW_STANDBY_BIT);
    if ( on )
    {
        next = (uint8_t)(next | AW_STANDBY_BIT);
    }
    if ( !reg_write(AW_REG_SYSCTRL2, next) )
    {
        return KODE_IO_ERROR;
    }
    return KODE_OK;
}

/* ---- Playback ------------------------------------------------------------ */

/**
 * @brief Did GO take? The chip shows it in GLB_STATE within a millisecond.
 *
 * A handful of status reads rather than a delay: each read is a bus round
 * trip of tens of microseconds, and a working part answers on the first.
 *
 * @return true once GLB_STATE reads RTP; false when it never did, or the
 *         bus failed.
 */
static bool go_took(void)
{
    uint8_t st = 0U;

    for ( uint32_t i = 0U; AW_GO_READS > i; ++i )
    {
        if ( !reg_read(AW_REG_GLBRD5, &st) )
        {
            return false;
        }
        if ( (st & AW_GLB_STATE_MASK) == AW_GLB_RTP )
        {
            return true;
        }
    }
    return false;
}

kode_status_t kdrv_aw86233_rtp_begin(uint8_t gain_pct, uint8_t *reg_written)
{
    uint8_t pct = gain_pct;
    uint8_t gain;

    if ( !s_present )
    {
        return KODE_DENIED;
    }
    /* The motor's rating, not the caller's judgement: asking for more is not
     * an error, it is simply not granted. */
    if ( (uint8_t)KDRV_AW_GAIN_MAX_PCT < pct )
    {
        pct = (uint8_t)KDRV_AW_GAIN_MAX_PCT;
    }
    gain = (uint8_t)(((uint32_t)pct * AW_FULL_SCALE) / AW_PCT_FULL);

    if ( !reg_write(AW_REG_PLAYCFG2, gain) )
    {
        return KODE_IO_ERROR;
    }
    if ( !reg_write(AW_REG_PLAYCFG3, AW_PLAYMODE_RTP) )
    {
        return KODE_IO_ERROR;
    }
    /* GO before a single sample exists: the chip plays what arrives and
     * waits on an empty FIFO, so this is the mode's contract and not a race. */
    if ( !reg_write(AW_REG_PLAYCFG4, AW_GO) )
    {
        return KODE_IO_ERROR;
    }
    /* The datasheet's own step between GO and the first sample, and the one
     * that turns the silent failure into a reported one: a driver stage
     * that is parked, or a part that is not what it claims, acknowledges GO
     * and stays in standby. */
    if ( !go_took() )
    {
        KLOG_E(TAG, "GO was acknowledged and the chip did not enter RTP");
        return KODE_IO_ERROR;
    }
    /* The byte, not the percentage: the clamp and the truncation both happen
     * above, so what the caller asked for and what the motor gets are two
     * different numbers and only one of them is worth quoting. */
    if ( NULL != reg_written )
    {
        *reg_written = gain;
    }
    return KODE_OK;
}

kode_status_t kdrv_aw86233_rtp_feed(const int8_t *pcm, size_t n, size_t *accepted)
{
    uint8_t st = 0U;
    size_t  len;

    if ( (NULL == pcm) || (NULL == accepted) || !s_present )
    {
        return KODE_DENIED;
    }
    *accepted = 0U;
    if ( 0U == n )
    {
        return KODE_OK;
    }
    if ( !reg_read(AW_REG_SYSST, &st) )
    {
        return KODE_IO_ERROR;
    }
    /* Not an error and not a wait: the FIFO is full of the caller's own
     * samples, and how to spend the time until it drains is the caller's
     * decision, not this driver's. */
    if ( (st & AW_FF_AFS) != 0U )
    {
        return KODE_OK;
    }

    len = n;
    if ( (size_t)KDRV_AW_BURST < len )
    {
        len = (size_t)KDRV_AW_BURST;
    }
    /* Signed samples on the wire as the bytes they already are: the FIFO
     * register does not auto-increment, so a burst lands sample after sample
     * on the same address. */
    if ( khal_i2c_write_regs(s_dev, AW_REG_RTPDATA, (const uint8_t *)pcm, len) != KODE_OK )
    {
        return KODE_IO_ERROR;
    }
    *accepted = len;
    return KODE_OK;
}

kode_status_t kdrv_aw86233_rtp_end(void)
{
    if ( !s_present )
    {
        return KODE_DENIED;
    }
    if ( !reg_write(AW_REG_PLAYCFG4, AW_STOP) )
    {
        return KODE_IO_ERROR;
    }
    return KODE_OK;
}

/* ---- The waveform -------------------------------------------------------- */

void kdrv_aw86233_sine_fill(int8_t *out, size_t n, uint32_t *phase)
{
    /* 12000 samples is exactly 235 whole cycles, so the index wraps there
     * with no discontinuity at all - which is why the phase is kept as a
     * sample count rather than as an angle that would drift. */
    uint32_t    k = 0U;
    float const peak = (AW_INT8_FULL * (float)KDRV_AW_GAIN_MAX_PCT)
                       / (float)AW_PCT_FULL;

    if ( (NULL == out) || (NULL == phase) )
    {
        return;
    }
    k = *phase % KDRV_AW_RATE_HZ;
    for ( size_t i = 0U; i < n; ++i )
    {
        /* The P4 has no double-precision unit, so every step of this is
         * single precision - and the angle is reduced in INTEGERS first.
         * Written the obvious way, the numerator 2*pi*F0*k reaches about
         * eighteen million by the end of a wrap, where a float's spacing is
         * of the order of a radian; F0 * k modulo the sample rate is exact,
         * and what is left to convert never exceeds one turn. */
        uint32_t const turn = ((uint32_t)KDRV_AW_F0_HZ * k) % KDRV_AW_RATE_HZ;
        float const    ph   = (AW_TWO_PI_F * (float)turn)
                              / (float)KDRV_AW_RATE_HZ;
        out[i] = (int8_t)lroundf(peak * sinf(ph));
        k++;
        if ( KDRV_AW_RATE_HZ <= k )
        {
            k = 0U;
        }
    }
    *phase = k;
}

uint32_t kdrv_aw86233_playing_ms(size_t samples)
{
    /* Rounded up: a partial millisecond is still playing, and this number is
     * what an owner waits out before STOP. */
    uint64_t const num = ((uint64_t)samples * 1000U) + (uint64_t)(KDRV_AW_RATE_HZ - 1U);

    return (uint32_t)(num / (uint64_t)KDRV_AW_RATE_HZ);
}

/* ---- Fields -------------------------------------------------------------- */

/** @brief The driver's own cache: no bus, and an answer even with no chip. */
static kode_status_t driver_field(kdrv_aw86233_field_t f, int32_t *value)
{
    if ( KDRV_AW_PRESENT == f )
    {
        *value = 0;
        if ( s_present )
        {
            *value = 1;
        }
        return KODE_OK;
    }
    if ( !s_present )
    {
        return KODE_DENIED;
    }
    *value = (int32_t)s_chipid;
    return KODE_OK;
}

kode_status_t kdrv_aw86233_get(kdrv_aw86233_field_t f, int32_t *value)
{
    uint8_t raw = 0U;

    if ( (NULL == value) || (KDRV_AW_FIELD_MAX <= f) )
    {
        return KODE_DENIED;
    }
    const field_t * const row = &FIELDS[f];

    if ( (uint8_t)KIND_DRIVER == row->kind )
    {
        return driver_field(f, value);
    }
    if ( !s_present )
    {
        return KODE_DENIED;
    }
    if ( !reg_read(row->reg, &raw) )
    {
        return KODE_IO_ERROR;
    }
    *value = (int32_t)((raw & field_mask(row)) >> row->shift);
    return KODE_OK;
}

kode_status_t kdrv_aw86233_set(kdrv_aw86233_field_t f, int32_t value)
{
    uint8_t raw = 0U;
    uint8_t bits;
    uint8_t next;

    if ( KDRV_AW_FIELD_MAX <= f )
    {
        return KODE_DENIED;
    }
    const field_t * const row = &FIELDS[f];

    /* Read-only is decided before the chip is: a console asking to write the
     * identity is wrong whether or not a haptic driver is fitted. */
    if ( !row->info.writable )
    {
        return KODE_DENIED;
    }
    if ( (value < row->info.min) || (value > row->info.max) )
    {
        return KODE_DENIED;
    }
    if ( !s_present )
    {
        return KODE_DENIED;
    }

    bits = (uint8_t)((uint8_t)value << row->shift);
    next = bits;
    if ( 8U > row->width )
    {
        if ( !reg_read(row->reg, &raw) )
        {
            return KODE_IO_ERROR;
        }
        next = (uint8_t)((raw & (uint8_t)~field_mask(row)) | bits);
    }
    if ( !reg_write(row->reg, next) )
    {
        return KODE_IO_ERROR;
    }
    return KODE_OK;
}

const kdrv_aw86233_field_info_t *kdrv_aw86233_field_info(kdrv_aw86233_field_t f)
{
    if ( KDRV_AW_FIELD_MAX <= f )
    {
        return NULL;
    }
    return &FIELDS[f].info;
}

kode_status_t kdrv_aw86233_field_by_name(const char *name, kdrv_aw86233_field_t *out)
{
    if ( (NULL == name) || (NULL == out) )
    {
        return KODE_DENIED;
    }
    for ( uint32_t i = 0U; (uint32_t)KDRV_AW_FIELD_MAX > i; ++i )
    {
        if ( strcmp(FIELDS[i].info.name, name) == 0 )
        {
            *out = (kdrv_aw86233_field_t)i;
            return KODE_OK;
        }
    }
    return KODE_DENIED;
}

/* ---- Dump ---------------------------------------------------------------- */

/** @brief Append @p text to @p line, flushing it as a log line when full. */
static void dump_append(char *line, size_t *n, const char *text)
{
    size_t const len = strlen(text);

    if ( (*n + len) >= AW_DUMP_LINE )
    {
        KLOG_N(TAG, "%s", line);
        *n = 0U;
        line[0] = '\0';
    }
    (void)memcpy(&line[*n], text, len + 1U);
    *n += len;
}

void kdrv_aw86233_dump(void)
{
    char   line[AW_DUMP_LINE];
    char   item[40];
    size_t n = 0U;

    if ( !s_present )
    {
        KLOG_N(TAG, "no AW86233; nothing to dump");
        return;
    }
    line[0] = '\0';
    for ( uint32_t f = 0U; (uint32_t)KDRV_AW_FIELD_MAX > f; ++f )
    {
        const field_t * const row = &FIELDS[f];
        int32_t v = 0;

        if ( kdrv_aw86233_get((kdrv_aw86233_field_t)f, &v) != KODE_OK )
        {
            (void)snprintf(item, sizeof(item), "%s=? ", row->info.name);
        }
        else
        {
            (void)snprintf(item, sizeof(item), "%s=%ld ", row->info.name, (long)v);
        }
        dump_append(line, &n, item);
    }
    KLOG_N(TAG, "%s", line);
}
