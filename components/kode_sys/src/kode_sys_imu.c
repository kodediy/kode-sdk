/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_imu.c
 * @brief Motion through AO_Imu: the IMU sample, the magnetometer field and the calibration.
 */
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kode_sys_priv.h"

#include "kaos.h"
#include "khal_sync.h"
#include "kdrv_qmc5883p.h"

#include <stddef.h>
#include <string.h>

_Static_assert(KODE_SYS_MAG_AXES == KDRV_QMC5883P_AXES,
               "the facade's field has the part's axes");

/* ---- The answers' payloads against the drivers' types they carry ---- */
_Static_assert(KODE_SYS_IMU_AXES == IMU_AXES, "the facade's IMU axes are the answer's");
_Static_assert(KODE_SYS_MAG_AXES == MAG_AXES, "the facade's field axes are the answer's");
_Static_assert(sizeof(kode_sys_imu_sample_t) == sizeof(kdrv_qmi8658_sample_t), "the sample is the driver's");
_Static_assert(offsetof(kode_sys_imu_sample_t, w_mdps) == offsetof(kdrv_qmi8658_sample_t, w_mdps),
               "the sample's rotation where the driver's is");
_Static_assert(offsetof(kode_sys_imu_sample_t, temp_dc) == offsetof(kdrv_qmi8658_sample_t, temp_dc),
               "the sample's temperature where the driver's is");
_Static_assert(sizeof(kode_sys_mag_corr_t) == sizeof(kdrv_qmc5883p_cal_t), "the calibration is the driver's");
_Static_assert(offsetof(kode_sys_mag_corr_t, scale_milli) == offsetof(kdrv_qmc5883p_cal_t, scale_milli),
               "the calibration's scales where the driver's are");
_Static_assert(KODE_SYS_MAG_CAL_GET == MAG_CAL_OP_GET, "a get is AO_Imu's get");
_Static_assert(KODE_SYS_MAG_CAL_SET == MAG_CAL_OP_SET, "a set is AO_Imu's set");
_Static_assert(KODE_SYS_MAG_CAL_CLEAR == MAG_CAL_OP_CLEAR, "a clear is AO_Imu's clear");

/* ---- Motion state: one claim shared by the IMU and the magnetometer read ---- */
static khal_mutex_t s_imu_lock;               /**< Guards s_imu_busy and s_imu_gen.   */
static khal_sem_t   s_imu_sem;                /**< Both motion reads sleep here.      */
static bool         s_imu_busy;               /**< A motion read holds the claim.     */
static uint32_t     s_imu_gen;                /**< Never 0; the tag being waited on, read by AO_Imu too. */
static uint32_t     s_imu_ack;                /**< Whose IMU answer that is.          */
static uint32_t     s_imu_rc;                 /**< The IMU read's kode_status_t.      */
static kdrv_qmi8658_sample_t s_imu_sample;    /**< What AO_Imu writes a sample into.  */
static int32_t      s_mag_sample[KODE_SYS_MAG_AXES];   /**< What AO_Imu writes a field into.  */
static uint32_t     s_mag_ack;                /**< Whose mag answer that is.          */
static uint32_t     s_mag_rc;                 /**< The mag read's kode_status_t.      */
static MagCalSlot   s_cal_slot;               /**< What AO_Imu answers a calibration get into. */
static uint32_t     s_cal_ack;                /**< Whose calibration answer that is.  */
static uint32_t     s_cal_rc;                 /**< The calibration call's kode_status_t. */

void kode_sys_imu_init(void)
{
    s_imu_sem          = khal_sem_create();
    s_imu_lock         = khal_mutex_create();
    s_imu_busy         = false;
    s_imu_gen          = 0U;
    s_imu_ack          = 0U;
    s_imu_rc           = (uint32_t)KODE_OK;
    s_mag_ack          = 0U;
    s_mag_rc           = (uint32_t)KODE_OK;
    s_cal_ack          = 0U;
    s_cal_rc           = (uint32_t)KODE_OK;
    s_cal_slot.present = false;
}

/* ---- The motion reads: one claim, one request at a time in AO_Imu ---- */

/**
 * @brief Take the IMU claim and draw a generation for this call.
 *
 * The generation is published with a release because AO_Imu reads it too: a
 * request whose tag no longer matches has nobody waiting and is discarded.
 * @param[out] gen This call's own tag.
 * @return true when this caller took it, and must release it again.
 */
static bool imu_claim(uint32_t *gen)
{
    bool took = false;

    khal_mutex_lock(s_imu_lock);
    if ( !s_imu_busy )
    {
        uint32_t next = s_imu_gen + 1U;

        if ( 0U == next )
        {
            next = 1U;
        }
        s_imu_busy = true;
        took       = true;
        __atomic_store_n(&s_imu_gen, next, __ATOMIC_RELEASE);
        *gen = next;
    }
    khal_mutex_unlock(s_imu_lock);
    return took;
}

/** @brief Give the IMU claim back. */
static void imu_release(void)
{
    khal_mutex_lock(s_imu_lock);
    s_imu_busy = false;
    khal_mutex_unlock(s_imu_lock);
}

kode_status_t kode_block_imu_read_sample(kdrv_qmi8658_sample_t *out)
{
    kode_status_t rc  = KODE_TIMEOUT;
    uint32_t      gen = 0U;

    if ( NULL == out )
    {
        return KODE_DENIED;
    }
    /* Before anything is posted, so a refused caller leaves no request behind. */
    if ( !imu_claim(&gen) )
    {
        return KODE_DENIED;
    }
    /* AO_Imu fills this file's sample, not the caller's: the wait is bounded and can end first.
     * s_imu_gen travels too, so AO_Imu writes nothing for a request that is no longer the one waited on.
     * The status is separate: an absent IMU answers at once having written nothing. */
    imu_read_request((void *)&s_imu_sample, &s_imu_rc, s_imu_sem, gen,
                     &s_imu_gen, &s_imu_ack);
    if ( kode_sys_answer_await(s_imu_sem, &s_imu_ack, gen, KODE_SYS_IMU_READ_WAIT_MS) )
    {
        rc = (kode_status_t)s_imu_rc;
        if ( KODE_OK == rc )
        {
            *out = s_imu_sample;
        }
    }
    imu_release();
    return rc;
}

kode_status_t kode_block_mag_read_raw(int32_t mg[KODE_SYS_MAG_AXES])
{
    kode_status_t rc  = KODE_TIMEOUT;
    uint32_t      gen = 0U;

    if ( NULL == mg )
    {
        return KODE_DENIED;
    }
    /* The IMU read's claim: AO_Imu holds one request at a time, whichever door it came through. */
    if ( !imu_claim(&gen) )
    {
        return KODE_DENIED;
    }
    /* The semaphore is the IMU read's too, filtered on this read's own ack and status. */
    if ( !mag_read_request((void *)&s_mag_sample, &s_mag_rc, s_imu_sem, gen,
                          &s_imu_gen, &s_mag_ack) )
    {
        /* AO_Imu's queue is full: nothing posted, so the claim goes back and the caller asks again. */
        imu_release();
        return KODE_DENIED;
    }
    if ( kode_sys_answer_await(s_imu_sem, &s_mag_ack, gen, KODE_SYS_MAG_READ_WAIT_MS) )
    {
        rc = (kode_status_t)s_mag_rc;
        if ( KODE_OK == rc )
        {
            (void)memcpy(mg, s_mag_sample, sizeof(s_mag_sample));
        }
    }
    imu_release();
    return rc;
}

/**
 * @brief One calibration request, waited for, through kode_block_mag_read_raw()'s claim and storage rules.
 * @param op   MAG_CAL_OP_*.
 * @param corr SET: the calibration.
 * @param slot GET: where the answer is copied; may be NULL.
 * @return As kode_block_mag_cal_get().
 */
static kode_status_t mag_cal_wait(uint8_t op, const kdrv_qmc5883p_cal_t *corr, MagCalSlot *slot)
{
    kode_status_t rc  = KODE_TIMEOUT;
    uint32_t      gen = 0U;

    if ( !imu_claim(&gen) )
    {
        return KODE_DENIED;
    }
    if ( !mag_cal_request(op, corr, &s_cal_slot, &s_cal_rc, s_imu_sem, gen, &s_imu_gen,
                         &s_cal_ack) )
    {
        imu_release();
        return KODE_DENIED;
    }
    if ( kode_sys_answer_await(s_imu_sem, &s_cal_ack, gen, KODE_SYS_MAG_CAL_WAIT_MS) )
    {
        rc = (kode_status_t)s_cal_rc;
        if ( (KODE_OK == rc) && (NULL != slot) )
        {
            *slot = s_cal_slot;
        }
    }
    imu_release();
    return rc;
}

kode_status_t kode_block_mag_cal_get(kdrv_qmc5883p_cal_t *corr, bool *present)
{
    MagCalSlot    slot;
    kode_status_t rc = KODE_DENIED;

    if ( (NULL != corr) && (NULL != present) )
    {
        rc = mag_cal_wait(MAG_CAL_OP_GET, NULL, &slot);
        if ( KODE_OK == rc )
        {
            *present = slot.present;
            if ( slot.present )
            {
                *corr = slot.corr;
            }
        }
    }
    return rc;
}

kode_status_t kode_block_mag_cal_set(const kdrv_qmc5883p_cal_t *corr)
{
    kode_status_t rc = KODE_DENIED;

    if ( NULL != corr )
    {
        rc = mag_cal_wait(MAG_CAL_OP_SET, corr, NULL);
    }
    return rc;
}

kode_status_t kode_block_mag_cal_clear(void)
{
    return mag_cal_wait(MAG_CAL_OP_CLEAR, NULL, NULL);
}

/* ---- kode_sys's requests: AO_Imu answers the asking AO ---- */

kode_status_t kode_sys_imu_read(QActive *me)
{
    return imu_read_ask(me);
}

kode_status_t kode_sys_mag_read(QActive *me)
{
    return mag_read_ask(me);
}

kode_status_t kode_sys_mag_cal_get(QActive *me)
{
    return mag_cal_ask(me, MAG_CAL_OP_GET, NULL);
}

kode_status_t kode_sys_mag_cal_set(QActive *me, const kode_sys_mag_corr_t *corr)
{
    kdrv_qmc5883p_cal_t part;

    if ( NULL == corr )
    {
        return KODE_DENIED;
    }
    (void)memcpy(part.offset_mg, corr->offset_mg, sizeof(part.offset_mg));
    (void)memcpy(part.scale_milli, corr->scale_milli, sizeof(part.scale_milli));
    return mag_cal_ask(me, MAG_CAL_OP_SET, &part);
}

kode_status_t kode_sys_mag_cal_clear(QActive *me)
{
    return mag_cal_ask(me, MAG_CAL_OP_CLEAR, NULL);
}
