#pragma once

#include "lvgl.h"

// 「我的 AI」：按住说话 → MiMo ASR → MiMo / GLM 流式对话（可思考）→ MiMo 流式 TTS 朗读。
// 配置见 SD 卡 metalio/e-ink/my_ai.json。
class MyAiScreen {
public:
    /** @brief 创建页面 */
    static lv_obj_t* Create();
    /** @brief 当前是否在本页（跨线程安全） */
    static bool IsActive();
    /** @brief 调试：发送第 idx 条建议问题（跳过语音识别） */
    static void DebugAsk(int idx);
    /** @brief 调试：打开模型选择浮层（须在 LVGL 线程） */
    static void DebugOpenPicker();
    /** @brief 调试：打开历史对话（须在 LVGL 线程） */
    static void DebugOpenHistory();
    /** @brief 调试：直接提问（跳过语音识别） */
    static void DebugAskText(const char* text);
    /** @brief 调试：选模型 */
    static void DebugSelectModel(int idx);
    /** @brief 调试：载入第 idx 个历史对话（须在 LVGL 线程） */
    static void DebugLoadHistory(int idx);
    /** @brief 截下当前屏幕，跳到我的AI，下一句提问带上这张截图（任意线程调用） */
    static void RequestOpenWithShot();
    /** @brief 跳到我的AI；若 BOOT 仍按着就直接开始录音（任意线程调用） */
    static void RequestOpenListen();

    // ---- 浮层：任意页面叠一张底部对话卡片，问完不用切应用
    /** @brief 截下当前页面（作对话上下文）并打开浮层；listen 且 BOOT 仍按着就直接开录（任意线程） */
    static void RequestOverlay(bool listen);
    /** @brief 不截图打开浮层（待机唤醒用） */
    static void RequestOverlayNoShot(bool listen);
    /** @brief 浮层是否打开 */
    static bool OverlayOpen();
    /** @brief 关闭浮层（任意线程） */
    static void RequestOverlayClose();
    /** @brief 浮层打开时的 BOOT 处理；浮层没开返回 false */
    static bool OverlayBootClick();
    static bool OverlayBootLongPress();
    static bool OverlayBootPressUp();
};
