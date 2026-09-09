# pico-hid-debugger

Raspberry Pi Pico 2 (RP2350) 输入转发固件。

PIO-USB 端连接的键盘/鼠标解析后按运行时选择的目的地输出：
- **设备转发模式**（开机默认）：写入原生 USB 上模拟的 HID 键盘+鼠标，Pico 成为硬件输入代理——接在 Pico 上的键鼠就是"插在上位机上"的键鼠
- **串口输出模式**：经 CDC 串口输出到上位机（文本行或 0x55AA 二进制帧格式）

## 命令

向 CDC 串口发送单字符即可切换（即刻生效）：

| 字符 | 作用 |
|------|------|
| `D` | 切换到设备转发模式（模拟键鼠，开机默认） |
| `S` | 切换到串口输出模式 |
| `T` | 文本格式并切到串口 |
| `B` | 二进制格式并切到串口 |
| `M` | Mac 模式：修饰键 LALT↔LGUI、RALT↔RGUI 交换，Ctrl 不变（**开机默认**） |
| `W` | Windows 模式：修饰键不交换 |

串口与模拟键鼠互斥，同一时刻只有一个目的地；Mac/Windows 修饰键交换则作用于**所有输出**（串口与模拟键鼠）。从设备模式切走时自动补发全零报告，避免上位机残留按住的键。

## 输出格式

### 文本模式（默认，core_input 风格事件行）

```
[MOUNT ] vid=046d pid=c52b dev=2 itf=1 proto=Keyboard   ← 设备挂载
KEY:E0 DN LCTRL          ← 左Ctrl按下（修饰键附名称）
KEY:04 DN                ← 'a' 按下
KEY:04 UP
MOUSE:DX=-12 DY=+5 WH=0 BTN=---   ← 鼠标移动帧（BTN= 当前按住的键）
MOUSE:BTN:L DN           ← 鼠标左键按下
HID:[A1 02 3C FF .. +12] ← 未识别设备原始 HEX（超20字节截断标注）
[UMOUNT] vid=046d pid=c52b dev=2 itf=1            ← 设备拔出
```

### 二进制模式（0x55 0xAA 帧）

```
紧凑帧（键盘/鼠标）：  55 AA | len | type | payload
带身份帧（其余类型）：  55 AA | len | type | PID u16le | VID u16le | port u8 | payload

len = 自本字段之后至帧尾的字节数；port = USB 设备地址 dev_addr（hub 下每设备唯一）
payload ≤ 64B
```

type 与 payload：
- `0x01` 键盘（紧凑帧）— **原始报文**（mod + keys）
- `0x02` 鼠标（紧凑帧）— **归一化解析结果**重新打包，6 字节：`buttons u8, wheel i8, x i16le, y i16le`
- `0x00` 其他 HID 设备（带身份帧）— 原始报文
- `0x10/0x11` 挂载/拔出、`0x12` 队列溢出补报、`0x13` 错误（均带身份帧；无设备来源的 PID/VID/port 填全零）

键盘边沿、鼠标按键按下/松开由上位机自行比对帧数据得出；固件侧不做边沿检测。

## 硬件

| 接口 | 引脚 | 用途 |
|------|------|------|
| 原生 USB | 板载 Type-C | 连接目标主机（CDC 串口 + 模拟键鼠） |
| PIO-USB | GPIO 12 (D+) / GPIO 13 (D-) | 连接实体 USB 键鼠/Hub |

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

刷入后用串口工具打开 CDC 端口即可（波特率任意，USB CDC 不依赖波特率）：

```bash
ls /dev/tty.usbmodem*        # macOS
screen /dev/tty.usbmodemXXXX 115200
```

插入键盘/鼠标后先出现挂载信息，随后每次按键/移动/点击都会输出事件（设备转发模式下串口静默，发 `T` 切回串口文本）。全部命令见上文[命令](#命令)一节。

### 上位机监视脚本

```bash
pip install pyserial                       # 首次使用装依赖
python3 tools/cdc_monitor.py               # 自动探测串口并连接
python3 tools/cdc_monitor.py -p /dev/tty.usbmodemXXXX --binary  # 指定串口、二进制起步
python3 tools/cdc_monitor.py --selftest    # 协议解码自测（无需硬件）
```

运行中单键切换：`d`=设备转发模式、`b`=二进制、`t`=文本、`m`/`w`=Mac/Windows 修饰键交换、`q`=退出。二进制模式下每帧同时显示解析结果与原始十六进制，例如：

```
[MOUSE] BTN=L WH=-1 DX=-12 DY=+345 | HEX: 55 AA 07 02 01 FF F4 FF 59 01
[UMOUNT] dev=3 vid=cafe pid=4001 UMOUNT itf=7 | HEX: 55 AA 07 11 01 40 FE CA 03 07
```

## 目录结构

```
src/
├── pico_hid_debugger.c        # 入口：双核初始化、CDC RX 命令（D/S/T/B/M/W）
├── hid_host_app.c/.h          # HID 报文处理：tuh 回调、描述符解析、统一事件产出
├── hid_output.c/.h            # 中间层：目的地 + 串口格式选层（static 变量，运行时可切）
├── out_text.c                 # 串口文本输出实现（含按键边沿检测）
├── out_binary.c               # 串口二进制输出实现（0x55AA 帧）
├── out_device.c/.h            # 设备转发实现（模拟键鼠）
├── cdc_output.c/.h            # 跨核 SPSC 队列，按 kind 分发到 CDC/模拟 HID
├── hid_parser.c/.h            # HID 报告描述符解析器（鼠标归一化提取）
├── tusb_config.h              # TinyUSB 配置
├── usb_descriptors.c          # USB 描述符（CDC + 模拟键盘/鼠标）
└── CMakeLists.txt             # 构建配置
tools/
└── cdc_monitor.py             # 上位机串口监视脚本
lib/
└── pico_pio_usb/              # PIO-USB 库（第三方）
```

## 许可证

见 [LICENSE](LICENSE)。
