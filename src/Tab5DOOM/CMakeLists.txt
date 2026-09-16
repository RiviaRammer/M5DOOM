cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(m5doom)

esptool_py_flash_to_partition(flash wad "${CMAKE_CURRENT_LIST_DIR}/doom1-cut.wad")

# Deliberately reset savegames on every full `idf.py flash`, even when flashing
# the same firmware. app-flash alone preserves saves. Never source this image
# from user files: this directory is build-generated and empty.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/empty_saves")
file(GLOB save_seed_files "${CMAKE_BINARY_DIR}/empty_saves/*")
if(save_seed_files)
    message(FATAL_ERROR "empty_saves must contain no files; refusing to seed private save data")
endif()
spiffs_create_partition_image(saves "${CMAKE_BINARY_DIR}/empty_saves" FLASH_IN_PROJECT)
