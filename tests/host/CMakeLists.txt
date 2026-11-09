# The host test suite: every test that needs no board.
#
# `set(COMPONENTS main)` below is not an optimisation. Without it the configure
# walks the whole component registry looking for host implementations that do
# not exist, and dies on esp_hal_ana_cmpr - a peripheral driver with nothing to
# do with anything here. It must come BEFORE project().
#
# mbedTLS comes in for PSA's SHA-256, which kdrv_kfw uses to check an image
# against the digest it carries. Its own x509 sources trip a warning this
# toolchain promotes to an error - third-party code compiled for a target its
# authors do not test - so that one warning is demoted here rather than
# anywhere that could hide one of ours.
#
# Guarded by compiler, because -Wuninitialized-const-pointer is a CLANG
# diagnostic and does not exist in GCC - and GCC does not shrug at an unknown
# `-Wno-error=`, it stops: "no option '-Wuninitialized-const-pointer'". A
# workstation on macOS never sees that and CI, which builds this target with
# gcc, sees nothing else. A generator expression rather than an if(), because
# this runs BEFORE project() and CMAKE_C_COMPILER_ID does not exist yet.

cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_build_set_property(COMPILE_OPTIONS
    "$<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-error=uninitialized-const-pointer>"
    APPEND)

# CMock chains every mock's expectations from one pool, and only a Mock*_Init()
# empties it. test_net_ao.c never calls one (runner.c says why), so its cases
# and setUp()'s khal_gpio expectation pile up in the pool until the next file
# that does: CMock's default 32 KB ran out there, and every case after it
# failed with "CMock has run out of memory". Defined for the whole build, so
# cmock.c and every generated mock agree on the size.
idf_build_set_property(COMPILE_DEFINITIONS "CMOCK_MEM_SIZE=65536" APPEND)
# Unity's TEST_PASS() and the fakes' restart leave by longjmp. On macOS
# longjmp restores the signal mask with sigprocmask(), which there rewrites
# every thread's mask: the port's suspended task threads then take the tick
# signal and the binary deadlocks. _setjmp/_longjmp leave the mask alone;
# glibc's setjmp never touches it.
if(CMAKE_HOST_APPLE)
    idf_build_set_property(COMPILE_DEFINITIONS "setjmp=_setjmp;longjmp=_longjmp" APPEND)
endif()

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components/qpc"
    "${CMAKE_CURRENT_LIST_DIR}/mocks")

# ESP-IDF's own mocks for the remote WiFi stack. kdrv_wifi.c talks to the C5
# through esp_wifi_remote, esp_netif and esp_event, none of which has a host
# implementation - and all three ship as generated mocks, so this is wiring
# rather than generation. The real freertos stays: its linux port carries
# event_groups.c, which is what kdrv_wifi waits on, and QP/C needs the real
# scheduler underneath the AO cases.
list(APPEND EXTRA_COMPONENT_DIRS "$ENV{IDF_PATH}/tools/mocks/esp_wifi/")
list(APPEND EXTRA_COMPONENT_DIRS "$ENV{IDF_PATH}/tools/mocks/esp_netif/")
list(APPEND EXTRA_COMPONENT_DIRS "$ENV{IDF_PATH}/tools/mocks/esp_event/")
# esp_wifi's headers reach esp_mesh.h, which reaches lwip/ip_addr.h.
list(APPEND EXTRA_COMPONENT_DIRS "$ENV{IDF_PATH}/tools/mocks/lwip/")
# NOTE: esp_timer is deliberately NOT mocked. It registers include dirs only
# for `linux`, so the header is available and only the symbol is missing - and
# klog stamps every line with esp_timer_get_time(), so a CMock mock would
# demand an expectation per log line in every case in the suite. The fake in
# fakes/fake_khal_time.c supplies it instead, off the same clock a case
# already advances by hand.

# Note on ESP-IDF's other mocks, which are NOT used here. Shadowing
# bootloader_support by name (the way IDF's host tests wire mocks in) breaks
# the real esp_partition, which reads bootloader_support's include directory;
# and shadowing esp_partition too then fails to compile because its generated
# mock needs a complete esp_flash_t. Three components deep for one function is
# the wrong trade - kdrv_apps.c needs esp_image_verify() to *link*, not to
# behave, so a four-line fake stands in for it instead. Revisit this if a case
# ever needs to control what the image verifier says.
# mbedTLS comes in for PSA's SHA-256, which kdrv_kfw uses to check an image
# against the digest it carries. Its own x509 sources trip a warning this
# toolchain promotes to an error - third-party code, compiled for a target its
# authors do not test, so the warning is demoted here rather than anywhere
# that could hide one of ours.
set(COMPONENTS main)

project(kode_host_tests)
