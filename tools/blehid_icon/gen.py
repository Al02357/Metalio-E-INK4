"""生成首页「蓝牙键鼠」图标（70×70 A8，线条风格同「看图」图标），输出 C 头文件。"""
from PIL import Image, ImageDraw
S = 4
W = 70
im = Image.new("L", (W * S, W * S), 0)
d = ImageDraw.Draw(im)
lw = 4 * S
# 键盘
d.rounded_rectangle((4 * S, 24 * S, 46 * S, 52 * S), radius=5 * S, outline=255, width=lw)
for row, y in enumerate((32, 39)):
    for i in range(5):
        x = 11 + i * 7
        d.rectangle((x * S, y * S, (x + 3) * S, (y + 3) * S), fill=255)
d.rectangle((16 * S, 45 * S, 34 * S, 47 * S), fill=255)
# 鼠标
d.rounded_rectangle((50 * S, 28 * S, 66 * S, 54 * S), radius=8 * S, outline=255, width=lw)
d.line([(58 * S, 30 * S), (58 * S, 38 * S)], fill=255, width=3 * S)
d.line([(58 * S, 28 * S), (58 * S, 20 * S), (50 * S, 14 * S)], fill=255, width=3 * S, joint="curve")
im = im.resize((W, W), Image.LANCZOS)
data = im.tobytes()
with open("../../main/display/screen/blehid_screen/blehid_icon.h", "w") as f:
    f.write("// 由 tools/blehid_icon/gen.py 生成\n#pragma once\n#include \"lvgl.h\"\n")
    f.write("static const uint8_t kBleHidIconA8[%d] = {%s};\n" % (len(data), ",".join(str(b) for b in data)))
    f.write("""inline const lv_image_dsc_t* BleHidIcon() {
    static lv_image_dsc_t d;
    if (d.data == nullptr) {
        d.header.magic = LV_IMAGE_HEADER_MAGIC;
        d.header.cf = LV_COLOR_FORMAT_A8;
        d.header.w = %d;
        d.header.h = %d;
        d.header.stride = %d;
        d.data_size = sizeof(kBleHidIconA8);
        d.data = kBleHidIconA8;
    }
    return &d;
}
""" % (W, W, W))
Image.eval(im, lambda v: 255 - v).save("preview.png")
