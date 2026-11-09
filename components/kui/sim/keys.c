/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file keys.c
 * @brief The default meaning of the switches. See keys.h.
 */

/*********************
 *      INCLUDES
 *********************/

#include "keys.h"

#include "device.h"

/*********************
 *      DEFINES
 *********************/

#define QUEUE_LEN   32
#define MAX_MEMBERS 256

/**********************
 *      TYPEDEFS
 **********************/

typedef struct {
    kui_sim_button_t id;
    lv_indev_state_t state;
} edge_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/

static bool eats_arrows(lv_obj_t *obj);
static bool is_scroller(lv_obj_t *obj);
static bool is_member(lv_obj_t *obj);
static void collect(lv_obj_t *obj, lv_obj_t **out, int *n);
static void sync_group(void);
static uint32_t key_for(kui_sim_button_t id);
static bool push(kui_sim_button_t id, lv_indev_state_t state);
static void on_button(kui_sim_button_t id, bool pressed, void *user);
static void keypad_read_cb(lv_indev_t *indev, lv_indev_data_t *data);

/**********************
 *  STATIC VARIABLES
 **********************/

static lv_display_t *g_ui_disp;
static lv_group_t *g_group;
static edge_t g_queue[QUEUE_LEN];
static int g_qhead;
static int g_qtail;
/** The edge the keypad was last handed. */
static edge_t g_last = { KUI_SIM_BTN_UP, LV_INDEV_STATE_RELEASED };
/** The key each switch's press was handed out as; its release too. */
static uint32_t g_active_key[KUI_SIM_BTN_COUNT];
/** The switch's press is queued or handed out, its release is not. */
static bool g_held[KUI_SIM_BTN_COUNT];

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void kui_sim_keys_install(lv_display_t *ui_disp)
{
    g_ui_disp = ui_disp;
    g_group = lv_group_create();
    lv_group_set_wrap(g_group, true);
    lv_indev_t *keypad = lv_indev_create();
    lv_indev_set_type(keypad, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(keypad, keypad_read_cb);
    lv_indev_set_display(keypad, ui_disp);
    lv_indev_set_group(keypad, g_group);
    kui_sim_device_set_button_cb(on_button, NULL);
}

void kui_sim_keys_reset(void)
{
    g_qhead = g_qtail;
    for ( int i = 0; KUI_SIM_BTN_COUNT > i; i++ )
    {
        g_held[i] = false;
    }
    if ( LV_INDEV_STATE_PRESSED == g_last.state )
    {
        (void)push(g_last.id, LV_INDEV_STATE_RELEASED);
    }
    if ( NULL != g_group )
    {
        lv_group_remove_all_objs(g_group);
    }
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

/** @brief Whether `obj` is one of the widgets that eat arrows themselves: buttonmatrix, textarea. */
static bool eats_arrows(lv_obj_t *obj)
{
    return lv_obj_has_class(obj, &lv_buttonmatrix_class) || lv_obj_has_class(obj, &lv_textarea_class);
}

/** @brief Scrollable, and with content past its edges on some side: what the finger drags rather than taps. */
static bool is_scroller(lv_obj_t *obj)
{
    if ( !lv_obj_is_scrollable(obj) )
    {
        return false;
    }
    return (lv_obj_get_scroll_top(obj) > 0) || (lv_obj_get_scroll_bottom(obj) > 0)
           || (lv_obj_get_scroll_left(obj) > 0) || (lv_obj_get_scroll_right(obj) > 0);
}

/**
 * @brief Whether `obj` belongs in the focus ring.
 *
 * With no XML naming which callback a tap means, an object is focusable
 * when it is clickable: the same test lv_obj_is_clickable() already ran,
 * asked again through the flag directly.
 */
static bool is_member(lv_obj_t *obj)
{
    if ( !lv_obj_is_clickable(obj) || !lv_obj_is_visible(obj) )
    {
        return false;
    }
    if ( eats_arrows(obj) )
    {
        return true;
    }
    return !is_scroller(obj);
}

/**
 * @brief Collect the focus ring in document order.
 *
 * A hidden object hides its subtree too; one that is only out of view does
 * not stop the walk, since a child of it may overflow into view.
 *
 * @param obj The object to visit.
 * @param out The members found so far.
 * @param n   How many `out` holds; updated.
 */
static void collect(lv_obj_t *obj, lv_obj_t **out, int *n)
{
    if ( lv_obj_is_hidden(obj) )
    {
        return;
    }
    if ( is_member(obj) && (MAX_MEMBERS > *n) )
    {
        out[*n] = obj;
        (*n)++;
    }
    uint32_t count = lv_obj_get_child_count(obj);
    for ( uint32_t i = 0U; i < count; i++ )
    {
        collect(lv_obj_get_child(obj, i), out, n);
    }
}

/** @brief Rebuild the group from what is on the active screen now. */
static void sync_group(void)
{
    lv_obj_t *members[MAX_MEMBERS];
    int n = 0;
    lv_obj_t *scr = lv_display_get_screen_active(g_ui_disp);
    uint32_t count = lv_obj_get_child_count(scr);
    for ( uint32_t i = 0U; i < count; i++ )
    {
        collect(lv_obj_get_child(scr, i), members, &n);
    }

    /* The same list as before: leave it, so the focus does not blink off and on. */
    if ( (int)lv_group_get_obj_count(g_group) == n )
    {
        bool same = true;
        for ( int i = 0; (i < n) && same; i++ )
        {
            if ( lv_group_get_obj_by_index(g_group, (uint32_t)i) != members[i] )
            {
                same = false;
            }
        }
        if ( same )
        {
            return;
        }
    }

    /* Frozen while it refills: the first object added to an empty group
     * would take the focus (FOCUSED and a scroll to it) only to hand it
     * back to the one that had it. */
    lv_obj_t *focused = lv_group_get_focused(g_group);
    bool keep = false;
    lv_group_remove_all_objs(g_group);
    lv_group_focus_freeze(g_group, true);
    for ( int i = 0; i < n; i++ )
    {
        lv_group_add_obj(g_group, members[i]);
        if ( members[i] == focused )
        {
            keep = true;
        }
    }
    lv_group_focus_freeze(g_group, false);
    if ( 0 < n )
    {
        lv_obj_t *first = members[0];
        if ( keep )
        {
            lv_group_focus_obj(focused);
        }
        else
        {
            lv_group_focus_obj(first);
        }
    }
}

/** @brief The LVGL key a switch means, for the object holding the focus now. */
static uint32_t key_for(kui_sim_button_t id)
{
    lv_obj_t *f = lv_group_get_focused(g_group);
    bool matrix = false;
    bool text = false;
    if ( NULL != f )
    {
        matrix = lv_obj_has_class(f, &lv_buttonmatrix_class);
        text = lv_obj_has_class(f, &lv_textarea_class);
    }
    switch ( id )
    {
        case KUI_SIM_BTN_UP:
            if ( matrix )
            {
                return LV_KEY_UP;
            }
            return LV_KEY_PREV;
        case KUI_SIM_BTN_DOWN:
            if ( matrix )
            {
                return LV_KEY_DOWN;
            }
            return LV_KEY_NEXT;
        case KUI_SIM_BTN_LEFT:
            if ( matrix || text )
            {
                return LV_KEY_LEFT;
            }
            return LV_KEY_PREV;
        case KUI_SIM_BTN_RIGHT:
            if ( matrix || text )
            {
                return LV_KEY_RIGHT;
            }
            return LV_KEY_NEXT;
        case KUI_SIM_BTN_TOP:
            return LV_KEY_ENTER;
        default:
            return LV_KEY_ESC;
    }
}

/**
 * @brief Queue one edge.
 * @param id    The switch.
 * @param state Pressed or released.
 * @return false on a burst no hand makes; the console already said the edge.
 */
static bool push(kui_sim_button_t id, lv_indev_state_t state)
{
    int next = (g_qtail + 1) % QUEUE_LEN;
    if ( next == g_qhead )
    {
        return false;
    }
    g_queue[g_qtail].id = id;
    g_queue[g_qtail].state = state;
    g_qtail = next;
    return true;
}

/**
 * @brief The button hook: queue an edge, rolling over whatever switch was held.
 *
 * Runs on the main thread (device.h): for the keyboard inside
 * lv_timer_handler, from the SDL driver's timer; for the picture inside the
 * frame display's pointer indev, as a region's event. It only queues: the
 * keypad reads the edges on its own timer. LVGL's keypad holds one key at a
 * time: a press that arrives while another is held reads as that key still
 * pressed, and the release that follows is taken as the held key's. So a
 * second switch first releases the one that is held, whose own release is
 * then dropped: a row pressed with top is released and clicked before down
 * moves on, and it is never left pressed.
 */
static void on_button(kui_sim_button_t id, bool pressed, void *user)
{
    LV_UNUSED(user);
    if ( pressed )
    {
        for ( int i = 0; KUI_SIM_BTN_COUNT > i; i++ )
        {
            if ( g_held[i] )
            {
                (void)push((kui_sim_button_t)i, LV_INDEV_STATE_RELEASED);
            }
            g_held[i] = false;
        }
        g_held[id] = push(id, LV_INDEV_STATE_PRESSED);
    }
    else if ( g_held[id] )
    {
        g_held[id] = false;
        (void)push(id, LV_INDEV_STATE_RELEASED);
    }
}

/**
 * @brief The keypad indev's read callback.
 *
 * One queued edge per read; between edges, the last one stands (a held key
 * stays held). A press is translated, and the ring rebuilt for it, here,
 * when the keypad takes it, not when it was queued: LVGL has processed
 * every earlier edge by then. So a rollover's release has clicked the row
 * it pressed before a refill can move the focus off that row, and a click
 * that opened a sheet has opened it before the next press walks the ring.
 * Once per press; a refill that finds the same list touches nothing.
 */
static void keypad_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    LV_UNUSED(indev);
    if ( g_qhead != g_qtail )
    {
        g_last = g_queue[g_qhead];
        g_qhead = (g_qhead + 1) % QUEUE_LEN;
        if ( LV_INDEV_STATE_PRESSED == g_last.state )
        {
            sync_group();
            g_active_key[g_last.id] = key_for(g_last.id);
        }
        data->continue_reading = g_qhead != g_qtail;
    }
    data->key = g_active_key[g_last.id];
    data->state = g_last.state;
}
