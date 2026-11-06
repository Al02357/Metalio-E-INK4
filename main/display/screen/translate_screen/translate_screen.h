#pragma once

#include "lvgl.h"

// 「翻译」：边说边出字幕的实时翻译（阿里云百炼 qwen livetranslate realtime，WebSocket）。
// 配置在 SD my_ai.json：dashscope_api_key（必填）、dashscope_workspace 或 dashscope_ws_url、translate_model。
class TranslateScreen {
public:
    static lv_obj_t* Create();
    static bool IsActive();
    /** 调试：START / STOP / LANG xx */
    static void DebugCmd(const char* cmd);
};
