# CMock over kdrv_exp_bus.h: J3's buses, without a wire.
#
# What kode_bus.c does around a transfer is the part worth asserting: the
# pins judged, released and lent, the controller taken, the loan given back
# when the driver refuses, the driver closed before the loan goes back. The
# books underneath are real (khal_exp, over the mocked khal_gpio); only the
# driver that would move bits is mocked.
set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../..")

idf_component_mock(
    INCLUDE_DIRS "${SDK_DIR}/components/kdrv/exp_bus/include"
                 "${SDK_DIR}/components/kdefs/include"
    MOCK_HEADER_FILES "${SDK_DIR}/components/kdrv/exp_bus/include/kdrv_exp_bus.h")
