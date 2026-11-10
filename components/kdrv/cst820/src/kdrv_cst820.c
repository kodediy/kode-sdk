/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_cst820.c
 * @brief CST820: the register map as a table, and the frame as one burst.
 */
#include "kdrv_cst820.h"

#include <stdio.h>
#include <string.h>

#include "kbsp.h"
#include "khal_i2c.h"
#include "khal_time.h"
#include "kode_log.h"

#define TAG "kdrv.cst820"

#define CST_REG_SLEEP       0xE5U   /* sleep mode: CST_SLEEP_DEEP stops the chip */
#define CST_SLEEP_DEEP      0x03U   /* deep sleep: no scan, no ACK, until a reset */
#define CST_PROBE_MS        50U
#define CST_PROBE_TRIES     3U
#define CST_PROBE_GAP_MS    20U
#define CST_LOOK_MS         20U     /* one look's probe (kdrv_cst820_answers()) */
#define CST_ID_TRIES        3U      /* reads of the identity before a part is judged not to be a CST8xx */
#define CST_REG_FRAME       0x01U   /* gesture, fingers, xh, xl, yh, yl */
#define CST_FRAME_LEN       6U
#define CST_REG_CHIP_ID     0xA7U   /* chip_id, proj_id, fw_ver */
#define CST_ID_LEN          3U
#define CST_FAMILY_MASK     0xF0U   /* the chip_id's family nibble */
#define CST_FAMILY_ID       0xB0U   /* CST816/820 read 0xB4..0xB7, CST9xx 0xBx too */
#define CST_NIBBLE          0x0FU   /* the high byte of a coordinate */
#define CST_REG_CFG         0xECU   /* motion_mask .. dis_auto_sleep */
#define CST_CFG_LEN         19U
#define CST_DUMP_LINE       96U

/* ---- The table ----------------------------------------------------------- */

#define F_RW      0U
#define F_RO      1U    /* the frame and the identity: set() refuses them */
#define F_CACHED  2U    /* the identity: served from the bring-up cache */

/** @brief One field: what the outside sees, then where it lives. */
typedef struct {
    kdrv_cst820_field_info_t info;
    uint8_t reg;      /**< The register; for a wide field, its high byte. */
    uint8_t shift;
    uint8_t width;    /**< Above 8: two registers, reg then reg + 1.     */
    uint8_t flags;
} field_t;

/* Every field here is a plain code - a byte, a bit, or two bytes - so min
 * is 0, max is the widest code, and step is 1. Spelled out so that
 * field_info() can hand back a pointer into the table. */
#define ROW(nm, un, r, sh, w, fl) \
    { { (nm), (un), 0, (int32_t)((1UL << (w)) - 1UL), 1U, (((fl) & F_RO) == 0U) }, \
      (r), (sh), (w), (fl) }
#define BYTE(nm, un, r, fl)   ROW(nm, un, r, 0U, 8U, fl)
#define BIT(nm, r, bit, fl)   ROW(nm, "bool", r, bit, 1U, fl)
#define WIDE(nm, un, r, w)    ROW(nm, un, r, 0U, w, F_RO)

static const field_t FIELDS[KDRV_CST_FIELD_MAX] = {
    [KDRV_CST_GESTURE]           = BYTE("gesture",  "code", 0x01U, F_RO),
    [KDRV_CST_FINGERS]           = BYTE("fingers",  "code", 0x02U, F_RO),
    [KDRV_CST_EVENT]             = ROW("event", "code", 0x03U, 6U, 2U, F_RO),
    [KDRV_CST_X]                 = WIDE("x", "px", 0x03U, 12U),
    [KDRV_CST_Y]                 = WIDE("y", "px", 0x05U, 12U),
    [KDRV_CST_CHIP_ID]           = BYTE("chip_id", "code", 0xA7U, F_RO | F_CACHED),
    [KDRV_CST_PROJ_ID]           = BYTE("proj_id", "code", 0xA8U, F_RO | F_CACHED),
    [KDRV_CST_FW_VER]            = BYTE("fw_ver",  "code", 0xA9U, F_RO | F_CACHED),
    [KDRV_CST_MOTION_MASK]       = BYTE("motion_mask", "bits", 0xECU, F_RW),
    [KDRV_CST_EN_DCLICK]         = BIT("en_dclick", 0xECU, 0U, F_RW),
    [KDRV_CST_EN_CON_UD]         = BIT("en_con_ud", 0xECU, 1U, F_RW),
    [KDRV_CST_EN_CON_LR]         = BIT("en_con_lr", 0xECU, 2U, F_RW),
    [KDRV_CST_IRQ_PULSE_WIDTH]   = BYTE("irq_pulse_width", "0.1ms", 0xEDU, F_RW),
    [KDRV_CST_NOR_SCAN_PER]      = BYTE("nor_scan_per", "10ms", 0xEEU, F_RW),
    [KDRV_CST_MOTION_SL_ANGLE]   = BYTE("motion_sl_angle", "code", 0xEFU, F_RW),
    [KDRV_CST_LP_SCAN_RAW1]      = WIDE("lp_scan_raw1", "code", 0xF0U, 16U),
    [KDRV_CST_LP_SCAN_RAW2]      = WIDE("lp_scan_raw2", "code", 0xF2U, 16U),
    [KDRV_CST_LP_AUTO_WAKE_TIME] = BYTE("lp_auto_wake_time", "min", 0xF4U, F_RW),
    [KDRV_CST_LP_SCAN_TH]        = BYTE("lp_scan_th",   "code", 0xF5U, F_RW),
    [KDRV_CST_LP_SCAN_WIN]       = BYTE("lp_scan_win",  "code", 0xF6U, F_RW),
    [KDRV_CST_LP_SCAN_FREQ]      = BYTE("lp_scan_freq", "code", 0xF7U, F_RW),
    [KDRV_CST_LP_SCAN_IDAC]      = BYTE("lp_scan_idac", "code", 0xF8U, F_RW),
    [KDRV_CST_AUTO_SLEEP_TIME]   = BYTE("auto_sleep_time", "s", 0xF9U, F_RW),
    [KDRV_CST_IRQ_CTL]           = BYTE("irq_ctl", "bits", 0xFAU, F_RW),
    [KDRV_CST_EN_TEST]           = BIT("en_test",   0xFAU, 7U, F_RW),
    [KDRV_CST_EN_TOUCH]          = BIT("en_touch",  0xFAU, 6U, F_RW),
    [KDRV_CST_EN_CHANGE]         = BIT("en_change", 0xFAU, 5U, F_RW),
    [KDRV_CST_EN_MOTION]         = BIT("en_motion", 0xFAU, 4U, F_RW),
    [KDRV_CST_ONCE_WLP]          = BIT("once_wlp",  0xFAU, 0U, F_RW),
    [KDRV_CST_AUTO_RESET]        = BYTE("auto_reset", "s", 0xFBU, F_RW),
    [KDRV_CST_LONG_PRESS_TIME]   = BYTE("long_press_time", "s", 0xFCU, F_RW),
    [KDRV_CST_IO_CTL]            = BYTE("io_ctl", "bits", 0xFDU, F_RW),
    [KDRV_CST_SOFT_RST]          = BIT("soft_rst", 0xFDU, 2U, F_RW),
    [KDRV_CST_IIC_OD]            = BIT("iic_od",   0xFDU, 1U, F_RW),
    [KDRV_CST_EN_1V8]            = BIT("en_1v8",   0xFDU, 0U, F_RW),
    [KDRV_CST_DIS_AUTO_SLEEP]    = BYTE("dis_auto_sleep", "bits", 0xFEU, F_RW),
};

static khal_i2c_dev_t s_dev;
static bool           s_bus_ok = true;   /* edge state for the log lines */
static bool           s_may_sleep;       /* auto-sleep was switched on: silence is the chip asleep */
static uint8_t        s_id[CST_ID_LEN];  /* the bring-up cache */

/* ---- Register access ----------------------------------------------------- */

/**
 * @brief Say once when the chip stops answering, and once when it returns.
 *
 * Every poll while a finger is down comes through here. A line per failed
 * read would bury the console this is trying to warn.
 */
static void note_bus(bool ok)
{
    if ( ok != s_bus_ok )
    {
        s_bus_ok = ok;
        if ( s_may_sleep && ok )
        {
            KLOG_D(TAG, "touch controller answering again");
        }
        else if ( s_may_sleep )
        {
            /* Its rest, asked for: it sleeps until a finger wakes it. */
            KLOG_D(TAG, "touch controller asleep");
        }
        else if ( ok )
        {
            KLOG_N(TAG, "touch controller answering again");
        }
        else
        {
            KLOG_W(TAG, "touch controller stopped answering (asleep, or gone); "
                        "samples withheld rather than reported as a release");
        }
    }
}

/** @brief Read @p len bytes from @p reg in one transaction. */
static bool regs_read(uint8_t reg, uint8_t *buf, size_t len)
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

/** @brief The mask of a byte-sized field's bits, in place. */
static uint8_t field_mask(const field_t *f)
{
    uint8_t const ones = (uint8_t)((1U << f->width) - 1U);
    return (uint8_t)(ones << f->shift);
}

/** @brief The value a raw register byte holds for a byte-sized @p f. */
static int32_t field_value(const field_t *f, uint8_t raw)
{
    return (int32_t)((raw & field_mask(f)) >> f->shift);
}

/** @brief The value two consecutive bytes hold for a wide @p f. */
static int32_t wide_value(const field_t *f, uint8_t hi, uint8_t lo)
{
    uint8_t const hmask = (uint8_t)((1U << (f->width - 8U)) - 1U);
    return (int32_t)(((uint16_t)(hi & hmask) << 8) | lo);
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

/** @brief Does anything ACK at the controller's address, given time to wake? */
static bool probe(void)
{
    for ( uint8_t i = 0U; CST_PROBE_TRIES > i; ++i )
    {
        if ( khal_i2c_probe(KBSP_I2C_ADDR_TOUCH, CST_PROBE_MS) )
        {
            return true;
        }
        if ( (uint8_t)(i + 1U) < CST_PROBE_TRIES )
        {
            khal_delay_ms(CST_PROBE_GAP_MS);
        }
    }
    return false;
}

kode_status_t kdrv_cst820_init(void)
{
    if ( NULL != s_dev )
    {
        return KODE_OK;
    }
    if ( khal_i2c_init() != KODE_OK )
    {
        return KODE_ERROR;
    }
    if ( !probe() )
    {
        KLOG_E(TAG, "no ACK at 0x%02X in %u tries: no CST820",
               (unsigned)KBSP_I2C_ADDR_TOUCH, (unsigned)CST_PROBE_TRIES);
        return KODE_ERROR;
    }
    if ( khal_i2c_add(KBSP_I2C_ADDR_TOUCH, &s_dev) != KODE_OK )
    {
        s_dev = NULL;
        return KODE_ERROR;
    }
    /* An ACK says something is at 0x15, not what. Whatever answers here
     * becomes the touch controller, and its register 0x02 becomes a finger
     * count - so a part outside the family is refused rather than read as
     * phantom presses. The family nibble and not the exact 0xB7: a second
     * batch of the same controller may read 0xB4..0xB6, and that is still a
     * touch controller with this register map. A controller that ACKs while
     * it wakes can read 0x00 once, so the read is repeated and only the last
     * answer is an error. */
    bool family = false;
    for ( uint8_t i = 0U; (CST_ID_TRIES > i) && !family; ++i )
    {
        if ( 0U < i )
        {
            khal_delay_ms(CST_PROBE_GAP_MS);
        }
        if ( !regs_read(CST_REG_CHIP_ID, s_id, CST_ID_LEN) )
        {
            release();
            return KODE_ERROR;
        }
        family = ((s_id[0] & CST_FAMILY_MASK) == CST_FAMILY_ID);
    }
    if ( !family )
    {
        KLOG_E(TAG, "chip_id=0x%02X at 0x%02X is not a CST8xx (0xBx); "
                    "refusing to drive it",
               (unsigned)s_id[0], (unsigned)KBSP_I2C_ADDR_TOUCH);
        release();
        return KODE_ERROR;
    }
    KLOG_D(TAG, "CST820 at 0x%02X: chip_id=0x%02X proj_id=0x%02X fw_ver=0x%02X; "
                "nothing written",
           (unsigned)KBSP_I2C_ADDR_TOUCH, (unsigned)s_id[0], (unsigned)s_id[1],
           (unsigned)s_id[2]);
    return KODE_OK;
}

kode_status_t kdrv_cst820_park(void)
{
    kode_status_t rc;

    /* One quiet probe first: a part that is not fitted is said once, by the
     * caller, and not as the bring-up's error. */
    if ( (khal_i2c_init() != KODE_OK) || !khal_i2c_probe(KBSP_I2C_ADDR_TOUCH, CST_PROBE_MS) )
    {
        return KODE_ERROR;
    }
    rc = kdrv_cst820_init();

    if ( (KODE_OK == rc) && !reg_write(CST_REG_SLEEP, CST_SLEEP_DEEP) )
    {
        rc = KODE_IO_ERROR;
    }
    if ( NULL != s_dev )
    {
        /* From here the chip NAKs until a reset: nothing is left to talk to. */
        release();
    }
    return rc;
}

bool kdrv_cst820_present(void)
{
    return NULL != s_dev;
}

bool kdrv_cst820_answers(void)
{
    if ( NULL != s_dev )
    {
        return true;
    }
    return (khal_i2c_init() == KODE_OK) && khal_i2c_probe(KBSP_I2C_ADDR_TOUCH, CST_LOOK_MS);
}

kode_status_t kdrv_cst820_identity(uint8_t *chip_id, uint8_t *proj_id,
                                   uint8_t *fw_ver)
{
    if ( (NULL == chip_id) || (NULL == proj_id) || (NULL == fw_ver)
        || (NULL == s_dev) )
    {
        return KODE_DENIED;
    }
    *chip_id = s_id[0];
    *proj_id = s_id[1];
    *fw_ver  = s_id[2];
    return KODE_OK;
}

/* ---- The frame ----------------------------------------------------------- */

kode_status_t kdrv_cst820_read(bool *pressed, int16_t *x, int16_t *y)
{
    uint8_t frame[CST_FRAME_LEN];

    if ( (NULL == pressed) || (NULL == x) || (NULL == y) || (NULL == s_dev) )
    {
        return KODE_DENIED;
    }
    if ( !regs_read(CST_REG_FRAME, frame, CST_FRAME_LEN) )
    {
        return KODE_ERROR;
    }
    /* frame[0] gesture, [1] fingers, [2] event|xh, [3] xl, [4] yh, [5] yl */
    *pressed = (0U != frame[1]);
    if ( *pressed )
    {
        *x = (int16_t)(((uint16_t)(frame[2] & CST_NIBBLE) << 8) | frame[3]);
        *y = (int16_t)(((uint16_t)(frame[4] & CST_NIBBLE) << 8) | frame[5]);
    }
    return KODE_OK;
}

/* ---- Fields -------------------------------------------------------------- */

kode_status_t kdrv_cst820_get(kdrv_cst820_field_t f, int32_t *value)
{
    uint8_t raw[2] = { 0U, 0U };

    if ( (NULL == value) || (KDRV_CST_FIELD_MAX <= f) || (NULL == s_dev) )
    {
        return KODE_DENIED;
    }
    const field_t * const row = &FIELDS[f];
    if ( (row->flags & F_CACHED) != 0U )
    {
        *value = (int32_t)s_id[row->reg - CST_REG_CHIP_ID];
        return KODE_OK;
    }
    if ( 8U < row->width )
    {
        if ( !regs_read(row->reg, raw, 2U) )
        {
            return KODE_ERROR;
        }
        *value = wide_value(row, raw[0], raw[1]);
        return KODE_OK;
    }
    if ( !regs_read(row->reg, raw, 1U) )
    {
        return KODE_ERROR;
    }
    *value = field_value(row, raw[0]);
    return KODE_OK;
}

kode_status_t kdrv_cst820_set(kdrv_cst820_field_t f, int32_t value)
{
    uint8_t raw  = 0U;
    uint8_t back = 0U;

    if ( (KDRV_CST_FIELD_MAX <= f) || (NULL == s_dev) )
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

    if ( !regs_read(row->reg, &raw, 1U) )
    {
        return KODE_ERROR;
    }
    uint8_t const bits = (uint8_t)((uint8_t)value << row->shift);
    uint8_t const next = (uint8_t)((raw & (uint8_t)~field_mask(row)) | bits);
    if ( !reg_write(row->reg, next) )
    {
        return KODE_ERROR;
    }
    /* Only this field's bits are compared: the rest of the register is
     * somebody else's setting, or a status bit the chip moves on its own. */
    if ( !regs_read(row->reg, &back, 1U) )
    {
        return KODE_ERROR;
    }
    if ( (back & field_mask(row)) != bits )
    {
        KLOG_W(TAG, "%s: wrote %ld, chip kept 0x%02X", row->info.name,
               (long)value, (unsigned)back);
        return KODE_ERROR;
    }
    if ( KDRV_CST_DIS_AUTO_SLEEP == f )
    {
        s_may_sleep = (0 == value);
    }
    return KODE_OK;
}

const kdrv_cst820_field_info_t *kdrv_cst820_field_info(kdrv_cst820_field_t f)
{
    if ( KDRV_CST_FIELD_MAX <= f )
    {
        return NULL;
    }
    return &FIELDS[f].info;
}

kode_status_t kdrv_cst820_field_by_name(const char *name, kdrv_cst820_field_t *out)
{
    if ( (NULL == name) || (NULL == out) )
    {
        return KODE_DENIED;
    }
    for ( uint32_t i = 0U; (uint32_t)KDRV_CST_FIELD_MAX > i; ++i )
    {
        if ( strcmp(FIELDS[i].info.name, name) == 0 )
        {
            *out = (kdrv_cst820_field_t)i;
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

    if ( (*n + len) >= CST_DUMP_LINE )
    {
        KLOG_N(TAG, "%s", line);
        *n = 0U;
        line[0] = '\0';
    }
    (void)memcpy(&line[*n], text, len + 1U);
    *n += len;
}

void kdrv_cst820_dump(void)
{
    uint8_t frame[CST_FRAME_LEN];
    uint8_t cfg[CST_CFG_LEN];
    char    line[CST_DUMP_LINE];
    char    item[32];
    size_t  n = 0U;

    if ( NULL == s_dev )
    {
        KLOG_N(TAG, "no CST820; nothing to dump");
        return;
    }
    KLOG_N(TAG, "chip_id=0x%02X proj_id=0x%02X fw_ver=0x%02X (read at bring-up)",
           (unsigned)s_id[0], (unsigned)s_id[1], (unsigned)s_id[2]);

    /* Two bursts: the frame, then the whole configuration block. A chip that
     * is asleep NAKs the first one, and that is one line, not twenty. */
    if ( !regs_read(CST_REG_FRAME, frame, CST_FRAME_LEN) )
    {
        KLOG_W(TAG, "the chip did not answer: asleep until a finger wakes it, "
                    "or gone. Finger on the glass, then ask again");
        return;
    }
    KLOG_N(TAG, "gesture=%u fingers=%u event=%u x=%u y=%u",
           (unsigned)frame[0], (unsigned)frame[1], (unsigned)(frame[2] >> 6),
           (unsigned)(((uint16_t)(frame[2] & CST_NIBBLE) << 8) | frame[3]),
           (unsigned)(((uint16_t)(frame[4] & CST_NIBBLE) << 8) | frame[5]));
    if ( !regs_read(CST_REG_CFG, cfg, CST_CFG_LEN) )
    {
        KLOG_W(TAG, "the configuration block did not read");
        return;
    }

    line[0] = '\0';
    for ( uint32_t f = (uint32_t)KDRV_CST_MOTION_MASK; (uint32_t)KDRV_CST_FIELD_MAX > f; ++f )
    {
        const field_t * const row = &FIELDS[f];
        uint8_t const at = (uint8_t)(row->reg - CST_REG_CFG);
        int32_t v;
        if ( 8U < row->width )
        {
            v = wide_value(row, cfg[at], cfg[at + 1U]);
        }
        else
        {
            v = field_value(row, cfg[at]);
        }
        (void)snprintf(item, sizeof(item), "%s=%ld ", row->info.name, (long)v);
        dump_append(line, &n, item);
    }
    KLOG_N(TAG, "%s", line);
}
