/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_tca6408a.c
 * @brief TCA6408A: the register map as a table, and the interrupt line.
 */
#include "kdrv_tca6408a.h"

#include <stdio.h>
#include <string.h>

#include "kbsp.h"
#include "khal_gpio.h"
#include "khal_i2c.h"
#include "kode_log.h"

#define TAG "kdrv.tca6408a"

#define TCA_PROBE_MS        100U
#define TCA_REG_INPUT       0x00U
#define TCA_REG_OUTPUT      0x01U
#define TCA_REG_POLARITY    0x02U
#define TCA_REG_CONFIG      0x03U
#define TCA_REG_COUNT       4U
#define TCA_RESET_OUTPUT    0xFFU   /* the power-on value of 0x01 */
#define TCA_RESET_POLARITY  0x00U   /* of 0x02 */
#define TCA_RESET_CONFIG    0xFFU   /* of 0x03: every pin an input */
#define TCA_DUMP_LINE       96U

/* ---- The table ----------------------------------------------------------- */

#define F_RW    0U
#define F_RO    1U      /* the input port: set() refuses it */

/** @brief One field: what the outside sees, then where it lives. */
typedef struct {
    kdrv_tca6408a_field_info_t info;
    uint8_t reg;
    uint8_t shift;
    uint8_t width;
    uint8_t flags;
} field_t;

/* Every field here is a plain code - a byte or a bit - so min is 0, max is
 * the widest code, and step is 1. Spelled out so that field_info() can hand
 * back a pointer into the table rather than into a scratch buffer. */
#define ROW(nm, un, r, sh, w, fl) \
    { { (nm), (un), 0, (int32_t)((1U << (w)) - 1U), 1U, (((fl) & F_RO) == 0U) }, \
      (r), (sh), (w), (fl) }
#define BYTE(nm, r, fl)      ROW(nm, "bits", r, 0U, 8U, fl)
#define BIT(nm, r, bit, fl)  ROW(nm, "bool", r, bit, 1U, fl)

static const field_t FIELDS[KDRV_TCA_FIELD_MAX] = {
    [KDRV_TCA_INPUT]    = BYTE("input",    TCA_REG_INPUT,    F_RO),
    [KDRV_TCA_OUTPUT]   = BYTE("output",   TCA_REG_OUTPUT,   F_RW),
    [KDRV_TCA_POLARITY] = BYTE("polarity", TCA_REG_POLARITY, F_RW),
    [KDRV_TCA_CONFIG]   = BYTE("config",   TCA_REG_CONFIG,   F_RW),
    [KDRV_TCA_I0] = BIT("i0", TCA_REG_INPUT, 0U, F_RO),
    [KDRV_TCA_I1] = BIT("i1", TCA_REG_INPUT, 1U, F_RO),
    [KDRV_TCA_I2] = BIT("i2", TCA_REG_INPUT, 2U, F_RO),
    [KDRV_TCA_I3] = BIT("i3", TCA_REG_INPUT, 3U, F_RO),
    [KDRV_TCA_I4] = BIT("i4", TCA_REG_INPUT, 4U, F_RO),
    [KDRV_TCA_I5] = BIT("i5", TCA_REG_INPUT, 5U, F_RO),
    [KDRV_TCA_I6] = BIT("i6", TCA_REG_INPUT, 6U, F_RO),
    [KDRV_TCA_I7] = BIT("i7", TCA_REG_INPUT, 7U, F_RO),
    [KDRV_TCA_O0] = BIT("o0", TCA_REG_OUTPUT, 0U, F_RW),
    [KDRV_TCA_O1] = BIT("o1", TCA_REG_OUTPUT, 1U, F_RW),
    [KDRV_TCA_O2] = BIT("o2", TCA_REG_OUTPUT, 2U, F_RW),
    [KDRV_TCA_O3] = BIT("o3", TCA_REG_OUTPUT, 3U, F_RW),
    [KDRV_TCA_O4] = BIT("o4", TCA_REG_OUTPUT, 4U, F_RW),
    [KDRV_TCA_O5] = BIT("o5", TCA_REG_OUTPUT, 5U, F_RW),
    [KDRV_TCA_O6] = BIT("o6", TCA_REG_OUTPUT, 6U, F_RW),
    [KDRV_TCA_O7] = BIT("o7", TCA_REG_OUTPUT, 7U, F_RW),
    [KDRV_TCA_N0] = BIT("n0", TCA_REG_POLARITY, 0U, F_RW),
    [KDRV_TCA_N1] = BIT("n1", TCA_REG_POLARITY, 1U, F_RW),
    [KDRV_TCA_N2] = BIT("n2", TCA_REG_POLARITY, 2U, F_RW),
    [KDRV_TCA_N3] = BIT("n3", TCA_REG_POLARITY, 3U, F_RW),
    [KDRV_TCA_N4] = BIT("n4", TCA_REG_POLARITY, 4U, F_RW),
    [KDRV_TCA_N5] = BIT("n5", TCA_REG_POLARITY, 5U, F_RW),
    [KDRV_TCA_N6] = BIT("n6", TCA_REG_POLARITY, 6U, F_RW),
    [KDRV_TCA_N7] = BIT("n7", TCA_REG_POLARITY, 7U, F_RW),
    [KDRV_TCA_C0] = BIT("c0", TCA_REG_CONFIG, 0U, F_RW),
    [KDRV_TCA_C1] = BIT("c1", TCA_REG_CONFIG, 1U, F_RW),
    [KDRV_TCA_C2] = BIT("c2", TCA_REG_CONFIG, 2U, F_RW),
    [KDRV_TCA_C3] = BIT("c3", TCA_REG_CONFIG, 3U, F_RW),
    [KDRV_TCA_C4] = BIT("c4", TCA_REG_CONFIG, 4U, F_RW),
    [KDRV_TCA_C5] = BIT("c5", TCA_REG_CONFIG, 5U, F_RW),
    [KDRV_TCA_C6] = BIT("c6", TCA_REG_CONFIG, 6U, F_RW),
    [KDRV_TCA_C7] = BIT("c7", TCA_REG_CONFIG, 7U, F_RW),
};

static khal_i2c_dev_t s_dev;
static bool           s_bus_ok = true;   /* edge state for the log lines */

/* ---- Register access ----------------------------------------------------- */

/**
 * @brief Say once when the bus stops answering, and once when it returns.
 *
 * Every interrupt's read comes through here. A line per failed read would
 * bury the console this is trying to warn.
 */
static void note_bus(bool ok)
{
    if ( ok != s_bus_ok )
    {
        s_bus_ok = ok;
        if ( ok )
        {
            KLOG_N(TAG, "expander answering again");
        }
        else
        {
            KLOG_E(TAG, "expander stopped answering; inputs withheld rather "
                        "than reported as released");
        }
    }
}

/** @brief Read one register. Each register is its own transaction: the
 *  command byte does not auto-increment on this part. */
static bool reg_read(uint8_t reg, uint8_t *val)
{
    bool const ok = (NULL != s_dev)
                 && (khal_i2c_read_reg(s_dev, reg, val, 1U) == KODE_OK);
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

/** @brief The value a raw register byte holds for @p f. */
static int32_t field_value(const field_t *f, uint8_t raw)
{
    return (int32_t)((raw & field_mask(f)) >> f->shift);
}

/** @brief All four registers, one transaction each, in register order. */
static bool regs_read(uint8_t regs[TCA_REG_COUNT])
{
    bool ok = true;
    for ( uint8_t r = 0U; (TCA_REG_COUNT > r) && ok; ++r )
    {
        ok = reg_read(r, &regs[r]);
    }
    return ok;
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

/* What the registers said when this boot first read them. */
static uint8_t s_boot_output;
static uint8_t s_boot_config = 0xFFU;

/* cppcheck-suppress misra-c2012-8.7 ; a driver call for other components (kdrv_display.c) */
bool kdrv_tca6408a_boot_drove_high(uint8_t pin)
{
    uint8_t const bit = (uint8_t)(1U << (pin & 7U));

    return ((s_boot_config & bit) == 0U) && ((s_boot_output & bit) != 0U);
}

kode_status_t kdrv_tca6408a_init(void)
{
    uint8_t regs[TCA_REG_COUNT];

    if ( NULL != s_dev )
    {
        return KODE_OK;
    }
    if ( khal_i2c_init() != KODE_OK )
    {
        return KODE_ERROR;
    }
    if ( !khal_i2c_probe(KBSP_I2C_ADDR_IOEXP, TCA_PROBE_MS) )
    {
        KLOG_E(TAG, "no ACK at 0x%02X: no expander", (unsigned)KBSP_I2C_ADDR_IOEXP);
        return KODE_ERROR;
    }
    if ( khal_i2c_add(KBSP_I2C_ADDR_IOEXP, &s_dev) != KODE_OK )
    {
        s_dev = NULL;
        return KODE_ERROR;
    }
    if ( !regs_read(regs) )
    {
        release();
        return KODE_ERROR;
    }

    /* RESET is strapped high on this board, so only a power cycle puts the
     * defaults back. Anything else is the last firmware's doing - most often
     * P7 already an output driving the panel across a reboot. */
    bool const cold = (TCA_RESET_OUTPUT == regs[TCA_REG_OUTPUT])
                   && (TCA_RESET_POLARITY == regs[TCA_REG_POLARITY])
                   && (TCA_RESET_CONFIG == regs[TCA_REG_CONFIG]);
    s_boot_output = regs[TCA_REG_OUTPUT];
    s_boot_config = regs[TCA_REG_CONFIG];
    const char *state = "left by the last firmware";
    if ( cold )
    {
        state = "power-on defaults";
    }
    KLOG_D(TAG, "TCA6408A at 0x%02X: input=0x%02X output=0x%02X polarity=0x%02X "
                "config=0x%02X (%s); nothing written",
           (unsigned)KBSP_I2C_ADDR_IOEXP, (unsigned)regs[TCA_REG_INPUT],
           (unsigned)regs[TCA_REG_OUTPUT], (unsigned)regs[TCA_REG_POLARITY],
           (unsigned)regs[TCA_REG_CONFIG], state);
    return KODE_OK;
}

/* cppcheck-suppress misra-c2012-8.7 ; a driver call for its owner and the console, beside the rest of the API */
bool kdrv_tca6408a_present(void)
{
    return NULL != s_dev;
}

/* ---- Fields -------------------------------------------------------------- */

kode_status_t kdrv_tca6408a_get(kdrv_tca6408a_field_t f, int32_t *value)
{
    uint8_t raw = 0U;

    if ( (NULL == value) || (KDRV_TCA_FIELD_MAX <= f) || (NULL == s_dev) )
    {
        return KODE_DENIED;
    }
    if ( !reg_read(FIELDS[f].reg, &raw) )
    {
        return KODE_ERROR;
    }
    *value = field_value(&FIELDS[f], raw);
    return KODE_OK;
}

kode_status_t kdrv_tca6408a_set(kdrv_tca6408a_field_t f, int32_t value)
{
    uint8_t raw  = 0U;
    uint8_t back = 0U;

    if ( (KDRV_TCA_FIELD_MAX <= f) || (NULL == s_dev) )
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

    if ( !reg_read(row->reg, &raw) )
    {
        return KODE_ERROR;
    }
    uint8_t const bits = (uint8_t)((uint8_t)value << row->shift);
    uint8_t const next = (uint8_t)((raw & (uint8_t)~field_mask(row)) | bits);
    if ( !reg_write(row->reg, next) )
    {
        return KODE_ERROR;
    }
    /* Only this field's bits are compared: the other seven belong to other
     * pins, and an input among them may have moved between the two reads. */
    if ( !reg_read(row->reg, &back) )
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

const kdrv_tca6408a_field_info_t *kdrv_tca6408a_field_info(kdrv_tca6408a_field_t f)
{
    if ( KDRV_TCA_FIELD_MAX <= f )
    {
        return NULL;
    }
    return &FIELDS[f].info;
}

kode_status_t kdrv_tca6408a_field_by_name(const char *name,
                                          kdrv_tca6408a_field_t *out)
{
    if ( (NULL == name) || (NULL == out) )
    {
        return KODE_DENIED;
    }
    for ( uint32_t i = 0U; (uint32_t)KDRV_TCA_FIELD_MAX > i; ++i )
    {
        if ( strcmp(FIELDS[i].info.name, name) == 0 )
        {
            *out = (kdrv_tca6408a_field_t)i;
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

    if ( (*n + len) >= TCA_DUMP_LINE )
    {
        KLOG_N(TAG, "%s", line);
        *n = 0U;
        line[0] = '\0';
    }
    (void)memcpy(&line[*n], text, len + 1U);
    *n += len;
}

void kdrv_tca6408a_dump(void)
{
    uint8_t regs[TCA_REG_COUNT];
    char    line[TCA_DUMP_LINE];
    char    item[16];
    size_t  n = 0U;

    if ( NULL == s_dev )
    {
        KLOG_N(TAG, "no expander; nothing to dump");
        return;
    }
    if ( !regs_read(regs) )
    {
        KLOG_E(TAG, "dump: the expander did not answer");
        return;
    }
    KLOG_N(TAG, "input=0x%02X output=0x%02X polarity=0x%02X config=0x%02X",
           (unsigned)regs[TCA_REG_INPUT], (unsigned)regs[TCA_REG_OUTPUT],
           (unsigned)regs[TCA_REG_POLARITY], (unsigned)regs[TCA_REG_CONFIG]);

    line[0] = '\0';
    for ( uint32_t f = (uint32_t)KDRV_TCA_I0; (uint32_t)KDRV_TCA_FIELD_MAX > f; ++f )
    {
        const field_t * const row = &FIELDS[f];
        (void)snprintf(item, sizeof(item), "%s=%ld ", row->info.name,
                       (long)field_value(row, regs[row->reg]));
        dump_append(line, &n, item);
    }
    KLOG_N(TAG, "%s", line);
}

/* ---- The interrupt line -------------------------------------------------- */

/** @brief The owner's callback, and what it was handed. */
static void (*s_cb)(void *arg);
static void  *s_cb_arg;

/** @brief The sleep wake is armed: the interrupt withdraws it before the callback. */
static volatile bool s_wake;

/**
 * @brief INT went low. Interrupt context: withdraw the wake, then call the owner.
 *
 * @param arg Unused.
 */
static void int_isr(void *arg)
{
    (void)arg;
    if ( s_wake )
    {
        (void)khal_gpio_wake(KBSP_PIN_IOEXP_INT, false);
    }
    s_cb(s_cb_arg);
}

kode_status_t kdrv_tca6408a_int_attach(void (*cb)(void *arg), void *arg)
{
    if ( NULL == cb )
    {
        return KODE_DENIED;
    }
    /* No internal pull-up: R24 is on the board, and INT is open drain. */
    if ( khal_gpio_input(KBSP_PIN_IOEXP_INT, KHAL_GPIO_PULL_NONE) != KODE_OK )
    {
        return KODE_ERROR;
    }
    s_cb     = cb;
    s_cb_arg = arg;
    return khal_gpio_irq_attach(KBSP_PIN_IOEXP_INT, KHAL_GPIO_IRQ_LOW_LEVEL,
                                &int_isr, NULL);
}

void kdrv_tca6408a_int_rearm(void)
{
    if ( s_wake )
    {
        (void)khal_gpio_wake(KBSP_PIN_IOEXP_INT, true);
    }
    khal_gpio_irq_enable(KBSP_PIN_IOEXP_INT);
}

kode_status_t kdrv_tca6408a_sleep_wake(bool armed)
{
    /* The flag first, so the interrupt withdraws the wake from the moment it
     * could exist and rearm() never puts back one that was asked away; a wake
     * that would not arm is not believed armed. */
    s_wake = armed;
    kode_status_t const rc = khal_gpio_wake(KBSP_PIN_IOEXP_INT, armed);

    if ( KODE_OK != rc )
    {
        s_wake = false;
    }
    return rc;
}

bool kdrv_tca6408a_int_idle(void)
{
    return khal_gpio_level(KBSP_PIN_IOEXP_INT);
}
