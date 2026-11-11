# kdrv - one file per chip, and the only layer that knows a part number.
#
# Where a component from the registry exists it is wrapped, not reimplemented:
# the CO5300 comes from idf_component.yml. What stays ours is the part no
# component has - the bring-up sequence, the KTD2026's, the TCA6408A's and
# the CST820's register maps (the expander and the touch controller had
# registry components once; a field table over khal_i2c is what the console
# and the host suite can reach, and neither chip was worth a dependency for
# what it hid) - plus the two flash regions (the slot and otadata), which
# are devices in every sense that matters here.
#
# Public headers speak kode_status_t. That is what keeps ESP-IDF from leaking
# into kaos and above (kodeOS invariant 1); this component and khal are where
# it stops.
#
# Layout. A single-file driver lives flat: its .c here and its .h in
# include/. A family of more than one file - a driver plus the protocol
# arithmetic above it, or a subsystem of several drivers - lives in a folder
# of its own with its .c and .h together (kdrv_nfc/, kdrv_ir/, kdrv_storage/,
# kdrv_radio/, kdrv_mirror/, kdrv_apps/, kdrv_power/). Every folder is listed
# in INCLUDE_DIRS, so an `#include "knfc_ndef.h"` resolves the same whether
# the header sits in include/ or in a folder - the grouping is on disk only
# and no include path spells it.
set(srcs
    # Single-file drivers.
    "led/src/kdrv_led.c"
    "clocks/src/kdrv_clocks.c"
    "tca6408a/src/kdrv_tca6408a.c"
    "display/src/kdrv_display.c"
    "cst820/src/kdrv_cst820.c"
    "buttons/src/kdrv_buttons.c"

    # kdrv_exp_bus/ - the buses an app runs on J3's pins. It drives only: the
    # positions and the controller are lent through khal_exp's books first.
    "exp_bus/src/kdrv_exp_bus.c"

    # kdrv_ir/ - the infrared block and the NEC codec. Both infrared lines
    # are the ESP32-C5's, so kdrv_ir.c is a client of the service the C5's
    # firmware runs, spoken over the link's peer-data seam; the P4 opens no
    # RMT for it. knec is its own file because the host suite mocks kdrv_ir.h
    # and a header cannot be both mocked and linked.
    "ir/src/kdrv_ir.c"
    "ir/src/knec.c"

    # kdrv_nfc/ - the ST25R3916B and the NFC protocol layers above it. The
    # layers are their own files for knec.c's reason: the host suite mocks
    # kdrv_st25r3916.h to test them without a radio. Everything that touches
    # RFAL is on the driver's side of that line; knfc_* and knfc_crypto1 are
    # protocol arithmetic the host runs.
    "nfc/src/kdrv_st25r3916.c"
    "nfc/src/knfc_crypto1.c"
    "nfc/src/knfc_emu_img.c"
    "nfc/src/knfc_field.c"
    "nfc/src/knfc_mfc.c"
    "nfc/src/knfc_mfc_dict.c"
    "nfc/src/knfc_ndef.c"
    "nfc/src/knfc_power.c"
    "nfc/src/knfc_ident.c"
    "nfc/src/knfc_tag.c"

    # kdrv_audio/ - the ES8311 and everything above it. Three of the four
    # files are separate for knec.c's reason: the host suite mocks
    # kdrv_es8311.h, and the tone, the detector, the WAV header and the field
    # table all have to be compiled under test rather than mocked away. The
    # table is the one that would not be obvious - kdrv_cst820 keeps its own
    # and is compiled whole on the host, because it only ever speaks khal.
    # This codec cannot: driving it means driver/i2s_std.h and a vendor
    # component, so the half that knows the registers is split from the half
    # that knows the peripheral.
    "audio/src/kdrv_es8311.c"
    "audio/src/kes_fields.c"
    "audio/src/kring.c"
    "audio/src/ktone.c"
    "audio/src/kwav.c"

    # kdrv_motion/ - the IMU, the magnetometer, the haptic and the witness
    # arithmetic. kimu_energy is its own file for knec.c's reason: the host
    # suite compiles the numbers with no bus beneath them. The QMI8658A and
    # the QMC5883P are both AO_Imu's, each written from its datasheet and
    # pinned register by register in the host suite.
    "motion/src/kimu_energy.c"
    "motion/src/kcompass.c"
    "motion/src/kdrv_qmi8658.c"
    "motion/src/kdrv_qmc5883p.c"
    "motion/src/kdrv_aw86233.c"

    # kdrv_power/ - the PMIC and the fuel gauge, both AO_Power's.
    "power/src/kdrv_bq25896.c"
    "power/src/kdrv_cw2217.c"

    # kdrv_storage/ - the microSD, the internal FAT, and the file API over them.
    "storage/src/kdrv_storage.c"
    "storage/src/kdrv_sd.c"
    "storage/src/kdrv_fs.c"

    # kdrv_apps/ - the apps region, the OTA slot, the firmware-image format,
    # and app metadata. kapps_fmt.h is the wire format shared verbatim with
    # bootloader_components/ and named in partitions.csv.
    "apps/src/kdrv_slot.c"
    "apps/src/kdrv_slot_crash.c"
    "apps/src/kdrv_apps.c"
    "apps/src/kdrv_kfw.c"
    "apps/src/kapp_desc.c"
    "apps/src/kapp_cat.c"
    "apps/src/kapp_data.c"

    # kdrv_rfid/ - the 125 kHz LF front end. krfid_em4100 is the pure
    # Manchester/EM4100 codec, its own file for knec.c's reason: the host suite
    # compiles it directly, so nothing that touches a peripheral may live in it.
    # krfid_env is the same kind of file: it folds the comparator's pulse
    # widths, as RMT words, into the envelope the codec reads. krfid_wave is
    # the capture's stats and peak-per-bucket decimation, pure C for the same
    # reason. krfid_mod is the tag emulation's Manchester modulator, pure C
    # as well.
    # kdrv_rfid_lf drives the carrier (through khal_pwm), the ADC-continuous
    # read and the T5577 downlink - it is the peripheral half, and has no host test.
    "rfid/src/krfid_em4100.c"
    "rfid/src/krfid_env.c"
    "rfid/src/krfid_mod.c"
    "rfid/src/krfid_wave.c"
    "rfid/src/kdrv_rfid_lf.c"
)

# kdrv_mirror/ - the three files the mirror is made of, compiled with the
# feature that is their only caller. The codec and the scaler are not free to
# carry: between them they hold about 530 KB of PSRAM from boot (128 KB of
# encoder output, a full panel of RGB565 for the scaler), and a build that has
# said no to KODEOS_MIRROR has said no to paying it. kmirror_proto.c holds no
# memory at all and goes with them anyway, because a wire format nobody can
# speak is dead weight in the same sense.
if(CONFIG_KODEOS_MIRROR)
    list(APPEND srcs
        "mirror/src/kdrv_jpeg.c"
        "mirror/src/kdrv_ppa.c"
        "mirror/src/kmirror_proto.c"
    )
endif()

# kdrv_radio/ - the C5 link driver and everything on top of it, compiled out
# with its owner: without AO_Radio nothing may call it, and a driver nobody
# may call is dead weight. (The esp_hosted dependency itself cannot be
# conditional - the component manager resolves dependencies before Kconfig
# values exist - so a radio-less firmware still downloads it and the linker
# drops it.)
if(CONFIG_KODEOS_RADIO)
    list(APPEND srcs "radio/src/kdrv_radio.c")
    # One synchronous RPC at a time: esp-hosted's request path has no lock.
    list(APPEND srcs "radio/src/kdrv_radio_rpc.c")
    # The handover note's layout and the rule that reads it, pure C.
    list(APPEND srcs "radio/src/kdrv_radio_note.c")
    # The sleep sentinel: the rules (pure C) and the calls that carry them out.
    if(CONFIG_KODE_LOWPOWER)
        list(APPEND srcs "radio/src/kdrv_radio_sleep_policy.c")
        list(APPEND srcs "radio/src/kdrv_radio_sleep.c")
    endif()
    # The coprocessor's table: what the link can say about the chip on the
    # other end, refreshed by the worker above. No link, nothing to say.
    list(APPEND srcs "radio/src/kdrv_c5.c")
    # The KODE services on the C5 (vault, identity, BLE keys): kode_wire.h.
    list(APPEND srcs "radio/src/kdrv_c5svc.c")
    # The Wi-Fi frames service's P4 end: the C5's batches decoded and held.
    list(APPEND srcs "radio/src/kdrv_frames.c")
    if(CONFIG_KODEOS_WIFI)
        list(APPEND srcs "radio/src/kdrv_wifi.c")
        # The host's own stack, on top of the address the C5's radio earns.
        # Nested under WIFI rather than beside it because an address is the
        # only way onto a network this board has.
        if(CONFIG_KODEOS_NET)
            list(APPEND srcs "radio/src/kdrv_net.c")
        endif()
    endif()
    if(CONFIG_KODEOS_BLE)
        list(APPEND srcs "radio/src/kdrv_ble.c")
        list(APPEND srcs "radio/src/kdrv_ble_tokens.c")
        # The scan response's packing, pure C.
        list(APPEND srcs "radio/src/kdrv_ble_adv.c")
        # The app's own service: its rules, values and ring, pure C.
        list(APPEND srcs "radio/src/kdrv_ble_app.c")
        # Which advertisement a find waits for, pure C.
        list(APPEND srcs "radio/src/kdrv_ble_find.c")
    endif()
else()
    # No link, but the C5 is still on the board: its reset line alone, so
    # the core can hold it off (kaos_park_c5()).
    list(APPEND srcs "radio/src/kdrv_c5_hold.c")
endif()

# kdrv_ov5640/ and kdrv_station/ - the E5 final-test station: the camera
# module on J3 and the station's relay expander and ADC, on the camera's own
# I2C bus. Built only for the station (KODE_TEST_DOT); the manifest fetches
# esp_cam_sensor and esp_sccb_intf under the same option.
if(CONFIG_KODE_TEST_DOT)
    list(APPEND srcs
        "ov5640/src/kdrv_ov5640.c"
        "station/src/kdrv_station_i2c.c"
        "station/src/kdrv_pcf8574.c"
        "station/src/kdrv_ads1115.c"
        "station/src/kdrv_dock_loop.c"
    )
endif()

# rfal/ - STMicroelectronics' RFAL and ST25R3916 driver, vendored under
# SLA0051 (rfal/LICENSE.txt). The sources under rfal/rfal/ and rfal/chip/ are
# copied from X-CUBE-NFC6 and are not edited here; the port is rfal/port/.
file(GLOB rfal_srcs "${CMAKE_CURRENT_LIST_DIR}/rfal/rfal/Src/*.c"
                    "${CMAKE_CURRENT_LIST_DIR}/rfal/chip/*.c")
list(APPEND srcs ${rfal_srcs})

idf_component_register(
    SRCS ${srcs}
    PRIV_INCLUDE_DIRS
        "rfal/rfal/Inc"
        "rfal/rfal/Src"
        "rfal/chip"
        "rfal/port"
    INCLUDE_DIRS
        "apps/include"
        "audio/include"
        "buttons/include"
        "clocks/include"
        "cst820/include"
        "display/include"
        "exp_bus/include"
        "ir/include"
        "led/include"
        "mirror/include"
        "motion/include"
        "nfc/include"
        "ov5640/include"
        "power/include"
        "radio/include"
        "rfid/include"
        "station/include"
        "storage/include"
        "tca6408a/include"
    REQUIRES kdefs klog khal kbsp
    PRIV_REQUIRES
        esp_lcd
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_spi
        esp_driver_uart
        esp_driver_gpio
        esp_driver_parlio
        esp_driver_pcnt
        esp_adc
        fatfs
        wear_levelling
        app_update
        esp_partition
        espcoredump
        spi_flash
        esp_mm
        esp_driver_jpeg
        esp_driver_ppa
        bootloader_support
        esp_event
        esp_netif
        esp_wifi
        bt
        lwip
        esp_http_client
        esp-tls
        mbedtls
        espressif__mqtt
        espressif__esp_codec_dev
        espressif__esp_lcd_co5300
        espressif__esp_hosted
)

# esp_cam_sensor and esp_sccb_intf join the build only with KODE_TEST_DOT, so
# they are named as optional: required when present, nothing when not. IDF's
# own esp_driver_cam is always there; the linker drops it when nothing calls it.
idf_component_optional_requires(PRIVATE esp_driver_cam espressif__esp_cam_sensor espressif__esp_sccb_intf)

# Which silicon: IC_IDENTITY reads 0x31 on this board, an ST25R3916B.
# ST25R_COM_SINGLETXRX stays undefined: with it the chip driver hands
# platformSpiTxRx() one buffer as both tx and rx, which the P4's SPI master
# with DMA does not support, and rfalNfcInitialize() returns HW_MISMATCH.
target_compile_definitions(${COMPONENT_LIB} PRIVATE ST25R3916B)

# ST's sources warn under this toolchain. The warnings are portability noise,
# and they are demoted for those files only.
set_source_files_properties(${rfal_srcs} PROPERTIES COMPILE_OPTIONS
    "-Wno-unused-function;-Wno-unused-variable;-Wno-unused-parameter;-Wno-missing-field-initializers;-Wno-sign-compare;-Wno-implicit-fallthrough;-Wno-pointer-sign;-Wno-type-limits;-Wno-error")

# An app runs in place from the `apps` region (a DATA partition), so IDF's
# flash-write guard - esp_ota_get_running_partition(), APP partitions only -
# aborts every persistent write it makes, and at boot that is a panic loop.
# Under the app role only, wrap the two OTA lookups (kdrv_slot.c answers them);
# kodeOS itself (ROLE_OS) runs from a real APP partition and is left untouched.
# -u keeps the wrap objects when nothing in the app references the symbol yet.
if(CONFIG_KODEOS_BOOT_ROLE_APP)
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=esp_ota_get_running_partition"
        "-Wl,--wrap=esp_ota_get_next_update_partition"
        "-u __wrap_esp_ota_get_running_partition"
        "-u __wrap_esp_ota_get_next_update_partition")
endif()

# esp-hosted 2.12.9 leaves a memory pool and two mutexes behind at every
# teardown of the link, and neither is reachable from outside the component.
# kdrv_radio.c wraps the two pool calls to keep the list esp-hosted does not,
# and gives back what is left once esp_hosted_deinit() has returned.
if(CONFIG_KODEOS_RADIO)
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=hosted_mempool_create"
        "-Wl,--wrap=hosted_mempool_destroy"
        "-u __wrap_hosted_mempool_create"
        "-u __wrap_hosted_mempool_destroy")
endif()

# esp-hosted's request path holds no lock and this firmware has three threads
# on it: kdrv_radio_rpc.c gives them turns. Both entry points are called across
# a file (rpc_slave_if.c), so the linker's wrap sees every request. The file
# reads the request's layout, which only esp-hosted's private headers carry.
if(CONFIG_KODEOS_RADIO)
    idf_component_get_property(kdrv_hosted_dir espressif__esp_hosted COMPONENT_DIR)
    set_property(SOURCE "radio/src/kdrv_radio_rpc.c" APPEND PROPERTY INCLUDE_DIRECTORIES
        "${kdrv_hosted_dir}/host/drivers/rpc/slaveif"
        "${kdrv_hosted_dir}/host/drivers/rpc/core"
        "${kdrv_hosted_dir}/common/proto"
        "${kdrv_hosted_dir}/common/protobuf-c"
        "${kdrv_hosted_dir}/host/port/esp/freertos/include")
    # Named for the same headers: esp_eap_client.h and esp_dpp.h.
    idf_component_optional_requires(PRIVATE wpa_supplicant)
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=rpc_send_req"
        "-Wl,--wrap=rpc_wait_and_parse_sync_resp")
endif()

# The sleep sentinel counts what esp-hosted sends while the bus is silent and
# wakes the C5 for it: kdrv_radio_sleep.c wraps esp_hosted_tx(), which
# esp-hosted's other files call, so the linker's wrap sees every frame.
if(CONFIG_KODEOS_RADIO AND CONFIG_KODE_LOWPOWER)
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=esp_hosted_tx"
        "-u __wrap_esp_hosted_tx")
endif()
