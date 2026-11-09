# CMock over kdrv_display.h: the panel's control path, without a panel.
#
# What AO_Display owns is not the DSI conversation - that is the driver's -
# but the number that reaches the glass: a floor that applies to a chosen
# brightness and must NOT apply to a deliberate blank, applied before the
# announcement so that what is reported and what is driven are the same.
# Asserting on the value passed here is the only way to see that.
set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../..")

file(GLOB khal_includes LIST_DIRECTORIES true "${SDK_DIR}/components/khal/*/include")

idf_component_mock(
    INCLUDE_DIRS "${SDK_DIR}/components/kdrv/audio/include"
                 "${SDK_DIR}/components/kdrv/buttons/include"
                 "${SDK_DIR}/components/kdrv/clocks/include"
                 "${SDK_DIR}/components/kdrv/cst820/include"
                 "${SDK_DIR}/components/kdrv/display/include"
                 "${SDK_DIR}/components/kdrv/led/include"
                 "${SDK_DIR}/components/kdrv/tca6408a/include"
                 ${khal_includes}
                 "${SDK_DIR}/components/kdefs/include"
    MOCK_HEADER_FILES "${SDK_DIR}/components/kdrv/display/include/kdrv_display.h")
