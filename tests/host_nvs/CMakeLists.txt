# The second host suite: khal_nvs over the REAL nvs_flash.
#
# It exists because of a rule C gives us no way around - one definition per
# binary. The main suite fakes khal_nvs, and that fake is load-bearing for
# every driver and AO case in it, so the real one cannot be linked there. A
# second binary is the cheapest way to test the seam without taking the fake
# away from everybody else.
#
# What is under test is OURS, not IDF's: the status mapping. KODE_DENIED means
# "nothing was stored" rather than "something went wrong", and six callers
# branch on that distinction to decide whether to apply a default.
#
# nvs_flash builds for `linux` as a real component, and esp_partition's linux
# emulation backs it with a temp flash file sized from this project's own
# generated partition table. So nothing here is mocked below khal_nvs.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(COMPONENTS main)

project(kode_host_nvs)
