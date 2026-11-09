set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../..")
set(TDAN_DIR "${SDK_DIR}/components/kaos/test_dot/analysis")

file(GLOB CASE_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../cases/test_*.c")
# Sorted: link order is run order (see tests/host/main/CMakeLists.txt).
list(SORT CASE_SOURCES)

idf_component_register(
    SRCS "runner.c"
         "frame_png.c"
         ${CASE_SOURCES}
         "${TDAN_DIR}/src/tdan_geom.c"
         "${TDAN_DIR}/src/tdan_pix.c"
         "${TDAN_DIR}/src/tdan_judge.c"
    INCLUDE_DIRS "."
                 "${TDAN_DIR}/include"
    PRIV_INCLUDE_DIRS "${TDAN_DIR}/src"
    REQUIRES unity
    WHOLE_ARCHIVE)

# Fused multiply-add off, as in the firmware (components/kaos/CMakeLists.txt):
# the analysis must answer the same on the host and on the P4.
set_source_files_properties(
    "${TDAN_DIR}/src/tdan_geom.c"
    "${TDAN_DIR}/src/tdan_pix.c"
    "${TDAN_DIR}/src/tdan_judge.c"
    PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")

# The analysis calls atan2, hypot, sqrt and fmaxf. macOS folds libm into
# libSystem; on Linux it has to be named, or the suite compiles and then fails
# to link.
target_link_libraries(${COMPONENT_LIB} PRIVATE m)

# The frames are read from the source tree, where tools/tdan_gold.py writes them.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    TDAN_FRAMES_DIR="${CMAKE_CURRENT_LIST_DIR}/../frames")

# Sanitizers, only when KODE_SANITIZE is set; see tests/sanitize.cmake.
include(${CMAKE_CURRENT_LIST_DIR}/../../sanitize.cmake)
