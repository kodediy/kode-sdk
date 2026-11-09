# CMock over esp_ota_ops.h, because the real one is out of reach.
#
# app_update's own CMakeLists returns early for the linux target - "this
# component is not supported by the POSIX/Linux simulator" - so there is no
# real esp_ota_ops to link against here, whatever the flash emulation can do.
#
# That is less of a loss than it looks. What kdrv_slot.c actually owns is the
# part above this seam: which of ota_0/ota_1 is not the running one, the
# esp_err_t to kode_status_t mapping, the session handle's discipline, and the
# length check the sink adds. All of that is testable against a mock, and none
# of it is what app_update does.
#
# esp_partition stays REAL: kdrv_otadata_blank() walks the whole otadata
# partition byte by byte, and that walk is worth running against a flash file
# laid out from the board's own table rather than against a script.
set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../..")

idf_component_mock(
    INCLUDE_DIRS "$ENV{IDF_PATH}/components/app_update/include"
                 "$ENV{IDF_PATH}/components/bootloader_support/include"
                 "$ENV{IDF_PATH}/components/esp_app_format/include"
                 "$ENV{IDF_PATH}/components/esp_bootloader_format/include"
    REQUIRES esp_partition
    MOCK_HEADER_FILES "$ENV{IDF_PATH}/components/app_update/include/esp_ota_ops.h")
