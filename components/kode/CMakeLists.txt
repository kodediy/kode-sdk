# kode - the standard layer: the one include an app needs.
#
# It sits on kode_sys (the facade for apps with Active Objects of their own)
# and drags it in, with every layer below it: an app pins kodediy/kode and
# nothing else. kdrv is named because kode.h includes kapp_desc.h, a kdrv
# header. The screen calls live in kode_screen, which an app that draws names
# itself, so kode does not depend on kscreen: a headless app must not download
# LVGL.
# include/ holds kode.h alone. internal/include holds what an app never
# includes: kode_colors.h, the palette gen_tokens.py writes, which compiles
# only inside kode.h; and kode_internal.h, kode_screen's door into the layer,
# which compiles only under KODE_INTERNAL_ALLOWED, defined by kode and
# kode_screen for their own sources, and which tools/check_layers.sh keeps to
# those two; and kode_pin_lock.h, the lock J3's pin and bus calls share, under
# the same guard.
set(srcs "src/kode_audio.c" "src/kode_battery.c" "src/kode_bus.c" "src/kode_button.c" "src/kode_entry.c" "src/kode_event.c" "src/kode_file.c" "src/kode_haptic.c" "src/kode_http.c"
         "src/kode_ir.c" "src/kode_ir_capture.c" "src/kode_ir_file.c" "src/kode_led.c" "src/kode_motion.c" "src/kode_mqtt.c" "src/kode_net.c" "src/kode_pin.c" "src/kode_pin_power.c" "src/kode_rfid.c" "src/kode_screen_bright.c" "src/kode_setting.c"
         "src/kode_standard_ao.c" "src/kode_system.c" "src/kode_tag.c" "src/kode_touch.c" "src/kode_wifi.c" "src/kode_ble.c")
if(CONFIG_KODEOS_NFC)
    # Off, the reader is the app's, and a call to kode_nfc_*() fails to link.
    list(APPEND srcs "src/kode_nfc.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include" "internal/include"
    REQUIRES kode_sys kdefs kaos kdrv khal
)

# The two private headers compile only where these are defined: here, privately.
target_compile_definitions(${COMPONENT_LIB} PRIVATE KODE_SYS_INTERNAL_ALLOWED KODE_INTERNAL_ALLOWED)
