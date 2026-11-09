# The fifth host suite: kdrv_net, over mocked network stacks.
#
# It is its own project for the same reason tests/host_nvs and tests/host_slot
# are: the main suite MOCKS kdrv_net, because AO_Net is tested by the
# conversation it holds with it, and C allows one definition per binary.
#
# Everything under this driver has to be mocked, and not by preference - both
# esp_http_client and lwip return early from their own CMakeLists for the
# linux target. That is less of a loss than it looks. What kdrv_net.c owns is
# not the protocol work: it is the job bodies in front of it, the clamping of
# every result table an operator reads, the budget of a stream's open, and the
# refusals that keep an AO from waiting out its own deadline for an answer
# nobody was going to send.
#
# freertos is REAL, which costs nothing: the driver owns no task or queue now -
# AO_Net runs these bodies on its own worker, and that arrangement is tested
# beside AO_Net - but the ping session still gives a semaphore.
#
# khal_time_ms() is a stand-in the cases move by hand (main/time_stub.c): the
# open spends a budget, and nothing here really waits.
#
# CONFIG_KODEOS_MQTT is deliberately OFF here, and that is a decision worth
# stating rather than discovering. mqtt_client.h is not in the IDF tree at all
# - esp-mqtt is a managed component, so the header only exists under a
# generated managed_components/ directory that a resolve happens to have
# filled. Reaching into another project's is the kind of path that works until
# somebody runs a clean checkout.
#
# What that costs: the MQTT half of kdrv_net.c is not covered here. What it
# buys, besides everything else in the file: the MQTT-DISABLED build has its
# own guards - four entry points that must refuse rather than return success,
# and a run that must refuse an MQTT start outright, because "an unanswered
# job is an AO waiting forever" - and this is the only place they are reachable
# at all. That build ships; KODEOS_MQTT is a Kconfig, not a constant.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/mocks"
    "$ENV{IDF_PATH}/tools/mocks/esp_netif")

set(COMPONENTS main)

project(kode_host_net)
