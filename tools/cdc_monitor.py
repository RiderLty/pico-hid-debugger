#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
pico-hid-debugger 上位机串口监视脚本。

功能：
  - 实时接收并显示固件 CDC 输出（文本事件行 / 二进制帧）
  - 单键运行时切换：d = 设备转发，t = 文本，b = 二进制，m/w = Mac/Windows 修饰键交换
  - 二进制模式下每帧**同时**显示解析后的语义与原始十六进制

用法：
  python3 tools/cdc_monitor.py                 # 自动探测串口
  python3 tools/cdc_monitor.py -p /dev/tty.usbmodemXXXX
  python3 tools/cdc_monitor.py --selftest      # 内置协议解码自测

依赖：pip install pyserial（--selftest 不需要）
"""

import argparse
import glob
import struct
import sys
import threading

# ---------- 协议常量（与固件 src/hid_output.h 头部注释保持一致） ----------
FRAME_HDR = b"\x55\xAA"
ID_LEN = 5                     # 来源身份区：PID2 + VID2 + port1（仅非键鼠帧携带）
PAYLOAD_MAX = 64               # 与 CFG_TUH_HID_EPIN_BUFSIZE 一致
LEN_MIN = 1                    # len 最小 = type 本身
LEN_MAX = 1 + ID_LEN + PAYLOAD_MAX
FRAME_TOTAL_MAX = 2 + 1 + LEN_MAX

TYPE_NONE = 0x00
TYPE_KEYBOARD = 0x01
TYPE_MOUSE = 0x02
TYPE_MOUNT = 0x10
TYPE_UMOUNT = 0x11
TYPE_DROP = 0x12
TYPE_ERROR = 0x13

TYPE_NAMES = {
    TYPE_NONE: "OTHER",
    TYPE_KEYBOARD: "KEYBOARD",
    TYPE_MOUSE: "MOUSE",
    TYPE_MOUNT: "MOUNT",
    TYPE_UMOUNT: "UMOUNT",
    TYPE_DROP: "DROP",
    TYPE_ERROR: "ERROR",
}

MOD_NAMES = ["LCTRL", "LSHIFT", "LALT", "LGUI", "RCTRL", "RSHIFT", "RALT", "RGUI"]
PROTO_NAMES = ["None", "Keyboard", "Mouse"]
BTN_LETTERS = "LRMBF567"


def decode_key_name(code):
    """常用键码转可读名：字母 a-z、数字行，其余显示两位十六进制。"""
    if 0x04 <= code <= 0x1D:
        return chr(ord("a") + code - 4)
    if 0x1E <= code <= 0x26:
        return str(code - 0x1E + 1)
    if code == 0x27:
        return "0"
    return "%02X" % code


def _signed8(v):
    return v - 256 if v >= 128 else v


def _signed16(v):
    return v - 65536 if v >= 32768 else v


def describe_payload(ptype, payload):
    """把一帧 payload 解析成人类可读摘要（不含原始十六进制部分）。"""
    if ptype == TYPE_KEYBOARD:
        if len(payload) < 3:
            return "键盘报文过短(%dB)" % len(payload)
        mods = []
        for byte_idx in range(2):
            for bit in range(8):
                if payload[byte_idx] & (1 << bit):
                    mods.append(MOD_NAMES[byte_idx * 8 + bit])
        keys = [decode_key_name(k) for k in payload[2:] if k]
        parts = ["KEYBOARD"]
        parts.append("MOD=" + ("+".join(mods) if mods else "---"))
        parts.append("KEY=" + (" ".join(keys) if keys else "---"))
        return " ".join(parts)

    if ptype == TYPE_MOUSE:
        if len(payload) < 6:
            return "鼠标帧过短(%dB)" % len(payload)
        buttons, wheel = payload[0], _signed8(payload[1])
        x = _signed16(payload[2] | (payload[3] << 8))
        y = _signed16(payload[4] | (payload[5] << 8))
        btns = "|".join(BTN_LETTERS[b] for b in range(8) if buttons & (1 << b)) or "---"
        return "MOUSE BTN=%s WH=%+d DX=%+d DY=%+d" % (btns, wheel, x, y)

    if ptype == TYPE_MOUNT:
        if len(payload) < 2:
            return "MOUNT 帧过短"
        itf, proto = payload[0], payload[1]
        pname = PROTO_NAMES[proto] if proto < 3 else "?"
        return "MOUNT itf=%u proto=%s" % (itf, pname)

    if ptype == TYPE_UMOUNT:
        if len(payload) < 1:
            return "UMOUNT 帧过短"
        return "UMOUNT itf=%u" % payload[0]

    if ptype == TYPE_DROP:
        if len(payload) < 4:
            return "DROP 帧过短"
        return "DROP lost_events=%u" % struct.unpack("<I", payload[:4])[0]

    if ptype == TYPE_ERROR:
        return "ERROR %s" % payload.decode("ascii", errors="replace")

    # 其他设备：原始报文，不做语义解释
    return "OTHER %dB" % len(payload)


class Monitor(object):
    """按当前模式把串口字节流转成显示行。线程安全。"""

    def __init__(self):
        self.mode = "text"      # "text" | "binary"
        self._buf = bytearray()
        self._lock = threading.Lock()

    def set_mode(self, mode):
        with self._lock:
            self.mode = mode
            self._buf.clear()   # 切换即弃置残留半行/半帧，避免衔接噪声

    def feed(self, data):
        """喂入新收到的字节，返回待打印的显示行列表。"""
        with self._lock:
            if self.mode == "text":
                return self._feed_text(data)
            if self.mode == "binary":
                return self._feed_binary(data)
            return []   # 设备转发模式：串口无数据流，丢弃以防万一

    def _feed_text(self, data):
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

    def _feed_binary(self, data):
        self._buf.extend(data)
        lines = []
        while True:
            # 扫描帧头
            hdr = self._buf.find(FRAME_HDR)
            if hdr < 0:
                # 找不到完整帧头则全部丢弃，只留最后 1 字节以防 0x55 恰在末尾
                if len(self._buf) > 1:
                    del self._buf[: len(self._buf) - 1]
                break
            del self._buf[:hdr]

            if len(self._buf) < 3:
                break                       # len 字节未到齐
            plen = self._buf[2]
            if plen < LEN_MIN or plen > LEN_MAX:
                del self._buf[:3]           # 非法长度 → 失步，丢弃继续扫描
                continue

            # 解析规则按 type 二分：键盘/鼠标紧凑无身份区；其余带 PID/VID/port
            ptype = self._buf[3]
            has_id = ptype not in (TYPE_KEYBOARD, TYPE_MOUSE)
            if has_id and plen < 1 + ID_LEN:
                del self._buf[:3]           # 身份区不完整 → 失步重扫
                continue

            total = 2 + 1 + plen
            if len(self._buf) < total:
                break                       # 整帧未到齐，等下一包数据

            frame = bytes(self._buf[:total])
            del self._buf[:total]

            if has_id:
                pid = frame[4] | (frame[5] << 8)
                vid = frame[6] | (frame[7] << 8)
                port = frame[8]
                payload = frame[9:]
            else:
                pid = vid = port = None
                payload = frame[4:]

            summary = describe_payload(ptype, payload)
            hexs = " ".join("%02X" % b for b in frame)
            meta = "" if (port is None or port == 0) \
                else " dev=%u vid=%04x pid=%04x" % (port, vid, pid)
            lines.append("[%s]%s %s | HEX: %s"
                         % (TYPE_NAMES.get(ptype, "%02X" % ptype), meta, summary, hexs))
        return lines


# ---------- 串口探测与打开 ----------

def find_port():
    patterns = ["/dev/tty.usbmodem*", "/dev/tty.usbserial*", "/dev/ttyACM*"]
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
    ap = argparse.ArgumentParser(description="pico-hid-debugger 串口监视器")
    ap.add_argument("-p", "--port", help="串口设备路径（默认自动探测）")
    ap.add_argument("-b", "--baud", type=int, default=115200,
                    help="波特率（USB CDC 下仅形式参数）")
    ap.add_argument("--binary", action="store_true", help="启动即切到二进制模式")
    ap.add_argument("--selftest", action="store_true", help="运行内置协议解码自测")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    port = args.port or find_port()
    if not port:
        sys.exit("未找到串口设备，请用 -p 指定（如 /dev/tty.usbmodemXXXX）")

    ser = open_serial(port, args.baud)
    mon = Monitor()
    if args.binary:
        mon.set_mode("binary")

    stop = threading.Event()

    def reader():
        first_chunk = True
        while not stop.is_set():
            try:
                data = ser.read(256)
            except Exception:
                break
            if data:
                # 首包自动识别：上次会话可能把固件留在二进制模式
                if first_chunk:
                    first_chunk = False
                    if mon.mode == "text" and data.startswith(FRAME_HDR):
                        mon.set_mode("binary")
                        note("检测到二进制流，本地解析器已自动切换到二进制模式")
                for line in mon.feed(data):
                    out_line(line)

    t = threading.Thread(target=reader, daemon=True)
    t.start()

    note("已连接 %s  —— 单键操作：d=设备转发  t=文本  b=二进制  m=Mac交换(默认)  w=不交换  q=退出"
         % port)

    try:
        while True:
            ch = getch()
            if ch in ("q", "Q"):
                break
            elif ch in ("b", "B"):
                mon.set_mode("binary")
                ser.write(b"B")     # 通知固件同步切换
                note(">> 已切换到二进制模式")
            elif ch in ("t", "T"):
                mon.set_mode("text")
                ser.write(b"T")
                note(">> 已切换到文本模式")
            elif ch in ("d", "D"):
                # 切回设备转发模式：固件把事件送往模拟键鼠，串口不再有数据流
                mon.set_mode("device")
                ser.write(b"D")
                note(">> 已切换到设备转发模式（模拟键鼠，串口无输出；按 t/b 回到串口）")
            elif ch in ("m", "M"):
                ser.write(b"M")     # Mac 模式：LALT↔LGUI、RALT↔RGUI 交换（默认），Ctrl 不变
                note(">> 已切换到 Mac 模式（Alt 与 Gui 交换）")
            elif ch in ("w", "W"):
                ser.write(b"W")
                note(">> 已切换到 Windows 模式（修饰键不交换）")
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


# ---------- 自测：构造帧喂入解码器并断言输出 ----------

def make_frame(ptype, payload):
    """紧凑帧（键盘/鼠标）：55 AA len type payload"""
    body = bytes([ptype]) + payload
    return FRAME_HDR + bytes([len(body)]) + body


def make_frame_ident(pid, vid, port, ptype, payload):
    """带身份帧：55 AA len type PID VID port payload"""
    body = (bytes([ptype, pid & 0xFF, pid >> 8, vid & 0xFF, vid >> 8, port])
            + payload)
    return FRAME_HDR + bytes([len(body)]) + body


def selftest():
    mon = Monitor()
    mon.set_mode("binary")

    # 鼠标紧凑帧：buttons=L|R wheel=-1 x=-12 y=345，无来源字段
    out = mon.feed(make_frame(TYPE_MOUSE,
                              bytes([0x03, 0xFF]) + struct.pack("<hh", -12, 345)))
    assert len(out) == 1 and "BTN=L|R" in out[0] and "WH=-1" in out[0] \
        and "DX=-12" in out[0] and "DY=+345" in out[0], out
    assert "[MOUSE]" in out[0] and "dev=" not in out[0], out
    assert "| HEX: 55 AA 07 02" in out[0], out

    # 键盘紧凑帧：LSHIFT + 'a'
    out = mon.feed(make_frame(TYPE_KEYBOARD, bytes([0x02, 0x00, 0x04])))
    assert "[KEYBOARD]" in out[0] and "MOD=LSHIFT" in out[0] \
        and "KEY=a" in out[0], out

    # 其他设备带身份帧：帧前混入垃圾字节（失步重同步）
    out = mon.feed(b"\x00\x99"
                   + make_frame_ident(0x4001, 0xCAFE, 3, TYPE_UMOUNT, b"\x07"))
    assert any("[UMOUNT] dev=3 vid=cafe pid=4001 UMOUNT itf=7" in l
               for l in out), out

    # 半帧到达不出行，补齐后出（DROP 元信息帧，PID/VID/port 全零）
    f = make_frame_ident(0, 0, 0, TYPE_DROP, struct.pack("<I", 42))
    out = mon.feed(f[:8])
    assert out == [], out
    out = mon.feed(f[8:])
    assert len(out) == 1 and "lost_events=42" in out[0], out

    # 错误帧（ASCII payload）
    out = mon.feed(make_frame_ident(0, 0, 0, TYPE_ERROR, b"boom"))
    assert "[ERROR]" in out[0] and "boom" in out[0], out

    # 文本模式透传
    mon.set_mode("text")
    out = mon.feed(b"KEY:04 DN\r\nMOUSE:DX=1 DY=2 WH=0 BTN=---\r\n")
    assert out == ["KEY:04 DN", "MOUSE:DX=1 DY=2 WH=0 BTN=---"], out

    print("SELFTEST PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
