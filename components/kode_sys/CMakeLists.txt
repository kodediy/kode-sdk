# kode_sys - the extended layer: kode_init() and what an app with Active
# Objects of its own writes against. One source per module of doors, so an app links the
# modules it calls and no others.
#
# kaos, kqp, kdrv, khal and kdefs are public because kode_sys.h includes and
# re-exports their headers; kbsp is private (the facade claims KBSP_RES_EXP in
# kode_sys_exp.c, and an app never names a resource).
#
# internal/include holds kode_sys_internal.h, the blocking calls kode.h is
# built from; it is not part of the API. ESP-IDF has no include path for some consumers only, so it is public
# here; the header compiles only under KODE_SYS_INTERNAL_ALLOWED, which kode_sys
# and kode define for their own sources, and tools/check_layers.sh keeps it
# to those two.
set(srcs "src/kode_sys.c"
         "src/kode_sys_time.c"
         "src/kode_sys_led.c"
         "src/kode_sys_display.c"
         "src/kode_sys_buttons.c"
         "src/kode_sys_ir.c"
         "src/kode_sys_audio.c"
         "src/kode_sys_haptic.c"
         "src/kode_sys_imu.c"
         "src/kode_sys_rfid.c"
         "src/kode_sys_nvs.c"
         "src/kode_sys_files.c"
         "src/kode_sys_flash.c"
         "src/kode_sys_net.c"
         "src/kode_sys_mqtt.c"
         "src/kode_sys_wifi.c"
         "src/kode_sys_wifi_status.c"
         "src/kode_sys_wifi_scan.c"
         "src/kode_sys_scan.c"
         "src/kode_sys_ble.c"
         "src/kode_sys_power.c"
         "src/kode_sys_exp.c"
         "src/kode_sys_exp_power.c"
         "src/kode_sys_home.c"
         "src/kode_sys_touch.c"
         "src/kode_sys_console.c"
         "src/kode_gate.c"
         "src/kode_cert_rfid.c" "src/kode_banner.c" "src/kode_banner_text.c")
if(CONFIG_KODEOS_NFC)
    # The NFC doors. Off, the reader is the app's, and a call to them fails to link.
    list(APPEND srcs "src/kode_sys_nfc.c" "src/kode_cert_nfc.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include" "internal/include"
    REQUIRES kaos kqp kdefs klog kdrv khal
    PRIV_REQUIRES kbsp
)

# kode_sys_internal.h compiles only where this is defined: here, privately.
target_compile_definitions(${COMPONENT_LIB} PRIVATE KODE_SYS_INTERNAL_ALLOWED)

# The SDK's version, taken from git. The target runs on every build and
# rewrites the header only when the string changed; the component library
# waits for it, so the header exists before kode_sys's sources compile.
# Not in early expansion, which reads this file only for the requirements.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(sdk_version_header "${CMAKE_CURRENT_BINARY_DIR}/kode_sdk_version.h")
    add_custom_target(kode_sdk_version ALL
        COMMAND "${CMAKE_COMMAND}"
                -DSDK_ROOT=${CMAKE_CURRENT_LIST_DIR}/../..
                -DLOCK_FILE=${CMAKE_SOURCE_DIR}/dependencies.lock
                -DOUT=${sdk_version_header}
                -P "${CMAKE_CURRENT_LIST_DIR}/version.cmake"
        BYPRODUCTS "${sdk_version_header}"
        COMMENT "Reading the SDK version"
        VERBATIM)
    add_dependencies(${COMPONENT_LIB} kode_sdk_version)
    target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endif()
