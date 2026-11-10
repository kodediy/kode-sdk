# The suite is one component: the runner, the fakes, the cases, and the real
# SDK sources under test.
#
# The cases are globbed and the sources under test are listed, on purpose.
# Adding a case is adding a file; pulling a production source into the host
# build is a decision about which fakes it needs and which layer it drags in,
# and a decision should cost a line somebody reads.
set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../..")

set(SOURCES_UNDER_TEST
    "${SDK_DIR}/components/kdrv/apps/src/kapp_cat.c"
    "${SDK_DIR}/components/kdrv/apps/src/kapp_desc.c"
    "${SDK_DIR}/components/kdrv/apps/src/kapp_data.c"
    "${SDK_DIR}/components/khal/crc32/src/khal_crc32_esp.c"
    "${SDK_DIR}/components/khal/zlib/src/khal_zlib_esp.c"
    "${SDK_DIR}/components/khal/exp/src/khal_exp.c"
    "${SDK_DIR}/components/khal/identity/src/khal_identity_esp.c"
    "${SDK_DIR}/components/kdrv/led/src/kdrv_led.c"
    "${SDK_DIR}/components/kdrv/power/src/kdrv_bq25896.c"
    "${SDK_DIR}/components/kdrv/power/src/kdrv_cw2217.c"
    "${SDK_DIR}/components/kdrv/tca6408a/src/kdrv_tca6408a.c"
    "${SDK_DIR}/components/kdrv/motion/src/kdrv_qmi8658.c"
    "${SDK_DIR}/components/kdrv/motion/src/kdrv_qmc5883p.c"
    "${SDK_DIR}/components/kdrv/cst820/src/kdrv_cst820.c"
    "${SDK_DIR}/components/kdrv/clocks/src/kdrv_clocks.c"
    "${SDK_DIR}/components/kdrv/buttons/src/kdrv_buttons.c"
    "${SDK_DIR}/components/kdrv/radio/src/kdrv_wifi.c"
    "${SDK_DIR}/components/kdrv/radio/src/kdrv_ble_tokens.c"
    "${SDK_DIR}/components/kdrv/radio/src/kdrv_ble_adv.c"
    "${SDK_DIR}/components/kdrv/radio/src/kdrv_ble_app.c"
    "${SDK_DIR}/components/kdrv/radio/src/kdrv_ble_find.c"
    "${SDK_DIR}/components/kdrv/radio/src/kdrv_radio_note.c"
    "${SDK_DIR}/components/kdrv/radio/src/kdrv_radio_sleep_policy.c"
    "${SDK_DIR}/components/kdrv/storage/src/kdrv_storage.c"
    "${SDK_DIR}/components/kdrv/storage/src/kdrv_sd.c"
    "${SDK_DIR}/components/kaos/led/src/led_mgr.c"
    "${SDK_DIR}/components/kode_sys/src/kode_sys.c"
    "${SDK_DIR}/components/kode_sys/src/kode_sys_time.c"
    "${SDK_DIR}/components/kode_sys/src/kode_sys_led.c"
    "${SDK_DIR}/components/kode_sys/src/kode_sys_buttons.c"
    "${SDK_DIR}/components/kode_sys/src/kode_sys_ir.c"
    "${SDK_DIR}/components/kode_sys/src/kode_sys_audio.c"
    "${SDK_DIR}/components/kode_sys/src/kode_sys_haptic.c"
    "${SDK_DIR}/components/kode_sys/src/kode_sys_display.c"
    "${SDK_DIR}/components/kode_sys/src/kode_sys_imu.c"
    "${SDK_DIR}/components/kode_sys/src/kode_sys_nfc.c"
    "${SDK_DIR}/components/kode_sys/src/kode_sys_rfid.c"
    "${SDK_DIR}/components/kode_sys/src/kode_sys_nvs.c"
    "${SDK_DIR}/components/kode_sys/src/kode_sys_files.c"
    "${SDK_DIR}/components/kode_sys/src/kode_sys_flash.c"
    "${SDK_DIR}/components/kode_sys/src/kode_sys_net.c"
    "${SDK_DIR}/components/kode_sys/src/kode_sys_mqtt.c"
    "${SDK_DIR}/components/kode_sys/src/kode_sys_wifi.c"
    "${SDK_DIR}/components/kode_sys/src/kode_sys_wifi_status.c"
    "${SDK_DIR}/components/kode_sys/src/kode_sys_wifi_scan.c"
    "${SDK_DIR}/components/kode_sys/src/kode_sys_scan.c"
    "${SDK_DIR}/components/kode_sys/src/kode_sys_ble.c"
    "${SDK_DIR}/components/kode_sys/src/kode_sys_power.c"
    "${SDK_DIR}/components/kode_sys/src/kode_sys_exp.c"
    "${SDK_DIR}/components/kode_sys/src/kode_sys_exp_power.c"
    "${SDK_DIR}/components/kode_sys/src/kode_sys_home.c"
    "${SDK_DIR}/components/kode_sys/src/kode_sys_touch.c"
    "${SDK_DIR}/components/kode_sys/src/kode_sys_console.c"
    "${SDK_DIR}/components/kode_sys/src/kode_cert_nfc.c"
    "${SDK_DIR}/components/kode_sys/src/kode_cert_rfid.c"
    "${SDK_DIR}/components/kode_sys/src/kode_banner.c"
    "${SDK_DIR}/components/kode_sys/src/kode_banner_text.c"
    "${SDK_DIR}/components/kode_sys/src/kode_gate.c"
    "${SDK_DIR}/components/khal/sync/src/khal_once.c"
    "${SDK_DIR}/components/khal/time/src/khal_time_zone.c"
    "${SDK_DIR}/components/kode/src/kode_audio.c"
    "${SDK_DIR}/components/kode/src/kode_battery.c"
    "${SDK_DIR}/components/kode/src/kode_bus.c"
    "${SDK_DIR}/components/kode/src/kode_button.c"
    "${SDK_DIR}/components/kode/src/kode_entry.c"
    "${SDK_DIR}/components/kode/src/kode_event.c"
    "${SDK_DIR}/components/kode/src/kode_file.c"
    "${SDK_DIR}/components/kode/src/kode_haptic.c"
    "${SDK_DIR}/components/kode/src/kode_http.c"
    "${SDK_DIR}/components/kode/src/kode_ir.c"
    "${SDK_DIR}/components/kode/src/kode_ir_capture.c"
    "${SDK_DIR}/components/kode/src/kode_ir_file.c"
    "${SDK_DIR}/components/kode/src/kode_led.c"
    "${SDK_DIR}/components/kode/src/kode_motion.c"
    "${SDK_DIR}/components/kode/src/kode_mqtt.c"
    "${SDK_DIR}/components/kode/src/kode_net.c"
    "${SDK_DIR}/components/kode/src/kode_nfc.c"
    "${SDK_DIR}/components/kode/src/kode_pin.c"
    "${SDK_DIR}/components/kode/src/kode_pin_power.c"
    "${SDK_DIR}/components/kode/src/kode_rfid.c"
    "${SDK_DIR}/components/kode/src/kode_screen_bright.c"
    "${SDK_DIR}/components/kode/src/kode_setting.c"
    "${SDK_DIR}/components/kode/src/kode_standard_ao.c"
    "${SDK_DIR}/components/kode/src/kode_system.c"
    "${SDK_DIR}/components/kode/src/kode_tag.c"
    "${SDK_DIR}/components/kode/src/kode_touch.c"
    "${SDK_DIR}/components/kode/src/kode_wifi.c"
    "${SDK_DIR}/components/kode/src/kode_ble.c"
    # kode_screen's side of the screen calls, which posts and draws and holds
    # no LVGL; the LVGL side (kode_screen.c) is test_kode_screen.c's fake.
    "${SDK_DIR}/components/kode_screen/src/kode_widget.c"
    "${SDK_DIR}/components/kode_screen/src/kode_canvas.c"
    "${SDK_DIR}/components/kode_screen/src/kode_png.c"
    "${SDK_DIR}/components/kode_screen/src/kode_compass.c"
    "${SDK_DIR}/components/kqp/src/kqp_isr.c"
    "${SDK_DIR}/components/kqp/src/kqp_self.c"
    "${SDK_DIR}/components/kqp/src/kqp_timer.c"
    "${SDK_DIR}/components/kqp/src/kqp_tick.c"
    "${SDK_DIR}/components/kqp/src/kqp_worker.c"
    "${SDK_DIR}/components/kqp/src/kqp_watch_judge.c"
    "${SDK_DIR}/components/kaos/boot/src/boot_ao.c"
    "${SDK_DIR}/components/kaos/boot/src/boot_zone.c"
    "${SDK_DIR}/components/kaos/log/src/log_ao.c"
    "${SDK_DIR}/components/kaos/power/src/power_ao.c"
    "${SDK_DIR}/components/kaos/touch/src/touch_ao.c"
    "${SDK_DIR}/components/kaos/touch/src/touch_record.c"
    "${SDK_DIR}/components/kaos/buttons/src/buttons_ao.c"
    "${SDK_DIR}/components/kaos/ioexp/src/ioexp_ao.c"
    "${SDK_DIR}/components/kaos/ir/src/ir_ao.c"
    "${SDK_DIR}/components/kaos/nfc/src/nfc_ao.c"
    "${SDK_DIR}/components/kaos/rfid/src/rfid_ao.c"
    "${SDK_DIR}/components/kaos/audio/src/audio_ao.c"
    "${SDK_DIR}/components/kaos/imu/src/imu_ao.c"
    "${SDK_DIR}/components/kaos/haptic/src/haptic_ao.c"
    "${SDK_DIR}/components/kaos/lowpower/src/lowpower_ao.c"
    "${SDK_DIR}/components/kaos/lowpower/src/lowpower_last.c"
    "${SDK_DIR}/components/kaos/display/src/display_ao.c"
    "${SDK_DIR}/components/kaos/display/src/display_clock.c"
    "${SDK_DIR}/components/kaos/storage/src/storage_ao.c"
    "${SDK_DIR}/components/kaos/net/src/net_ao.c"
    "${SDK_DIR}/components/kaos/radio/src/radio_ao.c"
    "${SDK_DIR}/components/kdrv/radio/src/kdrv_c5.c"
    "${SDK_DIR}/components/kdrv/radio/src/kdrv_c5svc.c"
    "${SDK_DIR}/components/kdrv/radio/src/kdrv_frames.c"
    "${SDK_DIR}/components/kaos/appmgr/src/appmgr_ao.c"
    "${SDK_DIR}/components/kaos/autotest/src/autotest_ao.c"
    "${SDK_DIR}/components/kaos/autotest/src/autotest_psram.c"
    "${SDK_DIR}/components/kaos/scheduler/src/sched_cron.c"
    "${SDK_DIR}/components/kaos/scheduler/src/scheduler_ao.c"
    "${SDK_DIR}/components/kaos/console/src/console_ao.c"
    "${SDK_DIR}/components/kaos/console/src/console_fs.c"
    "${SDK_DIR}/components/kaos/console/src/console_grammar.c"
    "${SDK_DIR}/components/kaos/core/src/kverbs.c"
    "${SDK_DIR}/components/kaos/core/src/krows.c"
    "${SDK_DIR}/components/kaos/led/src/led_verbs.c"
    "${SDK_DIR}/components/kaos/touch/src/touch_verbs.c"
    "${SDK_DIR}/components/kaos/audio/src/audio_verbs.c"
    "${SDK_DIR}/components/kaos/haptic/src/haptic_verbs.c"
    "${SDK_DIR}/components/kaos/ir/src/ir_verbs.c"
    "${SDK_DIR}/components/kaos/nfc/src/nfc_verbs.c"
    "${SDK_DIR}/components/kaos/rfid/src/rfid_verbs.c"
    "${SDK_DIR}/components/kaos/display/src/display_verbs.c"
    "${SDK_DIR}/components/kaos/radio/src/radio_verbs.c"
    "${SDK_DIR}/components/kaos/net/src/net_verbs.c"
    "${SDK_DIR}/components/kaos/storage/src/storage_verbs.c"
    "${SDK_DIR}/components/kaos/appmgr/src/appmgr_verbs.c"
    "${SDK_DIR}/components/kaos/lowpower/src/lowpower_verbs.c"
    "${SDK_DIR}/components/kaos/scheduler/src/scheduler_verbs.c"
    "${SDK_DIR}/components/kaos/net/src/net_channel.c"
    "${SDK_DIR}/components/kaos/core/src/kregistry.c"
    "${SDK_DIR}/components/kaos/core/src/kode_config.c"
    "${SDK_DIR}/components/kaos/core/src/kaos_claim.c"
    "${SDK_DIR}/components/kaos/core/src/kaos_park.c"
    "${SDK_DIR}/components/kaos/core/src/kaos_ask.c"
    "${SDK_DIR}/components/kbsp/owners/src/kbsp_owners.c"
    "${SDK_DIR}/components/kbsp/exp/src/kbsp_exp.c"
    "${SDK_DIR}/components/kbsp/exp/src/kbsp_station.c"
    "${SDK_DIR}/components/kdrv/station/src/kdrv_pcf8574.c"
    "${SDK_DIR}/components/kdrv/station/src/kdrv_ads1115.c"
    # AO_TestDot, the E5 final test, with its screen analysis: the camera and
    # the pads loop are fakes/fake_station.c, J3's I2C transfers are mocked,
    # and the worker's encodes (test_dot_jpeg.c, a wait on AO_Display) are
    # test_dot_support.c's.
    "${SDK_DIR}/components/kaos/test_dot/src/test_dot_ao.c"
    "${SDK_DIR}/components/kaos/test_dot/src/test_dot_worker.c"
    "${SDK_DIR}/components/kaos/test_dot/src/test_dot_cam.c"
    "${SDK_DIR}/components/kaos/test_dot/src/test_dot_verbs.c"
    "${SDK_DIR}/components/kaos/test_dot/analysis/src/tdan_geom.c"
    "${SDK_DIR}/components/kaos/test_dot/analysis/src/tdan_pix.c"
    "${SDK_DIR}/components/kaos/test_dot/analysis/src/tdan_judge.c"
    "${SDK_DIR}/components/klog/src/klog.c"
    "${SDK_DIR}/components/kdrv/apps/src/kdrv_apps.c"
    "${SDK_DIR}/components/kdrv/apps/src/kdrv_kfw.c"
    "${SDK_DIR}/components/kdrv/mirror/src/kmirror_proto.c"
    "${SDK_DIR}/components/kdrv/ir/src/knec.c"
    "${SDK_DIR}/components/kdrv/audio/src/kes_fields.c"
    "${SDK_DIR}/components/kdrv/audio/src/kring.c"
    "${SDK_DIR}/components/kdrv/audio/src/ktone.c"
    "${SDK_DIR}/components/kdrv/audio/src/kwav.c"
    "${SDK_DIR}/components/kdrv/motion/src/kimu_energy.c"
    "${SDK_DIR}/components/kdrv/motion/src/kcompass.c"
    "${SDK_DIR}/components/kdrv/motion/src/kdrv_aw86233.c"
    "${SDK_DIR}/components/kdrv/nfc/src/knfc_crypto1.c"
    "${SDK_DIR}/components/kdrv/nfc/src/knfc_emu_img.c"
    "${SDK_DIR}/components/kdrv/nfc/src/knfc_field.c"
    "${SDK_DIR}/components/kdrv/nfc/src/knfc_mfc.c"
    "${SDK_DIR}/components/kdrv/nfc/src/knfc_mfc_dict.c"
    "${SDK_DIR}/components/kdrv/nfc/src/knfc_ndef.c"
    "${SDK_DIR}/components/kdrv/nfc/src/knfc_power.c"
    "${SDK_DIR}/components/kdrv/nfc/src/knfc_ident.c"
    "${SDK_DIR}/components/kdrv/nfc/src/knfc_tag.c"
    "${SDK_DIR}/components/kdrv/rfid/src/krfid_em4100.c"
    "${SDK_DIR}/components/kdrv/rfid/src/krfid_env.c"
    "${SDK_DIR}/components/kdrv/rfid/src/krfid_mod.c"
    "${SDK_DIR}/components/kdrv/rfid/src/krfid_wave.c"
    "${SDK_DIR}/components/kui/drag/src/kui_gesture.c"
    "${SDK_DIR}/components/kui/drag/src/kui_spring.c"
    "${SDK_DIR}/components/kui/drag/src/kui_drag.c"
    "${SDK_DIR}/components/kui/glass/src/kui_glass.c"
    "${SDK_DIR}/components/kui/kody/src/kui_kody_anchor.c"
    "${SDK_DIR}/components/kui/multitap/src/kui_multitap.c"
    "${SDK_DIR}/components/kui/pack/src/kui_pack.c"
)

# No CONFIGURE_DEPENDS: ESP-IDF evaluates this file a second time in CMake's
# script mode while it scans component requirements, and the option is invalid
# there. So a new case file does not reconfigure the build by itself -
# tools/host_tests.sh watches the file list and reconfigures when it changes.
file(GLOB CASE_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../cases/test_*.c")
# Sorted, and that is not tidiness. A case registers itself from a constructor,
# so the order cases run in is the order their objects reach the linker, which
# is the order this glob returns them - and a glob returns what the filesystem
# hands it. APFS and the CI image's filesystem hand it different orders, so an
# unsorted list means the bench and CI run the same suite in two different
# orders and disagree about it. Lexicographic here, everywhere.
list(SORT CASE_SOURCES)

file(GLOB FAKE_SOURCES "${CMAKE_CURRENT_LIST_DIR}/fakes/fake_*.c")
list(SORT FAKE_SOURCES)

file(GLOB khal_includes LIST_DIRECTORIES true "${SDK_DIR}/components/khal/*/include")

# WHOLE_ARCHIVE is the difference between a suite and a lie. A case file that
# nothing references is dropped from the link, and Unity then reports OK over
# the cases that happened to survive.
idf_component_register(
    SRCS "runner.c"
         "qf_support.c"
         "ao_spy.c"
         "ask_support.c"
         "config_support.c"
         "motion_support.c"
         "standard_ao_support.c"
         "autotest_support.c"
         "owners_support.c"
         "probe_ao.c"
         "test_dot_support.c"
         ${CASE_SOURCES}
         ${FAKE_SOURCES}
         ${SOURCES_UNDER_TEST}
    INCLUDE_DIRS "."
                 "fakes"
                 # Ahead of esp_rom's, so shim/miniz.h wins the lookup and
                 # pulls the real one in with #include_next. See that file for
                 # why a warning flag could not do this job on both compilers.
                 "shim"
                 "${SDK_DIR}/components/kdrv/audio/include"
                 "${SDK_DIR}/components/kdrv/buttons/include"
                 "${SDK_DIR}/components/kdrv/clocks/include"
                 "${SDK_DIR}/components/kdrv/cst820/include"
                 "${SDK_DIR}/components/kdrv/display/include"
                 "${SDK_DIR}/components/kdrv/exp_bus/include"
                 "${SDK_DIR}/components/kdrv/led/include"
                 "${SDK_DIR}/components/kdrv/tca6408a/include"
                 "${SDK_DIR}/components/kdrv/audio/include"
                 "${SDK_DIR}/components/kdrv/ir/include"
                 "${SDK_DIR}/components/kdrv/nfc/include"
                 "${SDK_DIR}/components/kdrv/motion/include"
                 "${SDK_DIR}/components/kdrv/power/include"
                 "${SDK_DIR}/components/kdrv/storage/include"
                 "${SDK_DIR}/components/kdrv/apps/include"
                 "${SDK_DIR}/components/kdrv/apps/src"
                 "${SDK_DIR}/components/kdrv/mirror/include"
                 "${SDK_DIR}/components/kdrv/radio/include"
                 # kdrv_radio_note.h and kdrv_radio_sleep.h are private to the driver
                 "${SDK_DIR}/components/kdrv/radio/src"
                 "${SDK_DIR}/components/kdrv/rfid/include"
                 "${SDK_DIR}/components/kdrv/station/include"
                 "${SDK_DIR}/components/kdrv/ov5640/include"
                 "${SDK_DIR}/components/kaos/test_dot/src"
                 "${SDK_DIR}/components/kaos/test_dot/analysis/include"
                 "${SDK_DIR}/components/kaos/test_dot/analysis/src"
                 ${khal_includes}
                 "${SDK_DIR}/components/kdefs/include"
                 "${SDK_DIR}/components/klog/include"
                 "${SDK_DIR}/components/kbsp/board/include"
                 "${SDK_DIR}/components/kbsp/owners/include"
                 "${SDK_DIR}/components/kbsp/exp/include"
                 "${SDK_DIR}/components/kui/core/include"
                 "${SDK_DIR}/components/kui/drag/include"
                 "${SDK_DIR}/components/kui/glass/include"
                 "${SDK_DIR}/components/kui/kody/include"
                 "${SDK_DIR}/components/kui/multitap/include"
                 "${SDK_DIR}/components/kui/pack/include"
                 "${SDK_DIR}/components/kui/panel/include"
                 "${SDK_DIR}/components/kui/core/src"
                 "${SDK_DIR}/components/kaos/core/include"
                 "${SDK_DIR}/components/kode_sys/include"
                 "${SDK_DIR}/components/kode_sys/internal/include"
                 "${SDK_DIR}/components/kode/include"
                 # kode_entry.h is private to the component
                 "${SDK_DIR}/components/kode/src"
                 "${SDK_DIR}/components/kode/internal/include"
                 # kode_screen_priv.h is private to the component
                 "${SDK_DIR}/components/kode_screen/src"
                 # kode_banner.h and kode_sys_priv.h are private to the component
                 "${SDK_DIR}/components/kode_sys/src"
                 # console_fs.h is private to the component
                 "${SDK_DIR}/components/kaos/console/src"
                 "${SDK_DIR}/components/kaos/core/src"
                 "${SDK_DIR}/components/kaos/autotest/src"
                 "${SDK_DIR}/components/kaos/net/src"
                 "${SDK_DIR}/components/kaos/scheduler/src"
                 "${SDK_DIR}/components/kaos/display/src"
                 "${SDK_DIR}/components/kqp/include"
                 # kqp_watch_judge.h is private to the component
                 "${SDK_DIR}/components/kqp/src"
                 # bootloader_support does not build for the host, but
                 # kdrv_apps.c needs esp_image_format.h to compile.
                 "$ENV{IDF_PATH}/components/bootloader_support/include"
    REQUIRES unity esp_timer mbedtls kdrv_slot kdrv_net kdrv_kfw kdrv_display kdrv_jpeg kdrv_ppa kdrv_ir kdrv_st25r3916 kdrv_es8311 kdrv_exp_bus kdrv_station_i2c esp_app_format esp_rom khal_i2c khal_gpio khal_exp_port khal_pwm khal_clk khal_power qpc esp_partition
             esp_wifi esp_netif esp_event
    WHOLE_ARCHIVE)

# The linux FreeRTOS port does not declare xPortCanYield(); only the Xtensa and
# RISC-V ones do. klog reads it to tell task context from an ISR, so the host
# build needs a stand-in - and a controllable one, because that branch is
# otherwise reachable only from a real interrupt.
#
# Force-included rather than #included by hand: the file that needs it is
# klog.c, a production source this suite compiles and must not edit. A
# function-like -D was the first attempt and does not survive CMake's quoting.
target_compile_options(${COMPONENT_LIB} PRIVATE
    "-include" "${CMAKE_CURRENT_LIST_DIR}/fakes/host_port_shim.h")

# The audio, IMU and haptic sources call sinf, cosf, sqrtf, log10f and lrintf.
# macOS folds libm into libSystem; on Linux it has to be named, or the suite
# compiles and then fails to link.
target_link_libraries(${COMPONENT_LIB} PRIVATE m)

# kaos and kdrv arrive here as loose sources rather than as components with
# their Kconfig, so no CONFIG_KODEOS_* symbol exists unless it is spelled out.
# That is a trap rather than a detail: a `#if CONFIG_KODEOS_APPMGR` block
# compiles out **silently**, the cases around it still run and still pass, and
# what they assert about is simply absent. The declared-versus-ran check
# cannot see it either. test_kode_flash.c asserts the symbol is here, so the
# guard is a case rather than a comment.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    # kode_sys, kode and kode_screen's own sources are compiled here, and the
    # cases that drive the blocking calls include their header.
    "KODE_SYS_INTERNAL_ALLOWED"
    "KODE_INTERNAL_ALLOWED"
    "CONFIG_KODEOS_APPMGR=1"
    # AO_Nfc, its doors and kode_init()'s two NFC rows. On in every build but
    # an app that drives the reader itself; test_kode_init_rows.c asserts it.
    "CONFIG_KODEOS_NFC=1"
    # The self-test rows are compiled under it, owners' and AO_Autotest's
    # alike: without it no row would exist and every autotest case would
    # assert about a refusal.
    "CONFIG_KODEOS_AUTOTEST=1"
    # AO_LowPower and the signals, events and facade it adds. Off in the SDK
    # and on in kodeOS; on here, because off is the facade alone and
    # test_lowpower_ao.c asserts that the symbol is here rather than let its
    # cases compile out and pass over nothing.
    "CONFIG_KODE_LOWPOWER=1"
    # Its idle count and `sleep_after`, on in the SDK and off in kodeOS: the
    # idle cases test the count.
    "CONFIG_KODE_LOWPOWER_IDLE=1"
    # AO_Scheduler, off in the SDK and on in kodeOS, spelled out for the same
    # reason: without it test_scheduler_ao.c would compile out and pass.
    "CONFIG_KODE_SCHEDULER=1"
    # The console is compiled here as a loose source, so this symbol is what
    # the registry reads to know there is a console to reach the table at
    # all; without it every kode_variables_add() and kode_functions_add()
    # refuses, and every case in test_kregistry.c, test_console_variables.c and
    # test_console_functions.c asserts about a refusal.
    # test_kregistry.c guards it the way test_kode_flash.c guards APPMGR.
    "CONFIG_KODEOS_CONSOLE=1"
    "CONFIG_KODEOS_BOOT_ROLE_OS=1"
    # The app the factory reset keeps and runs afterwards. The role flag above
    # gates the code that reads it, and Kconfig's default is this same word.
    "CONFIG_KODEOS_WELCOME_APP=\"welcome\""
    "CONFIG_KODEOS_NET=1"
    "CONFIG_KODEOS_MQTT=1"
    # Not optional here any more, and this is exactly the trap the paragraph
    # above describes: KODEOS_MIRROR now gates display_ao.c's whole state
    # machine, not just console_ao.c's four verbs, so without this every case
    # in test_display_mirror.c would fail to LINK rather than fail quietly -
    # which is the good outcome, but only because the symbol is spelled out
    # here. It also puts the four mirror verbs into the console's own command
    # table, where test_console_parse.c can see them.
    "CONFIG_KODEOS_MIRROR=1"
    # The home hold, and the same trap once more. On the board it depends on
    # the APP role - kodeOS is home already - and the role spelled out above
    # is the OS one, because the initial transition's interesting half is
    # there. Both together is what lets one binary drive both: the C gates the
    # grace on this symbol alone and leaves the role to Kconfig. Without the
    # line the grace, the two facade calls and the restart compile out
    # silently; test_boot_ao.c asserts the symbol is here.
    "CONFIG_KODEOS_HOME_HOLD=1"
    # AO_Radio and the C5's field-table driver are real here; the link's
    # worker (kdrv_radio.c) and esp-hosted's host API are the two fakes under
    # them. WiFi is on because the C5 row scans, and kdrv_wifi.c is already
    # real over IDF's own esp_wifi mock. BLE is on because the row starts the
    # stack and listens; kdrv_ble.c cannot build here (NimBLE, esp-hosted),
    # so fakes/fake_kdrv_ble.c is the whole of its header. KODEOS_BLE_CONSOLE
    # stays scoped to display_ao.c below.
    "CONFIG_KODEOS_RADIO=1"
    # On in kodeOS and off in the SDK. On here, because off is the code as it
    # was before the option existed and on is everything it added: AO_Radio
    # holding the C5 at boot, and AO_Audio waiting for it before a first
    # sound.
    "CONFIG_KODEOS_C5_HOLD=1"
    "CONFIG_KODEOS_WIFI=1"
    "CONFIG_KODEOS_WIFI_AUTOJOIN=1"
    "CONFIG_KODEOS_BLE=1"
    # On the board this is "y if KODEOS_GUI", and there is no GUI here - so
    # without this line AO_Radio's pairing arms would compile out in exactly
    # the silent way the paragraph above warns about, and the cases that
    # assert about them would pass having tested nothing. It also switches on
    # the pairing half of fakes/fake_kdrv_ble.c, which is guarded by the same
    # symbol because the header it stands in for is.
    "CONFIG_KODEOS_BLE_PAIRING_PASSKEY=1")

# Scoped to display_ao.c alone, not the whole project: the real
# CONFIG_KODEOS_BLE_CONSOLE also gates a great deal of console_ao.c and
# log_ao.c that has no fake here and is not this suite's concern. display_
# ao.c's own two calls into kdrv_ble.h - kdrv_ble_mirror_attached() and
# kdrv_ble_mirror_write() - are the only ones test_display_mirror.c needs,
# and the mirror half of fakes/fake_kdrv_ble.c is the whole seam; see that
# file and fakes.h's comment on it.
set_source_files_properties(
    "${SDK_DIR}/components/kaos/display/src/display_ao.c"
    PROPERTIES COMPILE_DEFINITIONS "CONFIG_KODEOS_BLE_CONSOLE=1")

# And to kdrv_ble_app.c, the app's own service's rules: the service rides
# beside the console's, so without the symbol every table would be judged
# left out and the cases in test_kdrv_ble_app_table.c would assert about that
# one refusal.
set_source_files_properties(
    "${SDK_DIR}/components/kdrv/radio/src/kdrv_ble_app.c"
    PROPERTIES COMPILE_DEFINITIONS "CONFIG_KODEOS_BLE_CONSOLE=1")

# The standard layer's Active Object is built for the APP role only, and the
# project-wide role above is the OS one. This source is the app build, so the
# cases can start the real Active Object and the dispatch task; the OS build,
# which only refuses, is compiled again by test_kode_event_os.c under its own
# name. Undefined rather than set to 0, so the compiler has nothing to say
# about a redefinition.
set_source_files_properties(
    "${SDK_DIR}/components/kode/src/kode_standard_ao.c"
    PROPERTIES COMPILE_OPTIONS "-UCONFIG_KODEOS_BOOT_ROLE_OS")

# The Kode channel is AO_Net's, and only AO_Net's: the option is scoped to the
# AO and its case file the way the cert carriers below are. The renewal is 120 s
# here instead of 23 h, which keeps a whole renewal inside a few thousand ticks
# and still leaves a window past the minute a session must stay up to reset
# the back-off. The case file refuses to build without both, for the reason
# every symbol here is spelled out: a channel that compiles out leaves the
# cases around it green and asserting about nothing. The screen upload rides
# on the channel and is scoped the same way (it also needs KODEOS_MIRROR,
# which is on for the whole project above).
set_source_files_properties(
    "${SDK_DIR}/components/kaos/net/src/net_ao.c"
    "${CMAKE_CURRENT_LIST_DIR}/../cases/test_net_ao.c"
    PROPERTIES COMPILE_DEFINITIONS "CONFIG_KODEOS_CHANNEL=1;CONFIG_KODEOS_CHANNEL_RENEW_S=120;CONFIG_KODEOS_SHOT_HTTP=1")

# `screenshot dst=http` is the console's half of the upload and is scoped the
# same way: on the board the option implies the channel and the mirror, which
# are on or off for the whole project here, so the console and its case file
# take the one symbol. Without it `dst=http` would not exist in the enum and
# the cases around it would assert about a refusal of the wrong kind.
set_source_files_properties(
    "${SDK_DIR}/components/kaos/console/src/console_ao.c"
    "${CMAKE_CURRENT_LIST_DIR}/../cases/test_console_net.c"
    PROPERTIES COMPILE_DEFINITIONS "CONFIG_KODEOS_SHOT_HTTP=1")

# A line off the channel ends by telling AO_Net, and only a console built with
# the channel says so: the option is the console's alone here, since the cases
# of test_console_cron.c read AO_Net's mailbox for that end.
set_property(SOURCE
    "${SDK_DIR}/components/kaos/console/src/console_ao.c"
    APPEND PROPERTY COMPILE_DEFINITIONS "CONFIG_KODEOS_CHANNEL=1")

# AO_Log hands the replies of the channel's commands to the ring. Without the
# option its case file would assert about a push that was compiled out.
set_source_files_properties(
    "${SDK_DIR}/components/kaos/log/src/log_ao.c"
    "${CMAKE_CURRENT_LIST_DIR}/../cases/test_log_ao.c"
    PROPERTIES COMPILE_DEFINITIONS "CONFIG_KODEOS_CHANNEL=1")

# Scoped to the two front ends' AOs and their case files the same way. The
# real CONFIG_KODEOS_NFC_CERT builds the certification app's maximum-drive
# carriers, and nothing else here has any business seeing them. The last case
# in test_nfc_ao.c asks for the 13.56 MHz carrier while a listen window is
# open, and one in test_rfid_ao.c asks for the 125 kHz one while a capture
# holds the front end - the places they meet - so each pair needs the symbol:
# the case file for the *_cert_request() declaration in kaos.h, the AO for the
# arm that answers it. Each case file refuses to compile without it rather
# than let its case vanish.
set_source_files_properties(
    "${SDK_DIR}/components/kaos/nfc/src/nfc_ao.c"
    "${SDK_DIR}/components/kaos/rfid/src/rfid_ao.c"
    "${SDK_DIR}/components/kode_sys/src/kode_cert_nfc.c"
    "${SDK_DIR}/components/kode_sys/src/kode_cert_rfid.c"
    "${CMAKE_CURRENT_LIST_DIR}/../cases/test_nfc_ao.c"
    "${CMAKE_CURRENT_LIST_DIR}/../cases/test_rfid_ao.c"
    PROPERTIES COMPILE_DEFINITIONS "CONFIG_KODEOS_NFC_CERT=1")

# The E5 final test is the station's, and only the station's: the option is
# scoped to AO_TestDot, the console that hands it `test component=all` and
# refuses factory_reset during a run, the support that brings the AO up and
# the case files. Project-wide it would take KODE_PRIO_USER_LOW, the level
# ao_spy.c stands on. The case files refuse to build without it.
set_property(SOURCE
    "${SDK_DIR}/components/kaos/console/src/console_ao.c"
    "${SDK_DIR}/components/kaos/test_dot/src/test_dot_ao.c"
    "${SDK_DIR}/components/kaos/test_dot/src/test_dot_worker.c"
    "${SDK_DIR}/components/kaos/test_dot/src/test_dot_cam.c"
    "${SDK_DIR}/components/kaos/test_dot/src/test_dot_verbs.c"
    "${CMAKE_CURRENT_LIST_DIR}/test_dot_support.c"
    "${CMAKE_CURRENT_LIST_DIR}/../cases/test_test_dot_ao.c"
    "${CMAKE_CURRENT_LIST_DIR}/../cases/test_test_dot_run.c"
    "${CMAKE_CURRENT_LIST_DIR}/../cases/test_console_test_dot.c"
    APPEND PROPERTY COMPILE_DEFINITIONS "CONFIG_KODE_TEST_DOT=1")

# Sanitizers, only when KODE_SANITIZE is set; see tests/sanitize.cmake.
include(${CMAKE_CURRENT_LIST_DIR}/../../sanitize.cmake)
