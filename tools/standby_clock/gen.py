"""生成待机时钟数字位图（0-9 和 :，1bpp，1=黑），输出 C 头文件。用法: python3 gen.py"""
from PIL import Image, ImageDraw, ImageFont
FONT = "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc"
SIZE = 104
CH = "0123456789:"
idx = next(i for i in range(10) if "SC" in ImageFont.truetype(FONT, 10, index=i).getname()[0])
font = ImageFont.truetype(FONT, SIZE, index=idx)
# 统一基线：用 "0" 的上下边界裁出高度
top = min(font.getbbox(c)[1] for c in CH)
bot = max(font.getbbox(c)[3] for c in CH)
H = bot - top
glyphs = []
for c in CH:
    l, t, r, b = font.getbbox(c)
    w = r - l
    if c != ":":
        w = max(font.getbbox(d)[2] - font.getbbox(d)[0] for d in "0123456789")  # 等宽数字，跳秒不抖
    im = Image.new("L", (w, H), 255)
    ImageDraw.Draw(im).text(((w - (r - l)) // 2 - l, -top), c, font=font, fill=0)
    im = im.point(lambda v: 0 if v < 128 else 255)
    stride = (w + 7) // 8
    data = []
    px = im.load()
    for y in range(H):
        for xb in range(stride):
            v = 0
            for k in range(8):
                x = xb * 8 + k
                if x < w and px[x, y] == 0:
                    v |= 0x80 >> k
            data.append(v)
    glyphs.append((c, w, stride, data))
with open("../../main/display/clock_glyphs.h", "w") as f:
    f.write("// 由 tools/standby_clock/gen.py 生成，勿手改。1bpp，1=黑\n#pragma once\n#include <cstdint>\n")
    f.write("constexpr int kClockGlyphH = %d;\n" % H)
    f.write("struct ClockGlyph { int w; int stride; const uint8_t* bits; };\n")
    for i, (c, w, stride, data) in enumerate(glyphs):
        f.write("static const uint8_t kClockBits%d[] = {%s};\n" % (i, ",".join("0x%02x" % v for v in data)))
    f.write("static const ClockGlyph kClockGlyphs[11] = {%s};\n" % ",".join(
        "{%d,%d,kClockBits%d}" % (w, s, i) for i, (c, w, s, d) in enumerate(glyphs)))
print("H", H, [(g[0], g[1]) for g in glyphs])
