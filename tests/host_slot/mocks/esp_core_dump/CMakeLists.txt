# CMock over esp_core_dump.h, the core dump in flash.
#
# espcoredump's own CMakeLists returns early for the linux target, as
# app_update's does, so there is nothing real to link; and its Kconfig is not
# read, so the options the header and kdrv_slot.c test are not in sdkconfig.h.
# They are given here instead, at the values the board builds with: kodeOS
# keeps its crashes in flash, and IDF's defaults for the rest. They reach
# whatever requires this component, kdrv_slot.c included, so the host build
# compiles the driver's core dump half and not its stand-in.
#
# The header is the real one. What it includes from outside espcoredump is the
# RISC-V summary port (the P4's) and esp_system's panic_internal.h, which
# needs nothing the linux build lacks.
idf_component_mock(
    INCLUDE_DIRS "$ENV{IDF_PATH}/components/espcoredump/include"
                 "$ENV{IDF_PATH}/components/espcoredump/include/port/riscv"
                 "$ENV{IDF_PATH}/components/esp_system/include"
    REQUIRES soc
    MOCK_HEADER_FILES "$ENV{IDF_PATH}/components/espcoredump/include/esp_core_dump.h")

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    "CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=1"
    "CONFIG_APP_RETRIEVE_LEN_ELF_SHA=9"
    "CONFIG_ESP_COREDUMP_SUMMARY_STACKDUMP_SIZE=1024")
