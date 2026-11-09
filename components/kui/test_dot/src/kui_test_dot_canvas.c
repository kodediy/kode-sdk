/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_test_dot_canvas.c
 * @brief The whole-glass canvas the E5 builders paint: its buffer and its timer live and die with it.
 */
#include <string.h>

#include "kui_test_dot_paint.h"

#define RGB565_BYTES 2U  /**< Bytes of one RGB565 pixel. */

/**
 * @brief Free what the canvas owns, as it is deleted.
 * @param e The delete event; its user data is the canvas.
 */
static void on_delete(lv_event_t *e)
{
    /* cppcheck-suppress misra-c2012-11.5 ; LVGL hands back the canvas this file registered */
    kui_td_canvas_t *c = (kui_td_canvas_t *)lv_event_get_user_data(e);
    if ( NULL != c->timer )
    {
        lv_timer_delete(c->timer);
    }
    lv_draw_buf_destroy(c->buf);
    lv_free(c);
}

kui_td_canvas_t *kui_td_canvas_create(lv_obj_t *parent, const char *name, uint8_t kind)
{
    /* cppcheck-suppress misra-c2012-11.5 ; an allocator returns void * */
    kui_td_canvas_t *c = (kui_td_canvas_t *)lv_malloc_zeroed(sizeof(kui_td_canvas_t));
    if ( NULL == c )
    {
        return NULL;
    }
    c->buf = lv_draw_buf_create((uint32_t)KUI_TD_W_PX, (uint32_t)KUI_TD_H_PX, LV_COLOR_FORMAT_RGB565, 0U);
    if ( NULL == c->buf )
    {
        lv_free(c);
        return NULL;
    }
    c->obj = lv_canvas_create(parent);
    if ( NULL == c->obj )
    {
        lv_draw_buf_destroy(c->buf);
        lv_free(c);
        return NULL;
    }
    c->px.px = (uint16_t *)c->buf->data;
    c->px.stride_px = (size_t)c->buf->header.stride / RGB565_BYTES;
    c->kind = kind;
    (void)memset(c->arg, KUI_TD_ST_PENDING, sizeof(c->arg));
    lv_obj_set_name_static(c->obj, name);
    lv_canvas_set_draw_buf(c->obj, c->buf);
    lv_obj_set_pos(c->obj, 0, 0);
    lv_obj_set_size(c->obj, KUI_TD_W_PX, KUI_TD_H_PX);
    lv_obj_set_user_data(c->obj, c);
    (void)lv_obj_add_event_cb(c->obj, on_delete, LV_EVENT_DELETE, c);
    return c;
}

kui_td_canvas_t *kui_td_canvas_find(const lv_obj_t *parent, const char *name)
{
    kui_td_canvas_t *c = NULL;
    if ( NULL != parent )
    {
        lv_obj_t *obj = lv_obj_find_by_name(parent, name);
        if ( NULL != obj )
        {
            /* cppcheck-suppress misra-c2012-11.5 ; the user data is the canvas kui_td_canvas_create() set */
            c = (kui_td_canvas_t *)lv_obj_get_user_data(obj);
        }
    }
    return c;
}

void kui_td_canvas_animate(kui_td_canvas_t *c, uint32_t period_ms, lv_timer_cb_t cb)
{
    c->timer = lv_timer_create(cb, period_ms, c);
}

void kui_td_canvas_show(const kui_td_canvas_t *c)
{
    /* The painter wrote through the CPU's cache; a draw unit that reads the canvas by DMA would see stale bytes. Write the
     * cache back, then ask LVGL to draw the canvas again. */
    lv_draw_buf_flush_cache(c->buf, NULL);
    (void)lv_obj_invalidate(c->obj);
}
