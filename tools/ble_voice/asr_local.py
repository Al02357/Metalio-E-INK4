#!/usr/bin/env python3
"""本地离线语音识别（免费）：sherpa-onnx + 阿里 SenseVoice-Small int8，中英粤日韩，CPU 几秒的话约 0.1～0.3 秒。

模型放 ~/.cache/eink_voice/sensevoice/（model.int8.onnx + tokens.txt，约 230MB），第一次用要先下载：
  https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17.tar.bz2
用法：python asr_local.py a.wav [b.wav ...]
"""
import os
import sys
import time
import wave

import numpy as np

MODEL_DIR = os.path.expanduser("~/.cache/eink_voice/sensevoice")


class LocalAsr:
    def __init__(self, model_dir=MODEL_DIR, threads=4):
        import sherpa_onnx
        model = os.path.join(model_dir, "model.int8.onnx")
        tokens = os.path.join(model_dir, "tokens.txt")
        if not (os.path.exists(model) and os.path.exists(tokens)):
            raise SystemExit(f"没找到模型：{model_dir}（见 asr_local.py 开头的下载说明）")
        self.rec = sherpa_onnx.OfflineRecognizer.from_sense_voice(
            model=model, tokens=tokens, num_threads=threads, language="zh", use_itn=True)  # zh 比 auto 准，英文单词照样能认

    def pcm(self, samples, rate=16000):
        """samples：int16 列表 / ndarray"""
        x = np.asarray(samples, dtype=np.float32) / 32768.0
        s = self.rec.create_stream()
        s.accept_waveform(rate, x)
        self.rec.decode_stream(s)
        return s.result.text.strip()

    def __call__(self, wav_path):
        with wave.open(wav_path) as w:
            x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
            return self.pcm(x, w.getframerate())


if __name__ == "__main__":
    t0 = time.time()
    asr = LocalAsr()
    print(f"模型加载 {time.time() - t0:.1f}s")
    for p in sys.argv[1:]:
        t0 = time.time()
        print(f"{os.path.basename(p)}  ({time.time() - t0:.2f}s)  {asr(p)}"
              .replace("(0.00s)", f"({time.time() - t0:.2f}s)"), flush=True)
