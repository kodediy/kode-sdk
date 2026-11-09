set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../..")

file(GLOB CASE_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../cases/test_*.c")
# Sorted: link order is run order (see tests/host/main/CMakeLists.txt).
list(SORT CASE_SOURCES)

file(GLOB khal_includes LIST_DIRECTORIES true "${SDK_DIR}/components/khal/*/include")

idf_component_register(
    SRCS "runner.c"
         "log_stub.c"
         "fake_khal_sync.c"
         ${CASE_SOURCES}
         "${SDK_DIR}/components/khal/pwm/src/khal_pwm.c"
    INCLUDE_DIRS "."
                 ${khal_includes}
                 "${SDK_DIR}/components/khal/pwm/src"
                 "${SDK_DIR}/components/kdefs/include"
                 "${SDK_DIR}/components/klog/include"
    REQUIRES unity khal_pwm_port
    WHOLE_ARCHIVE)
