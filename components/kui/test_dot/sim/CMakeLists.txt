# The E5 test's screens and patterns on the desktop: a simulator to look at them and a headless check.
cmake_minimum_required(VERSION 3.18)
project(kui_td_sim C)
include(${CMAKE_CURRENT_LIST_DIR}/../../sim/CMakeLists.txt)
kui_sim_add(kui_td_sim SOURCES ${CMAKE_CURRENT_LIST_DIR}/kui_td_sim.c)
kui_sim_add_check(kui_td_check SOURCES ${CMAKE_CURRENT_LIST_DIR}/kui_td_check.c)
