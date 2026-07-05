cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(m5doom)

esptool_py_flash_to_partition(flash wad "${CMAKE_CURRENT_LIST_DIR}/doom1-cut.wad")
