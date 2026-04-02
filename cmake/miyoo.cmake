# CMake toolchain file for Miyoo CFW (ARM Linux)

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

# Miyoo toolchain path
set(CHAINPREFIX "/opt/miyoo" CACHE PATH "Miyoo toolchain prefix")
set(CMAKE_C_COMPILER "${CHAINPREFIX}/bin/arm-linux-gcc")
set(CMAKE_CXX_COMPILER "${CHAINPREFIX}/bin/arm-linux-g++")

# Only use strip for Release builds (preserve debug symbols in Debug)
set(CMAKE_STRIP_DEFAULT "${CHAINPREFIX}/bin/arm-linux-strip")
set(CMAKE_STRIP "${CMAKE_STRIP_DEFAULT}" CACHE FILEPATH "Strip utility" FORCE)

# Sysroot
execute_process(
    COMMAND ${CMAKE_C_COMPILER} --print-sysroot
    OUTPUT_VARIABLE CMAKE_SYSROOT
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Search paths
set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Compiler flags
set(CMAKE_C_FLAGS_INIT "-std=gnu11")
set(CMAKE_CXX_FLAGS_INIT "-std=gnu++11")

# Debug flags: disable optimization, enable debug symbols
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")

# Optimization flags (CMake list format with semicolons)
set(MIYOO_OPT_FLAGS "-Ofast;-ffunction-sections;-fdata-sections;-fno-common;-fno-PIC;-flto" CACHE STRING "Miyoo optimization flags")

# Common definitions
add_compile_definitions(HAVE_STDINT_H VERSION_BITTBOY)
