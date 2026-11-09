/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_battery.c
 * @brief The standard layer's battery: level, charge and voltage, USB, the
 *        charger and J3's boost, switching off, and the edge that becomes
 *        battery_low.
 */
#include "kode.h"

#include "kdrv_bq25896.h"
#include "khal_once.h"
#include "khal_sync.h"
#include "kode_battery.h"
#include "kode_entry.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"

_Static_assert(KODE_CHARGE_CURRENT_MIN_MA == KODE_SYS_CHARGE_LIMIT_MIN_MA,
               "the smallest charge limit is the system's");
_Static_assert(KODE_CHARGE_CURRENT_MAX_MA == KODE_SYS_CHARGE_LIMIT_MAX_MA,
               "the largest charge limit is the system's");

/** @brief How long kode_power_off() waits for the board to go once AO_LowPower took the request, ms:
 *         the window a refused request is begun again in, a last round and the grace before deep sleep. */
#define POWER_OFF_WAIT_MS (KODE_LOWPOWER_REQUEST_RETRY_WINDOW_MS + KODE_LOWPOWER_ROUND_MS \
                           + KODE_LOWPOWER_OFF_GRACE_MS)

/** @brief The text kode_error() gives when the build has no AO_LowPower to switch the board off. */
#define POWER_OFF_NOT_BUILT "the board cannot switch off: low power is not built (CONFIG_KODE_LOWPOWER)"

/** @brief The lock every public battery call holds; made on the first call. */
static khal_mutex_t s_battery_lock;

/** @brief Make the battery lock. @return true when it exists. */
static bool battery_lock_make(void)
{
    s_battery_lock = khal_mutex_create();
    return NULL != s_battery_lock;
}

/**
 * @brief kode_enter(), then the battery turn; battery_leave() gives it back.
 * @param name The public name of the call.
 * @return true when the call has its turn; false with the error set.
 */
static bool battery_enter(const char *name)
{
    static khal_once_t lock_once;  /* the one-shot the lock is made by */
    bool               ok = kode_enter(name);

    if ( ok )
    {
        ok = khal_once_try(&lock_once, &battery_lock_make);
        if ( ok )
        {
            khal_mutex_lock(s_battery_lock);
        }
        else
        {
            kode_err_set("%s: no memory for the battery lock", name);
        }
    }
    return ok;
}

/** @brief Give the turn back, at the end of a call battery_enter() let in. */
static void battery_leave(void)
{
    khal_mutex_unlock(s_battery_lock);
}

/** @brief One read of the battery and its answer. */
typedef struct {
    kode_sys_battery_t reading;  /**< The last reading.  */
    kode_status_t      rc;       /**< The last answer.   */
} battery_try_t;

/** @brief One try at the reading: true once the owner has one. */
static bool battery_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the battery_try_t battery_get() passed */
    battery_try_t *const t = (battery_try_t *)ctx;

    t->rc = kode_sys_battery_read(&t->reading);
    return KODE_OK == t->rc;
}

/**
 * @brief Take the owner's reading, waiting for the first one; the error is set on failure.
 * @param out Receives the reading.
 * @return true when there is one.
 */
static bool battery_get(kode_sys_battery_t *out)
{
    battery_try_t t  = { { 0U, 0U, 0U, false, false }, KODE_DENIED };
    const bool    ok = kode_retry(&battery_attempt, &t, KODE_BATTERY_WAIT_MS);

    if ( ok )
    {
        *out = t.reading;
    }
    else
    {
        kode_err_set("the battery has not been read yet");
    }
    return ok;
}

/**
 * @brief Whether a charge state is pre-charge or fast charge.
 * @param chrg A kdrv_bq25896_chrg_stat_t.
 * @return true when the cell is charging.
 */
static bool battery_charging(uint8_t chrg)
{
    return ((uint8_t)KDRV_BQ_CHRG_PRE == chrg) || ((uint8_t)KDRV_BQ_CHRG_FAST == chrg);
}

uint8_t kode_battery_level(void)
{
    uint8_t         level = 0U;
    kode_sys_battery_t r;

    if ( battery_enter("kode_battery_level") )
    {
        if ( !battery_get(&r) )
        {
            /* battery_get() said why */
        }
        else if ( !r.gauge_ok )
        {
            kode_err_set("the battery gauge does not answer");
        }
        else
        {
            level = r.soc_pct;
        }
        battery_leave();
    }
    return level;
}

bool kode_battery_is_charging(void)
{
    bool            charging = false;
    kode_sys_battery_t r;

    if ( battery_enter("kode_battery_is_charging") )
    {
        if ( battery_get(&r) )
        {
            charging = battery_charging(r.charging);
        }
        battery_leave();
    }
    return charging;
}

uint32_t kode_battery_voltage_mv(void)
{
    uint32_t        mv = 0U;
    kode_sys_battery_t r;

    if ( battery_enter("kode_battery_voltage_mv") )
    {
        if ( battery_get(&r) )
        {
            mv = r.vbat_mv;
        }
        battery_leave();
    }
    return mv;
}

/** @brief What a request asks of AO_Power, or of AO_LowPower for the switch-off. */
typedef enum {
    POWER_ASK_OFF = 0,    /**< Switch the board off.            */
    POWER_ASK_CHARGE,     /**< Allow or forbid charging.        */
    POWER_ASK_LIMIT,      /**< Set the fast-charge current.     */
    POWER_ASK_BOOST       /**< Switch J3's 5 V on or off.       */
} power_ask_t;

/** @brief The text kode_error() gives for a power_ask_t that names no request: nothing is posted. */
#define POWER_ASK_UNKNOWN "the power manager was asked for a request it does not know"

/** @brief A request to AO_Power and its last answer. */
typedef struct {
    power_ask_t   ask;      /**< What to ask.                              */
    uint16_t      arg;      /**< 1 or 0 for the switches, mA for the limit. */
    kode_status_t rc;       /**< The last answer.                          */
    bool          unknown;  /**< @c ask names no request; nothing was posted. */
} power_try_t;

/** @brief One try of the request: true unless its owner's queue had no room. */
static bool power_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the power_try_t power_ask() passed */
    power_try_t *const t = (power_try_t *)ctx;

    switch ( t->ask )
    {
    case POWER_ASK_OFF:
        t->rc = kode_sys_power_off();
        break;
    case POWER_ASK_CHARGE:
        t->rc = kode_sys_power_charge(0U != t->arg);
        break;
    case POWER_ASK_LIMIT:
        t->rc = kode_sys_power_charge_limit(t->arg);
        break;
    case POWER_ASK_BOOST:
        t->rc = kode_sys_power_boost(0U != t->arg);
        break;
    default:
        /* Never a guess: the one request that cannot be taken back is not
         * what an unknown value means. */
        t->unknown = true;
        t->rc      = KODE_DENIED;
        break;
    }
    return KODE_NO_RESOURCES != t->rc;
}

/**
 * @brief Post a request to its owner, waiting for room; the error is set when it was not taken.
 * @param ask What to ask; a value that names no request is refused and posts nothing.
 * @param arg Its argument.
 * @return true when AO_Power took it.
 */
static bool power_ask(power_ask_t ask, uint16_t arg)
{
    power_try_t t = { ask, arg, KODE_ERROR, false };

    (void)kode_retry(&power_attempt, &t, KODE_BATTERY_WAIT_MS);
    if ( t.unknown )
    {
        kode_err_set(POWER_ASK_UNKNOWN);
    }
    else if ( (POWER_ASK_OFF == t.ask) && (KODE_DENIED == t.rc) )
    {
        kode_err_set(POWER_OFF_NOT_BUILT);
    }
    else if ( KODE_OK != t.rc )
    {
        kode_err_set("the power manager is busy");
    }
    else
    {
        /* taken */
    }
    return KODE_OK == t.rc;
}

/** @brief The switch-off itself, under the battery lock; sets the error on every path. */
static void power_off_run(void)
{
    kode_sys_battery_t r;

    if ( !battery_get(&r) )
    {
        /* battery_get() said why: without a reading USB cannot be ruled out, so nothing is asked */
    }
    else if ( r.vbus )
    {
        kode_err_set("the board cannot switch off while USB is plugged in");
    }
    else if ( power_ask(POWER_ASK_OFF, 0U) )
    {
        kode_block_delay_ms(POWER_OFF_WAIT_MS);
        kode_err_set("the board did not switch off");
    }
    else
    {
        /* power_ask() said the power manager is busy */
    }
}

bool kode_power_off(void)
{
    if ( battery_enter("kode_power_off") )
    {
        power_off_run();
        battery_leave();
    }
    return false;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_power_is_usb_connected(void)
{
    bool               plugged = false;
    kode_sys_battery_t r;

    if ( battery_enter("kode_power_is_usb_connected") )
    {
        if ( battery_get(&r) )
        {
            plugged = r.vbus;
        }
        battery_leave();
    }
    return plugged;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_power_set_charging(bool on)
{
    bool taken = false;

    if ( battery_enter("kode_power_set_charging") )
    {
        uint16_t arg = 0U;

        if ( on )
        {
            arg = 1U;
        }
        taken = power_ask(POWER_ASK_CHARGE, arg);
        battery_leave();
    }
    return taken;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_power_set_charge_current(uint16_t ma)
{
    bool taken = false;

    if ( battery_enter("kode_power_set_charge_current") )
    {
        if ( (KODE_CHARGE_CURRENT_MIN_MA > ma) || (KODE_CHARGE_CURRENT_MAX_MA < ma) )
        {
            kode_err_set("charge limit out of range: use %u to %u mA",
                         (unsigned)KODE_CHARGE_CURRENT_MIN_MA, (unsigned)KODE_CHARGE_CURRENT_MAX_MA);
        }
        else
        {
            taken = power_ask(POWER_ASK_LIMIT, ma);
        }
        battery_leave();
    }
    return taken;
}

/**
 * @brief Whether the cell and the cable leave the boost room to start; the error is set when not.
 * @return true when it may be asked for.
 */
static bool boost_may_start(void)
{
    kode_sys_battery_t r;
    bool               ok = battery_get(&r);

    if ( ok && r.vbus )
    {
        kode_err_set("the boost cannot start while USB is plugged in");
        ok = false;
    }
    else if ( ok && (KODE_SYS_BOOST_MIN_MV > r.vbat_mv) )
    {
        kode_err_set("the cell is at %u mV: the boost needs %u mV",
                     (unsigned)r.vbat_mv, (unsigned)KODE_SYS_BOOST_MIN_MV);
        ok = false;
    }
    else
    {
        /* battery_get() said why, or the boost may start */
    }
    return ok;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_pin_power_5v(bool on)
{
    bool taken = false;

    if ( battery_enter("kode_pin_power_5v") )
    {
        if ( !on )
        {
            taken = power_ask(POWER_ASK_BOOST, 0U);
        }
        else if ( boost_may_start() )
        {
            taken = power_ask(POWER_ASK_BOOST, 1U);
        }
        else
        {
            /* boost_may_start() said why */
        }
        battery_leave();
    }
    return taken;
}

bool kode_battery_low_edge(bool *low, const kode_sys_battery_t *r)
{
    bool fire = false;

    if ( r->gauge_ok )
    {
        if ( battery_charging(r->charging) || ((KODE_BATTERY_LOW_PCT + KODE_BATTERY_REARM_PCT) <= r->soc_pct) )
        {
            *low = false;
        }
        else if ( (KODE_BATTERY_LOW_PCT > r->soc_pct) && !*low )
        {
            *low = true;
            fire = true;
        }
        else
        {
            /* between the mark and its re-arm, or already low */
        }
    }
    return fire;
}
