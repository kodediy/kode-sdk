/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_rest.c
 * @brief The standard layer's rest: kode_rest() and kode_rest_end().
 *
 * A task hears no LOWPOWER_PART_SIG, so each call posts its request and
 * watches AO_LowPower's count of answers until one more is there.
 */
#include "kode.h"

#include "kode_entry.h"
#include "kode_pin_lock.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"

_Static_assert((uint32_t)KODE_REST_SCREEN == KODE_SYS_REST_SCREEN, "the parts are the system's");
_Static_assert((uint32_t)KODE_REST_PANEL == KODE_SYS_REST_PANEL, "the parts are the system's");
_Static_assert((uint32_t)KODE_REST_TOUCH == KODE_SYS_REST_TOUCH, "the parts are the system's");
_Static_assert((uint32_t)KODE_REST_SOUND == KODE_SYS_REST_SOUND, "the parts are the system's");
_Static_assert((uint32_t)KODE_REST_CARD == KODE_SYS_REST_CARD, "the parts are the system's");
_Static_assert((uint32_t)KODE_REST_RADIO == KODE_SYS_REST_RADIO, "the parts are the system's");
_Static_assert((uint32_t)KODE_REST_CPU == KODE_SYS_REST_CPU, "the parts are the system's");
_Static_assert(KODE_REST_BOUND_MS == KODE_LOWPOWER_PART_BOUND_MS, "the wait kode.h promises is the system's");

/** @brief What one kode_rest() or kode_rest_end() is waiting for. */
typedef struct {
    bool                        end;      /**< What was asked: true to end the rest.       */
    uint32_t                    parts;    /**< The parts to put away.                      */
    const kode_sys_rest_wake_t *wake;     /**< The wake sources; NULL for none.            */
    bool                        posted;   /**< AO_LowPower has the request.                */
    bool                        denied;   /**< The system refused to take the request.     */
    bool                        taken;    /**< Another request was being arranged.         */
    bool                        listened; /**< The wake pin has a listener of the app's.   */
    bool                        refused;  /**< AO_LowPower answered that it will not.      */
    uint16_t                    before;   /**< AO_LowPower's answers before this one's.    */
    const char                 *why;      /**< Why it will not, in words.                  */
} rest_wait_t;

/**
 * @brief One try: post the request if it is not posted, then look for its answer.
 * @param ctx The rest_wait_t of the call.
 * @return true when the call is settled: answered, or refused at the door.
 */
static bool rest_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the rest_wait_t the call passed */
    rest_wait_t *const w    = (rest_wait_t *)ctx;
    bool               done = false;

    if ( !w->posted )
    {
        kode_status_t rc   = KODE_DENIED;
        bool          open = false;

        w->before = kode_block_rest_answers(NULL, NULL, &open, NULL);
        if ( open )
        {
            /* AO_LowPower would refuse it on the bus, where a task hears
             * nothing: said here, and at once. */
            w->taken = true;
            done     = true;
        }
        else if ( w->end )
        {
            rc = kode_sys_rest_end();
        }
        else
        {
            rc = kode_sys_rest(w->parts, w->wake);
        }
        w->posted = (!open) && (KODE_OK == rc);
        if ( (!open) && (KODE_DENIED == rc) )
        {
            /* Not a full queue: asking again changes nothing. */
            w->denied = true;
            done      = true;
        }
    }
    if ( w->posted )
    {
        kode_status_t rc = KODE_OK;

        if ( kode_block_rest_answers(&rc, NULL, NULL, &w->why) != w->before )
        {
            w->refused = (KODE_OK != rc);
            done       = true;
        }
    }
    return done;
}

/**
 * @brief Make one request and wait for its answer.
 * @param w    The request, its outcome fields cleared.
 * @param what What the call does, for the error text.
 * @return true when AO_LowPower answered that it is done.
 */
static bool rest_run(rest_wait_t *w, const char *what)
{
    bool settled = false;
    bool done    = false;

    if ( !w->listened )
    {
        settled = kode_retry(&rest_attempt, w, KODE_REST_BOUND_MS);
    }

    if ( w->listened )
    {
        kode_err_set("the rest did not %s: pin: listened to", what);
    }
    else if ( w->taken )
    {
        kode_err_set("the rest did not %s: another request is being arranged", what);
    }
    else if ( w->denied )
    {
        kode_err_set("the rest did not %s: low power is not built (CONFIG_KODE_LOWPOWER)", what);
    }
    else if ( settled && w->refused )
    {
        kode_err_set("the rest did not %s: %s", what, w->why);
    }
    else if ( settled )
    {
        done = true;
    }
    else if ( !w->posted )
    {
        kode_err_set("the rest did not %s: the system is busy", what);
    }
    else
    {
        kode_err_set("the rest did not %s in %u ms", what, (unsigned)KODE_REST_BOUND_MS);
    }
    return done;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_rest(uint32_t parts, const kode_rest_wake_t *wake)
{
    bool done = false;

    if ( kode_enter("kode_rest") )
    {
        kode_sys_rest_wake_t sys  = { 0 };
        rest_wait_t          w    = { 0 };

        w.parts = parts;
        w.why   = "";
        if ( NULL != wake )
        {
            sys.touch     = wake->touch;
            sys.pin       = wake->pin;
            sys.pin_level = wake->pin_level;
            sys.after_ms  = wake->after_ms;
            w.wake        = &sys;
            /* A wake line takes the pin's interrupt and its pull: the listener
             * would hear nothing more, and nothing could put it back. */
            w.listened    = kode_pin_is_listened(wake->pin);
        }
        done = rest_run(&w, "begin");
    }
    return done;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_rest_end(void)
{
    bool done = false;

    if ( kode_enter("kode_rest_end") )
    {
        rest_wait_t w = { 0 };

        w.end = true;
        w.why = "";
        done  = rest_run(&w, "end");
    }
    return done;
}
