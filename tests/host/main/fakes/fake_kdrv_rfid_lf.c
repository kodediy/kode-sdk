/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_kdrv_rfid_lf.c
 * @brief The LF front end, without hardware.
 */
#include <string.h>

#include "fake_kdrv_rfid_lf.h"
#include "kdrv_rfid_lf.h"

static struct {
    bool            (*is_worker)(void); /**< The AO's worker check, if set.  */
    int               win_rc;      /**< What the next READ decodes.          */
    uint8_t           win_id[5];
    uint8_t           win_rf;
    int               read_calls;
    int               clone_calls;
    uint8_t           clone_wrote[5]; /**< The ID the last CLONE was asked to write. */
    bool              clone_differs;  /**< Whether its read-back is another ID.   */
    bool              last_boost;  /**< READ = false, CLONE = true.          */
    int               cap_rc;      /**< What the next CAPTURE answers.       */
    uint16_t          cap_win[KRFID_EM4100_MAX_SAMPLES]; /**< Its window.    */
    size_t            cap_n;       /**< Samples cap_win holds.               */
    int               capture_calls;
    int               field_off_calls;
    int               rest_calls;
    int               cert_rc;      /**< What the certification carrier answers. */
    uint32_t          emu_rc;       /**< What an EMULATE answers when idle.      */
    uint32_t          emu_calls;
    uint32_t          emu_stop_calls;
    bool              emu_on;       /**< The faked loop is running.              */
    uint8_t           emu_id[5];    /**< The ID the last EMULATE carried.        */
} s_f;

void fake_rfid_reset(void)
{
    (void)memset(&s_f, 0, sizeof(s_f));
    /* A flat window until a case stages a decode. */
    s_f.win_rc = (int)KODE_TIMEOUT;
    /* An ADC that delivers nothing until a case stages a window. */
    s_f.cap_rc = (int)KODE_ERROR;
    /* A carrier that comes up, until a case says otherwise. */
    s_f.cert_rc = (int)KODE_OK;
    /* A loop that starts, until a case says otherwise. */
    s_f.emu_rc = (uint32_t)KODE_OK;
}

void fake_rfid_set_emulate_rc(uint32_t rc)
{
    s_f.emu_rc = rc;
}

uint32_t fake_rfid_emulate_calls(void)
{
    return s_f.emu_calls;
}

uint32_t fake_rfid_emu_stop_calls(void)
{
    return s_f.emu_stop_calls;
}

bool fake_rfid_emulating(void)
{
    return s_f.emu_on;
}

void fake_rfid_last_emulate_id(uint8_t out[5])
{
    (void)memcpy(out, s_f.emu_id, sizeof(s_f.emu_id));
}

void fake_rfid_set_cert_rc(int rc)
{
    s_f.cert_rc = rc;
}

void fake_rfid_set_window(int rc, const uint8_t id[5], uint8_t rf_div)
{
    s_f.win_rc = rc;
    (void)memset(s_f.win_id, 0, sizeof(s_f.win_id));
    if ( NULL != id )
    {
        (void)memcpy(s_f.win_id, id, sizeof(s_f.win_id));
    }
    s_f.win_rf = rf_div;
}

int fake_rfid_read_calls(void)
{
    return s_f.read_calls;
}

int fake_rfid_clone_calls(void)
{
    return s_f.clone_calls;
}

void fake_rfid_set_clone_mismatch(bool mismatch)
{
    s_f.clone_differs = mismatch;
}

void fake_rfid_last_clone_id(uint8_t out[5])
{
    (void)memcpy(out, s_f.clone_wrote, sizeof(s_f.clone_wrote));
}

bool fake_rfid_last_boost(void)
{
    return s_f.last_boost;
}

void fake_rfid_set_capture(int rc, const uint16_t *samples, size_t n)
{
    size_t keep = n;

    if ( NULL == samples )
    {
        keep = 0U;
    }
    if ( (size_t)KRFID_EM4100_MAX_SAMPLES < keep )
    {
        keep = (size_t)KRFID_EM4100_MAX_SAMPLES;
    }
    s_f.cap_rc = rc;
    s_f.cap_n  = keep;
    if ( 0U < keep )
    {
        (void)memcpy(s_f.cap_win, samples, keep * sizeof(s_f.cap_win[0]));
    }
}

int fake_rfid_capture_calls(void)
{
    return s_f.capture_calls;
}

int fake_rfid_field_off_calls(void)
{
    return s_f.field_off_calls;
}

/* ---- The driver's header, faked whole ------------------------------------- */

kode_status_t kdrv_rfid_lf_init(void)
{
    return KODE_OK;
}

kode_status_t kdrv_rfid_lf_field_on(bool boost)
{
    (void)boost;
    return KODE_OK;
}

kode_status_t kdrv_rfid_lf_field_off(void)
{
    s_f.field_off_calls += 1;
    return KODE_OK;
}

kode_status_t kdrv_rfid_lf_rest(void)
{
    s_f.rest_calls += 1;
    return KODE_OK;
}

int fake_rfid_rest_calls(void)
{
    return s_f.rest_calls;
}

kode_status_t kdrv_rfid_lf_read_window(uint8_t id[5], krfid_em4100_diag_t *d)
{
    if ( NULL != id )
    {
        (void)memset(id, 0, 5U);
    }
    if ( NULL != d )
    {
        (void)memset(d, 0, sizeof(*d));
    }
    return KODE_TIMEOUT;
}

kode_status_t kdrv_rfid_lf_clone(const uint8_t id[5])
{
    (void)id;
    return KODE_OK;
}

/* The certification carrier comes up at once, or refuses with what a case
 * staged through fake_rfid_set_cert_rc(). */
kode_status_t kdrv_rfid_lf_cert_carrier(bool modulated)
{
    (void)modulated;
    return (kode_status_t)s_f.cert_rc;
}

void kdrv_rfid_lf_set_worker_check(bool (*is_worker)(void))
{
    s_f.is_worker = is_worker;
}

/* Writes through the pointers only on KODE_OK, as the driver does, and skips
 * a NULL one the way the driver does: the statistics; the waveform whenever
 * one is asked for, with or without a count to report it in; and the count,
 * 0 with no waveform. */
kode_status_t kdrv_rfid_lf_field_capture(uint32_t ms, krfid_field_t *out,
                                         uint16_t *wave, size_t cap, size_t *n)
{
    if ( (NULL == s_f.is_worker) || !s_f.is_worker() )
    {
        return KODE_DENIED;
    }
    if ( 0U == ms )
    {
        return KODE_ERROR;
    }
    s_f.capture_calls += 1;
    if ( (int)KODE_OK == s_f.cap_rc )
    {
        size_t points = 0U;

        if ( NULL != out )
        {
            krfid_wave_stats(s_f.cap_win, s_f.cap_n, out);
        }
        if ( NULL != wave )
        {
            points = krfid_wave_decimate(s_f.cap_win, s_f.cap_n, wave, cap);
        }
        if ( NULL != n )
        {
            *n = points;
        }
    }
    return (kode_status_t)s_f.cap_rc;
}

kode_status_t kdrv_rfid_lf_run(kdrv_rfid_lf_job_t const *job, uint8_t id[5],
                               uint8_t *rf_div)
{
    if ( (NULL == job) || (NULL == id) || (NULL == rf_div) )
    {
        return KODE_DENIED;
    }
    (void)memset(id, 0, 5U);
    *rf_div = 0U;

    if ( KDRV_RFID_JOB_EMULATE == job->type )
    {
        /* As the driver: one loop at a time, and a refusal leaves none. */
        s_f.emu_calls += 1;
        (void)memcpy(s_f.emu_id, job->id, sizeof(s_f.emu_id));
        if ( s_f.emu_on )
        {
            return KODE_DENIED;
        }
        if ( (uint32_t)KODE_OK == s_f.emu_rc )
        {
            s_f.emu_on = true;
        }
        return (kode_status_t)s_f.emu_rc;
    }
    if ( KDRV_RFID_JOB_EMU_STOP == job->type )
    {
        /* Harmless with nothing running, as the driver is. */
        s_f.emu_stop_calls += 1;
        s_f.emu_on          = false;
        return KODE_OK;
    }
    if ( KDRV_RFID_JOB_CLONE == job->type )
    {
        s_f.clone_calls += 1;
        s_f.last_boost   = true;
        (void)memcpy(s_f.clone_wrote, job->id, 5U);
        /* The read-back a real clone verifies with: the ID just written, or
         * another one when a case says the tag took something else. */
        (void)memcpy(id, job->id, 5U);
        if ( s_f.clone_differs )
        {
            id[0] = (uint8_t)(id[0] ^ 0xFFU);
        }
        return KODE_OK;
    }
    if ( KDRV_RFID_JOB_CAPTURE == job->type )
    {
        /* A capture decodes nothing: the ID stays empty and the rate 0; the
         * answer is in the job's pointers. */
        return kdrv_rfid_lf_field_capture(job->ms, job->field, job->wave,
                                          job->cap, job->n);
    }
    s_f.read_calls += 1;
    s_f.last_boost  = false;
    (void)memcpy(id, s_f.win_id, 5U);
    *rf_div = s_f.win_rf;
    return (kode_status_t)s_f.win_rc;
}
