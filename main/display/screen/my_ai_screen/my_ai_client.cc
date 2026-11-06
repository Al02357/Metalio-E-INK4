#include "my_ai_client.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <mbedtls/base64.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>

#include "board.h"

namespace my_ai {
namespace {

constexpr const char* TAG = "MyAiClient";
constexpr int kConnectReadTimeoutMs = 30000;

using JsonPtr = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

std::unique_ptr<Http> NewHttp() {
    auto* network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return nullptr;
    }
    auto http = network->CreateHttp(0);
    if (http) {
        http->SetTimeout(kConnectReadTimeoutMs);
    }
    return http;
}

void ApplyAuth(Http& http, const std::string& key, bool bearer) {
    if (bearer) {
        http.SetHeader("Authorization", "Bearer " + key);
    } else {
        http.SetHeader("api-key", key);
    }
    http.SetHeader("Content-Type", "application/json");
}

// 从错误回包中挑出可读信息
std::string ExtractError(int status, const std::string& body) {
    std::string msg = "HTTP " + std::to_string(status);
    // 流式接口的错误也可能带 SSE 前缀 "data:"
    size_t off = 0;
    while (off < body.size() && (body[off] == ' ' || body[off] == '\n' || body[off] == '\r')) ++off;
    if (body.compare(off, 5, "data:") == 0) off += 5;
    JsonPtr root(cJSON_Parse(body.c_str() + off), cJSON_Delete);
    if (root) {
        const cJSON* e = cJSON_GetObjectItemCaseSensitive(root.get(), "error");
        const cJSON* m = nullptr;
        if (cJSON_IsObject(e)) {
            m = cJSON_GetObjectItemCaseSensitive(e, "message");
        } else if (cJSON_IsString(e)) {
            m = e;
        }
        if (m == nullptr) {
            m = cJSON_GetObjectItemCaseSensitive(root.get(), "msg");
        }
        if (cJSON_IsString(m) && m->valuestring != nullptr) {
            msg += ": ";
            msg += m->valuestring;
            const cJSON* prm = cJSON_IsObject(e) ? cJSON_GetObjectItemCaseSensitive(e, "param") : nullptr;
            if (cJSON_IsString(prm) && prm->valuestring != nullptr && prm->valuestring[0] != '\0') {
                msg += " (";
                msg += prm->valuestring;
                msg += ")";
            }
            return msg;
        }
    }
    if (!body.empty()) {
        msg += ": " + body.substr(0, 160);
    }
    return msg;
}

bool OpenPost(Http& http, const std::string& url, std::string&& body, std::string& err) {
    http.SetContent(std::move(body));
    if (!http.Open("POST", url)) {
        err = "连接失败 (0x" + std::to_string(http.GetLastError()) + ")";
        return false;
    }
    const int status = http.GetStatusCode();
    if (status != 200) {
        const std::string resp = http.ReadAll();
        err = ExtractError(status, resp);
        ESP_LOGW(TAG, "POST %s -> %s", url.c_str(), err.c_str());
        http.Close();
        return false;
    }
    return true;
}

// 逐行读 SSE；每个 data: 负载交给 on_data（返回 false 中止）。
bool ReadSse(Http& http, const std::atomic<bool>& cancel,
             const std::function<bool(const char* json)>& on_data, std::string& err) {
    std::string buf;
    std::vector<char> chunk(2048);
    while (true) {
        if (cancel.load(std::memory_order_acquire)) {
            err = "已取消";
            return false;
        }
        const int n = http.Read(chunk.data(), chunk.size());
        if (n < 0) {
            err = "读取超时或连接中断";
            return false;
        }
        if (n == 0) {
            return true;  // 对端正常结束
        }
        buf.append(chunk.data(), static_cast<size_t>(n));
        size_t start = 0;
        while (true) {
            const size_t nl = buf.find('\n', start);
            if (nl == std::string::npos) {
                break;
            }
            size_t end = nl;
            if (end > start && buf[end - 1] == '\r') {
                --end;
            }
            if (end - start > 5 && buf.compare(start, 5, "data:") == 0) {
                size_t p = start + 5;
                while (p < end && buf[p] == ' ') {
                    ++p;
                }
                if (end - p == 6 && buf.compare(p, 6, "[DONE]") == 0) {
                    return true;
                }
                buf[end] = '\0';
                if (!on_data(buf.c_str() + p)) {
                    return true;
                }
            }
            start = nl + 1;
        }
        buf.erase(0, start);
    }
}

void AppendWavHeader(std::string& out, uint32_t pcm_bytes, uint32_t sample_rate) {
    auto u32 = [&out](uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
    };
    auto u16 = [&out](uint16_t v) {
        out.push_back(static_cast<char>(v & 0xff));
        out.push_back(static_cast<char>(v >> 8));
    };
    out.append("RIFF");
    u32(36 + pcm_bytes);
    out.append("WAVEfmt ");
    u32(16);
    u16(1);  // PCM
    u16(1);  // mono
    u32(sample_rate);
    u32(sample_rate * 2);
    u16(2);
    u16(16);
    out.append("data");
    u32(pcm_bytes);
}

std::string Base64(const uint8_t* data, size_t len) {
    size_t olen = 0;
    mbedtls_base64_encode(nullptr, 0, &olen, data, len);
    std::string out(olen, '\0');
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(out.data()), out.size(), &olen, data,
                              len) != 0) {
        return {};
    }
    out.resize(olen);  // olen 不含结尾 0
    return out;
}

const cJSON* FirstDelta(const cJSON* root) {
    const cJSON* choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    if (!cJSON_IsArray(choices)) {
        return nullptr;
    }
    const cJSON* c0 = cJSON_GetArrayItem(choices, 0);
    if (!cJSON_IsObject(c0)) {
        return nullptr;
    }
    return cJSON_GetObjectItemCaseSensitive(c0, "delta");
}

}  // namespace

bool Asr(const Config& cfg, const std::vector<int16_t>& pcm16k, std::string& text, std::string& err,
         const std::atomic<bool>& cancel) {
    const int64_t t0 = esp_timer_get_time();
    const uint32_t pcm_bytes = static_cast<uint32_t>(pcm16k.size() * sizeof(int16_t));
    std::string wav;
    wav.reserve(44 + pcm_bytes);
    AppendWavHeader(wav, pcm_bytes, 16000);
    wav.append(reinterpret_cast<const char*>(pcm16k.data()), pcm_bytes);
    std::string b64 = Base64(reinterpret_cast<const uint8_t*>(wav.data()), wav.size());
    wav.clear();
    wav.shrink_to_fit();
    if (b64.empty()) {
        err = "音频编码失败";
        return false;
    }

    std::string body;
    body.reserve(b64.size() + 512);
    body += "{\"model\":\"";
    body += cfg.asr_model;
    body += "\",\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"input_audio\","
            "\"input_audio\":{\"data\":\"data:audio/wav;base64,";
    body += b64;
    body += "\"}}]}],\"asr_options\":{\"language\":\"";
    body += cfg.asr_language;
    body += "\"}}";
    b64.clear();
    b64.shrink_to_fit();

    if (cancel.load()) {
        err = "已取消";
        return false;
    }
    auto http = NewHttp();
    if (!http) {
        err = "无网络";
        return false;
    }
    ApplyAuth(*http, cfg.mimo_key, false);
    if (!OpenPost(*http, cfg.asr_url, std::move(body), err)) {
        return false;
    }
    const std::string resp = http->ReadAll();
    http->Close();

    JsonPtr root(cJSON_Parse(resp.c_str()), cJSON_Delete);
    const cJSON* choices = root ? cJSON_GetObjectItemCaseSensitive(root.get(), "choices") : nullptr;
    const cJSON* c0 = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : nullptr;
    const cJSON* msg = c0 ? cJSON_GetObjectItemCaseSensitive(c0, "message") : nullptr;
    const cJSON* content = msg ? cJSON_GetObjectItemCaseSensitive(msg, "content") : nullptr;
    if (!cJSON_IsString(content) || content->valuestring == nullptr) {
        err = "识别结果解析失败";
        ESP_LOGW(TAG, "asr resp: %.200s", resp.c_str());
        return false;
    }
    text = content->valuestring;
    ESP_LOGI(TAG, "ASR %.1fs audio -> %d ms: %s", pcm16k.size() / 16000.0,
             static_cast<int>((esp_timer_get_time() - t0) / 1000), text.c_str());
    return true;
}

// ---------------------------------------------------------------- 智谱联网搜索 MCP（streamable HTTP）

namespace {

struct McpSession {
    std::string url;
    std::string key;
    std::string session_id;
    std::string tool = "webSearchPrime";
    std::string query_arg;  // 工具入参名，tools/list 后确定
    int next_id = 1;
};

std::mutex& McpMu() {
    static std::mutex mu;
    return mu;
}

McpSession& Mcp() {
    static McpSession s;
    return s;
}

// MCP 回包可能是纯 JSON，也可能是 SSE（data: 行）；取第一个带 result 或 error 的 JSON
JsonPtr ParseMcpBody(const std::string& body) {
    JsonPtr root(cJSON_Parse(body.c_str()), cJSON_Delete);
    if (root) return root;
    size_t pos = 0;
    while (pos < body.size()) {
        size_t nl = body.find('\n', pos);
        if (nl == std::string::npos) nl = body.size();
        if (body.compare(pos, 5, "data:") == 0) {
            size_t p = pos + 5;
            while (p < nl && body[p] == ' ') ++p;
            std::string line = body.substr(p, nl - p);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            JsonPtr j(cJSON_Parse(line.c_str()), cJSON_Delete);
            if (j && (cJSON_GetObjectItemCaseSensitive(j.get(), "result") ||
                      cJSON_GetObjectItemCaseSensitive(j.get(), "error"))) {
                return j;
            }
        }
        pos = nl + 1;
    }
    return JsonPtr(nullptr, cJSON_Delete);
}

// 发一条 JSON-RPC；id<0 表示通知（不等结果）
bool McpPost(McpSession& m, const char* method, cJSON* params, int id, JsonPtr& out, std::string& err) {
    JsonPtr req(cJSON_CreateObject(), cJSON_Delete);
    cJSON_AddStringToObject(req.get(), "jsonrpc", "2.0");
    if (id >= 0) cJSON_AddNumberToObject(req.get(), "id", id);
    cJSON_AddStringToObject(req.get(), "method", method);
    if (params) cJSON_AddItemToObject(req.get(), "params", params);
    char* raw = cJSON_PrintUnformatted(req.get());
    if (!raw) {
        err = "MCP 请求构造失败";
        return false;
    }
    std::string body(raw);
    cJSON_free(raw);

    auto http = NewHttp();
    if (!http) {
        err = "无网络";
        return false;
    }
    http->SetHeader("Authorization", "Bearer " + m.key);
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("Accept", "application/json, text/event-stream");
    if (!m.session_id.empty()) http->SetHeader("Mcp-Session-Id", m.session_id);
    http->SetContent(std::move(body));
    if (!http->Open("POST", m.url)) {
        err = "搜索服务连接失败";
        return false;
    }
    const int status = http->GetStatusCode();
    const std::string sid = http->GetResponseHeader("Mcp-Session-Id");
    if (!sid.empty()) m.session_id = sid;
    const std::string resp = http->ReadAll();
    http->Close();
    if (status == 404 || status == 400) {
        m.session_id.clear();  // 会话过期：调用方重建
    }
    if (status < 200 || status >= 300) {
        err = "搜索服务 " + ExtractError(status, resp);
        ESP_LOGW(TAG, "mcp %s -> %d %.200s", method, status, resp.c_str());
        return false;
    }
    if (id < 0) return true;
    out = ParseMcpBody(resp);
    if (!out) {
        err = "搜索结果解析失败";
        ESP_LOGW(TAG, "mcp %s resp: %.200s", method, resp.c_str());
        return false;
    }
    const cJSON* e = cJSON_GetObjectItemCaseSensitive(out.get(), "error");
    if (e) {
        const cJSON* msg = cJSON_GetObjectItemCaseSensitive(e, "message");
        err = std::string("搜索服务错误：") + (cJSON_IsString(msg) ? msg->valuestring : "未知");
        return false;
    }
    return true;
}

bool McpEnsureSession(McpSession& m, std::string& err) {
    if (!m.session_id.empty() && !m.query_arg.empty()) return true;
    m.session_id.clear();
    cJSON* params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "protocolVersion", "2025-03-26");
    cJSON_AddObjectToObject(params, "capabilities");
    cJSON* ci = cJSON_AddObjectToObject(params, "clientInfo");
    cJSON_AddStringToObject(ci, "name", "metalio-e-ink4");
    cJSON_AddStringToObject(ci, "version", "1.0");
    JsonPtr out(nullptr, cJSON_Delete);
    if (!McpPost(m, "initialize", params, m.next_id++, out, err)) return false;
    JsonPtr dummy(nullptr, cJSON_Delete);
    McpPost(m, "notifications/initialized", nullptr, -1, dummy, err);  // 失败不致命
    err.clear();

    // 找搜索工具及其字符串入参名
    if (!McpPost(m, "tools/list", cJSON_CreateObject(), m.next_id++, out, err)) return false;
    const cJSON* res = cJSON_GetObjectItemCaseSensitive(out.get(), "result");
    const cJSON* tools = res ? cJSON_GetObjectItemCaseSensitive(res, "tools") : nullptr;
    const cJSON* t = nullptr;
    cJSON_ArrayForEach(t, tools) {
        const cJSON* name = cJSON_GetObjectItemCaseSensitive(t, "name");
        if (!cJSON_IsString(name)) continue;
        if (std::strstr(name->valuestring, "earch") == nullptr) continue;
        m.tool = name->valuestring;
        const cJSON* schema = cJSON_GetObjectItemCaseSensitive(t, "inputSchema");
        const cJSON* req = schema ? cJSON_GetObjectItemCaseSensitive(schema, "required") : nullptr;
        const cJSON* props = schema ? cJSON_GetObjectItemCaseSensitive(schema, "properties") : nullptr;
        const cJSON* r0 = cJSON_IsArray(req) ? cJSON_GetArrayItem(req, 0) : nullptr;
        if (cJSON_IsString(r0)) {
            m.query_arg = r0->valuestring;
        } else if (props && props->child && props->child->string) {
            m.query_arg = props->child->string;
        }
        break;
    }
    if (m.query_arg.empty()) m.query_arg = "search_query";
    ESP_LOGI(TAG, "mcp session ok tool=%s arg=%s", m.tool.c_str(), m.query_arg.c_str());
    return true;
}

// 返回给模型的搜索结果文本；refs = 结果条数
bool McpSearch(const Config& cfg, const std::string& query, std::string& result, int& refs, std::string& err) {
    std::lock_guard<std::mutex> lock(McpMu());
    McpSession& m = Mcp();
    if (m.url != cfg.glm_search_url || m.key != cfg.glm_key) {
        m = McpSession{};
        m.url = cfg.glm_search_url;
        m.key = cfg.glm_key;
    }
    const int64_t t0 = esp_timer_get_time();
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!McpEnsureSession(m, err)) {
            if (attempt == 0) continue;
            return false;
        }
        cJSON* params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "name", m.tool.c_str());
        cJSON* args = cJSON_AddObjectToObject(params, "arguments");
        cJSON_AddStringToObject(args, m.query_arg.c_str(), query.c_str());
        JsonPtr out(nullptr, cJSON_Delete);
        if (!McpPost(m, "tools/call", params, m.next_id++, out, err)) {
            if (m.session_id.empty() && attempt == 0) continue;  // 会话失效，重建再试一次
            return false;
        }
        const cJSON* res = cJSON_GetObjectItemCaseSensitive(out.get(), "result");
        const cJSON* content = res ? cJSON_GetObjectItemCaseSensitive(res, "content") : nullptr;
        result.clear();
        const cJSON* c = nullptr;
        cJSON_ArrayForEach(c, content) {
            const cJSON* text = cJSON_GetObjectItemCaseSensitive(c, "text");
            if (cJSON_IsString(text)) result += text->valuestring;
        }
        // 结果多为 JSON 数组（或被再包一层字符串）；数一下条数
        refs = 0;
        JsonPtr parsed(cJSON_Parse(result.c_str()), cJSON_Delete);
        if (parsed && cJSON_IsString(parsed.get())) {
            JsonPtr inner(cJSON_Parse(parsed->valuestring), cJSON_Delete);
            if (inner && cJSON_IsArray(inner.get())) refs = cJSON_GetArraySize(inner.get());
        } else if (parsed && cJSON_IsArray(parsed.get())) {
            refs = cJSON_GetArraySize(parsed.get());
        }
        if (refs == 0 && !result.empty()) refs = 1;
        // 交给模型的内容控制在 ~8KB，防止上下文和内存膨胀
        constexpr size_t kMax = 8 * 1024;
        if (result.size() > kMax) {
            size_t cut = kMax;
            while (cut > 0 && (static_cast<unsigned char>(result[cut]) & 0xC0) == 0x80) --cut;
            result.resize(cut);
            result += "…（已截断）";
        }
        ESP_LOGI(TAG, "mcp search \"%s\" -> %d results %uB in %d ms", query.c_str(), refs,
                 static_cast<unsigned>(result.size()), static_cast<int>((esp_timer_get_time() - t0) / 1000));
        if (result.empty()) result = "（没有搜索到结果）";
        return true;
    }
    return false;
}

struct ToolCall {
    std::string id;
    std::string name;
    std::string args;
};

std::string QueryFromArgs(const std::string& args) {
    JsonPtr j(cJSON_Parse(args.c_str()), cJSON_Delete);
    if (j) {
        const cJSON* q = cJSON_GetObjectItemCaseSensitive(j.get(), "query");
        if (cJSON_IsString(q) && q->valuestring) return q->valuestring;
        for (const cJSON* it = j->child; it; it = it->next) {
            if (cJSON_IsString(it)) return it->valuestring;
        }
    }
    return args;
}

}  // namespace

bool ChatStream(const Config& cfg, const ChatEndpoint& ep, const std::vector<ChatMessage>& messages,
                const ChatCallbacks& cb, std::string& err, const std::atomic<bool>& cancel) {
    const int64_t t0 = esp_timer_get_time();
    // GLM 联网：函数调用 + 智谱搜索 MCP；MiMo 联网：平台内置 web_search 工具
    const bool glm_tools = ep.web_search && ep.vendor == "glm" && !cfg.glm_search_url.empty();
    const bool mimo_tools = ep.web_search && ep.vendor == "mimo";

    JsonPtr msgs(cJSON_CreateArray(), cJSON_Delete);
    for (const auto& m : messages) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "role", m.role.c_str());
        if (m.image.empty()) {
            cJSON_AddStringToObject(o, "content", m.content.c_str());
        } else {
            cJSON* parts = cJSON_AddArrayToObject(o, "content");
            cJSON* img = cJSON_CreateObject();
            cJSON_AddStringToObject(img, "type", "image_url");
            cJSON* iu = cJSON_AddObjectToObject(img, "image_url");
            cJSON_AddStringToObject(iu, "url", m.image.c_str());
            cJSON_AddItemToArray(parts, img);
            cJSON* txt = cJSON_CreateObject();
            cJSON_AddStringToObject(txt, "type", "text");
            cJSON_AddStringToObject(txt, "text", m.content.c_str());
            cJSON_AddItemToArray(parts, txt);
        }
        cJSON_AddItemToArray(msgs.get(), o);
    }

    size_t reasoning_bytes = 0;
    int web_refs = 0;
    int64_t first_reason_us = 0, first_content_us = 0;
    std::string finish;
    bool ok = false;
    constexpr int kMaxToolRounds = 3;

    for (int round = 0; round <= kMaxToolRounds; ++round) {
        JsonPtr req(cJSON_CreateObject(), cJSON_Delete);
        cJSON_AddStringToObject(req.get(), "model", ep.model.c_str());
        cJSON_AddItemReferenceToObject(req.get(), "messages", msgs.get());
        cJSON_AddBoolToObject(req.get(), "stream", true);
        cJSON* thinking = cJSON_AddObjectToObject(req.get(), "thinking");
        cJSON_AddStringToObject(thinking, "type", ep.thinking ? "enabled" : "disabled");
        cJSON_AddNumberToObject(req.get(), ep.max_tokens_field.c_str(), cfg.max_tokens);
        if (mimo_tools) {
            cJSON* tools = cJSON_AddArrayToObject(req.get(), "tools");
            cJSON* ws = cJSON_CreateObject();
            cJSON_AddStringToObject(ws, "type", "web_search");
            cJSON_AddNumberToObject(ws, "max_keyword", cfg.web_max_keyword);
            cJSON_AddBoolToObject(ws, "force_search", false);
            cJSON_AddNumberToObject(ws, "limit", 1);
            cJSON_AddItemToArray(tools, ws);
            cJSON_AddStringToObject(req.get(), "tool_choice", "auto");
        } else if (glm_tools && round < kMaxToolRounds) {  // 最后一轮不给工具，逼它作答
            cJSON* tools = cJSON_AddArrayToObject(req.get(), "tools");
            cJSON* t = cJSON_CreateObject();
            cJSON_AddStringToObject(t, "type", "function");
            cJSON* fn = cJSON_AddObjectToObject(t, "function");
            cJSON_AddStringToObject(fn, "name", "web_search");
            cJSON_AddStringToObject(fn, "description",
                                    "搜索互联网获取实时或最新信息（新闻、天气、价格、近期事件等）。"
                                    "仅在需要时调用，query 用简洁的中文关键词。");
            cJSON* params = cJSON_AddObjectToObject(fn, "parameters");
            cJSON_AddStringToObject(params, "type", "object");
            cJSON* props = cJSON_AddObjectToObject(params, "properties");
            cJSON* q = cJSON_AddObjectToObject(props, "query");
            cJSON_AddStringToObject(q, "type", "string");
            cJSON_AddStringToObject(q, "description", "搜索关键词");
            cJSON* reqd = cJSON_AddArrayToObject(params, "required");
            cJSON_AddItemToArray(reqd, cJSON_CreateString("query"));
            cJSON_AddItemToArray(tools, t);
            cJSON_AddStringToObject(req.get(), "tool_choice", "auto");
        }
        char* raw = cJSON_PrintUnformatted(req.get());
        req.reset();  // 引用不会释放 msgs
        if (raw == nullptr) {
            err = "请求构造失败";
            return false;
        }
        std::string body(raw);
        cJSON_free(raw);

        auto http = NewHttp();
        if (!http) {
            err = "无网络";
            return false;
        }
        ApplyAuth(*http, ep.key, ep.bearer);
        http->SetHeader("Accept", "text/event-stream");
        if (!OpenPost(*http, ep.url, std::move(body), err)) {
            return false;
        }

        std::string content_all, reasoning_all;
        std::vector<ToolCall> calls;
        finish.clear();
        ok = ReadSse(*http, cancel, [&](const char* json) {
            JsonPtr ev(cJSON_Parse(json), cJSON_Delete);
            if (!ev) return true;
            const cJSON* delta = FirstDelta(ev.get());
            if (cJSON_IsObject(delta)) {
                const cJSON* ann = cJSON_GetObjectItemCaseSensitive(delta, "annotations");
                if (cJSON_IsArray(ann) && cJSON_GetArraySize(ann) > 0) {
                    web_refs += cJSON_GetArraySize(ann);
                    if (cb.on_web_refs) cb.on_web_refs(web_refs);
                }
                const cJSON* r = cJSON_GetObjectItemCaseSensitive(delta, "reasoning_content");
                if (cJSON_IsString(r) && r->valuestring != nullptr && r->valuestring[0] != '\0') {
                    if (first_reason_us == 0) first_reason_us = esp_timer_get_time();
                    reasoning_bytes += std::strlen(r->valuestring);
                    if (glm_tools) reasoning_all += r->valuestring;
                    if (cb.on_reasoning) cb.on_reasoning(reasoning_bytes);
                }
                const cJSON* c = cJSON_GetObjectItemCaseSensitive(delta, "content");
                if (cJSON_IsString(c) && c->valuestring != nullptr && c->valuestring[0] != '\0') {
                    if (first_content_us == 0) first_content_us = esp_timer_get_time();
                    content_all += c->valuestring;
                    if (cb.on_content) cb.on_content(c->valuestring);
                }
                // 流式 tool_calls：按 index 累积 id / name / arguments 片段
                const cJSON* tcs = cJSON_GetObjectItemCaseSensitive(delta, "tool_calls");
                const cJSON* tc = nullptr;
                cJSON_ArrayForEach(tc, tcs) {
                    const cJSON* idx = cJSON_GetObjectItemCaseSensitive(tc, "index");
                    const size_t i = cJSON_IsNumber(idx) ? static_cast<size_t>(idx->valueint) : calls.size();
                    if (i >= 8) continue;
                    if (calls.size() <= i) calls.resize(i + 1);
                    const cJSON* id = cJSON_GetObjectItemCaseSensitive(tc, "id");
                    if (cJSON_IsString(id) && id->valuestring[0]) calls[i].id = id->valuestring;
                    const cJSON* fn = cJSON_GetObjectItemCaseSensitive(tc, "function");
                    const cJSON* nm = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : nullptr;
                    const cJSON* ag = fn ? cJSON_GetObjectItemCaseSensitive(fn, "arguments") : nullptr;
                    if (cJSON_IsString(nm) && nm->valuestring[0]) calls[i].name = nm->valuestring;
                    if (cJSON_IsString(ag)) calls[i].args += ag->valuestring;
                }
            }
            const cJSON* choices = cJSON_GetObjectItemCaseSensitive(ev.get(), "choices");
            const cJSON* c0 = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : nullptr;
            const cJSON* fr = c0 ? cJSON_GetObjectItemCaseSensitive(c0, "finish_reason") : nullptr;
            if (cJSON_IsString(fr) && fr->valuestring != nullptr) finish = fr->valuestring;
            return !cancel.load(std::memory_order_acquire);
        }, err);
        http->Close();
        if (!ok || cancel.load()) break;

        calls.erase(std::remove_if(calls.begin(), calls.end(),
                                   [](const ToolCall& c) { return c.name.empty(); }),
                    calls.end());
        if (!glm_tools || calls.empty()) break;  // 正常作答结束

        // 回传本轮 assistant（含完整思考内容，GLM 交错思考 + 工具调用必须原样带回）
        cJSON* am = cJSON_CreateObject();
        cJSON_AddStringToObject(am, "role", "assistant");
        cJSON_AddStringToObject(am, "content", content_all.c_str());
        if (!reasoning_all.empty()) cJSON_AddStringToObject(am, "reasoning_content", reasoning_all.c_str());
        cJSON* tca = cJSON_AddArrayToObject(am, "tool_calls");
        for (size_t i = 0; i < calls.size(); ++i) {
            if (calls[i].id.empty()) calls[i].id = "call_" + std::to_string(round) + "_" + std::to_string(i);
            cJSON* o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "id", calls[i].id.c_str());
            cJSON_AddStringToObject(o, "type", "function");
            cJSON* fn = cJSON_AddObjectToObject(o, "function");
            cJSON_AddStringToObject(fn, "name", calls[i].name.c_str());
            cJSON_AddStringToObject(fn, "arguments", calls[i].args.c_str());
            cJSON_AddItemToArray(tca, o);
        }
        cJSON_AddItemToArray(msgs.get(), am);

        for (const auto& c : calls) {
            std::string result;
            if (c.name == "web_search") {
                const std::string query = QueryFromArgs(c.args);
                if (cb.on_searching) cb.on_searching(query);
                int refs = 0;
                std::string serr;
                if (McpSearch(cfg, query, result, refs, serr)) {
                    web_refs += refs;
                    if (cb.on_web_refs) cb.on_web_refs(web_refs);
                } else {
                    ESP_LOGW(TAG, "search failed: %s", serr.c_str());
                    result = "搜索失败：" + serr + "。请根据已有知识回答，并说明无法联网确认。";
                }
            } else {
                result = "未知工具";
            }
            cJSON* tm = cJSON_CreateObject();
            cJSON_AddStringToObject(tm, "role", "tool");
            cJSON_AddStringToObject(tm, "tool_call_id", c.id.c_str());
            cJSON_AddStringToObject(tm, "content", result.c_str());
            cJSON_AddItemToArray(msgs.get(), tm);
            if (cancel.load()) break;
        }
        if (cancel.load()) break;
        ESP_LOGI(TAG, "tool round %d done, continue", round + 1);
    }

    auto ms = [t0](int64_t t) { return t ? static_cast<int>((t - t0) / 1000) : -1; };
    ESP_LOGI(TAG, "chat %s/%s think=%d web=%d refs=%d ok=%d think_first=%dms think=%uB content_first=%dms total=%dms finish=%s",
             ep.vendor.c_str(), ep.model.c_str(), ep.thinking ? 1 : 0, ep.web_search ? 1 : 0, web_refs,
             ok ? 1 : 0, ms(first_reason_us),
             static_cast<unsigned>(reasoning_bytes), ms(first_content_us),
             ms(esp_timer_get_time()), finish.c_str());
    if (ok && cancel.load()) {
        err = "已取消";
        return false;
    }
    if (ok && first_content_us == 0 && finish == "length") {
        err = "思考过长被截断，可调大 max_tokens";
        return false;
    }
    return ok;
}

bool TtsStream(const Config& cfg, const std::string& text,
               const std::function<bool(std::vector<int16_t>&& pcm24k)>& on_pcm, std::string& err,
               const std::atomic<bool>& cancel) {
    const int64_t t0 = esp_timer_get_time();
    JsonPtr req(cJSON_CreateObject(), cJSON_Delete);
    cJSON_AddStringToObject(req.get(), "model", cfg.tts_model.c_str());
    cJSON* arr = cJSON_AddArrayToObject(req.get(), "messages");
    cJSON* m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "assistant");
    cJSON_AddStringToObject(m, "content", text.c_str());
    cJSON_AddItemToArray(arr, m);
    cJSON* audio = cJSON_AddObjectToObject(req.get(), "audio");
    cJSON_AddStringToObject(audio, "format", "pcm16");
    cJSON_AddStringToObject(audio, "voice", cfg.tts_voice.c_str());
    cJSON_AddBoolToObject(req.get(), "stream", true);
    char* raw = cJSON_PrintUnformatted(req.get());
    if (raw == nullptr) {
        err = "请求构造失败";
        return false;
    }
    std::string body(raw);
    cJSON_free(raw);
    req.reset();

    auto http = NewHttp();
    if (!http) {
        err = "无网络";
        return false;
    }
    ApplyAuth(*http, cfg.mimo_key, false);
    http->SetHeader("Accept", "text/event-stream");
    if (!OpenPost(*http, cfg.tts_url, std::move(body), err)) {
        return false;
    }

    int64_t first_us = 0;
    size_t total = 0;
    uint8_t carry = 0;
    bool has_carry = false;  // 奇数字节跨包时保留一字节
    bool aborted = false;
    const bool ok = ReadSse(*http, cancel, [&](const char* json) {
        JsonPtr ev(cJSON_Parse(json), cJSON_Delete);
        if (!ev) {
            return true;
        }
        const cJSON* delta = FirstDelta(ev.get());
        const cJSON* a = cJSON_IsObject(delta) ? cJSON_GetObjectItemCaseSensitive(delta, "audio") : nullptr;
        const cJSON* d = cJSON_IsObject(a) ? cJSON_GetObjectItemCaseSensitive(a, "data") : nullptr;
        if (!cJSON_IsString(d) || d->valuestring == nullptr || d->valuestring[0] == '\0') {
            return true;
        }
        const size_t in_len = std::strlen(d->valuestring);
        std::vector<uint8_t> bytes(in_len / 4 * 3 + 4 + 1);
        size_t olen = 0;
        size_t off = 0;
        if (has_carry) {
            bytes[0] = carry;
            off = 1;
        }
        if (mbedtls_base64_decode(bytes.data() + off, bytes.size() - off, &olen,
                                  reinterpret_cast<const unsigned char*>(d->valuestring), in_len) != 0) {
            return true;
        }
        olen += off;
        has_carry = (olen % 2) != 0;
        if (has_carry) {
            carry = bytes[olen - 1];
            --olen;
        }
        if (olen == 0) {
            return true;
        }
        if (first_us == 0) first_us = esp_timer_get_time();
        total += olen;
        std::vector<int16_t> pcm(olen / 2);
        std::memcpy(pcm.data(), bytes.data(), olen);
        if (!on_pcm(std::move(pcm))) {
            aborted = true;
            return false;
        }
        return true;
    }, err);
    http->Close();
    ESP_LOGI(TAG, "tts %u chars first=%dms audio=%.1fs total=%dms ok=%d", static_cast<unsigned>(text.size()),
             first_us ? static_cast<int>((first_us - t0) / 1000) : -1, total / 2.0 / kTtsSampleRate,
             static_cast<int>((esp_timer_get_time() - t0) / 1000), ok ? 1 : 0);
    if (aborted) {
        err = "已取消";
        return false;
    }
    return ok;
}

}  // namespace my_ai
