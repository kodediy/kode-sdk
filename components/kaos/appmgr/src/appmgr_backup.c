/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file appmgr_backup.c
 * @brief Which installed apps have a copy on the card.
 */
#include "appmgr_backup.h"

#include <string.h>

/**
 * @brief Where the table holds @p seq.
 * @param t   The table.
 * @param seq The app's install number.
 * @return Its place; t->count when the table does not hold it.
 */
static uint32_t backup_find(const appmgr_backup_t *t, uint32_t seq)
{
    uint32_t at = t->count;

    for ( uint32_t i = 0U; i < t->count; ++i )
    {
        if ( t->seq[i] == seq )
        {
            at = i;
            break;
        }
    }
    return at;
}

void appmgr_backup_reset(appmgr_backup_t *t)
{
    t->count = 0U;
}

appmgr_backup_state_t appmgr_backup_get(const appmgr_backup_t *t, uint32_t seq)
{
    appmgr_backup_state_t state = APPMGR_BACKUP_UNSEEN;
    uint32_t const        at    = backup_find(t, seq);

    if ( at < t->count )
    {
        state = (appmgr_backup_state_t)t->state[at];
    }
    return state;
}

void appmgr_backup_set(appmgr_backup_t *t, uint32_t seq, appmgr_backup_state_t state)
{
    uint32_t const at = backup_find(t, seq);

    if ( APPMGR_BACKUP_UNSEEN == state )
    {
        appmgr_backup_forget(t, seq);
    }
    else if ( at < t->count )
    {
        t->state[at] = (uint8_t)state;
    }
    else if ( KAPPS_MAX_ENTRIES > t->count )
    {
        t->seq[t->count]   = seq;
        t->state[t->count] = (uint8_t)state;
        t->count += 1U;
    }
    else
    {
        /* Full: the app stays unseen, and the card is asked about it again. */
    }
}

void appmgr_backup_forget(appmgr_backup_t *t, uint32_t seq)
{
    uint32_t const at = backup_find(t, seq);

    if ( at < t->count )
    {
        /* The last one takes its place: the table keeps no order. */
        t->count -= 1U;
        t->seq[at]   = t->seq[t->count];
        t->state[at] = t->state[t->count];
    }
}

bool appmgr_backup_same(uint32_t file_len, const uint8_t *file_build,
                        uint32_t image_len, const uint8_t *image_build)
{
    return (file_len == image_len)
        && (memcmp(file_build, image_build, KAPP_BUILD_LEN) == 0);
}
