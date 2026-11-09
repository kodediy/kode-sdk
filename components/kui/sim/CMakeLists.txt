# The kit on the desktop: LVGL 9.6 over SDL2, the Kode Dot around the screen.
# A consumer includes this file and calls kui_sim_add() with its screens.
cmake_minimum_required(VERSION 3.18)
include(FetchContent)
# Fetched when the simulator is configured, and only then: a submodule would
# be cloned by every app that fetches the SDK. SOURCE_SUBDIR names a directory
# LVGL does not have, so MakeAvailable fetches the tree and adds none of its
# targets: kui_sim_add() compiles the sources it needs itself.
FetchContent_Declare(lvgl GIT_REPOSITORY https://github.com/lvgl/lvgl.git GIT_TAG v9.6.0
                     GIT_SHALLOW TRUE SOURCE_SUBDIR kui-sim-sources-only)
FetchContent_MakeAvailable(lvgl)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

set(KUI_SIM_DIR ${CMAKE_CURRENT_LIST_DIR})
set(KUI_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
set(KBSP_INCLUDE ${KUI_DIR}/../kbsp/board/include)

# LVGL, the kit and the consumer's sources, into one executable; HEADLESS leaves SDL out.
function(kui_sim_kit target)
    cmake_parse_arguments(A "HEADLESS" "" "SOURCES;INCLUDES" ${ARGN})
    file(GLOB_RECURSE LVGL_SRCS ${lvgl_SOURCE_DIR}/src/*.c)
    list(FILTER LVGL_SRCS EXCLUDE REGEX
         "/(thorvg|vg_lite[^/]*|gltf|svg|rlottie|ffmpeg|gstreamer|libpng|libjpeg_turbo|freetype|FT800[^/]*)/")
    if(A_HEADLESS)
        list(FILTER LVGL_SRCS EXCLUDE REGEX "/drivers/sdl/")
    endif()
    file(GLOB KUI_SRCS ${KUI_DIR}/*/src/*.c)
    # Kody's generated scenes are not built here either.
    list(FILTER KUI_SRCS EXCLUDE REGEX "/kody_[^/]*\\.c$")
    # The pack parser needs cJSON and khal, which the simulator does not have.
    list(FILTER KUI_SRCS EXCLUDE REGEX "/kui_pack\\.c$")
    add_executable(${target} ${LVGL_SRCS} ${KUI_SRCS} ${A_SOURCES})
    target_include_directories(${target} PRIVATE ${KUI_SIM_DIR} ${lvgl_SOURCE_DIR}
        ${lvgl_SOURCE_DIR}/src/libs/lodepng ${KUI_DIR}/core/include ${KUI_DIR}/drag/include ${KUI_DIR}/glass/include ${KUI_DIR}/kody/include
        ${KUI_DIR}/multitap/include ${KUI_DIR}/pack/include ${KUI_DIR}/panel/include ${KUI_DIR}/test_dot/include
        ${KUI_DIR}/core/src ${KBSP_INCLUDE} ${A_INCLUDES})
    target_compile_definitions(${target} PRIVATE LV_CONF_INCLUDE_SIMPLE
        LV_CONF_PATH="${KUI_SIM_DIR}/lv_conf.h" LV_LVGL_H_INCLUDE_SIMPLE)
    set_source_files_properties(${LVGL_SRCS} PROPERTIES COMPILE_OPTIONS "-w")
    if(A_HEADLESS)
        # LVGL's private header still names SDL's: its headers, never its library.
        target_include_directories(${target} PRIVATE
            $<TARGET_PROPERTY:PkgConfig::SDL2,INTERFACE_INCLUDE_DIRECTORIES>)
        target_link_libraries(${target} PRIVATE m)
    else()
        target_link_libraries(${target} PRIVATE PkgConfig::SDL2 m)
    endif()
endfunction()

# The simulator: the frame, the picture of the Kode Dot and the consumer's screens.
function(kui_sim_add target)
    cmake_parse_arguments(A "" "" "SOURCES;INCLUDES" ${ARGN})
    set(DEVICE_PNG_C ${CMAKE_CURRENT_BINARY_DIR}/kode_dot_png.c)
    add_custom_command(OUTPUT ${DEVICE_PNG_C}
        COMMAND ${CMAKE_COMMAND} -DIN=${KUI_SIM_DIR}/device/kode-dot.png -DOUT=${DEVICE_PNG_C}
                -DNAME=kode_dot_png -P ${KUI_SIM_DIR}/embed.cmake
        DEPENDS ${KUI_SIM_DIR}/device/kode-dot.png ${KUI_SIM_DIR}/embed.cmake)
    kui_sim_kit(${target} SOURCES ${KUI_SIM_DIR}/main.c ${KUI_SIM_DIR}/device.c ${KUI_SIM_DIR}/keys.c
                ${DEVICE_PNG_C} ${A_SOURCES} INCLUDES ${A_INCLUDES})
endfunction()

# A headless program over the same kit, for checks that need no window: the
# consumer's sources hold its main().
function(kui_sim_add_check target)
    cmake_parse_arguments(A "" "" "SOURCES;INCLUDES" ${ARGN})
    kui_sim_kit(${target} HEADLESS SOURCES ${A_SOURCES} INCLUDES ${A_INCLUDES})
endfunction()
