cmake_minimum_required(VERSION 3.22)

# The target build: this tree's components compiled for the chip, with every
# optional Active Object but AO_TestDot switched on (the `testdot` variant,
# sdkconfig.testdot, turns it on). It is not an example - examples/ is
# where those live - and it has no behaviour worth running. It exists so that
# a change which stops the SDK compiling on the ESP-IDF kodeOS ships with is
# found by CI (.github/workflows/target-build.yml), not by the next person who
# builds an app.

# The chip, spelled out for the reason every example gives: ESP-IDF
# resolves the target before any board file has been read, and on a clean
# checkout it guesses esp32.
set(IDF_TARGET esp32p4)

# The board file straight from kbsp, by path. The examples cannot do this -
# they fetch kbsp and have to bootstrap around the fetch - but this project
# lives in the same tree as kbsp, so the file is simply there.
set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/../../components/kbsp/sdkconfig.board;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")

# A variant is one more fragment on top, named by KODE_TARGET_VARIANT
# (sdkconfig.<variant> beside this file). target-build.yml uses it for the
# configurations Kconfig offers and nobody builds day to day - which are the
# ones that stop linking without anyone noticing.
if(NOT "$ENV{KODE_TARGET_VARIANT}" STREQUAL "")
    list(APPEND SDKCONFIG_DEFAULTS
         "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.$ENV{KODE_TARGET_VARIANT}")
endif()

# A configuration that exists keeps its values over the fragments, and main/
# picks its source at configure time, so a build directory holds one variant.
# The sdkconfig lives in it, and it records the variant it was configured for:
# another variant in the same directory is refused. `rm -rf build` between
# variants (the empty name is the full build).
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
if(DEFINED KODE_TARGET_VARIANT_BUILT
   AND NOT "${KODE_TARGET_VARIANT_BUILT}" STREQUAL "$ENV{KODE_TARGET_VARIANT}")
    message(FATAL_ERROR
        "${CMAKE_BINARY_DIR} holds the variant '${KODE_TARGET_VARIANT_BUILT}', "
        "not '$ENV{KODE_TARGET_VARIANT}': rm -rf build")
endif()
set(KODE_TARGET_VARIANT_BUILT "$ENV{KODE_TARGET_VARIANT}"
    CACHE INTERNAL "The variant this build directory was configured for.")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(kode_target)

# In this tree's own components, a function nothing calls fails the build.
# ESP-IDF builds with -Werror but leaves this one a warning
# (-Wno-error=unused-function), and that warning was the only sign of #94:
# AO_Display armed its look at the glass in every build but handled it only
# with low power. Each variant compiles other #if branches, so each can hide
# one. The vendored QP/C, the managed components and ST's sources in kdrv
# (demoted file by file) keep ESP-IDF's default.
get_filename_component(kode_sdk_components "${CMAKE_CURRENT_LIST_DIR}/../../components" REALPATH)
idf_build_get_property(kode_build_components BUILD_COMPONENTS)
foreach(kode_component ${kode_build_components})
    idf_component_get_property(kode_dir ${kode_component} COMPONENT_DIR)
    idf_component_get_property(kode_lib ${kode_component} COMPONENT_LIB)
    get_filename_component(kode_dir "${kode_dir}" REALPATH)
    get_target_property(kode_type ${kode_lib} TYPE)
    if(kode_dir MATCHES "^${kode_sdk_components}/"
       AND NOT kode_dir STREQUAL "${kode_sdk_components}/qpc"
       AND NOT kode_type STREQUAL "INTERFACE_LIBRARY")
        target_compile_options(${kode_lib} PRIVATE "-Werror=unused-function")
    endif()
endforeach()
