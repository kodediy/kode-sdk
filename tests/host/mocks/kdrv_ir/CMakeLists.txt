# CMock over kdrv_ir.h: the RMT, without a board.
#
# What AO_Ir does with the block is a sequence - take the capture, arm again,
# transmit, stay deaf for the echo - and that sequence is the thing worth
# asserting. The receive interrupt cannot be raised here; a case calls the
# callback it captured from kdrv_ir_init(), which is exactly what the ISR
# would do.
#
# knec.c is NOT mocked. It is arithmetic, it is compiled under test, and it
# lives in its own header so that this mock does not swallow it.
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
    MOCK_HEADER_FILES "${SDK_DIR}/components/kdrv/ir/include/kdrv_ir.h")
