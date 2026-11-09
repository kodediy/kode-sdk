/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_panel.h
 * @brief The corner check and the bezel, built on kui_glass.
 */
#ifndef KUI_PANEL_H
#define KUI_PANEL_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#include "kui_glass.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Which corner a report is about; the order is reading order. */
typedef enum {
    KUI_PANEL_CORNER_TOP_LEFT = 0,
    KUI_PANEL_CORNER_TOP_RIGHT,
    KUI_PANEL_CORNER_BOTTOM_LEFT,
    KUI_PANEL_CORNER_BOTTOM_RIGHT,
    KUI_PANEL_CORNER_COUNT
} kui_panel_corner_t;

/**
 * @brief Called once per offending object by kui_panel_check().
 * @param obj      The object that falls outside the glass.
 * @param name     Its lv_obj_get_name(), or its class name when it has none; never NULL.
 * @param corner   The corner it enters.
 * @param depth_px How far its nearest point lies outside that corner circle, rounded up.
 * @param user     The pointer given to kui_panel_check().
 */
typedef void (*kui_panel_report_cb_t)(lv_obj_t *obj, const char *name, kui_panel_corner_t corner,
                                       int32_t depth_px, void *user);

/**
 * @brief Walk an object tree and report everything that draws outside the glass.
 *
 * Visits `root` and its descendants. What is tested is what is drawn: an
 * object's rectangle is cut down by every ancestor that clips it, so a row
 * scrolled out of a list is not on the glass because it is not on the
 * screen, while the list that holds it has to clear the glass itself. An
 * object is tested when it is not hidden, when what is left of its
 * rectangle meets the screen, and when it actually paints: a label, an
 * image, or any object whose main part has a non-zero bg_opa or
 * border_width. A hidden object's subtree is skipped with it. An empty
 * rectangle and the bezel of kui_panel_draw_bezel(), which is known by its
 * name, are skipped too.
 *
 * ### The three exceptions
 *
 * 1. `root` itself: the screen is the page, not content on it.
 * 2. A surface that spans the panel from side to side and runs off the top
 *    or the bottom edge (or spans it top to bottom and runs off a side).
 *    Such a background is meant to be rounded off by the glass: a page
 *    background and a bottom sheet are the two cases.
 * 3. A surface whose name begins with `bleed_`. This is an opt-out per
 *    object and not a switch on the check, so no screen can lose the rule
 *    by accident: a card that starts on the glass and disappears under it
 *    so a swipe-up gesture is discoverable is the case it exists for.
 *
 * In all three the surface's own rectangle goes unmeasured and what it
 * carries does not: the walk goes on into the children one at a time. A
 * label inside a `bleed_` card that falls in a corner is reported, which is
 * the whole point - the card may bleed, the words may not.
 *
 * @param glass The glass; NULL answers 0.
 * @param root  The object to walk; NULL answers 0.
 * @param cb    Called once per offending object, with its worst corner; may be NULL.
 * @param user  Passed through to `cb` unchanged.
 * @return The number of offending objects.
 */
uint32_t kui_panel_check(const kui_glass_t *glass, lv_obj_t *root, kui_panel_report_cb_t cb, void *user);

/**
 * @brief Paint the four corner regions over, the way the silk screen does on the device.
 *
 * Creates on `layer` (lv_layer_top() in practice) one full-size object that
 * cannot be clicked, with radius corner_r, a transparent background and an
 * outline of width max(w, h) in `color`: the outline follows the rounded
 * rectangle from outside, so everything the glass hides is covered and
 * everything it shows is untouched. With `edge_line`, a 1 px border in
 * medium grey traces the rounded edge as well; on the device that edge is
 * not a sharp line, so the border is a desktop reading aid only.
 *
 * The object is named "kui_bezel". Delete it with lv_obj_delete() to take
 * the bezel away.
 *
 * @param glass     The glass; NULL answers NULL.
 * @param layer     The parent to create the bezel on; NULL answers NULL.
 * @param color     The outline colour.
 * @param edge_line Whether to also draw the 1 px edge line.
 * @return The bezel object, or NULL when `glass` or `layer` is NULL.
 */
lv_obj_t *kui_panel_draw_bezel(const kui_glass_t *glass, lv_obj_t *layer, lv_color_t color, bool edge_line);

/**
 * @brief The name of a corner.
 * @param corner The corner.
 * @return "top-left", "top-right", "bottom-left", "bottom-right", or "?" when `corner` is out of range.
 */
const char *kui_panel_corner_name(kui_panel_corner_t corner);

#ifdef __cplusplus
}
#endif

#endif /* KUI_PANEL_H */
