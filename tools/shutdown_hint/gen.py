"""生成关机提示条位图（1bpp，1=白），输出 C 头文件。用法: python3 gen.py"""
from PIL import Image, ImageDraw, ImageFont
W, H = 448, 64
TEXT = "已关机 · 按电源键开机"
FONT = "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc"
idx = next(i for i in range(10) if "SC" in ImageFont.truetype(FONT, 10, index=i).getname()[0])
font = ImageFont.truetype(FONT, 30, index=idx)
im = Image.new("L", (W, H), 255)
d = ImageDraw.Draw(im)
d.rounded_rectangle((0, 0, W - 1, H - 1), radius=16, fill=0)
l, t, r, b = d.textbbox((0, 0), TEXT, font=font)
d.text(((W - (r - l)) // 2 - l, (H - (b - t)) // 2 - t), TEXT, font=font, fill=255)
im = im.point(lambda v: 255 if v >= 128 else 0)
im.save("preview.png")
stride = W // 8
rows = []
px = im.load()
for y in range(H):
    bs = []
    for xb in range(stride):
        v = 0
        for k in range(8):
            if px[xb * 8 + k, y]:
                v |= 0x80 >> k
        bs.append("0x%02x" % v)
    rows.append("    " + ",".join(bs) + ",")
with open("../../main/display/shutdown_hint_bitmap.h", "w") as f:
    f.write("// 由 tools/shutdown_hint/gen.py 生成，勿手改\n#pragma once\n#include <cstdint>\n")
    f.write("constexpr int kShutdownHintW = %d;\nconstexpr int kShutdownHintH = %d;\n" % (W, H))
    f.write("static const uint8_t kShutdownHintBits[%d] = {\n%s\n};\n" % (stride * H, "\n".join(rows)))
print("ok", idx)
