# CMock over kdrv_kfw.h: the firmware container decoder.
#
# AO_AppMgr feeds every incoming image through this, and the real decoder
# needs a host inflate implementation the suite does not have. Mocked, the
# AO's own bookkeeping - the chunk ledger, the busy matrix, the watchdog on a
# medium that never answers - is reachable without it.
set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../..")

idf_component_mock(
    INCLUDE_DIRS "${SDK_DIR}/components/kdrv/audio/include"
                 "${SDK_DIR}/components/kdrv/buttons/include"
                 "${SDK_DIR}/components/kdrv/clocks/include"
                 "${SDK_DIR}/components/kdrv/cst820/include"
                 "${SDK_DIR}/components/kdrv/display/include"
                 "${SDK_DIR}/components/kdrv/led/include"
                 "${SDK_DIR}/components/kdrv/tca6408a/include"
                 "${SDK_DIR}/components/kdrv/ir/include"
                 "${SDK_DIR}/components/kdrv/nfc/include"
                 "${SDK_DIR}/components/kdrv/power/include"
                 "${SDK_DIR}/components/kdrv/storage/include"
                 "${SDK_DIR}/components/kdrv/apps/include"
                 "${SDK_DIR}/components/kdrv/mirror/include"
                 "${SDK_DIR}/components/kdrv/radio/include"
                 "${SDK_DIR}/components/kdefs/include"
    MOCK_HEADER_FILES "${SDK_DIR}/components/kdrv/apps/include/kdrv_kfw.h")
