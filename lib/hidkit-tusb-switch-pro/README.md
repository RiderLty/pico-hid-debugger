# hidkit-tusb-switch-pro

Switch Pro USB 握手、IMU 开启和 SPI 校准读取库，MIT 许可。整个目录可以复制到
其他工程，拥有自己的 CMake 配置，不依赖本调试器的源文件。

支持 Nintendo `057e:2009`（Pro）及 `057e:2006/2007`（Joy-Con USB 接口）。
USB 握手按 `80 02 → 80 03 → 80 02 → 80 04` 的时间步进执行，随后通过
`0x01` 子命令开启震动、`0x30` 全量报告、IMU。初始化子命令等待成功 ACK，
约 105ms 超时重发，最多发送 3 次；OUT busy 时不推进状态或包序号。
SPI 读回核对 ACK、地址和完整长度；用户校准优先，读不到时回退工厂/缺省值。

核心只使用标准 C 和两个宿主接口：发送报文、32 位微秒时钟。无动态分配，
所有等待在周期任务里完成。TinyUSB 适配器使用现有 HID 类驱动发送中断 OUT，
宿主仍负责枚举、订阅 IN 端点及转发报文。此库不注册新的 USB 类驱动。
按键/摇杆/六轴解析与单位、坐标转换由 hidkit 或宿主解析器处理。

## TinyUSB 集成

在 `tinyusb_host` 已定义后添加目录，并链接 `hidkit_tusb_switch_pro`：

```cmake
add_subdirectory(lib/hidkit-tusb-switch-pro)
target_link_libraries(my_app PRIVATE hidkit_tusb_switch_pro)
# 地址表含地址 0 和 hub；最大 USB 地址必须小于这个值，默认 16。
target_compile_definitions(my_app PRIVATE SWITCH_PRO_ADDR_MAX=16)
```

提供时钟函数，例如 Pico SDK：

```c
#include "tusb.h"
#include "switch_pro.h"
#include "pico/time.h"

uint32_t switch_pro_time_us(void) { return time_us_32(); }
```

将以下调用合并到工程已有回调中，避免重复定义 TinyUSB 回调：

```c
/* tuh_hid_mount_cb 中：取 VID/PID 后通知库，然后订阅 IN。 */
uint16_t vid, pid;
tuh_vid_pid_get(dev_addr, &vid, &pid);
switch_pro_maybe_mount(dev_addr, instance, vid, pid);
tuh_hid_receive_report(dev_addr, instance);

/* tuh_hid_report_received_cb 中：先喂应答，再交给自己的输入解析器。 */
switch_pro_report(dev_addr, instance, report, len);
/* your_input_parser(report, len); */
tuh_hid_receive_report(dev_addr, instance);

/* tuh_hid_umount_cb 中：清除此设备地址的握手状态。 */
switch_pro_umount(dev_addr);

/* 大循环：同一 USB 核/线程中周期调用，不要在回调里自旋等 ACK。 */
tuh_task();
switch_pro_task();
```

所有调用及回调都在同一线程/核心执行；输入包必须包含 Report ID。
每个 USB 地址认领一个 HID instance，重复 mount 不覆盖已运行的握手。
USB `0x81` 应答使用固定间隔处理；只有后续初始化子命令等待 `0x21` ACK。
日志和校准使用可选弱符号回调（GCC/Clang），默认空实现：

```c
void switch_pro_log(bool error, const char *fmt, ...);
void switch_pro_calibration(uint8_t dev_addr, uint8_t instance,
                            const switch_pro_calib_t *cal);
```

日志回调接收 printf 格式，`error=true` 表示初始化失败/NACK 等错误。
校准在 SPI 阶段结束时交付一次；回调后指针失效，需要保留时复制结构体。
`valid` 的 `SWITCH_PRO_CAL_STICKS` / `SWITCH_PRO_CAL_MOTION` 位表示可用的校准项。
数据使用传感器原生坐标；`acc_hoffset` 是含重力的平放参考元数据。

## hidkit 校准接入（可选）

库核心和 TinyUSB 适配器都不依赖 hidkit。使用 hidkit 时，显式包含转换辅助头，
并在宿主维护的 `dev_addr/instance → slot` 映射中找到正确的槽位：

```c
#include "tusb.h" /* 在 hidkit 之前，避免鼠标按钮宏与 TinyUSB 枚举冲突 */
#include "hidkit_switch_pro_glue.h"

void switch_pro_calibration(uint8_t dev_addr, uint8_t instance,
                            const switch_pro_calib_t *cal)
{
    int8_t slot = my_hidkit_slot(dev_addr, instance); /* 宿主提供 */
    if (slot < 0) return;
    hidkit_switchpro_calib_t converted = hidkit_switch_pro_calibration(cal);
    hidkit_switchpro_set_calib(slot, &converted);
}
```

宿主同时照常调用 `hidkit_mount/report/umount`。使用含 Switch Pro 布局及
`acc_scale/gyro_scale` 校准字段的 hidkit；握手 `0x21/0x81` 应答不能作为输入帧输出。

## 其他 USB 栈 / 独立构建

链接 `hidkit_switch_pro_core`，自行实现 `switch_pro_send_report()` 和
`switch_pro_time_us()` 即可。发送缓冲已经包含 Report ID，发送函数必须在返回前
复制数据；返回 `true` 表示成功入队，`false` 表示稍后重试。
时钟使用单调的、允许回绕的 32 位微秒计数，任务须频繁调用。

不需要 Pico、TinyUSB 或 hidkit 的核心编译检查：

```sh
cmake -S lib/hidkit-tusb-switch-pro -B build-switch-pro
cmake --build build-switch-pro
```

本调试器的 `tests/host` 使用模拟 USB/时钟验证真实核心和 TinyUSB 发送适配器，
覆盖握手、ACK/SPI 超时、序号、重新挂载、校准和 hidkit 六轴转换。

协议参考：[Linux hid-nintendo](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-nintendo.c)、
[USB 握手](https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering/blob/master/USB-HID-Notes.md)、
[SPI 校准](https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering/blob/master/imu_sensor_notes.md)。
