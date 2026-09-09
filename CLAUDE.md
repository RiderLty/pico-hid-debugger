# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

USB HID 设备调试器固件，运行于 **Raspberry Pi Pico 2 (RP2350)**。

PIO-USB 端口（GPIO12/13）枚举插入的 USB 设备（含 Hub），挂载时经异步控制传输抓取并 dump：设备描述符、配置描述符（原始整块）、字符串描述符（语言 ID/厂商/产品/序列号）；HID 接口挂载时 dump 接口信息与报告描述符；运行时把每份 HID 报告按行 hexdump。**不做任何 HID 语义解析**——本固件是"所见即原始行为"的调试采集器。

原生 USB Device 栈禁用（`CFG_TUD_ENABLED=0`）：Pico 在上位机上不枚举任何设备，输出经硬件 UART0（**GPIO2=TX / GPIO3=RX，921600bps 8N1**）。注意 RP2350 上 GPIO2/3 的 UART 复用在 FUNCSEL 11（`GPIO_FUNC_UART_AUX`），不是 RP2040 的 FUNC2。

## Build

Requires `PICO_SDK_PATH` set, ARM cross-compiler (`gcc-arm-none-eabi`, `libnewlib-arm-none-eabi`), and CMake >= 3.13.

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

Output: `build/src/pico-hid-debugger.uf2` — flash by holding BOOTSEL and copying to the mass storage device.

No test suite or linter is configured.

## Architecture

**双核分工 + SPSC 队列（无 Device 栈）：**

```
core1 (PIO-USB Host, GPIO12/13)                 core0 (UART0, GPIO2/3, 921600)
─────────────────────────────────────           ──────────────────────────────
tuh_task()                                      uart_output_flush(): 批量出队
 └ tuh_mount_cb        [hid_host_app]              → uart_write_blocking()
     │ 描述符抓取状态机（异步控制传输链）
     │   DEV → CFG → LANG → Mfg → Prod → Ser
     ├ tuh_hid_mount_cb → itf 信息 + 报告描述符 dump + 订阅报文
     ├ tuh_hid_report_received_cb → 报文 hexdump
     ▼ 格式化文本行（emit / dump_hex）→ uart_output_send() 入队
        SPSC 字节块队列（critical_section 保护，core1 写 core0 读）
```

关键约束：
- 所有 UART 写操作只允许出现在 core0 的 `uart_output_flush()`；core1 只做格式化与入队，消除并发写 UART。
- `uart_output_init()`（UART + 队列 + 临界区）必须在 `multicore_launch_core1()` 之前调用：生产者随时可能入队。
- 系统时钟必须为 12MHz 整数倍（当前 120MHz），PIO-USB 时序依赖。
- 描述符抓取是**异步**控制传输链（tuh_descriptor_get_* 完成回调里发起下一步）；状态按 dev_addr 分槽（`desc_state_t`）。设备拔出时 `tuh_umount_cb` 将 step 置 IDLE，迟到的完成回调据此丢弃。
- HID 报文回调里先输出再 `tuh_hid_receive_report()` 重新订阅，报文流才持续。

## 输出格式

每行 `\r\n` 结尾。行格式权威定义见 [README.md](README.md)「输出格式」一节。概要：`[TAG]` 定界，TAG 为 5 字符（`MOUNT`/`DEVDS`/`CFGDS`/`STRDS`/`HIDMT`/`RPTDS`/`UNHID`/`DEVRM`/`ERROR`/`DROP`/`HID`）；HEX 折行 dump 首行带 `len=`，续行以 `[TAG+]` 开头；报文行 `[HID] dev= itf= len=:`，续行 `[HID+]`。

## Key Files

| File | Role |
|------|------|
| `src/pico_hid_debugger.c` | 入口：`main()`（core0：UART 初始化与输出循环）、`core1_main()`（core1：tuh 配置与任务循环） |
| `src/hid_host_app.c/.h` | 信息采集模块：全部 tuh 回调、描述符抓取状态机（异步控制传输链）、hexdump 格式化 |
| `src/uart_output.c/.h` | 跨核传输层：SPSC 字节块队列、UART0 初始化（GPIO2/3 @ 921600）、core0 批量写出 |
| `src/tusb_config.h` | TinyUSB 配置：仅 Host 栈（CFG_TUD_ENABLED=0），Host HID×16 + Hub，枚举缓冲 512 |
| `src/CMakeLists.txt` | 构建目标，链接 pico_stdlib, pico_pio_usb, tinyusb_host；stdio UART/USB 显式关闭 |
| `CMakeLists.txt` | Top-level: sets board to `pico2`, includes Pico SDK |
| `lib/pico_pio_usb/` | Vendored PIO-USB library (sekigon-gonnoc) |
| `tools/uart_monitor.py` | 上位机串口监视脚本（pyserial，自动探测/冻结/清屏） |

## TinyUSB Configuration Notes

- `tusb_config.h` must be in an include path reachable from the source directory.
- `CFG_TUD_ENABLED=0`：仅 Host 栈。device 侧源（`usb_descriptors.c`、`dcd_pio_usb.c`）已删除，链接库无 `tinyusb_device`。
- `CFG_TUH_ENUMERATION_BUFSIZE` 为 512：复杂设备描述符常超 256 字节；报告描述符超过该值时 TinyUSB 不抓取（`desc_report=NULL`），本固件显示 `[RPTDS] not captured`。
- `CFG_TUH_HID` is set to 16 (max HID instances). `CFG_TUH_HUB` is 1, `CFG_TUH_DEVICE_MAX` is 4 (hub 下设备数，不含 hub 自身)。
- `tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT)`：拿设备原生报文（Boot 协议下设备改写为简化布局，丢失厂商扩展字段）。
- 描述符抓取缓冲（`desc_state_t` 内）按 TinyUSB 惯例 `CFG_TUSB_MEM_ALIGN`（4 字节）对齐。

## Known Limitations

1. **UART 带宽**：921600bps ≈ 11.5KB/s，hexdump 使字节膨胀 3 倍。1kHz×8B 的移动报文约占带宽一半；大报告（>30B）高频率会超载。队列满丢行计数，排空后 `[DROP ]` 补报。若需更高带宽：换更高速率（RP2350 UART 可跑 5Mbps+）或压缩输出。
2. 描述符抓取与 HID 驱动自身的控制传输（报告描述符请求等）共用设备控制通道，由 TinyUSB 排队串行化；抓取失败（如设备不支持字符串）仅 `[ERROR]`/跳过，不影响报文流。
3. 多配置设备只 dump 配置 1；字符串非 ASCII 字符显示 `?`。
4. 枚举信息只在挂载时抓取一次，运行中不会重复查询（设备描述符/字符串不会变化，属有意为之）。

## Conventions

- All source comments and commit messages are in **Chinese**.
- Compiler flags: `-Wall -Wextra` with memory usage reporting via `--print-memory-usage`.
- The `lib/pico_pio_usb/` directory is vendored — edit with caution, as it's a third-party library.
- 行格式变更需同步更新 README「输出格式」表与 `tools/uart_monitor.py` 的高亮规则。
