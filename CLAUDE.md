# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

USB HID 设备调试器固件，运行于 **Raspberry Pi Pico 2 (RP2350)**。

PIO-USB 端口（GPIO12/13）枚举插入的 USB 设备（含 Hub），挂载时经异步控制传输抓取并 dump：设备描述符、配置描述符（原始整块）、字符串描述符（语言 ID/厂商/产品/序列号）；HID 接口挂载时 dump 接口信息与报告描述符；运行时把每份 HID 报告按行 hexdump。**不做任何 HID 语义解析**——本固件是"所见即原始行为"的调试采集器。

原生 USB Device 栈禁用（`CFG_TUD_ENABLED=0`）：Pico 在上位机上不枚举任何设备，输出经硬件 UART0（**GPIO2=TX / GPIO3=RX，2000000bps 8N1**）。注意 RP2350 上 GPIO2/3 的 UART 复用在 FUNCSEL 11（`GPIO_FUNC_UART_AUX`），不是 RP2040 的 FUNC2。

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

每行 `\r\n` 结尾。行格式权威定义见 [README.md](README.md)「输出格式」一节。格式约定：

- TAG 定宽：`[TAG]` 紧跟 TAG 本体，其后用空格补齐到第 8 列（`TAG_COL`）再输出内容——hexdump 内用 `%*s` 补位，其余 emit 行的 TAG 均为 5 字符 + 1 空格天然对齐。
- HEX 折行 dump 统一走 `hexdump()`：每行前缀含 `len=`（整块总长）与 `off=`（本行起始偏移），前缀用空格补齐到固定列 `HEX_COL`（40）后才输出数据，跨行数据列垂直对齐。`itf_num` 传 `-1` 省略 itf 字段（设备级描述符），报文与报告描述符传实际接口号。

## Key Files

| File | Role |
|------|------|
| `src/pico_hid_debugger.c` | 入口：`main()`（core0：UART 初始化与输出循环）、`core1_main()`（core1：tuh 配置与任务循环） |
| `src/hid_host_app.c/.h` | 信息采集模块：全部 tuh 回调、描述符抓取状态机（异步控制传输链）、hexdump 格式化 |
| `src/uart_output.c/.h` | 跨核传输层：SPSC 字节块队列、UART0 初始化（GPIO2/3 @ 2000000）、core0 批量写出 |
| `src/tusb_log.c/.h` | TinyUSB 内部日志桥接：`CFG_TUSB_DEBUG_PRINTF` 挂接 `tu_printf`，片段按行组装（core1 临界区防穿插），`[TUSB]` 头入队 |
| `src/tusb_config.h` | TinyUSB 配置：仅 Host 栈（CFG_TUD_ENABLED=0），Host HID×16 + Hub，枚举缓冲 512，`CFG_TUSB_DEBUG=3` |
| `src/CMakeLists.txt` | 构建目标，链接 pico_stdlib, pico_pio_usb, tinyusb_host；stdio UART/USB 显式关闭 |
| `CMakeLists.txt` | Top-level: sets board to `pico2`, includes Pico SDK |
| `lib/pico_pio_usb/` | PIO-USB 库，**git 子模块**锁定上游 sekigon-gonnoc/Pico-PIO-USB **旧血脉顶端 `9510f79`**（0.6.0 重写之前；含 `0f747aa` "retired all transferring endpoint if device is disconnected"）。**本仓库放弃低速支持换 hub 热插拔可靠**，版本对照与理由见 `patches/pio_usb/README.md`；`[BOOT]` 行的 `piousb=` 即该提交，排查前先核对。**不要直接改子模块里的文件**：需要的修改走 `patches/pio_usb/` + `scripts/apply-patches.sh`。**注意**：D+/D− 引脚（GPIO12/13）在应用代码 `pico_hid_debugger.c` 的 `pio_cfg.pin_dp` 显式配置——上游 `PIO_USB_DP_PIN_DEFAULT` 是 GPIO0，不要依赖库内默认值 |
| `patches/pio_usb/` | 当前**只有一枚** `0001-sdk2-compat.patch`（旧血脉缺的 Pico SDK 2 构建兼容：本地 `pio_sm_set_jmp_pin` 与 SDK2 重名冲突 + 生成头缺 `pio_version` 字段）。**基线 = 子模块锁定提交 9510f79**。针对 0.6+ 血脉写的 9 枚补丁与整轮调查结论归档在 `patches/pio_usb_archive_0.6plus/`（当前不使用） |
| `scripts/apply-patches.sh` | 幂等打补丁脚本（默认应用 / `--status` / `--revert`）；`build.sh` 与 CMake 配置期都会检查补丁是否在位 |
| `tools/uart_monitor.py` | 上位机串口监视脚本（pyserial，自动探测/冻结/清屏） |
| `index.html` | Web Serial 日志查看器：**xterm.js + WebGL 渲染**（vendor/ 于 `vendor/`，UMD 挂载注意：xterm 展开式、fit/webgl 命名空间式），默认 2M，`[TUSB]` 三态过滤 + 正则内容过滤（叠加、忽略大小写，历史存 localStorage regexHist/regex，input 防抖 400ms 实时应用、回车/失焦记忆，无效红框保持上次视图），贴底跟随为 xterm 原生语义（视口 scroll 判贴底 + 回到底部角标），授权持久化 + `connect` 事件 + 100ms 轮询看门狗自动重连，ANSI 着色，模型上限 50000 行 |

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

1. **UART 带宽**：2Mbaud ≈ 200KB/s（120MHz 时钟下分频恰为整数，零波特率误差）。1kHz 鼠标全量输出（报文行 + 每报文 TUSB 日志）≈ 106KB/s，占 53%。更高流量用 `cmake -DUART_BAUD=` 提速（RP2350 UART 可跑 5Mbps+）或降 `CFG_TUSB_DEBUG`。队列满丢行计数，排空后 `[DROP]` 补报。
2. 描述符抓取与 HID 驱动自身的控制传输（报告描述符请求等）共用设备控制通道，由 TinyUSB 排队串行化；抓取失败（如设备不支持字符串）仅 `[ERROR]`/跳过，不影响报文流。
3. 多配置设备只 dump 配置 1；字符串非 ASCII 字符显示 `?`。
4. 枚举信息只在挂载时抓取一次，运行中不会重复查询（设备描述符/字符串不会变化，属有意为之）。
5. **PIO-USB 版本策略（重要）**：0.6.0 的重写带来低速支持、但破坏了 hub 上设备拔出（上游 issue #149 / TinyUSB #2971 报告人 bisect 到那对提交；上游自己认定最后一个能正确处理的是 `0f747aa`）。本仓库**固定旧血脉顶端 `9510f79` 并放弃低速支持**：`CMakeLists.txt` 会检查子模块仍是旧血脉（缺 `pio_usb_host_task` 即 `FATAL_ERROR`），`build.sh`/cmake 还会确保那枚 SDK-2 构建兼容补丁在位；`[BOOT]` 的 `piousb=` 用于核对实际刷入的提交。**另有一处与版本无关的补丁**：SDK 捆绑的 TinyUSB 0.18.0 hub 驱动一次传输失败就永久停摆（上游 PR #2994 / 0.19.0 才修好），由 configure 期生成 `hub.c` 副本补上（未打时症状：`hub_port_get_status_complete ... ASSERT FAILED` 后 hub 事件彻底断绝）。针对 0.6+ 血脉写的 9 枚补丁与整轮调查结论见 `patches/pio_usb_archive_0.6plus/`。
6. **历史调查（已归档）**：为定位 0.6+ 血脉的 hub 拔出回归，曾在库内加过"事务失败探针"并输出 `[PIODBG]` 行（记录失败事务的原始接收字节、`started`、IN/OUT/SETUP 尝试次数；判读要点：`sync/pid` 非 0 但不像合法握手 = 收到了但**锁偏错帧**，如 `01 A5` 即 `80 D2`；`00/00` = 对端无应答）。**探针已从应用移除**，补丁与全部现场结论归档在 `patches/pio_usb_archive_0.6plus/`（含未走完的定向修思路）。`[BOOT]` 行现带 `tusb=`/`hubpatch=`/`piousb=`，**排查任何 USB 异常前先核对这三个值**（曾出现"两个固件都试过但日志一样"实为刷错固件的情况）。

## Conventions

- All source comments and commit messages are in **Chinese**.
- Compiler flags: `-Wall -Wextra` with memory usage reporting via `--print-memory-usage`.
- `lib/pico_pio_usb/` 是 **git 子模块**（第三方）——不要直接改里面的文件：需要的修改写成 `patches/pio_usb/` 下的补丁，由 `scripts/apply-patches.sh` 应用（上游合并后删除补丁并把子模块升到含修复的提交）。
- 行格式变更需同步更新 README「输出格式」表（`tools/uart_monitor.py` 对行内容透明，无格式依赖）。
