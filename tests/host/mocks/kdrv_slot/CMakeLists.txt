# CMock over kdrv_slot.h: the kodeOS OTA slots and otadata.
#
# AO_Boot's whole job is deciding which of three mechanisms owns the next
# reset, and every one of those decisions is a call into this seam. What a
# case needs is to say what the slot answered - present or not, holding an
# image or not, otadata blank or dirty - and then assert which calls followed.
# That is what a mock is for, and it is unreachable on a board: making
# otadata refuse a write means arranging a failing flash sector.
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
    MOCK_HEADER_FILES "${SDK_DIR}/components/kdrv/apps/include/kdrv_slot.h")
