#include "my_ai_config.h"

#include <cJSON.h>
#include <esp_log.h>

#include <cctype>
#include <cstdio>
#include <memory>

#include "sd_paths.h"

namespace my_ai {
namespace {

constexpr const char* TAG = "MyAiConfig";
constexpr const char* kPath = SD_APP_ROOT "/my_ai.json";
constexpr long kMaxConfigBytes = 16 * 1024;

constexpr const char* kMimoUrl = "https://api.xiaomimimo.com/v1/chat/completions";
constexpr const char* kGlmUrl = "https://open.bigmodel.cn/api/coding/paas/v4/chat/completions";

constexpr const char* kDefaultPrompt =
    "你是墨水屏设备上的语音助手。用户通过语音和你对话，你的回答会显示在墨水屏上并被朗读出来。"
    "回答要简洁、口语化，一般不超过 150 字；不要使用 Markdown、表格、代码块或表情符号。";

void ReadStr(const cJSON* obj, const char* name, std::string& out) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (cJSON_IsString(v) && v->valuestring != nullptr) {
        out = v->valuestring;
    }
}

void ReadBool(const cJSON* obj, const char* name, bool& out) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (cJSON_IsBool(v)) {
        out = cJSON_IsTrue(v);
    }
}

void ReadInt(const cJSON* obj, const char* name, int& out, int lo, int hi) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (cJSON_IsNumber(v)) {
        int x = v->valueint;
        out = x < lo ? lo : (x > hi ? hi : x);
    }
}

// 读字符串数组；没有该字段时保持 out 不变
void ReadStrList(const cJSON* obj, const char* name, std::vector<std::string>& out) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (!cJSON_IsArray(v)) return;
    std::vector<std::string> list;
    const cJSON* it = nullptr;
    cJSON_ArrayForEach(it, v) {
        if (cJSON_IsString(it) && it->valuestring != nullptr && it->valuestring[0] != '\0') {
            list.emplace_back(it->valuestring);
        }
    }
    if (!list.empty()) out = std::move(list);
}

}  // namespace

const char* ConfigPath() { return kPath; }

std::string PrettyModelName(const std::string& model) {
    std::string rest;
    std::string out;
    if (model.rfind("mimo-v", 0) == 0) {
        out = "MiMo ";
        rest = model.substr(6);
    } else if (model.rfind("glm-", 0) == 0) {
        // glm-5.3-flash → GLM-5.3 Flash：版本号保留连字符
        const size_t dash = model.find('-', 4);
        out = "GLM-" + model.substr(4, dash == std::string::npos ? std::string::npos : dash - 4);
        if (dash == std::string::npos) return out;
        out.push_back(' ');
        rest = model.substr(dash + 1);
    } else {
        return model;
    }
    bool cap = true;
    for (char c : rest) {
        if (c == '-') {
            out.push_back(' ');
            cap = true;
            continue;
        }
        out.push_back(cap ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c);
        cap = false;
    }
    return out;
}

int Config::FindModel(const std::string& model) const {
    for (size_t i = 0; i < models.size(); ++i) {
        if (models[i].model == model) return static_cast<int>(i);
    }
    return -1;
}

Config LoadConfig() {
    Config c;
    c.system_prompt = kDefaultPrompt;

    FILE* f = std::fopen(kPath, "rb");
    if (f == nullptr) {
        c.error = "未找到 SD 卡配置 metalio/e-ink/my_ai.json";
        ESP_LOGW(TAG, "open %s failed", kPath);
        return c;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > kMaxConfigBytes) {
        std::fclose(f);
        c.error = "my_ai.json 大小异常";
        return c;
    }
    std::string text(static_cast<size_t>(size), '\0');
    const size_t n = std::fread(text.data(), 1, text.size(), f);
    std::fclose(f);
    text.resize(n);

    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(text.c_str()), cJSON_Delete);
    if (!root || !cJSON_IsObject(root.get())) {
        c.error = "my_ai.json 不是合法 JSON";
        return c;
    }
    const cJSON* r = root.get();

    std::string glm_key, glm_url = kGlmUrl;
    ReadStr(r, "mimo_api_key", c.mimo_key);
    ReadStr(r, "glm_api_key", glm_key);
    ReadStr(r, "glm_url", glm_url);

    // 模型列表；兼容旧字段 mimo_model / glm_model（放到列表最前）
    std::vector<std::string> mimo_models = {"mimo-v2.6-flash", "mimo-v2.6-pro"};
    std::vector<std::string> glm_models = {"glm-5.3", "glm-5.3-flash"};
    ReadStrList(r, "mimo_models", mimo_models);
    ReadStrList(r, "glm_models", glm_models);
    auto front = [](std::vector<std::string>& v, const std::string& m) {
        if (m.empty()) return;
        for (auto it = v.begin(); it != v.end(); ++it) {
            if (*it == m) {
                v.erase(it);
                break;
            }
        }
        v.insert(v.begin(), m);
    };
    std::string legacy;
    ReadStr(r, "mimo_model", legacy);
    front(mimo_models, legacy);
    legacy.clear();
    ReadStr(r, "glm_model", legacy);
    front(glm_models, legacy);

    bool mimo_thinking = true, glm_thinking = true;
    ReadBool(r, "mimo_thinking", mimo_thinking);
    ReadBool(r, "glm_thinking", glm_thinking);

    if (!c.mimo_key.empty()) {
        for (const auto& m : mimo_models) {
            ChatEndpoint ep;
            ep.vendor = "mimo";
            ep.label = PrettyModelName(m);
            ep.url = kMimoUrl;
            ep.key = c.mimo_key;
            ep.model = m;
            ep.thinking = mimo_thinking;
            ep.bearer = false;
            ep.max_tokens_field = "max_completion_tokens";
            c.models.push_back(std::move(ep));
        }
    }
    c.glm_key = glm_key;
    ReadStr(r, "glm_search_url", c.glm_search_url);
    if (!glm_key.empty()) {
        for (const auto& m : glm_models) {
            ChatEndpoint ep;
            ep.vendor = "glm";
            ep.label = PrettyModelName(m);
            ep.url = glm_url;
            ep.key = glm_key;
            ep.model = m;
            ep.thinking = glm_thinking;  // glm-5.3 / 5.3-flash 强制思考，关不掉
            ep.bearer = true;
            c.models.push_back(std::move(ep));
        }
    }

    ReadBool(r, "web_search", c.web_mimo);  // 旧字段：MiMo
    ReadBool(r, "mimo_web_search", c.web_mimo);
    ReadBool(r, "glm_web_search", c.web_glm);
    ReadInt(r, "web_max_keyword", c.web_max_keyword, 1, 5);
    ReadStr(r, "asr_language", c.asr_language);
    ReadBool(r, "tts", c.tts);
    ReadStr(r, "tts_voice", c.tts_voice);
    ReadStr(r, "system_prompt", c.system_prompt);
    ReadInt(r, "max_turns", c.max_turns, 0, 30);
    ReadInt(r, "max_tokens", c.max_tokens, 256, 32768);

    if (c.mimo_key.empty()) {
        c.error = "my_ai.json 缺少 mimo_api_key（语音识别 / 朗读需要）";
        return c;
    }

    // 默认模型：default_model；兼容旧字段 chat_provider=glm
    std::string def;
    ReadStr(r, "default_model", def);
    std::string provider;
    ReadStr(r, "chat_provider", provider);
    int idx = def.empty() ? -1 : c.FindModel(def);
    if (idx < 0 && provider == "glm") {
        for (size_t i = 0; i < c.models.size(); ++i) {
            if (c.models[i].vendor == "glm") {
                idx = static_cast<int>(i);
                break;
            }
        }
    }
    c.model_idx = idx < 0 ? 0 : idx;
    c.loaded = true;
    ESP_LOGI(TAG, "loaded: %u models, default=%s thinking=%d tts=%d voice=%s turns=%d",
             static_cast<unsigned>(c.models.size()), c.chat().model.c_str(), c.chat().thinking ? 1 : 0,
             c.tts ? 1 : 0, c.tts_voice.c_str(), c.max_turns);
    return c;
}

}  // namespace my_ai
