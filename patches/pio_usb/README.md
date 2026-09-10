# PIO-USB 版本与补丁

## 当前安排（2026-09）

子模块 `lib/pico_pio_usb` **固定在旧血脉（0.6.0 重写之前）的顶端提交**：

```
9510f79  "Apply pin order settings to each port"   2024-06-02
```

外加**一枚**补丁（`patches/pio_usb/0001-sdk2-compat.patch`，见下）。
TinyUSB 侧的 hub 驱动韧性补丁是另一回事，在顶层 `CMakeLists.txt` 里 configure 期
生成副本（与 SDK 文件无关），不在这里。

## 为什么固定旧血脉

上游 0.6.0（2024-06）做了一次大重写，**换了整套 RX PIO 实现**（`e2e33f3`
"Reduce receive PIO instructions" 起，随后 `d735828` / `36d5be4` "works with ls/fs
device"）。这次重写：

- **带来**：低速（LS）设备经 hub 的支持（此前 LS 根本无法枚举）；
- **破坏**：hub 上设备**拔出**的处理（有在途传输时）——正是上游 issue
  [#149](https://github.com/sekigon-gonnoc/Pico-PIO-USB/issues/149) 与
  [TinyUSB #2971](https://github.com/hathach/tinyusb/issues/2971) 报告的症状
  （"插到 hub 上的设备能枚举；拔掉它，检测不到"），报告人 bisect 出的结论就是
  这一对提交，且指出**最后一个能正确处理它的提交是 `0f747aa`**（2023-11，重写之前）。

本项目**放弃低速支持**，换取 hub 热插拔可靠。选 `9510f79` 而不是 0.5.3 发布版，
是因为它只比 0.5.3 多 4 个提交，且**正好包含 `0f747aa` / `ffb1647`
"retired all transferring endpoint if device is disconnected"**：

```
9510f79  Apply pin order settings to each port          ← 本仓库固定在这里
98a7137  Add test program (Not passed yet)              （测试程序，无影响）
0f747aa  Merge PR #104 ... retired-all-eps-if-disconnected   ★ 关键修复
ffb1647  retired all transferring endpoint if device is disconnected  ★
```

（0.5.3 → HEAD 之间共有 **5 个发布版**：0.6.0 / 0.6.1 / 0.7.0 / 0.7.1 / 0.7.2，
共 83 个提交。）

**兼容性已实测**：`9510f79` 提供 TinyUSB 0.18 的 `hcd_pio_usb.c` 需要的全部 API
（带 `pinout` 的 `pio_usb_host_add_port`、`endpoint_open/transfer/abort`、
`send_setup`、`close_device`、`port_reset_start/end`、`bus_get_line_state`），
配 pico-sdk 2.3.0 可正常构建（旧血脉缺的 SDK 2 兼容由下面那枚补丁补上）。

## 唯一的一枚补丁

| 文件 | 来源 | 作用 |
| --- | --- | --- |
| `0001-sdk2-compat.patch` | 上游 `75e62ad` + `0a14a34`（"fix build with pico sdk v2" / "Remove pio_sm_set_jmp_pin Pico SDK 2 has it"） | 旧血脉**从未**拿到 Pico SDK 2 的兼容修复（那两笔提交都在 0.6.0 重写之后）：① `usb_rx.pio` 里的本地 `pio_sm_set_jmp_pin` 与 SDK 2 自带同名函数**重定义冲突** → 用 `#if PICO_SDK_VERSION_MAJOR < 2` 包住；② `usb_rx.pio.h` / `usb_tx.pio.h` 由旧版 pioasm 生成，缺 SDK 2 的 `pio_program_t` 新字段（`pio_version`、`used_gpio_ranges`）→ 用当前 pioasm 重新生成。**纯构建兼容，无语义改动** |

用法（`build.sh` 会自动调用）：

```bash
./scripts/apply-patches.sh            # 幂等
./scripts/apply-patches.sh --status
./scripts/apply-patches.sh --revert   # 丢弃补丁，回到 9510f79 原样
```

## 归档：0.6+ 血脉的 9 枚补丁

`patches/pio_usb_archive_0.6plus/` 保存着针对 **0.6+/0.7 血脉**（即提交 `5a37a66`）
写的 9 枚补丁，以及那次调查的全部结论。**当前不使用**（它们只对 0.6+ 生效），
保留是因为：若将来上游修好 #149、重新启用低速支持，或者是别人想继续啃这个问题，
这些是最省力的起点。

那轮调查的要点（供后来者，勿重复踩）：

- 症状链：hub 上报端口变化 ✓ → `GetPortStatus` 的 **SETUP 阶段失败**（实测抓到的字节
  `01 A5` 正是 `80 D2` = SYNC+ACK 左移一位，即 hub 其实 ACK 了，是我们解错）→
  TinyUSB 0.18 hub 驱动一次失败即永久停摆（上游 PR #2994 才修）→ 拔出事件永远处理不完。
- 补丁 0001/0002 = 上游 PR #206 / #211（未合并）；0003/0007 = 事务失败探针；
  0004/0006/0008 = 短包重对齐、`idx==3` 空洞、全速上膛时机；0005 = 重试窗口；
  0009 = 移植 0.5.3 的 RX PIO 对。
- **最后定位到**：hub 自己的控制管道 IN 解出**非均匀位移**（SYNC 试 k=1..7 全无解、
  换过不同上膛时机字节不变、30 次重试完全一致）→ 指向 0.6 重写的 RX 解码器/边沿检测器
  **这一对**（两者配套，单独换任一个都会让枚举彻底失败）。未走完的一步是"保留新触发
  时序、只恢复 `pin_high` 路径对 `x` 的处理"这类**定向修**。

## 何时可以重新评估

- 上游合并了 #149 相关的修复（或 PR #206/#211 被合并）且给出了新发布版 → 可考虑升级
  回新血脉并重获低速支持；
- 或者你重新需要低速支持时 → 从 `patches/pio_usb_archive_0.6plus/` 出发继续。
