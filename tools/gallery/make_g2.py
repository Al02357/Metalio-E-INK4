"""把图片转成看图用的 .g2（480×H，4 级灰误差扩散抖动），可选用串口传到设备 SD 卡。
用法：python make_g2.py 输出目录 图1 图2 ...   （文件名以 00_test 开头的不做增强）
     python make_g2.py --send 串口 a.g2 b.g2 ...
"""
import base64, os, struct, sys, time
import numpy as np
from PIL import Image, ImageOps

W, H = 480, 755
DELAY = float(os.environ.get("G2_DELAY", "0.02"))


def to_g2(path):
    im = ImageOps.exif_transpose(Image.open(path)).convert("RGB")
    raw = os.path.basename(path).startswith("00_test")
    if im.width > im.height * 1.15:
        im = im.rotate(90, expand=True)  # 横图转成竖着放，占满屏幕（看时把设备横过来）
    if raw:
        g = im.convert("L")
    else:
        g = Image.blend(im.convert("L"), Image.fromarray(np.asarray(im).max(axis=2)), 0.5)
    sc = min(W / g.width, H / g.height)
    g = g.resize((max(1, round(g.width * sc)), max(1, round(g.height * sc))), Image.LANCZOS)
    if not raw:
        g = ImageOps.autocontrast(g, cutoff=1)
        g = Image.blend(g, ImageOps.equalize(g), 0.5)
        g = g.point(lambda v: round(255 * (v / 255) ** 0.8))
    canvas = Image.new("L", (W, H), 255)
    canvas.paste(g, ((W - g.width) // 2, (H - g.height) // 2))
    pal = Image.new("P", (1, 1))
    pal.putpalette([0, 0, 0, 85, 85, 85, 170, 170, 170, 255, 255, 255] + [0, 0, 0] * 252)
    q = np.frombuffer(canvas.convert("RGB").quantize(palette=pal, dither=Image.FLOYDSTEINBERG).tobytes(), np.uint8)
    q = q.reshape(-1, 4)
    packed = (q[:, 0] << 6) | (q[:, 1] << 4) | (q[:, 2] << 2) | q[:, 3]
    preview = Image.fromarray((q.reshape(H, W) * 85).astype(np.uint8))
    return b"G2" + struct.pack("<HHH", W, H, 0) + packed.astype(np.uint8).tobytes(), preview


def send(port, files):
    import serial
    s = serial.Serial(port, 115200, timeout=0.2)
    for f in files:
        data = open(f, "rb").read()
        name = "gallery/" + os.path.basename(f)
        t0 = time.time()
        s.write(("FPUT %s\n" % name).encode())
        time.sleep(0.3)
        for i in range(0, len(data), 384):
            s.write(b"FPB " + base64.b64encode(data[i:i + 384]) + b"\n")
            s.flush()
            time.sleep(DELAY)  # 设备 USB 收包靠轮询，发太快会丢
        s.write(b"FPE\n")
        s.flush()
        buf = b""
        while time.time() - t0 < 120 and b"FPUT done" not in buf:
            buf += s.read(4096)
        line = [l for l in buf.decode(errors="replace").splitlines() if "FPUT done" in l]
        print(name, len(data), "B", "%.1fs" % (time.time() - t0), line[-1][-60:] if line else "NO ACK")


if __name__ == "__main__":
    if sys.argv[1] == "--send":
        send(sys.argv[2], sys.argv[3:])
    else:
        out = sys.argv[1]
        os.makedirs(out, exist_ok=True)
        for p in sys.argv[2:]:
            g2, prev = to_g2(p)
            base = os.path.splitext(os.path.basename(p))[0]
            open(os.path.join(out, base + ".g2"), "wb").write(g2)
            prev.save(os.path.join(out, base + "_preview.png"))
            print(base, len(g2))
