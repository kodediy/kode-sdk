/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_radio_sleep_policy.c
 * @brief The order and the retries of the sleep sentinel's bus handshake.
 */
#include "kdrv_radio_sleep.h"

#include <stddef.h>

/* cppcheck-suppress misra-c2012-8.7 ; a file of its own so the host suite can compile the rules; kdrv_radio_sleep.c calls it */
kradio_down_t kradio_sleep_down(const kradio_sleep_seam_t *seam)
{
    bool settled = false;
    uint16_t knocks = 0U;

    if ( NULL == seam )
    {
        return KRADIO_DOWN_NO_PS;
    }
    /* Before the C5 hears anything: a frame queued from here on rings. */
    if ( !seam->bus_down(true) )
    {
        (void)seam->bus_down(false);
        return KRADIO_DOWN_NO_LINE;
    }
    if ( seam->power_save(true) != 0 )
    {
        (void)seam->bus_down(false);
        return KRADIO_DOWN_NO_PS;
    }
    /* The C5 finishes what it was sending before the bus goes quiet. */
    seam->delay_ms(KRADIO_SLEEP_PS_WAIT_MS);
    for ( uint32_t tries = 0U; KRADIO_SLEEP_SETTLE_TRIES >= tries; tries++ )
    {
        if ( 0U < tries )
        {
            seam->delay_ms(KRADIO_SLEEP_SETTLE_MS);
        }
        if ( seam->sdio_settled() )
        {
            settled = true;
            break;
        }
    }
    if ( !settled )
    {
        /* Nothing was suspended; take the announcement back. */
        (void)kradio_sleep_up(seam, &knocks);
        (void)seam->bus_down(false);
        return KRADIO_DOWN_BUSY;
    }
    seam->sdio_tasks(false);
    return KRADIO_DOWN_OK;
}

/* cppcheck-suppress misra-c2012-8.7 ; a file of its own so the host suite can compile the rules; kdrv_radio_sleep.c calls it */
bool kradio_sleep_up(const kradio_sleep_seam_t *seam, uint16_t *knocks)
{
    uint16_t sent = 0U;
    bool heard = false;

    if ( NULL == seam )
    {
        return false;
    }
    for ( uint32_t tries = 0U; KRADIO_SLEEP_OFF_RETRIES >= tries; tries++ )
    {
        if ( 0U < tries )
        {
            seam->delay_ms(KRADIO_SLEEP_OFF_RETRY_MS);
        }
        sent++;
        if ( seam->power_save(false) == 0 )
        {
            heard = true;
            break;
        }
    }
    if ( NULL != knocks )
    {
        *knocks = sent;
    }
    if ( heard )
    {
        seam->delay_ms(KRADIO_SLEEP_RESUME_WAIT_MS);
        seam->sdio_tasks(true);
    }
    return heard;
}

/* cppcheck-suppress misra-c2012-8.7 ; a file of its own so the host suite can compile the rules; kdrv_radio_sleep.c calls it */
bool kradio_sleep_wake_once(const kradio_sleep_seam_t *seam, uint16_t *knocks)
{
    if ( NULL == seam )
    {
        return false;
    }
    /* Taken before the way up: a connect that began while the host was
     * still asleep is already in the count. */
    uint32_t seen = seam->gap_activity();

    if ( !kradio_sleep_up(seam, knocks) )
    {
        return false;
    }
    seam->delay_ms(KRADIO_SLEEP_AWAKE_MS);
    while ( (seam->gap_activity() != seen) && !seam->cmd_waiting() )
    {
        seen = seam->gap_activity();
        seam->delay_ms(KRADIO_SLEEP_GAP_MS);
    }
    return true;
}

/* cppcheck-suppress misra-c2012-8.7 ; a file of its own so the host suite can compile the rules; kdrv_radio_sleep.c calls it */
kradio_sleep_tx_t kradio_sleep_tx_slot(uint8_t if_type)
{
    kradio_sleep_tx_t slot = KRADIO_SLEEP_TX_OTHER;

    if ( KRADIO_SLEEP_IF_STA == if_type )
    {
        slot = KRADIO_SLEEP_TX_STA;
    }
    else if ( KRADIO_SLEEP_IF_SERIAL == if_type )
    {
        slot = KRADIO_SLEEP_TX_SERIAL;
    }
    else if ( KRADIO_SLEEP_IF_HCI == if_type )
    {
        slot = KRADIO_SLEEP_TX_HCI;
    }
    else
    {
        /* private and control frames are not counted */
    }
    return slot;
}
