/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_screen_sleep.c
 * @brief The standard layer's display sleep: kode_display_sleep().
 */
#include "kode.h"

#include "kode_entry.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"

_Static_assert(KODE_DISPLAY_SLEEP_BOUND_MS == KODE_DISPLAY_SLEEP_WAIT_MS,
               "the wait kode.h promises is the system's");

/** @brief Answers since a request that, with the glass not as it asked, mean
 *         its own came and a later one undid it. */
#define SLEEP_UNDONE_ANSWERS  2U

/** @brief What one kode_display_sleep() is waiting for. */
typedef struct {
    bool          sleep;     /**< What was asked: true to sleep, false to wake.   */
    bool          posted;    /**< AO_Display has the request.                     */
    bool          refused;   /**< AO_Display answered that it will not.           */
    uint16_t      before;    /**< AO_Display's answers before this one's.         */
    const char   *why;       /**< Why it will not, in words.                      */
} sleep_wait_t;

/**
 * @brief One try: post the request if it is not posted, then look for its answer.
 *
 * A task hears no DISPLAY_SLEEP_SIG, so it watches AO_Display's count of
 * answers until one more is there.
 * @param ctx The sleep_wait_t of the call.
 * @return true when AO_Display has answered: done, or refused.
 */
static bool sleep_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the sleep_wait_t kode_display_sleep() passed */
    sleep_wait_t *const w    = (sleep_wait_t *)ctx;
    bool                done = false;

    if ( !w->posted )
    {
        w->before = kode_block_screen_sleep_answers(NULL, NULL);
        w->posted = (kode_sys_screen_sleep(w->sleep) == KODE_OK);
    }
    if ( w->posted )
    {
        kode_status_t  rc  = KODE_OK;
        const uint16_t now = kode_block_screen_sleep_answers(&rc, &w->why);

        if ( now == w->before )
        {
            /* Not answered yet. */
        }
        else if ( KODE_OK != rc )
        {
            w->refused = true;
            done       = true;
        }
        else if ( kode_sys_screen_is_asleep() == w->sleep )
        {
            done = true;
        }
        else if ( SLEEP_UNDONE_ANSWERS <= (uint16_t)(now - w->before) )
        {
            /* AO_Display answers in order and the word keeps the last: a wake
             * while the renderer parks answers this sleep no, then itself yes. */
            w->refused = true;
            w->why     = "a later request undid it";
            done       = true;
        }
        else
        {
            /* The answer of an earlier request: this one's is still to come. */
            w->before = now;
        }
    }
    return done;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_display_sleep(bool sleep)
{
    bool done = false;

    if ( kode_enter("kode_display_sleep") )
    {
        sleep_wait_t w    = { .sleep = sleep, .posted = false, .refused = false, .before = 0U, .why = "" };
        const char  *what = "wake";

        if ( sleep )
        {
            what = "go to sleep";
        }
        done = kode_retry(&sleep_attempt, &w, KODE_DISPLAY_SLEEP_BOUND_MS) && !w.refused;
        if ( done )
        {
            /* The display is as asked. */
        }
        else if ( w.refused )
        {
            kode_err_set("the display did not %s: %s", what, w.why);
        }
        else if ( !w.posted )
        {
            kode_err_set("the display did not %s: the system is busy", what);
        }
        else
        {
            kode_err_set("the display did not %s in %u ms", what, (unsigned)KODE_DISPLAY_SLEEP_BOUND_MS);
        }
    }
    return done;
}
