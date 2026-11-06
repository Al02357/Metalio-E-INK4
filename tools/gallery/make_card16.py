"""生成 16 级灰阶色卡（.g2）：墨水屏只有 4 级真灰（0/85/170/255），中间的 12 级用 4×4 Bayer 有序抖动混出来。
上方一条是 4 级真灰对照，下面 16 条从黑到白。
用法：python make_card16.py 输出目录      （生成 00_test16.g2 和预览 png）
"""
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

W, H = 480, 755
FONT = "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc"
BAYER4 = (np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]) + 0.5) / 16.0


def quant4(v, yy, xx):
    """v: 0..255 目标亮度 → 0..3 真灰等级（相邻两级之间按 Bayer 阈值选）"""
    t = v / 85.0
    lo = np.floor(t).clip(0, 3)
    frac = t - lo
    return (lo + (frac > BAYER4[yy % 4, xx % 4])).clip(0, 3).astype(np.uint8)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    target = np.full((H, W), 255.0)
    labels = []
    top, strip_h = 55, 60
    for i in range(4):  # 4 级真灰对照
        target[top:top + strip_h, i * W // 4:(i + 1) * W // 4] = i * 85
    y0, band = top + strip_h + 28, 37
    for i in range(16):
        v = round(i * 255 / 15)
        y = y0 + i * band
        target[y:y + band - 2, 90:W - 20] = v
        labels.append((y, i, v))
    yy, xx = np.mgrid[0:H, 0:W]
    q = quant4(target, yy, xx)
    q[target >= 254.5] = 3
    img = Image.fromarray((q * 85).astype(np.uint8))
    d = ImageDraw.Draw(img)
    f1 = ImageFont.truetype(FONT, 26)
    f2 = ImageFont.truetype(FONT, 20)
    d.text((W // 2, 28), "16 级灰阶（4 级真灰 + 抖动）", font=f1, fill=0, anchor="mm")
    for i in range(4):
        d.text((i * W // 4 + W // 8, top + strip_h + 14), "真灰 %d" % i, font=f2, fill=0, anchor="mm")
    for y, i, v in labels:
        d.text((12, y + band // 2 - 1), "%2d" % i, font=f2, fill=0, anchor="lm")
        d.text((50, y + band // 2 - 1), "%d" % round(v * 100 / 255), font=ImageFont.truetype(FONT, 14), fill=0,
               anchor="lm")
    d.rectangle((89, y0 - 1, W - 20, y0 + 16 * band - 2), outline=0)  # 外框：最后一条白的也看得出边界
    # 文字画完再量化一次（文字是纯黑，不影响）
    a = np.asarray(img, dtype=np.float64)
    q = np.rint(a / 85).clip(0, 3).astype(np.uint8).reshape(-1, 4)
    packed = (q[:, 0] << 6) | (q[:, 1] << 4) | (q[:, 2] << 2) | q[:, 3]
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "00_test16.g2"), "wb") as fo:
        fo.write(b"G2" + struct.pack("<HHH", W, H, 0) + packed.astype(np.uint8).tobytes())
    img.save(os.path.join(out, "00_test16_preview.png"))
    print("ok", os.path.join(out, "00_test16.g2"))


if __name__ == "__main__":
    main()
