#!/usr/bin/env python3
"""用「我的AI」同一个语音识别（MiMo ASR）识别 wav。key 从 ~/.config/ai_keys.env 的 MIMO_API_KEY 读，不打印。

用法：python asr_mimo.py a.wav [b.wav ...]
"""
import base64
import json
import os
import sys
import time
import urllib.request

URL = "https://api.xiaomimimo.com/v1/chat/completions"
MODEL = "mimo-v2.5-asr"


def load_key(path="~/.config/ai_keys.env"):
    with open(os.path.expanduser(path), encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if "=" in line and not line.startswith("#"):
                k, v = line.split("=", 1)
                if k.replace("export", "").strip() == "MIMO_API_KEY":
                    return v.strip().strip("'\"")
    raise SystemExit("~/.config/ai_keys.env 里没有 MIMO_API_KEY")


def asr(path, key, language="auto"):
    with open(path, "rb") as f:
        b64 = base64.b64encode(f.read()).decode()
    body = {
        "model": MODEL,
        "messages": [{"role": "user", "content": [
            {"type": "input_audio", "input_audio": {"data": "data:audio/wav;base64," + b64}}]}],
        "asr_options": {"language": language},
    }
    req = urllib.request.Request(URL, data=json.dumps(body).encode(), method="POST",
                                 headers={"api-key": key, "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        resp = json.loads(r.read())
    return resp["choices"][0]["message"]["content"]


if __name__ == "__main__":
    key = load_key()
    for p in sys.argv[1:]:
        t0 = time.time()
        try:
            text = asr(p, key)
        except Exception as e:  # noqa
            text = f"[失败] {e}"
        print(f"{os.path.basename(p)}  ({time.time() - t0:.1f}s)  {text}", flush=True)
