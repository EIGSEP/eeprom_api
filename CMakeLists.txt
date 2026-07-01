cmake_minimum_required(VERSION 3.13)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(eeprom C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

add_library(at28bv64b STATIC src/at28bv64b.c)
target_include_directories(at28bv64b PUBLIC include)
target_link_libraries(at28bv64b pico_stdlib hardware_gpio)

add_subdirectory(test)
add_subdirectory(program_paths)
