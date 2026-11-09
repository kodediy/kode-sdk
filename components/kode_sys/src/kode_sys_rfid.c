/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_rfid.c
 * @brief RFID (125 kHz LF) through AO_Rfid: the requests, and the blocking calls
 *        in the NFC arrangement.
 */
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kode_sys_priv.h"

#include "kaos.h"
#include "khal_sync.h"

#include <string.h>

_Static_assert(KODE_SYS_RFID_ID_BYTES == KQP_RFID_ID_BYTES, "the facade's ID is the events' ID");

/* ---- RFID state: one claim for the reads, the write, the emulation and the certification calls ---- */
static khal_mutex_t s_rfid_lock;         /**< Guards s_rfid_busy and s_rfid_gen. */
khal_sem_t   kode_sys_rfid_sem;           /**< AO_Rfid signals a request's end.  */
static bool         s_rfid_busy;         /**< An RFID call holds the claim.      */
static uint32_t     s_rfid_gen;          /**< Never 0.                           */
static uint8_t      s_rfid_id[KODE_SYS_RFID_ID_BYTES];   /**< A read's decoded ID. */
static uint8_t      s_rfid_rf;           /**< A read's bit-rate divisor.         */
static bool         s_rfid_have;         /**< Whether kode_block_rfid_read() decoded a tag. */
static bool         s_rfid_ok;           /**< Whether kode_block_rfid_write() read its ID back. */
uint32_t     kode_sys_rfid_ack;           /**< Whose answer that is.             */

/** @brief How much longer than @p ms a blocking call waits for AO_Rfid: its deadline rounds up to whole ticks. */
#define KODE_RFID_WAIT_MARGIN_MS  500U

void kode_sys_rfid_init(void)
{
    kode_sys_rfid_sem = khal_sem_create();
    s_rfid_lock       = khal_mutex_create();
    s_rfid_busy       = false;
    s_rfid_gen        = 0U;
}

/**
 * @brief Take the RFID facade and stamp this call's generation.
 * @param[out] gen The generation, never zero.
 * @return true when this caller has it.
 */
bool kode_sys_rfid_claim(uint32_t *gen)
{
    bool took = false;

    khal_mutex_lock(s_rfid_lock);
    if ( !s_rfid_busy )
    {
        s_rfid_busy = true;
        took        = true;
        s_rfid_gen += 1U;
        if ( 0U == s_rfid_gen )
        {
            s_rfid_gen = 1U;
        }
        *gen = s_rfid_gen;
    }
    khal_mutex_unlock(s_rfid_lock);
    return took;
}

/** @brief Give the RFID facade back. */
void kode_sys_rfid_release(void)
{
    khal_mutex_lock(s_rfid_lock);
    s_rfid_busy = false;
    khal_mutex_unlock(s_rfid_lock);
}

/**
 * @brief Wait for this call's own RFID answer, discarding an abandoned one.
 * @param gen This call's generation.
 * @param ms  How long AO_Rfid was asked to keep the field up.
 * @return true when the answer that arrived was this call's.
 */
bool kode_sys_rfid_await(uint32_t gen, uint32_t ms)
{
    return kode_sys_answer_await(kode_sys_rfid_sem, &kode_sys_rfid_ack, gen,
                                 ms + KODE_RFID_WAIT_MARGIN_MS);
}

kode_status_t kode_block_rfid_read(uint8_t id[KODE_SYS_RFID_ID_BYTES], uint8_t *rf_div, uint32_t ms)
{
    uint32_t gen = 0U;

    if ( (NULL == id) || (NULL == rf_div) || (0U == ms) )
    {
        return KODE_ERROR;
    }
    if ( !kode_sys_rfid_claim(&gen) )
    {
        return KODE_DENIED;
    }

    s_rfid_have = false;
    rfid_read_request(s_rfid_id, &s_rfid_rf, &s_rfid_have, kode_sys_rfid_sem, ms, gen,
                      &kode_sys_rfid_ack);

    kode_status_t rc = KODE_TIMEOUT;
    if ( kode_sys_rfid_await(gen, ms) && s_rfid_have )
    {
        (void)memcpy(id, s_rfid_id, sizeof(s_rfid_id));
        *rf_div = s_rfid_rf;
        rc      = KODE_OK;
    }
    kode_sys_rfid_release();
    return rc;
}

kode_status_t kode_block_rfid_write(const uint8_t id[KODE_SYS_RFID_ID_BYTES], uint32_t ms)
{
    uint32_t gen = 0U;

    if ( (NULL == id) || (0U == ms) )
    {
        return KODE_ERROR;
    }
    if ( !kode_sys_rfid_claim(&gen) )
    {
        return KODE_DENIED;
    }

    /* The facade's own copy: a timed-out write may still be queued in AO_Rfid. */
    (void)memcpy(s_rfid_id, id, sizeof(s_rfid_id));
    s_rfid_ok = false;
    rfid_write_request(s_rfid_id, &s_rfid_ok, kode_sys_rfid_sem, ms, gen,
                       &kode_sys_rfid_ack);

    kode_status_t rc = KODE_TIMEOUT;
    if ( kode_sys_rfid_await(gen, ms) && s_rfid_ok )
    {
        rc = KODE_OK;
    }
    kode_sys_rfid_release();
    return rc;
}

/** @brief How long a blocking emulation start or stop waits for AO_Rfid, in ms. */
#define KODE_RFID_EMU_WAIT_MS  1500U

/**
 * @brief Post an emulation start, or with NULL a stop, and wait for its answer.
 * @param id The ID to play, or NULL for a stop.
 * @return AO_Rfid's status; KODE_DENIED while another RFID call waits; KODE_TIMEOUT.
 */
static kode_status_t rfid_emulate_post(const uint8_t *id)
{
    static uint32_t s_rfid_rc;   /* outlives a call that gives up waiting */
    uint32_t        gen = 0U;

    if ( !kode_sys_rfid_claim(&gen) )
    {
        return KODE_DENIED;
    }
    s_rfid_rc = (uint32_t)KODE_TIMEOUT;
    if ( NULL != id )
    {
        rfid_emulate_request(id, &s_rfid_rc, kode_sys_rfid_sem, gen,
                             &kode_sys_rfid_ack);
    }
    else
    {
        rfid_emulate_stop_request(&s_rfid_rc, kode_sys_rfid_sem, gen,
                                  &kode_sys_rfid_ack);
    }

    kode_status_t rc = KODE_TIMEOUT;
    if ( kode_sys_rfid_await(gen, KODE_RFID_EMU_WAIT_MS) )
    {
        rc = (kode_status_t)s_rfid_rc;
    }
    kode_sys_rfid_release();
    return rc;
}

kode_status_t kode_block_rfid_emulate_start(const uint8_t id[KODE_SYS_RFID_ID_BYTES])
{
    if ( NULL == id )
    {
        return KODE_ERROR;
    }
    return rfid_emulate_post(id);
}

kode_status_t kode_block_rfid_emulate_stop(void)
{
    return rfid_emulate_post(NULL);
}

bool kode_sys_rfid_is_emulating(void)
{
    return rfid_is_emulating();
}

/* ---- kode_sys's requests: AO_Rfid answers the asking AO ---- */

kode_status_t kode_sys_rfid_read(QActive *me, uint32_t ms)
{
    if ( 0U == ms )
    {
        return KODE_DENIED;
    }
    return rfid_read_ask(me, ms);
}

kode_status_t kode_sys_rfid_write(QActive *me, const uint8_t id[KODE_SYS_RFID_ID_BYTES],
                                  uint32_t ms)
{
    if ( (NULL == id) || (0U == ms) )
    {
        return KODE_DENIED;
    }
    return rfid_write_ask(me, id, ms);
}

kode_status_t kode_sys_rfid_emulate_start(QActive *me,
                                          const uint8_t id[KODE_SYS_RFID_ID_BYTES])
{
    return rfid_emulate_ask(me, id);
}

kode_status_t kode_sys_rfid_emulate_stop(QActive *me)
{
    return rfid_emulate_stop_ask(me);
}
