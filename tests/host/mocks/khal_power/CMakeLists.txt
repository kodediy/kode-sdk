# CMock over khal_power.h: the PM lock, the wake lines and deep sleep, without a chip.
#
# A case that drives the low-power AO asserts the sequence of calls it makes
# (hold, arm, rearm, off_enter); the sleep itself is the board's.
set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../..")

idf_component_mock(
    INCLUDE_DIRS "${SDK_DIR}/components/khal/power/include"
                 "${SDK_DIR}/components/kdefs/include"
    MOCK_HEADER_FILES "${SDK_DIR}/components/khal/power/include/khal_power.h")
