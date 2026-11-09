# CMock over kdrv_station_i2c.h: the E5 station's transfers, without a station.
#
# What the relay and ADC drivers add to a transfer is the part worth
# asserting: the byte the expander is refused before anything goes out, the
# ADC's configuration word and the counts turned into mV, and the presence
# that needs both chips. The transfers themselves are IDF's.
set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../..")

idf_component_mock(
    INCLUDE_DIRS "${SDK_DIR}/components/kdrv/station/src"
                 "${SDK_DIR}/components/kdefs/include"
    MOCK_HEADER_FILES "${SDK_DIR}/components/kdrv/station/src/kdrv_station_i2c.h")
