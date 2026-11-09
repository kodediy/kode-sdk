/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file power_ao.c
 * @brief AO_Power: owner of the PMIC and the fuel gauge, which make one reading together.
 */
#include "kaos.h"
#include "kaos_start.h"

#include "kqp.h"
#include "kaos_claim.h"
#include "kbsp.h"
#include "kdrv_clocks.h"
#include "kdrv_bq25896.h"
#include "kdrv_cw2217.h"
#include "khal_nvs.h"
#include "khal_time.h"
#include "kode_log.h"
#include "kset.h"

#include <stdio.h>
#include <string.h>

#define TAG "kaos.power"

/* The cell, as kbsp describes it: the curve a percentage is read off, and
 * the resistance that turns a terminal voltage into an open-circuit one. */
static const uint16_t OCV_MV[KBSP_CELL_OCV_POINTS]  = KBSP_CELL_OCV_MV;
static const uint8_t  OCV_PCT[KBSP_CELL_OCV_POINTS] = KBSP_CELL_OCV_PCT;

/* Smoothing. Both exist for the same reason: a percentage that jumps around
 * while the load changes is worse than useless, because it invites you to
 * believe the jumps. The EMA runs in sixteenths of a millivolt so integer
 * division cannot stall it short of its target. */
#define OCV_EMA_DIV       5      /* alpha = 1/5                              */
#define OCV_FIX_SHIFT     4      /* 1/16 mV                                  */
#define SOC_SLEW_PCT      2      /* most one reading may move the displayed % */
#define GAUGE_MIN_MV      2000   /* below this the gauge has no cell to measure */
/* Under this, on battery, the 3.3 V rail is about to fall out of regulation and
 * every chip on it with it: a cell run flat browns the board out over and over,
 * and chips come out of that latched. Ship mode first, while it can be written. */
#define POWER_CUT_MV      KBSP_CELL_CUT_MV
#define POWER_CUT_READS   3U
#define ICHG_FLOOR_MA     KSET_CHG_MA_MIN  /* the chip's smallest non-zero step; 0 is "charge off" */

#define CTRL_BOOST_WAIT_MS  50U   /* the datasheet's 30 ms start delay, and margin */
#define CTRL_RESET_WAIT_MS  10U   /* one poll of REG_RST                            */
#define CTRL_RESET_TRIES    10U   /* 100 ms, the same budget the driver's loop had  */

/* ---- What the LED says ---------------------------------------------------- */

/**
 * @brief How long a write may go unreported before the LED is taken back.
 *
 * A host that dies mid-transfer stops sending chunks and nothing ever
 * publishes the done or the failure, so without this the light would pulse
 * until the next boot. Longer than one poll and shorter than two, so the
 * guard fires on the second poll after the last report.
 */
#define POWER_LED_WRITE_MS  3000U

/* The light of a write, as the glass says it, and very low. */
#define POWER_LED_ORANGE    0x14U, 0x07U, 0x00U
#define POWER_LED_GREEN     0x01U, 0x10U, 0x05U
#define POWER_LED_DONE_MS   1000U   /* The green that says it ended well. */

/** @brief One reading, composed from both chips. */
typedef struct {
    uint16_t vbat_mv;      /**< The gauge's when it answers, else the PMIC's. */
    uint16_t vsys_mv;
    uint16_t vbus_mv;      /**< 0 unless the input is good.                */
    int32_t  current_ma;   /**< Into the cell positive; 0 without a gauge.  */
    uint8_t  chrg;         /**< kdrv_bq25896_chrg_stat_t                   */
    uint8_t  fault;        /**< REG0C since the last poll.                 */
    uint8_t  ntc;          /**< NTC_FAULT, live: 0 normal, 2 warm, 3 cool, 5 cold, 6 hot. */
    bool     vbus;
    bool     boost;        /**< The chip is making its own 5 V (VBUS_STAT OTG). */
    bool     gauge_ok;
    bool     boost_fault;  /**< BOOST_FAULT latched since the last poll.   */
} reading_t;

typedef struct {
    QActive  super;
    QTimeEvt poll;
    uint8_t  low_reads;    /**< Readings in a row under POWER_CUT_MV.       */
    bool     pmic;         /**< The BQ25896 answered and took the policy.   */
    bool     gauge;        /**< The CW2217 answered at startup.            */
    bool     gauge_ready;  /**< Its algorithm has reported ready since.    */
    bool     gauge_owed;   /**< A gauge row waits for the algorithm.       */
    uint8_t  gauge_depth;  /**< That row's POWER_DEPTH_*.                  */
    uint32_t gauge_asked_ms; /**< khal_time_ms() when that row was asked.  */
    int32_t  ocv_x16;      /**< Filtered OCV, 1/16 mV; -1 before a reading. */
    int32_t  soc_disp;     /**< The percentage last shown; -1 before one.  */
    bool     charge_wanted; /**< The last charge request said on.           */
    bool     charge_held;  /**< The guard has charging off.                */
    QTimeEvt ctrl;         /**< Power_waiting's one-shot.                  */
    uint8_t  ctrl_op;      /**< What is being waited for (POWER_CTRL_*).   */
    int32_t  ctrl_value;   /**< Its argument, for the check afterwards.    */
    uint8_t  ctrl_tries;   /**< Reset polls so far.                        */
    bool     boost_wanted; /**< A boost was asked for and not yet stopped. */
    uint16_t vbat_mv;      /**< The last reading's cell; 0 before one.     */
    QTimeEvt pulse;        /**< The write pulse's own periodic timer.      */
    bool     writing;      /**< A flash write is reporting progress.       */
    uint32_t write_ms;     /**< khal_time_ms() at the last report.         */
    bool     led_done;     /**< The green is on, and the pulse's timer ends it. */
    bool     pulse_lit;    /**< The half of the pulse said last.           */
    bool     led_activity; /**< `led_activity`: a flash write pulses the LED. */
    int32_t  ichg_ma;      /**< The charge current this AO last wrote: the
                            *   stored `charge_ma`, or a facade call's.     */
    QTimeEvt plug;         /**< The `power_plug` row's next look at the
                            *   cable.                                      */
    uint8_t  plug_phase;   /**< PLUG_*: where that row is.                 */
    uint16_t plug_looks;   /**< Looks left in the phase it is in.          */
#if CONFIG_KODE_LOWPOWER
    uint8_t  lp_who;       /**< The id lowpower_join() gave.                */
    bool     rested;       /**< The ADC was left idle for the board's sleep. */
#endif
    QTimeEvt ship;         /**< The countdown after the cable leaves.      */
    QActive *ship_who;     /**< Who power_ship_on_unplug_request() named.  */
    bool     ship_armed;   /**< Someone asked and has not been told yet.   */
    bool     ship_counting; /**< `ship` is running.                        */
    bool     vbus_known;   /**< A poll has read the cable at least once.   */
    bool     vbus_last;    /**< What the last poll read of the cable.      */
    QActive *bt_who;       /**< Who asked the delivery test's boost; NULL
                            *   when no answer is owed.                    */
    uint8_t  bt_status;    /**< The kode_status_t that answer carries.     */
    bool     lim_raised;   /**< BOOST_LIM holds the delivery test's limit,
                            *   not the board's.                           */
} Power;

/* The `power_plug` row's phases. */
#define PLUG_IDLE    0U  /**< The row is not running.                       */
#define PLUG_WAIT_IN 1U  /**< Asked for the charger; not seen yet.          */
#define PLUG_SETTLE  2U  /**< Seen; the PMIC is given time before judging.  */
#define PLUG_WAIT_OUT 3U /**< Judged good; asked for the charger to go.     */

/** @brief How many looks each ask and the settle are. */
#define PLUG_LOOKS_STEP   (KODE_POWER_PLUG_STEP_MS / KODE_POWER_PLUG_LOOK_MS)
#define PLUG_LOOKS_SETTLE (KODE_POWER_PLUG_SETTLE_MS / KODE_POWER_PLUG_LOOK_MS)

static Power l_power;
QActive * const AO_Power = &l_power.super;

static QState Power_initial(Power * const me, void const * const par);
static QState Power_polling(Power * const me, QEvt const * const e);
static QState Power_waiting(Power * const me, QEvt const * const e);

/* ---- Requests ------------------------------------------------------------ */

void power_dump_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(POWER_DUMP_REQ_SIG);
    QACTIVE_POST(AO_Power, &evt, NULL);
}

void power_peek_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(POWER_PEEK_REQ_SIG);
    QACTIVE_POST(AO_Power, &evt, NULL);
}

bool power_ship_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(POWER_SHIP_REQ_SIG);
    return QACTIVE_POST_X(AO_Power, &evt, KQP_POWER_POST_MARGIN, NULL);
}

/** @brief The last reading, written by AO_Power's task and read from any. */
static power_reading_t l_reading;

/** @brief l_reading holds a reading. */
static bool l_reading_valid;

bool power_reading(power_reading_t *out)
{
    bool valid = false;
    QF_CRIT_STAT

    if ( NULL != out )
    {
        QF_CRIT_ENTRY();
        valid = l_reading_valid;
        if ( valid )
        {
            *out = l_reading;
        }
        QF_CRIT_EXIT();
    }
    return valid;
}

bool power_ship_on_unplug_request(QActive *who)
{
    PowerShipArmEvt *e;

    if ( NULL == who )
    {
        KLOG_E(TAG, "ship request with no asker dropped");
        return false;
    }
    e = Q_NEW_X(PowerShipArmEvt, 0U, POWER_SHIP_ARM_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; ship request dropped");
        return false;
    }
    e->who = who;
    if ( !QACTIVE_POST_X(AO_Power, &e->super, 0U, NULL) )
    {
        KLOG_E(TAG, "AO_Power's queue is full; ship request dropped");
        return false;
    }
    return true;
}

bool power_boost_test_request(QActive *who, bool on)
{
    PowerBoostTestReqEvt *e;

    if ( NULL == who )
    {
        KLOG_E(TAG, "boost test with no asker dropped");
        return false;
    }
    e = Q_NEW_X(PowerBoostTestReqEvt, 0U, POWER_BOOST_TEST_REQ_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; boost test dropped");
        return false;
    }
    e->who = who;
    e->on  = on;
    if ( !QACTIVE_POST_X(AO_Power, &e->super, KQP_POWER_CMD_MARGIN, NULL) )
    {
        KLOG_E(TAG, "AO_Power's queue is full; boost test dropped");
        return false;
    }
    return true;
}

void power_field_request(uint8_t chip, uint8_t op, uint16_t field, int32_t value)
{
    PowerFieldReqEvt *e = Q_NEW_X(PowerFieldReqEvt, 0U, POWER_FIELD_REQ_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; field request dropped");
        return;
    }
    e->chip  = chip;
    e->op    = op;
    e->field = field;
    e->value = value;
    QACTIVE_POST(AO_Power, &e->super, NULL);
}

void power_test_request(uint8_t chip, uint8_t depth)
{
    PowerTestReqEvt *e = Q_NEW_X(PowerTestReqEvt, 0U, POWER_TEST_REQ_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; test request dropped");
        return;
    }
    e->chip  = chip;
    e->depth = depth;
    QACTIVE_POST(AO_Power, &e->super, NULL);
}

void power_plug_test_request(void)
{
    power_test_request(POWER_CHIP_PLUG, POWER_DEPTH_FULL);
}

QEvt const *power_ctrl_evt(uint8_t op, int32_t value)
{
    PowerCtrlReqEvt *e   = Q_NEW_X(PowerCtrlReqEvt, 0U, POWER_CTRL_REQ_SIG);
    QEvt const      *evt = NULL;

    if ( NULL != e )
    {
        e->op    = op;
        e->value = value;
        evt      = &e->super;
    }
    return evt;
}

void power_ctrl_request(uint8_t op, int32_t value)
{
    QEvt const *e = power_ctrl_evt(op, value);

    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; control request dropped");
        return;
    }
    QACTIVE_POST(AO_Power, e, NULL);
}

void Power_ctor(void)
{
    Power * const me = &l_power;
    QActive_ctor(&me->super, Q_STATE_CAST(&Power_initial));
    QTimeEvt_ctorX(&me->poll, &me->super, POWER_POLL_SIG, 0U);
    QTimeEvt_ctorX(&me->ctrl, &me->super, POWER_CTRL_TICK_SIG, 0U);
    QTimeEvt_ctorX(&me->pulse, &me->super, POWER_PULSE_SIG, 0U);
    QTimeEvt_ctorX(&me->plug, &me->super, POWER_TEST_STEP_SIG, 0U);
    QTimeEvt_ctorX(&me->ship, &me->super, POWER_SHIP_TMO_SIG, 0U);
    me->plug_phase    = PLUG_IDLE;
    me->plug_looks    = 0U;
    me->ship_who      = NULL;
    me->ship_armed    = false;
    me->ship_counting = false;
    me->vbus_known    = false;
    me->low_reads     = 0U;
    me->vbus_last     = false;
    me->bt_who        = NULL;
    me->bt_status     = (uint8_t)KODE_OK;
    me->lim_raised    = false;
    me->pmic          = false;
    me->gauge         = false;
    me->gauge_ready   = false;
    me->gauge_owed    = false;
    me->gauge_depth   = POWER_DEPTH_FULL;
    me->gauge_asked_ms = 0U;
    me->ocv_x16       = -1;
    me->soc_disp      = -1;
    me->charge_wanted = true;
    me->charge_held   = false;
    me->ctrl_op       = 0U;
    me->ctrl_value    = 0;
    me->ctrl_tries    = 0U;
    me->boost_wanted  = false;
    me->vbat_mv       = 0U;
    me->writing       = false;
    me->write_ms      = 0U;
    me->led_done      = false;
    me->pulse_lit     = false;
    me->led_activity  = true;
    me->ichg_ma       = (int32_t)KBSP_CELL_ICHG_MA;
#if CONFIG_KODE_LOWPOWER
    me->lp_who        = KODE_LOWPOWER_WHO_NONE;
    me->rested        = false;
#endif
}

AO_STORAGE(power, KQP_POWER_QUEUE_LEN, KQP_AO_STACK_MEDIUM);

void kaos_power_start(void)
{
    Power_ctor();
    AO_START(AO_Power, power, KODE_PRIO_POWER);
#if CONFIG_KODEOS_CONSOLE
    /* `led_activity`, `charge` and `charge_ma`, registered once this AO has
     * the queue their requests are posted to. */
    static const kode_config_t POWER_CONFIG[] = {
        { .name = KSET_KEY_LED_ACT, .type = KODE_TYPE_BOOL },
        { .name = KSET_KEY_CHARGE, .type = KODE_TYPE_BOOL },
        { .name = KSET_KEY_CHG_MA, .type = KODE_TYPE_INT,
          .min = KSET_CHG_MA_MIN, .max = (int32_t)KBSP_CELL_ICHG_MAX_MA },
    };

    kaos_require(KODE_CONFIG_ADD(AO_Power, KQP_POWER_CMD_MARGIN, POWER_CONFIG), "power settings");
#endif
}

/* ---- The board's policy for the PMIC ------------------------------------- */

/** @brief One line of the policy: a field, and the value the board wants in it. */
typedef struct {
    kdrv_bq25896_field_t field;
    int32_t              value;
} policy_t;

/*
 * The policy, in the order it has to be written. The watchdog first: while it
 * runs, every later write reverts after about forty seconds. Then the ADC, or
 * every voltage reads as a flat cell. The chip keeps its charge registers across
 * a reflash, so the profile (kbsp_cell.h) is stated every boot. The boost is
 * off at boot and limited (0.5 A, hot stop at 55 C) before anyone can start it.
 */
static const policy_t POLICY[] = {
    { KDRV_BQ_WATCHDOG,   0 },
    { KDRV_BQ_CONV_RATE,  0 },
    { KDRV_BQ_EN_HIZ,     0 },
    { KDRV_BQ_CHG_CONFIG, 1 },
    { KDRV_BQ_OTG_CONFIG, 0 },
    { KDRV_BQ_BOOST_LIM,  (int32_t)KBSP_CELL_BOOST_LIM_CODE },
    { KDRV_BQ_BHOT,       (int32_t)KBSP_CELL_BOOST_HOT_CODE },
    { KDRV_BQ_VREG_MV,    (int32_t)KBSP_CELL_VREG_MV },
    { KDRV_BQ_ICHG_MA,    (int32_t)KBSP_CELL_ICHG_MA },
    { KDRV_BQ_IPRECHG_MA, (int32_t)KBSP_CELL_IPRECHG_MA },
    { KDRV_BQ_ITERM_MA,   (int32_t)KBSP_CELL_ITERM_MA },
};
#define POLICY_LINES  (sizeof(POLICY) / sizeof(POLICY[0]))

/** @brief Is @p f something the policy decides? */
static bool policy_has(kdrv_bq25896_field_t f)
{
    for ( uint32_t i = 0U; POLICY_LINES > i; ++i )
    {
        if ( POLICY[i].field == f )
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Start one ADC conversion; the next poll reads its result.
 *
 * One-shot rather than continuous: while the monitor runs REGN feeds the TS
 * divider, about 0.3 mA from the cell. A conversion takes 8 ms to 1 s
 * (SLUSC76C, tCONV); continuous mode would keep REGN up on battery all the time.
 */
static void adc_start(void)
{
    (void)kdrv_bq25896_set(KDRV_BQ_CONV_START, 1);
}

/**
 * @brief Write the policy into the PMIC, each line read back by the driver.
 *
 * Stops at the first line the chip did not keep and says which one: a policy
 * half applied is not one the owner can vouch for.
 */
static bool apply_policy(void)
{
    for ( uint32_t i = 0U; POLICY_LINES > i; ++i )
    {
        const policy_t * const p = &POLICY[i];
        if ( kdrv_bq25896_set(p->field, p->value) != KODE_OK )
        {
            KLOG_E(TAG, "policy: %s = %ld did not take",
                   kdrv_bq25896_field_info(p->field)->name, (long)p->value);
            return false;
        }
    }
    adc_start();   /* the first poll's readings */
    KLOG_I(TAG, "policy applied: watchdog off, ADC one-shot each poll, charging at "
                "%u mA to %u mV, done under %u mA",
           (unsigned)KBSP_CELL_ICHG_MA, (unsigned)KBSP_CELL_VREG_MV,
           (unsigned)KBSP_CELL_ITERM_MA);
    return true;
}

/* ---- The settings ---------------------------------------------------------- */

/** @brief A bool as the word `config` shows it. */
static const char *on_word(bool on)
{
    const char *w = "off";

    if ( on )
    {
        w = "on";
    }
    return w;
}

/** @brief A bool as the byte the store keeps. */
static uint8_t bool_u8(bool on)
{
    uint8_t v = 0U;

    if ( on )
    {
        v = 1U;
    }
    return v;
}

/**
 * @brief A stored u8 setting read as a bool, or @p dflt when the store has
 *        never held it.
 *
 * @param key         KSET_KEY_*.
 * @param dflt        The absent-store value.
 * @param[out] stored Whether the store held it. May be NULL.
 * @return The value.
 */
static bool setting_bool(const char *key, bool dflt, bool *stored)
{
    uint8_t v = 0U;
    bool    on = dflt;
    bool    in_store = false;

    if ( khal_nvs_get_u8(KSET_NAMESPACE, key, &v) == KODE_OK )
    {
        in_store = true;
        on = (0U != v);
    }
    if ( NULL != stored )
    {
        *stored = in_store;
    }
    return on;
}

/**
 * @brief The stored `charge_ma`, or the cell's profile when the store has
 *        never held one or holds one the pack would refuse.
 *
 * @param[out] stored Whether the store held a usable one. May be NULL.
 * @return mA.
 */
static int32_t setting_ichg(bool *stored)
{
    uint16_t ma  = 0U;
    size_t   len = sizeof(ma);
    int32_t  out = (int32_t)KBSP_CELL_ICHG_MA;
    bool     in_store = false;

    if ( (khal_nvs_get_blob(KSET_NAMESPACE, KSET_KEY_CHG_MA, &ma, &len) == KODE_OK)
        && (sizeof(ma) == len) && ((int32_t)ma >= ICHG_FLOOR_MA)
        && ((uint16_t)KBSP_CELL_ICHG_MAX_MA >= ma) )
    {
        out = (int32_t)ma;
        in_store = true;
    }
    if ( NULL != stored )
    {
        *stored = in_store;
    }
    return out;
}

/**
 * @brief What the owner holds a policy line to: the policy's value, except for
 *        the two lines a setting decides.
 *
 * @param me The AO.
 * @param p  The line.
 * @return The value the chip should read.
 */
static int32_t policy_held(Power const * const me, const policy_t *p)
{
    int32_t v = p->value;

    if ( KDRV_BQ_ICHG_MA == p->field )
    {
        v = me->ichg_ma;
    }
    else if ( KDRV_BQ_CHG_CONFIG == p->field )
    {
        v = 0;
        if ( me->charge_wanted && !me->charge_held )
        {
            v = 1;
        }
    }
    else
    {
        /* every other line is the policy's alone */
    }
    return v;
}

/* ---- State of charge ----------------------------------------------------- */

/** @brief The cell's curve, interpolated. Feed it an open-circuit voltage. */
static uint8_t soc_from_ocv(int32_t mv)
{
    if ( (int32_t)OCV_MV[0] >= mv )
    {
        return 0U;
    }
    for ( uint32_t i = 1U; KBSP_CELL_OCV_POINTS > i; ++i )
    {
        if ( (int32_t)OCV_MV[i] > mv )
        {
            int32_t const span = (int32_t)OCV_MV[i] - (int32_t)OCV_MV[i - 1U];
            int32_t const rise = (int32_t)OCV_PCT[i] - (int32_t)OCV_PCT[i - 1U];
            int32_t const into = mv - (int32_t)OCV_MV[i - 1U];
            return (uint8_t)((int32_t)OCV_PCT[i - 1U]
                             + (((into * rise) + (span / 2)) / span));
        }
    }
    return 100U;
}

/**
 * @brief The percentage to show for this reading.
 *
 * IR-compensated, then smoothed: an EMA on the voltage against momentary
 * loads, and a slew limit on the percentage so it cannot jump. VREG_MV sits
 * under the curve's 100 % point, so a charged cell would read 99 % forever:
 * once CHRG_STAT reads DONE the target is 100, whatever the curve says.
 */
static uint8_t soc_estimate(Power * const me, uint16_t vbat_mv, int32_t current_ma,
                            uint8_t chrg)
{
    int32_t const ocv = (int32_t)vbat_mv
                      - ((current_ma * (int32_t)KBSP_CELL_R_INT_MOHM) / 1000);
    int32_t const ocv_x16 = ocv << OCV_FIX_SHIFT;

    if ( 0 > me->ocv_x16 )
    {
        me->ocv_x16 = ocv_x16;
    }
    else
    {
        me->ocv_x16 += (ocv_x16 - me->ocv_x16) / OCV_EMA_DIV;
    }

    int32_t target = (int32_t)soc_from_ocv(me->ocv_x16 >> OCV_FIX_SHIFT);
    if ( (uint8_t)KDRV_BQ_CHRG_DONE == chrg )
    {
        target = 100;
    }
    if ( 0 > me->soc_disp )
    {
        me->soc_disp = target;
    }
    else if ( (me->soc_disp + SOC_SLEW_PCT) < target )
    {
        me->soc_disp += SOC_SLEW_PCT;
    }
    else if ( (me->soc_disp - SOC_SLEW_PCT) > target )
    {
        me->soc_disp -= SOC_SLEW_PCT;
    }
    else
    {
        me->soc_disp = target;
    }
    return (uint8_t)me->soc_disp;
}

/* ---- Reading ------------------------------------------------------------- */

/**
 * @brief The PMIC's half of a reading. False withholds the whole thing.
 *
 * Every read is checked, and non-short-circuiting on purpose: one register
 * that does not answer should not hide whether the others did, and a dead
 * bus must never decode into the 2.304 V a flat cell reads like.
 */
static bool read_pmic(reading_t *r)
{
    kdrv_bq25896_adc_t    adc;
    kdrv_bq25896_faults_t flt;
    int32_t               chrg  = 0;
    int32_t               pg    = 0;
    int32_t               vstat = 0;

    bool ok = (kdrv_bq25896_adc_read(&adc) == KODE_OK);
    ok = (kdrv_bq25896_get(KDRV_BQ_CHRG_STAT, &chrg) == KODE_OK) && ok;
    ok = (kdrv_bq25896_get(KDRV_BQ_PG_STAT, &pg) == KODE_OK) && ok;
    ok = (kdrv_bq25896_get(KDRV_BQ_VBUS_STAT, &vstat) == KODE_OK) && ok;
    ok = (kdrv_bq25896_faults(&flt, NULL) == KODE_OK) && ok;
    if ( !ok )
    {
        return false;
    }
    r->vbat_mv = adc.vbat_mv;
    r->vsys_mv = adc.vsys_mv;
    r->vbus    = (0 != pg);
    r->boost   = ((int32_t)KDRV_BQ_VBUS_OTG == vstat);
    /* The rail is reported when an input is good, and when the chip is
     * making it: 5 V on the header shown as 0 mV would be a lie. */
    r->vbus_mv = 0U;
    if ( r->vbus || r->boost )
    {
        r->vbus_mv = adc.vbus_mv;
    }
    r->chrg        = (uint8_t)chrg;
    r->fault       = flt.raw;
    r->boost_fault = flt.boost;
    r->ntc         = flt.ntc;
    return true;
}

/**
 * @brief The gauge's half: a finer voltage and the current, once it is up.
 *
 * The sign correction lives here and nowhere else. The driver reports the
 * datasheet's convention; this board's shunt is the other way round.
 */
static void read_gauge(Power * const me, reading_t *r)
{
    int32_t mv = 0;
    int32_t ma = 0;

    r->gauge_ok   = false;
    r->current_ma = 0;
    if ( !me->gauge )
    {
        return;
    }
    if ( !me->gauge_ready )
    {
        int32_t ready = 0;
        if ( (kdrv_cw2217_get(KDRV_CW_READY, &ready) != KODE_OK) || (0 == ready) )
        {
            return;
        }
        me->gauge_ready = true;
        KLOG_D(TAG, "gauge ready; the cell voltage is now its reading");
    }
    if ( (kdrv_cw2217_get(KDRV_CW_VCELL_MV, &mv) != KODE_OK)
        || (kdrv_cw2217_get(KDRV_CW_CURRENT_MA, &ma) != KODE_OK) )
    {
        return;
    }
    r->gauge_ok = true;
    r->current_ma = ma;
    if ( GAUGE_MIN_MV < mv )
    {
        r->vbat_mv = (uint16_t)mv;   /* finer than the PMIC's 20 mV steps */
    }
}

/** @brief Keep one reading for power_reading() and publish it as POWER_EVT_SIG (bounded, never asserts). */
static void power_publish(Power * const me, const reading_t *r, uint8_t soc)
{
    power_reading_t keep;
    PowerEvt       *ev = Q_NEW_X(PowerEvt, 0U, POWER_EVT_SIG);
    QF_CRIT_STAT

    keep.vbat_mv  = r->vbat_mv;
    keep.soc_pct  = soc;
    keep.charging = r->chrg;
    keep.vbus     = r->vbus;
    keep.gauge_ok = r->gauge_ok;
    QF_CRIT_ENTRY();
    l_reading       = keep;
    l_reading_valid = true;
    QF_CRIT_EXIT();

    if ( NULL != ev )
    {
        ev->vbat_mv    = r->vbat_mv;
        ev->vbus_mv    = r->vbus_mv;
        ev->current_ma = (int16_t)r->current_ma;
        ev->soc_pct    = soc;
        ev->charging   = r->chrg;
        ev->vbus       = r->vbus;
        ev->boost      = r->boost;
        ev->fault      = r->fault;
        ev->ntc        = r->ntc;
        QACTIVE_PUBLISH(&ev->super, &me->super);
    }
}

/* ---- The light --------------------------------------------------------- */

/**
 * @brief Say one colour on the LED.
 *
 * 255 is the sink's full drive, which is 5 mA on this board
 * (KTD_FULL_SCALE_MA): a case light, not a torch.
 *
 * @param me  The AO.
 * @param r  Red.
 * @param g  Green.
 * @param b  Blue.
 */
static void led_say(Power * const me, uint8_t r, uint8_t g, uint8_t b)
{
    LedRgbEvt *e = Q_NEW_X(LedRgbEvt, 0U, LED_RGB_SIG);
    if ( NULL != e )
    {
        e->r = r;
        e->g = g;
        e->b = b;
        QACTIVE_PUBLISH(&e->super, &me->super);
    }
}

/** @brief The pulse: the glass's orange, very low, or dark. */
static void led_publish(Power * const me, bool lit)
{
    if ( lit )
    {
        led_say(me, POWER_LED_ORANGE);
    }
    else
    {
        led_say(me, 0U, 0U, 0U);
    }
}

/**
 * @brief Start or stop the write pulse, and say it only when it changes.
 *
 * A flash write pulses orange, KODE_LED_PULSE_MS lit and as long dark, until
 * it ends. Outside a write this AO leaves the LED alone. Every change is a
 * published LED_RGB_SIG, never a kdrv_led call: AO_LedMgr owns the KTD2026
 * (design rule 1).
 *
 * @param me      The AO.
 * @param writing Whether a flash write is reporting progress now.
 */
static void led_write_step(Power * const me, bool writing)
{
    if ( writing != me->writing )
    {
        me->writing   = writing;
        me->pulse_lit = writing;
        me->led_done  = false;
        (void)QTimeEvt_disarm(&me->pulse);
        if ( writing )
        {
            QTimeEvt_armX(&me->pulse, KQP_TICKS_PER_MS(KODE_LED_PULSE_MS),
                          KQP_TICKS_PER_MS(KODE_LED_PULSE_MS));
        }
        led_publish(me, writing);  /* the first half lit, or dark at the end */
    }
}

/** @brief One complete reading, published; false when the PMIC did not answer. */
static bool power_read(Power * const me, reading_t *r, uint8_t *soc)
{
    if ( !me->pmic || !read_pmic(r) )
    {
        return false;
    }
    read_gauge(me, r);
    if ( (!r->vbus) && (!r->boost) && ((uint16_t)GAUGE_MIN_MV < r->vbat_mv) && (POWER_CUT_MV > r->vbat_mv) )
    {
        me->low_reads = (uint8_t)(me->low_reads + 1U);
        if ( POWER_CUT_READS <= me->low_reads )
        {
            KLOG_E(TAG, "the cell is at %u mV on battery: ship mode before it browns out",
                   (unsigned)r->vbat_mv);
            me->low_reads = 0U;
            (void)power_ship_request();
        }
    }
    else
    {
        me->low_reads = 0U;
    }
    me->vbat_mv = r->vbat_mv;
    *soc = soc_estimate(me, r->vbat_mv, r->current_ma, r->chrg);
    power_publish(me, r, *soc);
    return true;
}

/* ---- Field requests ------------------------------------------------------ */

/** @brief A word for the charge phase, for the log. */
static const char *chrg_name(uint8_t c)
{
    switch ( c )
    {
        case KDRV_BQ_CHRG_PRE:  return "pre-charging";
        case KDRV_BQ_CHRG_FAST: return "charging";
        case KDRV_BQ_CHRG_DONE: return "full";
        default:                return "discharging";
    }
}

/**
 * @brief One field of the PMIC, as asked. A write answers with the readback.
 *
 * BATFET_DIS and REG_RST are refused here: the first is ship mode, which has
 * three doors that all say what they do before the board goes away; the
 * second cannot wait for the chip from here, and `pmic_reset` can.
 */
static kode_status_t pmic_field(uint8_t op, uint16_t field, int32_t *value)
{
    kdrv_bq25896_field_t const f = (kdrv_bq25896_field_t)field;
    kode_status_t rc = KODE_DENIED;

    switch ( op )
    {
        case POWER_OP_GET: {
            rc = kdrv_bq25896_get(f, value);
            break;
        }
        case POWER_OP_SET: {
            if ( KDRV_BQ_BATFET_DIS == f )
            {
                KLOG_W(TAG, "batfet_dis is ship mode: use `ship_mode`, which says "
                            "what it does before the board goes away");
                break;
            }
            if ( KDRV_BQ_REG_RST == f )
            {
                KLOG_W(TAG, "reg_rst by hand cannot wait for the chip: use "
                            "`pmic_reset`, which does and re-applies the policy");
                break;
            }
            rc = kdrv_bq25896_set(f, *value);
            if ( KODE_OK != rc )
            {
                break;
            }
            if ( policy_has(f) )
            {
                KLOG_W(TAG, "%s changed by hand; the owner's policy is off "
                            "until the next boot",
                       kdrv_bq25896_field_info(f)->name);
            }
            rc = kdrv_bq25896_get(f, value);
            break;
        }
        case POWER_OP_DUMP: {
            kdrv_bq25896_dump();
            rc = KODE_OK;
            break;
        }
        default: {
            break;
        }
    }
    return rc;
}

/** @brief One field of the gauge, as asked. */
static kode_status_t gauge_field(uint8_t op, uint16_t field, int32_t *value)
{
    kdrv_cw2217_field_t const f = (kdrv_cw2217_field_t)field;
    kode_status_t rc = KODE_DENIED;

    switch ( op )
    {
        case POWER_OP_GET: {
            rc = kdrv_cw2217_get(f, value);
            break;
        }
        case POWER_OP_SET: {
            rc = kdrv_cw2217_set(f, *value);
            if ( KODE_OK == rc )
            {
                rc = kdrv_cw2217_get(f, value);
            }
            break;
        }
        case POWER_OP_DUMP: {
            kdrv_cw2217_dump();
            rc = KODE_OK;
            break;
        }
        default: {
            break;
        }
    }
    return rc;
}

/** @brief Answer a field request, refusal included. */
static void power_field(Power * const me, PowerFieldReqEvt const *q)
{
    kode_status_t rc    = KODE_DENIED;
    int32_t       value = q->value;

    if ( (POWER_CHIP_PMIC == q->chip) && me->pmic )
    {
        rc = pmic_field(q->op, q->field, &value);
    }
    else if ( (POWER_CHIP_GAUGE == q->chip) && me->gauge )
    {
        rc = gauge_field(q->op, q->field, &value);
    }
    else
    {
        KLOG_W(TAG, "no such chip answered at boot; nothing to ask");
    }

    PowerFieldEvt *ev = Q_NEW_X(PowerFieldEvt, 0U, POWER_FIELD_SIG);
    if ( NULL != ev )
    {
        ev->chip  = q->chip;
        ev->op    = q->op;
        ev->field = q->field;
        ev->rc    = (uint8_t)rc;
        ev->value = value;
        QACTIVE_PUBLISH(&ev->super, &me->super);
    }
}

/* ---- Self-tests ---------------------------------------------------------- */

/** @brief A word for an NTC band, for the log. */
static const char *ntc_name(uint8_t ntc)
{
    switch ( ntc )
    {
        case 2U:  return "warm";
        case 3U:  return "cool";
        case 5U:  return "cold";
        case 6U:  return "hot";
        default:  return "normal";
    }
}

/** @brief A word for what is on VBUS, for the log. */
static const char *vbus_name(int32_t v)
{
    switch ( v )
    {
        case KDRV_BQ_VBUS_SDP:     return "usb-host";
        case KDRV_BQ_VBUS_ADAPTER: return "adapter";
        case KDRV_BQ_VBUS_OTG:     return "otg";
        default:                   return "none";
    }
}

/*
 * The self-tests. Beyond answering and identity, the charger must hold the
 * policy, see the cable, convert, regulate SYS, read the thermistor in band and
 * charge; the gauge must agree with the charger on voltage and on current, which
 * needs current to flow. POWER_DEPTH_ANSWER leaves out SYS, charging and the
 * ammeter. Every reading is logged before any check, so a FAIL has its numbers.
 */

/** @brief USB's 5 V as the input ADC sees it, with a cable's drop allowed. */
#define TEST_VBUS_MIN_MV      4400U
#define TEST_VBUS_MAX_MV      5500U
/** @brief How far the two voltmeters may disagree: the PMIC's 20 mV steps and its offset. */
#define TEST_VCELL_AGREE_MV   100
/** @brief The least charge current that proves the ammeter (under it, 0 mA is a pass). */
#define TEST_ICHG_PROVE_MA    200U
/** @brief The ammeters' tolerance floor; a quarter of the current is added to it. */
#define TEST_ICHG_AGREE_MA    50

/* How the gauge's row treats its ammeter. */
#define AMMETER_PROVE         0U   /**< Judged: the charger must deliver enough to prove it. */
#define AMMETER_IF_FLOWING    1U   /**< Judged when it does; a full cell leaves it unjudged. */
#define AMMETER_SKIP          2U   /**< Not judged: another step proves it.                  */

/**
 * @brief `test component=pmic`: is the BQ25896 there, and doing its job?
 *
 * Identity, the policy held, the input good and at 5 V, an ADC that has
 * converted a cell, SYS regulated, the thermistor in band, no fault, and
 * a charge phase in progress. The readings go out first, then the verdict.
 *
 * @param me    The AO: the policy's two settings lines are held to its wish.
 * @param depth POWER_DEPTH_ANSWER leaves out SYS regulated and the charge
 *              phase, which depend on the load and the cell's charge.
 * @return true on a pass.
 */
static bool test_pmic(Power const * const me, uint8_t depth)
{
    int32_t pn = 0;
    int32_t rev = 0;
    int32_t chrg = 0;
    int32_t vbus = 0;
    int32_t pg = 0;
    int32_t vsysmin = 0;
    int32_t sysmin_mv = 0;
    kdrv_bq25896_adc_t    adc;
    kdrv_bq25896_faults_t live;

    if ( !kdrv_bq25896_present() )
    {
        KLOG_E(TAG, "test pmic: FAIL - no PMIC answered at boot");
        return false;
    }
    bool ok = (kdrv_bq25896_get(KDRV_BQ_PN, &pn) == KODE_OK);
    ok = (kdrv_bq25896_get(KDRV_BQ_DEV_REV, &rev) == KODE_OK) && ok;
    ok = (kdrv_bq25896_adc_read(&adc) == KODE_OK) && ok;
    ok = (kdrv_bq25896_get(KDRV_BQ_CHRG_STAT, &chrg) == KODE_OK) && ok;
    ok = (kdrv_bq25896_get(KDRV_BQ_VBUS_STAT, &vbus) == KODE_OK) && ok;
    ok = (kdrv_bq25896_get(KDRV_BQ_PG_STAT, &pg) == KODE_OK) && ok;
    ok = (kdrv_bq25896_get(KDRV_BQ_VSYS_STAT, &vsysmin) == KODE_OK) && ok;
    ok = (kdrv_bq25896_get(KDRV_BQ_SYS_MIN_MV, &sysmin_mv) == KODE_OK) && ok;
    ok = (kdrv_bq25896_faults(NULL, &live) == KODE_OK) && ok;
    if ( !ok )
    {
        KLOG_E(TAG, "test pmic: FAIL - the chip stopped answering");
        return false;
    }
    if ( 0 != pn )
    {
        KLOG_E(TAG, "test pmic: FAIL - PN=%ld is not a bq25896", (long)pn);
        return false;
    }
    KLOG_N(TAG, "test pmic: BQ25896 answers at 0x%02X, PN=%ld rev=%ld",
           (unsigned)KBSP_I2C_ADDR_CHARGER, (long)pn, (long)rev);
    KLOG_N(TAG, "test pmic: vbat %umV vsys %umV vbus %umV ichg %umA ts %lu.%lu%%",
           (unsigned)adc.vbat_mv, (unsigned)adc.vsys_mv, (unsigned)adc.vbus_mv,
           (unsigned)adc.ichg_ma, (unsigned long)(adc.ts_pct_x1000 / 1000U),
           (unsigned long)((adc.ts_pct_x1000 % 1000U) / 100U));
    KLOG_N(TAG, "test pmic: charge=%s input=%s pg=%ld vbus_gd=%u vsys_min=%ld faults=0x%02X ntc=%s",
           chrg_name((uint8_t)chrg), vbus_name(vbus), (long)pg,
           (unsigned)adc.vbus_gd, (long)vsysmin, (unsigned)live.raw,
           ntc_name(live.ntc));

    /* The policy, read back: every line the owner wrote at boot is still
     * what the chip holds. A register that does not keep what it is given
     * is found here, and so is a chip that reset underneath the firmware. */
    for ( uint32_t i = 0U; POLICY_LINES > i; ++i )
    {
        int32_t held = 0;
        int32_t const wrote = policy_held(me, &POLICY[i]);
        if ( kdrv_bq25896_get(POLICY[i].field, &held) != KODE_OK )
        {
            KLOG_E(TAG, "test pmic: FAIL - the chip stopped answering");
            return false;
        }
        if ( held != wrote )
        {
            KLOG_E(TAG, "test pmic: FAIL - %s reads %ld, the policy wrote %ld: "
                        "a register did not hold",
                   kdrv_bq25896_field_info(POLICY[i].field)->name,
                   (long)held, (long)wrote);
            return false;
        }
    }
    if ( !adc.vbus_gd || (0 == pg)
        || ((KDRV_BQ_VBUS_SDP != vbus) && (KDRV_BQ_VBUS_ADAPTER != vbus)) )
    {
        KLOG_E(TAG, "test pmic: FAIL - no USB input (input=%s pg=%ld vbus_gd=%u): "
                    "this test runs on the cable",
               vbus_name(vbus), (long)pg, (unsigned)adc.vbus_gd);
        return false;
    }
    if ( (TEST_VBUS_MIN_MV > adc.vbus_mv) || (TEST_VBUS_MAX_MV < adc.vbus_mv) )
    {
        KLOG_E(TAG, "test pmic: FAIL - vbus %u mV is not USB's 5 V (%u..%u)",
               (unsigned)adc.vbus_mv, (unsigned)TEST_VBUS_MIN_MV,
               (unsigned)TEST_VBUS_MAX_MV);
        return false;
    }
    if ( (KBSP_CELL_VCUT_MV > adc.vbat_mv) || (KBSP_CELL_VTRIP_MV < adc.vbat_mv) )
    {
        KLOG_E(TAG, "test pmic: FAIL - vbat %u mV is not a cell (%u..%u; 2304 is "
                    "an ADC that never converted, under %u is no pack)",
               (unsigned)adc.vbat_mv, (unsigned)KBSP_CELL_VCUT_MV,
               (unsigned)KBSP_CELL_VTRIP_MV, (unsigned)KBSP_CELL_VCUT_MV);
        return false;
    }
    if ( (POWER_DEPTH_FULL == depth) && ((uint16_t)sysmin_mv > adc.vsys_mv) )
    {
        KLOG_E(TAG, "test pmic: FAIL - vsys %u mV under sys_min %ld mV: the "
                    "power path is not regulating",
               (unsigned)adc.vsys_mv, (long)sysmin_mv);
        return false;
    }
    if ( 0U != live.ntc )
    {
        KLOG_E(TAG, "test pmic: FAIL - ts reads %s at %lu.%lu%%: the pack's "
                    "thermistor is not in the 10..45 C band (no pack floats "
                    "at 85%%, cold)",
               ntc_name(live.ntc), (unsigned long)(adc.ts_pct_x1000 / 1000U),
               (unsigned long)((adc.ts_pct_x1000 % 1000U) / 100U));
        return false;
    }
    if ( live.watchdog || live.boost || (0U != live.chrg) || live.bat_ovp )
    {
        KLOG_E(TAG, "test pmic: FAIL - fault 0x%02X is live (watchdog=%u boost=%u "
                    "chrg=%u bat_ovp=%u)",
               (unsigned)live.raw, (unsigned)live.watchdog, (unsigned)live.boost,
               (unsigned)live.chrg, (unsigned)live.bat_ovp);
        return false;
    }
    if ( (POWER_DEPTH_FULL == depth) && (KDRV_BQ_CHRG_NONE == chrg) )
    {
        KLOG_E(TAG, "test pmic: FAIL - not charging, with the input good, charging "
                    "enabled and the cell at %u mV",
               (unsigned)adc.vbat_mv);
        return false;
    }
    return true;
}

/**
 * @brief `test component=gauge`: is the CW2217 there, awake, and measuring?
 *
 * Identity and readiness first - a gauge that has not come up answers zeros
 * for its firmware and its cell, and zeros are numbers. Then the voltmeter
 * against the charger's, and the ammeter against the current the charger
 * says it is pushing in. The readings go out first, then the verdict.
 *
 * @param ammeter AMMETER_*: how the ammeter is judged.
 * @return true on a pass.
 */
static bool test_gauge(uint8_t ammeter)
{
    int32_t ver = 0;
    int32_t fw = 0;
    int32_t ready = 0;
    int32_t vcell = 0;
    int32_t ma = 0;
    int32_t ave = 0;
    int32_t temp = 0;
    int32_t soc = 0;
    int32_t soh = 0;
    int32_t cycles = 0;
    int32_t upd = 0;
    kdrv_bq25896_adc_t adc;

    if ( !kdrv_cw2217_present() )
    {
        KLOG_E(TAG, "test gauge: FAIL - no gauge answered at boot (it runs off VBAT: no battery?)");
        return false;
    }
    if ( (kdrv_cw2217_get(KDRV_CW_VERSION, &ver) != KODE_OK)
        || (kdrv_cw2217_get(KDRV_CW_READY, &ready) != KODE_OK)
        || (kdrv_cw2217_get(KDRV_CW_FW_VERSION, &fw) != KODE_OK) )
    {
        KLOG_E(TAG, "test gauge: FAIL - the chip stopped answering");
        return false;
    }
    if ( 0 == ready )
    {
        KLOG_E(TAG, "test gauge: FAIL - not ready (0xA7): the algorithm has not come up");
        return false;
    }
    if ( (fw & 0xC0) != 0x40 )
    {
        KLOG_E(TAG, "test gauge: FAIL - FW=0x%02lX is not a CW2217's (bits 7:6 = 01)",
               (unsigned long)fw);
        return false;
    }
    KLOG_N(TAG, "test gauge: CW2217 answers at 0x%02X, VERSION=0x%02lX FW=0x%02lX (rev 0x%02lX), ready",
           (unsigned)KBSP_I2C_ADDR_GAUGE, (unsigned long)ver, (unsigned long)fw,
           (unsigned long)(fw & 0x3F));

    bool ok = (kdrv_cw2217_get(KDRV_CW_VCELL_MV, &vcell) == KODE_OK);
    ok = (kdrv_cw2217_get(KDRV_CW_CURRENT_MA, &ma) == KODE_OK) && ok;
    ok = (kdrv_cw2217_get(KDRV_CW_AVE_CURRENT_MA, &ave) == KODE_OK) && ok;
    ok = (kdrv_cw2217_get(KDRV_CW_TEMP_C2, &temp) == KODE_OK) && ok;
    ok = (kdrv_cw2217_get(KDRV_CW_SOC_X256, &soc) == KODE_OK) && ok;
    ok = (kdrv_cw2217_get(KDRV_CW_SOH_PCT, &soh) == KODE_OK) && ok;
    ok = (kdrv_cw2217_get(KDRV_CW_CYCLES_X16, &cycles) == KODE_OK) && ok;
    ok = (kdrv_cw2217_get(KDRV_CW_UPDATE_FLAG, &upd) == KODE_OK) && ok;
    if ( !ok )
    {
        KLOG_E(TAG, "test gauge: FAIL - the chip stopped answering");
        return false;
    }
    /* Half degrees to a signed "x.5"; the floor of the scale is a pin that
     * is not connected on this board, and the line says so. */
    const char *tsign = "";
    const char *tnote = "";
    int32_t     tabs  = temp;
    if ( 0 > tabs )
    {
        tsign = "-";
        tabs  = -tabs;
    }
    if ( -80 >= temp )
    {
        tnote = " (TS pin unconnected)";
    }
    KLOG_N(TAG, "test gauge: vcell %ldmV current %ldmA ave %ldmA (into the cell positive) temp %s%ld.%ldC%s",
           (long)vcell, (long)ma, (long)ave, tsign, (long)(tabs / 2),
           (long)((tabs % 2) * 5), tnote);
    const char *profile = "";
    if ( (soc >> 8) > 100 )
    {
        profile = " (no profile)";
    }
    KLOG_N(TAG, "test gauge: soc 0x%04lX%s soh %ld%% cycles %ld/16 update_flag=%ld",
           (unsigned long)soc, profile, (long)soh, (long)cycles, (long)upd);

    if ( ((int32_t)KBSP_CELL_VCUT_MV > vcell) || ((int32_t)KBSP_CELL_VTRIP_MV < vcell) )
    {
        KLOG_E(TAG, "test gauge: FAIL - vcell %ld mV is not a cell (%u..%u)",
               (long)vcell, (unsigned)KBSP_CELL_VCUT_MV, (unsigned)KBSP_CELL_VTRIP_MV);
        return false;
    }
    /* The second opinion. Two voltmeters on one cell agreeing is the proof
     * that either measures; the charger's is coarser, hence the margin. */
    if ( !kdrv_bq25896_present() || (kdrv_bq25896_adc_read(&adc) != KODE_OK) )
    {
        KLOG_E(TAG, "test gauge: FAIL - no PMIC reading to check the voltmeter against");
        return false;
    }
    int32_t dv = vcell - (int32_t)adc.vbat_mv;
    if ( 0 > dv )
    {
        dv = -dv;
    }
    if ( TEST_VCELL_AGREE_MV < dv )
    {
        KLOG_E(TAG, "test gauge: FAIL - vcell %ld mV against the charger's %u mV: "
                    "the two voltmeters disagree by more than %d mV",
               (long)vcell, (unsigned)adc.vbat_mv, TEST_VCELL_AGREE_MV);
        return false;
    }
    if ( AMMETER_SKIP == ammeter )
    {
        KLOG_N(TAG, "test gauge: the ammeter is not judged in this run");
        return true;
    }
    /* The ammeter needs current to be proven: the shunt open reads 0 mA,
     * which is exactly what a full cell reads too. */
    if ( (TEST_ICHG_PROVE_MA > adc.ichg_ma) && (AMMETER_IF_FLOWING == ammeter) )
    {
        /* The owner's row: a cell that is full takes no current, and that
         * is the state most units are in. The voltmeters agreed; the
         * ammeter is left unjudged rather than failed for it. */
        KLOG_N(TAG, "test gauge: the charger is delivering %u mA, too little "
                    "to prove the ammeter with; it is not judged",
               (unsigned)adc.ichg_ma);
        return true;
    }
    if ( TEST_ICHG_PROVE_MA > adc.ichg_ma )
    {
        KLOG_E(TAG, "test gauge: FAIL - the charger is delivering %u mA, under the "
                    "%u mA the ammeter needs to be proven: a cell that is not "
                    "full, on an adapter rather than a 500 mA host port",
               (unsigned)adc.ichg_ma, (unsigned)TEST_ICHG_PROVE_MA);
        return false;
    }
    int32_t const tol = TEST_ICHG_AGREE_MA + ((int32_t)adc.ichg_ma / 4);
    int32_t di = ma - (int32_t)adc.ichg_ma;
    if ( 0 > di )
    {
        di = -di;
    }
    if ( di > tol )
    {
        KLOG_E(TAG, "test gauge: FAIL - current %ld mA against the charger's %u mA "
                    "(tolerance %ld): the shunt is open, the wrong value, or "
                    "the wrong way round",
               (long)ma, (unsigned)adc.ichg_ma, (long)tol);
        return false;
    }
    return true;
}

/**
 * @brief Whether the gauge is there and its algorithm is not up yet.
 *
 * Read off the chip rather than the poll's flag, because a row can be asked
 * before the first poll has looked. A read that fails is not "coming up":
 * the test itself says what it is.
 *
 * @param me The AO.
 * @return true while the CW2217 answers READY = 0.
 */
static bool gauge_coming_up(Power const * const me)
{
    int32_t ready = 0;

    if ( !me->gauge )
    {
        return false;
    }
    if ( kdrv_cw2217_get(KDRV_CW_READY, &ready) != KODE_OK )
    {
        return false;
    }
    return 0 == ready;
}

/**
 * @brief Judge one chip's self-test and publish the verdict.
 *
 * @param me    The AO.
 * @param chip  POWER_CHIP_PMIC or POWER_CHIP_GAUGE.
 * @param depth POWER_DEPTH_*.
 */
static void power_test_answer(Power * const me, uint8_t chip, uint8_t depth)
{
    const char *name = "pmic";
    bool ok;

    if ( POWER_CHIP_GAUGE == chip )
    {
        uint8_t ammeter = AMMETER_PROVE;
        if ( POWER_DEPTH_ANSWER == depth )
        {
            ammeter = AMMETER_SKIP;
        }
        name = "gauge";
        ok = test_gauge(ammeter);
    }
    else
    {
        ok = test_pmic(me, depth);
    }
    TestResultEvt *ev = Q_NEW_X(TestResultEvt, 0U, TEST_RESULT_SIG);
    if ( NULL != ev )
    {
        (void)snprintf(ev->name, sizeof(ev->name), "%s", name);
        ev->ok = ok;
        ev->origin = TEST_ORIGIN_AUTOTEST;
        QACTIVE_PUBLISH(&ev->super, &me->super);
    }
}

/**
 * @brief Run one chip's self-test, or hold a gauge row until the chip is up.
 *
 * A gauge woken from shutdown (the cell just connected) takes about 1.5 s to
 * come up, and a row asked in that time would fail a chip that is fine. That
 * row waits: each poll looks again (gauge_test_due()), up to
 * KODE_GAUGE_READY_WAIT_MS from the ask.
 *
 * @param me    The AO.
 * @param chip  POWER_CHIP_PMIC or POWER_CHIP_GAUGE.
 * @param depth POWER_DEPTH_*.
 */
static void power_test(Power * const me, uint8_t chip, uint8_t depth)
{
    if ( POWER_CHIP_GAUGE == chip )
    {
        if ( me->gauge_owed )
        {
            /* Already waiting: the one answer that comes covers this ask. */
            return;
        }
        if ( gauge_coming_up(me) )
        {
            me->gauge_owed     = true;
            me->gauge_depth    = depth;
            me->gauge_asked_ms = khal_time_ms();
            KLOG_N(TAG, "test gauge: the algorithm is not up yet; waiting up to %u ms",
                   (unsigned)KODE_GAUGE_READY_WAIT_MS);
            return;
        }
    }
    power_test_answer(me, chip, depth);
}

/**
 * @brief Answer a waiting gauge row once the chip is up or the wait is over.
 *
 * Past the wait the row is judged as it stands, so a gauge that never comes
 * up still fails with its own reason ("not ready").
 *
 * @param me The AO.
 */
static void gauge_test_due(Power * const me)
{
    if ( !me->gauge_owed )
    {
        return;
    }
    if ( gauge_coming_up(me)
        && ((khal_time_ms() - me->gauge_asked_ms) < KODE_GAUGE_READY_WAIT_MS) )
    {
        return;
    }
    me->gauge_owed = false;
    power_test_answer(me, POWER_CHIP_GAUGE, me->gauge_depth);
}

/* ---- The `power_plug` row ------------------------------------------------ */

/**
 * @brief Publish one step of the row.
 *
 * @param step  1 for the charger going in, 2 for it coming out.
 * @param phase POWER_TEST_*.
 */
static void plug_publish(uint8_t step, uint8_t phase)
{
    PowerTestEvt *ev = Q_NEW_X(PowerTestEvt, 0U, POWER_TEST_SIG);

    if ( NULL != ev )
    {
        ev->step  = step;
        ev->total = 2U;
        ev->phase = phase;
        QACTIVE_PUBLISH(&ev->super, AO_Power);
    }
}

/**
 * @brief End the row with its answer.
 *
 * @param me  The AO.
 * @param sig TEST_RESULT_SIG for a verdict, TEST_SKIP_SIG when no charger
 *            was ever plugged in.
 * @param ok  The verdict.
 */
static void plug_end(Power * const me, enum_t sig, bool ok)
{
    TestResultEvt *ev = Q_NEW_X(TestResultEvt, 0U, sig);

    (void)QTimeEvt_disarm(&me->plug);
    me->plug_phase = PLUG_IDLE;
    if ( NULL != ev )
    {
        (void)snprintf(ev->name, sizeof(ev->name), "%s", "power");
        ev->ok = ok;
        ev->origin = TEST_ORIGIN_AUTOTEST;
        QACTIVE_PUBLISH(&ev->super, &me->super);
    }
    else
    {
        KLOG_E(TAG, "out of events; the power_plug verdict is lost");
    }
}

/**
 * @brief Whether the charger sees a good input right now.
 *
 * @param[out] vbus true when it does.
 * @return false when the PMIC would not be read.
 */
static bool plug_vbus(bool *vbus)
{
    kdrv_bq25896_adc_t adc;
    bool const ok = kdrv_bq25896_present()
                    && (kdrv_bq25896_adc_read(&adc) == KODE_OK);

    *vbus = false;
    if ( ok )
    {
        *vbus = adc.vbus_gd;
    }
    return ok;
}

/**
 * @brief Start the row: ask for the charger.
 *
 * @param me The AO.
 */
static void plug_start(Power * const me)
{
    if ( PLUG_IDLE != me->plug_phase )
    {
        KLOG_W(TAG, "test power_plug: already running; ignored");
        return;
    }
    KLOG_N(TAG, "test power_plug: step 1 of 2 - plug in the charger");
    plug_publish(1U, POWER_TEST_ASK_PLUG);
    me->plug_phase = PLUG_WAIT_IN;
    me->plug_looks = PLUG_LOOKS_STEP;
    QTimeEvt_armX(&me->plug, KQP_TICKS_PER_MS(KODE_POWER_PLUG_LOOK_MS),
                  KQP_TICKS_PER_MS(KODE_POWER_PLUG_LOOK_MS));
}

/**
 * @brief One look at the cable, and whatever it moves the row to.
 *
 * @param me The AO.
 */
static void plug_look(Power * const me)
{
    bool vbus = false;

    if ( PLUG_IDLE == me->plug_phase )
    {
        return;
    }
    if ( !plug_vbus(&vbus) )
    {
        KLOG_E(TAG, "test power_plug: FAIL - the charger chip does not answer");
        plug_publish(1U, POWER_TEST_FAIL);
        plug_end(me, (enum_t)TEST_RESULT_SIG, false);
        return;
    }
    if ( 0U < me->plug_looks )
    {
        me->plug_looks -= 1U;
    }

    if ( PLUG_WAIT_IN == me->plug_phase )
    {
        if ( vbus )
        {
            KLOG_N(TAG, "test power_plug: the charger is in");
            /* The ADC converts once per poll: start one now, so the settle's
             * verdict reads VBUS with the charger in. */
            adc_start();
            plug_publish(1U, POWER_TEST_PLUGGED);
            me->plug_phase = PLUG_SETTLE;
            me->plug_looks = PLUG_LOOKS_SETTLE;
        }
        else if ( 0U == me->plug_looks )
        {
            KLOG_N(TAG, "test power_plug: no charger in %u ms; nothing to judge",
                   (unsigned)KODE_POWER_PLUG_STEP_MS);
            plug_end(me, (enum_t)TEST_SKIP_SIG, false);
        }
        else
        {
            /* Still waiting for it. */
        }
    }
    else if ( PLUG_SETTLE == me->plug_phase )
    {
        if ( 0U == me->plug_looks )
        {
            bool const pmic_ok  = test_pmic(me, POWER_DEPTH_FULL);
            bool const gauge_ok = test_gauge(AMMETER_IF_FLOWING);

            if ( pmic_ok && gauge_ok )
            {
                plug_publish(1U, POWER_TEST_PASS);
                KLOG_N(TAG, "test power_plug: step 2 of 2 - unplug the charger");
                plug_publish(2U, POWER_TEST_ASK_UNPLUG);
                me->plug_phase = PLUG_WAIT_OUT;
                me->plug_looks = PLUG_LOOKS_STEP;
            }
            else
            {
                plug_publish(1U, POWER_TEST_FAIL);
                plug_end(me, (enum_t)TEST_RESULT_SIG, false);
            }
        }
    }
    else
    {
        /* PLUG_WAIT_OUT. */
        if ( !vbus )
        {
            KLOG_N(TAG, "test power_plug: the charger is out, and the chip saw "
                        "it go");
            plug_publish(2U, POWER_TEST_PASS);
            plug_end(me, (enum_t)TEST_RESULT_SIG, true);
        }
        else if ( 0U == me->plug_looks )
        {
            KLOG_E(TAG, "test power_plug: nobody unplugged the charger in %u ms",
                   (unsigned)KODE_POWER_PLUG_STEP_MS);
            plug_publish(2U, POWER_TEST_TIMEOUT);
            plug_end(me, (enum_t)TEST_RESULT_SIG, false);
        }
        else
        {
            /* Still waiting for it to go. */
        }
    }
}

/* ---- Control requests ---------------------------------------------------- */

/** @brief Publish the answer to one control request. Bounded, never asserts. */
static void ctrl_answer(Power * const me, uint8_t op, kode_status_t rc, int32_t value)
{
    PowerCtrlEvt *ev = Q_NEW_X(PowerCtrlEvt, 0U, POWER_CTRL_SIG);
    if ( NULL != ev )
    {
        ev->op    = op;
        ev->rc    = (uint8_t)rc;
        ev->value = value;
        QACTIVE_PUBLISH(&ev->super, &me->super);
    }
}

/**
 * @brief Write charging as the last request wants it, and read it back.
 * @param[out] back What the chip holds afterwards.
 */
static kode_status_t charge_apply(Power const * const me, int32_t *back)
{
    int32_t want = 0;

    if ( me->charge_wanted && !me->charge_held )
    {
        want = 1;
    }
    kode_status_t rc = kdrv_bq25896_set(KDRV_BQ_CHG_CONFIG, want);
    if ( KODE_OK == rc )
    {
        rc = kdrv_bq25896_get(KDRV_BQ_CHG_CONFIG, back);
    }
    return rc;
}

/** @brief `charge state=on|off`: the bit is the state, so no wait. */
static QState ctrl_charge(Power * const me, bool on)
{
    int32_t back = 0;

    me->charge_wanted = on;
    kode_status_t const rc = charge_apply(me, &back);
    if ( (KODE_OK == rc) && on && me->charge_held )
    {
        KLOG_N(TAG, "charging wanted, held by temperature; it resumes when "
                    "TS is back in the pack's window");
    }
    ctrl_answer(me, POWER_CTRL_CHARGE, rc, back);
    return Q_HANDLED();
}

/** @brief `charge_limit ma=`: the pack's ceiling, then the chip's grid. */
static QState ctrl_ichg(Power * const me, int32_t ma)
{
    int32_t       back = 0;
    kode_status_t rc   = KODE_DENIED;

    if ( (ICHG_FLOOR_MA > ma) || ((int32_t)KBSP_CELL_ICHG_MAX_MA < ma) )
    {
        KLOG_W(TAG, "charge limit %ld mA refused: the pack takes %d to %u mA",
               (long)ma, ICHG_FLOOR_MA, (unsigned)KBSP_CELL_ICHG_MAX_MA);
    }
    else
    {
        rc = kdrv_bq25896_set(KDRV_BQ_ICHG_MA, ma);
        if ( KODE_OK == rc )
        {
            rc = kdrv_bq25896_get(KDRV_BQ_ICHG_MA, &back);
        }
        if ( KODE_OK == rc )
        {
            me->ichg_ma = back;   /* what the policy row holds the chip to */
        }
    }
    ctrl_answer(me, POWER_CTRL_ICHG, rc, back);
    return Q_HANDLED();
}

/**
 * @brief Write the stored charge current and charging over the policy's.
 *
 * Called right after the policy took, at boot and after a register reset:
 * the policy states the cell's profile, and the two settings then say what
 * the user chose within it.
 *
 * @param me The AO.
 */
static void power_settings_apply(Power * const me)
{
    int32_t back = 0;

    me->charge_wanted = setting_bool(KSET_KEY_CHARGE, true, NULL);
    me->ichg_ma       = setting_ichg(NULL);
    if ( (kdrv_bq25896_set(KDRV_BQ_ICHG_MA, me->ichg_ma) == KODE_OK)
        && (kdrv_bq25896_get(KDRV_BQ_ICHG_MA, &back) == KODE_OK) )
    {
        me->ichg_ma = back;
    }
    if ( charge_apply(me, &back) != KODE_OK )
    {
        KLOG_E(TAG, "CHG_CONFIG would not take the stored `charge`");
    }
    if ( !me->charge_wanted || ((int32_t)KBSP_CELL_ICHG_MA != me->ichg_ma) )
    {
        KLOG_N(TAG, "settings: charging %s at %ld mA",
               on_word(me->charge_wanted), (long)me->ichg_ma);
    }
}

/**
 * @brief Answer `config` for `led_activity`, `charge` or `charge_ma`, setting
 *        it first when asked.
 *
 * A set is applied at once and stored, even when the PMIC did not answer.
 * `charge_ma` is stored on the chip's 64 mA grid (500 reads back 512). The
 * facade's charge calls change the chip, not these settings.
 *
 * @param me The AO.
 * @param r  The request.
 */
static void power_config(Power * const me, KodeConfigReqEvt const * const r)
{
    bool    stored = false;
    int32_t back   = 0;
    char    word[KODE_CONFIG_VALUE_MAX + 1U];

    if ( strcmp(r->key, KSET_KEY_LED_ACT) == 0 )
    {
        if ( r->set )
        {
            me->led_activity = r->b;
            if ( !r->b )
            {
                led_write_step(me, false);   /* a pulse in flight goes dark */
            }
            (void)khal_nvs_set_u8(KSET_NAMESPACE, KSET_KEY_LED_ACT, bool_u8(r->b));
            KLOG_N(TAG, "led_activity %s (stored)", on_word(r->b));
        }
        bool const on = setting_bool(KSET_KEY_LED_ACT, true, &stored);
        (void)snprintf(word, sizeof(word), "%s", on_word(on));
    }
    else if ( strcmp(r->key, KSET_KEY_CHARGE) == 0 )
    {
        if ( r->set )
        {
            me->charge_wanted = r->b;
            if ( me->pmic )
            {
                (void)charge_apply(me, &back);
            }
            (void)khal_nvs_set_u8(KSET_NAMESPACE, KSET_KEY_CHARGE, bool_u8(r->b));
            KLOG_N(TAG, "charge %s (stored)", on_word(r->b));
        }
        bool const on = setting_bool(KSET_KEY_CHARGE, true, &stored);
        (void)snprintf(word, sizeof(word), "%s", on_word(on));
    }
    else if ( strcmp(r->key, KSET_KEY_CHG_MA) == 0 )
    {
        if ( r->set )
        {
            uint16_t ma = (uint16_t)r->i;

            if ( me->pmic
                && (kdrv_bq25896_set(KDRV_BQ_ICHG_MA, r->i) == KODE_OK)
                && (kdrv_bq25896_get(KDRV_BQ_ICHG_MA, &back) == KODE_OK) )
            {
                me->ichg_ma = back;
                ma = (uint16_t)back;
            }
            (void)khal_nvs_set_blob(KSET_NAMESPACE, KSET_KEY_CHG_MA, &ma,
                                    sizeof(ma));
            KLOG_N(TAG, "charge_ma %u (stored)", (unsigned)ma);
        }
        (void)snprintf(word, sizeof(word), "%ld", (long)setting_ichg(&stored));
    }
    else
    {
        return;   /* another owner's key */
    }
    (void)kode_config_answer(&me->super, r, KODE_OK, stored, word);
}

/**
 * @brief Clear OTG_CONFIG, and forget the wish only once the chip took it.
 *
 * The wish is what the poll's guard watches, so it outlives a write that did
 * not land: a boost the owner believed it had stopped, still running because
 * one I2C write failed, would drain the pack through the header with nobody
 * looking again. Forgotten on success, kept on failure, retried next poll.
 */
static void boost_disarm(Power * const me)
{
    if ( kdrv_bq25896_set(KDRV_BQ_OTG_CONFIG, 0) == KODE_OK )
    {
        me->boost_wanted = false;
    }
    else
    {
        KLOG_E(TAG, "OTG_CONFIG would not clear: the boost is still watched "
                    "and the next poll tries again");
    }
}

/**
 * @brief Whether a boost may start now, and why not.
 *
 * Refused while USB is present (an armed bit would start the boost when the
 * cable leaves) and while the cell is unread or under the pack's floor.
 *
 * @param me The AO.
 * @return KODE_OK; KODE_DENIED for a cable or a cell; KODE_ERROR when the
 *         charger did not answer.
 */
static kode_status_t boost_refusal(Power const * const me)
{
    int32_t       pg = 0;
    kode_status_t rc = KODE_OK;

    if ( kdrv_bq25896_get(KDRV_BQ_PG_STAT, &pg) != KODE_OK )
    {
        rc = KODE_ERROR;
    }
    else if ( 0 != pg )
    {
        KLOG_W(TAG, "boost refused: USB present, the OTG pin is held low by hardware");
        rc = KODE_DENIED;
    }
    else if ( 0U == me->vbat_mv )
    {
        KLOG_W(TAG, "boost refused: no battery reading yet");
        rc = KODE_DENIED;
    }
    else if ( KBSP_CELL_BOOST_MIN_MV > me->vbat_mv )
    {
        KLOG_W(TAG, "boost refused: cell at %u mV, under the %u mV floor",
               (unsigned)me->vbat_mv, (unsigned)KBSP_CELL_BOOST_MIN_MV);
        rc = KODE_DENIED;
    }
    else
    {
        /* Nothing stands in the way. */
    }
    return rc;
}

/**
 * @brief Put the board's BOOST_LIM back, whatever the delivery test left.
 *
 * A write that does not land keeps the flag, so the next poll's look at the
 * cable tries again.
 *
 * @param me The AO.
 */
static void boost_lim_restore(Power * const me)
{
    if ( kdrv_bq25896_set(KDRV_BQ_BOOST_LIM, (int32_t)KBSP_CELL_BOOST_LIM_CODE) == KODE_OK )
    {
        if ( me->lim_raised )
        {
            KLOG_N(TAG, "boost limit back to the board's");
        }
        me->lim_raised = false;
    }
    else
    {
        KLOG_E(TAG, "BOOST_LIM would not go back to the board's; tried again next poll");
    }
}

/**
 * @brief The pads delivery test's boost: on raises BOOST_LIM and starts it,
 *        off stops it and puts the limit back.
 *
 * The answer waits for the next poll, which carries the load's current; a
 * refusal is answered at once. Nothing is stored: the board's limit comes back
 * on the cable's fall, at AO_Power's start and at the station's end.
 *
 * @param me The AO.
 * @param r  The request.
 */
static void boost_test(Power * const me, PowerBoostTestReqEvt const * const r)
{
    kode_status_t rc = KODE_OK;

    if ( r->on )
    {
        rc = boost_refusal(me);
        if ( KODE_OK == rc )
        {
            rc = kdrv_bq25896_set(KDRV_BQ_BOOST_LIM, (int32_t)KODE_TEST_DOT_BOOST_LIM_CODE);
            me->lim_raised = true;
        }
        if ( KODE_OK == rc )
        {
            rc = kdrv_bq25896_set(KDRV_BQ_OTG_CONFIG, 1);
        }
        if ( KODE_OK == rc )
        {
            me->boost_wanted = true;
            KLOG_N(TAG, "boost on for the delivery test, the limit raised");
        }
        else if ( me->lim_raised )
        {
            boost_lim_restore(me);
        }
        else
        {
            /* Refused before anything was written. */
        }
    }
    else
    {
        boost_disarm(me);
        boost_lim_restore(me);
        if ( me->boost_wanted || me->lim_raised )
        {
            rc = KODE_IO_ERROR;
        }
    }
    me->bt_who    = r->who;
    me->bt_status = (uint8_t)rc;
    if ( KODE_OK != rc )
    {
        PowerBoostTestEvt * const ev = Q_NEW_X(PowerBoostTestEvt, 0U, POWER_BOOST_TEST_SIG);

        me->bt_who = NULL;
        if ( NULL != ev )
        {
            ev->status     = (uint8_t)rc;
            ev->boost      = false;
            ev->fault      = 0U;
            ev->current_ma = 0;
            QACTIVE_POST(r->who, &ev->super, &me->super);
        }
    }
}

/**
 * @brief After a reading: answer the delivery test's boost with it, once.
 * @param me The AO.
 * @param r  The reading.
 */
static void boost_test_answer(Power * const me, const reading_t *r)
{
    QActive * const who = me->bt_who;

    if ( NULL != who )
    {
        PowerBoostTestEvt * const ev = Q_NEW_X(PowerBoostTestEvt, 0U, POWER_BOOST_TEST_SIG);

        me->bt_who = NULL;
        if ( NULL != ev )
        {
            ev->status     = me->bt_status;
            ev->boost      = r->boost;
            ev->fault      = r->fault;
            ev->current_ma = (int16_t)r->current_ma;
            QACTIVE_POST(who, &ev->super, &me->super);
        }
    }
}

/**
 * @brief `boost state=on|off`: refuse what cannot work, write the bit, wait.
 *
 * On is refused without a write while USB is present, because an armed bit
 * starts the boost the instant the cable leaves and nobody asked for that,
 * and while the cell is under the pack's floor. Both are the owner's to
 * know; the chip would have taken the write.
 */
static QState ctrl_boost(Power * const me, bool on)
{
    int32_t bit = 0;

    if ( on )
    {
        kode_status_t const why = boost_refusal(me);

        if ( KODE_OK != why )
        {
            ctrl_answer(me, POWER_CTRL_BOOST, why, 0);
            return Q_HANDLED();
        }
        bit = 1;
    }
    if ( kdrv_bq25896_set(KDRV_BQ_OTG_CONFIG, bit) != KODE_OK )
    {
        ctrl_answer(me, POWER_CTRL_BOOST, KODE_ERROR, 0);
        return Q_HANDLED();
    }
    me->boost_wanted = on;
    me->ctrl_op      = POWER_CTRL_BOOST;
    me->ctrl_value   = bit;
    me->ctrl_tries   = 1U;
    return Q_TRAN(&Power_waiting);
}

/** @brief `pmic_reset`: write REG_RST, then wait for the chip to say it is done. */
static QState ctrl_reset(Power * const me)
{
    if ( kdrv_bq25896_set(KDRV_BQ_REG_RST, 1) != KODE_OK )
    {
        ctrl_answer(me, POWER_CTRL_RESET, KODE_ERROR, 0);
        return Q_HANDLED();
    }
    me->ctrl_op    = POWER_CTRL_RESET;
    me->ctrl_value = 0;
    me->ctrl_tries = 1U;
    return Q_TRAN(&Power_waiting);
}

/**
 * @brief After a reading, while a boost is wanted: the two reasons to stop it.
 *
 * Under the pack's floor the cell is what is protected; on a latched
 * BOOST_FAULT the chip is hiccupping into an overload, an over-voltage or a
 * cell too low, and a converter left hiccupping on a battery is nobody's
 * intention.
 */
static void boost_guard(Power * const me, const reading_t *r)
{
    if ( !me->boost_wanted )
    {
        return;
    }
    if ( KBSP_CELL_BOOST_STOP_MV > r->vbat_mv )
    {
        KLOG_W(TAG, "boost off: cell at %u mV, under the %u mV floor",
               (unsigned)r->vbat_mv, (unsigned)KBSP_CELL_BOOST_STOP_MV);
        boost_disarm(me);
    }
    else if ( r->boost_fault )
    {
        KLOG_W(TAG, "boost off: BOOST_FAULT (overload or over-voltage on the "
                    "5 V rail, or the cell too low)");
        boost_disarm(me);
    }
    else
    {
        /* Boosting as asked. */
    }
}

/**
 * @brief After a reading: hold charging outside the pack's window.
 *
 * The pack charges between 10 and 45 C (WW-20260822-01), stricter than the
 * chip's JEITA profile. NTC_FAULT carries the chip's hysteresis, so this does
 * not chatter. The hold goes through charge_apply() and is recorded only once
 * the chip took the write, or the pack could charge out of its window unseen.
 */
static void charge_guard(Power * const me, const reading_t *r)
{
    bool const out  = (0U != r->ntc);
    bool const was  = me->charge_held;
    int32_t    back = 0;

    if ( out == was )
    {
        return;                       /* nothing moved */
    }
    me->charge_held = out;
    if ( !me->charge_wanted )
    {
        return;                       /* charging is off by request already */
    }
    if ( charge_apply(me, &back) != KODE_OK )
    {
        me->charge_held = was;
        KLOG_E(TAG, "CHG_CONFIG would not take the temperature guard's write; "
                    "the next poll tries again");
        return;
    }
    if ( out )
    {
        KLOG_W(TAG, "charging held: TS reports %s; the pack charges between "
                    "10 and 45 C", ntc_name(r->ntc));
    }
    else
    {
        KLOG_N(TAG, "charging resumed: TS back in the pack's window");
    }
}

/**
 * @brief Name the one cause of a boost that did not start, and disarm it.
 *
 * The OTG pin has no register, so the last line is by elimination: everything
 * the chip can report is clear, so it is the pin.
 */
static void boost_explain(Power * const me)
{
    kdrv_bq25896_faults_t live;
    int32_t               pg = 0;

    live.ntc   = 0U;
    live.boost = false;
    (void)kdrv_bq25896_get(KDRV_BQ_PG_STAT, &pg);
    (void)kdrv_bq25896_faults(NULL, &live);
    if ( 0 != pg )
    {
        KLOG_W(TAG, "boost did not start: USB arrived meanwhile");
    }
    else if ( 0U != live.ntc )
    {
        KLOG_W(TAG, "boost did not start: TS outside the boost window (no pack, "
                    "or the cell under -10 or over 55 C)");
    }
    else if ( live.boost )
    {
        KLOG_W(TAG, "boost did not start: boost fault (overload, over-voltage "
                    "or the cell too low)");
    }
    else
    {
        KLOG_W(TAG, "boost did not start: no fault and no boost, the OTG pin "
                    "is low - measure TP33");
    }
    boost_disarm(me);
}

/** @brief The boost's wait is over: is the chip doing what was asked? */
static void boost_settled(Power * const me)
{
    int32_t stat = 0;
    int32_t is   = 0;

    if ( kdrv_bq25896_get(KDRV_BQ_VBUS_STAT, &stat) != KODE_OK )
    {
        ctrl_answer(me, POWER_CTRL_BOOST, KODE_ERROR, 0);
        return;
    }
    bool const boosting = ((int32_t)KDRV_BQ_VBUS_OTG == stat);
    bool const asked    = (0 != me->ctrl_value);
    if ( boosting )
    {
        is = 1;
    }
    if ( boosting == asked )
    {
        if ( boosting )
        {
            KLOG_N(TAG, "boost on: 5 V on the header from the battery, 0.5 A");
        }
        else
        {
            KLOG_N(TAG, "boost off");
        }
        ctrl_answer(me, POWER_CTRL_BOOST, KODE_OK, is);
        return;
    }
    if ( asked )
    {
        boost_explain(me);
    }
    else
    {
        KLOG_W(TAG, "boost still running after OTG_CONFIG was cleared");
        /* Written again, and if that write does not land either the wish
         * stays and the poll's guard keeps trying: a converter nobody can
         * stop is exactly what must not be left unwatched. */
        boost_disarm(me);
    }
    ctrl_answer(me, POWER_CTRL_BOOST, KODE_ERROR, is);
}

/**
 * @brief One poll of REG_RST. True when the wait is over, answered either way.
 *
 * Only a bit that reads 0 gets the policy: applied to a chip still resetting
 * it would be the write the reset undoes. The three wishes go back to their
 * boot values with the policy, which is what "reset" means here.
 */
static bool reset_settled(Power * const me)
{
    int32_t bit = 1;

    if ( kdrv_bq25896_get(KDRV_BQ_REG_RST, &bit) != KODE_OK )
    {
        ctrl_answer(me, POWER_CTRL_RESET, KODE_ERROR, 0);
        return true;
    }
    if ( 0 == bit )
    {
        kode_status_t rc = KODE_ERROR;
        me->boost_wanted  = false;
        me->charge_held   = false;
        if ( apply_policy() )
        {
            power_settings_apply(me);
            rc = KODE_OK;
            KLOG_N(TAG, "registers reset; the owner's policy is applied again");
        }
        else
        {
            KLOG_E(TAG, "registers reset, but the owner's policy did not take: "
                        "the chip is at its defaults");
        }
        ctrl_answer(me, POWER_CTRL_RESET, rc, 0);
        return true;
    }
    if ( CTRL_RESET_TRIES <= me->ctrl_tries )
    {
        KLOG_E(TAG, "REG_RST never cleared");
        ctrl_answer(me, POWER_CTRL_RESET, KODE_TIMEOUT, 0);
        return true;
    }
    ++me->ctrl_tries;
    return false;
}

/**
 * @brief One control request, dispatched by its op.
 *
 * A board whose PMIC never took its policy refuses all four, once and with a
 * line saying why: the console's `>err code=3` points at the log for the
 * reason, and silence there leaves nothing to point at.
 */
static QState power_ctrl(Power * const me, PowerCtrlReqEvt const *q)
{
    QState status;

    if ( !me->pmic )
    {
        KLOG_W(TAG, "no PMIC answered at boot; the operation is refused");
        ctrl_answer(me, q->op, KODE_DENIED, 0);
        return Q_HANDLED();
    }
    switch ( q->op )
    {
        case POWER_CTRL_CHARGE: {
            status = ctrl_charge(me, 0 != q->value);
            break;
        }
        case POWER_CTRL_ICHG: {
            status = ctrl_ichg(me, q->value);
            break;
        }
        case POWER_CTRL_BOOST: {
            status = ctrl_boost(me, 0 != q->value);
            break;
        }
        case POWER_CTRL_RESET: {
            status = ctrl_reset(me);
            break;
        }
        default: {
            ctrl_answer(me, q->op, KODE_DENIED, 0);
            status = Q_HANDLED();
            break;
        }
    }
    return status;
}

/* ---- Shipping after the cable leaves ------------------------------------- */

/**
 * @brief Start counting KODE_SHIP_UNPLUG_MS: the cable is out and someone asked.
 * @param me The AO.
 */
static void ship_start(Power * const me)
{
    me->ship_counting = true;
    QTimeEvt_armX(&me->ship, KQP_TICKS_PER_MS(KODE_SHIP_UNPLUG_MS), 0U);
    KLOG_N(TAG, "ship: cable out, telling the asker in %u s",
           KODE_SHIP_UNPLUG_S);
}

/**
 * @brief Take a new arming: remember who, and count at once if the cable is out.
 *
 * "Out" is what the last poll read. Before any poll has run nothing is known,
 * so the AO waits for the first one rather than guess. A second arming while
 * a countdown runs replaces the asker and leaves the countdown alone.
 *
 * @param me  The AO.
 * @param who The Active Object to tell.
 */
static void ship_arm(Power * const me, QActive * const who)
{
    me->ship_who   = who;
    me->ship_armed = true;
    KLOG_N(TAG, "ship: armed; will tell the asker %u s after the cable leaves",
           KODE_SHIP_UNPLUG_S);
    if ( !me->ship_counting && me->vbus_known && !me->vbus_last )
    {
        ship_start(me);
    }
}

/**
 * @brief Feed one poll's reading of the cable to the countdown.
 *
 * The countdown starts at the poll that sees the cable gone, within one
 * KODE_POWER_POLL_MS of the unplug. A poll that sees it back cancels it and
 * AO_Power stays armed; the next unplug counts the whole KODE_SHIP_UNPLUG_MS
 * again (the E5 station's replug, spec 6.3).
 *
 * @param me   The AO.
 * @param vbus Whether the poll found the input good.
 */
static void ship_watch(Power * const me, bool vbus)
{
    me->vbus_known = true;
    me->vbus_last  = vbus;
    if ( !me->ship_armed )
    {
        return;
    }
    if ( vbus && me->ship_counting )
    {
        me->ship_counting = false;
        (void)QTimeEvt_disarm(&me->ship);
        KLOG_N(TAG, "ship: cable back, countdown cancelled; still armed, the next unplug counts %u s again",
               KODE_SHIP_UNPLUG_S);
    }
    else if ( !vbus && !me->ship_counting )
    {
        ship_start(me);
    }
    else
    {
        /* The cable as it was: nothing changes. */
    }
}

/**
 * @brief The countdown is over: tell the asker once and disarm.
 *
 * AO_Power does not ship by itself; the asker writes what must be written
 * and then asks power_ship_request().
 *
 * @param me The AO.
 */
static void ship_due(Power * const me)
{
    static const QEvt due = QEVT_INITIALIZER(POWER_SHIP_DUE_SIG);
    QActive * const who = me->ship_who;

    if ( !me->ship_counting )
    {
        return;            /* a late tick of a countdown that was cancelled */
    }
    me->ship_counting = false;
    me->ship_armed    = false;
    me->ship_who      = NULL;
    KLOG_N(TAG, "ship: due, the cable has been out for %u s; telling the asker",
           KODE_SHIP_UNPLUG_S);
    if ( NULL != who )
    {
        QACTIVE_POST(who, &due, &me->super);
    }
}

/* ---- The state machine --------------------------------------------------- */

#if CONFIG_KODE_LOWPOWER
/**
 * @brief Why the chips cannot rest now.
 *
 * @param me The AO.
 * @return The busy reason, or NULL when nothing holds them.
 */
static const char *power_rest_refusal(Power const * const me)
{
    const char *why = NULL;

    if ( me->writing )
    {
        why = "flash write";
    }
    else if ( PLUG_IDLE != me->plug_phase )
    {
        why = "power plug test";
    }
    else if ( me->ship_armed )
    {
        /* Resting stops the poll that watches the cable leave. */
        why = "ship countdown";
    }
    else
    {
        /* Free. */
    }
    return why;
}

/**
 * @brief LOWPOWER_ROUND_SIG: the ADC back to one-shot, and answer once.
 *
 * @param me The AO.
 * @param r  The round.
 */
static void power_round(Power * const me, const LowPowerRoundEvt * const r)
{
    const char *const why = power_rest_refusal(me);

    if ( NULL != why )
    {
        lowpower_ready(me->lp_who, r->round, true, why);
    }
    else
    {
        if ( me->pmic )
        {
            if ( kdrv_bq25896_set(KDRV_BQ_CONV_RATE, 0) != KODE_OK )
            {
                KLOG_W(TAG, "the ADC would not go back to one-shot for the board's sleep");
            }
        }
        me->rested = true;
        if ( (uint8_t)KODE_LP_OFF == r->target )
        {
            /* The clock is on the store before the deep sleep. */
            (void)kdrv_clocks_wall_keep(true);
        }
        lowpower_ready(me->lp_who, r->round, false, NULL);
    }
}

/**
 * @brief LOWPOWER_STATE_SIG: on AWAKE, start the conversion the first poll reads.
 *
 * Idempotent: it acts only on an AO that rested, once.
 *
 * @param me The AO.
 * @param s  The change.
 */
static void power_state(Power * const me, const LowPowerStateEvt * const s)
{
    if ( ((uint8_t)KODE_LP_AWAKE == s->state) && me->rested )
    {
        me->rested = false;
        if ( me->pmic )
        {
            adc_start();
        }
    }
}
#endif /* CONFIG_KODE_LOWPOWER */

/**
 * @brief Claim the PMIC and the gauge, bring up whichever is fitted, and start polling.
 *
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to Power_polling.
 */
static QState Power_initial(Power * const me, void const * const par)
{
    (void)par;

    kaos_claim(KBSP_RES_PMIC,      me, "AO_Power");
    kaos_claim(KBSP_RES_GAUGE,     me, "AO_Power");

    /* The AO subscribes to the flash signals because whether flash is being
     * written belongs to AO_AppMgr; polling it would cost a request every two
     * seconds for something it already announces. */
    QActive_subscribe(&me->super, APP_FLASH_PROGRESS_SIG);
    QActive_subscribe(&me->super, APP_FLASH_DONE_SIG);
    QActive_subscribe(&me->super, APP_FLASH_FAIL_SIG);
#if CONFIG_KODE_LOWPOWER
    me->lp_who = lowpower_join(&me->super, "power");
    if ( KODE_LOWPOWER_WHO_NONE != me->lp_who )
    {
        QActive_subscribe(&me->super, LOWPOWER_ROUND_SIG);
        QActive_subscribe(&me->super, LOWPOWER_STATE_SIG);
    }
#endif

    me->led_activity = setting_bool(KSET_KEY_LED_ACT, true, NULL);
    me->pmic = (kdrv_bq25896_init() == KODE_OK) && apply_policy();
    if ( me->pmic )
    {
        power_settings_apply(me);
    }
    else
    {
        KLOG_W(TAG, "no PMIC; `bat_info` will have nothing to report");
    }

    /* The gauge is optional: it is powered from the cell, so on a board with
     * no battery it simply is not there. Woken if it is in shutdown, left
     * running if a reset found it so; never waited for. */
    me->gauge = (kdrv_cw2217_init() == KODE_OK);
    if ( me->gauge && (kdrv_cw2217_wake() != KODE_OK) )
    {
        KLOG_W(TAG, "the gauge did not take its wake sequence");
        me->gauge = false;
    }
    return Q_TRAN(&Power_polling);
}

/**
 * @brief Whether the chips are rested for the board's sleep.
 *
 * @param me The AO.
 * @return true between the round that rested them and the AWAKE.
 */
static bool power_resting(Power const * const me)
{
#if CONFIG_KODE_LOWPOWER
    return me->rested;
#else
    (void)me;
    return false;
#endif
}

/**
 * @brief One poll: the LED guard, the wall clock, the reading and its guards.
 *
 * @param me The AO.
 */
static void power_poll(Power * const me)
{
    reading_t r;
    uint8_t   soc = 0U;
    /* A write whose host died stops reporting and never says it
     * ended, so the LED is taken back here rather than left pulsing
     * until the next boot. */
    if ( me->writing
        && ((khal_time_ms() - me->write_ms) >= POWER_LED_WRITE_MS) )
    {
        led_write_step(me, false);
    }
    /* The supply this AO switches off takes the RTC with it, so this
     * AO keeps the wall clock: a check every poll, a write when one
     * is due (kdrv_clocks_wall_keep()). */
    (void)kdrv_clocks_wall_keep(false);
    if ( power_read(me, &r, &soc) )
    {
        if ( 0U != r.fault )
        {
            KLOG_W(TAG, "PMIC fault register is 0x%02X", (unsigned)r.fault);
        }
        boost_guard(me, &r);
        charge_guard(me, &r);
        /* The delivery test's limit never outlives the cable's fall: the
         * board leaves the station with the customer's 0.5 A. */
        if ( me->lim_raised && me->vbus_known && me->vbus_last && !r.vbus )
        {
            boost_lim_restore(me);
        }
        boost_test_answer(me, &r);
        ship_watch(me, r.vbus);
    }
    if ( me->pmic )
    {
        adc_start();   /* what the next poll reads */
    }
}

/**
 * @brief Polling the chips on a timer and serving every request; Power_waiting's superstate.
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState Power_polling(Power * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            /* First shot after the ADC has had time to convert; every one
             * after that on the ordinary period. */
            QTimeEvt_armX(&me->poll,
                          KQP_TICKS_PER_MS(KODE_POWER_FIRST_MS),
                          KQP_TICKS_PER_MS(KODE_POWER_POLL_MS));
            status = Q_HANDLED();
            break;
        }
        case Q_EXIT_SIG: {
            (void)QTimeEvt_disarm(&me->poll);
            (void)QTimeEvt_disarm(&me->pulse);
            (void)QTimeEvt_disarm(&me->ship);
            status = Q_HANDLED();
            break;
        }
        case POWER_POLL_SIG: {
            /* A radio wake runs the tick while the ADC is rested: its
             * registers are stale and the minute's peek keeps the clock, so
             * there is nothing to read. */
            if ( !power_resting(me) )
            {
                power_poll(me);
            }
            gauge_test_due(me);
            status = Q_HANDLED();
            break;
        }
        case POWER_PULSE_SIG: {
            if ( me->led_done )
            {
                me->led_done = false;
                led_publish(me, false);
            }
            else
            {
                me->pulse_lit = !me->pulse_lit;
                led_publish(me, me->pulse_lit);
            }
            status = Q_HANDLED();
            break;
        }
        case APP_FLASH_PROGRESS_SIG: {
            me->write_ms = khal_time_ms();
            led_write_step(me, me->led_activity);
            status = Q_HANDLED();
            break;
        }
        case APP_FLASH_DONE_SIG: {
            bool const was = me->writing;

            led_write_step(me, false);
            if ( was )
            {
                me->led_done = true;
                led_say(me, POWER_LED_GREEN);
                QTimeEvt_armX(&me->pulse, KQP_TICKS_PER_MS(POWER_LED_DONE_MS), 0U);
            }
            status = Q_HANDLED();
            break;
        }
        case APP_FLASH_FAIL_SIG: {
            led_write_step(me, false);
            status = Q_HANDLED();
            break;
        }
        case POWER_SHIP_REQ_SIG: {
            /* Say everything BEFORE the write: on battery the board is dead
             * the moment it lands, and a log line after it would never leave
             * the UART. The same rule reaches the other owners on the bus -
             * every AO above this one's priority gets one whole step out of
             * this publish, which is enough to put a device away. Nobody
             * answers it and nothing waits for them. */
            {
                static const QEvt going = QEVT_INITIALIZER(POWER_SHIP_SIG);
                QACTIVE_PUBLISH(&going, &me->super);
            }
            /* The clock goes with the supply: kept now, due or not. */
            (void)kdrv_clocks_wall_keep(true);
            KLOG_N(TAG, "ship mode: opening the BATFET");
            KLOG_N(TAG, "wake with BUTTON_BOT (QON) or by plugging USB");
            kode_status_t rc = KODE_DENIED;
            if ( me->pmic )
            {
                rc = kdrv_bq25896_ship_mode(false);
            }
            /* Still here: either USB is holding VSYS up, or the write failed. */
            if ( KODE_OK == rc )
            {
                KLOG_N(TAG, "battery path open; VBUS is keeping the board up "
                            "- it powers off when USB is unplugged");
            }
            else
            {
                KLOG_E(TAG, "ship mode FAILED (%d): the board is NOT going to "
                            "switch off", (int)rc);
            }
            status = Q_HANDLED();
            break;
        }
        case POWER_BOOST_TEST_REQ_SIG: {
            boost_test(me, Q_EVT_CAST(PowerBoostTestReqEvt));
            status = Q_HANDLED();
            break;
        }
        case POWER_SHIP_ARM_SIG: {
            ship_arm(me, Q_EVT_CAST(PowerShipArmEvt)->who);
            status = Q_HANDLED();
            break;
        }
        case POWER_SHIP_TMO_SIG: {
            ship_due(me);
            status = Q_HANDLED();
            break;
        }
        case POWER_DUMP_REQ_SIG: {
            reading_t r;
            uint8_t   soc = 0U;
            /* A fresh reading, published now: the console closes `bat_info`
             * with the next POWER_EVT_SIG it sees, and it should not have to
             * wait for the poll. Not ready / no battery publishes nothing and
             * the command times out, which is the honest answer. */
            if ( power_read(me, &r, &soc) )
            {
                KLOG_N(TAG, "%u%% - %s", (unsigned)soc, chrg_name(r.chrg));
                const char *dir = "out";
                if ( 0 <= r.current_ma )
                {
                    dir = "in";
                }
                KLOG_N(TAG, "  cell %umV, %ldmA %s", (unsigned)r.vbat_mv,
                       (long)r.current_ma, dir);
                const char *usb = "absent";
                if ( r.vbus )
                {
                    usb = "present";
                }
                KLOG_N(TAG, "  sys %umV, USB %s (%umV)", (unsigned)r.vsys_mv,
                       usb, (unsigned)r.vbus_mv);
                if ( !r.gauge_ok )
                {
                    KLOG_N(TAG, "  (no fuel gauge reading; voltage is the "
                                "PMIC's, in 20mV steps)");
                }
                kdrv_bq25896_dump();
                kdrv_cw2217_dump();
            }
            else
            {
                KLOG_W(TAG, "no readings - is a battery connected?");
            }
            status = Q_HANDLED();
            break;
        }
        case POWER_PEEK_REQ_SIG: {
            reading_t r;
            uint8_t   soc = 0U;
            /* The minute's look while the board sleeps: the poll that keeps
             * the clock stops with the tick, so the keep happens here, and
             * the reading is published for the cable. Nothing is logged and
             * no chip is dumped, or every minute asleep would print a page. */
            (void)kdrv_clocks_wall_keep(false);
            (void)power_read(me, &r, &soc);
            status = Q_HANDLED();
            break;
        }
        case POWER_FIELD_REQ_SIG: {
            power_field(me, Q_EVT_CAST(PowerFieldReqEvt));
            status = Q_HANDLED();
            break;
        }
        case POWER_TEST_REQ_SIG: {
            if ( POWER_CHIP_PLUG == Q_EVT_CAST(PowerTestReqEvt)->chip )
            {
                plug_start(me);
            }
            else
            {
                power_test(me, Q_EVT_CAST(PowerTestReqEvt)->chip,
                           Q_EVT_CAST(PowerTestReqEvt)->depth);
            }
            status = Q_HANDLED();
            break;
        }
        case POWER_TEST_STEP_SIG: {
            plug_look(me);
            status = Q_HANDLED();
            break;
        }
        case POWER_CTRL_REQ_SIG: {
            status = power_ctrl(me, Q_EVT_CAST(PowerCtrlReqEvt));
            break;
        }
        case CONFIG_REQ_SIG: {
            power_config(me, Q_EVT_CAST(KodeConfigReqEvt));
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODE_LOWPOWER
        case LOWPOWER_ROUND_SIG: {
            power_round(me, Q_EVT_CAST(LowPowerRoundEvt));
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_STATE_SIG: {
            power_state(me, Q_EVT_CAST(LowPowerStateEvt));
            status = Q_HANDLED();
            break;
        }
#endif
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }
    return status;
}

/**
 * @brief Waiting for the chip to settle after a boost or reset request.
 *
 * A substate of Power_polling: the poll and everything else keep running,
 * only a second control request is turned away. The transition back is to
 * the superstate, which QP performs as an exit of this state alone, so the
 * poll timer armed on Power_polling's entry is never re-armed.
 */
static QState Power_waiting(Power * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            uint32_t ms = CTRL_RESET_WAIT_MS;
            if ( POWER_CTRL_BOOST == me->ctrl_op )
            {
                ms = CTRL_BOOST_WAIT_MS;
            }
            QTimeEvt_armX(&me->ctrl, KQP_TICKS_PER_MS(ms), 0U);
            status = Q_HANDLED();
            break;
        }
        case Q_EXIT_SIG: {
            (void)QTimeEvt_disarm(&me->ctrl);
            status = Q_HANDLED();
            break;
        }
        case POWER_CTRL_TICK_SIG: {
            bool done = true;
            if ( POWER_CTRL_BOOST == me->ctrl_op )
            {
                boost_settled(me);
            }
            else if ( !reset_settled(me) )
            {
                QTimeEvt_armX(&me->ctrl, KQP_TICKS_PER_MS(CTRL_RESET_WAIT_MS), 0U);
                done = false;
            }
            else
            {
                /* Reset settled: leaving. */
            }
            if ( done )
            {
                status = Q_TRAN(&Power_polling);
            }
            else
            {
                status = Q_HANDLED();
            }
            break;
        }
        case POWER_CTRL_REQ_SIG: {
            ctrl_answer(me, Q_EVT_CAST(PowerCtrlReqEvt)->op, KODE_NO_RESOURCES, 0);
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODE_LOWPOWER
        case LOWPOWER_ROUND_SIG: {
            /* The chip is settling after a boost or a reset. */
            lowpower_ready(me->lp_who, Q_EVT_CAST(LowPowerRoundEvt)->round,
                           true, "power control");
            status = Q_HANDLED();
            break;
        }
#endif
        default: {
            status = Q_SUPER(&Power_polling);
            break;
        }
    }
    return status;
}
