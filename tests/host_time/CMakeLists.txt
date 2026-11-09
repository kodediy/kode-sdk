# The third host suite: khal_time over a MOCKED esp_timer.
#
# Its own project for the same reason the NVS one has: the main suite fakes
# khal_time, and that fake's controllable clock is load-bearing for every
# driver and AO case in it. One definition per binary, so the real seam gets
# its own.
#
# Mocked rather than faked here, and that is the opposite choice from the main
# suite - deliberately. What is worth testing in this file is not what time it
# is; it is *which esp_timer calls khal_timer_periodic makes*, and in what
# order, on the failure path that deletes a timer it could not start. That is
# the question CMock exists to answer, and the path is unreachable on a board.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(COMPONENTS main)
list(APPEND EXTRA_COMPONENT_DIRS "$ENV{IDF_PATH}/tools/mocks/esp_timer/")

project(kode_host_time)
