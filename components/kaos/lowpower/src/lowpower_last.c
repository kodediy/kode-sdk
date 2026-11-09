/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file lowpower_last.c
 * @brief The summary of the last rest: a fixed ring of gauge samples and the totals of the rest.
 */
#include "lowpower_last.h"

#include <string.h>

#include "khal_nvs.h"
#include "kode_log.h"
#include "kset.h"

#define TAG "kaos.lowpower"

void lowpower_last_begin(lowpower_last_t *l, uint32_t start_s)
{
    (void)memset(l, 0, sizeof(*l));
    l->version   = (uint8_t)KODE_LOWPOWER_LAST_VERSION;
    l->end_cause = (uint8_t)KODE_LOWPOWER_LAST_OPEN;
    l->start_s   = start_s;
}

void lowpower_last_add(lowpower_last_t *l, uint32_t t_s, int16_t ma, uint16_t mv)
{
    lowpower_sample_t * const slot = &l->samples[l->next];

    slot->t_s = t_s;
    slot->ma  = ma;
    slot->mv  = mv;
    l->next = (uint8_t)((l->next + 1U) % KODE_LOWPOWER_LAST_SAMPLES);
    if ( KODE_LOWPOWER_LAST_SAMPLES > l->count )
    {
        l->count += 1U;
    }
}

bool lowpower_last_save(const lowpower_last_t *l)
{
    bool const saved = khal_nvs_set_blob(KSET_NAMESPACE, KSET_KEY_LP_LAST, l,
                                         sizeof(*l)) == KODE_OK;

    if ( !saved )
    {
        KLOG_W(TAG, "the summary of the rest was not stored");
    }
    return saved;
}

bool lowpower_last_read(lowpower_last_t *out)
{
    size_t len = sizeof(*out);
    bool have = false;

    if ( NULL != out )
    {
        have = (khal_nvs_get_blob(KSET_NAMESPACE, KSET_KEY_LP_LAST, out, &len)
                == KODE_OK)
               && (sizeof(*out) == len)
               && ((uint8_t)KODE_LOWPOWER_LAST_VERSION == out->version)
               && (KODE_LOWPOWER_LAST_SAMPLES >= out->count)
               && (KODE_LOWPOWER_LAST_SAMPLES > out->next);
    }
    return have;
}

const lowpower_sample_t *lowpower_last_sample(const lowpower_last_t *l, uint8_t i)
{
    const lowpower_sample_t *sample = NULL;

    if ( i < l->count )
    {
        uint8_t oldest = 0U;

        if ( KODE_LOWPOWER_LAST_SAMPLES == l->count )
        {
            oldest = l->next;
        }
        sample = &l->samples[(oldest + i) % KODE_LOWPOWER_LAST_SAMPLES];
    }
    return sample;
}
