/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file imu_ao.c
 * @brief AO_Imu: the owner of the QMI8658A and the QMC5883P.
 */
#include "sdkconfig.h"
#include "kaos.h"
#include "kaos_ask.h"
#include "kaos_start.h"
#include "kaos_claim.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kbsp.h"
#include "kbsp_owners.h"
#include "kdrv_qmc5883p.h"
#include "kdrv_qmi8658.h"
#include "kimu_energy.h"
#include "khal_nvs.h"
#include "khal_sync.h"
#include "khal_time.h"
#include "kode_log.h"
#include "kqp.h"
#include "kset.h"
#include "krows.h"

#define TAG "kaos.imu"

/** @brief One g, in milli-g: what the board reads at rest, any way up. */
#define IMU_ONE_G_MG   1000

/* Where the IMU is. Each self-test is the part working on its own while this
 * AO polls it, so the row is a handful of short steps on the wait one-shot
 * rather than one long one. */
#define PHASE_NONE      0U  /**< Nothing holds the IMU.                      */
#define PHASE_ST_XL     1U  /**< The accelerometer's self-test is running.    */
#define PHASE_ST_GY     2U  /**< The gyroscope's.                             */
#define PHASE_SETTLE    3U  /**< Sensors back on; the gyroscope is turning on. */
#define PHASE_CAP_ARMED 4U  /**< A witness window is filling; its holder has
                             *   KODE_IMU_CAPTURE_MAX_MS to drain it.         */
#define PHASE_CAP_DRAIN 5U  /**< The window is coming out, a chunk a tick.    */
#define PHASE_SETTLING  6U  /**< The gyroscope is turning back on after the
                             *   last window or row, and a request that
                             *   arrived meanwhile waits for it (PENDING_*). */

/* What waits out a settle. While the gyroscope is turning on, the part does
 * not act on a FIFO reset or a self-test trigger. */
#define PENDING_NONE    0U
#define PENDING_ROW     1U  /**< `test component=imu` is waiting.            */
#define PENDING_CAP     2U  /**< A witness capture is waiting; @c who holds it. */
#define PENDING_READ    3U  /**< A read that woke the part is waiting; @c held
                             *   is its copy.                                */

/** @brief Witness samples read per step: 96 bytes, about 2.5 ms of bus at
 *         400 kHz - under one I2S transfer, so a chunk cannot bury the
 *         codec's refill. */
#define CAP_CHUNK       16U
/** @brief One chunk every 10 ms. */
#define CAP_CHUNK_MS    10U

/** @brief How often a running self-test is asked whether it has finished. */
#define ST_POLL_MS      20U
/** @brief Polls before a self-test is a timeout: the datasheet's typical
 *         25 ms and 400 ms, with room - 300 ms and 1 s. */
#define ST_XL_POLLS     15U
#define ST_GY_POLLS     50U
/** @brief From sensors-on to a gyroscope that is fully running: the
 *         datasheet's 150 ms + 3/ODR, with margin. The row's step 3 waits
 *         this out before it reads, and a FIFO reset or a self-test trigger
 *         asked for inside it is held until it is over - at 200 ms, one
 *         self-test in three asked right behind a window never finished;
 *         at 300 ms none did. */
#define SETTLE_MS       300U
/** @brief What the row gives a part it has just put to rest before it
 *         triggers a self-test on it. */
#define ROW_REST_MS     150U
/** @brief From sensors-on to a gyroscope sample that means something: the
 *         datasheet's 150 ms + 3/ODR, with margin. What a read that woke
 *         the part waits; shorter than SETTLE_MS because a read asks the
 *         part for nothing but its output registers. */
#define GYRO_READY_MS   200U

/**
 * @brief One part's rest: whether it converts, and the one-shot that ends it.
 *
 * The one-shot is armed again at every use, so its expiry means a whole
 * KODE_SYS_IMU_REST_AFTER_MS with nobody asking. @c stale and @c live are
 * wait_stop()'s bookkeeping, for the same reason: an expiry already queued
 * when the part is used again belongs to the silence before that use.
 */
typedef struct {
    QTimeEvt te;     /**< IMU_REST_SIG or MAG_REST_SIG.                     */
    uint32_t stale;  /**< Expired events still queued.                      */
    bool     live;   /**< The one-shot is armed and unexpired.              */
    bool     awake;  /**< The part is converting.                           */
} ImuPart;

typedef struct {
    QActive  super;
    QTimeEvt wait;         /**< IMU_WAIT_SIG: a self-test's poll, a window's
                            *   chunk, or a window nobody came back for.    */
    uint32_t wait_stale;   /**< Expired IMU_WAIT_SIG events still queued.   */
    bool     wait_live;    /**< The one-shot is armed and unexpired.        */
    bool     imu_up;       /**< The QMI8658A answered and is configured.    */
    bool     mag_up;       /**< The QMC5883P answered and is configured.    */
    ImuPart  imu;          /**< The QMI8658A's rest.                        */
    ImuPart  mag;          /**< The QMC5883P's.                             */
    ImuReadEvt held;       /**< The read PENDING_READ is holding, copied:
                            *   the request event is gone by the time the
                            *   gyroscope is ready.                         */
    uint8_t  phase;        /**< PHASE_*: what holds the IMU.                */
    uint8_t  pending;      /**< PENDING_*: what a settle is holding up.     */
    uint32_t gyro_on_ms;   /**< khal_time_ms() when the gyroscope was last
                            *   switched on: by a wake, after a window,
                            *   after a row.                                */
    uint8_t  polls;        /**< Polls left before the self-test in flight is
                            *   a timeout.                                  */
    uint16_t left;         /**< Witness samples still in the part's FIFO.   */
    uint16_t stored;       /**< Witness samples already in s_xyz.           */
    QActive *who;          /**< Who holds the capture.                      */
    uint16_t hz;           /**< The bin the drain was asked about.          */
    kdrv_qmc5883p_cal_t corr;  /**< The magnetometer's calibration, when one is stored. */
    bool     cal_present;      /**< `mag_cal` holds one.                    */
#if CONFIG_KODE_LOWPOWER
    uint8_t  lp_who;       /**< The id lowpower_join() gave.                */
#endif
} Imu;

_Static_assert(KDRV_QMC5883P_CAL_TEXT_MAX <= KODE_CONFIG_VALUE_MAX,
               "a calibration's text is one setting's value");
_Static_assert(MAG_CAL_AXES == KDRV_QMC5883P_AXES,
               "MagCalEvt's arrays are copied whole into a kdrv_qmc5883p_cal_t");

/**
 * @brief The drained window. One buffer, 768 bytes of .bss.
 *
 * Static rather than on the stack because it is exactly the size an AO stack
 * cannot spare, and there is never a second window: one FIFO, one drain, one
 * owner. It outlives a step on purpose - a paced drain fills it across eight
 * of them.
 */
static int16_t s_xyz[3U * KDRV_QMI8658_FIFO_DEPTH];

static Imu l_imu;
QActive * const AO_Imu = &l_imu.super;

static QState Imu_initial(Imu * const me, void const * const par);
static QState Imu_up(Imu * const me, QEvt const * const e);

/* ---- Timers ---------------------------------------------------------------- */

/**
 * @brief Disarm the one-shot and say whether its expiry is still queued.
 *
 * A timer that already fired has an event queued that belongs to nothing, and
 * acting on it would end the window that replaced the one it was timing.
 *
 * @param te   The one-shot.
 * @param live Whether the caller considers a previous arming outstanding.
 * @return true when an expired event for @p te is still queued.
 */
static bool timer_stale(QTimeEvt * const te, bool live)
{
    bool stale = false;

    if ( !QTimeEvt_disarm(te) )
    {
        stale = live;
    }
    return stale;
}

/** @brief Stop the one-shot, counting an expiry that is already queued. */
static void wait_stop(Imu * const me)
{
    if ( timer_stale(&me->wait, me->wait_live) )
    {
        me->wait_stale += 1U;
    }
    me->wait_live = false;
}

/**
 * @brief Start the one-shot for @p ms.
 *
 * @param me The AO.
 * @param ms How long to wait.
 */
static void wait_start(Imu * const me, uint32_t ms)
{
    uint32_t ticks = KQP_TICKS_PER_MS(ms);

    wait_stop(me);
    if ( 0U == ticks )
    {
        /* A window shorter than one tick is still a window, and QP resets on
         * an arming of zero. */
        ticks = 1U;
    }
    QTimeEvt_armX(&me->wait, (QTimeEvtCtr)ticks, 0U);
    me->wait_live = true;
}

/**
 * @brief A part was just used: its rest is KODE_SYS_IMU_REST_AFTER_MS from now.
 *
 * @param p The part.
 */
static void part_used(ImuPart * const p)
{
    uint32_t const ticks = KQP_TICKS_PER_MS(KODE_SYS_IMU_REST_AFTER_MS);

    if ( timer_stale(&p->te, p->live) )
    {
        p->stale += 1U;
    }
    QTimeEvt_armX(&p->te, (QTimeEvtCtr)ticks, 0U);
    p->live = true;
}

/* ---- Answering ------------------------------------------------------------- */

/**
 * @brief Is somebody still waiting on the request tagged @p gen?
 *
 * The facade answers every read into ONE shared storage, and a caller whose
 * wait ran out leaves its request queued here. So a request is answered only
 * while the facade's current tag is still its own. The load is an acquire
 * against the facade's release in its claim: the newer caller stores its tag
 * before it posts, so this never reads an older one across the two cores.
 *
 * @param gen The request's own tag.
 * @param cur Where the requester keeps its current tag. NULL: always current.
 * @return true when the request is to be answered.
 */
static bool gen_current(uint32_t gen, uint32_t const *cur)
{
    bool live = true;

    if ( NULL != cur )
    {
        live = (__atomic_load_n(cur, __ATOMIC_ACQUIRE) == gen);
    }
    if ( !live )
    {
        KLOG_W(TAG, "a read its caller stopped waiting for is discarded, "
                    "unanswered");
    }
    return live;
}

/**
 * @brief The end of every answer: the status, the tag, the signal.
 *
 * The generation goes in before the semaphore, as a release store: it is what
 * the caller's read acquires, and what orders the data written before it
 * across the two cores. A plain store could be reordered. The builtin is a
 * GCC extension, which both toolchains of this tree support.
 *
 * @param out_rc Where the status goes. May be NULL.
 * @param rc     The status.
 * @param ack    Where @p gen is written back. May be NULL.
 * @param gen    The requester's tag.
 * @param done   Signalled last. May be NULL.
 */
static void answer_end(uint32_t *out_rc, kode_status_t rc, uint32_t *ack, uint32_t gen,
                       khal_sem_t done)
{
    if ( NULL != out_rc )
    {
        *out_rc = (uint32_t)rc;
    }
    if ( NULL != ack )
    {
        __atomic_store_n(ack, gen, __ATOMIC_RELEASE);
    }
    if ( NULL != done )
    {
        khal_sem_signal(done);
    }
}

/* ---- The asks: kode_sys's requests, answered to the asking AO --------------
 * One door per asker and kind: an ask takes it, the answer opens it again
 * just before it is posted, so the asker may ask again from the answer's
 * handler. The device's own contention is the handler's refusal, answered.
 */

/** @brief The kinds of ask, one door each per asker. */
enum {
    IMU_ASK_SAMPLE = 0,  /**< imu_read_ask()  */
    IMU_ASK_MAG,         /**< mag_read_ask()  */
    IMU_ASK_CAL,         /**< mag_cal_ask()   */
    IMU_ASK_KINDS        /**< How many there are. */
};

/** @brief The doors, one per kind of ask. */
static kaos_ask_door_t s_imu_doors[IMU_ASK_KINDS];

/**
 * @brief Answer an IMU read request and wake whoever is asleep on it.
 *
 * Nothing at all - no sample, no status, no acknowledgement, no signal - for
 * a request gen_current() says nobody is waiting on.
 *
 * @param q      The request.
 * @param rc     What to tell them.
 * @param sample The reading. Written to @c out only when @p rc is KODE_OK;
 *               NULL when there is none to report.
 */
static void read_finish(ImuReadEvt const * const q, kode_status_t rc,
                        const kdrv_qmi8658_sample_t *sample)
{
    if ( NULL != q->who )
    {
        ImuSampleRspEvt * const a = q->rsp;

        a->rc = (uint8_t)rc;
        (void)memset(&a->sample, 0, sizeof(a->sample));
        if ( (KODE_OK == rc) && (NULL != sample) )
        {
            (void)memcpy(a->sample.a_mg, sample->a_mg, sizeof(a->sample.a_mg));
            (void)memcpy(a->sample.w_mdps, sample->w_mdps, sizeof(a->sample.w_mdps));
            a->sample.temp_dc = sample->temp_dc;
        }
        kaos_ask_answer(&s_imu_doors[IMU_ASK_SAMPLE], q->who, &a->super, AO_Imu);
        return;
    }
    if ( !gen_current(q->gen, q->cur) )
    {
        return;
    }
    if ( (KODE_OK == rc) && (NULL != q->out) && (NULL != sample) )
    {
        *(kdrv_qmi8658_sample_t *)q->out = *sample;
    }
    answer_end(q->rc, rc, q->ack, q->gen, q->done);
}

/**
 * @brief Answer a magnetometer read request: read_finish()'s contract, with
 *        the field's three axes in place of the IMU's sample.
 *
 * @param q      The request.
 * @param status What to tell the caller.
 * @param mg     The reading, milligauss on the chip's own axes; NULL when
 *               there is none to report.
 */
static void mag_read_finish(MagReadEvt const * const q, kode_status_t status,
                            const int32_t *mg)
{
    if ( NULL != q->who )
    {
        MagSampleRspEvt * const a = q->rsp;

        a->rc = (uint8_t)status;
        (void)memset(a->mg, 0, sizeof(a->mg));
        if ( (KODE_OK == status) && (NULL != mg) )
        {
            (void)memcpy(a->mg, mg, sizeof(a->mg));
        }
        kaos_ask_answer(&s_imu_doors[IMU_ASK_MAG], q->who, &a->super, AO_Imu);
        return;
    }
    if ( !gen_current(q->gen, q->cur) )
    {
        return;
    }
    if ( (KODE_OK == status) && (NULL != q->out) && (NULL != mg) )
    {
        (void)memcpy(q->out, mg, 3U * sizeof(int32_t));
    }
    answer_end(q->rc, status, q->ack, q->gen, q->done);
}

/* ---- The magnetometer's calibration -----------------------------------------
 * The `mag_cal` setting: this AO is the only reader and writer of its stored
 * text, for the console's `config` and for the facade's door alike.
 */

/** @brief Read the stored calibration at start-up; one that cannot be read is none. */
static void cal_load(Imu * const me)
{
    /* One byte of room past the longest text tells a longer value from a full one. */
    char          text[KDRV_QMC5883P_CAL_TEXT_MAX + 2U];
    kode_status_t rc;

    me->cal_present = false;
    rc = khal_nvs_get_str(KSET_NAMESPACE, KSET_KEY_MAG_CAL, text, sizeof(text));
    if ( KODE_OK == rc )
    {
        if ( strlen(text) > KDRV_QMC5883P_CAL_TEXT_MAX )
        {
            KLOG_W(TAG, "the stored magnetometer calibration is too long; ignored");
        }
        else
        {
            me->cal_present = kdrv_qmc5883p_cal_parse(text, &me->corr);
            if ( !me->cal_present )
            {
                KLOG_W(TAG, "the stored magnetometer calibration is unreadable; ignored");
            }
        }
    }
    else if ( KODE_DENIED != rc )
    {
        /* A value too long for the buffer fails here on the board. */
        KLOG_W(TAG, "the stored magnetometer calibration could not be read; ignored");
    }
    else
    {
        /* Nothing stored. */
    }
}

/**
 * @brief Store a calibration, or forget it.
 *
 * @param me  The AO.
 * @param corr The calibration; NULL forgets.
 * @return KODE_OK; KODE_DENIED when @p corr is past its limits; the store's
 *         refusal otherwise, and the old calibration stays.
 */
static kode_status_t cal_store(Imu * const me, const kdrv_qmc5883p_cal_t *corr)
{
    char          text[KDRV_QMC5883P_CAL_TEXT_MAX + 1U];
    kode_status_t rc;

    if ( NULL == corr )
    {
        rc = khal_nvs_erase(KSET_NAMESPACE, KSET_KEY_MAG_CAL);
        if ( KODE_OK == rc )
        {
            me->cal_present = false;
            KLOG_N(TAG, "magnetometer calibration cleared (stored)");
        }
    }
    else if ( kdrv_qmc5883p_cal_format(corr, text, sizeof(text)) == 0U )
    {
        rc = KODE_DENIED;
    }
    else
    {
        rc = khal_nvs_set_str(KSET_NAMESPACE, KSET_KEY_MAG_CAL, text);
        if ( KODE_OK == rc )
        {
            me->corr        = *corr;
            me->cal_present = true;
            KLOG_N(TAG, "magnetometer calibration %s (stored)", text);
        }
    }
    return rc;
}

/**
 * @brief Answer `config key="mag_cal"`, setting it first when asked.
 *
 * @param me The AO.
 * @param r  The request.
 */
static void cal_config(Imu * const me, KodeConfigReqEvt const * const r)
{
    kdrv_qmc5883p_cal_t corr;
    char                word[KODE_CONFIG_VALUE_MAX + 1U];
    kode_status_t       rc = KODE_OK;

    if ( strcmp(r->key, KSET_KEY_MAG_CAL) != 0 )
    {
        return;   /* another owner's key */
    }
    word[0] = '\0';
    if ( r->set )
    {
        if ( '\0' == r->s[0] )
        {
            rc = cal_store(me, NULL);
        }
        else if ( kdrv_qmc5883p_cal_parse(r->s, &corr) )
        {
            rc = cal_store(me, &corr);
        }
        else
        {
            rc = KODE_DENIED;
        }
    }
    if ( KODE_OK != rc )
    {
        rc = KODE_DENIED;   /* the store's refusal too: a config answer is OK or DENIED */
    }
    if ( me->cal_present )
    {
        (void)kdrv_qmc5883p_cal_format(&me->corr, word, sizeof(word));
    }
    (void)kode_config_answer(&me->super, r, rc, me->cal_present, word);
}

/**
 * @brief Answer an ask for the calibration with what is stored now.
 * @param me The AO.
 * @param q  The ask.
 * @param rc Its outcome.
 */
static void cal_answer(Imu const * const me, MagCalEvt const * const q, kode_status_t rc)
{
    MagCalRspEvt * const a = q->rsp;

    a->rc      = (uint8_t)rc;
    a->op      = q->op;
    a->present = me->cal_present;
    (void)memset(&a->corr, 0, sizeof(a->corr));
    if ( me->cal_present )
    {
        (void)memcpy(a->corr.offset_mg, me->corr.offset_mg, sizeof(a->corr.offset_mg));
        (void)memcpy(a->corr.scale_milli, me->corr.scale_milli, sizeof(a->corr.scale_milli));
    }
    kaos_ask_answer(&s_imu_doors[IMU_ASK_CAL], q->who, &a->super, AO_Imu);
}

/**
 * @brief Answer a calibration request: mag_read_finish()'s contract.
 *
 * @param me The AO.
 * @param q  The request.
 */
static void cal_request(Imu * const me, MagCalEvt const * const q)
{
    kode_status_t rc;

    if ( !gen_current(q->gen, q->cur) )
    {
        return;
    }
    if ( MAG_CAL_OP_SET == q->op )
    {
        kdrv_qmc5883p_cal_t corr;

        (void)memcpy(corr.offset_mg, q->offset_mg, sizeof(corr.offset_mg));
        (void)memcpy(corr.scale_milli, q->scale_milli, sizeof(corr.scale_milli));
        rc = cal_store(me, &corr);
    }
    else if ( MAG_CAL_OP_CLEAR == q->op )
    {
        rc = cal_store(me, NULL);
    }
    else if ( NULL != q->who )
    {
        rc = KODE_OK;   /* a get: the answer below carries it */
    }
    else
    {
        /* cppcheck-suppress misra-c2012-11.5 ; the request carries the extended layer's MagCalSlot as a void pointer */
        MagCalSlot * const slot = (MagCalSlot *)q->out;

        rc = KODE_DENIED;
        if ( NULL != slot )
        {
            slot->present = me->cal_present;
            slot->corr    = me->corr;
            rc            = KODE_OK;
        }
    }
    if ( NULL != q->who )
    {
        cal_answer(me, q, rc);
        return;
    }
    answer_end(q->rc, rc, q->ack, q->gen, q->done);
}

/**
 * @brief Tell @p who how the arming went.
 *
 * @param me  The AO.
 * @param who Where the answer goes.
 * @param rc  What to say.
 */
static void armed_post(Imu * const me, QActive *who, kode_status_t rc)
{
    ImuArmedEvt *e;

    if ( NULL == who )
    {
        return;
    }
    e = Q_NEW_X(ImuArmedEvt, 0U, IMU_ARMED_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; a capture's outcome is lost");
        return;
    }
    e->rc = (uint8_t)rc;
    QACTIVE_POST(who, &e->super, &me->super);
}

/**
 * @brief Tell @p who what the drained window amounted to.
 *
 * @param me  The AO.
 * @param who Where the answer goes.
 * @param rc  The drain's verdict.
 * @param n   How many samples the window held.
 * @param w   What the detector measured. NULL when there was nothing to
 *            measure.
 */
static void energy_post(Imu * const me, QActive *who, kode_status_t rc,
                        size_t n, const kimu_energy_t *w)
{
    ImuEnergyEvt *e;

    if ( NULL == who )
    {
        return;
    }
    e = Q_NEW_X(ImuEnergyEvt, 0U, IMU_ENERGY_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; a witness window's answer is lost");
        return;
    }
    e->rc      = (uint8_t)rc;
    e->n       = (uint16_t)n;
    e->bin_x10 = 0;
    e->rms_x10 = 0;
    e->axis    = 0U;
    if ( NULL != w )
    {
        e->bin_x10 = w->bin_x10;
        e->rms_x10 = w->rms_x10;
        e->axis    = w->axis;
    }
    QACTIVE_POST(who, &e->super, &me->super);
}

/**
 * @brief The length of a three-axis vector, in the unit its axes carry.
 *
 * Integer throughout, and 64-bit inside: two thousand degrees per second
 * squared is four times what an int32_t holds, and a magnitude that silently
 * wrapped would read as a board at rest.
 *
 * @param v The axes.
 * @return The magnitude, rounded down.
 */
static int32_t magnitude(const int32_t v[3])
{
    uint64_t rest = 0U;
    uint64_t root = 0U;
    uint64_t bit;
    unsigned i;

    for ( i = 0U; 3U > i; ++i )
    {
        int64_t const a = (int64_t)v[i];
        rest += (uint64_t)(a * a);
    }
    bit = (uint64_t)1U << 62U;
    while ( bit > rest )
    {
        bit >>= 2U;
    }
    while ( 0U != bit )
    {
        if ( (root + bit) <= rest )
        {
            rest -= root + bit;
            root = (root >> 1U) + bit;
        }
        else
        {
            root >>= 1U;
        }
        bit >>= 2U;
    }
    return (int32_t)root;
}

/* ---- The records the two rows publish ---------------------------------------- */

/**
 * @brief Publish one record of the IMU row.
 *
 * @param me    The AO.
 * @param step  1-based.
 * @param phase IMU_TEST_*
 * @param flags IMU_TF_*
 * @param a     What this phase measured first. See the phase.
 * @param b
 * @param c
 * @param d
 */
static void row_publish(Imu * const me, uint8_t step, uint8_t phase,
                        uint8_t flags, int32_t a, int32_t b, int32_t c,
                        int32_t d)
{
    ImuTestEvt *e = Q_NEW_X(ImuTestEvt, 0U, IMU_TEST_SIG);

    if ( NULL == e )
    {
        /* Bounded, never fatal: a record nobody sees costs the operator a
         * line, and asserting here would cost them the board. */
        return;
    }
    e->step  = step;
    e->phase = phase;
    e->flags = flags;
    e->a     = a;
    e->b     = b;
    e->c     = c;
    e->d     = d;
    QACTIVE_PUBLISH(&e->super, &me->super);
}

/**
 * @brief The IMU row's verdict, and the end of the row.
 *
 * Logged before it is published, because the log is what the operator reads:
 * the console prints `result=fail` and nothing else, and the reason has to be
 * on the line above it.
 *
 * @param me  The AO.
 * @param ok  Whether the part passed.
 * @param why What to say when it did not.
 */
static void row_verdict(Imu * const me, bool ok, const char *why)
{
    TestResultEvt *e;

    if ( ok )
    {
        KLOG_N(TAG, "test imu: PASS");
    }
    else
    {
        KLOG_E(TAG, "test imu: FAIL - %s", why);
    }
    wait_stop(me);
    me->phase = PHASE_NONE;

    e = Q_NEW_X(TestResultEvt, 0U, TEST_RESULT_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; the IMU verdict is lost");
        return;
    }
    (void)snprintf(e->name, sizeof(e->name), "%s", "imu");
    e->ok = ok;
    e->origin = TEST_ORIGIN_AUTOTEST;
    QACTIVE_PUBLISH(&e->super, &me->super);
}

/**
 * @brief Publish a row's refusal when it could not even start.
 *
 * row_verdict()'s TEST_RESULT_SIG without its wait_stop() and phase reset: no
 * row has started, and the one-shot may be timing a capture's deadline that
 * this must not touch. The caller has logged why; both rows use it.
 *
 * @param me   The AO.
 * @param name The row's name: "imu" or "mag".
 */
static void test_refused(Imu * const me, const char *name)
{
    TestResultEvt *e;

    KLOG_W(TAG, "test %s: the IMU is held by a window or a row; ask again "
                "after it", name);
    e = Q_NEW_X(TestResultEvt, 0U, TEST_RESULT_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; the %s refusal itself is lost", name);
        return;
    }
    (void)snprintf(e->name, sizeof(e->name), "%s", name);
    e->ok = false;
    e->origin = TEST_ORIGIN_AUTOTEST;
    QACTIVE_PUBLISH(&e->super, &me->super);
}

/**
 * @brief Publish one record of the magnetometer row.
 *
 * @param me    The AO.
 * @param step  1-based.
 * @param phase MAG_TEST_*
 * @param flags MAG_TF_*
 * @param a     See MagTestEvt, per phase.
 * @param b     See MagTestEvt, per phase.
 * @param c     See MagTestEvt, per phase.
 * @param d     See MagTestEvt, per phase.
 */
static void mag_row_publish(Imu * const me, uint8_t step, uint8_t phase,
                            uint8_t flags, int32_t a, int32_t b, int32_t c,
                            int32_t d)
{
    MagTestEvt *e = Q_NEW_X(MagTestEvt, 0U, MAG_TEST_SIG);

    if ( NULL == e )
    {
        /* row_publish()'s reason: a record nobody sees costs a line. */
        KLOG_E(TAG, "out of events; a record of the mag row is lost");
        return;
    }
    e->step  = step;
    e->phase = phase;
    e->flags = flags;
    e->a     = a;
    e->b     = b;
    e->c     = c;
    e->d     = d;
    QACTIVE_PUBLISH(&e->super, &me->super);
}

/**
 * @brief The magnetometer row's verdict, and the end of the row.
 *
 * row_verdict()'s shape: the reason is logged on the line above the verdict,
 * because `result=fail` is all the console prints. The end marker goes out
 * first, so the console knows the records are over.
 *
 * @param me  The AO.
 * @param ok  Whether the part passed.
 * @param why What to say when it did not.
 */
static void mag_row_verdict(Imu * const me, bool ok, const char *why)
{
    TestResultEvt *e;
    uint8_t        flags = 0U;

    if ( ok )
    {
        flags = (uint8_t)MAG_TF_PASS;
        KLOG_N(TAG, "test mag: PASS");
    }
    else
    {
        KLOG_E(TAG, "test mag: FAIL - %s", why);
    }
    /* Step 0: the end marker is no step of its own. */
    mag_row_publish(me, 0U, (uint8_t)MAG_TEST_DONE, flags, 0, 0, 0, 0);

    e = Q_NEW_X(TestResultEvt, 0U, TEST_RESULT_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; the mag verdict is lost");
        return;
    }
    (void)snprintf(e->name, sizeof(e->name), "%s", "mag");
    e->ok = ok;
    e->origin = TEST_ORIGIN_AUTOTEST;
    QACTIVE_PUBLISH(&e->super, &me->super);
}

/* ---- The requests kaos.h publishes ----------------------------------------- */

void imu_read_request(void *out, uint32_t *rc, khal_sem_t done, uint32_t gen,
                      uint32_t const *cur, uint32_t *ack)
{
    ImuReadEvt *e = Q_NEW_X(ImuReadEvt, 0U, IMU_READ_REQ_SIG);

    if ( NULL == e )
    {
        /* A dry pool must not leave a caller asleep on a request that was
         * never made. Answered here, with @p out left as it was. */
        if ( NULL != rc )
        {
            *rc = (uint32_t)KODE_NO_RESOURCES;
        }
        if ( NULL != ack )
        {
            *ack = gen;
        }
        if ( NULL != done )
        {
            khal_sem_signal(done);
        }
        return;
    }
    e->out  = out;
    e->rc   = rc;
    e->done = done;
    e->gen  = gen;
    e->cur  = cur;
    e->ack  = ack;
    e->who  = NULL;
    e->rsp  = NULL;
    QACTIVE_POST(AO_Imu, &e->super, NULL);
}

void imu_capture_request(QActive *who)
{
    ImuCaptureReqEvt *e = Q_NEW_X(ImuCaptureReqEvt, 0U, IMU_CAPTURE_REQ_SIG);

    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; a witness capture was never asked for");
        return;
    }
    e->who = who;
    QACTIVE_POST(AO_Imu, &e->super, NULL);
}

void imu_drain_request(QActive *who, uint16_t hz)
{
    ImuDrainReqEvt *e = Q_NEW_X(ImuDrainReqEvt, 0U, IMU_DRAIN_REQ_SIG);

    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; a witness window will not be drained");
        return;
    }
    e->who = who;
    e->hz  = hz;
    QACTIVE_POST(AO_Imu, &e->super, NULL);
}

void imu_test_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(IMU_TEST_REQ_SIG);

    QACTIVE_POST(AO_Imu, &evt, NULL);
}

bool mag_read_request(void *out, uint32_t *rc, khal_sem_t done, uint32_t gen,
                      uint32_t const *cur, uint32_t *ack)
{
    MagReadEvt *e      = Q_NEW_X(MagReadEvt, 0U, MAG_READ_REQ_SIG);
    bool        queued = true;

    if ( NULL == e )
    {
        /* imu_read_request()'s own dry-pool rule: a caller must not be left
         * asleep on a request that was never made. */
        if ( NULL != rc )
        {
            *rc = (uint32_t)KODE_NO_RESOURCES;
        }
        if ( NULL != ack )
        {
            *ack = gen;
        }
        if ( NULL != done )
        {
            khal_sem_signal(done);
        }
    }
    else
    {
        e->out  = out;
        e->rc   = rc;
        e->done = done;
        e->gen  = gen;
        e->cur  = cur;
        e->ack  = ack;
        e->who  = NULL;
        e->rsp  = NULL;
        queued  = QACTIVE_POST_X(AO_Imu, &e->super, KQP_IMU_CMD_MARGIN, NULL);
    }
    return queued;
}

bool mag_cal_request(uint8_t op, const kdrv_qmc5883p_cal_t *corr, MagCalSlot *out,
                     uint32_t *rc, khal_sem_t done, uint32_t gen, uint32_t const *cur,
                     uint32_t *ack)
{
    MagCalEvt *e      = Q_NEW_X(MagCalEvt, 0U, MAG_CAL_REQ_SIG);
    bool       queued = true;

    if ( NULL == e )
    {
        /* mag_read_request()'s own dry-pool rule. */
        if ( NULL != rc )
        {
            *rc = (uint32_t)KODE_NO_RESOURCES;
        }
        if ( NULL != ack )
        {
            *ack = gen;
        }
        if ( NULL != done )
        {
            khal_sem_signal(done);
        }
    }
    else
    {
        e->op = op;
        (void)memset(e->offset_mg, 0, sizeof(e->offset_mg));
        (void)memset(e->scale_milli, 0, sizeof(e->scale_milli));
        if ( NULL != corr )
        {
            (void)memcpy(e->offset_mg, corr->offset_mg, sizeof(e->offset_mg));
            (void)memcpy(e->scale_milli, corr->scale_milli, sizeof(e->scale_milli));
        }
        e->out  = out;
        e->rc   = rc;
        e->done = done;
        e->gen  = gen;
        e->cur  = cur;
        e->ack  = ack;
        e->who  = NULL;
        e->rsp  = NULL;
        queued  = QACTIVE_POST_X(AO_Imu, &e->super, KQP_IMU_CMD_MARGIN, NULL);
    }
    return queued;
}

kode_status_t imu_read_ask(QActive *who)
{
    kode_status_t   rc  = kaos_ask_take(&s_imu_doors[IMU_ASK_SAMPLE], who);
    ImuSampleRspEvt *a;
    ImuReadEvt      *e;

    if ( KODE_OK != rc )
    {
        return rc;
    }
    a = Q_NEW_X(ImuSampleRspEvt, 0U, IMU_SAMPLE_RSP_SIG);
    if ( NULL == a )
    {
        return kaos_ask_cancel(&s_imu_doors[IMU_ASK_SAMPLE], who, NULL);
    }
    e = Q_NEW_X(ImuReadEvt, 0U, IMU_READ_REQ_SIG);
    if ( NULL == e )
    {
        return kaos_ask_cancel(&s_imu_doors[IMU_ASK_SAMPLE], who, &a->super);
    }
    e->out  = NULL;
    e->rc   = NULL;
    e->done = NULL;
    e->gen  = 0U;     /* an ask is answered by its event, not matched by a tag */
    e->cur  = NULL;
    e->ack  = NULL;
    e->who  = who;
    e->rsp  = a;
    return kaos_ask_post(&s_imu_doors[IMU_ASK_SAMPLE], who, AO_Imu, &e->super, &a->super,
                         KQP_IMU_CMD_MARGIN);
}

kode_status_t mag_read_ask(QActive *who)
{
    kode_status_t    rc  = kaos_ask_take(&s_imu_doors[IMU_ASK_MAG], who);
    MagSampleRspEvt *a;
    MagReadEvt      *e;

    if ( KODE_OK != rc )
    {
        return rc;
    }
    a = Q_NEW_X(MagSampleRspEvt, 0U, MAG_SAMPLE_RSP_SIG);
    if ( NULL == a )
    {
        return kaos_ask_cancel(&s_imu_doors[IMU_ASK_MAG], who, NULL);
    }
    e = Q_NEW_X(MagReadEvt, 0U, MAG_READ_REQ_SIG);
    if ( NULL == e )
    {
        return kaos_ask_cancel(&s_imu_doors[IMU_ASK_MAG], who, &a->super);
    }
    e->out  = NULL;
    e->rc   = NULL;
    e->done = NULL;
    e->gen  = 0U;     /* an ask is answered by its event, not matched by a tag */
    e->cur  = NULL;
    e->ack  = NULL;
    e->who  = who;
    e->rsp  = a;
    return kaos_ask_post(&s_imu_doors[IMU_ASK_MAG], who, AO_Imu, &e->super, &a->super,
                         KQP_IMU_CMD_MARGIN);
}

kode_status_t mag_cal_ask(QActive *who, uint8_t op, const kdrv_qmc5883p_cal_t *corr)
{
    kode_status_t rc  = kaos_ask_take(&s_imu_doors[IMU_ASK_CAL], who);
    MagCalRspEvt *a;
    MagCalEvt    *e;

    if ( KODE_OK != rc )
    {
        return rc;
    }
    a = Q_NEW_X(MagCalRspEvt, 0U, MAG_CAL_RSP_SIG);
    if ( NULL == a )
    {
        return kaos_ask_cancel(&s_imu_doors[IMU_ASK_CAL], who, NULL);
    }
    e = Q_NEW_X(MagCalEvt, 0U, MAG_CAL_REQ_SIG);
    if ( NULL == e )
    {
        return kaos_ask_cancel(&s_imu_doors[IMU_ASK_CAL], who, &a->super);
    }
    e->op = op;
    (void)memset(e->offset_mg, 0, sizeof(e->offset_mg));
    (void)memset(e->scale_milli, 0, sizeof(e->scale_milli));
    if ( NULL != corr )
    {
        (void)memcpy(e->offset_mg, corr->offset_mg, sizeof(e->offset_mg));
        (void)memcpy(e->scale_milli, corr->scale_milli, sizeof(e->scale_milli));
    }
    e->out  = NULL;
    e->rc   = NULL;
    e->done = NULL;
    e->gen  = 0U;     /* an ask is answered by its event, not matched by a tag */
    e->cur  = NULL;
    e->ack  = NULL;
    e->who  = who;
    e->rsp  = a;
    return kaos_ask_post(&s_imu_doors[IMU_ASK_CAL], who, AO_Imu, &e->super, &a->super,
                         KQP_IMU_CMD_MARGIN);
}

void mag_test_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(MAG_TEST_REQ_SIG);

    QACTIVE_POST(AO_Imu, &evt, NULL);
}

/* ---- The IMU row ------------------------------------------------------------
 * `test component=imu`, three steps in fault-localisation order: the part's
 * identity; the manufacturer's self-test, which does not depend on how the
 * board is held; and the board at rest, last because moving it spoils it.
 * No step busy-waits: the AO starts a self-test, arms the one-shot, and leaves.
 */

/**
 * @brief The gyroscope has just been switched on: note when.
 *
 * What the note is for is the next thing that wants the part: a window or a
 * row has to wait SETTLE_MS from here before the part will act on a FIFO
 * reset or a self-test trigger, and a read GYRO_READY_MS before the rotation
 * it reads means anything.
 *
 * @param me The AO.
 */
static void gyro_back(Imu * const me)
{
    me->gyro_on_ms = khal_time_ms();
}

/**
 * @brief How much of @p span is still to run since the gyroscope came on.
 *
 * @param me   The AO.
 * @param span SETTLE_MS for a command, GYRO_READY_MS for a read.
 * @return 0 once @p span ms have passed since gyro_back().
 */
static uint32_t settle_left_ms(Imu const * const me, uint32_t span)
{
    uint32_t const gone = khal_time_ms() - me->gyro_on_ms;

    if ( gone >= span )
    {
        return 0U;
    }
    return span - gone;
}

/**
 * @brief The IMU is converting, whoever switched it on: a wake, or a row or
 *        a window handing the part back with both sensors enabled.
 *
 * @param me The AO.
 */
static void imu_running(Imu * const me)
{
    me->imu.awake = true;
    gyro_back(me);
    part_used(&me->imu);
}

/**
 * @brief Wake the IMU if it is resting, and count this as a use of it.
 *
 * @param me The AO.
 * @return true when the part is converting; false with no part, or when it
 *         would not leave standby.
 */
static bool imu_wake(Imu * const me)
{
    if ( !me->imu_up )
    {
        return false;
    }
    if ( me->imu.awake )
    {
        part_used(&me->imu);
        return true;
    }
    if ( kdrv_qmi8658_standby(false) != KODE_OK )
    {
        KLOG_W(TAG, "the IMU would not leave standby");
        return false;
    }
    KLOG_D(TAG, "the IMU is awake");
    imu_running(me);
    return true;
}

/**
 * @brief Wake the magnetometer if it is resting, and count this as a use.
 *
 * @param me The AO.
 * @return true when the part is converting; false with no part, or when it
 *         would not leave standby.
 */
static bool mag_wake(Imu * const me)
{
    if ( !me->mag_up )
    {
        return false;
    }
    if ( !me->mag.awake )
    {
        if ( kdrv_qmc5883p_standby(false) != KODE_OK )
        {
            KLOG_W(TAG, "the magnetometer would not leave standby");
            return false;
        }
        me->mag.awake = true;
        KLOG_D(TAG, "the magnetometer is awake");
    }
    part_used(&me->mag);
    return true;
}

/**
 * @brief Write a part back to standby, and believe it rests whatever it said.
 *
 * A part that will not take the write is believed resting all the same, so
 * the next request wakes it afresh instead of this asking every two seconds
 * for good.
 *
 * @param p      The part.
 * @param is_imu true for the QMI8658A, false for the QMC5883P.
 */
static void part_standby(ImuPart * const p, bool is_imu)
{
    kode_status_t rc;

    if ( is_imu )
    {
        rc = kdrv_qmi8658_standby(true);
    }
    else
    {
        rc = kdrv_qmc5883p_standby(true);
    }
    p->awake = false;
    if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "a part would not go back to standby");
    }
    else if ( is_imu )
    {
        KLOG_D(TAG, "the IMU rests");
    }
    else
    {
        KLOG_D(TAG, "the magnetometer rests");
    }
}

/**
 * @brief A part's rest one-shot fired: nobody asked for it in a whole
 *        KODE_SYS_IMU_REST_AFTER_MS, so it goes back to standby.
 *
 * Not while a row or a window holds the IMU: the part is then in the middle
 * of something, and the one-shot is simply started again.
 *
 * @param me     The AO.
 * @param p      The part.
 * @param is_imu true for the QMI8658A, false for the QMC5883P.
 */
static void part_rest(Imu const * const me, ImuPart * const p, bool is_imu)
{
    if ( 0U < p->stale )
    {
        p->stale -= 1U;
        return;
    }
    p->live = false;
    if ( !p->awake )
    {
        return;
    }
    if ( is_imu && (PHASE_NONE != me->phase) )
    {
        part_used(p);
        return;
    }
    part_standby(p, is_imu);
}

#if CONFIG_KODE_LOWPOWER
/**
 * @brief Put a part to rest for the board's sleep and silence its one-shot.
 *
 * The expiry that may already be queued is counted, exactly as a new use
 * counts it, so it is dropped when it comes.
 *
 * @param p      The part.
 * @param is_imu true for the QMI8658A, false for the QMC5883P.
 */
static void part_sleep(ImuPart * const p, bool is_imu)
{
    if ( timer_stale(&p->te, p->live) )
    {
        p->stale += 1U;
    }
    p->live = false;
    if ( p->awake )
    {
        part_standby(p, is_imu);
    }
}

/**
 * @brief LOWPOWER_ROUND_SIG: both parts to standby, and answer once.
 *
 * A row, a witness window or a read waiting for the gyroscope holds the part
 * in the middle of something the caller is timing: the round is refused.
 *
 * @param me The AO.
 * @param r  The round.
 */
static void imu_round(Imu * const me, const LowPowerRoundEvt * const r)
{
    if ( PHASE_NONE != me->phase )
    {
        lowpower_ready(me->lp_who, r->round, true, "imu busy");
    }
    else
    {
        part_sleep(&me->imu, true);
        part_sleep(&me->mag, false);
        lowpower_ready(me->lp_who, r->round, false, NULL);
    }
}
#endif /* CONFIG_KODE_LOWPOWER */

/**
 * @brief End the row: the part back to its running configuration, then the
 *        verdict.
 *
 * @param me  The AO.
 * @param ok  The verdict.
 * @param why Why not, when @p ok is false.
 */
static void imu_row_end(Imu * const me, bool ok, const char *why)
{
    if ( (PHASE_NONE != me->phase)
        && (kdrv_qmi8658_st_end() == KODE_OK) )
    {
        imu_running(me);
    }
    me->phase = PHASE_NONE;
    row_verdict(me, ok, why);
}

/**
 * @brief Start one sensor's self-test and the polling that watches it.
 *
 * @param me    The AO.
 * @param which The sensor.
 */
static void imu_st_begin(Imu * const me, kdrv_qmi8658_sensor_t which)
{
    if ( kdrv_qmi8658_st_begin(which) != KODE_OK )
    {
        imu_row_end(me, false, "the self-test could not be started");
        return;
    }
    if ( KDRV_QMI8658_XL == which )
    {
        me->phase = PHASE_ST_XL;
        me->polls = ST_XL_POLLS;
    }
    else
    {
        me->phase = PHASE_ST_GY;
        me->polls = ST_GY_POLLS;
    }
    wait_start(me, ST_POLL_MS);
}

/**
 * @brief Step 1: who the part is. Then the self-tests begin.
 *
 * The part is asked again here rather than trusted from boot: an operator
 * running the row may have just fitted it, and a part that answers now is
 * up from now on.
 *
 * @param me The AO.
 */
static void imu_row(Imu * const me)
{
    kdrv_qmi8658_id_t id = { 0U, 0U };
    kode_status_t     rc;

    rc = kdrv_qmi8658_create(&id);
    if ( KODE_IO_ERROR == rc )
    {
        me->imu_up = false;
        KLOG_E(TAG, "test imu: WHO_AM_I did not answer");
        row_publish(me, 1U, (uint8_t)IMU_TEST_WHOAMI, 0U, 0, 0, 0, 0);
        row_verdict(me, false, "the QMI8658A does not answer at 0x6A");
        return;
    }
    KLOG_N(TAG, "test imu: WHO_AM_I reads 0x%02X, revision 0x%02X",
           (unsigned)id.whoami, (unsigned)id.revision);
    if ( KODE_OK != rc )
    {
        me->imu_up = false;
        row_publish(me, 1U, (uint8_t)IMU_TEST_WHOAMI, 0U, (int32_t)id.whoami,
                    0, 0, 0);
        row_verdict(me, false, "what answers at 0x6A is not a QMI8658A");
        return;
    }
    me->imu_up = true;
    row_publish(me, 1U, (uint8_t)IMU_TEST_WHOAMI, (uint8_t)IMU_TF_PASS,
                (int32_t)id.whoami, 0, 0, 0);
    imu_st_begin(me, KDRV_QMI8658_XL);
}

/**
 * @brief Step 3: the board at rest, and the verdict.
 *
 * The gyroscope is judged here because step 2 has already proved it alive: a
 * rate near zero now means a still bench, not a dead sensor.
 *
 * @param me The AO.
 */
static void imu_rest(Imu * const me)
{
    kdrv_qmi8658_sample_t s;
    int32_t               a_mg;
    int32_t               w_mdps;
    int32_t               off;
    uint8_t               flags = 0U;
    const char           *why   = NULL;

    (void)memset(&s, 0, sizeof(s));
    if ( kdrv_qmi8658_read(&s) != KODE_OK )
    {
        imu_row_end(me, false, "the part passed its self-test and then would "
                               "not give a sample");
        return;
    }
    a_mg   = magnitude(s.a_mg);
    w_mdps = magnitude(s.w_mdps);
    off    = a_mg - IMU_ONE_G_MG;
    if ( 0 > off )
    {
        off = -off;
    }
    KLOG_N(TAG, "test imu: a %d %d %d mg, w %d %d %d mdps",
           (int)s.a_mg[0], (int)s.a_mg[1], (int)s.a_mg[2],
           (int)s.w_mdps[0], (int)s.w_mdps[1], (int)s.w_mdps[2]);
    KLOG_N(TAG, "test imu: at rest |a|=%d mg, |w|=%d mdps, die %d.%d C",
           (int)a_mg, (int)w_mdps, (int)(s.temp_dc / 10),
           (int)(s.temp_dc % 10));

    if ( (int32_t)KODE_IMU_GRAVITY_TOL_MG < off )
    {
        why = "gravity is not one g; the accelerometer is reading a scale "
              "that is not its own";
    }
    else if ( (int32_t)KODE_IMU_REST_MAX_MDPS < w_mdps )
    {
        why = "the gyroscope reads rotation on a bench that is not moving; "
              "hold the board still and run it again";
    }
    else if ( ((int32_t)KODE_IMU_TEMP_MIN_DC > s.temp_dc)
             || ((int32_t)KODE_IMU_TEMP_MAX_DC < s.temp_dc) )
    {
        why = "the die temperature is not a room's; the part is not converting";
    }
    else
    {
        flags = (uint8_t)IMU_TF_PASS;
    }
    row_publish(me, 3U, (uint8_t)IMU_TEST_GRAVITY, flags, a_mg, w_mdps,
                s.temp_dc, 0);
    imu_row_end(me, (NULL == why), why);
}

/**
 * @brief Step 2, one poll of it: is the self-test in flight over, and what
 *        did it move?
 *
 * @param me The AO.
 */
static void imu_st_poll(Imu * const me)
{
    kdrv_qmi8658_sensor_t which = KDRV_QMI8658_XL;
    int32_t               shift[3] = { 0, 0, 0 };
    bool                  done = false;
    uint8_t               flags = 0U;

    if ( PHASE_ST_GY == me->phase )
    {
        which = KDRV_QMI8658_GY;
    }
    if ( kdrv_qmi8658_st_poll(which, &done, shift) != KODE_OK )
    {
        imu_row_end(me, false, "the bus failed during the self-test");
        return;
    }
    if ( !done )
    {
        if ( 0U == me->polls )
        {
            imu_row_end(me, false, "the self-test never reported finishing");
            return;
        }
        me->polls -= 1U;
        wait_start(me, ST_POLL_MS);
        return;
    }

    if ( KDRV_QMI8658_XL == which )
    {
        KLOG_N(TAG, "test imu: accelerometer self-test shift %d %d %d mg",
               (int)shift[0], (int)shift[1], (int)shift[2]);
    }
    else
    {
        KLOG_N(TAG, "test imu: gyroscope self-test shift %d %d %d mdps",
               (int)shift[0], (int)shift[1], (int)shift[2]);
    }
    if ( kdrv_qmi8658_st_judge(which, shift) )
    {
        flags = (uint8_t)IMU_TF_PASS;
    }
    row_publish(me, 2U, (uint8_t)IMU_TEST_ST, flags, shift[0], shift[1],
                shift[2], (int32_t)which);
    if ( 0U == flags )
    {
        if ( KDRV_QMI8658_XL == which )
        {
            imu_row_end(me, false, "the accelerometer did not move under its "
                                   "own self-test: under 200 mg on some axis");
        }
        else
        {
            imu_row_end(me, false, "the gyroscope did not move under its own "
                                   "self-test: under 300 dps on some axis");
        }
        return;
    }
    if ( KDRV_QMI8658_XL == which )
    {
        imu_st_begin(me, KDRV_QMI8658_GY);
        return;
    }
    /* Both passed. Sensors back on, and the gyroscope its turn-on time. */
    if ( kdrv_qmi8658_st_end() != KODE_OK )
    {
        imu_row_end(me, false, "the part would not leave self-test");
        return;
    }
    imu_running(me);
    me->phase = PHASE_SETTLE;
    wait_start(me, SETTLE_MS);
}

/* ---- The witness window ------------------------------------------------------ */

/**
 * @brief End a witness window: the part back to its running configuration.
 *
 * @param me The AO.
 */
static void cap_end(Imu * const me)
{
    wait_stop(me);
    if ( kdrv_qmi8658_fifo_disarm() == KODE_OK )
    {
        imu_running(me);
    }
    me->phase = PHASE_NONE;
    me->who   = NULL;
}

/**
 * @brief Arm one FIFO window for @p who, now.
 *
 * The outcome goes back as IMU_ARMED_SIG, a refusal included, and a window
 * nobody drains is closed after KODE_IMU_CAPTURE_MAX_MS. The caller has
 * already made sure nothing holds the part.
 *
 * @param me  The AO.
 * @param who Where the IMU_ARMED_SIG goes.
 */
static void cap_arm(Imu * const me, QActive *who)
{
    kode_status_t rc;

    rc = kdrv_qmi8658_create(NULL);
    if ( KODE_OK == rc )
    {
        me->imu_up = true;
        rc = kdrv_qmi8658_fifo_arm();
    }
    else
    {
        me->imu_up = false;
    }
    armed_post(me, who, rc);
    if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "the FIFO would not arm; there is no witness window");
        /* An arm that failed half way has already turned the gyroscope off;
         * put the part back as it runs. Denied, harmlessly, with no part. */
        if ( kdrv_qmi8658_fifo_disarm() == KODE_OK )
        {
            imu_running(me);
        }
        return;
    }
    me->who   = who;
    me->phase = PHASE_CAP_ARMED;
    wait_start(me, KODE_IMU_CAPTURE_MAX_MS);
}

/**
 * @brief Hold @p what until the gyroscope has finished turning on.
 *
 * @param me   The AO.
 * @param what PENDING_ROW, PENDING_CAP or PENDING_READ.
 * @param who  For a capture, where its IMU_ARMED_SIG will go.
 * @param left How long the settle still has to run.
 */
static void settle_hold(Imu * const me, uint8_t what, QActive *who,
                        uint32_t left)
{
    me->pending = what;
    me->who     = who;
    me->phase   = PHASE_SETTLING;
    wait_start(me, left);
}

/**
 * @brief The settle is over: start what it was holding.
 *
 * @param me The AO.
 */
static void settle_over(Imu * const me)
{
    uint8_t const  what = me->pending;
    QActive * const who = me->who;

    me->pending = PENDING_NONE;
    me->phase   = PHASE_NONE;
    me->who     = NULL;
    if ( PENDING_ROW == what )
    {
        imu_row(me);
    }
    else if ( PENDING_CAP == what )
    {
        cap_arm(me, who);
    }
    else if ( PENDING_READ == what )
    {
        kdrv_qmi8658_sample_t s;
        kode_status_t         rc;

        (void)memset(&s, 0, sizeof(s));
        rc = kdrv_qmi8658_read(&s);
        read_finish(&me->held, rc, &s);
    }
    else
    {
        /* nothing was waiting */
    }
}

/**
 * @brief IMU_DRAIN_REQ_SIG: open the window and start taking it out.
 *
 * @param me The AO.
 * @param q  The request.
 */
static void cap_drain(Imu * const me, ImuDrainReqEvt const * const q)
{
    size_t        samples = 0U;
    kode_status_t rc;

    if ( (PHASE_CAP_ARMED != me->phase) || (q->who != me->who) )
    {
        energy_post(me, q->who, KODE_DENIED, 0U, NULL);
        return;
    }
    rc = kdrv_qmi8658_fifo_open(&samples);
    if ( KODE_OK != rc )
    {
        energy_post(me, me->who, rc, 0U, NULL);
        cap_end(me);
        return;
    }
    me->hz     = q->hz;
    me->left   = (uint16_t)samples;
    me->stored = 0U;
    me->phase  = PHASE_CAP_DRAIN;
    wait_start(me, CAP_CHUNK_MS);
}

/**
 * @brief Say what a drained window holds, at debug level: the extremes of
 *        each axis in LSB and the first two samples.
 *
 * The detector's bin and broadband figure do not show the raw samples; this
 * does. Debug, because it is nine numbers per window.
 *
 * @param n Samples in s_xyz.
 */
static void window_describe(uint16_t n)
{
    int16_t lo[3] = { INT16_MAX, INT16_MAX, INT16_MAX };
    int16_t hi[3] = { INT16_MIN, INT16_MIN, INT16_MIN };

    for ( uint16_t i = 0U; i < n; ++i )
    {
        for ( uint8_t a = 0U; 3U > a; ++a )
        {
            int16_t const v = s_xyz[(3U * (size_t)i) + a];

            if ( v < lo[a] )
            {
                lo[a] = v;
            }
            if ( v > hi[a] )
            {
                hi[a] = v;
            }
        }
    }
    KLOG_D(TAG, "window n=%u lsb: x %d..%d y %d..%d z %d..%d; first %d %d %d, "
                "then %d %d %d", (unsigned)n, (int)lo[0], (int)hi[0],
           (int)lo[1], (int)hi[1], (int)lo[2], (int)hi[2],
           (int)s_xyz[0], (int)s_xyz[1], (int)s_xyz[2],
           (int)s_xyz[3], (int)s_xyz[4], (int)s_xyz[5]);
}

/**
 * @brief One chunk of the window; on the last, measure it and answer.
 *
 * @param me The AO.
 */
static void cap_chunk(Imu * const me)
{
    kimu_energy_t w;
    kode_status_t rc = KODE_OK;
    uint16_t      n  = me->left;

    if ( (uint16_t)CAP_CHUNK < n )
    {
        n = (uint16_t)CAP_CHUNK;
    }
    if ( 0U < n )
    {
        rc = kdrv_qmi8658_fifo_read(&s_xyz[3U * (size_t)me->stored], (size_t)n);
        if ( KODE_OK == rc )
        {
            me->stored += n;
            me->left   -= n;
        }
    }
    if ( (KODE_OK == rc) && (0U < me->left) )
    {
        /* A timer, not a self-post, which would keep the queue busy and starve every task below. */
        wait_start(me, CAP_CHUNK_MS);
        return;
    }

    (void)memset(&w, 0, sizeof(w));
    if ( KODE_OK == rc )
    {
        window_describe(me->stored);
        rc = kimu_energy(s_xyz, (size_t)me->stored, me->hz,
                         (uint16_t)KDRV_QMI8658_WITNESS_HZ,
                         (uint16_t)KDRV_QMI8658_UG_PER_LSB, &w);
    }
    energy_post(me, me->who, rc, (size_t)me->stored, &w);
    cap_end(me);
}

/**
 * @brief The one-shot fired: a self-test's poll, a window's chunk, the
 *        gyroscope's turn-on, or a window nobody came back for.
 *
 * @param me The AO.
 */
static void imu_tick(Imu * const me)
{
    switch ( me->phase )
    {
        case PHASE_CAP_DRAIN: {
            cap_chunk(me);
            break;
        }
        case PHASE_CAP_ARMED: {
            KLOG_W(TAG, "a witness window was armed and never drained; closed");
            cap_end(me);
            break;
        }
        case PHASE_SETTLE: {
            imu_rest(me);
            break;
        }
        case PHASE_SETTLING: {
            settle_over(me);
            break;
        }
        case PHASE_ST_XL:
        case PHASE_ST_GY: {
            imu_st_poll(me);
            break;
        }
        default: {
            /* a tick with nothing in flight: nothing to do */
            break;
        }
    }
}

/* ---- The magnetometer row ----------------------------------------------------
 * `test component=mag`. Step 1 the identity: `mag=absent` for no ACK,
 * `mag=moved` for a part at KBSP_I2C_ADDR_MAG_STRAY (kdrv_qmc5883p_stray()).
 * No step 2: the datasheet gives no self-test threshold, so the shift is only
 * logged. Step 3 the field, with no verdict. The row runs inside one step.
 */

/**
 * @brief The magnetometer row, whole.
 *
 * @param me The AO.
 */
static void mag_row(Imu * const me)
{
    uint8_t       chip_id = 0U;
    int32_t       mg[3]   = { 0, 0, 0 };
    int32_t       st[3]   = { 0, 0, 0 };
    kode_status_t rc;

    KLOG_N(TAG, "test mag: identity, field at rest");
    rc = kdrv_qmc5883p_create(&chip_id);
    if ( (KODE_IO_ERROR == rc) && kdrv_qmc5883p_stray() )
    {
        me->mag_up = false;
        KLOG_E(TAG, "test mag: the QMC5883P answers at 0x%02X, not 0x%02X: it "
                    "loaded its address wrong at power-up; cut its supply "
                    "(ship_mode, then unplug) and test again",
               (unsigned)KBSP_I2C_ADDR_MAG_STRAY, (unsigned)KBSP_I2C_ADDR_MAG);
        mag_row_publish(me, 1U, (uint8_t)MAG_TEST_WHOAMI,
                        (uint8_t)MAG_TF_MOVED,
                        (int32_t)KBSP_I2C_ADDR_MAG_STRAY, 0, 0, 0);
        mag_row_verdict(me, false, "the QMC5883P answers at another address");
        return;
    }
    if ( KODE_IO_ERROR == rc )
    {
        me->mag_up = false;
        KLOG_E(TAG, "test mag: the chip id did not answer");
        mag_row_publish(me, 1U, (uint8_t)MAG_TEST_WHOAMI,
                        (uint8_t)MAG_TF_ABSENT, 0, 0, 0, 0);
        mag_row_verdict(me, false, "the QMC5883P does not answer at 0x2C");
        return;
    }
    KLOG_N(TAG, "test mag: chip id reads 0x%02X", (unsigned)chip_id);
    if ( KODE_OK != rc )
    {
        me->mag_up = false;
        mag_row_publish(me, 1U, (uint8_t)MAG_TEST_WHOAMI, 0U, (int32_t)chip_id,
                        0, 0, 0);
        mag_row_verdict(me, false, "what answers at 0x2C is not a QMC5883P");
        return;
    }
    me->mag_up = true;
    mag_row_publish(me, 1U, (uint8_t)MAG_TEST_WHOAMI, (uint8_t)MAG_TF_PASS,
                    (int32_t)chip_id, 0, 0, 0);

    /* Started here whatever this AO believed: a create() that had to
     * configure the part afresh has left it suspended. */
    me->mag.awake = false;
    if ( !mag_wake(me) )
    {
        mag_row_verdict(me, false, "the part identified itself and then "
                                   "would not start converting");
        return;
    }

    if ( kdrv_qmc5883p_self_test(st) == KODE_OK )
    {
        KLOG_N(TAG, "test mag: self-test shift %d %d %d mG (a reading, not "
                    "judged)", (int)st[0], (int)st[1], (int)st[2]);
    }
    else
    {
        KLOG_W(TAG, "test mag: the self-test could not be run");
    }

    if ( kdrv_qmc5883p_read(mg) != KODE_OK )
    {
        mag_row_verdict(me, false, "the part identified itself and then gave "
                                   "no conversion");
        return;
    }
    {
        int32_t const babs = magnitude(mg);

        KLOG_N(TAG, "test mag: at rest %d %d %d mG, |B| %d mG, on the chip's "
                    "own axes", (int)mg[0], (int)mg[1], (int)mg[2], (int)babs);
        mag_row_publish(me, 3U, (uint8_t)MAG_TEST_FIELD, 0U, mg[0], mg[1],
                        mg[2], babs);
    }
    mag_row_verdict(me, true, NULL);
}

/* ---- The state machine ------------------------------------------------------ */

/** @brief Initial transition: claim both parts, bring them up, and serve. */
static QState Imu_initial(Imu * const me, void const * const par)
{
    (void)par;

    kaos_claim(KBSP_RES_IMU, me, "AO_Imu");

    /* No subscription for its setting, which the console posts. This AO
     * publishes and answers; it listens to no other owner, because the parts
     * are reached only through it. */
    cal_load(me);
#if CONFIG_KODE_LOWPOWER
    /* The rounds only: the parts wake for whoever asks, so there is nothing
     * to restore when the board does. */
    me->lp_who = lowpower_join(&me->super, "imu");
    if ( KODE_LOWPOWER_WHO_NONE != me->lp_who )
    {
        QActive_subscribe(&me->super, LOWPOWER_ROUND_SIG);
    }
#endif

    me->phase  = PHASE_NONE;
    me->who    = NULL;
    me->imu_up = (kdrv_qmi8658_create(NULL) == KODE_OK);
    me->mag_up = (kdrv_qmc5883p_create(NULL) == KODE_OK);
    if ( !me->imu_up )
    {
        KLOG_W(TAG, "no QMI8658A at 0x6A; motion reads are refused until its "
                    "row finds one");
    }
    if ( !me->mag_up )
    {
        KLOG_W(TAG, "no QMC5883P at 0x2C; field reads are refused until its "
                    "row finds one");
    }
    if ( me->imu_up && me->mag_up )
    {
        KLOG_I(TAG, "up; +-8 g and +-512 dps at 896.8 Hz, the field at 50 Hz, "
                    "both resting until something asks");
    }
    return Q_TRAN(&Imu_up);
}

/**
 * @brief Serving: the rows, the witness window, and a sample on request.
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState Imu_up(Imu * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case IMU_READ_REQ_SIG: {
            ImuReadEvt const * const q = Q_EVT_CAST(ImuReadEvt);
            kdrv_qmi8658_sample_t     s;
            kode_status_t             rc = KODE_IO_ERROR;
            bool                      hold = false;

            /* @c out is left exactly as the caller gave it on every answer
             * but a sample: a struct of zeroes is a reading, and this is
             * the absence of one. */
            (void)memset(&s, 0, sizeof(s));
            if ( !me->imu_up )
            {
                rc = KODE_IO_ERROR;
            }
            else if ( (PHASE_SETTLING == me->phase)
                     && (PENDING_READ == me->pending)
                     && !gen_current(me->held.gen, me->held.cur) )
            {
                /* The read being held has nobody waiting on it any more,
                 * and this one has: it takes the place, and the wait that
                 * is already running. */
                me->held = *q;
                hold     = true;
            }
            else if ( PHASE_NONE != me->phase )
            {
                /* A row or a window has the rates and the scales; a sample
                 * now would be at a scale the caller does not know. And one
                 * read is held at a time. */
                rc = KODE_DENIED;
            }
            else if ( !imu_wake(me) )
            {
                rc = KODE_IO_ERROR;
            }
            else
            {
                uint32_t const left = settle_left_ms(me, GYRO_READY_MS);

                if ( 0U < left )
                {
                    /* Answered once the rotation means something: the
                     * caller waits on its semaphore, and the facade's wait
                     * outlasts a wake. */
                    me->held = *q;
                    hold     = true;
                    settle_hold(me, PENDING_READ, NULL, left);
                }
                else
                {
                    rc = kdrv_qmi8658_read(&s);
                }
            }
            if ( !hold )
            {
                read_finish(q, rc, &s);
            }
            status = Q_HANDLED();
            break;
        }
        case MAG_READ_REQ_SIG: {
            MagReadEvt const * const q = Q_EVT_CAST(MagReadEvt);
            int32_t                  mg[3] = { 0, 0, 0 };
            kode_status_t            rc = KODE_IO_ERROR;

            if ( mag_wake(me) )
            {
                rc = kdrv_qmc5883p_read(mg);
            }
            mag_read_finish(q, rc, mg);
            status = Q_HANDLED();
            break;
        }
        case MAG_CAL_REQ_SIG: {
            cal_request(me, Q_EVT_CAST(MagCalEvt));
            status = Q_HANDLED();
            break;
        }
        case CONFIG_REQ_SIG: {
            cal_config(me, Q_EVT_CAST(KodeConfigReqEvt));
            status = Q_HANDLED();
            break;
        }
        case IMU_TEST_REQ_SIG: {
            if ( PHASE_NONE != me->phase )
            {
                /* A witness window or the row already holds the part; the
                 * console runs one row at a time, so this is answered rather
                 * than expected. */
                test_refused(me, "imu");
            }
            else if ( !me->imu.awake )
            {
                /* Resting, the part is where a self-test starts from: both
                 * sensors disabled, and nothing turning on. Waking it first
                 * would put the trigger at the very end of the gyroscope's
                 * turn-on, which is the one place the part has been seen
                 * to ignore it. The row's own end leaves the part running. */
                imu_row(me);
            }
            else
            {
                /* Awake, the part is put to rest first and given
                 * ROW_REST_MS to get there: a self-test triggered in the
                 * same breath as the write that disables the sensors is
                 * one the part sometimes never acts on, however long the
                 * gyroscope had been running before it. */
                if ( kdrv_qmi8658_standby(true) == KODE_OK )
                {
                    me->imu.awake = false;
                }
                settle_hold(me, PENDING_ROW, NULL, ROW_REST_MS);
            }
            status = Q_HANDLED();
            break;
        }
        case IMU_CAPTURE_REQ_SIG: {
            ImuCaptureReqEvt const * const q = Q_EVT_CAST(ImuCaptureReqEvt);

            if ( PHASE_NONE != me->phase )
            {
                armed_post(me, q->who, KODE_DENIED);
            }
            else
            {
                uint32_t left;

                /* A part fitted since boot is found here rather than in
                 * cap_arm(), so that it too is woken and settled first. */
                if ( !me->imu_up )
                {
                    me->imu_up = (kdrv_qmi8658_create(NULL) == KODE_OK);
                }
                (void)imu_wake(me);
                left = settle_left_ms(me, SETTLE_MS);
                if ( 0U < left )
                {
                    /* Answered once the part will act on the arm: the
                     * requester waits on its IMU_ARMED_SIG, and a reply a
                     * fraction of a second late beats a window that never
                     * fills. */
                    settle_hold(me, PENDING_CAP, q->who, left);
                }
                else
                {
                    cap_arm(me, q->who);
                }
            }
            status = Q_HANDLED();
            break;
        }
        case IMU_DRAIN_REQ_SIG: {
            cap_drain(me, Q_EVT_CAST(ImuDrainReqEvt));
            status = Q_HANDLED();
            break;
        }
        case IMU_WAIT_SIG: {
            /* The stale count first, as everywhere this one-shot is used:
             * an expiry queued before a wait_stop() is not this phase's. */
            if ( 0U < me->wait_stale )
            {
                me->wait_stale -= 1U;
            }
            else
            {
                me->wait_live = false;
                imu_tick(me);
            }
            status = Q_HANDLED();
            break;
        }
        case IMU_REST_SIG: {
            part_rest(me, &me->imu, true);
            status = Q_HANDLED();
            break;
        }
        case MAG_REST_SIG: {
            part_rest(me, &me->mag, false);
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODE_LOWPOWER
        case LOWPOWER_ROUND_SIG: {
            imu_round(me, Q_EVT_CAST(LowPowerRoundEvt));
            status = Q_HANDLED();
            break;
        }
#endif
        case MAG_TEST_REQ_SIG: {
            /* The magnetometer is its own part and its row holds the IMU
             * for nothing, but the console runs one row at a time and the
             * self-test's 10 ms sits inside this step: answered rather than
             * run under a witness window somebody is timing. */
            if ( PHASE_NONE != me->phase )
            {
                test_refused(me, "mag");
            }
            else
            {
                mag_row(me);
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

void Imu_ctor(void)
{
    Imu * const me = &l_imu;

    QActive_ctor(&me->super, Q_STATE_CAST(&Imu_initial));
    QTimeEvt_ctorX(&me->wait, &me->super, IMU_WAIT_SIG, 0U);
    QTimeEvt_ctorX(&me->imu.te, &me->super, IMU_REST_SIG, 0U);
    QTimeEvt_ctorX(&me->mag.te, &me->super, MAG_REST_SIG, 0U);

    me->wait_stale = 0U;
    me->wait_live  = false;
    me->imu_up     = false;
    me->mag_up     = false;
    me->imu.stale  = 0U;
    me->imu.live   = false;
    me->imu.awake  = false;
    me->mag.stale  = 0U;
    me->mag.live   = false;
    me->mag.awake  = false;
    (void)memset(&me->held, 0, sizeof(me->held));
    me->phase      = PHASE_NONE;
    me->pending    = PENDING_NONE;
    me->gyro_on_ms = 0U;
    me->polls      = 0U;
    me->left       = 0U;
    me->stored     = 0U;
    me->who        = NULL;
    me->hz         = 0U;
#if CONFIG_KODE_LOWPOWER
    me->lp_who     = KODE_LOWPOWER_WHO_NONE;
#endif
}

AO_STORAGE(imu, KQP_AO_QUEUE_LEN, KQP_AO_STACK_MEDIUM);   /* a float Goertzel runs in its frames */

#if CONFIG_KODEOS_AUTOTEST
/* ---- The self-test rows: registered by kaos_imu_register() ---------------- */

/**
 * @brief `test component=imu`: the IMU, judged by itself.
 *
 * Three steps: the part names itself, each sensor passes the manufacturer's
 * own self-test with the chip's own stimulus, and a board at rest reads one
 * g, no rotation and a plausible die temperature. Every reading goes out as
 * a log line before the verdict.
 *
 * @return false: AO_Imu answers later, with TEST_RESULT_SIG.
 */
static bool imu_row_imu(void)
{
    imu_test_request();
    return false;
}

/**
 * @brief `test component=mag`: the magnetometer, judged by itself.
 *
 * Three steps and nobody's hands: the part names itself through the IMU's
 * hub, passes ST's self-test with its own stimulus, and has the field at
 * rest put in the log as a reading with no verdict. Every reading goes out
 * as a log line before the verdict.
 *
 * @return false: AO_Imu answers later, with TEST_RESULT_SIG.
 */
static bool imu_row_mag(void)
{
    mag_test_request();
    return false;
}
#endif

void kaos_imu_register(void)
{
#if CONFIG_KODEOS_AUTOTEST
    static const krow_t IMU_ROWS[] = {
        { "imu", AUTOTEST_ID_IMU, imu_row_imu, KROW_ASYNC, AUTOTEST_G_MAIN, KODE_IMU_TEST_BUDGET_MS,
          NULL, 0U, "Motion sensor",
          "Sensor not right",
          AUTOTEST_ORDER_IMU },
        { "mag", AUTOTEST_ID_MAG, imu_row_mag, KROW_ASYNC, AUTOTEST_G_MAIN, KODE_MAG_TEST_BUDGET_MS,
          NULL, 0U, "Compass",
          "Compass not right",
          AUTOTEST_ORDER_MAG },
    };

    kaos_require(krows_add(IMU_ROWS, sizeof(IMU_ROWS) / sizeof(IMU_ROWS[0])),
                 "imu rows");
#endif
}

void kaos_imu_start(void)
{
    Imu_ctor();
    kaos_imu_register();
    AO_START(AO_Imu, imu, KODE_PRIO_IMU);
#if CONFIG_KODEOS_CONSOLE
    /* The settings this AO answers, registered once it has the queue their
     * requests are posted to: an app that does not link it has no such keys. */
    static const kode_config_t IMU_CONFIG[] = {
        { .name = KSET_KEY_MAG_CAL, .type = KODE_TYPE_STRING },
    };

    kaos_require(KODE_CONFIG_ADD(AO_Imu, KQP_IMU_CMD_MARGIN, IMU_CONFIG), "imu settings");
#endif
}
