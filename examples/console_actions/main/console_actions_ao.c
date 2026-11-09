/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file console_actions_ao.c
 * @brief Your own console verbs: a variable that reads the AO's state, functions that change it.
 */
#include "console_actions_ao.h"

#include <stdio.h>

#define TAG "console_actions"

/** The level of the colour showing. Dim on purpose - this LED is bright. */
#define CA_LEVEL      32U

/** The counter never leaves -CA_LIMIT..CA_LIMIT, so `add` cannot overflow it
 *  and the word a getter spells stays short. */
#define CA_LIMIT      1000000

/** The bounds of `add`'s argument. */
#define CA_ADD_MAX    1000

/** @brief This AO's private signals. Posted, never published. */
enum ConsoleActionsSignals {
    CA_RESET_SIG = MAX_SIG,  /**< The console ran `reset`.               */
    CA_ADD_SIG               /**< The console ran `add`: AddEvt.         */
};

/** @brief What CA_ADD_SIG carries. */
typedef struct {
    QEvt    super;
    int32_t delta;   /**< What to add, already inside -CA_ADD_MAX..CA_ADD_MAX. */
} AddEvt;

/** The one immutable event `reset` posts. Static and never freed: QP treats an
 *  event with pool number zero as nobody's to recycle, which is what lets a
 *  callback on another task post it without touching the pools. */
static const QEvt RESET_EVT = QEVT_INITIALIZER(CA_RESET_SIG);

/** @brief The AO's own state, private to this file. */
typedef struct {
    QActive super;
    int32_t count;   /**< Written by this AO's steps alone; read by `get_count`. */
} ConsoleActions;

/* The two arrays kodeOS declares for every AO of its own: how big a burst this
 * AO survives, and what its deepest state function puts on the stack.
 * KQP_AO_QUEUE_LEN and KQP_AO_STACK_BYTES are the SDK's own defaults and a fine
 * place to start. The queue holds QEvtPtr, QP/C's own pointer wrapper, and the
 * stack is measured in StackType_t words even though KQP_AO_STACK_BYTES counts
 * bytes. */
static QEvtPtr     l_queue[KQP_AO_QUEUE_LEN];
static StackType_t l_stack[KQP_AO_STACK_BYTES / sizeof(StackType_t)];

static ConsoleActions l_ao;
QActive * const AO_ConsoleActions = &l_ao.super;

static QState ConsoleActions_initial(ConsoleActions * const me,
                                     void const * const par);
static QState ConsoleActions_running(ConsoleActions * const me,
                                     QEvt const * const e);

/* ---- The console's side ----------------------------------------------------
 * Everything from here to the tables runs on AO_Console's task.
 */

/* `run` and `get` never block. A `run` posts to the AO and never writes its
 * state: the counter and the LED belong to the AO's steps alone. */
/**
 * @brief Spell the counter for the wire: one word, the decimal number.
 *
 * `buf` has room for KODE_VALUE_STR_MAX characters plus one, so the console can
 * tell a word that fits from one that does not. A counter inside
 * -CA_LIMIT..CA_LIMIT is at most eight.
 *
 * @param buf Zeroed before the call.
 * @param cap Its size, terminator included.
 * @return KODE_OK.
 */
static kode_status_t get_count(char *buf, size_t cap)
{
    /* Read from the console's task: safe only because count is one aligned
     * 32-bit word, loaded whole. */
    (void)snprintf(buf, cap, "%ld", (long)l_ao.count);
    return KODE_OK;
}

/**
 * @brief `function name="reset"`: ask the AO to zero the counter.
 * @param arg Always NULL: the row takes no argument.
 * @return KODE_OK once the event is in the AO's queue, KODE_NO_RESOURCES when
 *         the queue had no room.
 */
static kode_status_t run_reset(const kode_value_t *arg)
{
    kode_status_t status = KODE_NO_RESOURCES;

    (void)arg;
    /* A margin of 1: a full queue refuses the post instead of asserting. */
    if ( QACTIVE_POST_X(AO_ConsoleActions, &RESET_EVT, 1U, (void *)0) )
    {
        status = KODE_OK;
    }
    return status;
}

/**
 * @brief `function name="add" arg=<n>`: ask the AO to add @p arg to the
 *        counter.
 * @param arg The number the line carried, or NULL when it carried none.
 * @return KODE_OK once the event is in the AO's queue; KODE_DENIED without an
 *         argument, which the console reports as `>err ... arg`;
 *         KODE_NO_RESOURCES when the pool or the queue had no room.
 */
static kode_status_t run_add(const kode_value_t *arg)
{
    kode_status_t status = KODE_DENIED;

    if ( NULL != arg )
    {
        AddEvt * const e = Q_NEW_X(AddEvt, 0U, CA_ADD_SIG);

        status = KODE_NO_RESOURCES;
        if ( NULL != e )
        {
            e->delta = arg->i;
            /* QActive_post_ gives the block back to the pool itself when it
             * refuses, so there is nothing to free on the failure path. */
            if ( QACTIVE_POST_X(AO_ConsoleActions, &e->super, 1U, (void *)0) )
            {
                status = KODE_OK;
            }
        }
    }
    return status;
}

/* Kept by pointer, so static const. A name is [a-z0-9_], at most
 * KODE_ROW_NAME_MAX, unique over both tables; one bad row refuses its table. */
/* Each .desc is at most KODE_ROW_DESC_MAX (48) characters. */
static const kode_variable_t VARIABLES[] = {
    { .name = "count", .get = get_count,
      .desc = "The counter: odd shows green, even blue" },
};

static const kode_function_t FUNCTIONS[] = {
    { .name = "reset", .run = run_reset,
      .desc = "Set the counter to zero" },
    { .name = "add", .arg = KODE_TYPE_INT, .min = -CA_ADD_MAX, .max = CA_ADD_MAX,
      .run = run_add, .desc = "Add arg, -1000 to 1000, to the counter" },
};

/* ---- The AO's side ---------------------------------------------------------- */

/**
 * @brief Show the counter's parity on the LED and say what it is.
 *
 * kode_sys_led_set_rgb() posts to AO_LedMgr and returns. AO_LedMgr owns the
 * chip; nothing here touches I2C, and that is the rule, not a courtesy. A
 * refusal (KODE_NO_RESOURCES: its queue is full) leaves the old colour.
 *
 * @param me  The AO.
 * @param why What moved the counter, for the log.
 */
static void show_count(ConsoleActions const * const me, const char *why)
{
    kode_status_t rc;

    if ( (me->count % 2) != 0 )
    {
        rc = kode_sys_led_set_rgb(0U, CA_LEVEL, 0U);
    }
    else
    {
        rc = kode_sys_led_set_rgb(0U, 0U, CA_LEVEL);
    }
    KLOG_I(TAG, "count=%ld, %s", (long)me->count, why);
    if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "the LED refused the colour (status %d)", (int)rc);
    }
}

/**
 * @brief Move the counter by @p delta, stopping at the limits.
 *
 * The sum is taken wide so that the clamp is what decides, not a signed
 * overflow.
 *
 * @param me    The AO.
 * @param delta What to add.
 */
static void count_add(ConsoleActions * const me, int32_t delta)
{
    int64_t const sum = (int64_t)me->count + (int64_t)delta;

    if ( (int64_t)CA_LIMIT < sum )
    {
        me->count = CA_LIMIT;
    }
    else if ( -(int64_t)CA_LIMIT > sum )
    {
        me->count = -CA_LIMIT;
    }
    else
    {
        me->count = (int32_t)sum;
    }
}

void ConsoleActions_ctor(void)
{
    ConsoleActions * const me = &l_ao;
    QActive_ctor(&me->super, Q_STATE_CAST(&ConsoleActions_initial));
    me->count = 0;
}

void ConsoleActions_start(void)
{
    /* Name the FreeRTOS task after the AO. Without this the port calls every
     * one of them "AO", which is fine until something asserts: a full queue
     * names the victim, never the culprit. */
    QActive_setAttr(AO_ConsoleActions, TASK_NAME_ATTR, "console_actions");
    QACTIVE_START(AO_ConsoleActions, KODE_PRIO_USER_LOW,
                  l_queue, Q_DIM(l_queue),
                  l_stack, sizeof(l_stack),
                  (void *)0);

    /* After the AO has a queue. A refused table is not fatal - the AO still
     * counts - but it is a mistake in the rows above, and the console has
     * already logged which one. */
    if ( KODE_VARIABLES_ADD(VARIABLES) != KODE_OK )
    {
        KLOG_E(TAG, "the variable table was refused - see the line above");
    }
    if ( KODE_FUNCTIONS_ADD(FUNCTIONS) != KODE_OK )
    {
        KLOG_E(TAG, "the function table was refused - see the line above");
    }
}

/**
 * @brief The initial transition: subscribe, then run.
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to ConsoleActions_running.
 */
static QState ConsoleActions_initial(ConsoleActions * const me,
                                     void const * const par)
{
    (void)par;

    /* One of kodeOS's published signals. AO_Buttons debounces and publishes on
     * change only, so a finger resting on the pad costs nothing here. The
     * console's two signals are ours and are posted, never subscribed to. */
    QActive_subscribe(&me->super, BUTTON_EVT_SIG);

    KLOG_I(TAG, "up; the top button adds one, `function_list` lists the rest");
    return Q_TRAN(&ConsoleActions_running);
}

/**
 * @brief The one state: every way the counter moves is an event handled here.
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() for what it handles, Q_SUPER() otherwise.
 */
static QState ConsoleActions_running(ConsoleActions * const me,
                                     QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            show_count(me, "up");
            status = Q_HANDLED();
            break;
        }
        case BUTTON_EVT_SIG: {
            ButtonEvt const * const be = (ButtonEvt const *)e;

            /* id is a button's number: KODE_SYS_BUTTON_x in kode_sys.h. */
            if ( be->pressed && (KODE_SYS_BUTTON_TOP == (uint32_t)be->id) )
            {
                count_add(me, 1);
                show_count(me, "top button");
            }
            else if ( be->pressed && (KODE_SYS_BUTTON_BOT == (uint32_t)be->id) )
            {
                /* Posts to AO_Boot, which resets the board a moment later:
                 * into kodeOS after a launcher start, into this app again
                 * after a mailbox install. */
                KLOG_I(TAG, "resetting");
                if ( KODE_OK != kode_sys_exit_to_os() )
                {
                    KLOG_W(TAG, "AO_Boot is busy; press again");
                }
            }
            else
            {
                /* A release, or another button: nothing to do. */
            }
            status = Q_HANDLED();
            break;
        }
        case CA_RESET_SIG: {
            me->count = 0;
            show_count(me, "reset from the console");
            status = Q_HANDLED();
            break;
        }
        case CA_ADD_SIG: {
            AddEvt const * const ae = (AddEvt const *)e;

            count_add(me, ae->delta);
            show_count(me, "add from the console");
            status = Q_HANDLED();
            break;
        }
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }

    return status;
}
