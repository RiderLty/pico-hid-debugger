# pico-hid-debugger

Raspberry Pi Pico 2 (RP2350) USB HID 设备调试器固件。

仓库地址：https://github.com/RiderLty/pico-hid-debugger

在线调试终端：https://riderlty.github.io/pico-hid-debugger/

PIO-USB 端口（GPIO 12/13）枚举插入的 USB 设备：挂载时抓取并显示设备/配置/字符串描述符、HID 接口信息与报告描述符；运行时把设备的原始报文按行 hexdump 输出。原始报文不做任何语义解释，所见即设备原始行为。

在此之上**叠加**一层语义解析（`[HIDKIT]`）：同样的报文经 [hidkit](https://github.com/RiderLty/hidkit) 解析成"按下了哪个键 / 移动了多少 / 手柄轴与扳机各是多少"的事件行，Xbox 手柄（非 HID 接口）由 [hidkit-tusb-xinput](https://github.com/RiderLty/hidkit-tusb-xinput) 适配器接管后走同一个出口。**这一层是纯附加的**：原始 hexdump 一行不少，且可以整体关掉（见「[hidkit 语义层](#hidkit-语义层)」）。

原生 USB Device 栈已完全禁用——Pico 在上位机上不再枚举为任何 USB 设备，避免 Host/Device 角色混淆；全部调试信息经硬件 UART（GPIO 2/3，2000000bps）输出。

## 硬件

| 接口 | 引脚 | 用途 |
|------|------|------|
| PIO-USB | GPIO 12 (D+) / GPIO 13 (D-) | 连接被调试的 USB 设备/Hub |
| UART0 | GPIO 2 (TX) / GPIO 3 (RX) | 调试输出，2000000 8N1 |

注意：RP2350 上 GPIO2/3 的 UART 功能在 FUNCSEL 11（`GPIO_FUNC_UART_AUX`），接线时 TX/RX 交叉连接 USB-UART 适配器。

## 输出格式

每行 `\r\n` 结尾，`[TAG]` 定界。格式约定：

- **TAG 定宽**：`[TAG]` 中括号紧跟 TAG 本体，其后用空格补齐到**第 8 列**再输出内容，所有行的内容列对齐（`[HID]` 补 3 格、`[DROP]` 补 2 格、`[MOUNT]` 补 1 格）。唯二的例外是 `[HIDKIT]`/`[HKDBG]`：TAG 本身就有 6/5 个字符，内容列统一放在**第 9 列**（彼此对齐，与其余行差一格）；
- **HEX 转储行**（`DEVDS`/`CFGDS`/`RPTDS`/`HID`）前缀含 `len=`（整块总长）与 `off=`（本行起始偏移），前缀用空格补齐到**固定第 40 列**后才输出数据，每行 16 字节——超长报文（如 DS5 手柄 64 字节报告）跨行数据列严格垂直对齐。

| 行格式 | 含义 |
|--------|------|
| `[BOOT]  system init: pico-hid-debugger uart=2000000 8N1 tusb=0.18.0 hubpatch=1 piousb=9510f79 hkdbg=1 hkevt=1` | 启动标记（开机第一条输出，先于任何 TinyUSB 日志；见到它即知本连接从系统启动起完整抓取）。`tusb=` 是实际链接的 TinyUSB 版本，`hubpatch=` 表示 hub 驱动韧性补丁是否生效，**`piousb=` 是 PIO-USB 子模块当前提交**——排查前先核对这三个值，避免"日志看着一样、其实刷的是上一版固件"。`hkdbg=`/`hkevt=` 是 hidkit 语义层的两个编译期开关（库内诊断 / 事件行）：**"为什么没有 `[HIDKIT]` 行"先看这两个值**，与"刷错固件"是同一类误判 |
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
| `[HIDKIT] key slot=%d code=0x%04X down\|up` | **语义事件**（键/鼠标按键/手柄按键统一出口，`code` 的段前缀区分类型：`0x00xx` 键盘 HID Usage ID、`0x01xx` 鼠标按键序号、`0x02xx` 手柄 `BTN_*`）。**仅在状态变化时**输出，紧跟触发它的 `[HID]` 行之后 |
| `[HIDKIT] mouse slot=%d dx=%d dy=%d wheel=%d` | 鼠标位移与滚轮（仅非零时输出） |
| `[HIDKIT] pad slot=%d ls=%d,%d rs=%d,%d lt=%d rt=%d` | 手柄绝对状态。**每份解析成功的报文都输出**（手柄报文本身就是当前绝对状态，不去重），故 1kHz 手柄下这行是持续的 |
| `[HIDKIT] dropped slot=%d vid=%04X pid=%04X` | 设备被丢弃（槽位耗尽且策略为 `DROP_NEW`；本固件用默认的 `EVICT_IDLE`，走不到） |
| `[HKDBG]  hidkit: ...` | **库内诊断**：库"自己怎么想的"——认领了哪个槽位、为什么不认、槽位被谁挤出、卸载、报文首次未被消费。只在冷路径打点，不随报文速率增长。判读"设备动作为什么没出来"看这行 |
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

## hidkit 语义层

原始 hexdump 之上**叠加**的一层语义解析，由两个独立仓库提供（都以 git 子模块挂在 `lib/` 下，本仓库只读不改）：

| 子模块 | 角色 |
|--------|------|
| [`lib/hidkit`](https://github.com/RiderLty/hidkit) | **解析核心**：报告描述符 → 字段表，报文 → 键/鼠标/手柄事件。纯 C、零平台依赖、static 内存、无堆分配，不认识任何 USB 栈 |
| [`lib/hidkit-tusb-xinput`](https://github.com/RiderLty/hidkit-tusb-xinput) | **XInput 适配器**：Xbox 手柄没有 HID 接口，无法靠描述符判定，必须自己跑端点管线与 LED/震动初始化握手，再把厂商报文解析成归一化 pad 交给 core |

接线全部在 [`src/hidkit_app.c`](src/hidkit_app.c)：把 TinyUSB 的三个 HID 回调转发给 hidkit 的 `hidkit_mount()`/`hidkit_report()`/`hidkit_umount()`，并把库的四个弱符号出口函数实现成 `[HIDKIT]` 行、库内诊断出口实现成 `[HKDBG]` 行。XInput 侧只多一件事：`usbh_app_driver_get_cb()` **由本文件定义**（Host 栈里唯一的应用类驱动注册钩子，适配器刻意不定义，避免抢符号），转发给适配器的驱动。

### 一段真实的事件流

下面这段不是手写的，是用**本固件链接的同一份 hidkit** 在主机侧跑"描述符 + 报文"得到的（探针按 `hidkit_app.c` 的行格式打印）：

```
[HKDBG]  hidkit: slot 0 <- 046d:c52b proto=1 kind=2      # 认领键盘（boot 协议，无需描述符）
[HIDKIT] key slot=0 code=0x0004 down                     # 按下 A（HID Usage 0x04）
[HIDKIT] key slot=0 code=0x0004 up                       # 抬起
[HKDBG]  hidkit: slot 1 <- 046d:c52b proto=2 kind=1      # 认领鼠标（按报告描述符解析）
[HIDKIT] mouse slot=1 dx=5 dy=-5 wheel=0                 # 向右 5、向上 5
[HIDKIT] key slot=1 code=0x0100 up                       # 鼠标左键抬起
[HKDBG]  hidkit: unhandled 046d:c52b proto=0 desc=none   # 不认识：不占槽位，原始采集照旧
```

读日志时两个容易误判的点：

- `[HIDKIT]` 行**紧跟**触发它的 `[HID]` 行（原始在前、语义在后），对不上就是"读错了"，两行对着看即可定位；
- **鼠标按键的首帧只建基线、不发边沿**（"我按了左键却只有一条 `[HID]`"多半是这个）；键盘路径无此行为。

### 两个开关（都是编译期的）

| CMake 变量 | 宏 | 默认 | 作用 |
|---|---|---|---|
| `HIDKIT_APP_EVENTS` | `HIDKIT_APP_EVENTS` | 1 | `[HIDKIT]` 事件行。置 0 只留原始 hexdump 与 `[HKDBG]`（1kHz 设备下省 UART 带宽） |
| `HIDKIT_LIB_DEBUG` | `HIDKIT_DEBUG` | 1 | `[HKDBG]` 库内诊断行。置 0 **连调用点一起编译掉**（连字符串都不进固件，零开销） |

```bash
cmake -S . -B build -DHIDKIT_APP_EVENTS=0    # 只看原始报文
cmake -S . -B build -DHIDKIT_LIB_DEBUG=0     # 只关库内诊断
```

两个开关都写进 `[BOOT]` 行的 `hkevt=`/`hkdbg=`：**"为什么没有 `[HIDKIT]` 行"先看它**，别猜。整体想退回"纯采集器"，把 `hidkit_app.c` 从 `src/CMakeLists.txt` 的源列表里去掉即可 —— 原始采集不依赖它。

### 容量

`src/CMakeLists.txt` 给到 4 键盘 / 4 鼠标 / 4 手柄（共 12 槽位，≤ `CFG_TUH_HID`=16，不会出现"枚举得到却拿不到槽位"）。槽位耗尽时按 hidkit 默认策略 `EVICT_IDLE` 挤掉最久没有报文的设备，并**先补发"全部抬起"**，`[HKDBG]` 里会留一行 `evict slot N`。要改就改那几个 `HIDKIT_MAX_*` 编译定义。

## 构建

```bash
git clone https://github.com/RiderLty/pico-hid-debugger.git
cd pico-hid-debugger
./build.sh    # 一键构建：自动初始化子模块 + 打补丁 + 探测 SDK（../pico-sdk 或 ~/pico-sdk）
```

`build.sh` 会顺带完成两件初始化工作（幂等，可反复执行），因此 `git clone` 后无需任何手工配置：

1. `git submodule update --init --recursive` —— 拉取三个子模块：`lib/pico_pio_usb`（PIO-USB 库，固定在**旧血脉顶端 `9510f79`**，0.6.0 重写之前，含 hub 拔出修复；本仓库放弃低速支持，见下）、`lib/hidkit`（解析核心）与 `lib/hidkit-tusb-xinput`（XInput 适配器）；
2. `./scripts/apply-patches.sh` —— 给 PIO-USB 子模块打上补丁（两枚：SDK 2 构建兼容、device SE0 超时，见下）。

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
├── hidkit_app.c/.h       # 语义层接线：hidkit 出口函数 → [HIDKIT]/[HKDBG] 行；XInput 类驱动注册
├── uart_output.c/.h      # 跨核 SPSC 队列 → UART0（GPIO2/3，2000000）
├── tusb_log.c/.h         # TinyUSB 内部日志桥接：tu_printf 钩子 → [TUSB] 行
├── tusb_config.h         # TinyUSB 配置（仅 Host 栈 + 调试日志级别）
└── CMakeLists.txt        # 构建配置
tools/
└── uart_monitor.py       # 上位机串口监视脚本
index.html                # Web Serial 日志查看器（xterm.js + WebGL，[TUSB] 过滤）
vendor/                   # xterm.js 及 WebGL/Fit 插件（第三方，vendored）
lib/
├── pico_pio_usb/         # PIO-USB 库（git 子模块，锁定旧血脉顶端 9510f79；放弃低速支持）
├── hidkit/               # HID/XInput 解析核心（git 子模块，独立开源项目）
└── hidkit-tusb-xinput/   # XInput 类驱动 + 归一化接线（git 子模块，独立开源项目）
patches/
└── pio_usb/              # 上游未合并修复的补丁 + 说明（README.md）
scripts/
└── apply-patches.sh      # 幂等打补丁脚本（build.sh 会自动调用）
```

## 已知限制

1. UART 无流控：2Mbaud 容量约 200KB/s。1kHz 鼠标全量输出（报文行 66B + 级别 3 的每报文 TUSB 日志约 40B ≈ 106KB/s）约占 53%；再叠上 `[HIDKIT]` 事件行（约 40B/报文）会到 ~140KB/s。此前 921600（92KB/s）即因此溢出丢行。更高流量可用 `cmake -DUART_BAUD=` 提速（RP2350 UART 可跑 5Mbps+），或用 `-DHIDKIT_APP_EVENTS=0` 只留原始报文。队列满即丢行并计数，排空后以 `[DROP]` 补报。
2. 报告描述符超过 TinyUSB 枚举缓冲（512 字节）时显示 `[RPTDS] not captured`（此时 hidkit 也拿不到描述符：`[HKDBG]` 会给出 `unhandled ... desc=none`）。
3. 多配置设备只转储配置 1。
4. 字符串描述符按 UTF-16LE 低字节转可打印 ASCII，非 ASCII 字符显示 `?`。
5. **语义层只覆盖 hidkit 认识的设备**：boot 键鼠、NKRO 键盘、靠报告描述符解析的鼠标、以及布局表里已知的手柄（DS5/Azeron/XInput 等）。其余设备 `[HKDBG]` 会明确说 `unhandled`，原始 hexdump 不受影响 —— 这正是本固件的常态，不是故障。
6. **XInput 路径（`lib/hidkit-tusb-xinput`）在本仓库只做编译级验证**：本机无 Xbox 手柄，握手时序与重订阅逻辑沿用同源实现（该实现在真机上跑过），但**本固件上未经实机验证**。

## 许可证

见 [LICENSE](LICENSE)。
