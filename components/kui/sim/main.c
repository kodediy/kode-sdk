/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file main.c
 * @brief kui_sim: a consumer's screens on the desktop, in a window or as a PNG.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include LV_SDL_INCLUDE_PATH
#include "lodepng.h"

#include "kui.h"
#include "kui_panel.h"
#include "kui_sim.h"
#include "device.h"
#include "keys.h"

/** The most --set options one run takes. */
#define SIM_MAX_SETS 16U

/** The command line, parsed. */
typedef struct {
    const char *screen;             /**< The screen's name. */
    const char *snapshot;           /**< Where the PNG goes; NULL for a window. */
    const char *sets[SIM_MAX_SETS]; /**< Each SUBJECT=VALUE. */
    uint32_t set_count;             /**< How many sets holds. */
    bool device;                    /**< The Kode Dot around the screen. */
    bool bezel;                     /**< The glass's corners painted over. */
} sim_opts_t;

/** @brief The usage line, on stderr. */
static void usage(void)
{
    (void)fprintf(stderr, "usage: kui_sim <screen> [--snapshot OUT.png] "
                          "[--set SUBJECT=VALUE]... [--device] [--bezel]\n");
}

/** @brief The screens a consumer offers, on stderr. */
static void list_screens(void)
{
    (void)fprintf(stderr, "screens:");
    for ( uint32_t i = 0U; i < kui_sim_screen_count; i++ )
    {
        (void)fprintf(stderr, " %s", kui_sim_screens[i].name);
    }
    (void)fprintf(stderr, "\n");
}

/**
 * @brief Parse the command line.
 * @param argc The argument count.
 * @param argv The arguments.
 * @param o    Filled in.
 * @return 0, or 2 for a usage error.
 */
static int parse(int argc, char **argv, sim_opts_t *o)
{
    (void)memset(o, 0, sizeof(*o));
    int i = 1;
    while ( i < argc )
    {
        const char *a = argv[i];
        bool const has_value = ((i + 1) < argc);
        if ( (strcmp(a, "--snapshot") == 0) && has_value )
        {
            o->snapshot = argv[i + 1];
            i += 2;
        }
        else if ( (strcmp(a, "--set") == 0) && has_value && (SIM_MAX_SETS > o->set_count) )
        {
            o->sets[o->set_count] = argv[i + 1];
            o->set_count++;
            i += 2;
        }
        else if ( strcmp(a, "--device") == 0 )
        {
            o->device = true;
            i += 1;
        }
        else if ( strcmp(a, "--bezel") == 0 )
        {
            o->bezel = true;
            i += 1;
        }
        else if ( ('-' != a[0]) && (NULL == o->screen) )
        {
            o->screen = a;
            i += 1;
        }
        else
        {
            return 2;
        }
    }
    if ( NULL == o->screen )
    {
        return 2;
    }
    return 0;
}

/**
 * @brief Let LVGL run for a while: timers, layout, the SDL events.
 * @param ms How long, in milliseconds.
 */
static void pump(uint32_t ms)
{
    for ( uint32_t t = 0U; t < ms; t += 5U )
    {
        (void)lv_timer_handler();
        SDL_Delay(5U);
    }
}

/**
 * @brief Feed every --set to the consumer.
 * @param o The command line.
 * @return 0, or 2 when one is malformed or unknown.
 */
static int apply_sets(const sim_opts_t *o)
{
    for ( uint32_t i = 0U; i < o->set_count; i++ )
    {
        char name[64];
        const char *eq = strchr(o->sets[i], '=');
        if ( NULL == eq )
        {
            (void)fprintf(stderr, "--set %s: want SUBJECT=VALUE\n", o->sets[i]);
            return 2;
        }
        size_t const n = (size_t)(eq - o->sets[i]);
        if ( sizeof(name) <= n )
        {
            (void)fprintf(stderr, "--set %s: name too long\n", o->sets[i]);
            return 2;
        }
        (void)memcpy(name, o->sets[i], n);
        name[n] = '\0';
        if ( !kui_sim_set(name, eq + 1) )
        {
            (void)fprintf(stderr, "--set: no subject %s, or a value it does not take\n", name);
            return 2;
        }
    }
    return 0;
}

/**
 * @brief Render an object into a PNG file.
 * @param obj  What to render (a screen, or the device frame).
 * @param path Where to write.
 * @return 0, or 1 on any failure (said on stdout).
 */
static int write_png(lv_obj_t *obj, const char *path)
{
    lv_draw_buf_t *buf = lv_snapshot_take(obj, LV_COLOR_FORMAT_ARGB8888);
    if ( NULL == buf )
    {
        (void)printf("snapshot failed\n");
        return 1;
    }
    uint32_t const w = buf->header.w;
    uint32_t const h = buf->header.h;
    unsigned char *rgba = malloc((size_t)w * h * 4U);
    if ( NULL == rgba )
    {
        lv_draw_buf_destroy(buf);
        (void)printf("out of memory\n");
        return 1;
    }
    for ( uint32_t y = 0U; y < h; y++ )
    {
        const uint8_t *row = (const uint8_t *)buf->data + ((size_t)y * buf->header.stride);
        unsigned char *out = rgba + ((size_t)y * w * 4U);
        for ( uint32_t x = 0U; x < w; x++ )
        {
            /* LVGL's ARGB8888 is B, G, R, A in memory. */
            out[(x * 4U) + 0U] = row[(x * 4U) + 2U];
            out[(x * 4U) + 1U] = row[(x * 4U) + 1U];
            out[(x * 4U) + 2U] = row[(x * 4U) + 0U];
            out[(x * 4U) + 3U] = 255U;
        }
    }
    /* Encoded in memory and written with stdio: LVGL's lodepng routes its own
     * file I/O through lv_fs drives. */
    unsigned char *png = NULL;
    size_t png_size = 0U;
    unsigned const err = lodepng_encode32(&png, &png_size, rgba, w, h);
    free(rgba);
    lv_draw_buf_destroy(buf);
    if ( 0U != err )
    {
        (void)printf("cannot encode %s: %s\n", path, lodepng_error_text(err));
        return 1;
    }
    FILE *f = fopen(path, "wb");
    if ( NULL == f )
    {
        (void)printf("cannot write %s\n", path);
        free(png);
        return 1;
    }
    size_t const wrote = fwrite(png, 1U, png_size, f);
    (void)fclose(f);
    free(png);
    if ( wrote != png_size )
    {
        (void)printf("cannot write %s\n", path);
        return 1;
    }
    (void)printf("wrote %s (%ux%u)\n", path, (unsigned)w, (unsigned)h);
    return 0;
}

/**
 * @brief The simulator.
 *
 * Exit status: 0 done, 1 a render or a write failed, 2 a usage error.
 *
 * @param argc The argument count.
 * @param argv The arguments.
 * @return The exit status.
 */
int main(int argc, char **argv)
{
    sim_opts_t o;
    if ( parse(argc, argv, &o) != 0 )
    {
        usage();
        return 2;
    }
    if ( (NULL != o.snapshot) && (getenv("SDL_VIDEODRIVER") == NULL) )
    {
        (void)setenv("SDL_VIDEODRIVER", "dummy", 1);
    }

    lv_init();
    lv_display_t *disp = NULL;
    if ( o.device )
    {
        disp = kui_sim_device_create(1.0f);
    }
    else
    {
        disp = lv_sdl_window_create(410, 502);
        if ( NULL != disp )
        {
            lv_sdl_window_set_title(disp, "kui_sim");
            (void)lv_sdl_mouse_create();
        }
    }
    if ( NULL == disp )
    {
        return 1;
    }
    kui_sim_device_watch_install();
    kui_sim_keys_install(disp);

    kui_init(disp);
    kui_sim_setup();

    lv_obj_t *scr = NULL;
    for ( uint32_t i = 0U; i < kui_sim_screen_count; i++ )
    {
        if ( strcmp(kui_sim_screens[i].name, o.screen) == 0 )
        {
            scr = kui_sim_screens[i].get();
        }
    }
    if ( NULL == scr )
    {
        (void)fprintf(stderr, "no screen %s\n", o.screen);
        list_screens();
        return 2;
    }
    lv_screen_load(scr);
    int const rc = apply_sets(&o);
    if ( 0 != rc )
    {
        return rc;
    }
    if ( o.bezel )
    {
        /* A snapshot renders the screen object and not the display, so for a
         * snapshot the bezel goes on the screen; in a window, on the top layer. */
        lv_obj_t *layer = lv_layer_top();
        if ( NULL != o.snapshot )
        {
            layer = scr;
        }
        kui_glass_t const g = kui_glass_kode_dot();
        (void)kui_panel_draw_bezel(&g, layer, lv_color_black(), false);
    }
    pump(400U);

    if ( NULL != o.snapshot )
    {
        lv_obj_t *what = scr;
        if ( o.device )
        {
            what = kui_sim_device_frame_screen();
        }
        return write_png(what, o.snapshot);
    }
    for ( ;; )
    {
        pump(5U);
    }
}
