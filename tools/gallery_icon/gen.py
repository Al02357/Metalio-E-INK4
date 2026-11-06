"""生成首页「看图」图标（70×70 A8，线条风格同其它图标），输出 C 头文件。"""
from PIL import Image, ImageDraw
S = 4
W = 70
im = Image.new("L", (W * S, W * S), 0)
d = ImageDraw.Draw(im)
lw = 5 * S
d.rounded_rectangle((6 * S, 12 * S, 64 * S, 58 * S), radius=8 * S, outline=255, width=lw)
d.line([(13 * S, 50 * S), (27 * S, 34 * S), (37 * S, 44 * S), (45 * S, 37 * S), (57 * S, 50 * S)], fill=255, width=lw, joint="curve")
r = 5 * S
d.ellipse((46 * S - r, 24 * S - r, 46 * S + r, 24 * S + r), outline=255, width=4 * S)
im = im.resize((W, W), Image.LANCZOS)
data = im.tobytes()
with open("../../main/display/screen/gallery_screen/gallery_icon.h", "w") as f:
    f.write("// 由 tools/gallery_icon/gen.py 生成\n#pragma once\n#include \"lvgl.h\"\n")
    f.write("static const uint8_t kGalleryIconA8[%d] = {%s};\n" % (len(data), ",".join(str(b) for b in data)))
    f.write("""inline const lv_image_dsc_t* GalleryIcon() {
    static lv_image_dsc_t d;
    if (d.data == nullptr) {
        d.header.magic = LV_IMAGE_HEADER_MAGIC;
        d.header.cf = LV_COLOR_FORMAT_A8;
        d.header.w = %d;
        d.header.h = %d;
        d.header.stride = %d;
        d.data_size = sizeof(kGalleryIconA8);
        d.data = kGalleryIconA8;
    }
    return &d;
}
""" % (W, W, W))
Image.eval(im, lambda v: 255 - v).save("preview.png")
