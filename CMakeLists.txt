# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)


if(NOT DEFINED ENV{IDF_PATH})
    set(ENV{IDF_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/esp-idf")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(conveyor_monitor)
