# pico-hid-debugger

Raspberry Pi Pico 2 (RP2350) USB HID 设备调试器固件。

PIO-USB 端口（GPIO 12/13）枚举插入的 USB 设备：挂载时抓取并显示设备/配置/字符串描述符、HID 接口信息与报告描述符；运行时把设备的原始报文按行 hexdump 输出。不做任何 HID 语义解析，所见即设备原始行为。

原生 USB Device 栈已完全禁用——Pico 在上位机上不再枚举为任何 USB 设备，避免 Host/Device 角色混淆；全部调试信息经硬件 UART（GPIO 2/3，921600bps）输出。

## 硬件

| 接口 | 引脚 | 用途 |
|------|------|------|
| PIO-USB | GPIO 12 (D+) / GPIO 13 (D-) | 连接被调试的 USB 设备/Hub |
| UART0 | GPIO 2 (TX) / GPIO 3 (RX) | 调试输出，921600 8N1 |

注意：RP2350 上 GPIO2/3 的 UART 功能在 FUNCSEL 11（`GPIO_FUNC_UART_AUX`），接线时 TX/RX 交叉连接 USB-UART 适配器。

## 输出格式

每行 `\r\n` 结尾，`[TAG]` 定界。格式约定：

- **TAG 定宽**：`[TAG]` 中括号紧跟 TAG 本体，其后用空格补齐到**第 8 列**再输出内容，所有行的内容列对齐（`[HID]` 补 3 格、`[DROP]` 补 2 格、`[MOUNT]` 补 1 格）；
- **HEX 转储行**（`DEVDS`/`CFGDS`/`RPTDS`/`HID`）前缀含 `len=`（整块总长）与 `off=`（本行起始偏移），前缀用空格补齐到**固定第 40 列**后才输出数据，每行 16 字节——超长报文（如 DS5 手柄 64 字节报告）跨行数据列严格垂直对齐。

| 行格式 | 含义 |
|--------|------|
| `[MOUNT] dev=%u vid=%04x pid=%04x` | 设备枚举完成（含 hub 设备自身） |
| `[DEVDS] dev=%u vid=... bcdUSB=... cls=.. pkt0=... cfgs=...` | 设备描述符关键字段 |
| `[DEVDS] dev=%u len=18 off=..: <hex>` | 设备描述符原始转储 |
| `[CFGDS] dev=%u total=%u itfs=%u cfg=%u attr=0x%02x power=%umA` | 配置描述符关键字段 |
| `[CFGDS] dev=%u len=%u off=..: <hex>` | 配置描述符原始转储（含全部接口/端点/HID 描述符） |
| `[STRDS] dev=%u langid=0x%04x` | 支持的语言 ID |
| `[STRDS] dev=%u Mfg(1)="..." / Prod(2)="..." / Ser(3)="..."` | 字符串描述符（UTF-16 转可打印 ASCII） |
| `[HIDMT] dev=%u vid=%04x pid=%04x itf=%u proto=%s cls=%02x sub=%02x eps=%u` | HID 接口挂载（proto: None/Keyboard/Mouse） |
| `[RPTDS] dev=%u itf=%u len=%u off=..: <hex>` | HID 报告描述符原始转储 |
| `[HID]   dev=%u itf=%u len=%u off=%u: <hex>` | **原始报文**（超 16 字节折行，`off=` 递增标注行内偏移） |
| `[UNHID] dev=%u itf=%u` | HID 接口拔出 |
| `[DEVRM] dev=%u` | 设备移除 |
| `[ERROR] dev=%u ...` | 描述符抓取失败 / 报告订阅失败等 |
| `[DROP]  lost_lines=%lu` | UART 队列溢出丢弃量补报 |
| `[TUSB] <TinyUSB 内部日志>` | TinyUSB 栈日志（枚举过程/传输错误等，级别见 `tusb_config.h` 的 `CFG_TUSB_DEBUG`，1=错误 2=+警告 3=+信息），上位机按 `[TUSB]` 头即可单独筛选 |

挂载时序示例（内容列对齐在第 8 列，HEX 数据列对齐在第 40 列）：

```
[MOUNT] dev=2 vid=046d pid=c52b
[DEVDS] dev=2 vid=046d pid=c52b bcdUSB=0210 cls=00/00/00 pkt0=64 bcdDev=2700 cfgs=1
[DEVDS] dev=2 iMfg=1 iProd=2 iSer=3
[DEVDS] dev=2 len=18 off=0:              12 01 10 02 00 00 00 40 6D 04 2B C5 00 27 01 02
[DEVDS] dev=2 len=18 off=16:             03 01
[CFGDS] dev=2 total=59 itfs=1 cfg=1 attr=0xA0 power=50mA
[CFGDS] dev=2 len=59 off=0:              09 02 3B 00 01 01 00 A0 FA 09 04 00 00 01 03 01
[STRDS] dev=2 langid=0x0409
[STRDS] dev=2 Mfg(1)="Logitech"
[STRDS] dev=2 Prod(2)="USB Receiver"
[HIDMT] dev=2 vid=046d pid=c52b itf=0 proto=Mouse cls=03 sub=01 eps=1
[RPTDS] dev=2 itf=0 len=67 off=0:        05 01 09 02 A1 01 09 01 A1 00 05 09 19 01 29 08
[HID]   dev=2 itf=0 len=8 off=0:         01 00 00 00 00 00 00 00
[HID]   dev=2 itf=0 len=64 off=0:        21 8F F2 04 69 DD FB 01 00 00 00 01 00 00 00 00
[HID]   dev=2 itf=0 len=64 off=16:       00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
[HID]   dev=2 itf=0 len=64 off=32:       00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
[HID]   dev=2 itf=0 len=64 off=48:       00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

## 构建

```bash
# 需要 PICO_SDK_PATH、ARM 交叉编译器、CMake >= 3.13
export PICO_SDK_PATH=/path/to/pico-sdk

mkdir build && cd build
cmake ..
make -j$(nproc)
```

输出：`build/src/pico-hid-debugger.uf2`

按住 BOOTSEL 按钮插入 Pico，将 `.uf2` 拷贝到出现的 U 盘即可刷入。

## 测试

1. USB-UART 适配器接 GPIO2(TX)/GPIO3(RX)（交叉接线），打开串口终端，波特率 921600：

```bash
ls /dev/tty.usbserial*           # macOS
screen /dev/tty.usbserialXXXX 921600
```

2. 被调试设备接 PIO-USB 端口（GPIO12/13，需外部 5V 供电与 D+ 1.5kΩ 上拉，或经 Hub）。插入后立即输出挂载信息与描述符 dump，随后每次报文一行。

### 上位机监视脚本

```bash
pip install pyserial                       # 首次使用装依赖
python3 tools/uart_monitor.py              # 自动探测串口并连接
python3 tools/uart_monitor.py -p /dev/tty.usbserialXXXX -b 921600  # 指定串口
```

运行中单键：`f` = 冻结/恢复滚动、`c` = 清屏、`q` = 退出。

### Web 日志查看器

根目录的 [index.html](index.html) 是基于 Web Serial API 的网页查看器（Chrome/Edge，需 https 或 localhost）：

```bash
python3 -m http.server   # 工程根目录运行，浏览器访问 http://localhost:8000/
```

- 过滤选择：全部日志 / 仅 `[TUSB]` / 排除 `[TUSB]`，切换即时生效；
- 自动重连：授权一次后，设备断开重插（含刷固件）会在重枚举瞬间自动恢复连接，全程无需再次确认；
- 断开重连后的半行由行缓冲自动拼接；日志按 TAG 着色，上限 8000 行。

## 目录结构

```
src/
├── pico_hid_debugger.c   # 入口：双核初始化（core1=USB Host，core0=UART 输出）
├── hid_host_app.c/.h     # 信息采集：TinyUSB 回调、描述符抓取状态机、报文 hexdump
├── uart_output.c/.h      # 跨核 SPSC 队列 → UART0（GPIO2/3，921600）
├── tusb_log.c/.h         # TinyUSB 内部日志桥接：tu_printf 钩子 → [TUSB] 行
├── tusb_config.h         # TinyUSB 配置（仅 Host 栈 + 调试日志级别）
└── CMakeLists.txt        # 构建配置
tools/
└── uart_monitor.py       # 上位机串口监视脚本
index.html                # Web Serial 日志查看器（自动重连 / [TUSB] 过滤）
lib/
└── pico_pio_usb/         # PIO-USB 库（第三方）
```

## 已知限制

1. UART 无流控：921600bps 约 11.5KB/s，hexdump 使字节膨胀 3 倍，高流量设备（连续移动报文、大报告）可能超出带宽，队列满即丢行并计数，排空后以 `[DROP]` 补报。`CFG_TUSB_DEBUG=3` 时 TinyUSB 信息日志会显著加大流量。
2. 报告描述符超过 TinyUSB 枚举缓冲（512 字节）时显示 `[RPTDS] not captured`。
3. 多配置设备只转储配置 1。
4. 字符串描述符按 UTF-16LE 低字节转可打印 ASCII，非 ASCII 字符显示 `?`。

## 许可证

见 [LICENSE](LICENSE)。
