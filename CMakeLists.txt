cmake_minimum_required(VERSION 3.13)

add_compile_definitions(PICO_DEFAULT_UART_BAUD_RATE=921600)
add_compile_definitions(PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)

set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/pico-sdk")
set(PICO_TINYUSB_PATH "${CMAKE_CURRENT_LIST_DIR}/tinyusb")

include(pico_sdk_import.cmake)

project(latency)

pico_sdk_init()

add_compile_options(-Wall)

add_executable(latency
    src/latency.c
    src/interval_override.c
)
target_include_directories(latency PRIVATE src)
target_link_libraries(latency
    pico_stdlib
    tinyusb_host
    tinyusb_board
)
pico_add_extra_outputs(latency)
