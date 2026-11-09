/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file knfc_field.c
 * @brief The listen window's stats fold. See knfc_field.h.
 */
#include "knfc_field.h"

void knfc_field_reset(knfc_field_acc_t *a)
{
    if ( NULL == a )
    {
        return;
    }
    a->sum  = 0U;
    a->min  = UINT8_MAX;
    a->peak = 0U;
    a->ext  = false;
    a->n    = 0U;
}

void knfc_field_fold(knfc_field_acc_t *a, const uint8_t *amp, size_t n,
                     bool ext)
{
    size_t i;

    if ( NULL == a )
    {
        return;
    }
    if ( ext )
    {
        a->ext = true;
    }
    if ( NULL == amp )
    {
        return;
    }
    for ( i = 0U; i < n; i++ )
    {
        const uint8_t v = amp[i];

        if ( v < a->min )
        {
            a->min = v;
        }
        if ( v > a->peak )
        {
            a->peak = v;
        }
        if ( UINT16_MAX > a->n )
        {
            a->sum += (uint32_t)v;
            a->n = (uint16_t)(a->n + 1U);
        }
    }
}

void knfc_field_close(const knfc_field_acc_t *a, knfc_field_t *out)
{
    if ( NULL == out )
    {
        return;
    }
    out->min  = 0U;
    out->mean = 0U;
    out->peak = 0U;
    out->mod  = 0U;
    out->ext  = false;
    out->n    = 0U;
    if ( NULL == a )
    {
        return;
    }
    out->ext = a->ext;
    if ( 0U == a->n )
    {
        return;
    }
    out->min  = a->min;
    out->peak = a->peak;
    out->mod  = (uint8_t)(a->peak - a->min);
    out->mean = (uint8_t)(a->sum / (uint32_t)a->n);
    out->n    = a->n;
}
