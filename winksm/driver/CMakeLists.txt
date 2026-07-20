# WinKSM Kernel Driver - CMakeLists.txt
# Requires Windows Driver Kit (WDK) and CMake 3.20+

cmake_minimum_required(VERSION 3.20)
project(winksm_driver LANGUAGES C)

# WDK is required for kernel-mode drivers
find_package(WDK REQUIRED)

# Source files
set(DRIVER_SOURCES
    winksm_driver.c
    winksm_scanner.c
    winksm_hasher.c
    winksm_trees.c
    winksm_sharer.c
    winksm_rmap.c
    winksm_stats.c
)

# Create the driver target
wdk_add_driver(winksm
    ${DRIVER_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared/winksm_types.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared/winksm_protocol.h"
    "winksm.h"
)

# WDK compile options
target_compile_definitions(winksm PRIVATE
    NTDDI_VERSION=0x0A000000
    _WIN64
    _AMD64_
    UNICODE
    _UNICODE
    KERNEL_MODE
    NDIS60
)

target_include_directories(winksm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared
)

# Output settings
set_target_properties(winksm PROPERTIES
    SUFFIX ".sys"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
