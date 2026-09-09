#ifndef OUT_DEVICE_H
#define OUT_DEVICE_H

/*
 * 设备转发实现（out_device.c）的对外接口。
 */

// 补发全零报告，松开模拟键鼠上仍按着的键/按键。
// 仅允许 core0 调用（内部直接操作 Device 栈）；切换到串口模式时调用。
void out_device_release_all(void);

#endif // OUT_DEVICE_H
