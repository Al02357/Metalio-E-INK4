#!/usr/bin/env python3
"""Metalio E-Ink 蓝牙语音接收端（电脑上运行）。

设备在「蓝牙键鼠 → 语音」页按住说话，麦克风声音（16kHz IMA ADPCM）经低功耗蓝牙发过来，
本程序解码后：
  0. 默认：松手后用本地 SenseVoice 模型（免费、离线，见 asr_local.py）转成文字，直接打进当前光标所在的窗口；
     --asr cloud 改用自己 ECS 上的同款模型（免费，见 asr_cloud.py），--asr mimo 改用 MiMo（按次计费），--asr none 关掉；
  1. 同时送进一个虚拟麦克风（Linux 自动建「EInk_Mic」；Windows 用 VB-Cable，macOS 用 BlackHole），
     电脑上的语音输入法把输入设备选成它，就能直接识别；
  2. 每段话另存一个 wav（默认 ~/eink_voice/），方便排查或以后接自己的识别模型。

依赖：pip install bleak numpy（Windows / macOS 另需 sounddevice）
用法：python eink_voice.py [--address 10:20:BA:59:58:6E] [--hold-key ctrl+alt+v]
"""
import argparse
import asyncio
import os
import platform
import shutil
import struct
import subprocess
import sys
import time
import wave
import warnings

warnings.filterwarnings("ignore", message="Using default MTU")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bleak import BleakClient, BleakScanner  # noqa: E402

SVC_UUID = "e1c80001-6f3a-4b2e-9d7a-3f5b2c1a0e4d"
AUDIO_UUID = "e1c80002-6f3a-4b2e-9d7a-3f5b2c1a0e4d"
CTRL_UUID = "e1c80003-6f3a-4b2e-9d7a-3f5b2c1a0e4d"
RATE = 16000

STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
        107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
        5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385,
        24623, 27086, 29794, 32767]
IDX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def adpcm_decode(payload, pred, idx):
    """IMA ADPCM → int16 列表；每字节低半字节在前"""
    out = []
    for b in payload:
        for code in (b & 0x0F, b >> 4):
            step = STEP[idx]
            diff = step >> 3
            if code & 4:
                diff += step
            if code & 2:
                diff += step >> 1
            if code & 1:
                diff += step >> 2
            pred = pred - diff if code & 8 else pred + diff
            pred = max(-32768, min(32767, pred))
            idx = max(0, min(88, idx + IDX[code]))
            out.append(pred)
    return out


class Gain:
    """简单自动增益：设备麦克风经降噪后偏小，按最近的峰值放大，最多 8 倍"""

    def __init__(self):
        self.g = 3.0

    def apply(self, pcm):
        peak = max((abs(x) for x in pcm), default=0)
        if peak > 0:
            want = min(8.0, 20000.0 / peak)
            self.g = self.g + (want - self.g) * (0.05 if want > self.g else 0.5)
        return [max(-32768, min(32767, int(x * self.g))) for x in pcm]


# ---------------------------------------------------------------- 虚拟麦克风

class LinuxMic:
    """PulseAudio / PipeWire：建一个空输出 eink_voice，再把它的监听端做成输入设备 EInk_Mic"""

    SINK = "eink_voice"
    SOURCE = "eink_mic"

    def __init__(self, set_default):
        self.proc = None
        self.set_default = set_default
        self.prev_default = None
        if not shutil.which("pactl") or not shutil.which("pacat"):
            raise RuntimeError("没找到 pactl / pacat（PulseAudio 或 PipeWire）")
        sinks = subprocess.run(["pactl", "list", "short", "sinks"], capture_output=True, text=True).stdout
        if self.SINK not in sinks:
            subprocess.run(["pactl", "load-module", "module-null-sink", f"sink_name={self.SINK}",
                            "sink_properties=device.description=EInk_Voice_Out"], check=True, capture_output=True)
        sources = subprocess.run(["pactl", "list", "short", "sources"], capture_output=True, text=True).stdout
        if f"{self.SOURCE}\t" not in sources:
            subprocess.run(["pactl", "load-module", "module-remap-source", f"master={self.SINK}.monitor",
                            f"source_name={self.SOURCE}", "source_properties=device.description=EInk_Mic"],
                           check=True, capture_output=True)
        log("虚拟麦克风：EInk_Mic（输入法 / 录音软件里把麦克风选成它）")

    def begin(self):
        if self.set_default:
            info = subprocess.run(["pactl", "get-default-source"], capture_output=True, text=True).stdout.strip()
            self.prev_default = info or None
            subprocess.run(["pactl", "set-default-source", self.SOURCE])
        if self.proc is None or self.proc.poll() is not None:
            self.proc = subprocess.Popen(["pacat", "--playback", "-d", self.SINK, "--rate", str(RATE), "--channels",
                                          "1", "--format", "s16le", "--latency-msec", "30"], stdin=subprocess.PIPE)

    def write(self, pcm):
        if self.proc and self.proc.poll() is None:
            try:
                self.proc.stdin.write(struct.pack(f"<{len(pcm)}h", *pcm))
                self.proc.stdin.flush()
            except BrokenPipeError:
                self.proc = None

    def end(self):
        # 补 0.3 秒静音再停，免得输入法把最后一个字截掉
        self.write([0] * (RATE * 3 // 10))
        if self.set_default and self.prev_default:
            subprocess.run(["pactl", "set-default-source", self.prev_default])


class SoundDeviceMic:
    """Windows（VB-Cable 的 CABLE Input）/ macOS（BlackHole）：往虚拟声卡的输出端放音"""

    def __init__(self, name):
        import sounddevice as sd  # noqa: 只在这两个系统上需要
        self.sd = sd
        dev = None
        for i, d in enumerate(sd.query_devices()):
            if d["max_output_channels"] > 0 and name.lower() in d["name"].lower():
                dev = i
                break
        if dev is None:
            raise RuntimeError(f"没找到输出设备「{name}」，请先装虚拟声卡（Windows: VB-Cable，macOS: BlackHole）")
        self.stream = sd.RawOutputStream(samplerate=RATE, channels=1, dtype="int16", device=dev, latency="low")
        self.stream.start()
        log(f"虚拟麦克风：往「{sd.query_devices(dev)['name']}」放音，输入法里选对应的输入端")

    def begin(self):
        pass

    def write(self, pcm):
        self.stream.write(struct.pack(f"<{len(pcm)}h", *pcm))

    def end(self):
        self.write([0] * (RATE * 3 // 10))


class NoMic:
    def begin(self):
        pass

    def write(self, pcm):
        pass

    def end(self):
        pass


def make_mic(args):
    if args.no_mic:
        return NoMic()
    system = platform.system()
    if system == "Linux":
        return LinuxMic(args.set_default)
    name = args.mic_device or ("CABLE Input" if system == "Windows" else "BlackHole")
    return SoundDeviceMic(name)


# ---------------------------------------------------------------- 按住说话时按住的热键（可选，用来触发输入法的语音键）

class HoldKey:
    def __init__(self, combo):
        self.keys = [k.strip() for k in combo.split("+")] if combo else []
        self.ctl = None
        if not self.keys:
            return
        try:
            from pynput.keyboard import Controller, Key
            self.ctl = Controller()
            self.map = lambda k: getattr(Key, k, k) if len(k) > 1 else k
        except ImportError:
            if platform.system() == "Linux" and shutil.which("xdotool"):
                self.ctl = "xdotool"
            else:
                log("要用 --hold-key 请先 pip install pynput")
                self.keys = []

    def down(self):
        if not self.keys:
            return
        if self.ctl == "xdotool":
            subprocess.run(["xdotool", "keydown", "+".join(self.keys)])
        else:
            for k in self.keys:
                self.ctl.press(self.map(k))

    def up(self):
        if not self.keys:
            return
        if self.ctl == "xdotool":
            subprocess.run(["xdotool", "keyup", "+".join(reversed(self.keys))])
        else:
            for k in reversed(self.keys):
                self.ctl.release(self.map(k))


# ---------------------------------------------------------------- 识别并打字

class TextOut:
    """把文字打进当前焦点窗口。Linux/X11：放进剪贴板再按粘贴键（终端用 Ctrl+Shift+V，其他 Ctrl+V）"""

    def __init__(self):
        self.system = platform.system()
        self.kb = None
        if self.system == "Linux":
            if not shutil.which("xdotool"):
                raise RuntimeError("Linux 打字需要 xdotool")
        else:
            from pynput.keyboard import Controller  # noqa: Windows / macOS
            self.kb = Controller()

    TERMINALS = ("alacritty", "gnome-terminal", "kitty", "xterm", "konsole", "terminator", "wezterm", "tilix",
                 "urxvt", "st-256color", "foot", "xfce4-terminal")

    def type(self, text):
        if not text:
            return
        if self.kb is not None:
            self.kb.type(text)
            return
        # Linux：走剪贴板 + 粘贴键。xdotool type 打汉字要临时改键盘映射，终端（Alacritty 等）里会丢字
        win = subprocess.run(["xdotool", "getactivewindow"], capture_output=True, text=True).stdout.strip()
        cls = ""
        if win:
            cls = subprocess.run(["xprop", "-id", win, "WM_CLASS"], capture_output=True, text=True).stdout.lower()
        helper = subprocess.Popen(["/usr/bin/python3", os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                                     "clip_helper.py"), "60"],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        helper.stdin.write(text)
        helper.stdin.close()
        if helper.stdout.readline().strip() != "ready":
            raise RuntimeError("剪贴板没设上")
        key = "ctrl+shift+v" if any(t in cls for t in self.TERMINALS) else "ctrl+v"
        subprocess.run(["xdotool", "key", "--clearmodifiers", key])


ENGINES = ["local", "cloud", "mimo"]  # 设备「开始说话」包里的识别方式：0 / 1 / 2
ENGINE_NAMES = {"local": "电脑本机", "cloud": "自己服务器", "mimo": "MiMo"}


class Recognizer:
    """三种识别方式都备着，用到才加载；每段话用哪个由设备选（设备没带就用启动参数）"""

    def __init__(self, default):
        self.default = default
        self._local = self._cloud = self._mimo_key = None

    def local(self):
        if self._local is None:
            import asr_local
            self._local = asr_local.LocalAsr()
        return self._local

    def __call__(self, wav_path, kind=None):
        kind = kind or self.default
        if kind == "cloud":
            try:
                if self._cloud is None:
                    import asr_cloud
                    self._cloud = asr_cloud.CloudAsr()
                return self._cloud(wav_path).strip(), "cloud"
            except Exception as e:  # noqa
                log(f"服务器识别失败，这次改用电脑本机：{e}")
                kind = "local"
        if kind == "mimo":
            import asr_mimo
            if self._mimo_key is None:
                self._mimo_key = asr_mimo.load_key()
            return asr_mimo.asr(wav_path, self._mimo_key).strip(), "mimo"
        return self.local()(wav_path).strip(), "local"


def clean_text(text, seconds):
    """识别结果把关：去掉模型的标记（如 <chinese>），明显不对的结果整段丢掉，不往窗口里打"""
    import re
    text = re.sub(r"<\|?[A-Za-z_]+\|?>", "", text)  # <chinese> / <|zh|> 之类
    text = re.sub(r"\s+", " ", text).strip()
    if not re.sub(r"[\s\.,!?;:'\"。，！？；：、…—·（）()《》“”‘’-]", "", text):
        return "", "识别结果为空"
    # 人说话一秒最多七八个字；超过很多就是模型乱输出
    limit = int(seconds * 12) + 10
    if len(text) > limit:
        return "", f"结果太长（{len(text)} 字，{seconds:.1f}s 录音最多该有约 {limit} 字）"
    # 同一个词重复一大串
    if re.search(r"(.{1,8}?)\1{5,}", text):
        return "", "结果是重复的乱码"
    return text, ""


# ---------------------------------------------------------------- 接收

class Receiver:
    def __init__(self, args, mic, hold, asr=None, out=None):
        self.args = args
        self.asr = asr
        self.engine = None
        self.client = None
        self.loop = None
        self.out = out
        self.mic = mic
        self.hold = hold
        self.gain = Gain()
        self.talking = False
        self.pcm = []
        self.next_seq = None
        self.frame_n = 320
        self.lost = 0
        self.t0 = 0

    def on_ctrl(self, _, data: bytearray):
        if not data:
            return
        if data[0] == 0x01:
            self.engine = ENGINES[data[3]] if len(data) >= 4 and data[3] < len(ENGINES) else None
            self.talking = True
            self.pcm = []
            self.next_seq = 0
            self.lost = 0
            self.t0 = time.time()
            self.mic.begin()
            self.hold.down()
            log("● 开始说话（识别：%s）" % ENGINE_NAMES[self.engine or self.args.asr] if (self.engine or self.args.asr) in ENGINE_NAMES else "● 开始说话")
        elif data[0] == 0x02 and self.talking:
            self.talking = False
            self.hold.up()
            self.mic.end()
            self.save()

    def on_audio(self, _, data: bytearray):
        if len(data) < 7:
            return
        seq, pred, idx = struct.unpack_from("<HhB", data, 0)
        payload = bytes(data[6:])
        if not self.talking:  # 没收到开始包（比如程序中途连上）：也照样收
            self.on_ctrl(None, bytearray([0x01]))
        if self.next_seq is not None and seq != self.next_seq:
            gap = (seq - self.next_seq) & 0xFFFF
            if gap < 50:  # 丢包补静音，时长对得上
                self.lost += gap
                fill = [0] * (self.frame_n * gap)
                self.pcm += fill
                self.mic.write(fill)
        self.next_seq = (seq + 1) & 0xFFFF
        pcm = adpcm_decode(payload, pred, idx)
        self.frame_n = len(pcm)
        pcm = self.gain.apply(pcm)
        self.pcm += pcm
        self.mic.write(pcm)

    def save(self):
        n = len(self.pcm)
        if n == 0:
            log("○ 结束（没有声音）")
            self.report(1, "没收到声音")
            return
        os.makedirs(self.args.out_dir, exist_ok=True)
        path = os.path.join(self.args.out_dir, time.strftime("%Y%m%d-%H%M%S") + ".wav")
        with wave.open(path, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(RATE)
            w.writeframes(struct.pack(f"<{n}h", *self.pcm))
        rms = (sum(x * x for x in self.pcm) / n) ** 0.5
        log(f"○ 结束 {n / RATE:.1f}s  音量 RMS {rms:.0f}  丢包 {self.lost}  → {path}")
        if self.asr and n < RATE // 3:
            self.report(1, "太短")
        elif self.asr:
            import threading
            threading.Thread(target=self.recognize, args=(path,), daemon=True).start()

    def report(self, status, text):
        """把结果回写给设备显示：[0x10][0 成功 / 1 丢弃 / 2 失败][文字]"""
        if not (self.client and self.loop):
            return
        data = bytes([0x10, status]) + text.encode("utf-8")[:180]
        try:
            asyncio.run_coroutine_threadsafe(self.client.write_gatt_char(CTRL_UUID, data, response=True),
                                             self.loop).result(5)
        except Exception as e:  # noqa
            log(f"回写设备失败：{e}")

    def stuck_audio(self):
        """设备蓝牙音频芯片卡死时，麦克风数据是同一段 512 点反复循环：识别只会出「我」之类，直接报给设备"""
        try:
            import numpy as np
            x = np.asarray(self.pcm[:RATE * 2], dtype=np.float64)
            if len(x) < 4096:
                return False
            a, b = x[512:], x[:-512]
            if a.std() < 1 or b.std() < 1:
                return False
            return float(np.corrcoef(a, b)[0, 1]) > 0.85
        except Exception:  # noqa
            return False

    def recognize(self, path):
        t0 = time.time()
        seconds = len(self.pcm) / RATE
        if self.stuck_audio():
            log("麦克风数据在循环（设备蓝牙音频芯片卡住了），不识别")
            self.report(2, "麦克风卡住了：退出蓝牙键鼠再进一次")
            return
        try:
            raw, used = self.asr(path, self.engine)
        except Exception as e:  # noqa
            log(f"识别失败：{e}")
            self.report(2, str(e)[:60])
            return
        dt = time.time() - t0
        text, why = clean_text(raw, seconds)
        if dt > 10:
            text, why = "", f"识别太慢（{dt:.0f}s），结果不打出来"
        if not text:
            log(f"识别（{ENGINE_NAMES[used]} {dt:.1f}s）：丢弃，{why}；原始结果前 60 字：{raw[:60]!r}")
            self.report(1, "")
            return
        log(f"识别（{ENGINE_NAMES[used]} {dt:.1f}s）：{text}")
        self.report(0, text)
        if text and self.out:
            try:
                self.out.type(text)
            except Exception as e:  # noqa
                log(f"打字失败：{e}")


def linux_known_device(addr, name):
    """BlueZ 里已配对、正连着的设备不广播，bleak 扫不到；直接按 D-Bus 路径构造设备对象"""
    from bleak.backends.device import BLEDevice
    path = "/org/bluez/hci0/dev_" + addr.replace(":", "_")
    return BLEDevice(addr, name, {"path": path, "props": {"Address": addr, "Name": name}})


async def find_device(args):
    if args.address:
        if platform.system() == "Linux":
            return linux_known_device(args.address, args.name)
        return args.address
    log(f"找「{args.name}」…（先在设备上打开「蓝牙键鼠」页）")
    if platform.system() == "Linux" and shutil.which("bluetoothctl"):
        # 已经被系统连着（当键盘鼠标用）时设备不再广播，从已配对列表里找
        out = subprocess.run(["bluetoothctl", "devices"], capture_output=True, text=True).stdout
        for line in out.splitlines():
            parts = line.split(" ", 2)
            if len(parts) == 3 and parts[2].strip() == args.name:
                return linux_known_device(parts[1], args.name)
    return await BleakScanner.find_device_by_name(args.name, timeout=8.0)


async def run(args):
    mic = make_mic(args)
    hold = HoldKey(args.hold_key)
    asr = Recognizer(args.asr) if args.asr != "none" else None
    if asr:
        log(f"语音识别：默认{ENGINE_NAMES[args.asr]}；设备「语音」页可以切换（电脑本机 / 自己服务器 / MiMo）")
        asr.local()  # 先把本机模型加载好（约 2 秒），服务器识别失败时也要用
    out = TextOut() if asr and not args.no_type else None
    rx = Receiver(args, mic, hold, asr, out)
    while True:
        target = await find_device(args)
        if not target:
            log("没找到设备，5 秒后再试")
            await asyncio.sleep(5)
            continue
        done = asyncio.Event()
        try:
            async with BleakClient(target, disconnected_callback=lambda _: done.set(), timeout=20.0) as c:
                ver = (await c.read_gatt_char(CTRL_UUID)).decode(errors="replace")
                log(f"已连上 {c.address}  {ver}  MTU={c.mtu_size}")
                await c.start_notify(CTRL_UUID, rx.on_ctrl)
                await c.start_notify(AUDIO_UUID, rx.on_audio)
                rx.client, rx.loop = c, asyncio.get_running_loop()
                log("等待按住说话…（Ctrl+C 退出）")
                await done.wait()
                log("连接断开")
                rx.client = None
        except Exception as e:  # noqa: 断线、找不到服务等，统一重连
            log(f"连接出错：{e}")
        if rx.talking:
            rx.on_ctrl(None, bytearray([0x02]))
        await asyncio.sleep(3)


def main():
    ap = argparse.ArgumentParser(description="Metalio E-Ink 蓝牙语音接收端")
    ap.add_argument("--name", default="Metalio E-Ink", help="设备蓝牙名")
    ap.add_argument("--address", help="设备蓝牙地址（找不到时手动指定）")
    ap.add_argument("--out-dir", default=os.path.expanduser("~/eink_voice"), help="wav 存放目录")
    ap.add_argument("--no-mic", action="store_true", help="不送虚拟麦克风，只存 wav")
    ap.add_argument("--mic-device", help="Windows/macOS：虚拟声卡输出端名字（默认 CABLE Input / BlackHole）")
    ap.add_argument("--set-default", action="store_true", help="Linux：说话期间把系统默认麦克风临时切成 EInk_Mic")
    ap.add_argument("--asr", default="local", choices=["local", "cloud", "mimo", "none"],
                    help="设备没指定时用的识别方式：local 本机 SenseVoice（默认，免费）/ cloud 自己服务器上的 SenseVoice（免费）"
                         " / mimo 小米云端（按次计费）/ none 不识别。设备「语音」页选了就以设备为准")
    ap.add_argument("--no-type", action="store_true", help="只识别、打印，不打进窗口")
    ap.add_argument("--hold-key", help="说话期间按住的键，触发输入法语音输入，如 ctrl+alt+v")
    args = ap.parse_args()
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())
