# CMock over kdrv_ppa.h: the P4's pixel accelerator, without a P4.
#
# The scaler/flip is hardware AO_Display drives; there is nothing here for a
# case to assert on that the driver itself does not already prove on the
# board (Task 6's honest answer is that the only real check is the first
# mirrored frame looking right way up, two tasks from now). What this mock is
# for is display_ao.c's call to kdrv_ppa_init() in Display_initial - it has
# to resolve to *something* for the host build to link, and "ignored,
# returns KODE_OK" is that something.
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
    MOCK_HEADER_FILES "${SDK_DIR}/components/kdrv/mirror/include/kdrv_ppa.h")
