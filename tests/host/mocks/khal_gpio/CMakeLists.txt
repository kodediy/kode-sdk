# CMock over khal_gpio.h: the two interrupting inputs, without a board.
#
# What the expander driver and the button driver do with a pin is a sequence
# of calls - make it an input, attach a callback, re-arm after a read - and
# that sequence is the thing worth asserting. The interrupt itself cannot be
# raised here; a case calls the callback it captured, which is exactly what
# the ISR would do.
set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../..")

file(GLOB khal_includes LIST_DIRECTORIES true "${SDK_DIR}/components/khal/*/include")

idf_component_mock(
    INCLUDE_DIRS ${khal_includes}
                 "${SDK_DIR}/components/kdefs/include"
    MOCK_HEADER_FILES "${SDK_DIR}/components/khal/gpio/include/khal_gpio.h")
