# CMock over khal_i2c.h.
#
# A fake says what the driver was told; this says what the driver *said* - the
# register writes, in order, with the right bytes, which is the whole content
# of a chip driver.
set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../..")

file(GLOB khal_includes LIST_DIRECTORIES true "${SDK_DIR}/components/khal/*/include")

idf_component_mock(
    INCLUDE_DIRS ${khal_includes}
                 "${SDK_DIR}/components/kdefs/include"
    MOCK_HEADER_FILES
    "${SDK_DIR}/components/khal/i2c/include/khal_i2c.h")
