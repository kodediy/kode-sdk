# The sixth host suite: khal_pwm over a MOCKED port.
#
# Its own project because the main suite mocks khal_pwm.h for khal_exp's
# cases, and one binary cannot hold the mock and the real table. Here the
# table is real and the LEDC behind it is a CMock, so every allocation
# decision is a case that names the timer and the channel it touched.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(COMPONENTS main)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/mocks")

project(kode_host_pwm)
