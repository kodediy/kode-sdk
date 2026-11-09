/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_pin_power.c
 * @brief The standard layer's switch of J3's supply: kode_pin_power_3v3().
 */
/* Its own object: it names AO_Radio's door, and an app that never switches J3 must not link it. */
#include "kode.h"

#include <inttypes.h>

#include "kdrv_radio.h"
#include "kdrv_wifi.h"
#include "khal_once.h"
#include "khal_sync.h"
#include "kode_entry.h"
#include "kode_standard_ao.h"
#include "kode_sys.h"
#include "kode_task.h"
#include "kode_wire.h"

/** @brief The longest a sweep holds the C5: its ceiling and a step's. */
#define PIN_POWER_SWEEP_MS (KDRV_WIFI_SCAN_TIMEOUT_MS + KDRV_WIFI_SCAN_STEP_TIMEOUT_MS)

/** @brief The longest an autojoin round holds the C5 after its sweep: one join for each network the vault keeps. */
#define PIN_POWER_AUTOJOIN_MS (KODE_VAULT_NETS_MAX * KDRV_RADIO_JOIN_MS)

/**
 * @brief The longest the switch waits: one autojoin round, its sweep and its joins.
 *
 * AO_Radio pays a switch before any owed Wi-Fi request or asked scan
 * (radio_settle()), so the switch waits only for what the worker is on when
 * it is asked. The longest of those is a round, which settles only at its
 * end; a sweep alone is shorter.
 */
#define PIN_POWER_WORST_MS (PIN_POWER_SWEEP_MS + PIN_POWER_AUTOJOIN_MS)

_Static_assert(KODE_PIN_POWER_MS > PIN_POWER_WORST_MS,
               "kode_pin_power_3v3() outwaits the one round a switch can wait behind");

/** @brief How long the request waits for room in AO_Radio's queue, in ms. */
#define PIN_POWER_DOOR_MS 2000U

/** @brief The text kode_error() gives when the C5 refused the switch (KODE_IO_ERROR). */
#define PIN_POWER_REFUSED "the C5 would not switch J3's supply"

/** @brief The text kode_error() gives when the C5's link would not come up, or went with the switch (KODE_DENIED). */
#define PIN_POWER_NO_LINK "the C5's link would not come up or went down: J3's supply switches at its next link-up"

/* Not the pins lock: held across the wait, so no pin or bus call waits behind a switch. */
/** @brief The lock two kode_pin_power_3v3() calls take turns under; made on the first call. */
static khal_mutex_t s_power_lock;

/** @brief Make the lock. @return true when it exists. */
static bool power_lock_make(void)
{
    s_power_lock = khal_mutex_create();
    return NULL != s_power_lock;
}

/** @brief One request to AO_Radio and its last answer. */
typedef struct {
    bool          on;   /**< The switch.          */
    kode_status_t rc;   /**< The facade's answer. */
} power_post_t;

/** @brief One try of the request: true unless AO_Radio's queue had no room. */
static bool power_post_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the power_post_t power_switch() passed */
    power_post_t *const p = (power_post_t *)ctx;

    p->rc = kode_sys_exp_power(p->on);
    return KODE_NO_RESOURCES != p->rc;
}

/**
 * @brief Keep why the request was not posted as the task's error.
 * @param rc The facade's answer.
 */
static void power_post_fail(kode_status_t rc)
{
    switch ( rc )
    {
    case KODE_DENIED:
        kode_err_set("this firmware has no radio, so J3's supply cannot be switched");
        break;
    case KODE_NO_RESOURCES:
        kode_err_set("the radio is busy, try again");
        break;
    default:
        kode_err_set("the radio did not take the request");
        break;
    }
}

/**
 * @brief The switch itself, under the lock: arm the wait, ask, wait.
 * @param on The switch: true for on.
 * @return true when the supply is as asked; false with the error set.
 */
static bool power_switch(bool on)
{
    kode_task_t *const t  = kode_task_ensure();
    bool               ok = false;

    if ( (NULL != t) && (NULL == t->wait_sem) )
    {
        t->wait_sem = khal_sem_create();
    }
    if ( (NULL == t) || (NULL == t->wait_sem) )
    {
        kode_err_set("no memory for waiting");
    }
    else
    {
        power_post_t p = { on, KODE_ERROR };

        /* A token left by an event that arrived just as an earlier
         * kode_event_wait() on this task gave up: the semaphore is shared. */
        (void)khal_sem_wait_timeout(t->wait_sem, 0U);
        /* Armed before the ask, so an end that comes at once is not missed. */
        kode_standard_ao_power_expect(on, t->wait_sem);
        (void)kode_retry(&power_post_attempt, &p, PIN_POWER_DOOR_MS);
        if ( KODE_OK != p.rc )
        {
            (void)kode_standard_ao_power_await(0U);
            power_post_fail(p.rc);
        }
        else
        {
            switch ( kode_standard_ao_power_await(KODE_PIN_POWER_MS) )
            {
            case KODE_OK:
                ok = true;
                break;
            case KODE_IO_ERROR:
                kode_err_set(PIN_POWER_REFUSED);
                break;
            case KODE_DENIED:
                kode_err_set(PIN_POWER_NO_LINK);
                break;
            default: {
                const uint32_t seconds = KODE_PIN_POWER_MS / KODE_MS_PER_S;

                kode_err_set("J3's supply did not switch within %" PRIu32 " s", seconds);
                break;
            }
            }
        }
    }
    return ok;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_pin_power_3v3(bool on)
{
    static khal_once_t lock_once;  /* the one-shot the lock is made by */
    bool               ok = kode_enter("kode_pin_power_3v3");

    if ( ok )
    {
        ok = kode_standard_ao_ensure();   /* its Active Object hears the end */
    }
    if ( ok )
    {
        ok = khal_once_try(&lock_once, &power_lock_make);
        if ( !ok )
        {
            kode_err_set("kode_pin_power_3v3: no memory for its lock");
        }
    }
    if ( ok )
    {
        khal_mutex_lock(s_power_lock);
        ok = power_switch(on);
        khal_mutex_unlock(s_power_lock);
    }
    return ok;
}
