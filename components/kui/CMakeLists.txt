# kui: the Kode Dot's UI kit. LVGL, kbsp and khal, and nothing above them.
# khal is there for three files: kui_mem.c, LVGL's allocator hooks, which
# reach the PSRAM heap the only legal way; kui_kody.c, which inflates an
# animation with the ROM's inflater behind khal_zlib.h; and kui_pack.c,
# which reads Kody's pack with cJSON on PSRAM and checksums it. klog, below
# khal, is the log the Kody player and the pack report a refusal through.
#
# Not WHOLE_ARCHIVE, on purpose: every face and every glyph is its own object
# file, so the linker keeps only what something names.
#
# Kody's built-in scene is the same: one object file per animation, written
# by tools/kodygen.py from assets/kody/. Only the reloj scene's three are
# compiled in - the floor a board falls back on when no pack is usable; every
# other animation is Kody's pack, in pack/, read from a medium (kui_pack.h).
# There is nothing to play them with unless LVGL has Lottie, so a firmware
# without it does not compile them; kui_kody.c compiles to nothing there by
# itself.
set(kody_srcs "")
if(CONFIG_LV_USE_LOTTIE)
    set(kody_srcs "kody/src/kody_reloj.c"
                  "kody/src/kody_despierta_reloj.c"
                  "kody/src/kody_reloj_te_mira.c")
endif()

# The E5 final test's screens and patterns, only in a firmware that runs the test.
set(test_dot_srcs "")
if(CONFIG_KODE_TEST_DOT)
    set(test_dot_srcs "test_dot/src/kui_test_dot_paint.c"
                      "test_dot/src/kui_test_dot_canvas.c"
                      "test_dot/src/kui_test_dot_screens.c"
                      "test_dot/src/kui_test_dot_patterns.c")
endif()

idf_component_register(
    SRCS "core/src/kui.c"
         "core/src/kui_mem.c"
         "core/src/kui_styles.c"
         "core/src/kui_flex.c"
         "drag/src/kui_gesture.c"
         "drag/src/kui_spring.c"
         "drag/src/kui_drag.c"
         "glass/src/kui_glass.c"
         "panel/src/kui_panel.c"
         "core/src/kui_pill.c"
         "core/src/kui_cat_pill.c"
         "core/src/kui_sheet.c"
         "core/src/kui_level.c"
         "core/src/kui_swatch.c"
         "core/src/kui_lamp.c"
         "kody/src/kui_kody.c"
         "kody/src/kui_kody_anchor.c"
         "multitap/src/kui_multitap.c"
         "pack/src/kui_pack.c"
         "fonts/src/kui_font_caption.c"
         "fonts/src/kui_font_body.c"
         "fonts/src/kui_font_button.c"
         "fonts/src/kui_font_name.c"
         "fonts/src/kui_font_title.c"
         "fonts/src/kui_font_heading.c"
         "fonts/src/kui_font_code.c"
         "fonts/src/kui_font_clock.c"
         "fonts/src/kui_font_digits_22.c"
         "fonts/src/kui_font_digits_26.c"
         "fonts/src/kui_font_digits_28.c"
         "fonts/src/kui_font_digits_34.c"
         "fonts/src/kui_font_digits_40.c"
         "fonts/src/kui_font_digits_60.c"
         "fonts/src/kui_font_display_40.c"
         "fonts/src/kui_font_title_60.c"
         "fonts/src/kui_font_code_80.c"
         "fonts/src/kui_font_hero_160.c"
         "images/src/kui_glyph_wifi.c"
         "images/src/kui_glyph_ble.c"
         "images/src/kui_glyph_sun.c"
         "images/src/kui_glyph_battery.c"
         "images/src/kui_glyph_power.c"
         "images/src/kui_glyph_info.c"
         "images/src/kui_glyph_check.c"
         "images/src/kui_glyph_lock.c"
         "images/src/kui_glyph_usb.c"
         "images/src/kui_glyph_bolt.c"
         "images/src/kui_glyph_sd.c"
         "images/src/kui_glyph_radio.c"
         "images/src/kui_glyph_x.c"
         "images/src/kui_glyph_apps_30.c"
         "images/src/kui_glyph_apps_64.c"
         "images/src/kui_glyph_brush_40.c"
         "images/src/kui_glyph_bt_30.c"
         "images/src/kui_glyph_bt_40.c"
         "images/src/kui_glyph_bt_96.c"
         "images/src/kui_glyph_check_30.c"
         "images/src/kui_glyph_chev_l_30.c"
         "images/src/kui_glyph_chev_r_30.c"
         "images/src/kui_glyph_chip_30.c"
         "images/src/kui_glyph_chip_96.c"
         "images/src/kui_glyph_clock_30.c"
         "images/src/kui_glyph_cup_28.c"
         "images/src/kui_glyph_cup_40.c"
         "images/src/kui_glyph_games_40.c"
         "images/src/kui_glyph_info_30.c"
         "images/src/kui_glyph_lock_30.c"
         "images/src/kui_glyph_maker_40.c"
         "images/src/kui_glyph_moon_30.c"
         "images/src/kui_glyph_more_40.c"
         "images/src/kui_glyph_phone_30.c"
         "images/src/kui_glyph_power_30.c"
         "images/src/kui_glyph_power_40.c"
         "images/src/kui_glyph_power_96.c"
         "images/src/kui_glyph_scene_30.c"
         "images/src/kui_glyph_sd_16.c"
         "images/src/kui_glyph_share_30.c"
         "images/src/kui_glyph_share_40.c"
         "images/src/kui_glyph_share_96.c"
         "images/src/kui_glyph_spark_40.c"
         "images/src/kui_glyph_spk0_40.c"
         "images/src/kui_glyph_spk1_40.c"
         "images/src/kui_glyph_spk2_40.c"
         "images/src/kui_glyph_spk3_40.c"
         "images/src/kui_glyph_sun0_40.c"
         "images/src/kui_glyph_sun1_40.c"
         "images/src/kui_glyph_sun2_40.c"
         "images/src/kui_glyph_sun3_40.c"
         "images/src/kui_glyph_talk_40.c"
         "images/src/kui_glyph_tool_40.c"
         "images/src/kui_glyph_torch_40.c"
         "images/src/kui_glyph_torch_64.c"
         "images/src/kui_glyph_vib_40.c"
         "images/src/kui_glyph_watch_30.c"
         "images/src/kui_glyph_wifi_30.c"
         "images/src/kui_glyph_wifi_40.c"
         "images/src/kui_glyph_wifi_96.c"
         "images/src/kui_glyph_x_30.c"
         "images/src/kui_wordmark.c"
         ${kody_srcs}
         ${test_dot_srcs}
    INCLUDE_DIRS
        "core/include"
        "drag/include"
        "glass/include"
        "kody/include"
        "multitap/include"
        "pack/include"
        "panel/include"
        "test_dot/include"
    PRIV_INCLUDE_DIRS "core/src"
    REQUIRES lvgl__lvgl kbsp
    PRIV_REQUIRES khal klog
)

# The allocator hooks are called BY LVGL and named by nothing in kui, and kui
# requires lvgl, so libkui.a reaches the linker before liblvgl.a asks for
# them: left alone, kui_mem.o would never be pulled. Naming one of its symbols
# up front pulls the object, and the object holds them all. Only when the
# board's config selects the custom allocator: otherwise kui_mem.c compiles
# to nothing and there is nothing to name.
if(CONFIG_LV_USE_CUSTOM_MALLOC)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_malloc_core")
endif()

# The font and image files include <lvgl/lvgl.h> unless told the flat layout
# is in use; the managed lvgl__lvgl component ships the flat one. PUBLIC so
# anything including kui.h makes the same choice.
target_compile_definitions(${COMPONENT_LIB} PUBLIC LV_LVGL_H_INCLUDE_SIMPLE)

# Both settings below reach into the lvgl component from here rather than
# from a project, so every firmware that draws with the kit gets them. After
# idf_component_register(), which returns early while ESP-IDF only collects
# requirements; by the real pass lvgl's target exists, because kui requires
# it and a component is processed after what it requires.
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)

# LVGL at -O2, whatever the firmware is optimised for: an app builds at -Og
# by default and kodeOS at -Os, and either leaves a full-screen frame well
# over the panel's 16.8 ms. Measured on the DVT with direct mode and two draw
# units in place, -O2 took a full-screen redraw from 30 to 59.4 fps.
# The option comes after the project's own, and the last -O wins.
target_compile_options(${lvgl_lib} PRIVATE "-O2")

# LVGL's draw threads with their stacks in PSRAM (khal/force/khal_task_psram.h
# says why), through the one xTaskCreate() LVGL makes, in its FreeRTOS layer
# and nowhere else: only that file is given the header. The same file
# includes FreeRTOS's atomic.h and uses none of its static functions, which
# is eleven warnings of LVGL's own on every build; they are silenced there.
# khal is looked up by the name this build gave it: "khal" in this tree,
# "kodediy__khal" in an app that fetched kui. The component manager renames
# what REQUIRES names, never what a lookup like this one names.
idf_build_get_property(kui_build_components BUILD_COMPONENTS)
if(khal IN_LIST kui_build_components)
    idf_component_get_property(khal_dir khal COMPONENT_DIR)
else()
    idf_component_get_property(khal_dir kodediy__khal COMPONENT_DIR)
endif()
idf_component_get_property(lvgl_dir lvgl__lvgl COMPONENT_DIR)
set_source_files_properties("${lvgl_dir}/src/osal/lv_freertos.c"
    TARGET_DIRECTORY ${lvgl_lib}
    PROPERTIES COMPILE_OPTIONS
        "-include;${khal_dir}/force/khal_task_psram.h;-Wno-unused-function")

# ThorVG without its SVG loader (thorvg/kui_tvg_config.h says why): the
# header is force-included into the lvgl component's C++ sources, and only
# those, so ThorVG's own config.h finds its guard taken.
if(CONFIG_LV_USE_THORVG)
    target_compile_options(${lvgl_lib} PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${CMAKE_CURRENT_LIST_DIR}/thorvg/kui_tvg_config.h>")
endif()
