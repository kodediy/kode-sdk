/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_power_ao.c
 * @brief AO_Power: two chip drivers composed into one reading.
 */
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "qpc.h"
#include "Mockkhal_i2c.h"

#include "ao_spy.h"
#include "config_support.h"
#include "fakes.h"
#include "kaos.h"
#include "kbsp.h"
#include "kbsp_owners.h"
#include "kdrv_bq25896.h"
#include "kdrv_cw2217.h"
#include "kdrv_clocks.h"
#include "khal_nvs.h"
#include "khal_once.h"
#include "kode.h"
#include "kode_standard_ao.h"
#include "kode_sys.h"
#include "kode_sys_priv.h"
#include "kqp.h"
#include "kqp_config.h"
#include "kset.h"
#include "lowpower_cases.h"
#include "qf_support.h"

#define PMIC   0U
#define GAUGE  1U

#define READING_FILL     0xA5U   /**< What a reading is filled with to see it untouched.  */
#define POWER_FILL_MAX   64U     /**< Posts that surely fill AO_Power's queue.            */
#define POLLS_TO_SETTLE  60U     /**< Polls that carry the percentage to a new cell.      */
#define CELL_FULL_MV     4200U   /**< A full cell: 100 %.                                 */
#define CELL_NEAR_FULL_MV 4100U  /**< 90 % on the curve: well above the low mark's re-arm. */
#define CELL_LOW_MV      3600U   /**< 11 % on the curve: below the low mark.              */
#define REG0B_OFF_USB    0x00U   /**< REG0B with no adapter and no charge.                */
#define REG0B_ON_USB     0x14U   /**< REG0B as the register file starts: adapter, fast charge. */

#define BQ_REG02_ADC     0x02U
#define BQ_CONV_START    0x80U   /* REG02[7]: start one conversion        */
#define BQ_CONV_RATE     0x40U   /* REG02[6]: convert every second        */
#define BQ_REG03_CTRL    0x03U
#define BQ_REG04_ICHG    0x04U
#define BQ_REG05_TERM    0x05U
#define BQ_REG07_TIMERS  0x07U
#define BQ_REG09_BATFET  0x09U
#define BQ_REG0B_STATUS  0x0BU
#define BQ_REG0C_FAULT   0x0CU
#define BQ_REG0E_BATV    0x0EU
#define BQ_REG14_ID      0x14U

#define CW_VCELL_H       0x02U
#define CW_CONFIG        0x08U
#define CW_CURRENT_H     0x0EU
#define CW_IC_STATE      0xA7U

/* ---- The two chips, as a register file ------------------------------------ */

static uint8_t        s_regs[2][256];
static khal_i2c_dev_t s_devs[2];
static bool           s_bus_dead;
static bool s_reset_sticks;   /* a chip whose REG_RST never clears */

/* A single PMIC register whose writes fail while every read still works.
 * `s_bus_dead` cannot express that: it fails reads too, so a guard that has
 * to read before it acts never runs at all. This is the failure the guards
 * are about - the bus answers, one write does not - and 0xFFFF is "none". */
#define NO_WRITE_FAIL  0xFFFFU
static uint16_t s_write_fails_reg = NO_WRITE_FAIL;

/** @brief One write the register file took, for the cases that care about order. */
typedef struct {
    uint8_t chip;
    uint8_t reg;
    uint8_t val;
} write_t;

static write_t  s_writes_log[64];
static unsigned s_nwrites;

/** @brief How many log lines had been written when the sleep register was hit. */
static unsigned s_lines_at_write;
static unsigned s_batfet_writes;

/* AO_Power's initial transition talks to the PMIC first, so the first handle is the PMIC's. */
/** @brief Which chip a handle belongs to, assigned in order of first use. */
static unsigned chip_of(khal_i2c_dev_t dev)
{
    for ( unsigned i = 0U; 2U > i; ++i )
    {
        if ( s_devs[i] == dev )
        {
            return i;
        }
    }
    for ( unsigned i = 0U; 2U > i; ++i )
    {
        if ( NULL == s_devs[i] )
        {
            s_devs[i] = dev;
            return i;
        }
    }
    return GAUGE;
}

/** @brief The PMIC's reset values, plus an ADC that has converted a 3.904 V cell on 5 V of USB. */
static void pmic_defaults(void)
{
    static const uint8_t d[0x15] = {
        0x08U, 0x06U, 0x11U, 0x1AU, 0x20U, 0x13U, 0x5EU, 0x9DU, 0x03U, 0x44U,
        0x73U, 0x14U, 0x00U, 0x12U, 0x50U, 0x50U, 0x4CU, 0x98U, 0x00U, 0x00U,
        0x02U,
    };
    (void)memset(s_regs[PMIC], 0, sizeof(s_regs[PMIC]));
    (void)memcpy(s_regs[PMIC], d, sizeof(d));
}

/**
 * @brief What the last firmware left in the PMIC: HIZ on, charging off, and
 *        the 256 mA a parked bench board charges at (Kode-Dot-Val bq_park).
 */
static void pmic_leftovers(void)
{
    s_regs[PMIC][0x00U] |= 0x80U;              /* EN_HIZ                   */
    s_regs[PMIC][0x03U] &= (uint8_t)~0x10U;    /* CHG_CONFIG off           */
    s_regs[PMIC][BQ_REG04_ICHG] = 0x04U;       /* ICHG = 256 mA            */
    s_regs[PMIC][0x03U] |= 0x20U;              /* OTG_CONFIG left set      */
}

/** @brief A gauge that is awake, with no profile, reading 3720 mV and no current. */
static void gauge_defaults(void)
{
    (void)memset(s_regs[GAUGE], 0, sizeof(s_regs[GAUGE]));
    s_regs[GAUGE][0x00U] = 0xA0U;
    s_regs[GAUGE][0x02U] = 0x2EU;
    s_regs[GAUGE][0x03U] = 0x80U;
    s_regs[GAUGE][0x04U] = 0xFEU;
    s_regs[GAUGE][0x05U] = 0xB3U;
    s_regs[GAUGE][0x08U] = 0xF0U;
    s_regs[GAUGE][0x0AU] = 0x40U;
    s_regs[GAUGE][0x0BU] = 0x14U;
    s_regs[GAUGE][0x0CU] = 0xAAU;
    s_regs[GAUGE][0x0DU] = 0x50U;
    s_regs[GAUGE][CW_IC_STATE] = 0x0CU;
    s_regs[GAUGE][0xABU] = 0x4AU;
}

static kode_status_t on_read(khal_i2c_dev_t dev, uint8_t reg, uint8_t *buf,
                             size_t len, int calls)
{
    (void)calls;
    if ( s_bus_dead )
    {
        return KODE_ERROR;
    }
    (void)memcpy(buf, &s_regs[chip_of(dev)][reg], len);
    return KODE_OK;
}

static kode_status_t on_write(khal_i2c_dev_t dev, uint8_t reg, uint8_t val,
                              int calls)
{
    unsigned const chip = chip_of(dev);

    (void)calls;
    if ( s_bus_dead )
    {
        return KODE_ERROR;
    }
    if ( (PMIC == chip) && ((uint16_t)reg == s_write_fails_reg) )
    {
        return KODE_ERROR;
    }
    if ( 64U > s_nwrites )
    {
        s_writes_log[s_nwrites].chip = (uint8_t)chip;
        s_writes_log[s_nwrites].reg  = reg;
        s_writes_log[s_nwrites].val  = val;
        ++s_nwrites;
    }
    if ( (PMIC == chip) && (BQ_REG09_BATFET == reg) )
    {
        s_lines_at_write = fake_klog_count();
        ++s_batfet_writes;
    }
    /* REG_RST: everything back to its defaults, and the bit clears itself. */
    if ( (PMIC == chip) && (BQ_REG14_ID == reg) && ((val & 0x80U) != 0U) )
    {
        if ( s_reset_sticks )
        {
            s_regs[chip][reg] = val;
        }
        else
        {
            pmic_defaults();
        }
        return KODE_OK;
    }
    s_regs[chip][reg] = val;
    return KODE_OK;
}

/** @brief The index in the write log of the first write of @p reg to @p chip, or -1. */
static int first_write_of(unsigned chip, uint8_t reg)
{
    for ( unsigned i = 0U; i < s_nwrites; ++i )
    {
        if ( (s_writes_log[i].chip == chip) && (s_writes_log[i].reg == reg) )
        {
            return (int)i;
        }
    }
    return -1;
}

/**
 * @brief Bring AO_Power up, once for the whole binary, over the register file.
 *
 * Once, not once per case, and for a reason with teeth. Power_ctor() runs
 * QTimeEvt_ctorX() over `poll`, and Power_polling's entry arms it - but this
 * AO has exactly one state, so it never exits and the timer is never
 * disarmed. Re-constructing an armed QTimeEvt rewrites the link QF's list of
 * armed timers is holding, and re-arming it links it in again: after a few
 * cases the list runs through the same object repeatedly and ONE tick posts
 * dozens of times. It surfaces as a full queue in an unrelated file - the
 * measured failure was `test_storage_ao` dying with 32 events waiting on a
 * queue nobody had posted to.
 *
 * The register file persists across cases on purpose: the chips keep their
 * state between polls on the board too. A case that needs a particular
 * register sets it before dispatching.
 */
static void power_up_once(void)
{
    Mockkhal_i2c_Init();
    khal_i2c_init_IgnoreAndReturn(KODE_OK);
    khal_i2c_probe_IgnoreAndReturn(true);
    khal_i2c_add_IgnoreAndReturn(KODE_OK);
    khal_i2c_read_reg_StubWithCallback(&on_read);
    khal_i2c_write_reg_StubWithCallback(&on_write);

    qf_support_init();
    (void)ao_spy();
    ao_spy_drain();
    ao_spy_subscribe(POWER_EVT_SIG);
    ao_spy_subscribe(POWER_FIELD_SIG);
    ao_spy_subscribe(POWER_CTRL_SIG);

    s_bus_dead       = false;
    s_nwrites        = 0U;
    s_lines_at_write = 0U;
    s_batfet_writes  = 0U;
    s_reset_sticks   = false;
    s_write_fails_reg = NO_WRITE_FAIL;
    fake_klog_reset();

    /* qf_support_claim_ao() rather than a flag of this file's own:
     * test_power_led.c brings the same Active Object up, and whichever of the
     * two the linker puts first has to be the one that constructs it. */
    if ( qf_support_claim_ao(AO_Power) )
    {
        pmic_defaults();
        pmic_leftovers();
        gauge_defaults();
        Power_ctor();
        /* The ship arm is posted with a margin, which QP answers from the
         * queue it lays out in the AO's own osObject. Another file's fake of
         * a neighbour may have given AO_Power a queue of the heap's, which QP
         * reads as always full: put the proper one in its place. */
        if ( (NULL != AO_Power->eQueue)
            && ((void *)AO_Power->eQueue != (void *)&AO_Power->osObject.queue) )
        {
            const QEvt *stale = NULL;

            while ( xQueueReceive(AO_Power->eQueue, &stale, 0U) == pdTRUE )
            {
                QF_gc(stale);
            }
            AO_Power->eQueue = NULL;
        }
        qf_support_give_queue(AO_Power);

        /* test_console_power.c's own console_up() says why: the console's
         * verbs post real events into this same queue, and its last case
         * (bat_info) leaves one behind for whoever pumps this queue first.
         * Nothing here ever did until the reset wait below, so drain it once
         * rather than let an unrelated file's leftover surface as a stray
         * POWER_EVT_SIG in the middle of a reset's poll count. */
        {
            const QEvt *stray;
            while ( xQueueReceive(AO_Power->eQueue, &stray, 0U) == pdTRUE )
            {
                QF_gc(stray);
            }
        }

        AO_Power->prio  = KODE_PRIO_POWER;
        AO_Power->pthre = KODE_PRIO_POWER;
        QActive_register_(AO_Power);
        QASM_INIT(AO_Power, (void *)0, 0U);

        /* The poll timer is armed for the rest of the run and nothing here
         * dispatches it. Say so, or every later tick in the binary posts to
         * a queue that only fills. */
        qf_support_background(AO_Power);
    }
    /* Harmless to repeat, and it has to be said whichever file constructed
     * the AO: the timer belongs to the AO, not to the file that armed it. */
    qf_support_background(AO_Power);
    qf_support_give_queue(AO_Power);
    ao_spy_drain();
}

/** @brief Dispatch one poll and take the reading it published. */
static const PowerEvt *poll_once(void)
{
    static const QEvt poll = QEVT_INITIALIZER(POWER_POLL_SIG);

    ao_spy_drain();
    QASM_DISPATCH(AO_Power, &poll, 0U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, ao_spy_count(), "one poll, one reading");
    return (const PowerEvt *)ao_spy_take();
}

/** @brief Dispatch one field request and take its answer. */
static const PowerFieldEvt *field_req(uint8_t chip, uint8_t op, uint16_t field,
                                      int32_t value)
{
    PowerFieldReqEvt req;

    req.super = (QEvt)QEVT_INITIALIZER(POWER_FIELD_REQ_SIG);
    req.chip  = chip;
    req.op    = op;
    req.field = field;
    req.value = value;
    ao_spy_drain();
    QASM_DISPATCH(AO_Power, &req.super, 0U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, ao_spy_count(), "every request is answered");
    return (const PowerFieldEvt *)ao_spy_take();
}

/** @brief Dispatch one control request and take its answer. */
static const PowerCtrlEvt *ctrl_req(uint8_t op, int32_t value)
{
    PowerCtrlReqEvt req;

    req.super = (QEvt)QEVT_INITIALIZER(POWER_CTRL_REQ_SIG);
    req.op    = op;
    req.value = value;
    ao_spy_drain();
    QASM_DISPATCH(AO_Power, &req.super, 0U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, ao_spy_count(), "every control request is answered");
    return (const PowerCtrlEvt *)ao_spy_take();
}

/** @brief Dispatch one control request that must NOT be answered yet. */
static void ctrl_req_pending(uint8_t op, int32_t value)
{
    PowerCtrlReqEvt req;

    req.super = (QEvt)QEVT_INITIALIZER(POWER_CTRL_REQ_SIG);
    req.op    = op;
    req.value = value;
    ao_spy_drain();
    QASM_DISPATCH(AO_Power, &req.super, 0U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "answered before the wait");
}

/** @brief The wait is over: dispatch the tick and take the answer it publishes. */
static const PowerCtrlEvt *tick(void)
{
    static const QEvt t = QEVT_INITIALIZER(POWER_CTRL_TICK_SIG);

    ao_spy_drain();
    QASM_DISPATCH(AO_Power, &t, 0U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, ao_spy_count(), "the tick answers");
    return (const PowerCtrlEvt *)ao_spy_take();
}

/** @brief A cell reading for the boost's floors: the gauge is ready and reads @p mv. */
static void cell_reads(uint16_t mv)
{
    uint16_t const code = (uint16_t)(((uint32_t)mv * 10000U) / 3125U);   /* 312.5 uV steps */

    s_regs[GAUGE][CW_IC_STATE]     = 0x0CU;
    s_regs[GAUGE][CW_VCELL_H]      = (uint8_t)(code >> 8);
    s_regs[GAUGE][CW_VCELL_H + 1U] = (uint8_t)(code & 0xFFU);
}

/** @brief Does any captured log line contain @p text? */
static bool logged(const char *text)
{
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, text) != NULL) )
        {
            return true;
        }
    }
    return false;
}

/* ---- Bring-up -------------------------------------------------------------- */

/* The chips are the drivers'; the policy is the owner's. Watchdog first,
 * because with it running anything written after it reverts in forty
 * seconds and the symptom is a setting that "did not take" long after the
 * code that set it. Then the ADC, or every voltage reads as the bottom of its
 * scale, which is what a flat cell reads like. */
TEST_CASE("AO_Power owns the chips and applies its policy: watchdog off, then the ADC",
          "[power_ao]")
{
    power_up_once();

    /* Nothing has polled yet: the owner has no reading to give, and says so. */
    {
        power_reading_t    r;
        power_reading_t    before;
        kode_sys_battery_t door;
        kode_sys_battery_t door_before;

        (void)memset(&r, READING_FILL, sizeof(r));
        (void)memcpy(&before, &r, sizeof(r));
        (void)memset(&door, READING_FILL, sizeof(door));
        (void)memcpy(&door_before, &door, sizeof(door));
        TEST_ASSERT_FALSE(power_reading(&r));
        TEST_ASSERT_EQUAL_INT(0, memcmp(&before, &r, sizeof(r)));
        TEST_ASSERT_EQUAL(KODE_DENIED, kode_sys_battery_read(&door));
        TEST_ASSERT_EQUAL_INT(0, memcmp(&door_before, &door, sizeof(door)));
    }

    TEST_ASSERT_EQUAL_STRING("AO_Power", kbsp_owner_name(KBSP_RES_PMIC));
    TEST_ASSERT_EQUAL_STRING("AO_Power", kbsp_owner_name(KBSP_RES_GAUGE));

    TEST_ASSERT_EQUAL_HEX8(0x00U, s_regs[PMIC][BQ_REG07_TIMERS] & 0x30U);
    /* One-shot, and one conversion started: in continuous mode REGN stays up
     * and the TS divider draws ~0.3 mA from the cell. */
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_regs[PMIC][BQ_REG02_ADC] & BQ_CONV_RATE);
    TEST_ASSERT_EQUAL_HEX8(BQ_CONV_START, s_regs[PMIC][BQ_REG02_ADC] & BQ_CONV_START);

    int const wd   = first_write_of(PMIC, BQ_REG07_TIMERS);
    int const adc  = first_write_of(PMIC, BQ_REG02_ADC);
    int const ichg = first_write_of(PMIC, BQ_REG04_ICHG);
    TEST_ASSERT_TRUE_MESSAGE(0 <= wd, "the watchdog was never written");
    TEST_ASSERT_TRUE_MESSAGE(adc > wd, "the ADC was started before the watchdog was silenced");
    TEST_ASSERT_TRUE_MESSAGE(ichg > wd, "the profile was written before the watchdog was silenced, so it would not last");

    /* And the gauge was woken, not waited for. */
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_regs[GAUGE][CW_CONFIG]);

    Mockkhal_i2c_Destroy();
}

/* The chip keeps its registers across a reflash and, with the watchdog off,
 * nothing ever puts them back: whatever the last firmware or the console
 * left in ICHG is what the cell would be charged with, for as long as the
 * battery stays plugged. So the profile is the cell's (kbsp_cell.h) and the
 * owner states all of it every boot, with the two bits that make it matter:
 * charging on, the input out of HIZ. The register file started with the
 * opposite of every one of these. */
TEST_CASE("the cell's charge profile is written into the PMIC, charging enabled",
          "[power_ao]")
{
    int32_t v = -1;

    power_up_once();

    TEST_ASSERT_EQUAL(KODE_OK, kdrv_bq25896_get(KDRV_BQ_ICHG_MA, &v));
    TEST_ASSERT_EQUAL_INT32((int32_t)KBSP_CELL_ICHG_MA, v);
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_bq25896_get(KDRV_BQ_ITERM_MA, &v));
    TEST_ASSERT_EQUAL_INT32((int32_t)KBSP_CELL_ITERM_MA, v);
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_bq25896_get(KDRV_BQ_IPRECHG_MA, &v));
    TEST_ASSERT_EQUAL_INT32((int32_t)KBSP_CELL_IPRECHG_MA, v);
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_bq25896_get(KDRV_BQ_VREG_MV, &v));
    TEST_ASSERT_EQUAL_INT32((int32_t)KBSP_CELL_VREG_MV, v);
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_bq25896_get(KDRV_BQ_CHG_CONFIG, &v));
    TEST_ASSERT_EQUAL_INT32_MESSAGE(1, v, "charging was left off");
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_bq25896_get(KDRV_BQ_EN_HIZ, &v));
    TEST_ASSERT_EQUAL_INT32_MESSAGE(0, v, "the input was left in HIZ");

    /* The boost is off at boot and limited to what the pack can give before
     * anyone can switch it on: 0.5 A, and a hot stop at the pack's 55 C. */
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_bq25896_get(KDRV_BQ_OTG_CONFIG, &v));
    TEST_ASSERT_EQUAL_INT32_MESSAGE(0, v, "a leftover OTG_CONFIG would boost at boot");
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_bq25896_get(KDRV_BQ_BOOST_LIM, &v));
    TEST_ASSERT_EQUAL_INT32((int32_t)KBSP_CELL_BOOST_LIM_CODE, v);
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_bq25896_get(KDRV_BQ_BHOT, &v));
    TEST_ASSERT_EQUAL_INT32((int32_t)KBSP_CELL_BOOST_HOT_CODE, v);

    Mockkhal_i2c_Destroy();
}

/* ---- Composing a reading --------------------------------------------------- */

/* The gauge answers zeros for about a second after being woken, and zeros
 * look like data. Until it says it is ready, the PMIC's 20 mV steps are the
 * voltage and the current is unknown. */
TEST_CASE("until the gauge reports ready the PMIC's voltage is the reading",
          "[power_ao]")
{
    power_up_once();
    s_regs[GAUGE][CW_IC_STATE] = 0x00U;

    const PowerEvt * const p = poll_once();
    TEST_ASSERT_EQUAL_UINT16(3904U, p->vbat_mv);
    TEST_ASSERT_EQUAL_INT16(0, p->current_ma);

    /* The snapshot the facade reads says the same, and that the gauge did not answer. */
    {
        power_reading_t r;

        TEST_ASSERT_TRUE(power_reading(&r));
        TEST_ASSERT_FALSE(r.gauge_ok);
        TEST_ASSERT_EQUAL_UINT16(p->vbat_mv, r.vbat_mv);
        TEST_ASSERT_EQUAL_UINT8(p->charging, r.charging);
        TEST_ASSERT_EQUAL(p->vbus, r.vbus);
    }

    Mockkhal_i2c_Destroy();
}

/* The shunt sits across the gauge's CSP/CSN the right way round on this board,
 * so the datasheet's sign - charging positive - is the board's, and the owner
 * passes the reading through untouched. */
TEST_CASE("once the gauge is ready its voltage and its current take over",
          "[power_ao]")
{
    power_up_once();
    s_regs[GAUGE][CW_IC_STATE]     = 0x0CU;
    s_regs[GAUGE][CW_CURRENT_H]    = 0xF9U;    /* -253 mA by the datasheet */
    s_regs[GAUGE][CW_CURRENT_H + 1U] = 0xD0U;
    s_regs[PMIC][BQ_REG0B_STATUS]  = 0x14U;    /* fast charge, power good  */

    const PowerEvt * const p = poll_once();
    TEST_ASSERT_EQUAL_UINT16(3720U, p->vbat_mv);
    TEST_ASSERT_EQUAL_INT16(-253, p->current_ma);
    TEST_ASSERT_EQUAL_UINT8(KDRV_BQ_CHRG_FAST, p->charging);
    TEST_ASSERT_TRUE(p->vbus);
    TEST_ASSERT_EQUAL_UINT16(5000U, p->vbus_mv);
    TEST_ASSERT_TRUE(logged("gauge ready"));

    /* And the snapshot is the same reading, now with the gauge answering. */
    {
        power_reading_t    r;
        kode_sys_battery_t again;

        TEST_ASSERT_TRUE(power_reading(&r));
        TEST_ASSERT_TRUE(r.gauge_ok);
        TEST_ASSERT_EQUAL_UINT16(p->vbat_mv, r.vbat_mv);
        TEST_ASSERT_EQUAL_UINT8(p->soc_pct, r.soc_pct);
        TEST_ASSERT_EQUAL_UINT8(p->charging, r.charging);
        TEST_ASSERT_EQUAL(p->vbus, r.vbus);
        TEST_ASSERT_EQUAL(KODE_OK, kode_sys_battery_read(&again));
        TEST_ASSERT_EQUAL_UINT16(r.vbat_mv, again.vbat_mv);
        TEST_ASSERT_EQUAL_UINT8(r.soc_pct, again.soc_pct);
        TEST_ASSERT_EQUAL_UINT8(r.charging, again.charging);
        TEST_ASSERT_EQUAL(r.vbus, again.vbus);
        TEST_ASSERT_EQUAL(r.gauge_ok, again.gauge_ok);
        TEST_ASSERT_FALSE(power_reading(NULL));
        TEST_ASSERT_EQUAL(KODE_DENIED, kode_sys_battery_read(NULL));
    }

    Mockkhal_i2c_Destroy();
}

/* With the register file's VBUS_STAT at 111 the chip is making its own 5 V:
 * the reading says so, and reports the rail's voltage although no input is
 * good, because 5 V on the header shown as 0 mV would be a lie about the one
 * thing the operator asked for. */
TEST_CASE("the reading reports the boost and the rail it makes", "[power_ao]")
{
    power_up_once();
    s_regs[PMIC][BQ_REG0B_STATUS] = 0xE2U;   /* VBUS_STAT OTG, PG 0, bit 1 reads 1 */

    const PowerEvt * const p = poll_once();
    TEST_ASSERT_TRUE(p->boost);
    TEST_ASSERT_FALSE(p->vbus);
    TEST_ASSERT_EQUAL_UINT16(5000U, p->vbus_mv);

    s_regs[PMIC][BQ_REG0B_STATUS] = 0x14U;
    Mockkhal_i2c_Destroy();
}

/* ---- What the facade reads -------------------------------------------------- */

/** @brief Empty AO_Power's queue, recycling what it holds. */
static void power_flush(void)
{
    const QEvt *e = NULL;

    qf_support_give_queue(AO_Power);
    while ( xQueueReceive(AO_Power->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/** @brief Fill AO_Power's queue until only its margin is free. */
static void power_fill_to_margin(void)
{
    static const QEvt filler = QEVT_INITIALIZER(POWER_DUMP_REQ_SIG);

    for ( uint32_t guard = 0U;
         (POWER_FILL_MAX > guard)
         && (uxQueueSpacesAvailable(AO_Power->eQueue) > KQP_POWER_POST_MARGIN);
         ++guard )
    {
        QACTIVE_POST(AO_Power, &filler, NULL);
    }
    TEST_ASSERT_EQUAL_UINT(KQP_POWER_POST_MARGIN, uxQueueSpacesAvailable(AO_Power->eQueue));
}

/** @brief Whether AO_Power's queue holds an event of @p sig. */
static bool power_queue_has(enum_t sig)
{
    const QEvt *e = NULL;
    bool        found = false;

    while ( xQueueReceive(AO_Power->eQueue, &e, 0U) == pdTRUE )
    {
        found = found || ((QSignal)sig == e->sig);
        QF_gc(e);
    }
    return found;
}

/* A poll the PMIC does not answer makes no reading, so the last good one
 * stays: it is the reading, not a guess. */
TEST_CASE("a poll the PMIC does not answer keeps the last reading", "[power_ao]")
{
    static const QEvt poll = QEVT_INITIALIZER(POWER_POLL_SIG);
    power_reading_t r;
    power_reading_t kept;

    power_up_once();
    (void)poll_once();
    TEST_ASSERT_TRUE(power_reading(&kept));

    s_bus_dead = true;
    QASM_DISPATCH(AO_Power, &poll, 0U);
    s_bus_dead = false;
    TEST_ASSERT_TRUE(power_reading(&r));
    TEST_ASSERT_EQUAL_INT(0, memcmp(&kept, &r, sizeof(r)));

    ao_spy_drain();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("the ship door posts while the queue has room and refuses within its margin",
          "[power_ao]")
{
    power_up_once();
    power_flush();

    TEST_ASSERT_TRUE(power_ship_request());
    TEST_ASSERT_EQUAL_UINT(1U, uxQueueMessagesWaiting(AO_Power->eQueue));
    TEST_ASSERT_TRUE(power_queue_has(POWER_SHIP_REQ_SIG));

    power_fill_to_margin();
    TEST_ASSERT_FALSE(power_ship_request());
    TEST_ASSERT_EQUAL_UINT(KQP_POWER_POST_MARGIN, uxQueueSpacesAvailable(AO_Power->eQueue));
    TEST_ASSERT_FALSE_MESSAGE(power_queue_has(POWER_SHIP_REQ_SIG), "a refused request was queued");

    /* One slot above the margin still takes it, and no more. */
    power_fill_to_margin();
    {
        const QEvt *e = NULL;

        TEST_ASSERT_TRUE(xQueueReceive(AO_Power->eQueue, &e, 0U) == pdTRUE);
        QF_gc(e);
    }
    TEST_ASSERT_TRUE(power_ship_request());
    TEST_ASSERT_EQUAL_UINT(KQP_POWER_POST_MARGIN, uxQueueSpacesAvailable(AO_Power->eQueue));
    TEST_ASSERT_FALSE(power_ship_request());

    power_flush();
    Mockkhal_i2c_Destroy();
}

#if CONFIG_KODE_LOWPOWER
/** @brief Empty AO_LowPower's queue, giving it one first when it has none. */
static void lowpower_flush(void)
{
    const QEvt *e = NULL;

    qf_support_give_queue(AO_LowPower);
    while ( xQueueReceive(AO_LowPower->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/**
 * @brief Whether AO_LowPower's queue held a request for @p target, emptying it.
 * @param target The state looked for.
 * @return true when one was there.
 */
static bool lowpower_queue_has(kode_lp_state_t target)
{
    const QEvt *e = NULL;
    bool        found = false;

    while ( xQueueReceive(AO_LowPower->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)LOWPOWER_REQ_SIG == e->sig )
        {
            found = found || (((const LowPowerReqEvt *)e)->target == (uint8_t)target);
        }
        QF_gc(e);
    }
    return found;
}

/** @brief Fill AO_LowPower's queue until only its command margin is free. */
static void lowpower_fill_to_margin(void)
{
    static const QEvt filler = QEVT_INITIALIZER(LOWPOWER_RADIO_DONE_SIG);

    for ( uint32_t guard = 0U;
         (POWER_FILL_MAX > guard)
         && (uxQueueSpacesAvailable(AO_LowPower->eQueue) > KQP_LOWPOWER_CMD_MARGIN);
         ++guard )
    {
        QACTIVE_POST(AO_LowPower, &filler, NULL);
    }
    TEST_ASSERT_EQUAL_UINT(KQP_LOWPOWER_CMD_MARGIN, uxQueueSpacesAvailable(AO_LowPower->eQueue));
}

/* The facade's sleep is a rest request to AO_LowPower, which asks every owner
 * first; AO_Power hears nothing of it, and only the console reaches ship mode. */
TEST_CASE("the facade's sleep says posted or no resources", "[power_ao]")
{
    power_up_once();
    power_flush();
    lowpower_flush();

    TEST_ASSERT_EQUAL(KODE_OK, kode_sys_power_sleep());
    TEST_ASSERT_TRUE(lowpower_queue_has(KODE_LP_ASLEEP));
    TEST_ASSERT_FALSE_MESSAGE(power_queue_has(POWER_SHIP_REQ_SIG), "the facade reached ship mode");

    lowpower_fill_to_margin();
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, kode_sys_power_sleep());
    TEST_ASSERT_FALSE(lowpower_queue_has(KODE_LP_ASLEEP));

    lowpower_flush();
    power_flush();
    Mockkhal_i2c_Destroy();
}
#endif

/* ---- Switching the board off ----------------------------------------------- */

/* THE case. Everything an operator needs is on the wire before the write that
 * opens the BATFET, because on battery there is no "after". */
TEST_CASE("everything is said before the write that kills the board",
          "[power_ao]")
{
    static const QEvt sleep_req = QEVT_INITIALIZER(POWER_SHIP_REQ_SIG);

    power_up_once();

    unsigned const before = fake_klog_count();
    QASM_DISPATCH(AO_Power, &sleep_req, 0U);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_batfet_writes,
        "the ship-mode register was never written; this case proves nothing");
    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(before, s_lines_at_write,
        "a line logged after the BATFET opens never leaves the UART");
    TEST_ASSERT_EQUAL_HEX8(0x20U, s_regs[PMIC][BQ_REG09_BATFET] & 0x28U);

    Mockkhal_i2c_Destroy();
}

/* Still here after the write means one of two things, and the log has to say
 * which. On USB, VBUS holds VSYS up and the board simply does not switch off. */
TEST_CASE("a ship request that returns explains why the board is still on",
          "[power_ao]")
{
    static const QEvt sleep_req = QEVT_INITIALIZER(POWER_SHIP_REQ_SIG);

    power_up_once();
    QASM_DISPATCH(AO_Power, &sleep_req, 0U);
    TEST_ASSERT_TRUE_MESSAGE(logged("battery path open"),
        "silence here reads as a board that refused to sleep");

    Mockkhal_i2c_Destroy();
}

TEST_CASE("a ship request that could not be attempted says FAILED", "[power_ao]")
{
    static const QEvt sleep_req = QEVT_INITIALIZER(POWER_SHIP_REQ_SIG);

    power_up_once();
    s_bus_dead = true;
    QASM_DISPATCH(AO_Power, &sleep_req, 0U);
    TEST_ASSERT_TRUE(logged("ship mode FAILED"));

    Mockkhal_i2c_Destroy();
}

/* ---- Polling --------------------------------------------------------------- */

/* The supply this AO switches off takes the RTC with it, so this AO is what
 * keeps the clock: a check every poll, a write when one is due. */
TEST_CASE("the poll keeps the clock", "[power_ao]")
{
    static const QEvt poll = QEVT_INITIALIZER(POWER_POLL_SIG);
    uint64_t kept = 0U;

    power_up_once();
    fake_nvs_reset();
    fake_time_wall_set(1790000000ULL * 1000000ULL);
    (void)kdrv_clocks_wall_restore();   /* a fresh boot's schedule */

    QASM_DISPATCH(AO_Power, &poll, 0U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_get_u64("kclock", "wall", &kept));
    TEST_ASSERT_TRUE(1790000000ULL == kept);

    ao_spy_drain();
    Mockkhal_i2c_Destroy();
}

/* On battery nothing runs after the BATFET write, so the clock is kept
 * before it, whether a keep was due or not. */
TEST_CASE("a ship request keeps the clock before the BATFET write",
          "[power_ao]")
{
    static const QEvt poll = QEVT_INITIALIZER(POWER_POLL_SIG);
    static const QEvt sleep_req = QEVT_INITIALIZER(POWER_SHIP_REQ_SIG);
    uint64_t kept = 0U;

    power_up_once();
    fake_nvs_reset();
    fake_time_wall_set(1790000000ULL * 1000000ULL);
    (void)kdrv_clocks_wall_restore();
    QASM_DISPATCH(AO_Power, &poll, 0U);           /* the first keep */
    fake_time_wall_set((1790000000ULL + 30ULL) * 1000000ULL);   /* not due */

    QASM_DISPATCH(AO_Power, &sleep_req, 0U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_get_u64("kclock", "wall", &kept));
    TEST_ASSERT_TRUE((1790000000ULL + 30ULL) == kept);

    ao_spy_drain();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("every poll publishes, whether or not the reading moved",
          "[power_ao]")
{
    static const QEvt poll = QEVT_INITIALIZER(POWER_POLL_SIG);

    power_up_once();
    ao_spy_drain();
    QASM_DISPATCH(AO_Power, &poll, 0U);
    QASM_DISPATCH(AO_Power, &poll, 0U);
    TEST_ASSERT_EQUAL_UINT(2U, ao_spy_count());
    TEST_ASSERT_EQUAL_INT(POWER_EVT_SIG, ao_spy_take()->sig);

    Mockkhal_i2c_Destroy();
}

/* One-shot: each poll reads the conversion the poll before it started, and
 * starts the next. A poll that left no conversion running would read the
 * same numbers for ever. */
TEST_CASE("every poll starts the next ADC conversion", "[power_ao]")
{
    static const QEvt poll = QEVT_INITIALIZER(POWER_POLL_SIG);

    power_up_once();
    s_regs[PMIC][BQ_REG02_ADC] &= (uint8_t)~BQ_CONV_START;   /* it finished */
    QASM_DISPATCH(AO_Power, &poll, 0U);
    TEST_ASSERT_EQUAL_HEX8(BQ_CONV_START, s_regs[PMIC][BQ_REG02_ADC] & BQ_CONV_START);
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_regs[PMIC][BQ_REG02_ADC] & BQ_CONV_RATE);

    ao_spy_drain();
    Mockkhal_i2c_Destroy();
}

/* A board whose PMIC does not answer publishes nothing rather than zeros. */
TEST_CASE("a poll with nothing to read publishes nothing", "[power_ao]")
{
    static const QEvt poll = QEVT_INITIALIZER(POWER_POLL_SIG);

    power_up_once();
    s_bus_dead = true;
    ao_spy_drain();
    QASM_DISPATCH(AO_Power, &poll, 0U);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    Mockkhal_i2c_Destroy();
}

/* The percentage is computed from the cell's open-circuit curve in kbsp_cell.h,
 * smoothed and slew-limited, which is why a settled value takes polls to
 * reach. Its ends and its middle pin the curve; the slew is why sixty. */
TEST_CASE("the state of charge follows the cell's curve", "[power_ao]")
{
    const PowerEvt *p = NULL;

    power_up_once();
    s_regs[GAUGE][CW_CURRENT_H]      = 0x00U;
    s_regs[GAUGE][CW_CURRENT_H + 1U] = 0x00U;

    s_regs[GAUGE][CW_VCELL_H]      = 0x34U;   /* 4200 mV */
    s_regs[GAUGE][CW_VCELL_H + 1U] = 0x80U;
    for ( unsigned i = 0U; 60U > i; ++i )
    {
        p = poll_once();
    }
    TEST_ASSERT_EQUAL_UINT8(100U, p->soc_pct);

    s_regs[GAUGE][CW_VCELL_H]      = 0x2FU;   /* 3800 mV */
    s_regs[GAUGE][CW_VCELL_H + 1U] = 0x80U;
    for ( unsigned i = 0U; 60U > i; ++i )
    {
        uint8_t const last = p->soc_pct;
        p = poll_once();
        TEST_ASSERT_TRUE_MESSAGE(p->soc_pct <= last, "a falling cell never reads fuller");
    }
    TEST_ASSERT_EQUAL_UINT8(42U, p->soc_pct);

    s_regs[GAUGE][CW_VCELL_H]      = 0x29U;   /* 3300 mV */
    s_regs[GAUGE][CW_VCELL_H + 1U] = 0x40U;
    for ( unsigned i = 0U; 60U > i; ++i )
    {
        p = poll_once();
    }
    TEST_ASSERT_EQUAL_UINT8(0U, p->soc_pct);

    Mockkhal_i2c_Destroy();
}

/* VREG_MV is 8 mV under the pack's nominal 4.20 V on purpose, to stay clear
 * of the protection board's trip, so the curve alone never reaches its own
 * 100 % point at the charger's regulation voltage: a cell the charger itself
 * calls done would sit at 99 % forever. Once CHRG_STAT reaches DONE the
 * charger is the authority on "full"; the curve keeps deciding every other
 * state. */
TEST_CASE("a charger that reports done shows 100 %, even short of the curve's top",
          "[power_ao]")
{
    const PowerEvt *p = NULL;

    power_up_once();
    s_regs[GAUGE][CW_IC_STATE]       = 0x0CU;
    s_regs[GAUGE][CW_VCELL_H]        = 0x34U;   /* 4192 mV, VREG_MV        */
    s_regs[GAUGE][CW_VCELL_H + 1U]   = 0x66U;
    s_regs[GAUGE][CW_CURRENT_H]      = 0x00U;   /* charging stopped        */
    s_regs[GAUGE][CW_CURRENT_H + 1U] = 0x00U;
    s_regs[PMIC][BQ_REG0B_STATUS]    = 0x5EU;   /* adapter, done, power good */

    for ( unsigned i = 0U; 60U > i; ++i )
    {
        p = poll_once();
    }
    TEST_ASSERT_EQUAL_UINT8(KDRV_BQ_CHRG_DONE, p->charging);
    TEST_ASSERT_EQUAL_UINT8(100U, p->soc_pct);

    s_regs[PMIC][BQ_REG0B_STATUS] = 0x14U;
    Mockkhal_i2c_Destroy();
}

/* ---- Field requests -------------------------------------------------------- */

TEST_CASE("a field read is answered with the chip's value", "[power_ao]")
{
    power_up_once();

    const PowerFieldEvt *a = field_req(POWER_CHIP_PMIC, POWER_OP_GET,
                                       (uint16_t)KDRV_BQ_ICHG_MA, 0);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_UINT8(POWER_CHIP_PMIC, a->chip);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)KDRV_BQ_ICHG_MA, a->field);
    TEST_ASSERT_EQUAL_INT32((int32_t)KBSP_CELL_ICHG_MA, a->value);

    a = field_req(POWER_CHIP_GAUGE, POWER_OP_GET, (uint16_t)KDRV_CW_VERSION, 0);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_INT32(0xA0, a->value);

    /* A field the driver does not have is refused, and still answered. */
    a = field_req(POWER_CHIP_PMIC, POWER_OP_GET, (uint16_t)KDRV_BQ_FIELD_MAX, 0);
    TEST_ASSERT_EQUAL_UINT8(KODE_DENIED, a->rc);

    Mockkhal_i2c_Destroy();
}

/* A write answers with the readback, so 500 comes back as 512. BATFET_DIS is
 * the one field the owner will not write on request: ship mode has three
 * doors already and every one of them logs before the write. */
TEST_CASE("a field write answers with the readback, and batfet_dis is refused",
          "[power_ao]")
{
    power_up_once();

    const PowerFieldEvt *a = field_req(POWER_CHIP_PMIC, POWER_OP_SET,
                                       (uint16_t)KDRV_BQ_ICHG_MA, 500);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_INT32(512, a->value);
    TEST_ASSERT_EQUAL_HEX8(0x08U, s_regs[PMIC][BQ_REG04_ICHG]);

    uint8_t const batfet = s_regs[PMIC][BQ_REG09_BATFET];
    a = field_req(POWER_CHIP_PMIC, POWER_OP_SET, (uint16_t)KDRV_BQ_BATFET_DIS, 1);
    TEST_ASSERT_EQUAL_UINT8(KODE_DENIED, a->rc);
    TEST_ASSERT_EQUAL_HEX8(batfet, s_regs[PMIC][BQ_REG09_BATFET]);

    a = field_req(POWER_CHIP_GAUGE, POWER_OP_SET, (uint16_t)KDRV_CW_SOC_ALERT_PCT, 10);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_INT32(10, a->value);

    Mockkhal_i2c_Destroy();
}

/* REG_RST puts the chip back to its defaults: ADC stopped, watchdog running,
 * 256 mA of termination. The owner writes the bit, waits for it to clear on
 * its timer, and only then re-applies the whole policy: the ordering the
 * hand path could never guarantee. */
TEST_CASE("pmic_reset waits for REG_RST to clear and re-applies the owner's policy",
          "[power_ao]")
{
    int32_t iterm = -1;

    power_up_once();
    ctrl_req_pending(POWER_CTRL_RESET, 0);
    const PowerCtrlEvt * const a = tick();
    TEST_ASSERT_EQUAL_UINT8(POWER_CTRL_RESET, a->op);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_regs[PMIC][BQ_REG07_TIMERS] & 0x30U);
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_regs[PMIC][BQ_REG02_ADC] & BQ_CONV_RATE);
    TEST_ASSERT_EQUAL_HEX8(BQ_CONV_START, s_regs[PMIC][BQ_REG02_ADC] & BQ_CONV_START);
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_bq25896_get(KDRV_BQ_ITERM_MA, &iterm));
    TEST_ASSERT_EQUAL_INT32((int32_t)KBSP_CELL_ITERM_MA, iterm);
    TEST_ASSERT_TRUE(logged("policy"));

    Mockkhal_i2c_Destroy();
}

TEST_CASE("a REG_RST that never clears times out after ten polls", "[power_ao]")
{
    power_up_once();
    s_reset_sticks = true;
    ctrl_req_pending(POWER_CTRL_RESET, 0);
    for ( unsigned i = 0U; 9U > i; ++i )
    {
        ao_spy_drain();
        /* qf_support_tick(), not a hand-built event: the AO re-arms its own
         * one-shot on every poll but that only clears QP's ctr bookkeeping
         * when the timer fires through QF's real tick machinery (see
         * test_display_mirror.c's mirror_tick()) - dispatching the tick
         * signal by hand here would trip Q_REQUIRE_INCRIT(450) on this
         * AO's own re-arm. */
        (void)qf_support_tick(AO_Power, 1U);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "answered before the tenth poll");
    }
    const PowerCtrlEvt * const a = tick();
    TEST_ASSERT_EQUAL_UINT8(KODE_TIMEOUT, a->rc);
    TEST_ASSERT_TRUE(logged("never cleared"));

    s_reset_sticks = false;
    s_regs[PMIC][BQ_REG14_ID] = 0x02U;
    Mockkhal_i2c_Destroy();
}

/* The hand path cannot wait for the bit, so it is refused and pointed at
 * pmic_reset, the way batfet_dis is pointed at sleep. */
TEST_CASE("reg_rst by hand is refused", "[power_ao]")
{
    power_up_once();
    uint8_t const before = s_regs[PMIC][BQ_REG05_TERM];
    const PowerFieldEvt * const a = field_req(POWER_CHIP_PMIC, POWER_OP_SET,
                                              (uint16_t)KDRV_BQ_REG_RST, 1);
    TEST_ASSERT_EQUAL_UINT8(KODE_DENIED, a->rc);
    TEST_ASSERT_EQUAL_HEX8(before, s_regs[PMIC][BQ_REG05_TERM]);
    TEST_ASSERT_TRUE(logged("pmic_reset"));
    Mockkhal_i2c_Destroy();
}

TEST_CASE("a dump request logs the chip and is answered", "[power_ao]")
{
    power_up_once();

    const PowerFieldEvt *a = field_req(POWER_CHIP_PMIC, POWER_OP_DUMP, 0U, 0);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->rc);
    TEST_ASSERT_TRUE(logged("REG04="));

    a = field_req(POWER_CHIP_GAUGE, POWER_OP_DUMP, 0U, 0);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->rc);
    TEST_ASSERT_TRUE(logged("vcell_mv="));

    Mockkhal_i2c_Destroy();
}

/* bat_info: a fresh reading published now, so the console does not wait for
 * the poll, then everything both chips say. */
TEST_CASE("bat_info publishes a fresh reading and dumps both chips", "[power_ao]")
{
    static const QEvt dump_req = QEVT_INITIALIZER(POWER_DUMP_REQ_SIG);

    power_up_once();
    ao_spy_drain();
    QASM_DISPATCH(AO_Power, &dump_req, 0U);
    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());
    TEST_ASSERT_EQUAL_INT(POWER_EVT_SIG, ao_spy_take()->sig);
    TEST_ASSERT_TRUE(logged("REG0C="));
    TEST_ASSERT_TRUE(logged("profile"));

    Mockkhal_i2c_Destroy();
}

/* The minute's look asleep: a reading for the cable, the clock kept, and not a
 * line in the log (a page of chip dumps every minute is what it replaced). */
TEST_CASE("a peek publishes one reading, keeps the clock and logs nothing", "[power_ao]")
{
    static const QEvt peek = QEVT_INITIALIZER(POWER_PEEK_REQ_SIG);
    uint64_t kept = 0U;

    power_up_once();
    fake_nvs_reset();
    fake_time_wall_set(1790000000ULL * 1000000ULL);
    (void)kdrv_clocks_wall_restore();   /* a fresh boot's schedule */
    ao_spy_drain();
    fake_klog_reset();

    QASM_DISPATCH(AO_Power, &peek, 0U);
    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());
    TEST_ASSERT_EQUAL_INT(POWER_EVT_SIG, ao_spy_take()->sig);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_klog_count(), "a peek printed something");
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_get_u64("kclock", "wall", &kept));
    TEST_ASSERT_TRUE_MESSAGE(1790000000ULL == kept, "the peek did not keep the clock");

    Mockkhal_i2c_Destroy();
}

/* The poll stops with the tick, so a rested AO_Power is the one case where the
 * peek is the only thing that keeps the clock and sees the cable. */
TEST_CASE("a rested AO_Power's peek keeps the clock and sees the cable",
          "[power_ao][lowpower]")
{
    static const QEvt peek = QEVT_INITIALIZER(POWER_PEEK_REQ_SIG);
    const PowerEvt   *p;
    uint64_t          kept = 0U;

    power_up_once();
    lp_round(AO_Power, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);

    fake_nvs_reset();
    fake_time_wall_set(1790000000ULL * 1000000ULL);
    (void)kdrv_clocks_wall_restore();
    s_regs[PMIC][BQ_REG0B_STATUS] = 0x14U;      /* power good: a cable */
    ao_spy_drain();
    fake_klog_reset();

    QASM_DISPATCH(AO_Power, &peek, 0U);
    p = (const PowerEvt *)ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(p, "the rested AO did not answer the peek");
    TEST_ASSERT_EQUAL_INT(POWER_EVT_SIG, p->super.sig);
    TEST_ASSERT_TRUE(p->vbus);
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_get_u64("kclock", "wall", &kept));
    TEST_ASSERT_TRUE_MESSAGE(1790000000ULL == kept, "the clock was not kept asleep");

    lp_awake(AO_Power);
    ao_spy_drain();
    Mockkhal_i2c_Destroy();
}

/* What the interface needs to say why a board on a cable is not charging. */
TEST_CASE("a reading carries the temperature that holds the charge", "[power_ao]")
{
    static const QEvt peek = QEVT_INITIALIZER(POWER_PEEK_REQ_SIG);
    const PowerEvt   *p;

    power_up_once();
    s_regs[PMIC][BQ_REG0B_STATUS] = 0x14U;      /* power good: a cable */
    s_regs[PMIC][BQ_REG0C_FAULT]  = 0x06U;      /* NTC hot */
    ao_spy_drain();
    QASM_DISPATCH(AO_Power, &peek, 0U);
    p = (const PowerEvt *)ao_spy_take();
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT8(6U, p->ntc);

    s_regs[PMIC][BQ_REG0C_FAULT] = 0x00U;
    ao_spy_drain();
    QASM_DISPATCH(AO_Power, &peek, 0U);
    p = (const PowerEvt *)ao_spy_take();
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT8(0U, p->ntc);
    ao_spy_drain();
    Mockkhal_i2c_Destroy();
}

/* ---- Self-tests ------------------------------------------------------------ */

/** @brief Dispatch one self-test request at @p depth and take the verdict it published. */
static const TestResultEvt *test_req_at(uint8_t chip, uint8_t depth)
{
    PowerTestReqEvt req;

    req.super = (QEvt)QEVT_INITIALIZER(POWER_TEST_REQ_SIG);
    req.chip  = chip;
    req.depth = depth;
    ao_spy_drain();
    ao_spy_subscribe(TEST_RESULT_SIG);
    QASM_DISPATCH(AO_Power, &req.super, 0U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, ao_spy_count(), "a self-test is always answered");
    const QEvt * const e = ao_spy_take();
    TEST_ASSERT_EQUAL_INT(TEST_RESULT_SIG, e->sig);
    return (const TestResultEvt *)e;
}

/** @brief The whole row, as the main station and the console ask for it. */
static const TestResultEvt *test_req(uint8_t chip)
{
    return test_req_at(chip, POWER_DEPTH_FULL);
}

/**
 * @brief The PMIC as the production line finds it: holding exactly what the
 *        policy wrote, on an adapter, fast-charging a 3.904 V cell at 500 mA,
 *        the thermistor mid-band and no fault.
 *
 * Every byte is spelled out rather than left to an earlier case, because the
 * register file persists and the cases before this one write into it.
 */
static void pmic_on_the_line(void)
{
    s_regs[PMIC][0x00U] &= (uint8_t)~0x80U;    /* EN_HIZ off                       */
    s_regs[PMIC][0x01U]           = 0x46U;     /* BHOT 01, BCOLD 0, offset 600 mV  */
    s_regs[PMIC][BQ_REG02_ADC]    = 0x11U;     /* one-shot, as the policy writes   */
    s_regs[PMIC][BQ_REG03_CTRL]   = 0x1AU;     /* CHG on, OTG off, SYS_MIN 3.5 V   */
    s_regs[PMIC][BQ_REG04_ICHG]   = 0x08U;     /* 512 mA                           */
    s_regs[PMIC][BQ_REG05_TERM]   = 0x00U;     /* 64 mA / 64 mA                    */
    s_regs[PMIC][0x06U]           = 0x5AU;     /* VREG 4192 mV, BATLOWV 3.0 V      */
    s_regs[PMIC][BQ_REG07_TIMERS] = 0x8DU;     /* watchdog off                     */
    s_regs[PMIC][0x0AU]           = 0x70U;     /* BOOST_LIM 0.5 A                  */
    s_regs[PMIC][BQ_REG0B_STATUS] = 0x56U;     /* adapter, fast, power good        */
    s_regs[PMIC][BQ_REG0C_FAULT]  = 0x00U;
    s_regs[PMIC][BQ_REG0E_BATV]   = 0x50U;     /* BATV 3904 mV                     */
    s_regs[PMIC][0x0FU]           = 0x50U;     /* SYSV 3904 mV                     */
    s_regs[PMIC][0x10U]           = 0x4CU;     /* TS 56.3 %                        */
    s_regs[PMIC][0x11U]           = 0x98U;     /* VBUS_GD, 5000 mV                 */
    s_regs[PMIC][0x12U]           = 0x0AU;     /* ICHGR 500 mA                     */
}

/**
 * @brief What the gauge measures, in the board's own terms.
 * @param mv The cell, in 312.5 uV steps once encoded.
 * @param ma Into the cell positive; encoded through the shunt's value and
 *           orientation, so the register holds what the silicon would.
 */
static void gauge_reads(int32_t mv, int32_t ma)
{
    uint16_t const v    = (uint16_t)((mv * 16) / 5);
    int32_t        code = (int32_t)(((int64_t)ma * 32768LL
                                     * (int64_t)KBSP_CELL_RSENSE_MOHM) / 52400LL);
    s_regs[GAUGE][CW_VCELL_H]        = (uint8_t)(v >> 8);
    s_regs[GAUGE][CW_VCELL_H + 1U]   = (uint8_t)v;
    s_regs[GAUGE][CW_CURRENT_H]      = (uint8_t)(((uint16_t)code) >> 8);
    s_regs[GAUGE][CW_CURRENT_H + 1U] = (uint8_t)code;
}

/* The verdict is the chip doing its job on the production line's fixture:
 * a battery and an adapter. The readings are logged above it, in full. */
TEST_CASE("test pmic passes on a charger holding its policy, on USB, converting and charging",
          "[power_ao]")
{
    power_up_once();
    pmic_on_the_line();

    const TestResultEvt * const r = test_req(POWER_CHIP_PMIC);
    TEST_ASSERT_EQUAL_STRING("pmic", r->name);
    TEST_ASSERT_TRUE(r->ok);
    TEST_ASSERT_TRUE(logged("PN=0"));
    TEST_ASSERT_TRUE(logged("rev=2"));
    TEST_ASSERT_TRUE(logged("vbat 3904mV"));
    TEST_ASSERT_TRUE(logged("ichg 500mA"));
    TEST_ASSERT_TRUE(logged("charge=charging"));
    TEST_ASSERT_TRUE(logged("input=adapter"));
    TEST_ASSERT_TRUE(logged("ntc=normal"));
    TEST_ASSERT_FALSE(logged("FAIL"));

    Mockkhal_i2c_Destroy();
}

TEST_CASE("test pmic fails when the chip stops answering, and says so",
          "[power_ao]")
{
    power_up_once();
    s_bus_dead = true;

    const TestResultEvt * const r = test_req(POWER_CHIP_PMIC);
    TEST_ASSERT_EQUAL_STRING("pmic", r->name);
    TEST_ASSERT_FALSE(r->ok);
    TEST_ASSERT_TRUE(logged("FAIL"));

    Mockkhal_i2c_Destroy();
}

/* Each thing the verdict rests on, taken away one at a time, with the line
 * that names it. The readings still go out first: a FAIL has numbers above. */
TEST_CASE("test pmic fails on a register that did not hold, no USB, an unconverted ADC, "
          "a thermistor out of band, a live fault, and not charging",
          "[power_ao]")
{
    const TestResultEvt *r;

    power_up_once();

    pmic_on_the_line();
    s_regs[PMIC][BQ_REG04_ICHG] = 0x04U;       /* 256 mA: a bench tool's leftover */
    r = test_req(POWER_CHIP_PMIC);
    TEST_ASSERT_FALSE(r->ok);
    TEST_ASSERT_TRUE(logged("ichg_ma reads 256"));
    TEST_ASSERT_TRUE(logged("did not hold"));
    TEST_ASSERT_TRUE(logged("vbat 3904mV"));

    fake_klog_reset();
    pmic_on_the_line();
    s_regs[PMIC][BQ_REG0B_STATUS] = 0x12U;     /* nothing on VBUS, no power good */
    s_regs[PMIC][0x11U]           = 0x18U;     /* VBUS_GD clear                  */
    r = test_req(POWER_CHIP_PMIC);
    TEST_ASSERT_FALSE(r->ok);
    TEST_ASSERT_TRUE(logged("no USB input"));

    fake_klog_reset();
    pmic_on_the_line();
    s_regs[PMIC][BQ_REG0E_BATV] = 0x00U;       /* 2304 mV: never converted */
    r = test_req(POWER_CHIP_PMIC);
    TEST_ASSERT_FALSE(r->ok);
    TEST_ASSERT_TRUE(logged("vbat 2304 mV is not a cell"));

    fake_klog_reset();
    pmic_on_the_line();
    s_regs[PMIC][BQ_REG0C_FAULT] = 0x05U;      /* NTC cold: no pack on J7 */
    r = test_req(POWER_CHIP_PMIC);
    TEST_ASSERT_FALSE(r->ok);
    TEST_ASSERT_TRUE(logged("ts reads cold"));

    fake_klog_reset();
    pmic_on_the_line();
    s_regs[PMIC][BQ_REG0C_FAULT] = 0x20U;      /* thermal shutdown */
    r = test_req(POWER_CHIP_PMIC);
    TEST_ASSERT_FALSE(r->ok);
    TEST_ASSERT_TRUE(logged("fault 0x20 is live"));

    fake_klog_reset();
    pmic_on_the_line();
    s_regs[PMIC][BQ_REG0B_STATUS] = 0x46U;     /* adapter, power good, not charging */
    r = test_req(POWER_CHIP_PMIC);
    TEST_ASSERT_FALSE(r->ok);
    TEST_ASSERT_TRUE(logged("not charging"));

    pmic_on_the_line();
    Mockkhal_i2c_Destroy();
}

/* The gauge's verdict is its two meters against the charger's: the same
 * cell within the charger's coarser step, and the charge current the
 * charger says it is delivering, seen through the shunt the right way round. */
TEST_CASE("test gauge passes when awake, identified, and agreeing with the charger on volts and amps",
          "[power_ao]")
{
    power_up_once();
    pmic_on_the_line();
    s_regs[GAUGE][CW_IC_STATE] = 0x0CU;
    s_regs[GAUGE][0xABU]       = 0x4AU;
    gauge_reads(3905, 512);

    const TestResultEvt * const r = test_req(POWER_CHIP_GAUGE);
    TEST_ASSERT_EQUAL_STRING("gauge", r->name);
    TEST_ASSERT_TRUE(r->ok);
    TEST_ASSERT_TRUE(logged("VERSION=0xA0"));
    TEST_ASSERT_TRUE(logged("FW=0x4A"));
    TEST_ASSERT_TRUE(logged("vcell 3905mV current 512mA"));
    TEST_ASSERT_TRUE(logged("no profile"));
    TEST_ASSERT_FALSE(logged("FAIL"));

    Mockkhal_i2c_Destroy();
}

/* A gauge that has not come up answers zeros for its firmware and its cell:
 * numbers, not errors. The test must not call that a pass. */
TEST_CASE("test gauge fails while the algorithm is not up, and when the firmware is not a CW2217's",
          "[power_ao]")
{
    power_up_once();
    pmic_on_the_line();
    gauge_reads(3905, 512);
    s_regs[GAUGE][CW_IC_STATE] = 0x00U;

    const TestResultEvt *r = test_req(POWER_CHIP_GAUGE);
    TEST_ASSERT_FALSE(r->ok);
    TEST_ASSERT_TRUE(logged("not ready"));

    s_regs[GAUGE][CW_IC_STATE] = 0x0CU;
    s_regs[GAUGE][0xABU]       = 0x8AU;        /* bits 7:6 = 10: a CW2215 */
    r = test_req(POWER_CHIP_GAUGE);
    TEST_ASSERT_FALSE(r->ok);
    TEST_ASSERT_TRUE(logged("not a CW2217"));
    s_regs[GAUGE][0xABU] = 0x4AU;

    Mockkhal_i2c_Destroy();
}

/* An open shunt reads 0 mA and so does a full cell, so the ammeter is only
 * proven with current flowing; a reversed one reads the charge as a
 * discharge; and two voltmeters on one cell have to agree. */
TEST_CASE("test gauge fails when the voltmeters disagree, the shunt is open or reversed, "
          "or the charger delivers too little to prove the ammeter",
          "[power_ao]")
{
    const TestResultEvt *r;

    power_up_once();
    pmic_on_the_line();
    s_regs[GAUGE][CW_IC_STATE] = 0x0CU;
    s_regs[GAUGE][0xABU]       = 0x4AU;

    gauge_reads(3720, 512);                    /* 184 mV from the charger's 3904 */
    r = test_req(POWER_CHIP_GAUGE);
    TEST_ASSERT_FALSE(r->ok);
    TEST_ASSERT_TRUE(logged("voltmeters disagree"));

    fake_klog_reset();
    gauge_reads(3905, 0);                      /* nothing through the shunt */
    r = test_req(POWER_CHIP_GAUGE);
    TEST_ASSERT_FALSE(r->ok);
    TEST_ASSERT_TRUE(logged("current 0 mA against the charger's 500 mA"));

    fake_klog_reset();
    gauge_reads(3905, -512);                   /* the charge read as a discharge */
    r = test_req(POWER_CHIP_GAUGE);
    TEST_ASSERT_FALSE(r->ok);
    TEST_ASSERT_TRUE(logged("current -512 mA against the charger's 500 mA"));

    fake_klog_reset();
    gauge_reads(3905, 100);
    s_regs[PMIC][0x12U] = 0x02U;               /* ICHGR 100 mA: a host port, or a full cell */
    r = test_req(POWER_CHIP_GAUGE);
    TEST_ASSERT_FALSE(r->ok);
    TEST_ASSERT_TRUE(logged("delivering 100 mA, under the 200 mA"));

    pmic_on_the_line();
    gauge_reads(3905, 512);
    Mockkhal_i2c_Destroy();
}

/* The final test's rows run under load on a cell at any charge: the PMIC
 * answering is enough there, so SYS under SYS_MIN and no charge phase pass,
 * while what does not depend on the load still fails. */
TEST_CASE("test pmic asked only to answer passes off regulation and not charging, "
          "and still fails an unconverted ADC",
          "[power_ao]")
{
    const TestResultEvt *r;

    power_up_once();

    pmic_on_the_line();
    s_regs[PMIC][0x0FU] = 0x36U;               /* SYSV 3384 mV, under SYS_MIN 3.5 V */
    r = test_req(POWER_CHIP_PMIC);
    TEST_ASSERT_FALSE(r->ok);
    TEST_ASSERT_TRUE(logged("under sys_min"));

    fake_klog_reset();
    r = test_req_at(POWER_CHIP_PMIC, POWER_DEPTH_ANSWER);
    TEST_ASSERT_TRUE(r->ok);
    TEST_ASSERT_FALSE(logged("FAIL"));

    fake_klog_reset();
    pmic_on_the_line();
    s_regs[PMIC][BQ_REG0B_STATUS] = 0x46U;     /* adapter, power good, not charging */
    r = test_req_at(POWER_CHIP_PMIC, POWER_DEPTH_ANSWER);
    TEST_ASSERT_TRUE(r->ok);
    TEST_ASSERT_TRUE(logged("charge=discharging"));

    fake_klog_reset();
    pmic_on_the_line();
    s_regs[PMIC][BQ_REG0E_BATV] = 0x00U;       /* 2304 mV: never converted */
    r = test_req_at(POWER_CHIP_PMIC, POWER_DEPTH_ANSWER);
    TEST_ASSERT_FALSE(r->ok);
    TEST_ASSERT_TRUE(logged("vbat 2304 mV is not a cell"));

    pmic_on_the_line();
    Mockkhal_i2c_Destroy();
}

/* The final test proves the ammeter with the boost's discharge, so its gauge
 * row leaves the current alone; the voltmeters still have to agree. */
TEST_CASE("test gauge asked only to answer leaves the ammeter unjudged, "
          "and still fails voltmeters that disagree",
          "[power_ao]")
{
    const TestResultEvt *r;

    power_up_once();
    pmic_on_the_line();
    s_regs[GAUGE][CW_IC_STATE] = 0x0CU;
    s_regs[GAUGE][0xABU]       = 0x4AU;

    gauge_reads(3905, 100);
    s_regs[PMIC][0x12U] = 0x02U;               /* ICHGR 100 mA: a full cell */
    r = test_req_at(POWER_CHIP_GAUGE, POWER_DEPTH_ANSWER);
    TEST_ASSERT_TRUE(r->ok);
    TEST_ASSERT_TRUE(logged("ammeter is not judged"));
    TEST_ASSERT_FALSE(logged("FAIL"));

    fake_klog_reset();
    pmic_on_the_line();
    gauge_reads(3905, 120);                    /* the load took the charge current */
    r = test_req_at(POWER_CHIP_GAUGE, POWER_DEPTH_ANSWER);
    TEST_ASSERT_TRUE(r->ok);

    fake_klog_reset();
    gauge_reads(3720, 512);                    /* 184 mV from the charger's 3904 */
    r = test_req_at(POWER_CHIP_GAUGE, POWER_DEPTH_ANSWER);
    TEST_ASSERT_FALSE(r->ok);
    TEST_ASSERT_TRUE(logged("voltmeters disagree"));

    pmic_on_the_line();
    gauge_reads(3905, 512);
    Mockkhal_i2c_Destroy();
}

/* ---- Control requests ------------------------------------------------------ */

/* The bit is the state: off clears CHG_CONFIG and answers 0, on restores it
 * and answers 1, and the register file shows the write each time. */
TEST_CASE("charge off clears CHG_CONFIG and answers the readback; on restores it",
          "[power_ao]")
{
    power_up_once();

    const PowerCtrlEvt *a = ctrl_req(POWER_CTRL_CHARGE, 0);
    TEST_ASSERT_EQUAL_UINT8(POWER_CTRL_CHARGE, a->op);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_INT32(0, a->value);
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_regs[PMIC][BQ_REG03_CTRL] & 0x10U);

    a = ctrl_req(POWER_CTRL_CHARGE, 1);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_INT32(1, a->value);
    TEST_ASSERT_EQUAL_HEX8(0x10U, s_regs[PMIC][BQ_REG03_CTRL] & 0x10U);

    Mockkhal_i2c_Destroy();
}

/* The driver knows the chip's 3008 mA; only the owner knows the pack's 1C.
 * Inside the range the chip's grid decides what is kept, and the answer says
 * so; outside it nothing is written and the log names the ceiling. */
TEST_CASE("a charge limit is clamped to the pack, not the chip, and answers what the chip kept",
          "[power_ao]")
{
    power_up_once();

    const PowerCtrlEvt *a = ctrl_req(POWER_CTRL_ICHG, 500);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_INT32(512, a->value);
    TEST_ASSERT_EQUAL_HEX8(0x08U, s_regs[PMIC][BQ_REG04_ICHG]);

    a = ctrl_req(POWER_CTRL_ICHG, (int32_t)KBSP_CELL_ICHG_MAX_MA + 64);
    TEST_ASSERT_EQUAL_UINT8(KODE_DENIED, a->rc);
    TEST_ASSERT_EQUAL_HEX8(0x08U, s_regs[PMIC][BQ_REG04_ICHG]);
    TEST_ASSERT_TRUE(logged("refused"));

    a = ctrl_req(POWER_CTRL_ICHG, 0);
    TEST_ASSERT_EQUAL_UINT8(KODE_DENIED, a->rc);
    TEST_ASSERT_EQUAL_HEX8(0x08U, s_regs[PMIC][BQ_REG04_ICHG]);

    /* Back to the profile for the cases after this one. */
    a = ctrl_req(POWER_CTRL_ICHG, (int32_t)KBSP_CELL_ICHG_MA);
    TEST_ASSERT_EQUAL_INT32((int32_t)KBSP_CELL_ICHG_MA, a->value);

    Mockkhal_i2c_Destroy();
}

/* On: the bit is written, nothing is answered until the wait, and the tick
 * asks the chip whether it is really boosting. Off: the same shape. The
 * transition back to Power_polling must leave the poll alive and a second
 * request possible - the last two dispatches are that check. */
TEST_CASE("boost on sets OTG_CONFIG, waits, and answers when the chip reports OTG",
          "[power_ao]")
{
    power_up_once();
    cell_reads(3720U);
    s_regs[PMIC][BQ_REG0B_STATUS] = 0x02U;      /* no input, PG 0            */
    s_regs[PMIC][BQ_REG0C_FAULT]  = 0x00U;
    (void)poll_once();

    ctrl_req_pending(POWER_CTRL_BOOST, 1);
    TEST_ASSERT_EQUAL_HEX8(0x20U, s_regs[PMIC][BQ_REG03_CTRL] & 0x20U);

    s_regs[PMIC][BQ_REG0B_STATUS] = 0xE2U;      /* the chip: VBUS_STAT = OTG */
    const PowerCtrlEvt *a = tick();
    TEST_ASSERT_EQUAL_UINT8(POWER_CTRL_BOOST, a->op);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_INT32(1, a->value);
    TEST_ASSERT_TRUE(logged("boost on"));

    const PowerEvt * const p = poll_once();
    TEST_ASSERT_TRUE(p->boost);

    ctrl_req_pending(POWER_CTRL_BOOST, 0);
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_regs[PMIC][BQ_REG03_CTRL] & 0x20U);
    s_regs[PMIC][BQ_REG0B_STATUS] = 0x02U;
    a = tick();
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_INT32(0, a->value);

    s_regs[PMIC][BQ_REG0B_STATUS] = 0x14U;
    Mockkhal_i2c_Destroy();
}

/* Two refusals before any write: USB present (the pin is held low by
 * hardware and an armed bit would boost the instant the cable left), and a
 * cell under the pack's floor. */
TEST_CASE("boost on is refused with USB present or a low cell, and nothing is written",
          "[power_ao]")
{
    power_up_once();
    cell_reads(3720U);
    s_regs[PMIC][BQ_REG0B_STATUS] = 0x16U;      /* PG 1 */
    (void)poll_once();

    const PowerCtrlEvt *a = ctrl_req(POWER_CTRL_BOOST, 1);
    TEST_ASSERT_EQUAL_UINT8(KODE_DENIED, a->rc);
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_regs[PMIC][BQ_REG03_CTRL] & 0x20U);
    TEST_ASSERT_TRUE(logged("USB present"));

    s_regs[PMIC][BQ_REG0B_STATUS] = 0x02U;      /* PG 0 */
    cell_reads(3500U);
    (void)poll_once();
    fake_klog_reset();
    a = ctrl_req(POWER_CTRL_BOOST, 1);
    TEST_ASSERT_EQUAL_UINT8(KODE_DENIED, a->rc);
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_regs[PMIC][BQ_REG03_CTRL] & 0x20U);
    TEST_ASSERT_TRUE(logged("floor"));

    cell_reads(3720U);
    s_regs[PMIC][BQ_REG0B_STATUS] = 0x14U;
    Mockkhal_i2c_Destroy();
}

/* The chip says nothing about the OTG pin, so a boost that did not start is
 * explained by elimination and, whatever the cause, disarmed. */
TEST_CASE("a boost that did not start is explained and disarmed", "[power_ao]")
{
    power_up_once();
    cell_reads(3720U);
    s_regs[PMIC][BQ_REG0B_STATUS] = 0x02U;
    (void)poll_once();

    ctrl_req_pending(POWER_CTRL_BOOST, 1);
    s_regs[PMIC][BQ_REG0C_FAULT] = 0x05U;       /* NTC cold: no pack */
    const PowerCtrlEvt * const a = tick();
    TEST_ASSERT_EQUAL_UINT8(KODE_ERROR, a->rc);
    TEST_ASSERT_EQUAL_INT32(0, a->value);
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_regs[PMIC][BQ_REG03_CTRL] & 0x20U);
    TEST_ASSERT_TRUE(logged("TS outside the boost window"));

    s_regs[PMIC][BQ_REG0C_FAULT]  = 0x00U;
    s_regs[PMIC][BQ_REG0B_STATUS] = 0x14U;
    Mockkhal_i2c_Destroy();
}

/* One wait slot: a request that arrives during it is answered busy at once,
 * and the one that was waiting is still answered on its tick. */
TEST_CASE("a request during a wait is answered busy and the wait completes",
          "[power_ao]")
{
    power_up_once();
    cell_reads(3720U);
    s_regs[PMIC][BQ_REG0B_STATUS] = 0x02U;
    (void)poll_once();

    ctrl_req_pending(POWER_CTRL_BOOST, 1);
    const PowerCtrlEvt *a = ctrl_req(POWER_CTRL_CHARGE, 0);
    TEST_ASSERT_EQUAL_UINT8(POWER_CTRL_CHARGE, a->op);
    TEST_ASSERT_EQUAL_UINT8(KODE_NO_RESOURCES, a->rc);
    TEST_ASSERT_EQUAL_HEX8(0x10U, s_regs[PMIC][BQ_REG03_CTRL] & 0x10U);

    s_regs[PMIC][BQ_REG0B_STATUS] = 0xE2U;
    a = tick();
    TEST_ASSERT_EQUAL_UINT8(POWER_CTRL_BOOST, a->op);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->rc);

    ctrl_req_pending(POWER_CTRL_BOOST, 0);
    s_regs[PMIC][BQ_REG0B_STATUS] = 0x14U;
    (void)tick();
    Mockkhal_i2c_Destroy();
}

/* Started or not, a boost is the owner's to stop: under the pack's floor
 * the poll switches it off, and so does a BOOST_FAULT the chip latched. */
TEST_CASE("the poll stops a running boost on a low cell", "[power_ao]")
{
    power_up_once();
    cell_reads(3720U);
    s_regs[PMIC][BQ_REG0B_STATUS] = 0x02U;
    (void)poll_once();
    ctrl_req_pending(POWER_CTRL_BOOST, 1);
    s_regs[PMIC][BQ_REG0B_STATUS] = 0xE2U;
    (void)tick();

    cell_reads(3200U);
    (void)poll_once();
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_regs[PMIC][BQ_REG03_CTRL] & 0x20U);
    TEST_ASSERT_TRUE(logged("boost off: cell at"));

    cell_reads(3720U);
    s_regs[PMIC][BQ_REG0B_STATUS] = 0x14U;
    Mockkhal_i2c_Destroy();
}

TEST_CASE("the poll stops a running boost on a boost fault", "[power_ao]")
{
    power_up_once();
    cell_reads(3720U);
    s_regs[PMIC][BQ_REG0B_STATUS] = 0x02U;
    s_regs[PMIC][BQ_REG0C_FAULT]  = 0x00U;
    (void)poll_once();
    ctrl_req_pending(POWER_CTRL_BOOST, 1);
    s_regs[PMIC][BQ_REG0B_STATUS] = 0xE2U;
    (void)tick();

    s_regs[PMIC][BQ_REG0C_FAULT] = 0x40U;       /* BOOST_FAULT */
    (void)poll_once();
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_regs[PMIC][BQ_REG03_CTRL] & 0x20U);
    TEST_ASSERT_TRUE(logged("boost off: BOOST_FAULT"));

    s_regs[PMIC][BQ_REG0C_FAULT]  = 0x00U;
    s_regs[PMIC][BQ_REG0B_STATUS] = 0x14U;
    Mockkhal_i2c_Destroy();
}

/* The write that stops a boost can fail while the bus still answers, and a
 * guard that recorded it had acted would never look again: the cell would
 * drain through the header until the pack's protection board tripped. So the
 * wish survives a failed write and the next poll stops the boost. */
TEST_CASE("a boost whose off switch failed is still watched by the next poll",
          "[power_ao]")
{
    power_up_once();
    cell_reads(3720U);
    s_regs[PMIC][BQ_REG0B_STATUS] = 0x02U;
    s_regs[PMIC][BQ_REG0C_FAULT]  = 0x00U;
    (void)poll_once();
    ctrl_req_pending(POWER_CTRL_BOOST, 1);
    s_regs[PMIC][BQ_REG0B_STATUS] = 0xE2U;
    (void)tick();
    TEST_ASSERT_EQUAL_HEX8(0x20U, s_regs[PMIC][BQ_REG03_CTRL] & 0x20U);

    /* The cell falls under the floor and REG03 refuses the write. */
    s_write_fails_reg = BQ_REG03_CTRL;
    cell_reads(3200U);
    (void)poll_once();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x20U, s_regs[PMIC][BQ_REG03_CTRL] & 0x20U,
        "the write was supposed to fail; this case proves nothing");

    /* The bus recovers: the guard must still be armed. */
    s_write_fails_reg = NO_WRITE_FAIL;
    (void)poll_once();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00U, s_regs[PMIC][BQ_REG03_CTRL] & 0x20U,
        "one failed write disarmed the boost guard for the rest of the boot");

    cell_reads(3720U);
    s_regs[PMIC][BQ_REG0B_STATUS] = 0x14U;
    Mockkhal_i2c_Destroy();
}

/* The same shape on the temperature guard: charging outside the pack's
 * window must not be recorded as held until the chip has taken the write. */
TEST_CASE("a charge hold whose write failed is tried again on the next poll",
          "[power_ao]")
{
    power_up_once();
    s_regs[PMIC][BQ_REG0C_FAULT] = 0x00U;
    (void)poll_once();
    TEST_ASSERT_EQUAL_HEX8(0x10U, s_regs[PMIC][BQ_REG03_CTRL] & 0x10U);

    s_write_fails_reg = BQ_REG03_CTRL;
    s_regs[PMIC][BQ_REG0C_FAULT] = 0x02U;       /* NTC warm: 45 to 60 C */
    (void)poll_once();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x10U, s_regs[PMIC][BQ_REG03_CTRL] & 0x10U,
        "the write was supposed to fail; this case proves nothing");

    s_write_fails_reg = NO_WRITE_FAIL;
    (void)poll_once();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00U, s_regs[PMIC][BQ_REG03_CTRL] & 0x10U,
        "one failed write left charging on outside the pack's window");
    TEST_ASSERT_TRUE(logged("charging held"));

    s_regs[PMIC][BQ_REG0C_FAULT] = 0x00U;
    (void)poll_once();
    TEST_ASSERT_EQUAL_HEX8(0x10U, s_regs[PMIC][BQ_REG03_CTRL] & 0x10U);
    Mockkhal_i2c_Destroy();
}

/* The pack charges between 10 and 45 C; the chip's JEITA bands would keep
 * charging from 0 to 60. NTC_FAULT is live on every poll, so the owner holds
 * charging in any band but normal and releases it when normal returns. */
TEST_CASE("charging is held while TS is outside the pack's window and resumes after",
          "[power_ao]")
{
    power_up_once();
    s_regs[PMIC][BQ_REG0C_FAULT] = 0x02U;       /* NTC warm: 45 to 60 C */
    (void)poll_once();
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_regs[PMIC][BQ_REG03_CTRL] & 0x10U);
    TEST_ASSERT_TRUE(logged("charging held"));

    s_regs[PMIC][BQ_REG0C_FAULT] = 0x00U;
    (void)poll_once();
    TEST_ASSERT_EQUAL_HEX8(0x10U, s_regs[PMIC][BQ_REG03_CTRL] & 0x10U);
    TEST_ASSERT_TRUE(logged("charging resumed"));

    Mockkhal_i2c_Destroy();
}

/* A request outranks the guard's release: charging asked off while held
 * stays off when the temperature comes back. */
TEST_CASE("a charge-off request while held is remembered past the release",
          "[power_ao]")
{
    power_up_once();
    s_regs[PMIC][BQ_REG0C_FAULT] = 0x03U;       /* NTC cool: 0 to 10 C */
    (void)poll_once();
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_regs[PMIC][BQ_REG03_CTRL] & 0x10U);

    const PowerCtrlEvt *a = ctrl_req(POWER_CTRL_CHARGE, 0);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_INT32(0, a->value);

    s_regs[PMIC][BQ_REG0C_FAULT] = 0x00U;
    (void)poll_once();
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_regs[PMIC][BQ_REG03_CTRL] & 0x10U);

    a = ctrl_req(POWER_CTRL_CHARGE, 1);
    TEST_ASSERT_EQUAL_INT32(1, a->value);
    Mockkhal_i2c_Destroy();
}

/* ---- The `power_plug` row --------------------------------------------------- */

/** @brief Ask for the row, with the spy listening for its steps and answers. */
static void plug_req(void)
{
    PowerTestReqEvt req;

    req.super = (QEvt)QEVT_INITIALIZER(POWER_TEST_REQ_SIG);
    req.chip  = POWER_CHIP_PLUG;
    req.depth = POWER_DEPTH_FULL;
    ao_spy_drain();
    ao_spy_subscribe(TEST_RESULT_SIG);
    ao_spy_subscribe(TEST_SKIP_SIG);
    ao_spy_subscribe(POWER_TEST_SIG);
    QASM_DISPATCH(AO_Power, &req.super, 0U);
}

/** @brief The row looks at the cable @p n times. */
static void plug_looks(unsigned n)
{
    static const QEvt look = QEVT_INITIALIZER(POWER_TEST_STEP_SIG);

    for ( unsigned i = 0U; i < n; ++i )
    {
        QASM_DISPATCH(AO_Power, &look, 0U);
    }
}

/** @brief The spy's next event that is a step or an answer of the row. */
static const QEvt *plug_next(void)
{
    const QEvt *e = ao_spy_take();

    while ( (NULL != e) && (POWER_TEST_SIG != e->sig)
           && (TEST_RESULT_SIG != e->sig) && (TEST_SKIP_SIG != e->sig) )
    {
        e = ao_spy_take();
    }
    return e;
}

static void plug_expect_step(uint8_t step, uint8_t phase)
{
    const QEvt * const e = plug_next();

    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(POWER_TEST_SIG, e->sig);
    TEST_ASSERT_EQUAL_UINT8(step, ((const PowerTestEvt *)e)->step);
    TEST_ASSERT_EQUAL_UINT8(phase, ((const PowerTestEvt *)e)->phase);
}

/** @brief Stop listening, so the files after this one are not flooded. */
static void plug_done(void)
{
    QActive_unsubscribe(ao_spy(), TEST_SKIP_SIG);
    QActive_unsubscribe(ao_spy(), POWER_TEST_SIG);
    ao_spy_drain();
}

#define LOOKS_STEP    (KODE_POWER_PLUG_STEP_MS / KODE_POWER_PLUG_LOOK_MS)
#define LOOKS_SETTLE  (KODE_POWER_PLUG_SETTLE_MS / KODE_POWER_PLUG_LOOK_MS)

/* Nobody has a charger at hand: that is nothing to judge, not a fault. */
TEST_CASE("power_plug with no charger in its window is skipped", "[power_ao]")
{
    power_up_once();
    pmic_on_the_line();
    s_regs[PMIC][BQ_REG0B_STATUS] = 0x12U;     /* nothing on VBUS */
    s_regs[PMIC][0x11U]           = 0x00U;     /* VBUS_GD 0       */

    plug_req();
    plug_expect_step(1U, POWER_TEST_ASK_PLUG);
    plug_looks(LOOKS_STEP - 1U);
    TEST_ASSERT_NULL_MESSAGE(plug_next(), "answered inside its window");
    plug_looks(1U);
    const QEvt * const e = plug_next();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(TEST_SKIP_SIG, e->sig);
    TEST_ASSERT_EQUAL_STRING("power", ((const TestResultEvt *)e)->name);

    plug_done();
    Mockkhal_i2c_Destroy();
}

/* The whole row: the charger goes in, the chips are judged once the PMIC has
 * had its settle time, and the charger comes out. The cell is full, so the
 * charger delivers nothing - and that leaves the ammeter unjudged rather
 * than failing the gauge. */
TEST_CASE("power_plug passes on a charger in, both chips good, and out again",
          "[power_ao]")
{
    power_up_once();
    pmic_on_the_line();
    s_regs[PMIC][BQ_REG0B_STATUS] = 0x5EU;     /* adapter, done, power good */
    s_regs[PMIC][0x12U]           = 0x00U;     /* ICHGR 0 mA: a full cell   */
    s_regs[GAUGE][CW_IC_STATE]    = 0x0CU;
    s_regs[GAUGE][0xABU]          = 0x4AU;
    gauge_reads(3905, 0);

    plug_req();
    plug_expect_step(1U, POWER_TEST_ASK_PLUG);
    plug_looks(1U);
    plug_expect_step(1U, POWER_TEST_PLUGGED);
    plug_looks(LOOKS_SETTLE - 1U);
    TEST_ASSERT_NULL_MESSAGE(plug_next(), "judged before the PMIC settled");
    plug_looks(1U);
    plug_expect_step(1U, POWER_TEST_PASS);
    plug_expect_step(2U, POWER_TEST_ASK_UNPLUG);
    TEST_ASSERT_TRUE(logged("it is not judged"));

    s_regs[PMIC][BQ_REG0B_STATUS] = 0x12U;     /* the charger is taken out */
    s_regs[PMIC][0x11U]           = 0x00U;
    plug_looks(1U);
    plug_expect_step(2U, POWER_TEST_PASS);
    const QEvt * const e = plug_next();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(TEST_RESULT_SIG, e->sig);
    TEST_ASSERT_EQUAL_STRING("power", ((const TestResultEvt *)e)->name);
    TEST_ASSERT_TRUE(((const TestResultEvt *)e)->ok);

    plug_done();
    Mockkhal_i2c_Destroy();
}

/* One-shot: the readings are the last conversion's. When the charger goes
 * in, a conversion is started at once, so the settle's verdict reads VBUS as
 * it is with the charger in, not as a conversion begun before it. */
TEST_CASE("power_plug starts a conversion the moment the charger is in",
          "[power_ao]")
{
    power_up_once();
    pmic_on_the_line();
    s_regs[PMIC][BQ_REG0B_STATUS] = 0x5EU;     /* adapter, done, power good */
    s_regs[PMIC][0x12U]           = 0x00U;     /* ICHGR 0 mA: a full cell   */
    s_regs[GAUGE][CW_IC_STATE]    = 0x0CU;
    s_regs[GAUGE][0xABU]          = 0x4AU;
    gauge_reads(3905, 0);
    s_regs[PMIC][BQ_REG02_ADC] &= (uint8_t)~BQ_CONV_START;

    plug_req();
    plug_expect_step(1U, POWER_TEST_ASK_PLUG);
    plug_looks(1U);
    plug_expect_step(1U, POWER_TEST_PLUGGED);
    TEST_ASSERT_EQUAL_HEX8(BQ_CONV_START, s_regs[PMIC][BQ_REG02_ADC] & BQ_CONV_START);

    plug_looks(LOOKS_SETTLE);
    plug_expect_step(1U, POWER_TEST_PASS);
    plug_expect_step(2U, POWER_TEST_ASK_UNPLUG);
    s_regs[PMIC][BQ_REG0B_STATUS] = 0x12U;     /* the charger is taken out */
    s_regs[PMIC][0x11U]           = 0x00U;
    plug_looks(1U);
    plug_expect_step(2U, POWER_TEST_PASS);
    TEST_ASSERT_NOT_NULL(plug_next());         /* the verdict */

    plug_done();
    Mockkhal_i2c_Destroy();
}

/* A charger nobody takes out again: the step says nobody answered, and the
 * verdict is a fail that the sequencer reads as a row nobody judged. */
TEST_CASE("power_plug with the charger left in ends on an unanswered step",
          "[power_ao]")
{
    power_up_once();
    pmic_on_the_line();
    s_regs[GAUGE][CW_IC_STATE] = 0x0CU;
    s_regs[GAUGE][0xABU]       = 0x4AU;
    gauge_reads(3905, 512);

    plug_req();
    plug_looks(1U + LOOKS_SETTLE);
    plug_expect_step(1U, POWER_TEST_ASK_PLUG);
    plug_expect_step(1U, POWER_TEST_PLUGGED);
    plug_expect_step(1U, POWER_TEST_PASS);
    plug_expect_step(2U, POWER_TEST_ASK_UNPLUG);
    plug_looks(LOOKS_STEP);
    plug_expect_step(2U, POWER_TEST_TIMEOUT);
    const QEvt * const e = plug_next();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(TEST_RESULT_SIG, e->sig);
    TEST_ASSERT_FALSE(((const TestResultEvt *)e)->ok);

    plug_done();
    Mockkhal_i2c_Destroy();
}

/* A chip that fails with the charger in is a fault, and the row says which
 * step it was. */
TEST_CASE("power_plug fails when a chip fails with the charger in",
          "[power_ao]")
{
    power_up_once();
    pmic_on_the_line();
    s_regs[GAUGE][CW_IC_STATE] = 0x00U;        /* the gauge's algorithm is down */
    gauge_reads(3905, 512);

    plug_req();
    plug_looks(1U + LOOKS_SETTLE);
    plug_expect_step(1U, POWER_TEST_ASK_PLUG);
    plug_expect_step(1U, POWER_TEST_PLUGGED);
    plug_expect_step(1U, POWER_TEST_FAIL);
    const QEvt * const e = plug_next();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(TEST_RESULT_SIG, e->sig);
    TEST_ASSERT_FALSE(((const TestResultEvt *)e)->ok);

    plug_done();
    Mockkhal_i2c_Destroy();
}

/* ---- Shipping after the cable leaves ----------------------------------------- */

/* The countdown runs on QF's real ticks, 10 ms a step. The edge is read by a
 * poll, so a case hands the poll that should see it to the AO itself and
 * counts the 15 s from there; the AO's own periodic polls fire along the way
 * and see the same cable, which is the point: they must change nothing. */

/** @brief Plug state of the cable as the PMIC reports it (PG_STAT). */
static void ship_cable(bool in)
{
    if ( in )
    {
        s_regs[PMIC][BQ_REG0B_STATUS] = 0x14U;
    }
    else
    {
        s_regs[PMIC][BQ_REG0B_STATUS] = 0x12U;
    }
}

#define SHIP_PLUGGED_POLLS 3U   /**< Whole counts a replugged board is watched for, to see nothing fire. */

/** @brief Hand the AO one poll and forget what it published. */
static void ship_poll(void)
{
    static const QEvt poll = QEVT_INITIALIZER(POWER_POLL_SIG);

    QASM_DISPATCH(AO_Power, &poll, 0U);
    ao_spy_drain();
}

/** @brief Arm through the public door and let the AO take the request. */
static void ship_arm(void)
{
    ao_spy_drain();
    TEST_ASSERT_TRUE(power_ship_on_unplug_request(ao_spy()));
    (void)qf_support_settle(AO_Power);
}

/** @brief Let @p ms go by (a multiple of 10) and say how many times the asker was told. */
static unsigned ship_wait(unsigned ms)
{
    unsigned due = 0U;

    ao_spy_drain();
    (void)qf_support_tick(AO_Power, ms / 10U);
    for ( const QEvt *e = ao_spy_take(); NULL != e; e = ao_spy_take() )
    {
        if ( POWER_SHIP_DUE_SIG == e->sig )
        {
            ++due;
        }
    }
    ao_spy_drain();
    return due;
}

/** @brief Leave the cable in and the AO unarmed for the files after this one. */
static void ship_done(void)
{
    ship_cable(true);
    ship_poll();
}

TEST_CASE("ship is due only after 15 s without VBUS", "[power_ao]")
{
    power_up_once();
    pmic_on_the_line();
    ship_cable(true);
    ship_poll();
    ship_arm();
    TEST_ASSERT_TRUE(logged("armed"));

    ship_cable(false);
    ship_poll();
    TEST_ASSERT_TRUE(logged("cable out"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ship_wait(KODE_SHIP_UNPLUG_MS - 100U),
                                   "told before the 15 s");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, ship_wait(100U), "not told at 15 s");
    TEST_ASSERT_TRUE(logged("due"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ship_wait(KODE_SHIP_UNPLUG_MS),
                                   "told twice, or never disarmed");

    ship_done();
    Mockkhal_i2c_Destroy();
}

/* Resting stops the poll that watches the cable: an armed ship end keeps the
 * board up until it is told, and lets it rest once it has been. */
TEST_CASE("an armed ship end refuses the round until it is due",
          "[power_ao][lowpower]")
{
    power_up_once();
    pmic_on_the_line();
    ship_cable(true);
    ship_poll();
    ship_arm();

    lp_round(AO_Power, KODE_LP_ASLEEP);
    lp_expect_answer(true, "ship countdown");
    lp_expect_no_more();

    ship_cable(false);
    ship_poll();
    TEST_ASSERT_EQUAL_UINT(1U, ship_wait(KODE_SHIP_UNPLUG_MS));
    lp_round(AO_Power, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
    lp_expect_no_more();

    lp_awake(AO_Power);
    ship_done();
    Mockkhal_i2c_Destroy();
}

/* Spec 6.3, the owner's ruling: a replug cancels the count only. The Kode
 * stays armed and the next unplug counts the whole 15 s again. */
TEST_CASE("VBUS back cancels the countdown, and the AO stays armed", "[power_ao]")
{
    power_up_once();
    pmic_on_the_line();
    ship_cable(true);
    ship_poll();
    ship_arm();

    ship_cable(false);
    ship_poll();
    TEST_ASSERT_EQUAL_UINT(0U, ship_wait(10000U));
    ship_cable(true);
    ship_poll();
    TEST_ASSERT_TRUE(logged("cancel"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ship_wait(KODE_SHIP_UNPLUG_MS),
                                   "told although the cable came back");

    ship_cable(false);
    ship_poll();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ship_wait(KODE_SHIP_UNPLUG_MS - 100U),
                                   "the countdown did not restart from zero");
    TEST_ASSERT_EQUAL_UINT(1U, ship_wait(100U));

    ship_done();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("a replug ships nothing while the cable stays in", "[power_ao]")
{
    power_up_once();
    pmic_on_the_line();
    ship_cable(true);
    ship_poll();
    ship_arm();

    ship_cable(false);
    ship_poll();
    TEST_ASSERT_EQUAL_UINT(0U, ship_wait(KODE_SHIP_UNPLUG_MS - 100U));
    ship_cable(true);
    ship_poll();
    TEST_ASSERT_TRUE(logged("still armed"));
    /* Polled in, three counts' worth: no timer is left to fire. */
    for ( unsigned k = 0U; SHIP_PLUGGED_POLLS > k; k++ )
    {
        ship_poll();
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ship_wait(KODE_SHIP_UNPLUG_MS),
                                       "shipped with the cable in");
    }

    /* Still armed: the next unplug counts from zero. */
    ship_cable(false);
    ship_poll();
    TEST_ASSERT_EQUAL_UINT(0U, ship_wait(KODE_SHIP_UNPLUG_MS - 100U));
    TEST_ASSERT_EQUAL_UINT(1U, ship_wait(100U));

    ship_done();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("an unarmed unplug does nothing", "[power_ao]")
{
    power_up_once();
    pmic_on_the_line();
    ship_cable(true);
    ship_poll();

    ship_cable(false);
    ship_poll();
    TEST_ASSERT_EQUAL_UINT(0U, ship_wait(KODE_SHIP_UNPLUG_MS + 1000U));

    ship_done();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("armed with VBUS already absent starts counting now", "[power_ao]")
{
    power_up_once();
    pmic_on_the_line();
    ship_cable(false);
    ship_poll();

    ship_arm();
    TEST_ASSERT_TRUE(logged("cable out"));
    TEST_ASSERT_EQUAL_UINT(0U, ship_wait(KODE_SHIP_UNPLUG_MS - 100U));
    TEST_ASSERT_EQUAL_UINT(1U, ship_wait(100U));

    ship_done();
    Mockkhal_i2c_Destroy();
}

/* An arm that was not posted must be told to the asker, who is about to wait
 * for a word that is not coming. */
TEST_CASE("a ship arm that cannot be posted says so", "[power_ao]")
{
    static const QEvt filler = QEVT_INITIALIZER(POWER_DUMP_REQ_SIG);
    const QEvt       *held;

    power_up_once();
    pmic_on_the_line();
    ship_cable(false);
    ship_poll();

    TEST_ASSERT_FALSE_MESSAGE(power_ship_on_unplug_request(NULL),
                              "nobody to tell");

    while ( QACTIVE_POST_X(AO_Power, &filler, 0U, NULL) )
    {
        /* until AO_Power's queue has no room */
    }
    TEST_ASSERT_FALSE_MESSAGE(power_ship_on_unplug_request(ao_spy()),
                              "no room in AO_Power's queue");
    while ( xQueueReceive(AO_Power->eQueue, &held, 0U) == pdTRUE )
    {
        QF_gc(held);
    }

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ship_wait(KODE_SHIP_UNPLUG_MS + 1000U),
                                   "a refused arm arms nothing");
    ship_done();
    Mockkhal_i2c_Destroy();
}

/* ---- The settings ------------------------------------------------------------ */

/* `charge` and `charge_ma` are the user's choice inside the cell's profile:
 * applied at once, stored as the chip kept them, and written again over the
 * policy whenever the policy is - a register reset included. */
TEST_CASE("charge and charge_ma apply at once, are stored, and outlive a reset",
          "[power_ao]")
{
    const KodeConfigAnsEvt *a;
    int32_t                 v = -1;

    fake_nvs_reset();
    power_up_once();

    a = config_ask(AO_Power, &(config_ask_t){ .key = KSET_KEY_CHG_MA });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("512", a->value);
    TEST_ASSERT_FALSE(a->stored);

    a = config_ask(AO_Power, &(config_ask_t){ .key = KSET_KEY_CHG_MA, .set = true,
                                              .i = 300 });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("320", a->value,
                                     "not what the chip kept on its 64 mA grid");
    TEST_ASSERT_TRUE(a->stored);
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_bq25896_get(KDRV_BQ_ICHG_MA, &v));
    TEST_ASSERT_EQUAL_INT32(320, v);

    a = config_ask(AO_Power, &(config_ask_t){ .key = KSET_KEY_CHARGE, .set = true });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("off", a->value);
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_bq25896_get(KDRV_BQ_CHG_CONFIG, &v));
    TEST_ASSERT_EQUAL_INT32_MESSAGE(0, v, "charging was left on");

    ctrl_req_pending(POWER_CTRL_RESET, 0);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, tick()->rc);
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_bq25896_get(KDRV_BQ_ICHG_MA, &v));
    TEST_ASSERT_EQUAL_INT32_MESSAGE(320, v, "the reset's policy wrote over the setting");
    TEST_ASSERT_EQUAL(KODE_OK, kdrv_bq25896_get(KDRV_BQ_CHG_CONFIG, &v));
    TEST_ASSERT_EQUAL_INT32(0, v);

    /* Back to the cell's profile for the files after this one. */
    (void)config_ask(AO_Power, &(config_ask_t){ .key = KSET_KEY_CHARGE, .set = true,
                                                .b = true });
    (void)config_ask(AO_Power, &(config_ask_t){ .key = KSET_KEY_CHG_MA, .set = true,
                                                .i = (int32_t)KBSP_CELL_ICHG_MA });
    fake_nvs_reset();
    ao_spy_drain();
    Mockkhal_i2c_Destroy();
}

/* ---- battery_low, from the poll to the listener ------------------------------- */

#define TEST_WAIT_MS     4000U   /**< How long a case waits for an event.                 */
#define EVENT_REC_MAX    8U      /**< Events the listener keeps.                          */

static volatile uint32_t s_low_n;       /**< How many battery_low events the listener saw. */
static volatile int32_t  s_low_value;   /**< The value of the last one.                    */
static SemaphoreHandle_t s_low_given;   /**< Counts the calls.                             */

/** @brief The listener: count the event and keep its value. */
static void on_low(const kode_event_t *e)
{
    s_low_value = e->value;
    s_low_n     = s_low_n + 1U;
    (void)xSemaphoreGive(s_low_given);
}

/** @brief Poll, then let the standard layer's Active Object take the event. */
static void poll_and_listen(void)
{
    (void)poll_once();
    for ( uint32_t i = 0U; (TEST_WAIT_MS > i) && (uxQueueMessagesWaiting(AO_KodeStandard->eQueue) != 0U); ++i )
    {
        vTaskDelay(1U);
    }
    vTaskDelay(1U);
}

/** @brief @p n polls at a cell of @p mv, the gauge ready and no current. */
static void polls_at(uint16_t mv, uint32_t n)
{
    cell_reads(mv);
    for ( uint32_t i = 0U; i < n; ++i )
    {
        poll_and_listen();
    }
}

/* The real chain: AO_Power's poll, the snapshot, POWER_EVT_SIG, the standard
 * layer's Active Object and a listener. The level is the curve's, slewed, so
 * the case reads that it fell below the mark, not an exact figure; the exact
 * edges are test_kode_battery.c's. */
TEST_CASE("a cell that falls below the mark is one battery_low, and one again after it recovered",
          "[power_ao]")
{
    power_up_once();
    qf_support_init();
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
    fake_sync_real(true);
    TEST_ASSERT_TRUE(kode_standard_ao_ensure());
    QActive_subscribe(AO_KodeStandard, POWER_EVT_SIG);
    s_low_given = xSemaphoreCreateCounting(EVENT_REC_MAX, 0U);
    TEST_ASSERT_NOT_NULL(s_low_given);
    TEST_ASSERT_TRUE(kode_event_add_listener("battery_low", &on_low));

    s_regs[PMIC][BQ_REG0B_STATUS]    = REG0B_OFF_USB;   /* on battery           */
    s_regs[GAUGE][CW_CURRENT_H]      = 0x00U;
    s_regs[GAUGE][CW_CURRENT_H + 1U] = 0x00U;

    polls_at(CELL_FULL_MV, POLLS_TO_SETTLE);
    /* Whatever the climb did is forgotten: the count and the tokens it gave. */
    s_low_n = 0U;
    for ( uint32_t i = 0U; (EVENT_REC_MAX > i) && (xSemaphoreTake(s_low_given, 0U) == pdTRUE); ++i )
    {
    }
    polls_at(CELL_FULL_MV, 1U);
    TEST_ASSERT_EQUAL_UINT32(0U, s_low_n);

    polls_at(CELL_LOW_MV, POLLS_TO_SETTLE);
    TEST_ASSERT_TRUE(xSemaphoreTake(s_low_given, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, s_low_n, "the fall made one event, however many polls it took");
    TEST_ASSERT_LESS_THAN_INT32(KODE_BATTERY_LOW_PCT, s_low_value);

    polls_at(CELL_NEAR_FULL_MV, POLLS_TO_SETTLE);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, s_low_n, "the climb made none");

    polls_at(CELL_LOW_MV, POLLS_TO_SETTLE);
    TEST_ASSERT_TRUE(xSemaphoreTake(s_low_given, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);
    TEST_ASSERT_EQUAL_UINT32(2U, s_low_n);

    TEST_ASSERT_TRUE(kode_event_remove_listener("battery_low", &on_low));
    QActive_unsubscribe(AO_KodeStandard, POWER_EVT_SIG);
    s_regs[PMIC][BQ_REG0B_STATUS] = REG0B_ON_USB;
    ao_spy_drain();
    Mockkhal_i2c_Destroy();
}

/* ---- The board's rest ------------------------------------------------------- */

/** @brief How many writes since the reset went to chip @p chip. */
static unsigned writes_to(unsigned chip)
{
    unsigned n = 0U;

    for ( unsigned i = 0U; (i < s_nwrites) && ((sizeof(s_writes_log) / sizeof(s_writes_log[0])) > i); ++i )
    {
        if ( s_writes_log[i].chip == chip )
        {
            n += 1U;
        }
    }
    return n;
}

/* The ADC is one-shot by policy; a bench tool may have left it converting
 * every second, and the rest puts that right. The gauge is not touched. */
TEST_CASE("a round puts the ADC back to one-shot, leaves the gauge alone and answers once",
          "[power_ao][lowpower]")
{
    power_up_once();
    s_regs[PMIC][BQ_REG02_ADC] |= BQ_CONV_RATE;
    s_nwrites = 0U;

    lp_round(AO_Power, KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00U, s_regs[PMIC][BQ_REG02_ADC] & BQ_CONV_RATE,
                                   "the ADC is still converting every second");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, writes_to(GAUGE), "the gauge was written in the rest");
    lp_expect_answer(false, NULL);
    lp_expect_no_more();

    lp_round(AO_Power, KODE_LP_OFF);
    lp_expect_answer(false, NULL);

    lp_awake(AO_Power);             /* leave the AO as the next case expects */
    ao_spy_drain();
    Mockkhal_i2c_Destroy();
}

/* AO_LowPower asks for a reading every minute while the board sleeps; the
 * rest must not stand in the way, and the cable has to be seen. */
TEST_CASE("a rested AO_Power still answers the minute's dump with the cable as it is",
          "[power_ao][lowpower]")
{
    static const QEvt dump_req = QEVT_INITIALIZER(POWER_DUMP_REQ_SIG);
    const PowerEvt   *p;

    power_up_once();
    lp_round(AO_Power, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);

    s_regs[PMIC][BQ_REG0B_STATUS] = 0x14U;      /* power good: a cable */
    ao_spy_drain();
    QASM_DISPATCH(AO_Power, &dump_req, 0U);
    p = (const PowerEvt *)ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(p, "the rested AO did not answer the dump");
    TEST_ASSERT_EQUAL_INT(POWER_EVT_SIG, p->super.sig);
    TEST_ASSERT_TRUE(p->vbus);

    s_regs[PMIC][BQ_REG0B_STATUS] = 0x12U;      /* nothing on VBUS */
    ao_spy_drain();
    QASM_DISPATCH(AO_Power, &dump_req, 0U);
    p = (const PowerEvt *)ao_spy_take();
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_FALSE_MESSAGE(p->vbus, "the cable that went is still seen");

    s_regs[PMIC][BQ_REG0B_STATUS] = 0x14U;
    lp_awake(AO_Power);
    ao_spy_drain();
    Mockkhal_i2c_Destroy();
}

/* A radio wake runs the tick while the AO is rested: the ADC is idle, so a
 * poll that comes then has nothing true to read. */
TEST_CASE("a rested AO_Power runs no poll, and the first one after AWAKE does",
          "[power_ao][lowpower]")
{
    static const QEvt poll = QEVT_INITIALIZER(POWER_POLL_SIG);

    power_up_once();
    lp_round(AO_Power, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
    s_regs[PMIC][BQ_REG02_ADC] &= (uint8_t)~BQ_CONV_START;
    s_nwrites = 0U;
    ao_spy_drain();

    QASM_DISPATCH(AO_Power, &poll, 0U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "a rested poll published a reading");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_nwrites, "a rested poll wrote a chip");

    lp_awake(AO_Power);
    ao_spy_drain();
    QASM_DISPATCH(AO_Power, &poll, 0U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, ao_spy_count(), "the poll after AWAKE did not run");
    ao_spy_drain();
    Mockkhal_i2c_Destroy();
}

/* The first poll after the wake reads a conversion taken after the sleep. */
TEST_CASE("AWAKE starts a conversion once, and only after a rest", "[power_ao][lowpower]")
{
    power_up_once();
    s_regs[PMIC][BQ_REG02_ADC] &= (uint8_t)~BQ_CONV_START;

    lp_awake(AO_Power);             /* never rested: nothing to restore */
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00U, s_regs[PMIC][BQ_REG02_ADC] & BQ_CONV_START,
                                   "an AWAKE that rested nothing wrote the chip");

    lp_round(AO_Power, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
    lp_awake(AO_Power);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(BQ_CONV_START, s_regs[PMIC][BQ_REG02_ADC] & BQ_CONV_START,
                                   "the wake started no conversion");

    s_regs[PMIC][BQ_REG02_ADC] &= (uint8_t)~BQ_CONV_START;
    lp_awake(AO_Power);             /* an abandoned round publishes it again */
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00U, s_regs[PMIC][BQ_REG02_ADC] & BQ_CONV_START,
                                   "a repeated AWAKE started another");
    ao_spy_drain();
    Mockkhal_i2c_Destroy();
}

/* The pulse is how a flash write is shown; a write is not a moment to sleep. */
TEST_CASE("a flash write in progress refuses the round", "[power_ao][lowpower]")
{
    AppProgressEvt prog;
    AppFlashDoneEvt done = { QEVT_INITIALIZER(APP_FLASH_DONE_SIG), "doom" };

    power_up_once();
    prog.super   = (QEvt)QEVT_INITIALIZER(APP_FLASH_PROGRESS_SIG);
    prog.percent = 10U;
    prog.what    = (uint8_t)KWRITE_APP;
    (void)snprintf(prog.file, sizeof(prog.file), "doom.bin");
    QASM_DISPATCH(AO_Power, &prog.super, 0U);

    lp_round(AO_Power, KODE_LP_ASLEEP);
    lp_expect_answer(true, "flash write");
    lp_expect_no_more();

    QASM_DISPATCH(AO_Power, &done.super, 0U);
    (void)qf_support_tick(AO_Power, 2U);
    lp_round(AO_Power, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
    lp_awake(AO_Power);
    ao_spy_drain();
    Mockkhal_i2c_Destroy();
}

/* A boost or a reset is still settling on the chip. */
TEST_CASE("a control request waiting on the chip refuses the round", "[power_ao][lowpower]")
{
    power_up_once();
    ctrl_req_pending(POWER_CTRL_RESET, 0);

    lp_round(AO_Power, KODE_LP_ASLEEP);
    lp_expect_answer(true, "power control");

    TEST_ASSERT_EQUAL_UINT8(KODE_OK, tick()->rc);
    lp_round(AO_Power, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
    lp_awake(AO_Power);
    ao_spy_drain();
    Mockkhal_i2c_Destroy();
}

/* A radio wake is not AWAKE: the ADC stays idle, and the conversion the first
 * poll reads is started by AWAKE alone. */
TEST_CASE("STATE(RADIO) leaves a rested AO_Power alone; AWAKE still starts a conversion",
          "[power_ao][lowpower]")
{
    power_up_once();
    lp_round(AO_Power, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
    s_regs[PMIC][BQ_REG02_ADC] &= (uint8_t)~BQ_CONV_START;
    s_nwrites = 0U;
    ao_spy_drain();

    lp_radio(AO_Power);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_nwrites, "the radio wake wrote a chip");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00U, s_regs[PMIC][BQ_REG02_ADC] & BQ_CONV_START,
                                   "the radio wake started a conversion");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the radio wake published something");

    lp_awake(AO_Power);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(BQ_CONV_START, s_regs[PMIC][BQ_REG02_ADC] & BQ_CONV_START,
                                   "AWAKE after the radio wake started no conversion");
    ao_spy_drain();
    Mockkhal_i2c_Destroy();
}

/* ---- The pads delivery test's boost (power_boost_test_request) ------------- */

#define BQ_REG0A_BOOST    0x0AU   /**< BOOST_V and BOOST_LIM.                     */
#define BQ_BOOST_LIM_MASK 0x07U   /**< REG0A[2:0]: BOOST_LIM.                    */
#define BQ_OTG_BIT        0x20U   /**< REG03[5]: OTG_CONFIG.                     */
#define REG0B_NO_INPUT    0x02U   /**< REG0B: no input, PG 0.                    */
#define REG0B_CABLE       0x56U   /**< REG0B: adapter, fast charge, power good.  */
#define CELL_BOOST_OK_MV  3720U   /**< A cell well above the boost's floor.      */

/** @brief Hand the AO one poll and take the delivery test's answer, if it came. */
static const PowerBoostTestEvt *boost_test_poll(void)
{
    static const QEvt poll = QEVT_INITIALIZER(POWER_POLL_SIG);
    const PowerBoostTestEvt *found = NULL;

    ao_spy_drain();
    QASM_DISPATCH(AO_Power, &poll, 0U);
    for ( const QEvt *e = ao_spy_take(); NULL != e; e = ao_spy_take() )
    {
        if ( POWER_BOOST_TEST_SIG == e->sig )
        {
            found = (const PowerBoostTestEvt *)e;
        }
    }
    return found;
}

/** @brief Ask for the delivery test's boost and let the AO take the request. */
static void boost_test_ask(bool on)
{
    ao_spy_drain();
    TEST_ASSERT_TRUE(power_boost_test_request(ao_spy(), on));
    (void)qf_support_settle(AO_Power);
}

/** @brief BOOST_LIM as the register file holds it. */
static uint8_t boost_lim(void)
{
    return (uint8_t)(s_regs[PMIC][BQ_REG0A_BOOST] & BQ_BOOST_LIM_MASK);
}

TEST_CASE("test_boost_test_restores_the_limit_on_unplug", "[power_ao][test_dot]")
{
    power_up_once();
    pmic_on_the_line();
    cell_reads(CELL_BOOST_OK_MV);
    s_regs[PMIC][BQ_REG0B_STATUS] = REG0B_NO_INPUT;
    (void)boost_test_poll();

    boost_test_ask(true);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(KODE_TEST_DOT_BOOST_LIM_CODE, boost_lim(),
                                   "the limit raised before the boost");
    TEST_ASSERT_EQUAL_HEX8(BQ_OTG_BIT, s_regs[PMIC][BQ_REG03_CTRL] & BQ_OTG_BIT);
    gauge_reads(3700, -700);
    const PowerBoostTestEvt *a = boost_test_poll();
    TEST_ASSERT_NOT_NULL_MESSAGE(a, "answered at the next poll");
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->status);
    TEST_ASSERT_TRUE_MESSAGE(0 > a->current_ma, "the reading's discharge");
    TEST_ASSERT_NULL_MESSAGE(boost_test_poll(), "answered once");

    /* The station left: the cable came and went with the limit still up. */
    s_regs[PMIC][BQ_REG0B_STATUS] = REG0B_CABLE;
    (void)boost_test_poll();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(KODE_TEST_DOT_BOOST_LIM_CODE, boost_lim(),
                                   "a rising cable changes nothing");
    s_regs[PMIC][BQ_REG0B_STATUS] = REG0B_NO_INPUT;
    (void)boost_test_poll();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(KBSP_CELL_BOOST_LIM_CODE, boost_lim(),
                                   "the cable's fall puts the board's limit back");
    TEST_ASSERT_TRUE(logged("boost limit back"));

    boost_test_ask(false);
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_regs[PMIC][BQ_REG03_CTRL] & BQ_OTG_BIT);
    s_regs[PMIC][BQ_REG0B_STATUS] = 0x14U;
    ship_done();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("test_boost_test_off_puts_the_limit_back_and_answers", "[power_ao][test_dot]")
{
    power_up_once();
    pmic_on_the_line();
    cell_reads(CELL_BOOST_OK_MV);
    s_regs[PMIC][BQ_REG0B_STATUS] = REG0B_NO_INPUT;
    (void)boost_test_poll();
    boost_test_ask(true);
    (void)boost_test_poll();

    boost_test_ask(false);
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_regs[PMIC][BQ_REG03_CTRL] & BQ_OTG_BIT);
    TEST_ASSERT_EQUAL_HEX8(KBSP_CELL_BOOST_LIM_CODE, boost_lim());
    const PowerBoostTestEvt * const a = boost_test_poll();
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->status);
    TEST_ASSERT_FALSE(a->boost);

    s_regs[PMIC][BQ_REG0B_STATUS] = 0x14U;
    ship_done();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("test_boost_test_on_a_cable_is_refused_at_once_and_writes_nothing",
          "[power_ao][test_dot]")
{
    power_up_once();
    pmic_on_the_line();
    cell_reads(CELL_BOOST_OK_MV);
    s_regs[PMIC][BQ_REG0B_STATUS] = REG0B_CABLE;
    (void)boost_test_poll();

    ao_spy_drain();
    TEST_ASSERT_TRUE(power_boost_test_request(ao_spy(), true));
    (void)qf_support_settle(AO_Power);
    const QEvt * const e = ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "a refusal is answered at once");
    TEST_ASSERT_EQUAL(POWER_BOOST_TEST_SIG, e->sig);
    TEST_ASSERT_EQUAL_UINT8(KODE_DENIED, ((const PowerBoostTestEvt *)e)->status);
    TEST_ASSERT_EQUAL_HEX8(KBSP_CELL_BOOST_LIM_CODE, boost_lim());
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_regs[PMIC][BQ_REG03_CTRL] & BQ_OTG_BIT);

    s_regs[PMIC][BQ_REG0B_STATUS] = 0x14U;
    ship_done();
    Mockkhal_i2c_Destroy();
}
