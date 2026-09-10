set(target_name pico-hid-debugger)
add_executable(${target_name})

# UART0 波特率可编译期覆盖（默认 2000000，见 uart_output.h；
# 须与上位机选择一致且 USB-UART 适配器支持）
set(UART_BAUD "" CACHE STRING "UART0 baudrate override, e.g. 921600")
if(UART_BAUD)
  target_compile_definitions(${target_name} PRIVATE UARTO_BAUDRATE=${UART_BAUD})
endif()

# [HIDKIT] 事件行开关：1kHz 鼠标这类高流量设备下，语义事件行会吃掉可观的 UART
# 带宽（已知限制第 1 条），置 0 只保留库内诊断与原始 hexdump
set(HIDKIT_APP_EVENTS 1 CACHE STRING "[HIDKIT] 事件行开关，0=只留 hexdump 与库内诊断")
if(NOT HIDKIT_APP_EVENTS)
  target_compile_definitions(${target_name} PRIVATE HIDKIT_APP_EVENTS=0)
endif()

target_sources(${target_name} PRIVATE
 pico_hid_debugger.c
 hid_host_app.c
 hidkit_app.c
 uart_output.c
 log_switch.c
 tusb_log.c
 ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
 )

# print memory usage, enable all warnings
target_link_options(${target_name} PRIVATE -Xlinker --print-memory-usage)
target_compile_options(${target_name} PRIVATE -Wall -Wextra)

# use tinyusb implementation
target_compile_definitions(${target_name} PRIVATE PIO_USB_USE_TINYUSB)

# 补丁在位标志（顶层 CMakeLists.txt 依据实际打补丁情况设置）→ 写进 [BOOT] 行，
# 用来一眼确认刷进板子的到底是哪份固件（tusb 版本 + 两处补丁是否生效）
target_compile_definitions(${target_name} PRIVATE
  TUSB_HUB_PATCHED=${TUSB_HUB_PATCHED}
  PIO_USB_COMMIT="${PIO_USB_COMMIT}"
  HIDKIT_DEBUG=${HIDKIT_LIB_DEBUG})

# hidkit 容量：调试器要同时挂多个设备做对照，给得比默认宽（默认 2/1/2）。
# 槽位总数 4+4+4=12 ≤ CFG_TUH_HID(16)，不会出现"枚举得到、库拿不到槽位"。
# EVICT_IDLE 策略（默认）在 12 槽位下基本不会触发；真触发时库会打 [HKDBG] 行。
target_compile_definitions(${target_name} PRIVATE
  HIDKIT_MAX_KEYBOARDS=4
  HIDKIT_MAX_MICE=4
  HIDKIT_MAX_GAMEPADS=4)

# needed so tinyusb can find tusb_config.h
target_include_directories(${target_name} PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(${target_name} PRIVATE
 pico_stdlib
 pico_pio_usb
 tinyusb_host
 pico_time
 hidkit
 hidkit_tusb_xinput
)

# 输出走自管理的 UART0，不启用 SDK stdio 的默认串口/USB 通道
pico_enable_stdio_uart(${target_name} 0)
pico_enable_stdio_usb(${target_name} 0)

pico_add_extra_outputs(${target_name})
