cmake_minimum_required(VERSION 3.13)

add_compile_definitions(PICO_DEFAULT_UART_BAUD_RATE=921600)
add_compile_definitions(PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)

set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/pico-sdk")
set(PICO_TINYUSB_PATH "${CMAKE_CURRENT_LIST_DIR}/tinyusb")
set(PICO_PIO_USB_PATH "${CMAKE_CURRENT_LIST_DIR}/Pico-PIO-USB")

include(pico_sdk_import.cmake)

project(latency)

pico_sdk_init()

check_and_add_pico_pio_usb_support()

add_compile_options(-Wall)

add_executable(latency_native
    src/latency.cc
    src/interval_override.cc
    src/xbox.cc
    src/app_driver.cc
)
target_include_directories(latency_native PRIVATE
    src
    src/tusb_config_host
)
target_link_libraries(latency_native
    pico_stdlib
    pico_multicore
    tinyusb_host
    tinyusb_board
)
pico_add_extra_outputs(latency_native)

add_executable(latency_pio
    src/latency.cc
    src/interval_override.cc
    src/xbox.cc
    src/app_driver.cc
)
pico_enable_stdio_usb(latency_pio 1)
pico_enable_stdio_uart(latency_pio 0)
target_compile_definitions(latency_pio PUBLIC PICO_DEFAULT_PIO_USB_DP_PIN=0)
target_include_directories(latency_pio PRIVATE
    src
    src/tusb_config_both
    ${PICO_PIO_USB_PATH}
)
target_link_libraries(latency_pio
    pico_stdlib
    pico_multicore
    hardware_pio
    tinyusb_host
    tinyusb_board
    tinyusb_pico_pio_usb
)
pico_add_extra_outputs(latency_pio)
