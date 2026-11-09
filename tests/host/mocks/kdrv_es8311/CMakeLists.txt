# CMock over kdrv_es8311.h: the codec and the I2S controller, without a board.
#
# What AO_Audio does with the codec is a sequence - open, measure a floor,
# emit a tone and measure it, retry with a fresh pair - and that sequence is
# the thing worth asserting. It no longer ends in a close: since 2026-09-16
# the device is opened once and only a power-off closes it. The worker task is
# faked (fake_khal_worker.c): a case runs the job the AO submitted by hand,
# which calls kdrv_es8311_run() here exactly as the worker's own thread would.
#
# ktone.c and kwav.c are NOT mocked. They are arithmetic, they are compiled
# under test, and they live in their own headers so that this mock does not
# swallow them - knec.c's arrangement, for knec.c's reason.
# A trap this one paid for twice: ninja does NOT re-run CMock when the mocked
# header changes, only when this file does. So adding a function to
# kdrv_es8311.h and rebuilding fails at the LINK, complaining about a symbol
# the mock was never told to make. The fix is to delete
# tests/host/build/esp-idf/kdrv_es8311 and build again.
set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../..")

idf_component_mock(
    INCLUDE_DIRS "${SDK_DIR}/components/kdrv/audio/include"
                 "${SDK_DIR}/components/kdrv/buttons/include"
                 "${SDK_DIR}/components/kdrv/clocks/include"
                 "${SDK_DIR}/components/kdrv/cst820/include"
                 "${SDK_DIR}/components/kdrv/display/include"
                 "${SDK_DIR}/components/kdrv/led/include"
                 "${SDK_DIR}/components/kdrv/tca6408a/include"
                 "${SDK_DIR}/components/kdrv/audio/include"
                 "${SDK_DIR}/components/kdrv/ir/include"
                 "${SDK_DIR}/components/kdrv/nfc/include"
                 "${SDK_DIR}/components/kdrv/power/include"
                 "${SDK_DIR}/components/kdrv/storage/include"
                 "${SDK_DIR}/components/kdrv/apps/include"
                 "${SDK_DIR}/components/kdrv/mirror/include"
                 "${SDK_DIR}/components/kdrv/radio/include"
                 "${SDK_DIR}/components/kdefs/include"
    MOCK_HEADER_FILES "${SDK_DIR}/components/kdrv/audio/include/kdrv_es8311.h")
