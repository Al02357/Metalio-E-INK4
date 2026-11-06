#pragma once

#include <string>
#include <vector>

// 「我的 AI」配置：SD 卡 metalio/e-ink/my_ai.json（key 只放 SD，不进固件 / git）。
namespace my_ai {

struct ChatEndpoint {
    std::string vendor;    // "mimo" / "glm"
    std::string label;     // 界面显示名，如 "MiMo 2.6 Flash"
    std::string url;       // OpenAI 兼容 /chat/completions
    std::string key;
    std::string model;
    bool thinking = true;  // 是否开启深度思考
    bool bearer = true;    // true: Authorization: Bearer；false: api-key 头（MiMo）
    std::string max_tokens_field = "max_tokens";  // MiMo 用 max_completion_tokens
    bool web_search = false;  // 本次请求是否带联网搜索工具（仅 MiMo）

    bool SupportsWebSearch() const { return vendor == "mimo" || vendor == "glm"; }
    // 能看图：MiMo 2.6 全系、GLM-5.3-Flash（原生多模态）；GLM-5.3 只收文字
    bool SupportsVision() const { return vendor == "mimo" || model == "glm-5.3-flash"; }
    // glm-5.3 / glm-5.3-flash 强制思考，thinking=disabled 无效
    bool ThinkingForced() const { return model.rfind("glm-5.3", 0) == 0; }
};

struct Config {
    bool loaded = false;
    std::string error;          // 加载失败原因（给 UI 显示）

    std::string mimo_key;
    std::vector<ChatEndpoint> models;  // 可选对话模型（至少一个）
    int model_idx = 0;

    std::string asr_url = "https://api.xiaomimimo.com/v1/chat/completions";
    std::string asr_model = "mimo-v2.5-asr";
    std::string asr_language = "auto";

    bool tts = true;
    std::string tts_url = "https://api.xiaomimimo.com/v1/chat/completions";
    std::string tts_model = "mimo-v2.5-tts";
    std::string tts_voice = "冰糖";

    bool web_mimo = true;        // MiMo 联网搜索（需开通联网服务插件，按次计费）
    bool web_glm = true;         // GLM 联网搜索（Coding Plan 套餐内的搜索 MCP）
    bool WebOn(const ChatEndpoint& ep) const {
        return ep.vendor == "glm" ? web_glm : (ep.vendor == "mimo" ? web_mimo : false);
    }
    void SetWeb(const ChatEndpoint& ep, bool on) { (ep.vendor == "glm" ? web_glm : web_mimo) = on; }
    int web_max_keyword = 2;     // 一轮最多搜几个关键词（控制费用）
    std::string glm_key;         // GLM 联网搜索 MCP 也用它
    std::string glm_search_url = "https://open.bigmodel.cn/api/mcp/web_search_prime/mcp";

    std::string system_prompt;
    int max_turns = 8;           // 带进上下文的历史轮数
    int max_tokens = 4096;       // 含思考

    const ChatEndpoint& chat() const { return models[model_idx]; }
    int FindModel(const std::string& model) const;
};

/** @brief 从 SD 读取配置；须在非 PSRAM 栈任务调用（LVGL / 主循环） */
Config LoadConfig();

/** @brief 配置文件路径 */
const char* ConfigPath();

/** @brief 模型 id → 显示名（mimo-v2.6-pro → MiMo 2.6 Pro，glm-5.3-flash → GLM-5.3 Flash） */
std::string PrettyModelName(const std::string& model);

}  // namespace my_ai
