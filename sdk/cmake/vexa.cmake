# A CMake toolchain file for Vexa:
#
#   cmake -DCMAKE_TOOLCHAIN_FILE=<sdk>/cmake/vexa.cmake -B build .
#
# Programs (add_executable) and static libraries build with vexa-cc (C++
# with vexa-c++), and
# find_package(SDL2) finds the SDK's SDL (SDL2::SDL2). Vexa
# loads only libvexa.so as a shared library, so link everything else
# statically (BUILD_SHARED_LIBS stays off).
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(VEXA 1)
get_filename_component(VEXA_SDK "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(CMAKE_C_COMPILER "${VEXA_SDK}/bin/vexa-cc")
set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_CXX_COMPILER "${VEXA_SDK}/bin/vexa-c++")
set(CMAKE_CXX_COMPILER_WORKS 1)
set(CMAKE_FIND_ROOT_PATH "${VEXA_SDK}")
list(APPEND CMAKE_PREFIX_PATH "${VEXA_SDK}") # find_package(SDL2)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(BUILD_SHARED_LIBS OFF)
