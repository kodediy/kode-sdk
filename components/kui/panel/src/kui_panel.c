/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_panel.c
 * @brief The corner check and the bezel. See kui_panel.h.
 */

/*********************
 *      INCLUDES
 *********************/

#include "kui_panel.h"

#include "kui_internal.h"
#include "kui_tokens.h"
#include "lvgl_private.h" /* lv_obj_class_t is private in 9.6; the report falls back to the class name */

/*********************
 *      DEFINES
 *********************/

#define BEZEL_NAME "kui_bezel"
/* A surface whose name starts with this is a deliberate bleed; see kui_panel.h, "The three exceptions". */
#define BLEED_PREFIX "bleed_"
#define BLEED_PREFIX_LEN 6

/**********************
 *  STATIC PROTOTYPES
 **********************/

static int32_t isqrt_ceil(int32_t v);
static int32_t clamp(int32_t v, int32_t lo, int32_t hi);
static int32_t corner_depth(const kui_glass_t *glass, const lv_area_t *a, kui_panel_corner_t corner);
static bool draws_something(lv_obj_t *obj);
static uint32_t walk(const kui_glass_t *glass, lv_obj_t *obj, bool is_root, const lv_area_t *clip,
                      kui_panel_report_cb_t cb, void *user);

/**********************
 *  STATIC VARIABLES
 **********************/

static const char *const corner_names[KUI_PANEL_CORNER_COUNT] = {
    "top-left", "top-right", "bottom-left", "bottom-right"
};

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

const char *kui_panel_corner_name(kui_panel_corner_t corner)
{
    if ( (0 > corner) || (KUI_PANEL_CORNER_COUNT <= corner) )
    {
        return "?";
    }
    return corner_names[corner];
}

uint32_t kui_panel_check(const kui_glass_t *glass, lv_obj_t *root, kui_panel_report_cb_t cb, void *user)
{
    if ( (NULL == glass) || (NULL == root) )
    {
        return 0;
    }
    lv_area_t const panel = { 0, 0, glass->w - 1, glass->h - 1 };
    return walk(glass, root, true, &panel, cb, user);
}

lv_obj_t *kui_panel_draw_bezel(const kui_glass_t *glass, lv_obj_t *layer, lv_color_t color, bool edge_line)
{
    if ( (NULL == glass) || (NULL == layer) )
    {
        return NULL;
    }

    lv_obj_t *bezel = lv_obj_create(layer);
    lv_obj_remove_style_all(bezel);
    lv_obj_set_name(bezel, BEZEL_NAME);
    /* lv_obj_set_pos() is relative to the parent's content area, so a parent with padding would slide
     * the bezel off the panel and make the snapshot lie about what the glass cuts. Cancel the padding
     * here. */
    lv_obj_set_pos(bezel, -lv_obj_get_style_pad_left(layer, LV_PART_MAIN),
                    -lv_obj_get_style_pad_top(layer, LV_PART_MAIN));
    lv_obj_set_size(bezel, glass->w, glass->h);
    lv_obj_set_clickable(bezel, false);
    lv_obj_set_scrollable(bezel, false);
    lv_obj_set_ignore_layout(bezel, true);

    lv_obj_set_style_radius(bezel, glass->corner_r, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bezel, LV_OPA_TRANSP, LV_PART_MAIN);

    /* The outline follows the rounded rectangle from outside, so the four corner regions, and only
     * those, are painted. It is made wide enough to reach the edges of a screen of this size. */
    lv_obj_set_style_outline_width(bezel, LV_MAX(glass->w, glass->h), LV_PART_MAIN);
    lv_obj_set_style_outline_pad(bezel, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_color(bezel, color, LV_PART_MAIN);
    lv_obj_set_style_outline_opa(bezel, LV_OPA_COVER, LV_PART_MAIN);

    if ( edge_line )
    {
        lv_obj_set_style_border_width(bezel, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(bezel, lv_color_hex(KUI_PALETTE_MEDIUM_GREY), LV_PART_MAIN);
        lv_obj_set_style_border_opa(bezel, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_side(bezel, LV_BORDER_SIDE_FULL, LV_PART_MAIN);
    }

    return bezel;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

/**
 * @brief ceil(sqrt(v)).
 * @param v The radicand.
 * @return The smallest x with x * x >= v.
 */
static int32_t isqrt_ceil(int32_t v)
{
    int32_t const s = kui_isqrt_floor(v);
    if ( (s * s) < v )
    {
        return s + 1;
    }
    return s;
}

/**
 * @brief Clamp a value into a closed range.
 * @param v  The value.
 * @param lo The lower bound.
 * @param hi The upper bound.
 * @return `v`, or the bound it crosses.
 */
static int32_t clamp(int32_t v, int32_t lo, int32_t hi)
{
    if ( v < lo )
    {
        return lo;
    }
    if ( v > hi )
    {
        return hi;
    }
    return v;
}

/**
 * @brief How far the rectangle reaches past one corner circle, in pixels, rounded up; 0 when it stays
 * inside.
 *
 * The rectangle's point nearest that screen corner is clamped into the
 * corner's square and measured against the circle's centre. Outside the
 * square the glass is whole, which the clamp expresses: a clamped point
 * can never be farther from the centre than r unless the rectangle really
 * enters the corner. LVGL areas are inclusive, so the rectangle covers
 * [x1, x2 + 1] x [y1, y2 + 1] in the continuous frame the drawing's
 * millimetres live in.
 *
 * This is not kui_glass_inset(): the inset is a horizontal distance at a
 * given depth, and this is the depth of a point past a circle. The two
 * share the same circle but answer different questions, so the maths stays
 * here rather than folding into kui_glass.
 *
 * @param glass  The glass.
 * @param a      The object's clipped, on-screen rectangle.
 * @param corner The corner to measure against.
 * @return The depth in pixels past that corner's circle; 0 when inside.
 */
static int32_t corner_depth(const kui_glass_t *glass, const lv_area_t *a, kui_panel_corner_t corner)
{
    int32_t const r = glass->corner_r;
    if ( 0 >= r )
    {
        return 0;
    }

    bool const right = (KUI_PANEL_CORNER_TOP_RIGHT == corner) || (KUI_PANEL_CORNER_BOTTOM_RIGHT == corner);
    bool const bottom = (KUI_PANEL_CORNER_BOTTOM_LEFT == corner) || (KUI_PANEL_CORNER_BOTTOM_RIGHT == corner);

    /* Centre of the corner circle. */
    int32_t cx;
    if ( right )
    {
        cx = glass->w - r;
    }
    else
    {
        cx = r;
    }
    int32_t cy;
    if ( bottom )
    {
        cy = glass->h - r;
    }
    else
    {
        cy = r;
    }

    /* The rectangle's point nearest this screen corner. */
    int32_t px;
    if ( right )
    {
        px = a->x2 + 1;
    }
    else
    {
        px = a->x1;
    }
    int32_t py;
    if ( bottom )
    {
        py = a->y2 + 1;
    }
    else
    {
        py = a->y1;
    }

    if ( right )
    {
        px = clamp(px, cx, glass->w);
    }
    else
    {
        px = clamp(px, 0, cx);
    }
    if ( bottom )
    {
        py = clamp(py, cy, glass->h);
    }
    else
    {
        py = clamp(py, 0, cy);
    }

    int32_t const dx = px - cx;
    int32_t const dy = py - cy;
    int32_t const dd = (dx * dx) + (dy * dy);
    if ( (r * r) >= dd )
    {
        return 0;
    }
    return isqrt_ceil(dd) - r;
}

/** @brief Does this object put ink on the screen? A label, an image, or anything with a fill or a border. */
static bool draws_something(lv_obj_t *obj)
{
#if LV_USE_LABEL
    if ( lv_obj_check_type(obj, &lv_label_class) )
    {
        return true;
    }
#endif
#if LV_USE_IMAGE
    if ( lv_obj_check_type(obj, &lv_image_class) )
    {
        return true;
    }
#endif
    if ( lv_obj_get_style_bg_opa(obj, LV_PART_MAIN) != LV_OPA_TRANSP )
    {
        return true;
    }
    if ( lv_obj_get_style_border_width(obj, LV_PART_MAIN) != 0 )
    {
        return true;
    }
    return false;
}

/**
 * @brief Walk one object and its subtree, reporting whatever draws outside the glass.
 * @param glass   The glass.
 * @param obj     The object to visit.
 * @param is_root True for the object kui_panel_check() was called with.
 * @param clip    The rectangle this object's own drawing is cut down to.
 * @param cb      Called once per offending object; may be NULL.
 * @param user    Passed through to `cb` unchanged.
 * @return The number of offending objects in this subtree.
 */
static uint32_t walk(const kui_glass_t *glass, lv_obj_t *obj, bool is_root, const lv_area_t *clip,
                      kui_panel_report_cb_t cb, void *user)
{
    if ( lv_obj_is_hidden(obj) )
    {
        return 0; /* a hidden subtree draws nothing at all */
    }

    uint32_t count = 0;
    const char *name = lv_obj_get_name(obj);

    bool skip = is_root; /* the screen is the page, not content */
    if ( !skip && (NULL != name) && lv_streq(name, BEZEL_NAME) )
    {
        skip = true;
    }
    /* A surface the designer meant to run off an edge: its own rectangle is not measured, and what it
     * carries still is, because the walk goes on into its children. The name is the whole of the
     * opt-out, so a bleed is visible in a dump of the tree, not hidden in a flag. */
    if ( !skip && (NULL != name) && (lv_strncmp(name, BLEED_PREFIX, BLEED_PREFIX_LEN) == 0) )
    {
        skip = true;
    }

    if ( !skip && draws_something(obj) )
    {
        /* What is tested is what is drawn: the object's rectangle cut down by every ancestor that
         * clips it. A row scrolled out of a list is not on the glass, it is not on the screen at
         * all. */
        lv_area_t a;
        lv_obj_get_coords(obj, &a);
        if ( a.x1 < clip->x1 )
        {
            a.x1 = clip->x1;
        }
        if ( a.y1 < clip->y1 )
        {
            a.y1 = clip->y1;
        }
        if ( a.x2 > clip->x2 )
        {
            a.x2 = clip->x2;
        }
        if ( a.y2 > clip->y2 )
        {
            a.y2 = clip->y2;
        }
        bool const empty = (a.x2 < a.x1) || (a.y2 < a.y1); /* a zero-width label puts nothing anywhere */
        bool const on_screen = !empty && (0 <= a.x2) && (0 <= a.y2) && (a.x1 < glass->w) && (a.y1 < glass->h);
        /* A surface that spans the panel from side to side and runs off the top or the bottom edge
         * (or the other way round) is a background, not content: it is meant to be rounded off by the
         * glass, the way the page itself is. A bottom sheet is the case that matters; a full-screen
         * background is the case where both hold. What such a surface carries is still checked,
         * because the walk goes on into its children. */
        bool const spans_w = (0 >= a.x1) && ((glass->w - 1) <= a.x2);
        bool const spans_h = (0 >= a.y1) && ((glass->h - 1) <= a.y2);
        bool const bleeds = (spans_w && ((0 >= a.y1) || ((glass->h - 1) <= a.y2)))
                             || (spans_h && ((0 >= a.x1) || ((glass->w - 1) <= a.x2)));
        if ( on_screen && !bleeds )
        {
            kui_panel_corner_t worst = KUI_PANEL_CORNER_TOP_LEFT;
            int32_t depth = 0;
            for ( int32_t c = 0; (int32_t)KUI_PANEL_CORNER_COUNT > c; c++ )
            {
                int32_t const d = corner_depth(glass, &a, (kui_panel_corner_t)c);
                if ( d > depth )
                {
                    depth = d;
                    worst = (kui_panel_corner_t)c;
                }
            }
            if ( 0 < depth )
            {
                count++;
                if ( NULL != cb )
                {
                    const char *label = name;
                    if ( NULL == label )
                    {
                        const lv_obj_class_t *cls = lv_obj_get_class(obj);
                        if ( (NULL != cls) && (NULL != cls->name) )
                        {
                            label = cls->name;
                        }
                        else
                        {
                            label = "object";
                        }
                    }
                    cb(obj, label, worst, depth, user);
                }
            }
        }
    }

    /* LVGL draws children inside the parent's rectangle, so that is the clip the subtree inherits. An
     * object with LV_OBJ_FLAG_OVERFLOW_VISIBLE is not honoured here; nothing in the kit sets it. */
    lv_area_t own;
    lv_obj_get_coords(obj, &own);
    lv_area_t child_clip = *clip;
    if ( own.x1 > child_clip.x1 )
    {
        child_clip.x1 = own.x1;
    }
    if ( own.y1 > child_clip.y1 )
    {
        child_clip.y1 = own.y1;
    }
    if ( own.x2 < child_clip.x2 )
    {
        child_clip.x2 = own.x2;
    }
    if ( own.y2 < child_clip.y2 )
    {
        child_clip.y2 = own.y2;
    }

    uint32_t const n = lv_obj_get_child_count(obj);
    for ( uint32_t i = 0; i < n; i++ )
    {
        count += walk(glass, lv_obj_get_child(obj, i), false, &child_clip, cb, user);
    }
    return count;
}
