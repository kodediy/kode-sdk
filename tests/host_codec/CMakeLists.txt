# The eighth host suite: kdrv_es8311.c for real, over stand-ins for the I2S
# controller, the codec component and the GPIO driver.
#
# Its own project because the main suite mocks kdrv_es8311.h for the Active
# Objects above the driver, and one binary cannot hold the mock and the real
# file. Here the driver is the real source; what sits under it is a set of
# fakes that write every call they take into one ordered log, so a case can
# say what was done to the clock pin and in which order.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(COMPONENTS main)

project(kode_host_codec)
