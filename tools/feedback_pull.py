#!/usr/bin/env python3
"""从设备 SD 卡拉取问题反馈（USB 串口，设备没联网时用）。

用法：
  python tools/feedback_pull.py            # 列出并拉取最新一份到 ../feedback/<id>/
  python tools/feedback_pull.py <id>       # 拉指定那份
  python tools/feedback_pull.py --file <SD 相对路径> <本地文件>
"""
import base64
import glob
import os
import re
import sys
import time

import serial
from PIL import Image

OUT_ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "feedback")


def port():
    ps = sorted(glob.glob("/dev/ttyACM*"))
    if not ps:
        sys.exit("没找到 /dev/ttyACM*，设备插在电脑 USB 口上了吗")
    return ps[0]


def fget(s, path, off):
    s.reset_input_buffer()
    s.write(("FGET %s %d\n" % (path, off)).encode())
    buf, t0 = b"", time.time()
    while time.time() - t0 < 5:
        buf += s.read(65536)
        m = re.search(rb"@@FGET (\d+) ([A-Za-z0-9+/=]*)@@ total=(-?\d+)", buf)
        if m:
            data = base64.b64decode(m.group(2))
            if len(data) != int(m.group(1)):
                raise IOError("长度不符")
            return data, int(m.group(3))
    raise IOError("设备没回应 FGET")


def pull_file(s, path, dst):
    out, total = b"", None
    while total is None or len(out) < total:
        for _ in range(3):
            try:
                data, total = fget(s, path, len(out))
                break
            except IOError:
                time.sleep(0.5)
        else:
            raise IOError("读 %s 失败" % path)
        if total < 0:
            return False
        if not data:
            break
        out += data
    with open(dst, "wb") as f:
        f.write(out)
    return True


def main():
    s = serial.Serial(port(), 115200, timeout=0.05)
    if len(sys.argv) >= 4 and sys.argv[1] == "--file":
        ok = pull_file(s, sys.argv[2], sys.argv[3])
        print("ok" if ok else "文件不存在")
        return
    fid = sys.argv[1] if len(sys.argv) > 1 else None
    if fid is None:
        sys.exit("请给出反馈 id（设备日志里 Feedback: saved .../feedback/<id>，形如 20261105-193429）")
    d = os.path.join(OUT_ROOT, fid)
    os.makedirs(d, exist_ok=True)
    for name in ("info.txt", "screen.pbm", "log.txt"):
        ok = pull_file(s, "feedback/%s/%s" % (fid, name), os.path.join(d, name))
        print(name, "ok" if ok else "没有")
    pbm = os.path.join(d, "screen.pbm")
    if os.path.exists(pbm):
        info = open(os.path.join(d, "info.txt"), encoding="utf-8", errors="replace").read()
        m = re.search(r"orient=(\d+)", info)
        rot = {0: 270, 1: 0, 2: 180}.get(int(m.group(1)) if m else 0, 0)
        Image.open(pbm).convert("L").rotate(rot, expand=True).save(os.path.join(d, "screen.png"))
    print("已存到", os.path.abspath(d))


if __name__ == "__main__":
    main()
