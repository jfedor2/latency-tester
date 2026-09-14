cmake_minimum_required(VERSION 3.13)

add_compile_definitions(PICO_DEFAULT_UART_BAUD_RATE=921600)
add_compile_definitions(PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)

set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/pico-sdk")
set(PICO_TINYUSB_PATH "${CMAKE_CURRENT_LIST_DIR}/tinyusb")
set(PICO_PIO_USB_PATH "${CMAKE_CURRENT_LIST_DIR}/Pico-PIO-USB")

# Headers for custom boards. Don't add pico/pico2 here: shadowing a board the
# SDK already knows confuses its CMake board-header parsing (PICO_PLATFORM).
set(PICO_BOARD_HEADER_DIRS "${CMAKE_CURRENT_LIST_DIR}/boards")

include(pico_sdk_import.cmake)

project(latency)

pico_sdk_init()

# Build ID, e.g. "pico2/pio/abc1234-dirty". Must come after pico_sdk_init(),
# which resolves PICO_BOARD. The git part is regenerated on every build.
add_compile_definitions(BUILD_BOARD="${PICO_BOARD}")
set(GIT_DESCRIBE_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_custom_target(git_describe
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}
        -DOUTPUT=${GIT_DESCRIBE_DIR}/git_describe.h
        -P ${CMAKE_CURRENT_LIST_DIR}/git_describe.cmake
    BYPRODUCTS ${GIT_DESCRIBE_DIR}/git_describe.h
)

check_and_add_pico_pio_usb_support()

add_compile_options(-Wall)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-std=c++17>)

add_executable(latency_native
    src/latency.cc
    src/xbox.cc
    src/app_driver.cc
    src/descriptor_parser.cc
    src/json_format.cc
    src/sniffer.cc
    src/toggle.cc
)
pico_generate_pio_header(latency_native ${CMAKE_CURRENT_LIST_DIR}/src/sniffer.pio)
pico_generate_pio_header(latency_native ${CMAKE_CURRENT_LIST_DIR}/src/toggle.pio)
add_dependencies(latency_native git_describe)
target_compile_definitions(latency_native PRIVATE BUILD_VARIANT="native")
target_include_directories(latency_native PRIVATE
    src
    src/tusb_config_host
    ${GIT_DESCRIBE_DIR}
)
target_link_libraries(latency_native
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_dma
    tinyusb_host
    tinyusb_board
)
# The native build logs over UART1 on GPIO4/5.
target_compile_definitions(latency_native PUBLIC
    PICO_DEFAULT_UART=1
    PICO_DEFAULT_UART_TX_PIN=4
    PICO_DEFAULT_UART_RX_PIN=5
)
pico_add_extra_outputs(latency_native)

add_executable(latency_pio
    src/latency.cc
    src/xbox.cc
    src/app_driver.cc
    src/descriptor_parser.cc
    src/json_format.cc
    src/sniffer.cc
    src/toggle.cc
)
pico_enable_stdio_usb(latency_pio 1)
pico_enable_stdio_uart(latency_pio 0)
pico_generate_pio_header(latency_pio ${CMAKE_CURRENT_LIST_DIR}/src/sniffer.pio)
pico_generate_pio_header(latency_pio ${CMAKE_CURRENT_LIST_DIR}/src/toggle.pio)
# The stock pico/pico2 board headers don't define PICO_DEFAULT_PIO_USB_DP_PIN.
if((PICO_BOARD STREQUAL "pico") OR (PICO_BOARD STREQUAL "pico2"))
    target_compile_definitions(latency_pio PUBLIC PICO_DEFAULT_PIO_USB_DP_PIN=0)
endif()
target_compile_definitions(latency_pio PUBLIC PICO_STDIO_USB_USE_DEFAULT_DESCRIPTORS=1)
add_dependencies(latency_pio git_describe)
target_compile_definitions(latency_pio PRIVATE BUILD_VARIANT="pio")
target_include_directories(latency_pio PRIVATE
    src
    src/tusb_config_both
    ${PICO_PIO_USB_PATH}
    ${GIT_DESCRIBE_DIR}
)
target_link_libraries(latency_pio
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_dma
    tinyusb_host
    tinyusb_board
    tinyusb_pico_pio_usb
)
pico_add_extra_outputs(latency_pio)
