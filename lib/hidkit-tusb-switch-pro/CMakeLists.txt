cmake_minimum_required(VERSION 3.13)
project(hidkit_tusb_switch_pro C)

# Compile with the consumer's configuration and toolchain, like hidkit/XInput.
add_library(hidkit_switch_pro_core INTERFACE)
target_sources(hidkit_switch_pro_core INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/src/switch_pro.c)
target_include_directories(hidkit_switch_pro_core INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/include)
target_compile_features(hidkit_switch_pro_core INTERFACE c_std_11)

# Add this directory after the host stack has defined tinyusb_host.
if(TARGET tinyusb_host)
    add_library(hidkit_tusb_switch_pro INTERFACE)
    target_sources(hidkit_tusb_switch_pro INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/adapters/switch_pro_tinyusb.c)
    target_link_libraries(hidkit_tusb_switch_pro INTERFACE
        hidkit_switch_pro_core tinyusb_host)
endif()

# Standalone build proves the core compiles without Pico, TinyUSB or hidkit.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_library(hidkit_switch_pro_static STATIC)
    target_link_libraries(hidkit_switch_pro_static PRIVATE hidkit_switch_pro_core)
endif()
