# khal: what several drivers share; one folder per module, include/ and src/.
idf_component_register(
    SRCS
        "clk/src/khal_clk_esp.c"
        "console/src/khal_console_esp.c"
        "crc32/src/khal_crc32_esp.c"
        "exp/src/khal_exp.c"
        "exp/src/khal_exp_esp.c"
        "gpio/src/khal_gpio_esp.c"
        "i2c/src/khal_i2c_esp.c"
        "identity/src/khal_identity_esp.c"
        "keep/src/khal_keep_esp.c"
        "nvs/src/khal_nvs_esp.c"
        "power/src/khal_power_esp.c"
        "pwm/src/khal_pwm.c"
        "pwm/src/khal_pwm_esp.c"
        "sdmmc/src/khal_sdmmc_esp.c"
        "sync/src/khal_once.c"
        "sync/src/khal_sync_esp.c"
        "system/src/khal_system_esp.c"
        "temp/src/khal_temp_esp.c"
        "time/src/khal_time_esp.c"
        "time/src/khal_time_zone.c"
        "worker/src/khal_worker_esp.c"
        "zlib/src/khal_zlib_esp.c"
    INCLUDE_DIRS
        "clk/include"
        "console/include"
        "crc32/include"
        "exp/include"
        "gpio/include"
        "i2c/include"
        "identity/include"
        "keep/include"
        "nvs/include"
        "power/include"
        "pwm/include"
        "sdmmc/include"
        "sync/include"
        "system/include"
        "temp/include"
        "time/include"
        "worker/include"
        "zlib/include"
    REQUIRES kdefs klog kbsp
    PRIV_REQUIRES
        esp_adc
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_ledc
        esp_driver_sdmmc
        esp_driver_tsens
        esp_driver_uart
        esp_driver_usb_serial_jtag
        esp_hw_support
        esp_mm
        esp_pm
        esp_psram
        esp_system
        spi_flash
        esp_timer
        fatfs
        nvs_flash
        sdmmc
        vfs
)

# esp-hosted's link teardown releases its own SDMMC slot only, not the card's, and the
# card and the link change the shared controller one at a time (khal_sdmmc_esp.c).
if(CONFIG_ESP_HOSTED_ENABLED)
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=sdmmc_host_deinit"
        "-Wl,--wrap=sdmmc_host_init"
        "-Wl,--wrap=sdmmc_host_init_slot"
        "-Wl,--wrap=sdmmc_host_deinit_slot"
        "-Wl,--undefined=__wrap_sdmmc_host_deinit"
        "-Wl,--undefined=__wrap_sdmmc_host_init"
        "-Wl,--undefined=__wrap_sdmmc_host_init_slot"
        "-Wl,--undefined=__wrap_sdmmc_host_deinit_slot")
endif()

# A warm boot takes the 32 kHz crystal's calibration from the last one (khal_system_esp.c).
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=rtc_clk_cal"
    "-Wl,--undefined=__wrap_rtc_clk_cal")

# And the PSRAM's timing, checked with a few reads instead of swept (khal_system_esp.c).
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=mspi_timing_psram_tuning"
    "-Wl,--wrap=mspi_timing_psram_set_best_tuning_phase"
    "-Wl,--wrap=mspi_timing_psram_set_best_tuning_delayline"
    "-Wl,--undefined=__wrap_mspi_timing_psram_tuning")

# Keep the start hook that holds core 1, which ESP-IDF only weakly references (khal_system_esp.c).
if(NOT CONFIG_FREERTOS_UNICORE)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--undefined=port_start_app_hook")
endif()
