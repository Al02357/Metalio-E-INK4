#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "my_ai_config.h"

// 「我的 AI」云端调用：MiMo ASR / OpenAI 兼容流式对话 / MiMo 流式 TTS。
// 全部为阻塞调用，须在工作任务里执行；cancel 置位后尽快返回 false。
namespace my_ai {

struct ChatMessage {
    std::string role;
    std::string content;
    std::string image;  // 可选：data:image/jpeg;base64,...（只用于本轮 user 消息）
};

struct ChatCallbacks {
    // 累计收到的思考字数（UTF-8 字节数近似）
    std::function<void(size_t reasoning_bytes)> on_reasoning;
    // 正文增量
    std::function<void(const std::string& delta)> on_content;
    // 联网搜索引用的网页数（流式首包返回）
    std::function<void(int refs)> on_web_refs;
    // GLM 正在联网搜索（query 为关键词）
    std::function<void(const std::string& query)> on_searching;
};

/** @brief 16kHz 单声道 PCM → 文本 */
bool Asr(const Config& cfg, const std::vector<int16_t>& pcm16k, std::string& text, std::string& err,
         const std::atomic<bool>& cancel);

/** @brief 流式对话；正文经 on_content 回调 */
bool ChatStream(const Config& cfg, const ChatEndpoint& ep, const std::vector<ChatMessage>& messages,
                const ChatCallbacks& cb, std::string& err, const std::atomic<bool>& cancel);

/** @brief 流式 TTS；on_pcm 收到 24kHz PCM16 单声道，返回 false 表示中止 */
bool TtsStream(const Config& cfg, const std::string& text,
               const std::function<bool(std::vector<int16_t>&& pcm24k)>& on_pcm, std::string& err,
               const std::atomic<bool>& cancel);

constexpr int kTtsSampleRate = 24000;

}  // namespace my_ai
