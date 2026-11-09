# The fourth host suite: kdrv_slot over the REAL esp_ota_ops.
#
# Same reason as tests/host_nvs, one level up the stack. The main suite MOCKS
# kdrv_slot - AO_Boot and AO_AppMgr are tested by the conversation they hold
# with it - and C allows one definition per binary, so the real driver cannot
# be linked there. This binary mocks nothing at all below it.
#
# app_update builds for `linux`, and esp_partition's emulation backs it with a
# flash file laid out from this project's partition table. That table is the
# BOARD'S own, because everything kdrv_slot does is about a geometry it did
# not choose: which of ota_0/ota_1 is not running, whether otadata is blank,
# whether an image fits the slot. A table invented for the tests would make
# every one of those answers a fiction that happens to be self-consistent.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/mocks")

set(COMPONENTS main)

project(kode_host_slot)
