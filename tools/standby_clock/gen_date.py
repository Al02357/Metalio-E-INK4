"""生成待机信息卡日期行用的小字位图（26px，1bpp，1=黑）和 2025~2040 农历表，输出 C 头文件。
用法: /tmp/lunarenv/bin/python gen_date.py   （需要 lunar_python、PIL）"""
import datetime
from PIL import Image, ImageDraw, ImageFont
from lunar_python import Solar

FONT = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
SIZE = 26
CH = "0123456789月日星期一二三四五六七八九十农历正冬腊初廿卅闰 "
idx = next(i for i in range(10) if "SC" in ImageFont.truetype(FONT, 10, index=i).getname()[0])
font = ImageFont.truetype(FONT, SIZE, index=idx)
asc, desc = font.getmetrics()
H = asc + desc
glyphs = []
for c in CH:
    w = round(font.getlength(c)) if c != " " else SIZE // 2
    im = Image.new("L", (w, H), 255)
    ImageDraw.Draw(im).text((0, 0), c, font=font, fill=0)
    px = im.load()
    stride = (w + 7) // 8
    data = []
    for y in range(H):
        for xb in range(stride):
            v = 0
            for k in range(8):
                x = xb * 8 + k
                if x < w and px[x, y] < 128:
                    v |= 0x80 >> k
            data.append(v)
    glyphs.append((c, w, stride, data))

Y0 = datetime.date(2025, 1, 1)
days = (datetime.date(2041, 1, 1) - Y0).days
lun = []
for i in range(days):
    d = Y0 + datetime.timedelta(days=i)
    l = Solar.fromYmd(d.year, d.month, d.day).getLunar()
    m = l.getMonth()
    lun.append((abs(m) | (0x80 if m < 0 else 0), l.getDay()))

with open("../../main/display/date_glyphs.h", "w") as f:
    f.write("// 由 tools/standby_clock/gen_date.py 生成，勿手改。1bpp，1=黑\n#pragma once\n#include <cstdint>\n")
    f.write("constexpr int kDateGlyphH = %d;\n" % H)
    f.write("struct DateGlyph { const char* utf8; int w; int stride; const uint8_t* bits; };\n")
    for i, (c, w, s, data) in enumerate(glyphs):
        f.write("static const uint8_t kDateBits%d[] = {%s};\n" % (i, ",".join("0x%02x" % v for v in data)))
    f.write("static const DateGlyph kDateGlyphs[%d] = {%s};\n" % (len(glyphs), ",".join(
        '{"%s",%d,%d,kDateBits%d}' % (c, w, s, i) for i, (c, w, s, d) in enumerate(glyphs))))
    f.write("// 农历：从 2025-01-01 起每天 2 字节 [月(高位=闰), 日]\n")
    f.write("constexpr int kLunarY0 = 2025, kLunarDays = %d;\n" % days)
    f.write("static const uint8_t kLunar[%d] = {%s};\n" % (days * 2, ",".join("%d,%d" % p for p in lun)))
print("H", H, len(glyphs), "glyphs", days, "days")
