set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../..")

file(GLOB CASE_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../cases/test_*.c")
# Sorted: a case registers itself from a constructor, so link order is run
# order, and an unsorted glob returns whatever the filesystem hands it - a
# different order on the bench than in CI. See tests/host/main/CMakeLists.txt.
list(SORT CASE_SOURCES)

file(GLOB khal_includes LIST_DIRECTORIES true "${SDK_DIR}/components/khal/*/include")

idf_component_register(
    SRCS "runner.c"
         "log_stub.c"
         ${CASE_SOURCES}
         "${SDK_DIR}/components/kdrv/apps/src/kdrv_slot.c"
    INCLUDE_DIRS "."
                 "${SDK_DIR}/components/kdrv/audio/include"
                 "${SDK_DIR}/components/kdrv/buttons/include"
                 "${SDK_DIR}/components/kdrv/clocks/include"
                 "${SDK_DIR}/components/kdrv/cst820/include"
                 "${SDK_DIR}/components/kdrv/display/include"
                 "${SDK_DIR}/components/kdrv/led/include"
                 "${SDK_DIR}/components/kdrv/tca6408a/include"
                 "${SDK_DIR}/components/kdrv/apps/include"
                 "${SDK_DIR}/components/kdrv/apps/src"
                 ${khal_includes}
                 "${SDK_DIR}/components/kdefs/include"
                 "${SDK_DIR}/components/klog/include"
    REQUIRES unity esp_ota_ops esp_partition
    WHOLE_ARCHIVE)

# Sanitizers, only when KODE_SANITIZE is set; see tests/sanitize.cmake.
include(${CMAKE_CURRENT_LIST_DIR}/../../sanitize.cmake)
