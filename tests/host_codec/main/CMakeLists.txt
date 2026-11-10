set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../..")

file(GLOB CASE_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../cases/test_*.c")
# Sorted: link order is run order (see tests/host/main/CMakeLists.txt).
list(SORT CASE_SOURCES)

file(GLOB khal_includes LIST_DIRECTORIES true "${SDK_DIR}/components/khal/*/include")

idf_component_register(
    SRCS "runner.c"
         "log_stub.c"
         "fake_idf.c"
         ${CASE_SOURCES}
         "${SDK_DIR}/components/kdrv/audio/src/kdrv_es8311.c"
         "${SDK_DIR}/components/kdrv/audio/src/kes_fields.c"
         "${SDK_DIR}/components/kdrv/audio/src/kring.c"
         "${SDK_DIR}/components/kdrv/audio/src/ktone.c"
         "${SDK_DIR}/components/kdrv/audio/src/kwav.c"
    INCLUDE_DIRS "."
                 # The I2S driver's and the codec component's headers, cut to
                 # what the driver uses: neither builds for the host.
                 "shim"
                 "${SDK_DIR}/components/kdrv/audio/include"
                 ${khal_includes}
                 "${SDK_DIR}/components/kdefs/include"
                 "${SDK_DIR}/components/klog/include"
                 "${SDK_DIR}/components/kbsp/board/include"
    REQUIRES unity log freertos
    WHOLE_ARCHIVE)
