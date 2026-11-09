/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file touch_record.c
 * @brief The touch test's record, apart from AO_Touch: the console formats
 *        every step with it, and an app that never touches the glass links
 *        this file and not the owner, its stack and its driver.
 */
#include <stdio.h>

#include "kaos.h"

size_t touch_test_record(TouchTestEvt const *t, char *out, size_t size)
{
    const char *word = NULL;
    int         w    = 0;

    if ( (NULL == t) || (NULL == out) || (0U == size) )
    {
        return 0U;
    }
    if ( TP_TEST_PASS == t->phase )
    {
        word = "pass";
    }
    else if ( TP_TEST_FAIL == t->phase )
    {
        word = "fail";
    }
    else if ( TP_TEST_MISS == t->phase )
    {
        word = "miss";
    }
    else
    {
        /* No tap to say where it landed. */
    }
    if ( TP_TEST_PROMPT == t->phase )
    {
        w = snprintf(out, size, "step=%u of=%u target=%d,%d", (unsigned)t->step, (unsigned)t->total, (int)t->tx,
                     (int)t->ty);
    }
    else if ( NULL != word )
    {
        w = snprintf(out, size, "step=%u target=%d,%d got=%d,%d result=%s", (unsigned)t->step, (int)t->tx,
                     (int)t->ty, (int)t->gx, (int)t->gy, word);
    }
    else if ( TP_TEST_TIMEOUT == t->phase )
    {
        w = snprintf(out, size, "step=%u target=%d,%d result=timeout", (unsigned)t->step, (int)t->tx, (int)t->ty);
    }
    else
    {
        /* TP_TEST_DONE: the verdict follows as TEST_RESULT_SIG. */
        out[0] = '\0';
    }
    if ( 0 >= w )
    {
        return 0U;
    }
    if ( (size_t)w >= size )
    {
        return size - 1U;
    }
    return (size_t)w;
}
