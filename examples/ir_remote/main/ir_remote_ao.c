/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file ir_remote_ao.c
 * @brief Learn one key of a TV remote and send it back out, with the infrared block from an AO.
 */
#include "ir_remote_ao.h"

#define TAG "ir_remote"

/** How long one learning window listens, in milliseconds. Long enough to find
 *  the remote and aim it, short enough that a forgotten press is not a
 *  receiver held for the rest of the boot. */
#define IRR_LEARN_MS   10000U

/** Milliseconds in a second, for the log. */
#define IRR_MS_PER_S   1000U

/** @brief The AO's own state, private to this file. */
typedef struct {
    QActive  super;
    bool     have;    /**< Whether a frame has been learned yet.             */
    uint16_t addr;    /**< The learned NEC address, as the receiver read it. */
    uint16_t cmd;     /**< The learned NEC command, as the receiver read it. */
} IrRemote;

/* The two arrays kodeOS declares for every AO of its own; buttons_led_ao.c
 * says what each number decides. The queue's depth follows kode_sys.h's one
 * queue rule ("Queue depth"): here one request out at a time (the capture,
 * then the last-frame read) and a button's press and release. */
static QEvtPtr     l_queue[KQP_AO_QUEUE_LEN];
static StackType_t l_stack[KQP_AO_STACK_BYTES / sizeof(StackType_t)];

/* Where AO_Ir writes a captured frame's symbols. Written by another task,
 * never read here, and static because it has to outlive the request. One
 * kilobyte, the longest capture the receiver keeps. */
static kode_sys_ir_symbol_t l_lent[KODE_SYS_IR_RX_MAX_SYMBOLS];

static IrRemote l_ao;
QActive * const AO_IrRemote = &l_ao.super;

/* No step here waits: kode.h's kode_ir_receive() and kode_ir_send() do, so
 * they belong to a task. */
static QState IrRemote_initial(IrRemote * const me, void const * const par);
static QState IrRemote_ready(IrRemote * const me, QEvt const * const e);
static QState IrRemote_listening(IrRemote * const me, QEvt const * const e);
static QState IrRemote_decoding(IrRemote * const me, QEvt const * const e);

/**
 * @brief Whether @p e is the press of @p which.
 * @param e     A BUTTON_EVT_SIG event.
 * @param which A KODE_SYS_BUTTON_x.
 * @return true for the press, false for a release or another button.
 */
static bool pressed(QEvt const * const e, uint32_t const which)
{
    ButtonEvt const * const be = (ButtonEvt const *)e;

    return be->pressed && (which == (uint32_t)be->id);
}

/**
 * @brief Send the learned frame back out, or say why there is none.
 *
 * kode_sys_ir_send_nec() returns as soon as the frame is queued: AO_Ir hands
 * it to the C5 and never waits for the 52 to 86 ms it takes on the wire,
 * which is what makes it safe from here. It cannot say whether the frame went
 * out, so neither can this - the log line below is a request, not a
 * confirmation.
 *
 * @param me The AO.
 */
static void send_learned(IrRemote const * const me)
{
    if ( me->have )
    {
        kode_status_t const rc = kode_sys_ir_send_nec(me->addr, me->cmd);

        if ( KODE_OK == rc )
        {
            KLOG_I(TAG, "sending NEC addr=0x%04X cmd=0x%04X",
                   (unsigned)me->addr, (unsigned)me->cmd);
        }
        else
        {
            KLOG_W(TAG, "send not taken (status %d)", (int)rc);
        }
    }
    else
    {
        KLOG_W(TAG, "nothing learned yet; press the top button and point a "
                    "remote at the Dot");
    }
}

void IrRemote_ctor(void)
{
    IrRemote * const me = &l_ao;
    QActive_ctor(&me->super, Q_STATE_CAST(&IrRemote_initial));
    me->have = false;
    me->addr = 0U;
    me->cmd  = 0U;
}

void IrRemote_start(void)
{
    /* Name the FreeRTOS task after the AO, so a full queue names its owner. */
    QActive_setAttr(AO_IrRemote, TASK_NAME_ATTR, "ir_remote");
    QACTIVE_START(AO_IrRemote, KODE_PRIO_USER_LOW,
                  l_queue, Q_DIM(l_queue),
                  l_stack, sizeof(l_stack),
                  (void *)0);
}

/**
 * @brief The initial transition: subscribe to the buttons, then wait.
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to IrRemote_ready.
 */
static QState IrRemote_initial(IrRemote * const me, void const * const par)
{
    (void)par;

    QActive_subscribe(&me->super, BUTTON_EVT_SIG);

    KLOG_I(TAG, "up; the top button learns a remote key, the bottom one "
                "sends the last one learned");
    return Q_TRAN(&IrRemote_ready);
}

/**
 * @brief Nothing out: the top button learns, the bottom one sends.
 *
 * A capture refused at once posted nothing, so there is no answer to wait
 * for and the state stays.
 *
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() for what it handles, Q_SUPER() otherwise.
 */
static QState IrRemote_ready(IrRemote * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case BUTTON_EVT_SIG: {
            status = Q_HANDLED();
            if ( pressed(e, KODE_SYS_BUTTON_TOP) )
            {
                /* LENT: AO_Ir fills l_lent just before IR_CAPTURE_RSP_SIG. */
                kode_status_t const rc =
                    kode_sys_ir_capture(&me->super, l_lent, Q_DIM(l_lent),
                                        IRR_LEARN_MS);

                if ( KODE_OK == rc )
                {
                    status = Q_TRAN(&IrRemote_listening);
                }
                else
                {
                    KLOG_W(TAG, "capture not taken (status %d)", (int)rc);
                }
            }
            else if ( pressed(e, KODE_SYS_BUTTON_BOT) )
            {
                send_learned(me);
            }
            else
            {
                /* A release: nothing to do. */
            }
            break;
        }
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }

    return status;
}

/**
 * @brief Listening for one key: the capture's answer ends the window.
 *
 * The capture carries the window's length, so AO_Ir closes it and answers
 * KODE_TIMEOUT when nothing came: this AO needs no clock of its own. The
 * buttons are ignored meanwhile.
 *
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() for what it handles, Q_SUPER() otherwise.
 */
static QState IrRemote_listening(IrRemote * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            KLOG_I(TAG, "listening for %u s: point a remote at the Dot and "
                        "press a key", (unsigned)(IRR_LEARN_MS / IRR_MS_PER_S));
            status = Q_HANDLED();
            break;
        }
        case IR_CAPTURE_RSP_SIG: {
            kode_sys_ir_capture_answer_t const * const a =
                Q_EVT_CAST(kode_sys_ir_capture_answer_t);
            kode_status_t rc = (kode_status_t)a->rc;

            status = Q_TRAN(&IrRemote_ready);
            if ( KODE_OK == rc )
            {
                /* A frame came. What it decoded to is the next request. */
                rc = kode_sys_ir_last(&me->super);
                if ( KODE_OK == rc )
                {
                    status = Q_TRAN(&IrRemote_decoding);
                }
                else
                {
                    KLOG_W(TAG, "heard %u symbols; the read of them was not "
                                "taken (status %d)", (unsigned)a->n, (int)rc);
                }
            }
            else if ( KODE_TIMEOUT == rc )
            {
                KLOG_W(TAG, "nothing heard in %u s; is the C5 running a kode_c5 "
                            "image with the infrared service?",
                       (unsigned)(IRR_LEARN_MS / IRR_MS_PER_S));
            }
            else if ( KODE_IO_ERROR == rc )
            {
                KLOG_W(TAG, "this board has no infrared block");
            }
            else
            {
                /* KODE_DENIED: AO_Ir was busy, a later capture displaced this
                 * one, or the C5 link will not come. */
                KLOG_W(TAG, "capture refused (status %d); press the top "
                            "button to try again", (int)rc);
            }
            break;
        }
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }

    return status;
}

/**
 * @brief A frame arrived: waiting for what it decoded to.
 *
 * @param me The AO.
 * @param e  The event.
 * @return Q_TRAN() back to ready once answered, Q_SUPER() otherwise.
 */
static QState IrRemote_decoding(IrRemote * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case IR_LAST_RSP_SIG: {
            kode_sys_ir_last_answer_t const * const a =
                Q_EVT_CAST(kode_sys_ir_last_answer_t);

            if ( ((uint8_t)KODE_OK == a->rc) && a->have && a->frame.nec )
            {
                /* The two 16-bit words exactly as they came off the wire, the
                 * inverted copies a classic remote sends included: a remote
                 * whose address is 0x04 arrives as 0xFB04. They go back out
                 * unchanged, so what was learned is what is sent. */
                me->have = true;
                me->addr = a->frame.addr;
                me->cmd  = a->frame.cmd;
                KLOG_I(TAG, "learned NEC addr=0x%04X cmd=0x%04X; the bottom "
                            "button sends it", (unsigned)me->addr,
                       (unsigned)me->cmd);
            }
            else
            {
                /* A held key's repeat frame and other protocols land here. */
                KLOG_I(TAG, "heard %u symbols that are not NEC; press the top "
                            "button and the key again",
                       (unsigned)a->frame.symbols);
            }
            status = Q_TRAN(&IrRemote_ready);
            break;
        }
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }

    return status;
}
