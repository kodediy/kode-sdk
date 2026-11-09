set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../..")

file(GLOB CASE_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../cases/test_*.c")
# Sorted: a case registers itself from a constructor, so link order is run
# order, and an unsorted glob returns whatever the filesystem hands it - a
# different order on the bench than in CI. See tests/host/main/CMakeLists.txt.
list(SORT CASE_SOURCES)

file(GLOB khal_includes LIST_DIRECTORIES true "${SDK_DIR}/components/khal/*/include")

idf_component_register(
    SRCS "runner.c"
         ${CASE_SOURCES}
         "${SDK_DIR}/components/khal/time/src/khal_time_esp.c"
    INCLUDE_DIRS "."
                 ${khal_includes}
                 "${SDK_DIR}/components/kdefs/include"
                 "${SDK_DIR}/components/klog/include"
    REQUIRES unity esp_timer
    WHOLE_ARCHIVE)

# Sanitizers, only when KODE_SANITIZE is set; see tests/sanitize.cmake.
include(${CMAKE_CURRENT_LIST_DIR}/../../sanitize.cmake)
