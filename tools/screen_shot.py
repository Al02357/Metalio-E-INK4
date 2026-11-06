#!/usr/bin/env python3
"""从 Metalio E-Ink 4 截取当前墨水屏画面（固件 main/display/screen_dump.cc）。

用法：python tools/screen_shot.py [-p /dev/ttyACM0] [-o shot.png] [--rotate auto|0|90|180|270]
需 pyserial + Pillow（IDF 的 python 环境里都有）。
打开串口时保持 DTR/RTS 不变，不会复位设备。
"""
import argparse
import base64
import sys
import time
import zlib

import serial
from PIL import Image

ap = argparse.ArgumentParser()
ap.add_argument("-p", "--port", default="/dev/ttyACM0")
ap.add_argument("-o", "--out", default="screen.png")
ap.add_argument("--rotate", default="auto", help="auto 按设备方向旋转；或 0/90/180/270（逆时针）")
ap.add_argument("--timeout", type=float, default=8.0)
ap.add_argument("--open", choices=["myai"], help="截屏前先打开指定页面（等 2 秒刷屏）")
args = ap.parse_args()

# pyserial 默认 DTR=RTS=1；若先拉低 DTR 而 RTS 仍高，USB-Serial/JTAG 会复位芯片
s = serial.Serial(args.port, 115200, timeout=0.2)
s.reset_input_buffer()
if args.open:
    s.write(b"OPEN MYAI\n")
    s.flush()
    time.sleep(2.5)
    s.reset_input_buffer()
s.write(b"SHOT\n")
s.flush()

deadline = time.time() + args.timeout
head = None
chunks = {}
buf = b""
done = False
while time.time() < deadline and not done:
    buf += s.read(65536)
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        line = line.strip()
        k = line.find(b"@@")
        if k > 0:
            line = line[k:]  # 前面粘了别的日志
        if line.startswith(b"@@SHOT BEGIN"):
            head = line.split()[2:]
            chunks = {}
        elif line.startswith(b"@@SHOT ERR"):
            sys.exit(line.decode())
        elif line.startswith(b"@@S ") and head is not None:
            parts = line.split(b" ", 2)
            if len(parts) == 3:
                chunks[int(parts[1])] = parts[2]
        elif line.startswith(b"@@SHOT END") and head is not None:
            done = True
            break
s.close()

if not done:
    sys.exit("超时：没收到完整截屏（固件是否已含 screen_dump？设备是否处于虚拟 U 盘模式？）")

w, h, size, orient, crc = int(head[0]), int(head[1]), int(head[2]), int(head[3]), int(head[4], 16)
data = b"".join(base64.b64decode(chunks[i]) for i in sorted(chunks))
if len(head) > 5 and head[5] == b"packbits":
    out = bytearray()
    i = 0
    while i < len(data):
        n = data[i]
        i += 1
        if n < 128:
            out += data[i:i + n + 1]
            i += n + 1
        else:
            out += bytes([data[i]]) * (257 - n)
            i += 1
    data = bytes(out)
if len(data) != size:
    sys.exit(f"长度不符：{len(data)} != {size}")
if zlib.crc32(data) & 0xFFFFFFFF != crc:
    print("警告：CRC 不一致，图像可能有错行", file=sys.stderr)

img = Image.frombytes("1", (w, h), data)  # 1=白 0=黑，MSB 在左
rot = args.rotate
if rot == "auto":
    rot = {0: 270, 1: 0, 2: 180}.get(orient, 0)  # 竖屏：面板原生横向 → 转回竖向
img = img.rotate(int(rot), expand=True)
img.save(args.out)
print(f"已保存 {args.out}  面板 {w}x{h} orient={orient} rotate={rot}")
