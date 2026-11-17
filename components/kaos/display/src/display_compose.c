/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file display_compose.c
 * @brief The pixel accelerator behind AO_Display: the request, its answer and the `compose` row.
 */
#include "display_compose.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "kaos.h"
#include "kbsp_board.h"
#include "kdrv_compose.h"
#include "kdrv_display.h"
#include "khal_sync.h"
#include "khal_system.h"
#include "khal_time.h"
#include "kode_log.h"

#define TAG "kaos.display"

#define COMPOSE_TAG_RUNNING  0x80000000U  /**< Set in an asker's tag while AO_Display runs that request's pass. */
#define COMPOSE_TAG_MASK     0x7FFFFFFFU  /**< A tag's number: the bits under COMPOSE_TAG_RUNNING. */

/**
 * @brief How long an asker goes on waiting for a pass AO_Display has started, ms.
 *
 * Far longer than any pass, which takes milliseconds: a pass that outlasts
 * it is an accelerator that stopped answering.
 */
#define COMPOSE_PASS_MAX_MS  2000U

/**
 * @brief Read a tag the other thread writes.
 * @param tag The tag.
 * @return Its value.
 */
static uint32_t tag_get(uint32_t const *tag)
{
    /* cppcheck-suppress [misra-c2012-17.3,misra-config] ; GCC's __atomic builtins are the compiler's own, never declared in a header */
    return __atomic_load_n(tag, __ATOMIC_ACQUIRE);
}

/**
 * @brief Write a tag the other thread reads.
 * @param tag   The tag.
 * @param value What it becomes.
 */
static void tag_set(uint32_t *tag, uint32_t value)
{
    /* cppcheck-suppress [misra-c2012-17.3,misra-config] ; GCC's __atomic builtins are the compiler's own, never declared in a header */
    __atomic_store_n(tag, value, __ATOMIC_RELEASE);
}

/**
 * @brief Change a tag from one value to another, if it still holds the first.
 *
 * The asker and AO_Display each try this on the asker's tag when the
 * deadline falls: one to give the request up, the other to take it. Only one
 * of the two succeeds.
 *
 * @param tag  The tag.
 * @param from What it must hold.
 * @param to   What it becomes.
 * @return true when it held @p from and now holds @p to.
 */
static bool tag_swap(uint32_t *tag, uint32_t from, uint32_t to)
{
    uint32_t expected = from;

    /* cppcheck-suppress [misra-c2012-17.3,misra-config] ; GCC's __atomic builtins are the compiler's own, never declared in a header */
    return __atomic_compare_exchange_n(tag, &expected, to, false, __ATOMIC_ACQ_REL,
                                       __ATOMIC_ACQUIRE);
}

/**
 * @brief The tag after @p tag, never one with COMPOSE_TAG_RUNNING set.
 * @param tag A tag, marked or not.
 * @return The next number.
 */
static uint32_t tag_next(uint32_t tag)
{
    return ((tag & COMPOSE_TAG_MASK) + 1U) & COMPOSE_TAG_MASK;
}

/**
 * @brief Post one pass to AO_Display, leaving its queue the margin it keeps.
 *
 * A dry pool or a queue with no room makes no request: nothing is written
 * into @p ask and nobody is signalled, so no answer is left owed.
 *
 * @param ask The asker's state: where the answer lands.
 * @param job The pass; copied into the request.
 * @param gen The asker's tag for this request.
 * @return true once the request is in AO_Display's queue.
 */
static bool compose_post(display_compose_ask_t *ask, display_compose_t const *job, uint32_t gen)
{
    DisplayComposeReqEvt * const e = Q_NEW_X(DisplayComposeReqEvt, 0U, DISPLAY_COMPOSE_REQ_SIG);
    bool posted = false;

    if ( NULL != e )
    {
        e->job  = *job;
        e->rc   = &ask->rc;
        e->us   = &ask->us;
        e->done = ask->done;
        e->gen  = gen;
        e->cur  = &ask->gen;
        e->ack  = &ask->ack;
        /* A request that does not fit is recycled by the post itself. */
        posted = QACTIVE_POST_X(AO_Display, &e->super, KQP_DISPLAY_CMD_MARGIN, (void *)0);
    }
    return posted;
}

/**
 * @brief Wait for the answer to tag @p gen, and for no other.
 *
 * Only a pass that outlived its asker (KODE_IO_ERROR) leaves a signal owed:
 * it wakes the next wait, which finds a foreign tag and goes on for what is
 * left of its own time. A request given up before AO_Display took it is
 * never answered.
 *
 * @param ask        The asker's state.
 * @param gen        The tag.
 * @param timeout_ms How long to wait in all.
 * @return true once AO_Display answered @p gen; false when the time ran out.
 */
static bool compose_await(display_compose_ask_t *ask, uint32_t gen, uint32_t timeout_ms)
{
    uint32_t const start = khal_time_ms();
    uint32_t       left  = timeout_ms;
    bool           mine  = false;

    while ( !mine && khal_sem_wait_timeout(ask->done, left) )
    {
        uint32_t const spent = khal_time_ms() - start;

        if ( tag_get(&ask->ack) == gen )
        {
            mine = true;
        }
        else if ( timeout_ms <= spent )
        {
            left = 0U;
        }
        else
        {
            left = timeout_ms - spent;
        }
    }
    return mine;
}

kode_status_t display_compose_run(display_compose_ask_t *ask, display_compose_t const *job,
                                  uint32_t timeout_ms, uint32_t *us)
{
    kode_status_t rc = KODE_TIMEOUT;

    if ( (NULL == ask) || (NULL == job) )
    {
        return KODE_DENIED;
    }
    if ( NULL == ask->done )
    {
        ask->done = khal_sem_create();
    }
    if ( NULL == ask->done )
    {
        return KODE_NO_RESOURCES;
    }

    uint32_t const gen = tag_next(tag_get(&ask->gen));
    bool answered;

    tag_set(&ask->gen, gen);
    if ( !compose_post(ask, job, gen) )
    {
        /* No request was made: there is nothing to wait for. */
        return KODE_NO_RESOURCES;
    }
    answered = compose_await(ask, gen, timeout_ms);
    if ( !answered )
    {
        if ( tag_swap(&ask->gen, gen, tag_next(gen)) )
        {
            /* Given up before AO_Display took it: when the request reaches
             * it, it is dropped unread, and no pass ever runs for it. */
            KLOG_W(TAG, "compose: AO_Display did not answer within %u ms", (unsigned)timeout_ms);
        }
        else
        {
            /* AO_Display has taken it: the accelerator is writing the
             * destination now. Returning here would let the asker free
             * pictures a pass still writes, so the pass is waited for. */
            answered = compose_await(ask, gen, COMPOSE_PASS_MAX_MS);
            if ( !answered )
            {
                KLOG_E(TAG, "compose: a pass did not end in %u ms; it may still be writing "
                            "its picture", (unsigned)COMPOSE_PASS_MAX_MS);
                rc = KODE_IO_ERROR;
            }
        }
    }
    if ( answered )
    {
        rc = (kode_status_t)ask->rc;
        if ( NULL != us )
        {
            *us = ask->us;
        }
    }
    return rc;
}

/**
 * @brief Run one pass on the accelerator.
 * @param job The pass.
 * @return The driver's answer; KODE_DENIED for an unknown pass or a glass
 *         source that is not a scale; KODE_NO_RESOURCES with no frame on
 *         the glass to read.
 */
static kode_status_t compose_exec(display_compose_t const *job)
{
    kdrv_compose_pic_t const  dst  = { .px = job->dst.px, .bytes = job->dst.bytes,
                                       .w = job->dst.w, .h = job->dst.h };
    kdrv_compose_pic_t        src  = { .px = job->src.px, .bytes = job->src.bytes,
                                       .w = job->src.w, .h = job->src.h };
    kdrv_compose_rect_t const from = { .x = job->x, .y = job->y, .w = job->w, .h = job->h };
    kode_status_t             rc   = KODE_OK;

    if ( job->glass )
    {
        /* The frame the panel shows: only its owner knows which buffer that
         * is, and it cannot change before this step ends. */
        /* cppcheck-suppress misra-c2012-11.5 ; the driver hands the framebuffer back as void * */
        uint8_t * const front = (uint8_t *)kdrv_display_front_buffer();

        if ( DISPLAY_COMPOSE_SCALE != job->op )
        {
            rc = KODE_DENIED;
        }
        else if ( NULL == front )
        {
            rc = KODE_NO_RESOURCES;
        }
        else
        {
            src.px    = front;
            src.bytes = (uint32_t)KDRV_DISPLAY_FB_BYTES;
            src.w     = (uint16_t)KBSP_LCD_H_RES;
            src.h     = (uint16_t)KBSP_LCD_V_RES;
        }
    }

    if ( KODE_OK == rc )
    {
        switch ( job->op )
        {
            case DISPLAY_COMPOSE_FILL: {
                rc = kdrv_compose_fill(&dst, &from, job->colour);
                break;
            }
            case DISPLAY_COMPOSE_SCALE: {
                rc = kdrv_compose_scale(&dst, job->dst_x, job->dst_y, &src, &from, job->scale_x,
                                        job->scale_y, job->turns);
                break;
            }
            case DISPLAY_COMPOSE_BLEND: {
                rc = kdrv_compose_blend(&dst, job->dst_x, job->dst_y, &src, &from, job->opa);
                break;
            }
            default: {
                rc = KODE_DENIED;
                break;
            }
        }
    }
    return rc;
}

void display_compose_answer(DisplayComposeReqEvt const *q)
{
    /* Taken, or already given up: the asker tries the same tag the other
     * way when its time runs out, and only one of the two changes it. */
    if ( !tag_swap(q->cur, q->gen, q->gen | COMPOSE_TAG_RUNNING) )
    {
        KLOG_W(TAG, "a compose pass its asker stopped waiting for is dropped");
    }
    else
    {
        kode_status_t rc = KODE_DENIED;
        uint32_t      us = 0U;

        if ( display_asleep() )
        {
            /* The link is down and the framebuffers may be gone; the
             * renderer was parked before it went. Said, not served. */
            KLOG_W(TAG, "a compose pass is refused: the glass is asleep");
        }
        else
        {
            uint64_t const t0 = khal_time_us();

            rc = compose_exec(&q->job);
            us = (uint32_t)(khal_time_us() - t0);
        }
        *q->us = us;
        *q->rc = (uint32_t)rc;
        /* The tag before the semaphore, whose release orders both writes
         * before the asker's read on the other core. */
        tag_set(q->ack, q->gen);
        khal_sem_signal(q->done);
    }
}

#if defined(CONFIG_KODEOS_AUTOTEST)
/* ---- The `compose` row ------------------------------------------------------
 * Four pictures the size of the glass, held for the row and no longer. Three
 * passes are judged by a pixel each; then COMPOSE_ROW_FRAMES frames of a
 * transition are timed: the picture that goes shrinks on black, the one that
 * comes fades in over it, and the frame is copied as LVGL copies an image.
 *
 * A bench row, asked for by name only. It holds AO_Autotest's step for about
 * a second, on purpose: what it times is a thread that asks AO_Display for a
 * pass and waits, as a renderer does, and a step cut in pieces would time
 * something else.
 */

#define COMPOSE_ROW_FRAMES    32U   /**< Frames timed. */
#define COMPOSE_ROW_COPIES    4U    /**< Whole-frame copies by the accelerator, timed. */
#define COMPOSE_ROW_WAIT_MS   500U  /**< The bound of one pass, as a renderer's flush. */
#define COMPOSE_ROW_PICS      4U    /**< Pictures the row holds. */
#define COMPOSE_ROW_LINE      160U  /**< The record's room, bytes. */

#define COMPOSE_ROW_W         ((uint16_t)KBSP_LCD_H_RES)  /**< A picture's width: the glass's. */
#define COMPOSE_ROW_H         ((uint16_t)KBSP_LCD_V_RES)  /**< Its height. */

#define COMPOSE_ROW_RED       0xF800U  /**< The picture that goes, as its pixels read: RGB565. */
#define COMPOSE_ROW_BLUE      0x001FU  /**< The picture that comes. */
#define COMPOSE_ROW_BLACK     0x0000U  /**< What is under both. */
#define COMPOSE_ROW_RED_FILL   0xFFFF0000U  /**< The same red, as a fill takes it: 0xAARRGGBB. */
#define COMPOSE_ROW_BLUE_FILL  0xFF0000FFU  /**< The same blue. */
#define COMPOSE_ROW_BLACK_FILL 0xFF000000U  /**< The same black. */
#define COMPOSE_ROW_HALF_OPA  128U     /**< Half way between two pictures. */
#define COMPOSE_ROW_HALF      8U       /**< Half size, in sixteenths. */
#define COMPOSE_ROW_SHRINK    8U       /**< Frames one shrink from full to half size lasts. */

#define COMPOSE_ROW_R_SHIFT   11U    /**< Red's place in a pixel. */
#define COMPOSE_ROW_5_MASK    0x1FU  /**< Red or blue: five bits. */
#define COMPOSE_ROW_MID_LO    13U    /**< The least half of 31 may read back as. */
#define COMPOSE_ROW_MID_HI    18U    /**< The most. */

_Static_assert((COMPOSE_ROW_HALF * 2U) == KDRV_COMPOSE_SCALE_ONE, "half of the same size");

/** @brief The row's pictures, by what they hold. */
enum {
    PIC_GOES = 0,  /**< The screen that goes. */
    PIC_COMES,     /**< The screen that comes. */
    PIC_FRAME,     /**< The frame composed. */
    PIC_COPY       /**< Where the frame is copied: the framebuffer's stand-in. */
};

/** @brief What the row measured, microseconds summed over its frames. */
typedef struct {
    uint32_t wall_us;      /**< The frames, start to end. */
    uint32_t ppa_us;       /**< Inside the accelerator's passes. */
    uint32_t door_us;      /**< The requests' round trips less @c ppa_us. */
    uint32_t copy_sw_us;   /**< The software copies. */
    uint32_t copy_ppa_us;  /**< COMPOSE_ROW_COPIES copies by the accelerator. */
} compose_row_t;

/** @brief A pass of this run never ended: the accelerator may still write the row's pictures. */
static bool s_crow_stuck;

/**
 * @brief One pass through AO_Display, its times added to the row's.
 * @param job  The pass.
 * @param meas The row's sums; NULL for a pass that is not timed.
 * @return The pass's answer.
 */
static kode_status_t crow_pass(display_compose_t const *job, compose_row_t *meas)
{
    static display_compose_ask_t ask;   /* the row's own asker state */
    uint32_t            ppa = 0U;
    uint64_t const      t0  = khal_time_us();
    kode_status_t const rc  = display_compose_run(&ask, job, COMPOSE_ROW_WAIT_MS, &ppa);

    if ( KODE_IO_ERROR == rc )
    {
        s_crow_stuck = true;
    }
    if ( NULL != meas )
    {
        uint32_t const wall = (uint32_t)(khal_time_us() - t0);

        meas->ppa_us += ppa;
        if ( wall > ppa )
        {
            meas->door_us += wall - ppa;
        }
    }
    return rc;
}

/**
 * @brief A whole-picture fill.
 * @param dst    The picture.
 * @param colour Its colour, 0xAARRGGBB.
 * @param meas   The row's sums, or NULL.
 * @return The pass's answer.
 */
static kode_status_t crow_fill(display_pic_t const *dst, uint32_t colour, compose_row_t *meas)
{
    display_compose_t job;

    (void)memset(&job, 0, sizeof(job));
    job.op     = DISPLAY_COMPOSE_FILL;
    job.dst    = *dst;
    job.w      = dst->w;
    job.h      = dst->h;
    job.colour = colour;
    return crow_pass(&job, meas);
}

/**
 * @brief A whole picture scaled into another, its top left at @p x, @p y.
 * @param dst   The picture written.
 * @param x     Column it lands on.
 * @param y     Row it lands on.
 * @param src   The picture read; NULL reads the frame on the glass.
 * @param scale The scale, in sixteenths.
 * @param meas  The row's sums, or NULL.
 * @return The pass's answer.
 */
static kode_status_t crow_scale(display_pic_t const *dst, uint16_t x, uint16_t y,
                               display_pic_t const *src, uint16_t scale, compose_row_t *meas)
{
    display_compose_t job;

    (void)memset(&job, 0, sizeof(job));
    job.op      = DISPLAY_COMPOSE_SCALE;
    job.dst     = *dst;
    job.dst_x   = x;
    job.dst_y   = y;
    job.w       = COMPOSE_ROW_W;
    job.h       = COMPOSE_ROW_H;
    job.scale_x = scale;
    job.scale_y = scale;
    if ( NULL == src )
    {
        job.glass = true;
    }
    else
    {
        job.src = *src;
    }
    return crow_pass(&job, meas);
}

/**
 * @brief A whole picture laid over another.
 * @param dst  The picture under it.
 * @param fg   The picture over it.
 * @param opa  How much of @p fg shows.
 * @param meas The row's sums, or NULL.
 * @return The pass's answer.
 */
static kode_status_t crow_blend(display_pic_t const *dst, display_pic_t const *fg, uint8_t opa,
                               compose_row_t *meas)
{
    display_compose_t job;

    (void)memset(&job, 0, sizeof(job));
    job.op  = DISPLAY_COMPOSE_BLEND;
    job.dst = *dst;
    job.src = *fg;
    job.w   = fg->w;
    job.h   = fg->h;
    job.opa = opa;
    return crow_pass(&job, meas);
}

/**
 * @brief The pixel at @p x, @p y of a picture.
 * @param pic The picture.
 * @param x   Its column.
 * @param y   Its row.
 * @return The pixel, RGB565.
 */
static uint16_t crow_pixel(display_pic_t const *pic, uint32_t x, uint32_t y)
{
    size_t const at = (((size_t)y * pic->w) + x) * KDRV_COMPOSE_PX_BYTES;

    return (uint16_t)((uint16_t)pic->px[at] | ((uint16_t)pic->px[at + 1U] << 8U));
}

/**
 * @brief Whether a five-bit channel reads as half of full.
 * @param v The channel.
 * @return true inside COMPOSE_ROW_MID_LO..COMPOSE_ROW_MID_HI.
 */
static bool crow_is_half(uint16_t v)
{
    return (COMPOSE_ROW_MID_LO <= v) && (COMPOSE_ROW_MID_HI >= v);
}

/**
 * @brief The three passes, each judged by the pixels it leaves.
 * @param pic The row's pictures.
 * @return true when all three wrote what they should.
 */
static bool crow_judge(display_pic_t const *pic)
{
    display_pic_t const * const goes  = &pic[PIC_GOES];
    display_pic_t const * const comes = &pic[PIC_COMES];
    display_pic_t const * const frame = &pic[PIC_FRAME];
    uint32_t const half_w = (uint32_t)COMPOSE_ROW_W / 2U;
    uint32_t const half_h = (uint32_t)COMPOSE_ROW_H / 2U;
    bool ok = true;
    kode_status_t rc;

    /* A fill reaches the first pixel and the last. */
    rc = crow_fill(goes, COMPOSE_ROW_RED_FILL, NULL);
    if ( KODE_OK == rc )
    {
        rc = crow_fill(comes, COMPOSE_ROW_BLUE_FILL, NULL);
    }
    if ( (KODE_OK != rc)
        || (crow_pixel(goes, 0U, 0U) != COMPOSE_ROW_RED)
        || (crow_pixel(goes, (uint32_t)COMPOSE_ROW_W - 1U, (uint32_t)COMPOSE_ROW_H - 1U)
            != COMPOSE_ROW_RED)
        || (crow_pixel(comes, 0U, 0U) != COMPOSE_ROW_BLUE) )
    {
        KLOG_E(TAG, "test compose: FAIL - a fill did not paint its picture");
        ok = false;
    }

    /* Red halved onto black: red inside the half, black beside and below it. */
    if ( ok )
    {
        rc = crow_fill(frame, COMPOSE_ROW_BLACK_FILL, NULL);
        if ( KODE_OK == rc )
        {
            rc = crow_scale(frame, 0U, 0U, goes, (uint16_t)COMPOSE_ROW_HALF, NULL);
        }
        if ( (KODE_OK != rc)
            || (crow_pixel(frame, half_w - 1U, half_h - 1U) != COMPOSE_ROW_RED)
            || (crow_pixel(frame, half_w, 0U) != COMPOSE_ROW_BLACK)
            || (crow_pixel(frame, 0U, half_h) != COMPOSE_ROW_BLACK) )
        {
            KLOG_E(TAG, "test compose: FAIL - a scale to half did not land where it should");
            ok = false;
        }
    }

    /* Blue at half over red: half of each, and no green. */
    if ( ok )
    {
        rc = crow_scale(frame, 0U, 0U, goes, (uint16_t)KDRV_COMPOSE_SCALE_ONE, NULL);
        if ( KODE_OK == rc )
        {
            rc = crow_blend(frame, comes, (uint8_t)COMPOSE_ROW_HALF_OPA, NULL);
        }
        if ( KODE_OK != rc )
        {
            KLOG_E(TAG, "test compose: FAIL - a copy or a blend was refused");
            ok = false;
        }
        else
        {
            uint16_t const px = crow_pixel(frame, half_w, half_h);
            uint16_t const r  = (px >> COMPOSE_ROW_R_SHIFT) & COMPOSE_ROW_5_MASK;
            uint16_t const b  = px & COMPOSE_ROW_5_MASK;

            if ( !crow_is_half(r) || !crow_is_half(b)
                || (0U != (px & (uint16_t)~(COMPOSE_ROW_RED | COMPOSE_ROW_BLUE))) )
            {
                KLOG_E(TAG, "test compose: FAIL - blue at half over red read 0x%04x",
                       (unsigned)px);
                ok = false;
            }
        }
    }
    return ok;
}

/**
 * @brief Time COMPOSE_ROW_FRAMES frames of the transition, and the copies.
 * @param pic  The row's pictures, PIC_GOES and PIC_COMES painted.
 * @param meas Receives the sums.
 * @return true when every pass answered.
 */
static bool crow_time(display_pic_t const *pic, compose_row_t *meas)
{
    display_pic_t const * const frame = &pic[PIC_FRAME];
    display_pic_t const * const copy  = &pic[PIC_COPY];
    size_t const   bytes = (size_t)COMPOSE_ROW_W * COMPOSE_ROW_H * KDRV_COMPOSE_PX_BYTES;
    uint64_t const start = khal_time_us();
    bool ok = true;

    for ( uint32_t i = 0U; (COMPOSE_ROW_FRAMES > i) && ok; i++ )
    {
        /* From full size down to half, over and over; the other fades in once. */
        uint16_t const scale = (uint16_t)(KDRV_COMPOSE_SCALE_ONE - (i % COMPOSE_ROW_SHRINK));
        uint16_t const w     = (uint16_t)(((uint32_t)COMPOSE_ROW_W * scale) / KDRV_COMPOSE_SCALE_ONE);
        uint16_t const h     = (uint16_t)(((uint32_t)COMPOSE_ROW_H * scale) / KDRV_COMPOSE_SCALE_ONE);
        uint8_t const  opa   = (uint8_t)(((i + 1U) * KDRV_COMPOSE_OPA_MAX) / COMPOSE_ROW_FRAMES);

        kode_status_t  rc    = crow_fill(frame, COMPOSE_ROW_BLACK_FILL, meas);

        if ( KODE_OK == rc )
        {
            rc = crow_scale(frame, (uint16_t)((COMPOSE_ROW_W - w) / 2U),
                           (uint16_t)((COMPOSE_ROW_H - h) / 2U), &pic[PIC_GOES], scale, meas);
        }
        if ( KODE_OK == rc )
        {
            rc = crow_blend(frame, &pic[PIC_COMES], opa, meas);
        }
        if ( KODE_OK == rc )
        {
            uint64_t const t0 = khal_time_us();

            (void)memcpy(copy->px, frame->px, bytes);
            meas->copy_sw_us += (uint32_t)(khal_time_us() - t0);
        }
        else
        {
            ok = false;
        }
    }
    meas->wall_us = (uint32_t)(khal_time_us() - start);

    for ( uint32_t i = 0U; (COMPOSE_ROW_COPIES > i) && ok; i++ )
    {
        uint64_t const      t0 = khal_time_us();
        kode_status_t const rc = crow_scale(copy, 0U, 0U, frame, (uint16_t)KDRV_COMPOSE_SCALE_ONE,
                                           NULL);

        meas->copy_ppa_us += (uint32_t)(khal_time_us() - t0);
        ok = (KODE_OK == rc);
    }
    if ( !ok )
    {
        KLOG_E(TAG, "test compose: FAIL - a pass was refused while timing");
    }
    return ok;
}

bool display_compose_row(void)
{
    display_pic_t pic[COMPOSE_ROW_PICS];
    compose_row_t meas;
    const char   *glass = "none";
    bool          ok    = true;

    s_crow_stuck = false;
    (void)memset(&meas, 0, sizeof(meas));
    (void)memset(pic, 0, sizeof(pic));
    for ( size_t i = 0U; COMPOSE_ROW_PICS > i; i++ )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; khal_alloc_psram() answers void *: PSRAM is taken by name (rule 8) */
        pic[i].px    = (uint8_t *)khal_alloc_psram(KDRV_COMPOSE_ALIGN, KDRV_DISPLAY_FB_BYTES);
        pic[i].bytes = (uint32_t)KDRV_DISPLAY_FB_BYTES;
        pic[i].w     = COMPOSE_ROW_W;
        pic[i].h     = COMPOSE_ROW_H;
        if ( NULL == pic[i].px )
        {
            ok = false;
        }
    }
    if ( !ok )
    {
        KLOG_E(TAG, "test compose: FAIL - no PSRAM for %u pictures of %u KB",
               (unsigned)COMPOSE_ROW_PICS, (unsigned)(KDRV_DISPLAY_FB_BYTES / 1024U));
    }
    else
    {
        /* The frame on the glass, read whole. A panel with one framebuffer
         * or a link that is down has none: said, not judged. */
        kode_status_t const g = crow_scale(&pic[PIC_COPY], 0U, 0U, NULL,
                                          (uint16_t)KDRV_COMPOSE_SCALE_ONE, NULL);

        if ( KODE_OK == g )
        {
            glass = "ok";
        }
        else if ( KODE_NO_RESOURCES != g )
        {
            KLOG_E(TAG, "test compose: FAIL - the frame on the glass was not read");
            ok = false;
        }
        else
        {
            /* no frame to read */
        }
    }

    if ( ok )
    {
        ok = crow_judge(pic);
    }
    if ( ok )
    {
        ok = crow_time(pic, &meas);
    }
    if ( ok )
    {
        char line[COMPOSE_ROW_LINE];

        (void)snprintf(line, sizeof(line),
                       ">|compose frames=%" PRIu32 " frame_us=%" PRIu32 " ppa_us=%" PRIu32
                       " door_us=%" PRIu32 " copy_sw_us=%" PRIu32 " copy_ppa_us=%" PRIu32
                       " glass=%s",
                       (uint32_t)COMPOSE_ROW_FRAMES, meas.wall_us / COMPOSE_ROW_FRAMES,
                       meas.ppa_us / COMPOSE_ROW_FRAMES, meas.door_us / COMPOSE_ROW_FRAMES,
                       meas.copy_sw_us / COMPOSE_ROW_FRAMES,
                       meas.copy_ppa_us / COMPOSE_ROW_COPIES, glass);
        kode_log_reply(line);
    }
    if ( s_crow_stuck )
    {
        /* Lost for good, and on purpose: memory a pass may still write is
         * not handed to anybody else. */
        KLOG_E(TAG, "test compose: the row's pictures are kept, %u KB of PSRAM",
               (unsigned)((COMPOSE_ROW_PICS * KDRV_DISPLAY_FB_BYTES) / 1024U));
    }
    else
    {
        for ( size_t i = 0U; COMPOSE_ROW_PICS > i; i++ )
        {
            khal_free(pic[i].px);   /* takes NULL */
        }
    }
    return ok;
}
#endif /* CONFIG_KODEOS_AUTOTEST */
