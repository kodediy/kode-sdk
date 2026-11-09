/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_station.c
 * @brief The E5 station's camera and pads loop, faked. See fake_station.h.
 */
#include <stddef.h>
#include <string.h>

#include "fake_station.h"
#include "kdrv_dock_loop.h"
#include "kdrv_ov5640.h"

/** @brief The map a found camera answers. */
#define FAKE_STATION_MAP  0U

/** @brief Pixels across the largest frame a grab gives: the camera's window (TD_WIN_W). */
#define FAKE_STATION_FRAME_W  1056U
/** @brief Its rows (TD_WIN_H). */
#define FAKE_STATION_FRAME_H  768U
/** @brief Its pixels, RGB565. */
#define FAKE_STATION_FRAME_PX ((size_t)FAKE_STATION_FRAME_W * (size_t)FAKE_STATION_FRAME_H)
/** @brief The one grey every pixel of a frame holds, RGB565: mid grey. */
#define FAKE_STATION_GREY     0x8410U

/** @brief The frame every grab gives while frames are on. */
static uint16_t s_frame[FAKE_STATION_FRAME_PX];

static kode_status_t s_probe_rc = KODE_OK;  /**< What the probe answers.  */
static kode_status_t s_loop_rc  = KODE_OK;  /**< What the loop answers.   */
static unsigned      s_probes;              /**< Probes since the reset.  */
static unsigned      s_closes;              /**< Closes since the reset.  */
static unsigned      s_opens;               /**< Opens since the reset.   */
static unsigned      s_loops;               /**< Loops since the reset.   */
static bool          s_frames;              /**< Grabs give s_frame.      */

void fake_station_reset(void)
{
    s_probe_rc = KODE_OK;
    s_loop_rc  = KODE_OK;
    s_probes   = 0U;
    s_closes   = 0U;
    s_opens    = 0U;
    s_loops    = 0U;
    s_frames   = false;
}

void fake_station_frames(bool on)
{
    s_frames = on;
    if ( on )
    {
        for ( size_t i = 0U; FAKE_STATION_FRAME_PX > i; i++ )
        {
            s_frame[i] = FAKE_STATION_GREY;
        }
    }
}

void fake_station_probe_rc(kode_status_t rc)
{
    s_probe_rc = rc;
}

unsigned fake_station_probes(void)
{
    return s_probes;
}

unsigned fake_station_closes(void)
{
    return s_closes;
}

unsigned fake_station_opens(void)
{
    return s_opens;
}

void fake_station_loop_rc(kode_status_t rc)
{
    s_loop_rc = rc;
}

unsigned fake_station_loops(void)
{
    return s_loops;
}

kode_status_t kdrv_ov5640_probe(uint32_t budget_ms, uint8_t *map)
{
    (void)budget_ms;
    s_probes++;
    if ( NULL != map )
    {
        *map = FAKE_STATION_MAP;
    }
    return s_probe_rc;
}

kode_status_t kdrv_ov5640_open(const kdrv_ov5640_mode_t *mode)
{
    (void)mode;
    s_opens++;
    return KODE_OK;
}

void kdrv_ov5640_close(void)
{
    s_closes++;
}

kode_status_t kdrv_ov5640_grab(uint32_t skip, uint32_t timeout_ms, const uint8_t **frame, size_t *len)
{
    (void)skip;
    (void)timeout_ms;
    if ( NULL != frame )
    {
        *frame = NULL;
        if ( s_frames )
        {
            *frame = (const uint8_t *)(const void *)s_frame;
        }
    }
    if ( NULL != len )
    {
        *len = 0U;
        if ( s_frames )
        {
            *len = sizeof(s_frame);
        }
    }
    if ( s_frames )
    {
        return KODE_OK;
    }
    return KODE_TIMEOUT;
}

void kdrv_ov5640_release(void)
{
}

kode_status_t kdrv_ov5640_apply(const kdrv_ov5640_cfg_t *cfg)
{
    (void)cfg;
    return KODE_OK;
}

void *kdrv_ov5640_bus(void)
{
    return NULL;
}

kode_status_t kdrv_dock_loop_run(uint8_t count, kdrv_dock_loop_t *out)
{
    s_loops++;
    if ( NULL == out )
    {
        return KODE_DENIED;
    }
    (void)memset(out, 0, sizeof(*out));
    if ( KODE_OK == s_loop_rc )
    {
        out->slow = count;
        out->fast = count;
        out->rev  = count;
        out->why  = (uint8_t)KDRV_DOCK_LOOP_OK;
    }
    else if ( KODE_NO_RESOURCES == s_loop_rc )
    {
        out->why = (uint8_t)KDRV_DOCK_LOOP_UART_BUSY;
    }
    else
    {
        out->why = (uint8_t)KDRV_DOCK_LOOP_NO_ECHO;
    }
    return s_loop_rc;
}
