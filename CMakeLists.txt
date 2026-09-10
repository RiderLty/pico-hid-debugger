set(PICO_BOARD pico2 CACHE STRING "Board type")

cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)
project(pico-hid-debugger)

pico_sdk_init()

# ------------------------------------------------------------------
# TinyUSB 3 级日志编译补丁（不修改 SDK 内任何文件）
#
# SDK 捆绑的 hid_host.c 在 CFG_TUSB_DEBUG>=3 时无法编译：两处
# TU_LOG3_MEM 仍引用重构前的端点缓冲成员（p_hid->epin_buf /
# p_hid->epout_buf，实际缓冲已移入独立的 hidh_epbuf_t，局部变量 epbuf）。
# 这里在 configure 期生成修正副本到 build 目录，并替换 tinyusb_host_base
# INTERFACE 源列表中的原始文件。上游修复该笔误后，FIND 不再命中、补丁
# 自动失效，SDK 原文件始终原样。
# ------------------------------------------------------------------
if(TARGET tinyusb_host_base)
  file(READ "${PICO_TINYUSB_PATH}/src/class/hid/hid_host.c" _hid_txt)
  string(FIND "${_hid_txt}" "TU_LOG3_MEM(p_hid->epout_buf" _bad_pos)
  if(NOT _bad_pos EQUAL -1)
    string(REPLACE
      "TU_LOG3_MEM(p_hid->epout_buf, len, 2);"
      "TU_LOG3_MEM(epbuf->epout, len, 2);"
      _hid_txt "${_hid_txt}")
    string(REPLACE
      "TU_LOG3_MEM(p_hid->epin_buf, xferred_bytes, 2);"
      "TU_LOG3_MEM(epbuf->epin, xferred_bytes, 2);"
      _hid_txt "${_hid_txt}")

    set(_hid_patched "${CMAKE_BINARY_DIR}/tusb_patch/hid_host.c")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tusb_patch")
    file(WRITE "${_hid_patched}" "${_hid_txt}")

    get_target_property(_tuh_srcs tinyusb_host_base INTERFACE_SOURCES)
    set(_tuh_srcs_new "")
    set(_replaced FALSE)
    foreach(_s IN LISTS _tuh_srcs)
      if(_s MATCHES "src/class/hid/hid_host\\.c$")
        list(APPEND _tuh_srcs_new "${_hid_patched}")
        set(_replaced TRUE)
      else()
        list(APPEND _tuh_srcs_new "${_s}")
      endif()
    endforeach()
    if(_replaced)
      set_target_properties(tinyusb_host_base PROPERTIES INTERFACE_SOURCES "${_tuh_srcs_new}")
      # 副本不在原目录，补上 hid_host.h 所在目录的显式 include 路径
      target_include_directories(tinyusb_host_base INTERFACE "${PICO_TINYUSB_PATH}/src/class/hid")
      message(STATUS "TinyUSB hid_host.c level-3 log patch applied (SDK file untouched)")
    else()
      message(WARNING "TinyUSB hid_host.c patch expected but source entry not found; unpatched build may fail at CFG_TUSB_DEBUG>=3")
    endif()
  endif()
endif()

# ------------------------------------------------------------------
# TinyUSB hub 驱动韧性编译补丁（不修改 SDK 内任何文件）
#
# SDK 捆绑的 TinyUSB 0.18.0（src/host/hub.c）在 hub 端口变化流程里，任何一次
# 传输结果非成功就直接放弃：hub_xfer_cb 用 TU_VERIFY 提前返回（状态轮询自此
# 不再入队），端口变化流程中的五处完成回调用 TU_ASSERT 断在当场。而 pio-usb
# 这类 HCD 出现事务级错误是常态（上游 hathach 原话 "often out-of-frame usb
# transaction error"），于是"Hub 上的设备拔出后检测不到"：实测日志即 hub 上报
# 到位 → GetPortStatus 控制传输失败 → ASSERT → hub 驱动永久停摆。
#
# 上游已在 TinyUSB PR #2994（Enhance hub driver，0.19.0 起）改为：结果非成功时
# 重新入队状态轮询——端口变化位仍留在 hub 内，下次上报会再次带来，从而自愈。
# 这里把同样的处理移植到 0.18.0 的 hub.c 副本（build 目录）并替换
# tinyusb_host_base 源列表中的原始文件，副本编译需补 src/host 显式 include 路径
# ——SDK 文件不动；SDK 内 TinyUSB 升到 >=0.19 后 FIND 不再命中、补丁自动失效。
# ------------------------------------------------------------------
if(TARGET tinyusb_host_base AND EXISTS "${PICO_TINYUSB_PATH}/src/host/hub.c")
  file(READ "${PICO_TINYUSB_PATH}/src/host/hub.c" _hub_txt)
  string(FIND "${_hub_txt}" "TU_VERIFY(result == XFER_RESULT_SUCCESS);" _hub_bad_pos)
  if(NOT _hub_bad_pos EQUAL -1)
    # (1) 中断轮询结果非成功：重新入队，而不是让 hub 事件永久失联
    string(REPLACE
      "  TU_VERIFY(result == XFER_RESULT_SUCCESS);"
      "  // 上游 TinyUSB #2994：HCD 报告非成功结果（pio-usb 事务级错误常见）时重新入队轮询；\n  // 端口变化位仍留在 hub 内，下次上报会再次带来。\n  if (result != XFER_RESULT_SUCCESS) {\n    hub_edpt_status_xfer(dev_addr);\n    return true;\n  }"
      _hub_txt "${_hub_txt}")
    # (2) 端口变化流程中的控制传输失败：重新入队状态轮询后再重试，而不是断言放弃
    string(REPLACE
      "  TU_ASSERT(xfer->result == XFER_RESULT_SUCCESS, );"
      "  // 上游 TinyUSB #2994：端口变化流程里的控制传输失败不能放弃，\n  // 重新入队状态轮询，等 hub 下次上报同一个变化位再重试。\n  if (xfer->result != XFER_RESULT_SUCCESS) {\n    hub_edpt_status_xfer(xfer->daddr);\n    return;\n  }"
      _hub_txt "${_hub_txt}")

    set(_hub_patched "${CMAKE_BINARY_DIR}/tusb_patch/hub.c")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tusb_patch")
    file(WRITE "${_hub_patched}" "${_hub_txt}")

    get_target_property(_tuh_srcs tinyusb_host_base INTERFACE_SOURCES)
    set(_tuh_srcs_new "")
    set(_replaced FALSE)
    foreach(_s IN LISTS _tuh_srcs)
      if(_s MATCHES "src/host/hub\\.c$")
        list(APPEND _tuh_srcs_new "${_hub_patched}")
        set(_replaced TRUE)
      else()
        list(APPEND _tuh_srcs_new "${_s}")
      endif()
    endforeach()
    if(_replaced)
      set_target_properties(tinyusb_host_base PROPERTIES INTERFACE_SOURCES "${_tuh_srcs_new}")
      # 副本不在原目录，补上 hub.h/usbh.h 所在目录的显式 include 路径
      target_include_directories(tinyusb_host_base INTERFACE "${PICO_TINYUSB_PATH}/src/host")
      set(TUSB_HUB_PATCHED 1)
      message(STATUS "TinyUSB hub.c resilience patch applied (SDK file untouched)")
    else()
      set(TUSB_HUB_PATCHED 0)
      message(WARNING "TinyUSB hub.c patch expected but source entry not found; hub 端口变化失败后可能永久停摆")
    endif()
  else()
    set(TUSB_HUB_PATCHED 1)
    message(STATUS "TinyUSB hub.c already has the resilience fix (>= 0.19.0); patch skipped")
  endif()
elseif(NOT EXISTS "${PICO_TINYUSB_PATH}/src/host/hub.c")
  # 新版 TinyUSB 换了布局（class/hub/hub.c）或 SDK 结构变化：视为已含修复，不打补丁
  set(TUSB_HUB_PATCHED 1)
  message(STATUS "TinyUSB hub.c 不在 src/host/（新版布局）；跳过 hub 韧性补丁")
else()
  set(TUSB_HUB_PATCHED 0)
endif()
if(NOT DEFINED TUSB_HUB_PATCHED)
  set(TUSB_HUB_PATCHED 0)
endif()

# ------------------------------------------------------------------
# PIO-USB 子模块版本检查：本仓库固定在**旧血脉（0.6.0 重写之前）的顶端**
#
#   9510f79 "Apply pin order settings to each port"（2024-06-02）
#
# 这一版含上游 PR #104 / 0f747aa
# "retire all transferring endpoint if device is disconnected" ——
# 即上游 issue #149 报告人认定的"最后一个能正确处理 hub 上设备拔出
# （有在途传输时）的提交"。0.6.0 起的重写换了整套 RX PIO 实现、带来
# 低速支持，但破坏了这条路径；本项目**放弃低速支持**换 hub 热插拔可靠，
# 因此固定在旧血脉。（此前针对 0.6+ 的 9 枚补丁已归档到
# patches/pio_usb_archive_0.6plus/，见 patches/pio_usb/README.md。）
#
# 兼容性：本版 API 与 TinyUSB 0.18 的 hcd_pio_usb.c 及本应用完全匹配
# （带 pinout 的 add_port、endpoint_open/transfer/abort、send_setup、
# close_device、port_reset_start/end、bus_get_line_state 全都有）。
# ------------------------------------------------------------------
set(_pio_usb_hdr "${CMAKE_CURRENT_SOURCE_DIR}/lib/pico_pio_usb/src/pio_usb.h")
if(NOT EXISTS "${_pio_usb_hdr}")
  message(FATAL_ERROR
    "lib/pico_pio_usb 子模块未初始化。\n"
    "  请执行：git submodule update --init --recursive   （或用 ./build.sh）")
endif()
file(READ "${_pio_usb_hdr}" _pio_usb_hdr_txt)
string(FIND "${_pio_usb_hdr_txt}" "pio_usb_host_task" _pio_legacy_pos)
if(_pio_legacy_pos EQUAL -1)
  message(FATAL_ERROR
    "lib/pico_pio_usb 不是旧血脉（0.6.0 之前）的版本，Hub 下设备拔出检测会失效。\n"
    "  本仓库固定 9510f79：git -C lib/pico_pio_usb checkout 9510f79 && git add lib/pico_pio_usb\n"
    "  背景见 README「PIO-USB 版本」与 patches/pio_usb/README.md")
endif()

# 子模块当前提交 → [BOOT] 行自证版本（排查时先核对，避免"刷错固件"）
execute_process(
  COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}/lib/pico_pio_usb" rev-parse --short HEAD
  OUTPUT_VARIABLE PIO_USB_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT PIO_USB_COMMIT)
  set(PIO_USB_COMMIT "unknown")
endif()
message(STATUS "PIO-USB: old lineage (0.6.0-pre) tip ${PIO_USB_COMMIT}")

# 补丁是否在位（旧血脉没有 SDK 2 兼容，缺了会以 pio_sm_set_jmp_pin 重定义报错，
# 这里提前给出可执行的提示；构建走 ./build.sh 时该补丁会自动应用）
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/lib/pico_pio_usb/src/usb_rx.pio" _pio_rx_txt)
string(FIND "${_pio_rx_txt}" "PICO_SDK_VERSION_MAJOR" _pio_sdk2_pos)
if(_pio_sdk2_pos EQUAL -1)
  message(FATAL_ERROR
    "lib/pico_pio_usb 缺少 SDK 2 构建兼容补丁（旧血脉从未拿到该修复）。\n"
    "  请执行：./scripts/apply-patches.sh  然后重新 configure（或用 ./build.sh）")
endif()

# ------------------------------------------------------------------
# hidkit（HID/XInput 解析库）与其 TinyUSB XInput 适配器：都是 git 子模块
#
#   lib/hidkit              —— 纯 C、零平台依赖的解析 core（不碰 USB 栈，只吃
#                              描述符与报文，吐标准事件；src/hidkit_app.c 是接线层）
#   lib/hidkit-tusb-xinput  —— XInput 类驱动 + 归一化接线（Xbox 没有 HID 接口，
#                              必须自己跑端点管线与初始化握手）
#
# 本固件把两者接成 [HIDKIT] 事件流：原始报文 hexdump **照旧**，语义事件是叠加的
# 一层 —— "所见即原始行为"这条不破。两个仓库都只读不改，升级靠 checkout 子模块。
# ------------------------------------------------------------------
foreach(_hk_path
    "lib/hidkit/include/hidkit.h"
    "lib/hidkit-tusb-xinput/CMakeLists.txt")
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_hk_path}")
    message(FATAL_ERROR
      "hidkit 子模块未初始化（缺 ${_hk_path}）。\n"
      "  请执行：git submodule update --init --recursive   （或用 ./build.sh）")
  endif()
endforeach()

# 库内诊断输出（[HKDBG] 行）默认开：本固件是调试器，"库自己怎么想的"正是要看的东西。
# 置 0 连调用点一起编译掉（零开销、无字符串），与 hidkit 的 HIDKIT_DEBUG 语义一致。
set(HIDKIT_LIB_DEBUG 1 CACHE STRING "hidkit 库内诊断输出（HIDKIT_DEBUG），0=关闭")

add_subdirectory(lib/hidkit)
add_subdirectory(lib/hidkit-tusb-xinput)

add_subdirectory(lib/pico_pio_usb)
add_subdirectory(src)
