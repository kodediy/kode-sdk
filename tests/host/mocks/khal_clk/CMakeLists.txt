# CMock over khal_clk.h: the clock block, without silicon to count against.
#
# kdrv_clocks is arithmetic and judgement over three numbers - which source
# the mux picked, what the crystal measured, whether it measured at all - and
# on real hardware only one of the three is ever interesting, because a good
# board always answers the same way. The mock is what lets the failing boards
# exist: the one with no crystal fitted, the one whose crystal never started,
# the one that fell back to the RC without saying so.
set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../..")

file(GLOB khal_includes LIST_DIRECTORIES true "${SDK_DIR}/components/khal/*/include")

idf_component_mock(
    INCLUDE_DIRS ${khal_includes}
                 "${SDK_DIR}/components/kdefs/include"
    MOCK_HEADER_FILES "${SDK_DIR}/components/khal/clk/include/khal_clk.h")
