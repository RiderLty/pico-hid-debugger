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

add_subdirectory(lib/pico_pio_usb)
add_subdirectory(src)
