# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

USB HID Host → CDC 串口桥固件，运行于 **Raspberry Pi Pico 2 (RP2350)**。

PIO-USB 端口连接的键盘/鼠标报文被解析成语义事件，按运行时选择的目的地输出：默认转发给原生 USB 上**模拟的 HID 键盘+鼠标**（Pico 成为硬件输入代理），也可经 CDC 串口（文本/二进制格式）输出到上位机；无法识别的 HID 设备（手柄、厂商自定义等）在串口模式回退输出原始报文，设备模式丢弃。

支持两种输出目的地，**运行时可切**（上位机经 CDC 下发单字符命令）：
- **设备转发模式**（`'D'`，开机默认）：解析后的事件直接写入原生 USB 上模拟的 HID 键盘+鼠标——Pico 成为硬件输入代理
- **串口输出模式**（`'S'`）：事件经 CDC 串口输出到上位机，格式二选一：
  - **文本**（`'T'`）：core_input 风格的可读事件行
  - **二进制**（`'B'`）：0x55 0xAA 帧头定界的紧凑帧

另有 Mac 模式修饰键交换开关（`'M'` 开启 / `'W'` 关闭，**开机默认开启**）：在 `hid_output_keyboard()` 单点完成 LALT↔LGUI、RALT↔RGUI 对调（Ctrl 不变）后再路由，作用于所有输出。

串口与模拟键鼠互斥，同一时刻只有一个目的地。

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

**三层结构 + 双核双 USB 栈：**

```
core1 (PIO-USB Host, GPIO12/13)                 core0 (原生USB Device = CDC+模拟键鼠)
─────────────────────────────────────           ─────────────────────────────────────
tuh_task()                                      tud_task()
 └ tuh_hid_*_cb        [hid_host_app]           cdc_output_flush(): 按 kind 分发出队记录
     │ 解析(键盘原始/鼠标归一化/其他原始)              ├ CDC数据 → tud_cdc_write
     ▼                                              └ HID报文 → tud_hid_n_report(键盘/鼠标)
hid_output 中间层（目的地变量 + 串口格式变量，均 static 可运行时切）
 ├ 目的地=设备 → out_device  键盘8B原样 / 鼠标6B归一化（挂载等元事件丢弃）
 └ 目的地=串口 ├ out_text   文本行（含按键边沿检测，拔出补发松开）
               └ out_binary 二进制帧（无边沿检测，边沿由上位机比对）
     ▼ 统一经 cdc_output SPSC 字节块队列（记录带 kind 标签，critical_section 保护）→ core0 写出
```

关键约束：
- 所有 `tud_cdc_*` 与 `tud_hid_*` 调用只允许出现在 core0（flush 分发、CDC RX 命令）。旧固件双核直写 Device FIFO 是竞态 bug，勿回退。
- `cdc_output_init()` 必须在 `multicore_launch_core1()` 之前调用（自旋锁先于生产者就绪）。
- 目的地/格式都是 static 变量（非宏）：core0 的 CDC RX 命令写入，core1 转发时原子读取。
- 边沿检测语义只在文本模式存在（out_text 内按 instance 分槽）；二进制与设备模式直接发状态，由接收方比对。

## 输出格式

### 文本模式（默认）

每行以 `\r\n` 结尾：

| 行格式 | 含义 |
|--------|------|
| `[MOUNT ] vid=%04x pid=%04x dev=%u itf=%u proto=%s` | 设备挂载 |
| `[UMOUNT] vid=%04x pid=%04x dev=%u itf=%u` | 设备拔出 |
| `KEY:%02X DN\|UP [名称]` | 键盘按键边沿；修饰键 E0-E7 附名称（如 `KEY:E0 DN LCTRL`） |
| `MOUSE:DX=%+d DY=%+d WH=%+d BTN=%s` | 鼠标移动帧（BTN= 当前按住的键：`---` / `L` / `L|R` …） |
| `MOUSE:BTN:%c DN\|UP` | 鼠标按键边沿（L/R/M/B/F/位号5-7） |
| `HID:[A1 02 3C FF]` | 未识别设备原始 HEX 兜底（超 20 字节截断为 `.. +N` 标注剩余长度） |
| `[DROP ] lost_events=%lu` | 行队列溢出的补报（排空后一次性发出） |
| `[ERROR ] %s` | 报告接收请求失败等错误 |

### 二进制模式

帧格式见 `src/hid_output.h` 头部注释（权威定义）。概要：解析规则按 type 二分——键盘/鼠标为紧凑帧 `55 AA | len(u8) | type | payload`；其余类型在 type 后跟来源身份（PID u16le、VID u16le、port u8=dev_addr）再跟 payload。type：0x01 键盘原始报文（紧凑）、0x02 鼠标归一化帧（buttons u8, wheel i8, x i16le, y i16le 共 6B，紧凑）、0x00 其他 HID 设备原始报文（带身份）、0x10/0x11 挂载/拔出、0x12 溢出补报、0x13 错误。

## Key Files

| File | Role |
|------|------|
| `src/pico_hid_debugger.c` | 入口：`main()`（core0）、`core1_main()`（core1）、CDC RX 命令 |
| `src/hid_host_app.c/.h` | HID 报文处理模块：tuh_hid 回调、鼠标描述符解析、统一事件产出 |
| `src/hid_output.c/.h` | 输出中间层：ops 接口 + static 模式变量选层；二进制帧格式权威定义在头注释 |
| `src/out_text.c` | 文本输出实现：core_input 风格行格式化 + 按键边沿检测（原 hid_dispatch 逻辑并入于此） |
| `src/out_binary.c` | 二进制输出实现：0x55AA 帧封装 |
| `src/out_device.c/.h` | 设备转发实现：事件写入模拟键鼠（键盘 8B 原样/鼠标 6B 归一化），`out_device_release_all()` 切换时补发全零 |
| `src/cdc_output.c/.h` | CDC 传输层：跨核 SPSC 字节块队列、批量 flush |
| `src/hid_parser.c/.h` | HID 报告描述符解析器（自 pico-hid-mapper 移植）：通用字段展开、跨字节位提取、鼠标纯解析 `hid_mouse_parse_frame` |
| `src/tusb_config.h` | TinyUSB 配置：Device = CDC + HID 键盘/鼠标 ×2；Host HID×16 + Hub |
| `src/usb_descriptors.c` | USB 描述符：CDC + 模拟键盘/鼠标复合设备，报告描述符在此文件 |
| `src/CMakeLists.txt` | 构建目标，链接 pico_stdlib, pico_pio_usb, tinyusb_device, tinyusb_host |
| `CMakeLists.txt` | Top-level: sets board to `pico2`, includes Pico SDK |
| `lib/pico_pio_usb/` | Vendored PIO-USB library (sekigon-gonnoc) |

## USB Interface & Endpoint Layout

| Interface | Type | Endpoints |
|-----------|------|-----------|
| ITF 0 (CDC) | CDC COMM | EP 0x81 (notif) |
| ITF 1 (CDC) | CDC DATA | EP 0x02 (OUT), EP 0x82 (IN) |
| ITF 2 (HID) | 模拟键盘（Boot 协议，8B 报文） | EP 0x83 (IN, 1ms) |
| ITF 3 (HID) | 模拟鼠标（报告协议，6B 报文：buttons/wheel/x16/y16） | EP 0x84 (IN, 1ms) |

VID 0xCAFE / PID 0x4002（字面量定义；接口集变化时升 PID 使主机干净重枚举）。报告描述符长度一律用 `sizeof()` 取值。

## HID 解析管线（移植说明）

管线自参考项目 `pico-hid-mapper` 的输入层移植，切割点为其 `core_input_*` 函数边界：
- 原 `makcu_intercept_*` 物理输入拦截层替换为 hid_output 中间层的统一事件入口
- 参考工程的 license XOR 报文扰动、游戏手柄/XInput 分支、Lua/宏/网络/触屏均未移植
- 相比参考工程修复的缺口：
  - 多 Report ID 复合鼠标——每个字段记录所属 Report ID，索引与解析锁定 X/Y 所在的鼠标集合；
  - 实现了 PUSH(0xA0)/POP(0xB2) 全局项保存恢复；
  - 按键字段支持非连续布局（多组侧键可被 X/Y 隔开），dispatch 顺序扫描编号；
  - 接口协议为 None 的接口也尝试鼠标解析（部分游戏鼠 bInterfaceProtocol 不规范填 0），判定标准为"相对轴 + 按键 ≥1"，手柄摇杆/触摸板等绝对轴设备不会误判；
  - 文本模式拔出时补发仍按着的修饰键/普通键/鼠标键（out_text 的 umount 内完成）

## TinyUSB Configuration Notes

- `tusb_config.h` must be in an include path reachable from the source directory.
- Both `CFG_TUD_ENABLED` and `CFG_TUH_ENABLED` are set to 1 — the firmware runs device and host stacks simultaneously.
- `CFG_TUH_ENUMERATION_BUFSIZE` 为 512：复杂鼠标描述符常超 256 字节，超限时 desc_report=NULL、该设备无法解析。
- `CFG_TUH_HID` is set to 16 (max HID devices). `CFG_TUH_DEVICE_MAX` is 4 (hub ports).

## Known Limitations

1. **Report-ID 键盘会误码**：REPORT 协议下自带 Report ID 的键盘每帧多一个前导字节。文本模式的边沿检测会把 ID 字节当修饰键；设备转发模式同样会把该字节透传进模拟键盘导致错码。二进制模式发原始报文不受影响。
2. 设备转发模式下：其他 HID 设备（手柄等）与挂载/拔出/溢出/错误元事件没有承载通道，静默丢弃；模拟鼠标不声明 Boot 子类（16bit 轴不符合 boot 布局），极少数仅支持 boot 鼠标的环境（部分 BIOS）不可用。
3. hub 下接多个键盘时，任一键盘拔出会向模拟键盘补发全零，连带松开其他键盘按住的键（换取实现简单的取舍）。
4. 队列满只在上位机停止读取/设备未枚举完成时才可能发生（128 块 ≈ 1kHz 报告率下约 128ms 缓冲）；溢出有计数且排空后可见（仅串口目的地可显示）。
5. CDC 未连接时串口事件即产即弃，不缓存回放（有意为之，避免重连后重放过期移动流）。
6. 运行中切换输出格式即刻生效，但切换瞬间可能产生半行文本/半帧二进制的衔接噪声，上位机解析器应具备重同步能力（二进制按 55 AA 扫描即可）。

## Conventions

- All source comments and commit messages are in **Chinese**.
- Compiler flags: `-Wall -Wextra` with memory usage reporting via `--print-memory-usage`.
- The `lib/pico_pio_usb/` directory is vendored — edit with caution, as it's a third-party library.
