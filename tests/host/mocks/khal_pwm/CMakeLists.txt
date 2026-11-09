# CMock over khal_pwm.h: the LEDC's table, for khal_exp's cases and the facade.
#
# khal_pwm itself is tested for real in tests/host_pwm, over a mock of its port.
set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../..")

idf_component_mock(
    INCLUDE_DIRS "${SDK_DIR}/components/khal/pwm/include"
                 "${SDK_DIR}/components/kdefs/include"
    MOCK_HEADER_FILES "${SDK_DIR}/components/khal/pwm/include/khal_pwm.h")
