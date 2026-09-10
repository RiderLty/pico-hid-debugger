# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

USB HID 设备调试器固件，运行于 **Raspberry Pi Pico 2 (RP2350)**。

PIO-USB 端口（GPIO12/13）枚举插入的 USB 设备（含 Hub），挂载时经异步控制传输抓取并 dump：设备描述符、配置描述符（原始整块）、字符串描述符（语言 ID/厂商/产品/序列号）；HID 接口挂载时 dump 接口信息与报告描述符；运行时把每份 HID 报告按行 hexdump。**原始报文不做任何语义解释**——本固件首先是"所见即原始行为"的调试采集器。

在其之上**叠加**一层语义解析：报文同时喂给 [hidkit](https://github.com/RiderLty/hidkit)（`lib/hidkit` 子模块，纯 C 解析库），事件以 `[HIDKIT]` 行输出、库内诊断以 `[HKDBG]` 行输出；Xbox 手柄这类没有 HID 接口的设备由 `lib/hidkit-tusb-xinput`（XInput 类驱动 + 归一化接线）接管后走同一个出口。这一层是纯附加的：语义解析不改变原始采集，两者各自独立开关。

四类输出（`[HID]` 原始报文 / `[HIDKIT]` 语义事件 / `[HKDBG]` 库内诊断 / `[TUSB]` 栈日志）**各有运行期开关位**，由上位机经 UART 下行**单字节**控制（`src/log_switch.c/.h`），另有挂载/描述符行的 `INFO` 位。**固件上电默认全开 `0x1F`**（"失败要响"：没有下发手段的观察者不该遇到类别静默消失）；`index.html` 的记忆偏好默认 `0x13`（语义层 + 挂载信息），连上即下发。编译期开关（`HIDKIT_APP_EVENTS`/`HIDKIT_LIB_DEBUG`）保持原样，运行期位叠加在其上：编译期决定代码在不在固件里，运行期决定现在出不出。`[BOOT]`/`[ERROR]`/`[DROP]`/`[CTRL]` 恒开。

原生 USB Device 栈禁用（`CFG_TUD_ENABLED=0`）：Pico 在上位机上不枚举任何设备，输出经硬件 UART0（**GPIO2=TX / GPIO3=RX，2000000bps 8N1**），同一个口的 RX 收上位机下行的开关指令。注意 RP2350 上 GPIO2/3 的 UART 复用在 FUNCSEL 11（`GPIO_FUNC_UART_AUX`），不是 RP2040 的 FUNC2。**RX 引脚必须上拉**（`uart_output_init()` 里的 `gpio_pull_up`）：`gpio_set_function()` 不动上下拉、RP2350 pad 复位是浮空的，适配器没接时悬空输入会在 2Mbaud 下产生随机字节，每个都会被当成一发开关指令。

## Build

Requires `PICO_SDK_PATH` set, ARM cross-compiler (`gcc-arm-none-eabi`, `libnewlib-arm-none-eabi`), and CMake >= 3.13.

```bash
./build.sh    # 自动：初始化子模块 → 打 PIO-USB 补丁 → 配置 → 编译
```

或手动：

```bash
git submodule update --init --recursive   # lib/pico_pio_usb 是子模块
./scripts/apply-patches.sh                # 子模块补丁（当前=SDK2 构建兼容，幂等）
mkdir build && cd build
cmake ..
make -j$(nproc)
```

Output: `build/src/pico-hid-debugger.uf2` — flash by holding BOOTSEL and copying to the mass storage device.

No test suite or linter is configured.

## Architecture

**双核分工 + SPSC 队列（无 Device 栈）：**

```
core1 (PIO-USB Host, GPIO12/13)                 core0 (UART0, GPIO2/3, 2000000)
─────────────────────────────────────           ──────────────────────────────
tuh_task()                                      while(1) 循环：
 └ tuh_mount_cb        [hid_host_app]             1. log_switch_poll()  排空 UART0 RX
     │ 描述符抓取状态机（异步控制传输链）           │   → 置掩码 + [CTRL] 直写
     │                                             └ 2. uart_output_flush() 批量出队
     │   DEV → CFG → LANG → Mfg → Prod → Ser
     ├ tuh_hid_mount_cb → itf 信息 + 报告描述符 dump + 订阅报文
     │                    └ hidkit_app_mount() → hidkit_mount()（不认就不占槽位）
     ├ tuh_hid_report_received_cb → 报文 hexdump
     │                    └ hidkit_app_report() → hidkit_report()
     │                         └ hidkit_input_*（弱符号）→ [HIDKIT] 行
     ├ tuh_hid_umount_cb → hidkit_app_umount()（库内先补发"全部抬起"）
     ├ tuh_xinput_* [lib/hidkit-tusb-xinput] → hidkit_xinput_report() → 同一批出口
     ▼ 格式化文本行（hid_app_emit / emit_tagged / hexdump）→ uart_output_send() 入队
        SPSC 字节块队列（critical_section 保护，core1 写 core0 读）
```

关键约束：
- **所有 UART 写操作只在 core0**，出口有两个：`uart_output_flush()`（队列批量出队）与 `uart_output_write_direct()`（绕过队列的直写，给 core0 自己要输出的行用）。core1 只做格式化与 `uart_output_send()` 入队，消除并发写 UART。
- **core0 不得调用 `uart_output_send()`**（除 `boot_banner()` 外——它在 `multicore_launch_core1()` 之前，SPSC 单生产者约定未被破坏）：队列是 core1 单向的生产者，core0 入队会破坏它。core0 自己输出（`[DROP]` 补报、`[CTRL]` 回执）一律走 `uart_output_write_direct()`。代价是这类行**可能排在 core1 更早入队的行之前**，属外观问题。
- `uart_output_init()`（UART + 队列 + 临界区）必须在 `multicore_launch_core1()` 之前调用：生产者随时可能入队。
- `log_switch_init()` 同样必须在 `multicore_launch_core1()` 之前、且在 core0（core1 一起来就可能读掩码；晚于 launch 会让 core1 读到 BSS 的 0，把最早的栈日志全丢掉）。它**不输出任何东西**，也不得打印——`[BOOT]` 必须仍是本次连接的第一行。
- 运行期掩码 `s_mask` 是**单字节 `volatile`**，core0 写、core1（含中断上下文）读，**刻意不加临界区**（对齐字节访问在 ARMv8-M 上原子、SRAM 无 cache、无伴随数据）。改多字段结构或配计数器就必须加锁。
- 判定放**最外层**（`hid_app_emit_info` / `hexdump` / `emit_tagged` / `tusb_log_printf` 入口）：关掉时连格式化都不做，且一次 dump 全有或全无。`hexdump` / `emit_tagged` 的开关位是**首参**，漏改调用点会编译报错而非静默变义。
- **开关只管打印，不管解析**：`[HID]` 关掉时 `hidkit_app_report()` 照常调用，`INFO` 关掉时描述符抓取状态机照常跑。
- 系统时钟必须为 12MHz 整数倍（当前 120MHz），PIO-USB 时序依赖。
- 描述符抓取是**异步**控制传输链（tuh_descriptor_get_* 完成回调里发起下一步）；状态按 dev_addr 分槽（`desc_state_t`）。设备拔出时 `tuh_umount_cb` 将 step 置 IDLE，迟到的完成回调据此丢弃。
- HID 报文回调里先输出再 `tuh_hid_receive_report()` 重新订阅，报文流才持续。

## 输出格式

每行 `\r\n` 结尾。行格式权威定义见 [README.md](README.md)「输出格式」一节。格式约定：

- TAG 定宽：`[TAG]` 紧跟 TAG 本体，其后用空格补齐到第 8 列（`TAG_COL`）再输出内容——hexdump 内用 `%*s` 补位，其余 emit 行的 TAG 均为 5 字符 + 1 空格天然对齐。**例外**：`[HIDKIT]`/`[HKDBG]`（6/5 字符）统一补到第 9 列（`hidkit_app.c` 的 `HK_TAG_COL`），两族行彼此对齐。
- HEX 折行 dump 统一走 `hexdump()`：每行前缀含 `len=`（整块总长）与 `off=`（本行起始偏移），前缀用空格补齐到固定列 `HEX_COL`（40）后才输出数据，跨行数据列垂直对齐。`itf_num` 传 `-1` 省略 itf 字段（设备级描述符），报文与报告描述符传实际接口号。
- 运行期开关按 TAG 分族：`[HIDKIT]`/`[HKDBG]`/`[TUSB]`/`[HID]` 各一位，挂载与描述符行（`[MOUNT]`/`[DEVDS]`/`[CFGDS]`/`[STRDS]`/`[HIDMT]`/`[RPTDS]`/`[UNHID]`/`[DEVRM]`）共用 `INFO` 位；`[BOOT]`/`[ERROR]`/`[DROP]`/`[CTRL]` 恒开。

## Key Files

| File | Role |
|------|------|
| `src/pico_hid_debugger.c` | 入口：`main()`（core0：UART 初始化与输出循环）、`core1_main()`（core1：tuh 配置与任务循环） |
| `src/hid_host_app.c/.h` | 信息采集模块：全部 tuh 回调、描述符抓取状态机（异步控制传输链）、hexdump 格式化；HID 三个回调在 dump 之后各多一步 `hidkit_app_*` 转发。**两个行出口**：`hid_app_emit()`（恒开，`[ERROR]` 走它）与 `hid_app_emit_info()`（受 `LOG_SW_INFO` 门控，挂载/描述符行走它），共用 static 内核 `emit_v()`。`hexdump()` 首参是开关位 |
| `src/hidkit_app.c/.h` | **语义层接线**：实现 hidkit 的四个弱符号出口（`hidkit_input_*`）与库内诊断出口（`hidkit_debug_printf`）→ `[HIDKIT]`/`[HKDBG]` 行；维护 instance→槽位映射；**定义 `usbh_app_driver_get_cb()`** 转发 XInput 适配器的类驱动（该钩子全工程只能有一个定义，适配器刻意不定义）。static `emit_tagged(bit, tag, fmt, ...)` 首参是开关位 |
| `src/log_switch.c/.h` | **运行期输出开关**：位定义（`LOG_SW_*`/`LOG_SW_DEFAULT`=0x13）、`log_switch_init()`（core0，置默认值 + 读丢弃 RX 残留，必须在 launch core1 之前）、`log_switch_on()`（热路径只读单字节 volatile）、`log_switch_poll()`（core0 排空 UART RX、应用掩码、回 `[CTRL]`）。位表是**固件与 index.html 的接口**，改这里要同步改 `index.html` 的 `data-sw` 与 README 的表 |
| `src/uart_output.c/.h` | 跨核传输层：SPSC 字节块队列、UART0 初始化（GPIO2/3 @ 2000000）、core0 批量写出、core0 直写出口 `uart_output_write_direct()`；RX 上拉在此设置（防悬空噪声被当成开关指令） |
| `src/tusb_log.c/.h` | TinyUSB 内部日志桥接：`CFG_TUSB_DEBUG_PRINTF` 挂接 `tu_printf`，片段按行组装（core1 临界区防穿插），`[TUSB]` 头入队 |
| `src/tusb_config.h` | TinyUSB 配置：仅 Host 栈（CFG_TUD_ENABLED=0），Host HID×16 + Hub + `CFG_TUH_XINPUT`，枚举缓冲 512，`CFG_TUSB_DEBUG=3`（SDK 命令行自带 `-DCFG_TUSB_DEBUG=0`，故这里先 `#undef` 再定义，否则每份 TU 都报 redefined） |
| `src/CMakeLists.txt` | 构建目标，链接 pico_stdlib, pico_pio_usb, tinyusb_host, hidkit, hidkit_tusb_xinput；传 hidkit 容量宏（`HIDKIT_MAX_*` 4/4/4）与两个开关（`HIDKIT_DEBUG`、`HIDKIT_APP_EVENTS`）；stdio UART/USB 显式关闭 |
| `CMakeLists.txt` | Top-level: sets board to `pico2`, includes Pico SDK |
| `lib/hidkit/` | **语义解析核心**（git 子模块，独立开源仓库 [RiderLty/hidkit](https://github.com/RiderLty/hidkit)）。纯 C、零平台依赖、static 内存；报告描述符 → 字段表，报文 → 事件，出口是**弱符号函数**（`hidkit_input_*`）。**只读不改**：要升级就 checkout 子模块到新提交；缺陷与残余限制记在它自己的 `KNOWN_ISSUES.md`（目前 7 条已修：5 条移植时故意保留的描述符缺陷 + `Report Count = 0` 规范符合性 + 一条**移植引入的回归**：HID 手柄布局表曾被槽位守界挡住探测调用而恒不命中，真机表现为 `unhandled 054c:0ce6`，DS5 全不认识）。容量/策略宏见其 `src/hidkit_config.h` |
| `lib/hidkit-tusb-xinput/` | **XInput 适配器**（git 子模块，独立开源仓库 [RiderLty/hidkit-tusb-xinput](https://github.com/RiderLty/hidkit-tusb-xinput)）：TinyUSB XInput 类驱动（移植件）+ 接线层（`tuh_xinput_*` → `hidkit_xinput_report()`）。同样**只读不改**。`CFG_TUH_XINPUT` 在 `src/tusb_config.h` 里打开（关掉则整个驱动编译掉） |
| `lib/pico_pio_usb/` | PIO-USB 库，**git 子模块**锁定上游 sekigon-gonnoc/Pico-PIO-USB **旧血脉顶端 `9510f79`**（0.6.0 重写之前；含 `0f747aa` "retired all transferring endpoint if device is disconnected"）。**本仓库放弃低速支持换 hub 热插拔可靠**，版本对照与理由见 `patches/pio_usb/README.md`；`[BOOT]` 行的 `piousb=` 即该提交，排查前先核对。**不要直接改子模块里的文件**：需要的修改走 `patches/pio_usb/` + `scripts/apply-patches.sh`。**注意**：D+/D− 引脚（GPIO12/13）在应用代码 `pico_hid_debugger.c` 的 `pio_cfg.pin_dp` 显式配置——上游 `PIO_USB_DP_PIN_DEFAULT` 是 GPIO0，不要依赖库内默认值 |
| `patches/pio_usb/` | 当前**只有一枚** `0001-sdk2-compat.patch`（旧血脉缺的 Pico SDK 2 构建兼容：本地 `pio_sm_set_jmp_pin` 与 SDK2 重名冲突 + 生成头缺 `pio_version` 字段）。**基线 = 子模块锁定提交 9510f79**。针对 0.6+ 血脉写的 9 枚补丁与整轮调查结论归档在 `patches/pio_usb_archive_0.6plus/`（当前不使用） |
| `scripts/apply-patches.sh` | 幂等打补丁脚本（默认应用 / `--status` / `--revert`）；`build.sh` 与 CMake 配置期都会检查补丁是否在位 |
| `tools/uart_monitor.py` | 上位机串口监视脚本（pyserial，自动探测/冻结/清屏） |
| `index.html` | Web Serial 日志查看器：**xterm.js + WebGL 渲染**（vendor/ 于 `vendor/`，UMD 挂载注意：xterm 展开式、fit/webgl 命名空间式），默认 2M，**固件输出开关勾选框**（`data-sw` 位号须与 `src/log_switch.h` 一致；localStorage `switch` 记忆，默认 `0x13`，连接打开即下发、`[BOOT]` 行补发，下发经 promise 链串行化——`WritableStream` 同一时刻只能有一个 writer）。勾选框**一个控件管两件事**：下发掩码 + 本地按位显示（`TAG_INFO` 表同时给出配色与开关位，两者不是一回事：`[BOOT]` 与 `[MOUNT]` 同色但前者恒开）。按位显示不是冗余——固件默认全开，而 `[BOOT]` 先于 core1 打印，队列会把启动那波 `[TUSB]` 缓冲在后，掩码翻回来还要再吐一段。正则内容过滤（忽略大小写，历史存 localStorage regexHist/regex，input 防抖 400ms 实时应用、回车/失焦记忆，无效红框保持上次视图），贴底跟随为 xterm 原生语义（视口 scroll 判贴底 + 回到底部角标），授权持久化 + `connect` 事件 + 100ms 轮询看门狗自动重连，ANSI 着色，模型上限 50000 行 |

## TinyUSB Configuration Notes

- `tusb_config.h` must be in an include path reachable from the source directory.
- `CFG_TUD_ENABLED=0`：仅 Host 栈。device 侧源（`usb_descriptors.c`、`dcd_pio_usb.c`）已删除，链接库无 `tinyusb_device`。
- `CFG_TUH_ENUMERATION_BUFSIZE` 为 512：复杂设备描述符常超 256 字节；报告描述符超过该值时 TinyUSB 不抓取（`desc_report=NULL`），本固件显示 `[RPTDS] not captured`。
- `CFG_TUH_HID` is set to 16 (max HID instances). `CFG_TUH_HUB` is 1, `CFG_TUH_DEVICE_MAX` is 4 (hub 下设备数，不含 hub 自身)。
- `tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT)`：拿设备原生报文（Boot 协议下设备改写为简化布局，丢失厂商扩展字段）。
- `CFG_TUSB_DEBUG=3` 开启 TinyUSB 内部日志（1=错误 2=+警告 3=+信息，级别 3 极啰嗦且挤占 UART 带宽）；经 `CFG_TUSB_DEBUG_PRINTF=tusb_log_printf` 重定向到 `src/tusb_log.c`：片段按 `\n` 组装成行（TinyUSB 日志不保证行完整）、加 `[TUSB]` 头走同一 SPSC 队列。`tu_printf` 钩子按 TinyUSB 约定返回 `int`；`tusb_log.h` 由 `tusb_config.h` include，使所有 TinyUSB 翻译单元拿到原型。`tusb_log_init()` 必须在 `tuh_init()` 之前调用（最早日志出现在栈初始化期间）。
- **TinyUSB hid_host.c 编译补丁**（顶层 CMakeLists.txt）：SDK 捆绑的 hid_host.c 在 `CFG_TUSB_DEBUG>=3` 时引用已重构掉的成员（`p_hid->epin_buf/epout_buf`，实际缓冲已移入 `hidh_epbuf_t`），无法编译。configure 期生成修正副本（改用 `epbuf->epin/epout`）到 build 目录并替换 `tinyusb_host_base` INTERFACE 源列表中的原始文件，副本编译需补 `src/class/hid` 显式 include 路径——SDK 文件不动；上游修复后 `string(FIND)` 不再命中，补丁自动失效。
- **TinyUSB hub.c 韧性补丁**（顶层 CMakeLists.txt，同一手法）：SDK 捆绑的 TinyUSB **0.18.0**（`src/host/hub.c`）在 hub 端口变化流程里对任何非成功传输结果都永久放弃——`hub_xfer_cb` 的 `TU_VERIFY(result == XFER_RESULT_SUCCESS)` 直接返回（状态轮询自此不再入队），端口变化流程五处完成回调的 `TU_ASSERT(xfer->result == ...)` 直接断言。pio-usb 出现事务级错误是常态，于是 hub 上报到位后 GetPortStatus 一失败就永久停摆 → **Hub 上设备拔出检测失效**。移植上游 TinyUSB **PR #2994**（Enhance hub driver，0.19.0 起）的语义：结果非成功时 `hub_edpt_status_xfer()` 重新入队（变化位仍在 hub 内，下次上报再带来，自愈）。同样只在 build 目录生成副本、SDK 文件不动，SDK 内 TinyUSB ≥0.19 后自动失效。
- 描述符抓取缓冲（`desc_state_t` 内）按 TinyUSB 惯例 `CFG_TUSB_MEM_ALIGN`（4 字节）对齐。

## Known Limitations

1. **UART 带宽**：2Mbaud ≈ 200KB/s（120MHz 时钟下分频恰为整数，零波特率误差）。1kHz 鼠标全量输出（报文行 + 每报文 TUSB 日志）≈ 106KB/s，占 53%；再叠 `[HIDKIT]` 事件行（约 40B/报文）约 140KB/s。更高流量用 `cmake -DUART_BAUD=` 提速（RP2350 UART 可跑 5Mbps+）、降 `CFG_TUSB_DEBUG`，或 `-DHIDKIT_APP_EVENTS=0`。队列满丢行计数，排空后 `[DROP]` 补报。
2. **语义层只覆盖 hidkit 认识的设备**（boot 键鼠 / NKRO 键盘 / 描述符可解析的鼠标 / 布局表内手柄）。其余设备 `[HKDBG]` 报 `unhandled`、不占槽位，原始采集照旧——这是常态不是故障。**XInput 路径在本仓库只做过编译级验证**（本机没有 Xbox 手柄），握手时序与重订阅逻辑沿用**已在真机上验证过**的实现。
3. 描述符抓取与 HID 驱动自身的控制传输（报告描述符请求等）共用设备控制通道，由 TinyUSB 排队串行化；抓取失败（如设备不支持字符串）仅 `[ERROR]`/跳过，不影响报文流。
4. 多配置设备只 dump 配置 1；字符串非 ASCII 字符显示 `?`。
5. 枚举信息只在挂载时抓取一次，运行中不会重复查询（设备描述符/字符串不会变化，属有意为之）。
6. **PIO-USB 版本策略（重要）**：0.6.0 的重写带来低速支持、但破坏了 hub 上设备拔出（上游 issue #149 / TinyUSB #2971 报告人 bisect 到那对提交；上游自己认定最后一个能正确处理的是 `0f747aa`）。本仓库**固定旧血脉顶端 `9510f79` 并放弃低速支持**：`CMakeLists.txt` 会检查子模块仍是旧血脉（缺 `pio_usb_host_task` 即 `FATAL_ERROR`），`build.sh`/cmake 还会确保那枚 SDK-2 构建兼容补丁在位；`[BOOT]` 的 `piousb=` 用于核对实际刷入的提交。**另有一处与版本无关的补丁**：SDK 捆绑的 TinyUSB 0.18.0 hub 驱动一次传输失败就永久停摆（上游 PR #2994 / 0.19.0 才修好），由 configure 期生成 `hub.c` 副本补上（未打时症状：`hub_port_get_status_complete ... ASSERT FAILED` 后 hub 事件彻底断绝）。针对 0.6+ 血脉写的 9 枚补丁与整轮调查结论见 `patches/pio_usb_archive_0.6plus/`。
7. **历史调查（已归档）**：为定位 0.6+ 血脉的 hub 拔出回归，曾在库内加过"事务失败探针"并输出 `[PIODBG]` 行（记录失败事务的原始接收字节、`started`、IN/OUT/SETUP 尝试次数；判读要点：`sync/pid` 非 0 但不像合法握手 = 收到了但**锁偏错帧**，如 `01 A5` 即 `80 D2`；`00/00` = 对端无应答）。**探针已从应用移除**，补丁与全部现场结论归档在 `patches/pio_usb_archive_0.6plus/`（含未走完的定向修思路）。`[BOOT]` 行现带 `tusb=`/`hubpatch=`/`piousb=`，**排查任何 USB 异常前先核对这三个值**（曾出现"两个固件都试过但日志一样"实为刷错固件的情况）。

## Conventions

- All source comments and commit messages are in **Chinese**.
- Compiler flags: `-Wall -Wextra` with memory usage reporting via `--print-memory-usage`.
- `lib/pico_pio_usb/` 是 **git 子模块**（第三方）——不要直接改里面的文件：需要的修改写成 `patches/pio_usb/` 下的补丁，由 `scripts/apply-patches.sh` 应用（上游合并后删除补丁并把子模块升到含修复的提交）。
- `lib/hidkit/` 与 `lib/hidkit-tusb-xinput/` 同样是子模块，但它们是**自有仓库**（RiderLty/hidkit、RiderLty/hidkit-tusb-xinput）——要改就在那两个仓库里改、提交，再把本仓库的子模块指过去（`git -C lib/hidkit fetch && checkout`）。**不要在子模块工作区里留下未提交的修改**，那会让别人 clone 到的固件与你的不一致。
- hidkit 的语义层要新增事件类型时，优先在 hidkit 侧加（`hidkit_input_*` 新增出口/段前缀），本仓库只加对应的打印分支；不要在本仓库里重写解析逻辑。
- 行格式变更需同步更新 README「输出格式」表（`tools/uart_monitor.py` 对行内容透明，无格式依赖）。
