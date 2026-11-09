# The seventh host suite: the screen analysis of the end-of-line test, on frames.
#
# Its own project because the analysis is pure C with nothing to fake: the
# cases read PNG photos from frames/ (written by tools/tdan_gold.py) and judge
# them with the real code.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(COMPONENTS main)

project(kode_host_tdan)
