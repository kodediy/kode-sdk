/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_init_gate.c
 * @brief The gate at the top of kode_init(): a boot from OFF goes on only on BOT.
 */
/* Named to sort after test_kdrv_tca6408a.c, whose first case needs the driver still down. */
#include <stdbool.h>
#include <string.h>

#include "unity.h"

#include "Mockkdrv_slot.h"
#include "Mockkhal_i2c.h"
#include "Mockkhal_power.h"

#include "fakes.h"
#include "kbsp.h"
#include "kdrv_led.h"
#include "kdrv_slot.h"
#include "kdrv_tca6408a.h"
#include "kode_gate.h"

#if !CONFIG_KODE_LOWPOWER
#error "test_kode_init_gate.c needs CONFIG_KODE_LOWPOWER: main/CMakeLists.txt spells it out"
#endif

#define REG_INPUT     0x00U
#define REG_OUTPUT    0x01U
#define REG_POLARITY  0x02U
#define REG_CONFIG    0x03U

/** @brief Every pin high: nothing pressed. */
#define LEVELS_IDLE     0xFFU
/** @brief BOT low, the rest up. */
#define LEVELS_BOT      0xFEU
/** @brief PAD_UP low, the rest up. */
#define LEVELS_PAD_UP   0xFDU
/** @brief BOT and PAD_UP both low: the one wake with two buttons down. */
#define LEVELS_BOT_AND_PAD  0xFCU

static uint8_t  s_regs[4];
static bool     s_bus_dead;
static unsigned s_reads;            /**< Register reads the chip answered.    */
static unsigned s_releases;         /**< khal_power_holds_release() calls.    */
static unsigned s_off_enter;        /**< khal_power_off_enter() calls.        */
static unsigned s_reads_at_release; /**< s_reads when the holds were released. */
static uint32_t s_note;             /**< What khal_power_off_note_set() left. */
static unsigned s_input_reads;      /**< Reads of the input register. */
static unsigned s_bot_from;         /**< The read of the input register BOT comes down on; 0 is never. */
static unsigned s_bot_until;        /**< The read it goes up on; 0 is never. */
static unsigned s_pad_from;         /**< The read a pad button comes down on; 0 is never. */
static unsigned s_dead_from;        /**< The read the bus dies on; 0 is never. */
static unsigned s_led_writes;       /**< Writes to the LED's channel-enable register. */
static unsigned s_led_lit;          /**< Of those, the ones that lit something. */
static uint8_t  s_led_last;         /**< What the last one wrote. */

static kode_status_t on_read(khal_i2c_dev_t dev, uint8_t reg, uint8_t *buf,
                             size_t len, int ncalls)
{
    (void)dev; (void)ncalls;
    if ( s_bus_dead || (REG_CONFIG < reg) || (1U != len) )
    {
        return KODE_ERROR;
    }
    buf[0] = s_regs[reg];
    if ( REG_INPUT == reg )
    {
        ++s_input_reads;
        if ( (0U != s_dead_from) && (s_input_reads >= s_dead_from) )
        {
            return KODE_ERROR;
        }
        if ( (0U != s_bot_from) && (s_input_reads >= s_bot_from)
            && ((0U == s_bot_until) || (s_input_reads < s_bot_until)) )
        {
            buf[0] = LEVELS_BOT;
        }
        if ( (0U != s_pad_from) && (s_input_reads >= s_pad_from) )
        {
            buf[0] = LEVELS_PAD_UP;
        }
    }
    ++s_reads;
    return KODE_OK;
}

/** @brief Keep what the LED was told; any other write just lands. */
static kode_status_t on_write(khal_i2c_dev_t dev, uint8_t reg, uint8_t val, int ncalls)
{
    (void)dev; (void)ncalls;
    if ( 0x04U == reg )
    {
        ++s_led_writes;
        s_led_last = val;
        if ( 0U != val )
        {
            ++s_led_lit;
        }
    }
    return KODE_OK;
}

static void on_note_set(uint32_t note, int ncalls)
{
    (void)ncalls;
    s_note = note;
}

static uint32_t on_note(int ncalls)
{
    (void)ncalls;
    return s_note;
}

static void on_release(int calls)
{
    (void)calls;
    s_reads_at_release = s_reads;
    ++s_releases;
}

static void on_off_enter(int calls)
{
    (void)calls;
    ++s_off_enter;
}

/**
 * @brief Open a case: a chip that reads @p input, a board that did or did not wake from OFF.
 * @param woke_from_off What khal_power_woke_from_off() answers.
 * @param input         The expander's input byte.
 */
static void gate_up(bool woke_from_off, uint8_t input)
{
    Mockkhal_i2c_Init();
    Mockkhal_power_Init();
    khal_i2c_init_IgnoreAndReturn(KODE_OK);
    khal_i2c_probe_IgnoreAndReturn(true);
    khal_i2c_add_IgnoreAndReturn(KODE_OK);
    khal_i2c_read_reg_StubWithCallback(&on_read);
    khal_power_woke_from_off_IgnoreAndReturn(woke_from_off);
    khal_power_holds_release_Stub(&on_release);
    khal_power_off_enter_Stub(&on_off_enter);
    khal_i2c_write_reg_StubWithCallback(&on_write);
    khal_power_off_note_set_Stub(&on_note_set);
    khal_power_off_note_Stub(&on_note);

    s_regs[REG_INPUT]    = input;
    s_regs[REG_OUTPUT]   = 0xFFU;
    s_regs[REG_POLARITY] = 0x00U;
    s_regs[REG_CONFIG]   = 0x3FU;
    s_bus_dead           = false;
    s_reads              = 0U;
    s_releases           = 0U;
    s_off_enter          = 0U;
    s_reads_at_release   = 0U;
    s_note               = 0U;
    s_input_reads        = 0U;
    s_bot_from           = 0U;
    s_bot_until          = 0U;
    s_pad_from           = 0U;
    s_dead_from          = 0U;
    s_led_writes         = 0U;
    s_led_lit            = 0U;
    s_led_last           = 0U;
    fake_klog_reset();
}

static void gate_down(void)
{
    /* The driver's state outlives the case, and the LED's owner has its own
     * cases further on: a dark chip goes to sleep, so it is left awake. */
    (void)kdrv_led_rgb(0U, 0U, 1U);
    Mockkhal_power_Destroy();
    Mockkhal_i2c_Destroy();
}

/* A cold boot has no OFF to leave: the holds are let go (they are harmless
 * when none were set) and the expander is not even asked. */
TEST_CASE("a cold boot goes on and lets go of the holds", "[kode_init_gate]")
{
    gate_up(false, LEVELS_PAD_UP);

    kode_boot_gate();

    TEST_ASSERT_EQUAL_UINT(1U, s_releases);
    TEST_ASSERT_EQUAL_UINT(0U, s_off_enter);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_reads,
        "a cold boot has no reason to read the expander before its owner does");
    gate_down();
}

TEST_CASE("a wake from OFF with BOT down goes on", "[kode_init_gate]")
{
    gate_up(true, LEVELS_BOT);

    kode_boot_gate();

    TEST_ASSERT_EQUAL_UINT(1U, s_releases);
    TEST_ASSERT_EQUAL_UINT(0U, s_off_enter);
    gate_down();
}

/* BOT is the one wake that counts, whatever else is down with it. */
TEST_CASE("a wake from OFF with BOT and a pad button down goes on",
          "[kode_init_gate]")
{
    gate_up(true, LEVELS_BOT_AND_PAD);

    kode_boot_gate();

    TEST_ASSERT_EQUAL_UINT(1U, s_releases);
    TEST_ASSERT_EQUAL_UINT(0U, s_off_enter);
    gate_down();
}

/* The pad pulls the same line BOT does. The board goes back with its pins
 * still held: releasing them first would pulse the C5's reset and the panel's
 * for a boot that never happens. */
TEST_CASE("a wake from OFF by a pad button goes back to OFF with the holds kept",
          "[kode_init_gate]")
{
    gate_up(true, LEVELS_PAD_UP);

    kode_boot_gate();

    TEST_ASSERT_EQUAL_UINT(1U, s_off_enter);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_releases,
        "the holds were let go on a boot that goes straight back to sleep");
    gate_down();
}

/* A wake with nothing down is a button released before the read: the same
 * answer, and the read has cleared the line it will sleep on. */
TEST_CASE("a wake from OFF with nothing down goes back to OFF",
          "[kode_init_gate]")
{
    gate_up(true, LEVELS_IDLE);

    kode_boot_gate();

    TEST_ASSERT_EQUAL_UINT(1U, s_off_enter);
    TEST_ASSERT_EQUAL_UINT(0U, s_releases);
    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(0U, s_reads,
        "the gate decided without reading the expander");
    gate_down();
}

/* The holds go after the read, not before: they are what keeps a boot that
 * is about to be refused from touching the C5 and the panel. */
TEST_CASE("the holds are released only after the expander was read",
          "[kode_init_gate]")
{
    gate_up(true, LEVELS_BOT);

    kode_boot_gate();

    TEST_ASSERT_EQUAL_UINT(1U, s_releases);
    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(0U, s_reads_at_release,
        "the holds were released before BOT was looked at");
    gate_down();
}

/* A board that cannot read its own buttons cannot tell BOT from a pad press,
 * and one that sleeps on that is a board nobody can wake. */
TEST_CASE("a wake from OFF with an expander that will not answer goes on",
          "[kode_init_gate]")
{
    gate_up(true, LEVELS_PAD_UP);
    s_bus_dead = true;

    kode_boot_gate();

    TEST_ASSERT_EQUAL_UINT(1U, s_releases);
    TEST_ASSERT_EQUAL_UINT(0U, s_off_enter);
    gate_down();
}

/* ---- A press too short to be read ---------------------------------------- */

/* A person's tap and a pad's tap look the same by now: the board answers both,
 * and only a board that was asked for it goes on. */
TEST_CASE("a press over before the read gets a pulse and goes back with the holds kept",
          "[kode_init_gate]")
{
    gate_up(true, LEVELS_IDLE);

    kode_boot_gate();

    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_led_lit, "no sign, and a person thinks it is dead");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0U, s_led_last, "the LED was left lit through OFF");
    TEST_ASSERT_EQUAL_UINT(1U, s_off_enter);
    TEST_ASSERT_EQUAL_UINT(0U, s_releases);
    gate_down();
}

TEST_CASE("BOT coming down in the second after the pulse goes on, dark and with the holds let go",
          "[kode_init_gate]")
{
    gate_up(true, LEVELS_IDLE);
    s_bot_from = 10U;

    kode_boot_gate();

    TEST_ASSERT_EQUAL_UINT(1U, s_releases);
    TEST_ASSERT_EQUAL_UINT(0U, s_off_enter);
    TEST_ASSERT_EQUAL_HEX8(0U, s_led_last);
    gate_down();
}

TEST_CASE("one read of BOT in the window is a glitch, not a press", "[kode_init_gate]")
{
    gate_up(true, LEVELS_IDLE);
    s_bot_from  = 10U;
    s_bot_until = 11U;

    kode_boot_gate();

    TEST_ASSERT_EQUAL_UINT(0U, s_releases);
    TEST_ASSERT_EQUAL_UINT(1U, s_off_enter);
    gate_down();
}

/* Asleep on a quiet line, a finger that stays there would never be heard. */
TEST_CASE("BOT down on the window's very last look goes on", "[kode_init_gate]")
{
    gate_up(true, LEVELS_IDLE);
    s_bot_from = 51U;   /* The wake's own read, then fifty looks. */

    kode_boot_gate();

    TEST_ASSERT_EQUAL_UINT(1U, s_releases);
    TEST_ASSERT_EQUAL_UINT(0U, s_off_enter);
    gate_down();
}

TEST_CASE("an expander that stops answering in the window goes on", "[kode_init_gate]")
{
    gate_up(true, LEVELS_IDLE);
    s_dead_from = 5U;

    kode_boot_gate();

    TEST_ASSERT_EQUAL_UINT(1U, s_releases);
    TEST_ASSERT_EQUAL_UINT(0U, s_off_enter);
    TEST_ASSERT_EQUAL_HEX8(0U, s_led_last);
    gate_down();
}

/* A pocket: no light, no second awake, on the press or on its release. */
TEST_CASE("a pad button still down gets no light and no window, nor does its release",
          "[kode_init_gate]")
{
    gate_up(true, LEVELS_PAD_UP);

    kode_boot_gate();

    TEST_ASSERT_EQUAL_UINT(1U, s_off_enter);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_led_lit, "a press in a pocket lit the LED");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_input_reads, "a press in a pocket kept the board awake");

    s_regs[REG_INPUT] = LEVELS_IDLE;
    s_input_reads     = 0U;
    kode_boot_gate();

    TEST_ASSERT_EQUAL_UINT(2U, s_off_enter);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_led_lit, "the release of that press lit the LED");
    TEST_ASSERT_EQUAL_UINT(1U, s_input_reads);

    /* And it was one wake's silence: the next short press answers. */
    s_input_reads = 0U;
    kode_boot_gate();

    TEST_ASSERT_EQUAL_UINT(1U, s_led_lit);
    gate_down();
}

TEST_CASE("a pad button down in the window ends it, and its release is silent", "[kode_init_gate]")
{
    gate_up(true, LEVELS_IDLE);
    s_pad_from = 5U;

    kode_boot_gate();

    TEST_ASSERT_EQUAL_UINT(1U, s_off_enter);
    TEST_ASSERT_EQUAL_UINT(5U, s_input_reads);
    TEST_ASSERT_EQUAL_HEX8(0U, s_led_last);

    s_pad_from        = 0U;
    s_led_lit         = 0U;
    s_input_reads     = 0U;
    kode_boot_gate();

    TEST_ASSERT_EQUAL_UINT(2U, s_off_enter);
    TEST_ASSERT_EQUAL_UINT(0U, s_led_lit);
    gate_down();
}

/* ---- A cell that cannot carry the boot ------------------------------------ */

#define REG_BQ_CONV     0x02U
#define REG_BQ_BATFET   0x09U
#define REG_BQ_STATUS   0x0BU
#define REG_BQ_ADC      0x0EU
#define REG_BQ_BATV_I   0U
#define REG_BQ_VBUS_I   3U
#define REG_CW_VCELL    0x02U
#define REG_CW_READY    0xA7U
#define CW_READY_BITS   0x0CU
#define BQ_PG_BIT       0x04U
#define BQ_VBUS_SHIFT   5U
#define BQ_CHRG_FAST    0x10U
#define BQ_CONV_BIT     0x80U
#define BQ_BATFET_DIS   0x20U
#define BQ_BATV_BASE_MV 2304U
#define BQ_BATV_STEP_MV 20U
#define CW_MV_TO_CODE(mv)  ((uint16_t)(((uint32_t)(mv) * 16U) / 5U))

/** @brief The two power chips, as the driver reads them; the cases set what they say. */
typedef struct {
    bool     gauge_dead;
    bool     gauge_unready;
    uint16_t gauge_mv;
    bool     pmic_dead;
    uint8_t  status;       /**< REG0B. */
    bool     vbus_gd;      /**< REG11's top bit. */
    uint16_t batv_mv;      /**< What a finished conversion reads; 0 is code 0, the ADC at rest. */
    bool     conv_sticks;  /**< CONV_START never clears. */
    bool     conv_ran;     /**< Written and finished. */
    bool     plug_in_conv; /**< A cable comes in during the conversion. */
    unsigned conv_starts;
    unsigned ship_writes;
    unsigned batfet_backs;
    uint8_t  batfet;
    unsigned red_on;       /**< Writes that lit the LED's red sink. */
    unsigned releases_at_ship;
    unsigned lit_at_ship;  /**< LED writes that lit anything, by the ship write. */
} cell_chips_t;

static cell_chips_t s_cell;

/* A driver's register, told apart by what is asked: the gauge's VCELL is the only
 * two-byte read at 0x02, its readiness the only one at 0xA7, and the PMIC's own
 * registers do not overlap the expander's four. */
static kode_status_t on_read_power(khal_i2c_dev_t dev, uint8_t reg, uint8_t *buf,
                                   size_t len, int ncalls)
{
    (void)dev; (void)ncalls;
    if ( REG_INPUT == reg )
    {
        buf[0] = s_regs[REG_INPUT];
        return KODE_OK;
    }
    if ( (REG_CW_VCELL == reg) && (2U == len) )
    {
        uint16_t const code = CW_MV_TO_CODE(s_cell.gauge_mv);

        if ( s_cell.gauge_dead )
        {
            return KODE_ERROR;
        }
        buf[0] = (uint8_t)(code >> 8);
        buf[1] = (uint8_t)(code & 0xFFU);
        return KODE_OK;
    }
    if ( REG_CW_READY == reg )
    {
        if ( s_cell.gauge_dead )
        {
            return KODE_ERROR;
        }
        buf[0] = s_cell.gauge_unready ? 0U : CW_READY_BITS;
        return KODE_OK;
    }
    if ( s_cell.pmic_dead )
    {
        return KODE_ERROR;
    }
    if ( REG_BQ_STATUS == reg )
    {
        buf[0] = s_cell.status;
        return KODE_OK;
    }
    if ( REG_BQ_CONV == reg )
    {
        buf[0] = (s_cell.conv_sticks && (0U < s_cell.conv_starts)) ? BQ_CONV_BIT : 0U;
        if ( (0U < s_cell.conv_starts) && (!s_cell.conv_sticks) )
        {
            s_cell.conv_ran = true;
            if ( s_cell.plug_in_conv )
            {
                s_cell.status |= BQ_PG_BIT;
            }
        }
        return KODE_OK;
    }
    if ( REG_BQ_BATFET == reg )
    {
        buf[0] = s_cell.batfet;
        return KODE_OK;
    }
    if ( (REG_BQ_ADC == reg) && (5U == len) )
    {
        (void)memset(buf, 0, len);
        if ( s_cell.conv_ran && (BQ_BATV_BASE_MV <= s_cell.batv_mv) )
        {
            buf[REG_BQ_BATV_I] = (uint8_t)((s_cell.batv_mv - BQ_BATV_BASE_MV) / BQ_BATV_STEP_MV);
        }
        buf[REG_BQ_VBUS_I] = s_cell.vbus_gd ? 0x80U : 0U;
        return KODE_OK;
    }
    return KODE_ERROR;
}

static kode_status_t on_write_power(khal_i2c_dev_t dev, uint8_t reg, uint8_t val, int ncalls)
{
    (void)dev; (void)ncalls;
    if ( s_cell.pmic_dead )
    {
        return KODE_ERROR;
    }
    if ( (REG_BQ_CONV == reg) && (0U != (val & BQ_CONV_BIT)) )
    {
        ++s_cell.conv_starts;
    }
    if ( REG_BQ_BATFET == reg )
    {
        if ( 0U != (val & BQ_BATFET_DIS) )
        {
            ++s_cell.ship_writes;
            s_cell.releases_at_ship = s_releases;
            s_cell.lit_at_ship      = s_cell.red_on;
        }
        else if ( 0U != (s_cell.batfet & BQ_BATFET_DIS) )
        {
            ++s_cell.batfet_backs;
        }
        else
        {
            /* Not a change of the battery path. */
        }
        s_cell.batfet = val;
    }
    /* KTD2026's OUT3, the red sink; shares a number with no PMIC write above. */
    if ( (0x04U == reg) && (0U != (val & 0x30U)) )
    {
        ++s_cell.red_on;
    }
    return KODE_OK;
}

/**
 * @brief Open a cold boot on battery: no cable, a gauge and a PMIC that agree on @p mv.
 * @param mv What both chips read.
 */
static void cell_up(uint16_t mv)
{
    gate_up(false, LEVELS_IDLE);
    (void)memset(&s_cell, 0, sizeof(s_cell));
    s_cell.gauge_mv = mv;
    s_cell.batv_mv  = mv;
    s_cell.batfet   = 0x44U;
    khal_i2c_read_reg_StubWithCallback(&on_read_power);
    khal_i2c_write_reg_StubWithCallback(&on_write_power);
    Mockkdrv_slot_Init();
    kdrv_reset_reason_IgnoreAndReturn((int)KDRV_RESET_POWERON);
}

static void cell_down(void)
{
    Mockkdrv_slot_Destroy();
    gate_down();
}

/** @brief Ran the boot, and the board was never shipped. */
static void assert_booted(void)
{
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_cell.ship_writes, "a board that should boot was shipped");
    TEST_ASSERT_EQUAL_UINT(1U, s_releases);
    TEST_ASSERT_EQUAL_UINT(0U, s_off_enter);
}

TEST_CASE("a cell under the bar with no cable ships after three red flashes, and asks for a conversion",
          "[kode_init_gate]")
{
    cell_up(3200U);

    kode_boot_gate();

    TEST_ASSERT_EQUAL_UINT(1U, s_cell.conv_starts);
    TEST_ASSERT_EQUAL_UINT(1U, s_cell.ship_writes);
    TEST_ASSERT_EQUAL_UINT(3U, s_cell.red_on);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_cell.releases_at_ship,
        "the holds were released before the board was shipped");
    cell_down();
}

/* The healthy path has to stay nearly free: the first frame is waiting. */
TEST_CASE("a healthy cell boots on the gauge's word alone", "[kode_init_gate]")
{
    cell_up(3900U);

    kode_boot_gate();

    assert_booted();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_cell.conv_starts,
        "a healthy boot paid for a conversion of the PMIC's ADC");
    cell_down();
}

TEST_CASE("a cell over the bar that has not browned out boots", "[kode_init_gate]")
{
    cell_up(3350U);

    kode_boot_gate();

    assert_booted();
    cell_down();
}

/* The loop: a cell that rests over 3.3 V and browns out at the peak. */
TEST_CASE("the same cell right after a brownout ships", "[kode_init_gate]")
{
    cell_up(3350U);
    Mockkdrv_slot_Destroy();
    Mockkdrv_slot_Init();
    kdrv_reset_reason_IgnoreAndReturn((int)KDRV_RESET_BROWNOUT);

    kode_boot_gate();

    TEST_ASSERT_EQUAL_UINT(1U, s_cell.ship_writes);
    cell_down();
}

TEST_CASE("a brownout with a healthy cell boots: the bar is not the only witness",
          "[kode_init_gate]")
{
    cell_up(3700U);
    Mockkdrv_slot_Destroy();
    Mockkdrv_slot_Init();
    kdrv_reset_reason_IgnoreAndReturn((int)KDRV_RESET_BROWNOUT);

    kode_boot_gate();

    assert_booted();
    cell_down();
}

TEST_CASE("a low cell with a cable always boots, however the PMIC says it", "[kode_init_gate]")
{
    static const uint8_t statuses[] = {
        BQ_PG_BIT,                                     /* power good */
        (uint8_t)(1U << BQ_VBUS_SHIFT),                /* an input was detected */
        (uint8_t)(7U << BQ_VBUS_SHIFT),                /* its own boost */
        BQ_CHRG_FAST,                                  /* a charge running */
    };

    for ( size_t i = 0U; (sizeof(statuses) / sizeof(statuses[0])) > i; ++i )
    {
        cell_up(3100U);
        s_cell.status = statuses[i];

        kode_boot_gate();

        assert_booted();
        TEST_ASSERT_EQUAL_UINT(0U, s_cell.conv_starts);
        cell_down();
    }
}

TEST_CASE("VBUS_GD alone is a cable too", "[kode_init_gate]")
{
    cell_up(3100U);
    s_cell.vbus_gd = true;

    kode_boot_gate();

    assert_booted();
    cell_down();
}

TEST_CASE("a cable that comes in during the conversion boots", "[kode_init_gate]")
{
    cell_up(3100U);
    s_cell.plug_in_conv = true;

    kode_boot_gate();

    assert_booted();
    cell_down();
}

/* A bad reading must never ship a healthy board: every way of not knowing boots. */
TEST_CASE("a gauge that is dead, not ready or reads nothing boots", "[kode_init_gate]")
{
    cell_up(3100U);
    s_cell.gauge_dead = true;
    kode_boot_gate();
    assert_booted();
    cell_down();

    cell_up(3100U);
    s_cell.gauge_unready = true;
    kode_boot_gate();
    assert_booted();
    cell_down();

    /* VCELL at 0 is a gauge not yet running. */
    cell_up(0U);
    s_cell.batv_mv = 3100U;
    kode_boot_gate();
    assert_booted();
    cell_down();
}

TEST_CASE("a low gauge with a PMIC that does not answer boots", "[kode_init_gate]")
{
    cell_up(3100U);
    s_cell.pmic_dead = true;

    kode_boot_gate();

    assert_booted();
    cell_down();
}

TEST_CASE("a low gauge with a healthy PMIC boots: both must agree", "[kode_init_gate]")
{
    cell_up(3100U);
    s_cell.batv_mv = 3800U;

    kode_boot_gate();

    assert_booted();
    TEST_ASSERT_EQUAL_UINT(1U, s_cell.conv_starts);
    cell_down();
}

/* 2304 mV is what a flat cell and an ADC that never ran both read like. */
TEST_CASE("a conversion that never ends, or the ADC's code 0, is not a flat cell",
          "[kode_init_gate]")
{
    cell_up(3100U);
    s_cell.conv_sticks = true;
    kode_boot_gate();
    assert_booted();
    cell_down();

    cell_up(3100U);
    s_cell.batv_mv = 0U;
    kode_boot_gate();
    assert_booted();
    cell_down();
}

TEST_CASE("a board still up after the ship write gets its battery back and boots",
          "[kode_init_gate]")
{
    cell_up(3100U);

    kode_boot_gate();

    TEST_ASSERT_EQUAL_UINT(1U, s_cell.ship_writes);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_cell.batfet_backs,
        "a cable that came in late was left with the battery path open");
    TEST_ASSERT_EQUAL_UINT(1U, s_releases);
    cell_down();
}

TEST_CASE("a short press on a flat cell ships before the green pulse", "[kode_init_gate]")
{
    cell_up(3100U);
    Mockkhal_power_Destroy();
    Mockkhal_power_Init();
    khal_power_woke_from_off_IgnoreAndReturn(true);
    khal_power_holds_release_Stub(&on_release);
    khal_power_off_enter_Stub(&on_off_enter);
    khal_power_off_note_set_Stub(&on_note_set);
    khal_power_off_note_Stub(&on_note);

    kode_boot_gate();

    /* Past the ship write only because the host has no BATFET to open. */
    TEST_ASSERT_EQUAL_UINT(1U, s_cell.ship_writes);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(3U, s_cell.lit_at_ship,
        "something but the three red flashes came before the board was shipped");
    cell_down();
}
