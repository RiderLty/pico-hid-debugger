#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
pico-hid-debugger 上位机串口监视脚本。

功能：
  - 实时接收并显示固件 UART0 输出的调试行（挂载 dump / 报文 hexdump）
  - 单键操作：f = 冻结/恢复滚动，c = 清屏，q = 退出
  - 自动探测串口设备

用法：
  python3 tools/uart_monitor.py                        # 自动探测串口
  python3 tools/uart_monitor.py -p /dev/tty.usbserialXXXX
  python3 tools/uart_monitor.py -b 2000000              # 默认 2000000

依赖：pip install pyserial
"""

import argparse
import glob
import sys
import threading

# ---------- 行缓冲：串口字节流 → 显示行 ----------


class LineFeed(object):
    """把串口字节流按 \\n 切成显示行。线程安全。"""

    def __init__(self):
        self._buf = bytearray()
        self._lock = threading.Lock()

    def feed(self, data):
        """喂入新收到的字节，返回待打印的显示行列表。"""
        with self._lock:
            self._buf.extend(data)
            lines = []
            while True:
                nl = self._buf.find(b"\n")
                if nl < 0:
                    break
                line = bytes(self._buf[:nl]).decode("utf-8", errors="replace").rstrip("\r")
                del self._buf[: nl + 1]
                lines.append(line)
            # 防御：无换行的垃圾超长堆积时截断，避免内存增长
            if len(self._buf) > 512:
                lines.append(bytes(self._buf[:512]).decode("utf-8", errors="replace"))
                del self._buf[:512]
            return lines


# ---------- 串口探测与打开 ----------

def find_port():
    # UART 适配器优先（固件已无 CDC，不会出现 usbmodem）
    patterns = ["/dev/tty.usbserial*", "/dev/tty.usbmodem*", "/dev/ttyUSB*", "/dev/ttyACM*"]
    for pat in patterns:
        hits = sorted(glob.glob(pat))
        if hits:
            return hits[0]
    return None


def open_serial(port, baud):
    try:
        import serial
    except ImportError:
        sys.exit("缺少 pyserial：请先执行 pip install pyserial")
    return serial.Serial(port, baud, timeout=0.2)


def main():
    ap = argparse.ArgumentParser(description="pico-hid-debugger UART 监视器")
    ap.add_argument("-p", "--port", help="串口设备路径（默认自动探测）")
    ap.add_argument("-b", "--baud", type=int, default=2000000,
                    help="波特率（默认 2000000，与固件一致）")
    args = ap.parse_args()

    port = args.port or find_port()
    if not port:
        sys.exit("未找到串口设备，请用 -p 指定（如 /dev/tty.usbserialXXXX）")

    ser = open_serial(port, args.baud)
    feed = LineFeed()
    frozen = threading.Event()

    stop = threading.Event()

    def reader():
        while not stop.is_set():
            try:
                data = ser.read(256)
            except Exception:
                break
            if not data:
                continue
            for line in feed.feed(data):
                if frozen.is_set():
                    continue   # 冻结期间只消费不入屏
                out_line(line)

    t = threading.Thread(target=reader, daemon=True)
    t.start()

    note("已连接 %s @ %d —— 单键操作：f=冻结/恢复  c=清屏  q=退出"
         % (port, args.baud))

    try:
        while True:
            ch = getch()
            if ch in ("q", "Q"):
                break
            elif ch in ("f", "F"):
                if frozen.is_set():
                    frozen.clear()
                    note(">> 已恢复滚动")
                else:
                    frozen.set()
                    note(">> 已冻结（按 f 恢复）")
            elif ch in ("c", "C"):
                sys.stdout.write("\x1b[2J\x1b[H")   # 清屏并回到左上角
                sys.stdout.flush()
    finally:
        stop.set()
        t.join(timeout=1)
        ser.close()
        restore_tty()
    return 0


def out_line(s):
    """数据行输出：显式 \r\n 结尾（即使终端处于无 ONLCR 的原始模式也不会错位）。"""
    sys.stdout.write(s + "\r\n")
    sys.stdout.flush()


def note(s):
    """提示信息输出到 stderr。"""
    sys.stderr.write(s + "\n")
    sys.stderr.flush()


# ---------- 终端单键输入（POSIX / Windows） ----------
_tty_saved = None

def getch():
    global _tty_saved
    if sys.platform == "win32":
        import msvcrt
        return msvcrt.getch().decode(errors="replace")
    import termios
    import tty
    fd = sys.stdin.fileno()
    if _tty_saved is None:
        _tty_saved = termios.tcgetattr(fd)
    # 用 cbreak 而非 raw：单键读取、关闭回显，但保留终端输出处理（ONLCR），
    # 否则后续 print 的 \n 不带回车，整个输出会阶梯状错位
    tty.setcbreak(fd)
    return sys.stdin.read(1)


def restore_tty():
    global _tty_saved
    if sys.platform != "win32" and _tty_saved is not None:
        import termios
        termios.tcsetattr(sys.stdin.fileno(), termios.TCSADRAIN, _tty_saved)
        _tty_saved = None


if __name__ == "__main__":
    sys.exit(main())
