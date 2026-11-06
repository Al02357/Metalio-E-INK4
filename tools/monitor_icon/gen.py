"""生成首页「监控」图标（70×70 A8，线条风格同其它图标），输出 C 头文件。"""
from PIL import Image, ImageDraw
S = 4  # 超采样
W = 70
im = Image.new("L", (W * S, W * S), 0)
d = ImageDraw.Draw(im)
lw = 5 * S
d.rounded_rectangle((6 * S, 10 * S, 64 * S, 58 * S), radius=10 * S, outline=255, width=lw)
pts = [(15, 44), (26, 32), (35, 39), (46, 24), (55, 30)]
d.line([(x * S, y * S) for x, y in pts], fill=255, width=lw, joint="curve")
for x, y in pts[:1] + pts[-1:]:
    r = 2.5 * S
    d.ellipse((x * S - r, y * S - r, x * S + r, y * S + r), fill=255)
im = im.resize((W, W), Image.LANCZOS)
data = im.tobytes()
with open("../../main/display/screen/monitor_screen/monitor_icon.h", "w") as f:
    f.write("// 由 tools/monitor_icon/gen.py 生成\n#pragma once\n#include \"lvgl.h\"\n")
    f.write("static const uint8_t kMonitorIconA8[%d] = {%s};\n" % (len(data), ",".join(str(b) for b in data)))
    f.write("""inline const lv_image_dsc_t* MonitorIcon() {
    static lv_image_dsc_t d;
    if (d.data == nullptr) {
        d.header.magic = LV_IMAGE_HEADER_MAGIC;
        d.header.cf = LV_COLOR_FORMAT_A8;
        d.header.w = %d;
        d.header.h = %d;
        d.header.stride = %d;
        d.data_size = sizeof(kMonitorIconA8);
        d.data = kMonitorIconA8;
    }
    return &d;
}
""" % (W, W, W))
Image.eval(im, lambda v: 255 - v).save("preview.png")
