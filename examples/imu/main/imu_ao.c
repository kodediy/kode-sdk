/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file imu_ao.c
 * @brief Reading the motion sensors from an Active Object, without waiting.
 */
#include "imu_ao.h"

#include <math.h>

#define TAG "imu"

/** How often a reading is asked for: fast enough to follow a hand turning the
 *  board, slow enough to read the log. */
#define IS_PERIOD_MS   200U

/** Degrees in one radian. */
#define IS_DEG_PER_RAD 57.29578f

/** Degrees in a full turn. */
#define IS_DEG_TURN    360

/** @brief This AO's private signals. Posted to itself, never published. */
enum ImuDemoSignals {
    IS_TICK_SIG = MAX_SIG   /**< The stream's periodic QTimeEvt. */
};

/** @brief One sensor's requests: whether one is out, and whether to ask. */
typedef struct {
    bool busy;    /**< A request was taken and its answer has not arrived. */
    bool absent;  /**< The part is not fitted: there is no use asking.    */
} Sensor;

/** @brief The AO's own state, private to this file. */
typedef struct {
    QActive  super;
    QTimeEvt tick;
    Sensor   imu;   /**< The IMU's request.          */
    Sensor   mag;   /**< The magnetometer's request. */
} ImuDemo;

/* The queue and the stack every AO of ours carries. The SDK's defaults are a
 * fine place to start: the queue holds a burst of button edges, a tick and
 * the two answers, and the stack is what the deepest state function needs,
 * this one's float maths and the formatter included. */
static QEvtPtr     l_queue[KQP_AO_QUEUE_LEN];
static StackType_t l_stack[KQP_AO_STACK_BYTES / sizeof(StackType_t)];

static ImuDemo l_ao;
QActive * const AO_ImuDemo = &l_ao.super;

static QState ImuDemo_initial(ImuDemo * const me, void const * const par);
static QState ImuDemo_running(ImuDemo * const me, QEvt const * const e);
static QState ImuDemo_streaming(ImuDemo * const me, QEvt const * const e);
static QState ImuDemo_paused(ImuDemo * const me, QEvt const * const e);

/**
 * @brief Whether an event is the press of one button.
 *
 * ButtonEvt.id is a button's number, KODE_SYS_BUTTON_x, and a release is an
 * event of its own, so both have to be looked at.
 *
 * @param e     A BUTTON_EVT_SIG event.
 * @param which The button, a KODE_SYS_BUTTON_x.
 * @return true when @p which went down.
 */
static bool button_pressed(QEvt const * const e, uint32_t const which)
{
    ButtonEvt const * const be = (ButtonEvt const *)e;

    return be->pressed && (which == (uint32_t)be->id);
}

/**
 * @brief Pitch of the chip, in whole degrees, from the direction of gravity.
 *
 * At rest the accelerometer reads the reaction to gravity, so the axis that
 * points up reads +1 g. Pitch is how far the X axis has tipped away from
 * level.
 *
 * @param ax_mg Acceleration on X, mg.
 * @param ay_mg Acceleration on Y, mg.
 * @param az_mg Acceleration on Z, mg.
 * @return Degrees, -90 to +90.
 */
static int32_t pitch_deg(int32_t const ax_mg, int32_t const ay_mg,
                         int32_t const az_mg)
{
    float const ay   = (float)ay_mg;
    float const az   = (float)az_mg;
    float const flat = sqrtf((ay * ay) + (az * az));

    return (int32_t)lroundf(atan2f((float)(-ax_mg), flat) * IS_DEG_PER_RAD);
}

/**
 * @brief Roll of the chip, in whole degrees: how far Y has tipped from level.
 * @param ay_mg Acceleration on Y, mg.
 * @param az_mg Acceleration on Z, mg.
 * @return Degrees, -180 to +180.
 */
static int32_t roll_deg(int32_t const ay_mg, int32_t const az_mg)
{
    return (int32_t)lroundf(atan2f((float)ay_mg, (float)az_mg) * IS_DEG_PER_RAD);
}

/**
 * @brief The angle of the field in the chip's X-Y plane, from +X toward +Y.
 * @param mx_mg Field on X, milligauss.
 * @param my_mg Field on Y, milligauss.
 * @return Degrees, 0 to 359.
 */
static int32_t heading_deg(int32_t const mx_mg, int32_t const my_mg)
{
    int32_t deg = (int32_t)lroundf(atan2f((float)my_mg, (float)mx_mg)
                                   * IS_DEG_PER_RAD);

    if ( 0 > deg )
    {
        deg += IS_DEG_TURN;
    }
    if ( IS_DEG_TURN <= deg )
    {
        deg -= IS_DEG_TURN;
    }
    return deg;
}

/**
 * @brief Log the IMU's answer, whatever it was.
 *
 * The sample is only there on KODE_OK: on every other answer it is zero,
 * which is the absence of a reading, not a reading of zero.
 *
 * @param me The AO.
 * @param a  The answer.
 */
static void report_imu(ImuDemo * const me, kode_sys_imu_answer_t const * const a)
{
    kode_status_t const rc = (kode_status_t)a->rc;

    if ( KODE_OK == rc )
    {
        kode_sys_imu_sample_t const * const s = &a->sample;

        KLOG_I(TAG, "imu  a=(%d, %d, %d) mg  w=(%d, %d, %d) mdps  "
                    "pitch=%d roll=%d deg",
               (int)s->a_mg[0], (int)s->a_mg[1], (int)s->a_mg[2],
               (int)s->w_mdps[0], (int)s->w_mdps[1], (int)s->w_mdps[2],
               (int)pitch_deg(s->a_mg[0], s->a_mg[1], s->a_mg[2]),
               (int)roll_deg(s->a_mg[1], s->a_mg[2]));
    }
    else if ( KODE_IO_ERROR == rc )
    {
        me->imu.absent = true;
        KLOG_W(TAG, "no IMU on this board; not asking for it again");
    }
    else
    {
        /* KODE_DENIED: a self-test holds the part, and a sample now would be
         * at a scale nobody here knows. */
        KLOG_W(TAG, "imu read refused (status %u); asking again next tick",
               (unsigned)rc);
    }
}

/**
 * @brief Log the magnetometer's answer, whatever it was.
 * @param me The AO.
 * @param a  The answer.
 */
static void report_mag(ImuDemo * const me, kode_sys_mag_answer_t const * const a)
{
    kode_status_t const rc = (kode_status_t)a->rc;

    if ( KODE_OK == rc )
    {
        KLOG_I(TAG, "mag  m=(%d, %d, %d) mG  heading=%d deg",
               (int)a->mg[0], (int)a->mg[1], (int)a->mg[2],
               (int)heading_deg(a->mg[0], a->mg[1]));
    }
    else if ( KODE_IO_ERROR == rc )
    {
        me->mag.absent = true;
        KLOG_W(TAG, "no magnetometer on this board; not asking for it again");
    }
    else
    {
        /* KODE_ERROR is a conversion that was not ready, or overflowed: the
         * part converts at 50 Hz and this asks at 5, so it is rare and the
         * next ask is the whole remedy. */
        KLOG_W(TAG, "mag read failed (status %u); asking again next tick",
               (unsigned)rc);
    }
}

/**
 * @brief One tick: ask each sensor for a reading, unless one is out or the
 *        part is absent.
 *
 * A flag is set only when its request was taken: a refusal posted nothing, so
 * there is no answer to wait for, and the next tick asks again.
 *
 * @param me The AO.
 */
static void stream_step(ImuDemo * const me)
{
    /* One request per sensor at a time: a second before the answer is refused
     * with KODE_DENIED. */
    if ( !me->imu.busy && !me->imu.absent )
    {
        if ( KODE_OK == kode_sys_imu_read(&me->super) )
        {
            me->imu.busy = true;
        }
    }
    if ( !me->mag.busy && !me->mag.absent )
    {
        if ( KODE_OK == kode_sys_mag_read(&me->super) )
        {
            me->mag.busy = true;
        }
    }
}

void ImuDemo_ctor(void)
{
    ImuDemo * const me = &l_ao;

    QActive_ctor(&me->super, Q_STATE_CAST(&ImuDemo_initial));
    QTimeEvt_ctorX(&me->tick, &me->super, (enum_t)IS_TICK_SIG, 0U);
    me->imu.busy   = false;
    me->imu.absent = false;
    me->mag.busy   = false;
    me->mag.absent = false;
}

void ImuDemo_start(void)
{
    /* Name the FreeRTOS task after the AO, so a QP assert names the culprit
     * and not just "AO". */
    QActive_setAttr(AO_ImuDemo, TASK_NAME_ATTR, "imu");
    QACTIVE_START(AO_ImuDemo, KODE_PRIO_USER_LOW,
                  l_queue, Q_DIM(l_queue),
                  l_stack, sizeof(l_stack),
                  (void *)0);
}

/**
 * @brief The initial transition: subscribe, then run.
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to ImuDemo_running.
 */
static QState ImuDemo_initial(ImuDemo * const me, void const * const par)
{
    (void)par;

    /* The buttons are the only published signal this AO listens to. The
     * sensors' answers are posted to it, never published, so there is
     * nothing to subscribe to for them. */
    QActive_subscribe(&me->super, BUTTON_EVT_SIG);

    KLOG_I(TAG, "up; the top button pauses and resumes the stream, "
                "the bottom one resets");
    return Q_TRAN(&ImuDemo_running);
}

/**
 * @brief The superstate of both modes: what holds whether or not it streams.
 *
 * The reset lives here, and so do the answers: one can arrive after a
 * pause, and it still frees its sensor's door. Streaming logs them first. A
 * press of the top button is the modes' own business, and they pass
 * everything else up to this state.
 *
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() for what it handles, Q_SUPER() otherwise.
 */
static QState ImuDemo_running(ImuDemo * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case Q_INIT_SIG: {
            status = Q_TRAN(&ImuDemo_streaming);
            break;
        }
        case IMU_SAMPLE_RSP_SIG: {
            me->imu.busy = false;
            status = Q_HANDLED();
            break;
        }
        case MAG_SAMPLE_RSP_SIG: {
            me->mag.busy = false;
            status = Q_HANDLED();
            break;
        }
        case BUTTON_EVT_SIG: {
            if ( button_pressed(e, KODE_SYS_BUTTON_BOT) )
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

/**
 * @brief Streaming: a tick asks for a reading and reports the last one.
 *
 * The tick is armed only while streaming. A paused AO has no timer, and so
 * nothing to wake it: it costs the system nothing until a button does.
 *
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() for what it handles, Q_SUPER() otherwise.
 */
static QState ImuDemo_streaming(ImuDemo * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            QTimeEvt_armX(&me->tick,
                          KQP_TICKS_PER_MS(IS_PERIOD_MS),
                          KQP_TICKS_PER_MS(IS_PERIOD_MS));
            KLOG_I(TAG, "streaming, every %ums", (unsigned)IS_PERIOD_MS);
            status = Q_HANDLED();
            break;
        }
        case Q_EXIT_SIG: {
            (void)QTimeEvt_disarm(&me->tick);
            status = Q_HANDLED();
            break;
        }
        case IS_TICK_SIG: {
            stream_step(me);
            status = Q_HANDLED();
            break;
        }
        case IMU_SAMPLE_RSP_SIG: {
            report_imu(me, (kode_sys_imu_answer_t const *)e);
            /* The superstate frees the door. */
            status = Q_SUPER(&ImuDemo_running);
            break;
        }
        case MAG_SAMPLE_RSP_SIG: {
            report_mag(me, (kode_sys_mag_answer_t const *)e);
            status = Q_SUPER(&ImuDemo_running);
            break;
        }
        case BUTTON_EVT_SIG: {
            if ( button_pressed(e, KODE_SYS_BUTTON_TOP) )
            {
                status = Q_TRAN(&ImuDemo_paused);
            }
            else
            {
                /* The bottom button is the superstate's to answer. */
                status = Q_SUPER(&ImuDemo_running);
            }
            break;
        }
        default: {
            status = Q_SUPER(&ImuDemo_running);
            break;
        }
    }

    return status;
}

/**
 * @brief Paused: no timer, no requests, and the top button resumes.
 *
 * An answer to a request made before the pause still arrives, and the
 * superstate takes it without a log line.
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() for what it handles, Q_SUPER() otherwise.
 */
static QState ImuDemo_paused(ImuDemo * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            KLOG_I(TAG, "paused");
            status = Q_HANDLED();
            break;
        }
        case BUTTON_EVT_SIG: {
            if ( button_pressed(e, KODE_SYS_BUTTON_TOP) )
            {
                status = Q_TRAN(&ImuDemo_streaming);
            }
            else
            {
                status = Q_SUPER(&ImuDemo_running);
            }
            break;
        }
        default: {
            status = Q_SUPER(&ImuDemo_running);
            break;
        }
    }

    return status;
}
