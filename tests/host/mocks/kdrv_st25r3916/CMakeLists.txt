# CMock over kdrv_st25r3916.h: the reader, without a radio.
#
# What knfc_tag.c does with a tag is a sequence - read the capability
# container, walk the TLVs, refuse a message that does not fit, write four
# bytes at a time - and that sequence is the thing worth asserting. None of
# it needs a chip, and all of it is wrong in ways only a test would find.
#
# knfc_ndef.c and knfc_tag.c are NOT mocked. They are the code under test,
# and they live in headers of their own so that this mock does not swallow
# them - the same split knec.c has from kdrv_ir.h.
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
    MOCK_HEADER_FILES "${SDK_DIR}/components/kdrv/nfc/include/kdrv_st25r3916.h")
