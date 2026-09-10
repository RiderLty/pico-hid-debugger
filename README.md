# pico-hid-debugger

Raspberry Pi Pico 2 (RP2350) USB HID 设备调试器固件。

仓库地址：https://github.com/RiderLty/pico-hid-debugger

在线调试终端：https://riderlty.github.io/pico-hid-debugger/

PIO-USB 端口（GPIO 12/13）枚举插入的 USB 设备：挂载时抓取并显示设备/配置/字符串描述符、HID 接口信息与报告描述符；运行时把设备的原始报文按行 hexdump 输出。不做任何 HID 语义解析，所见即设备原始行为。

原生 USB Device 栈已完全禁用——Pico 在上位机上不再枚举为任何 USB 设备，避免 Host/Device 角色混淆；全部调试信息经硬件 UART（GPIO 2/3，2000000bps）输出。

## 硬件

| 接口 | 引脚 | 用途 |
|------|------|------|
| PIO-USB | GPIO 12 (D+) / GPIO 13 (D-) | 连接被调试的 USB 设备/Hub |
| UART0 | GPIO 2 (TX) / GPIO 3 (RX) | 调试输出，2000000 8N1 |

注意：RP2350 上 GPIO2/3 的 UART 功能在 FUNCSEL 11（`GPIO_FUNC_UART_AUX`），接线时 TX/RX 交叉连接 USB-UART 适配器。

## 输出格式

每行 `\r\n` 结尾，`[TAG]` 定界。格式约定：

- **TAG 定宽**：`[TAG]` 中括号紧跟 TAG 本体，其后用空格补齐到**第 8 列**再输出内容，所有行的内容列对齐（`[HID]` 补 3 格、`[DROP]` 补 2 格、`[MOUNT]` 补 1 格）；
- **HEX 转储行**（`DEVDS`/`CFGDS`/`RPTDS`/`HID`）前缀含 `len=`（整块总长）与 `off=`（本行起始偏移），前缀用空格补齐到**固定第 40 列**后才输出数据，每行 16 字节——超长报文（如 DS5 手柄 64 字节报告）跨行数据列严格垂直对齐。

| 行格式 | 含义 |
|--------|------|
| `[BOOT]  system init: pico-hid-debugger uart=2000000 8N1 tusb=0.18.0 hubpatch=1 piousb=9510f79` | 启动标记（开机第一条输出，先于任何 TinyUSB 日志；见到它即知本连接从系统启动起完整抓取）。`tusb=` 是实际链接的 TinyUSB 版本，`hubpatch=` 表示 hub 驱动韧性补丁是否生效，**`piousb=` 是 PIO-USB 子模块当前提交**——排查前先核对这三个值，避免"日志看着一样、其实刷的是上一版固件" |
| `[PIODBG] kind=SETUP ep=00 res=-1 started=0 sync=01 pid=A5 retry=0 att=21111/20/39 seq=31` | **事务失败探针**（诊断用；三类事务各留一份不会被覆盖的样本，每类每 200ms 最多一行）。**判读看 `sync`/`pid`**：`sync=00 pid=00` = 真的什么都没收到（对端无应答）；非 0 但不像合法握手 = 收到了但**锁偏错帧**（把 `sync<<1\|carry` 还原即可，例：`01 A5` 就是 `80 D2` = SYNC+ACK）。`att=IN/OUT/SETUP` 为累计尝试次数 |
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
git clone https://github.com/RiderLty/pico-hid-debugger.git
cd pico-hid-debugger
./build.sh    # 一键构建：自动初始化子模块 + 打补丁 + 探测 SDK（../pico-sdk 或 ~/pico-sdk）
```

`build.sh` 会顺带完成两件初始化工作（幂等，可反复执行），因此 `git clone` 后无需任何手工配置：

1. `git submodule update --init --recursive` —— 拉取 PIO-USB 库。`lib/pico_pio_usb` 是 **git 子模块**，锁定在上游提交 `5a37a66`；
2. `./scripts/apply-patches.sh` —— 给子模块打上补丁（当前只有 SDK 2 构建兼容那一枚，见下）。

或手动构建（需要 PICO_SDK_PATH、ARM 交叉编译器、CMake >= 3.13）：

```bash
export PICO_SDK_PATH=/path/to/pico-sdk

git submodule update --init --recursive
./scripts/apply-patches.sh

mkdir build && cd build
cmake ..
make -j$(nproc)
```

> **关于 PIO-USB 版本（重要）**：本仓库把 `lib/pico_pio_usb` **固定在旧血脉（0.6.0 重写之前）的顶端提交 `9510f79`**，并**放弃低速（LS）设备支持**——因为上游 0.6.0 的重写在带来"低速经 hub 可用"的同时，破坏了"hub 上设备拔出"的处理（上游 issue [#149](https://github.com/sekigon-gonnoc/Pico-PIO-USB/issues/149)、[TinyUSB #2971](https://github.com/hathach/tinyusb/issues/2971) 报告人 bisect 出的正是那对提交）。`9510f79` 只比 0.5.3 发布版多 4 个提交，且**包含上游 `0f747aa` "retired all transferring endpoint if device is disconnected"** —— 即上游自己认定的"最后一个能正确处理 hub 拔出的提交"。
> 为什么不是简单回退：旧血脉**从未**拿到 Pico SDK 2 的构建兼容（那两笔修复都在重写之后），因此有**一枚** `patches/pio_usb/0001-sdk2-compat.patch`（纯构建兼容，无语义改动，由 `./scripts/apply-patches.sh` 幂等应用；`git submodule update --checkout` 清掉后重跑即可，cmake 配置期也会检测并提示）。针对新血脉写的 9 枚补丁与整轮调查结论**已归档**在 [`patches/pio_usb_archive_0.6plus/`](patches/pio_usb_archive_0.6plus/)，全部说明见 [`patches/pio_usb/README.md`](patches/pio_usb/README.md)。
>
> **另一处补丁**（与上面无关，继续保留）：**SDK 捆绑的 TinyUSB 0.18.0**（`src/host/hub.c`）在 hub 端口变化流程里，任何一次传输失败就**永久放弃**（`hub_xfer_cb` 用 `TU_VERIFY` 提前返回 → 状态轮询不再入队；五处完成回调 `TU_ASSERT` 直接断言停摆），而 pio-usb 这类 HCD 出现事务级错误是常态。上游已在 **TinyUSB PR #2994**（0.19.0 起）改为失败即重新入队轮询。该补丁在 **configure 期自动**生成 `hub.c` 修正副本到 build 目录并替换源列表（与既有的 `hid_host.c` 三级日志补丁同一手法），**SDK 文件始终原样**；SDK 内 TinyUSB 升到 ≥ 0.19.0 后自动失效，无需手工步骤。

可选：`UART_BAUD=921600 ./build.sh` 覆盖 UART 波特率（默认 2M，须与上位机一致且适配器支持）。

输出：`build/src/pico-hid-debugger.uf2`

按住 BOOTSEL 按钮插入 Pico，将 `.uf2` 拷贝到出现的 U 盘即可刷入。

## 测试

1. USB-UART 适配器接 GPIO2(TX)/GPIO3(RX)（交叉接线），打开串口终端，波特率 2000000：

```bash
ls /dev/tty.usbserial*           # macOS
screen /dev/tty.usbserialXXXX 2000000
```

2. 被调试设备接 PIO-USB 端口（GPIO12/13，需外部 5V 供电与 D+ 1.5kΩ 上拉，或经 Hub）。插入后立即输出挂载信息与描述符 dump，随后每次报文一行。

### 上位机监视脚本

```bash
pip install pyserial                       # 首次使用装依赖
python3 tools/uart_monitor.py              # 自动探测串口并连接
python3 tools/uart_monitor.py -p /dev/tty.usbserialXXXX -b 2000000  # 指定串口
```

运行中单键：`f` = 冻结/恢复滚动、`c` = 清屏、`q` = 退出。

### Web 日志查看器

根目录的 [index.html](index.html) 是基于 Web Serial + **xterm.js（WebGL 渲染，GPU 加速）** 的终端式查看器（Chrome/Edge，需 https 或 localhost；库文件已 vendor 在 `vendor/`，无需联网）：

```bash
python3 -m http.server   # 工程根目录运行，浏览器访问 http://localhost:8000/
```

- 过滤选择：全部日志 / 仅 `[TUSB]` / 排除 `[TUSB]`，切换即时重写终端缓冲；
- **正则过滤**：过滤选择右侧的输入框按正则匹配整行内容（忽略大小写，与 TAG 过滤叠加），实时生效；回车记忆进历史（localStorage 持久化，输入时自动补全），`▾` 菜单可应用/删除/清空历史；无效表达式红框提示并保持上次有效过滤；
- 终端级跟随语义：贴底时自动跟随输出，上滚查看历史时新输出不拖动视口，"回到底部"角标一键恢复；
- 自动重连：授权一次后，设备断开重插（含刷固件）会在重枚举瞬间自动恢复连接，全程无需再次确认；
- WebGL 不可用时自动回退 DOM 渲染器；`?renderer=dom` 可强制禁用 WebGL；
- 日志按 TAG 用 ANSI 着色，模型上限 50000 行（导出/过滤的数据源），导出为带过滤模式与时间戳的 `.log` 文件。

## 目录结构

```
src/
├── pico_hid_debugger.c   # 入口：双核初始化（core1=USB Host，core0=UART 输出）
├── hid_host_app.c/.h     # 信息采集：TinyUSB 回调、描述符抓取状态机、报文 hexdump
├── uart_output.c/.h      # 跨核 SPSC 队列 → UART0（GPIO2/3，2000000）
├── tusb_log.c/.h         # TinyUSB 内部日志桥接：tu_printf 钩子 → [TUSB] 行
├── tusb_config.h         # TinyUSB 配置（仅 Host 栈 + 调试日志级别）
└── CMakeLists.txt        # 构建配置
tools/
└── uart_monitor.py       # 上位机串口监视脚本
index.html                # Web Serial 日志查看器（xterm.js + WebGL，[TUSB] 过滤）
vendor/                   # xterm.js 及 WebGL/Fit 插件（第三方，vendored）
lib/
└── pico_pio_usb/         # PIO-USB 库（git 子模块，锁定旧血脉顶端 9510f79；放弃低速支持）
patches/
└── pio_usb/              # 上游未合并修复的补丁 + 说明（README.md）
scripts/
└── apply-patches.sh      # 幂等打补丁脚本（build.sh 会自动调用）
```

## 已知限制

1. UART 无流控：2Mbaud 容量约 200KB/s。1kHz 鼠标全量输出（报文行 66B + 级别 3 的每报文 TUSB 日志约 40B ≈ 106KB/s）约占 53%；此前 921600（92KB/s）即因此溢出丢行。更高流量可用 `cmake -DUART_BAUD=` 提速（RP2350 UART 可跑 5Mbps+）。队列满即丢行并计数，排空后以 `[DROP]` 补报。
2. 报告描述符超过 TinyUSB 枚举缓冲（512 字节）时显示 `[RPTDS] not captured`。
3. 多配置设备只转储配置 1。
4. 字符串描述符按 UTF-16LE 低字节转可打印 ASCII，非 ASCII 字符显示 `?`。

## 许可证

见 [LICENSE](LICENSE)。
