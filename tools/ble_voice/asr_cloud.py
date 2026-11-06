#!/usr/bin/env python3
"""云端语音识别：发到自己 ECS 上的 /api/asr（SenseVoice，免费，只占服务器资源）。

配置 ~/.config/eink_voice.json：{"server": "http://IP:8080", "token": "..."}（和设备 weread.json 同一个 token）。
服务器空闲 30 分钟会自动关机：连不上时调用 weread_cloud/ecs_power.py wake 叫醒（约 30～60 秒），叫醒前先返回失败，
由调用方改用本地识别。
用法：python asr_cloud.py a.wav [b.wav ...]
"""
import json
import os
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request

CFG = os.path.expanduser("~/.config/eink_voice.json")
ECS_POWER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "weread_cloud", "ecs_power.py")


class CloudAsr:
    def __init__(self, cfg_path=CFG):
        with open(cfg_path, encoding="utf-8") as f:
            c = json.load(f)
        self.server = c["server"].rstrip("/")
        self.token = c["token"]
        self.waking = False

    def wake_async(self):
        """后台叫醒 ECS（不阻塞这次识别）"""
        if self.waking or not os.path.exists(ECS_POWER):
            return
        self.waking = True

        def run():
            try:
                subprocess.run([sys.executable, ECS_POWER, "wake"], capture_output=True, timeout=180)
            finally:
                self.waking = False

        threading.Thread(target=run, daemon=True).start()

    def __call__(self, wav_path, timeout=4):
        with open(wav_path, "rb") as f:
            body = f.read()
        req = urllib.request.Request(self.server + "/api/asr", data=body, method="POST",
                                     headers={"X-Token": self.token, "Content-Type": "audio/wav"})
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return json.loads(r.read())["text"]
        except urllib.error.HTTPError as e:
            raise RuntimeError(f"服务器返回 {e.code}: {e.read()[:200].decode(errors='replace')}")
        except (urllib.error.URLError, TimeoutError, ConnectionError) as e:
            self.wake_async()
            raise RuntimeError(f"连不上服务器（可能已自动关机，正在叫醒）：{e}")


if __name__ == "__main__":
    asr = CloudAsr()
    for p in sys.argv[1:]:
        t0 = time.time()
        try:
            text = asr(p, timeout=30)
        except Exception as e:  # noqa
            text = f"[失败] {e}"
        print(f"{os.path.basename(p)}  ({time.time() - t0:.2f}s)  {text}", flush=True)
