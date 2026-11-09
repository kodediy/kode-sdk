# CMock over khal_exp_port.h: the ADC and the LEDC without a board.
#
# khal_exp.c is compiled for real over this and over the khal_gpio mock, so
# every refusal in its table is a case that asserts the port was NOT called.
set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../..")

idf_component_mock(
    INCLUDE_DIRS "${SDK_DIR}/components/khal/exp/src"
                 "${SDK_DIR}/components/kdefs/include"
    MOCK_HEADER_FILES "${SDK_DIR}/components/khal/exp/src/khal_exp_port.h")
