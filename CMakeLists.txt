# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2_w CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(cmqtt C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Source files now live in src/ - just the network.c/network.h
# test harness, no PIO/ao27 stuff yet.
add_executable(cmqtt
        src/main.c
        src/network.c
)

pico_set_program_name(cmqtt "main")
pico_set_program_version(cmqtt "0.1")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(cmqtt 0)
pico_enable_stdio_usb(cmqtt 1)

# Add the standard include files to the build
target_include_directories(cmqtt PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/src
)

# MBEDTLS_CONFIG_FILE points mbedtls at our trimmed config so it fits in RP2350/RP2040 RAM
target_compile_definitions(cmqtt PRIVATE
        MBEDTLS_CONFIG_FILE="mbedtls_config.h"
)

# Add the standard library + wifi/tls/mqtt-support libraries to the build
target_link_libraries(cmqtt
        pico_stdlib
        pico_cyw43_arch_lwip_poll     # single-core polled lwip
        pico_mbedtls                  # vendored mbedtls (lib/mbedtls submodule)
        pico_rand                     # hardware RNG for entropy seeding
)

pico_add_extra_outputs(cmqtt)