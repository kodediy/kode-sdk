/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_dot_worker.c
 * @brief AO_TestDot's worker jobs: the station's chips, the camera and the pads loop.
 */
#include <stddef.h>
#include <stdint.h>

#include "kdrv_ads1115.h"
#include "kdrv_dock_loop.h"
#include "kdrv_ov5640.h"
#include "kdrv_pcf8574.h"
#include "khal_time.h"
#include "kqp.h"

#include "test_dot_priv.h"

/** @brief The station's two chips: they answer, and the relays go to rest. */
static kode_status_t td_station(void)
{
    kode_status_t rc = KODE_ERROR;

    if ( kdrv_station_present() )
    {
        rc = kdrv_pcf8574_write(KDRV_PCF8574_REST);
    }
    return rc;
}

/** @brief The relays at rest when they were touched, then the camera closed: every path out of a wake ends here. */
static kode_status_t td_close(bool rest)
{
    kode_status_t rc = KODE_OK;

    if ( rest )
    {
        rc = kdrv_pcf8574_write(KDRV_PCF8574_REST);
    }
    kdrv_ov5640_close();
    return rc;
}

kode_status_t td_job_run(void *arg, uint32_t *value)
{
    td_job_t * const j = (td_job_t *)arg;
    uint32_t const t0 = khal_time_ms();
    kode_status_t rc = KODE_DENIED;

    (void)value;
    switch ( j->kind )
    {
        case (uint8_t)TD_JOB_PROBE:
            rc = kdrv_ov5640_probe(KODE_TEST_DOT_PROBE_MS, &j->map);
            break;
        case (uint8_t)TD_JOB_STATION:
            rc = td_station();
            break;
        case (uint8_t)TD_JOB_CLOSE:
            rc = td_close(j->rest);
            break;
        case (uint8_t)TD_JOB_RELAYS:
            rc = kdrv_pcf8574_write(j->arg);
            break;
        case (uint8_t)TD_JOB_ADC:
            rc = kdrv_ads1115_read_mv(j->arg, &j->mv);
            break;
        case (uint8_t)TD_JOB_LOOP:
            rc = kdrv_dock_loop_run(j->arg, &j->loop);
            break;
        case (uint8_t)TD_JOB_CAM:
            if ( NULL != j->cam )
            {
                rc = td_cam_step(j->cam, j->arg);
            }
            break;
        case (uint8_t)TD_JOB_PLACE:
            if ( NULL != j->cam )
            {
                rc = td_cam_place(j->cam);
            }
            break;
        case (uint8_t)TD_JOB_PHOTO_OUT:
            if ( NULL != j->cam )
            {
                rc = td_cam_photo_out(j->cam, j->arg);
            }
            break;
        default:
            rc = KODE_DENIED;
            break;
    }
    j->ms = khal_time_ms() - t0;
    return rc;
}
