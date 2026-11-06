#include "translate_screen.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>

#include "application.h"
#include "board.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "lv_adapter_display.h"
#include "power_policy.h"
#include "screen_common.h"
#include "sd_paths.h"
#include "vk_key_handler.h"
#include "web_socket.h"

namespace {

constexpr const char* TAG = "Translate";
constexpr const char* kScreenName = "translate";
constexpr int kPad = 16;
constexpr int kSampleRate = 16000;
constexpr int kChunkSamples = kSampleRate / 10;    // 每 100ms 发一包
constexpr int kIdleStopSec = 90;                   // 这么久没识别到话就自动停（省钱）
constexpr int kMaxSegments = 40;
constexpr int kUiMinGapMs = 700;                   // 墨水屏刷新节流

struct Lang {
    const char* code;
    const char* name;    // 按钮上的字
    uint32_t probe;      // 用来检查字体里有没有这种文字
};
const Lang kLangs[] = {
    {"zh", "中文", 0x4E2D},
    {"en", "英文", 'A'},
    {"de", "德文", 0x00E4},   // ä
    {"ja", "日文", 0x3042},   // あ
    {"ko", "韩文", 0xD55C},   // 한
};
constexpr int kLangCount = sizeof(kLangs) / sizeof(kLangs[0]);

struct Segment {
    std::string src_id;
    std::string src;
    std::string dst;
    std::string lang;
    int speaker = -1;
};

enum class Phase { Idle, Connecting, Running, Stopping };

struct State {
    std::mutex mu;
    std::deque<Segment> segs;
    std::map<std::string, std::string> dst2src;  // 译文 item_id -> 原文 item_id
    Phase phase = Phase::Idle;
    std::string hint;
    int target = 1;              // 默认译成英文
    int64_t start_us = 0;
    int64_t last_speech_us = 0;
    int64_t total_audio_ms = 0;  // 本次已发送音频时长（估算费用用）
    bool dirty = false;
    bool rebuild = false;
};
State& S() {
    static State s;
    return s;
}

std::atomic<bool> s_active{false};
std::atomic<bool> s_stop_req{false};
std::atomic<bool> s_task_running{false};

// 采集缓冲：AFE 线程往里塞，发送任务每 100ms 取走
std::mutex s_pcm_mu;
std::vector<int16_t> s_pcm;

struct Ui {
    lv_obj_t* scr = nullptr;
    lv_obj_t* subs = nullptr;
    lv_obj_t* empty = nullptr;
    lv_obj_t* btn = nullptr;
    lv_obj_t* btn_lbl = nullptr;
    lv_obj_t* status = nullptr;
    lv_obj_t* lang_btns[kLangCount] = {};
    bool lang_ok[kLangCount] = {};
    const lv_font_t* f = nullptr;
    const lv_font_t* fb = nullptr;
    const lv_font_t* ft = nullptr;
    int top = 0;
    lv_timer_t* timer = nullptr;
    int64_t last_ui_us = 0;
    std::vector<std::pair<lv_obj_t*, lv_obj_t*>> rows;  // 每段的 (原文, 译文) label
} s_ui;

// ---------------------------------------------------------------- 配置

struct Cfg {
    std::string key, url, model;
    bool ok() const { return !key.empty() && !url.empty(); }
};

std::string ReadFile(const char* path) {
    std::string out;
    FILE* f = std::fopen(path, "rb");
    if (!f) return out;
    char buf[512];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && out.size() < 32768) out.append(buf, n);
    std::fclose(f);
    return out;
}

std::string JStr(const cJSON* o, const char* k) {
    const cJSON* v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : nullptr;
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

Cfg LoadCfg() {
    Cfg c;
    cJSON* r = cJSON_Parse(ReadFile(SD_APP_ROOT "/my_ai.json").c_str());
    if (!r) return c;
    c.key = JStr(r, "dashscope_api_key");
    c.model = JStr(r, "translate_model");
    if (c.model.empty()) c.model = "qwen3.8-livetranslate-flash-realtime";
    c.url = JStr(r, "dashscope_ws_url");
    const std::string ws = JStr(r, "dashscope_workspace");
    if (c.url.empty() && !ws.empty()) c.url = "wss://" + ws + ".cn-beijing.maas.aliyuncs.com/api-ws/v1/realtime";
    if (c.url.empty()) c.url = "wss://dashscope.aliyuncs.com/api-ws/v1/realtime";
    if (c.url.find("model=") == std::string::npos) {
        c.url += (c.url.find('?') == std::string::npos ? "?model=" : "&model=") + c.model;
    }
    cJSON_Delete(r);
    return c;
}

// ---------------------------------------------------------------- 状态更新

void PostUi();

void SetHint(const std::string& h) {
    {
        std::lock_guard<std::mutex> lock(S().mu);
        S().hint = h;
        S().dirty = true;
    }
    PostUi();
}

void SetPhase(Phase p) {
    std::lock_guard<std::mutex> lock(S().mu);
    S().phase = p;
    S().dirty = true;
}

Segment* FindSegLocked(const std::string& src_id) {
    for (auto it = S().segs.rbegin(); it != S().segs.rend(); ++it) {
        if (it->src_id == src_id) return &*it;
    }
    return nullptr;
}

Segment& EnsureSegLocked(const std::string& src_id) {
    if (Segment* s = FindSegLocked(src_id)) return *s;
    S().segs.push_back(Segment{});
    S().segs.back().src_id = src_id;
    while (S().segs.size() > kMaxSegments) S().segs.pop_front();
    S().rebuild = true;
    return S().segs.back();
}

/** 服务端事件 */
void HandleEvent(const char* data, size_t len) {
    cJSON* ev = cJSON_ParseWithLength(data, len);
    if (!ev) return;
    const std::string type = JStr(ev, "type");
    bool changed = true;
    {
        std::lock_guard<std::mutex> lock(S().mu);
        if (type == "input_audio_buffer.speech_started") {
            Segment& s = EnsureSegLocked(JStr(ev, "item_id"));
            const cJSON* sp = cJSON_GetObjectItemCaseSensitive(ev, "speaker_id");
            if (cJSON_IsNumber(sp)) s.speaker = sp->valueint;
            S().last_speech_us = esp_timer_get_time();
        } else if (type == "conversation.item.input_audio_transcription.delta" ||
                   type == "conversation.item.input_audio_transcription.text") {
            Segment& s = EnsureSegLocked(JStr(ev, "item_id"));
            if (type == "conversation.item.input_audio_transcription.text") {
                s.src = JStr(ev, "text") + JStr(ev, "stash");
            } else {
                s.src += JStr(ev, "delta");
            }
            S().last_speech_us = esp_timer_get_time();
        } else if (type == "conversation.item.input_audio_transcription.completed") {
            Segment& s = EnsureSegLocked(JStr(ev, "item_id"));
            const std::string t = JStr(ev, "transcript");
            if (!t.empty()) s.src = t;
            s.lang = JStr(ev, "language");
        } else if (type == "conversation.item.created") {
            const cJSON* item = cJSON_GetObjectItemCaseSensitive(ev, "item");
            const std::string prev = JStr(ev, "previous_item_id");
            if (item && JStr(item, "role") == "assistant" && !prev.empty()) S().dst2src[JStr(item, "id")] = prev;
            changed = false;
        } else if (type == "response.text.delta" || type == "response.audio_transcript.delta" ||
                   type == "response.text.done" || type == "response.audio_transcript.done") {
            const std::string id = JStr(ev, "item_id");
            auto it = S().dst2src.find(id);
            Segment* s = nullptr;
            if (it != S().dst2src.end()) s = FindSegLocked(it->second);
            if (!s && !S().segs.empty()) s = &S().segs.back();  // 没对上就挂到最新一段
            if (!s) s = &EnsureSegLocked(id);
            if (type == "response.text.done") s->dst = JStr(ev, "text");
            else if (type == "response.audio_transcript.done") s->dst = JStr(ev, "transcript");
            else s->dst += JStr(ev, "delta");
            S().last_speech_us = esp_timer_get_time();
        } else if (type == "error") {
            const cJSON* e = cJSON_GetObjectItemCaseSensitive(ev, "error");
            S().hint = "出错：" + JStr(e, "message");
            ESP_LOGW(TAG, "server error %s: %s", JStr(e, "code").c_str(), JStr(e, "message").c_str());
        } else {
            changed = false;
        }
        if (changed) S().dirty = true;
    }
    if (type == "session.created" || type == "session.updated" || type == "session.finished" || type == "error") {
        ESP_LOGI(TAG, "event %s", type.c_str());
    }
    cJSON_Delete(ev);
    if (changed) PostUi();
}

// ---------------------------------------------------------------- 会话任务

std::string MakeAppend(const int16_t* pcm, size_t n) {
    const size_t bytes = n * sizeof(int16_t);
    size_t olen = 0;
    std::string b64((bytes + 2) / 3 * 4 + 4, '\0');
    mbedtls_base64_encode(reinterpret_cast<unsigned char*>(&b64[0]), b64.size(), &olen,
                          reinterpret_cast<const unsigned char*>(pcm), bytes);
    b64.resize(olen);
    std::string msg = "{\"type\":\"input_audio_buffer.append\",\"audio\":\"";
    msg += b64;
    msg += "\"}";
    return msg;
}

void StartCaptureOnMain() {
    auto& app = Application::GetInstance();
    app.EnsureAudioServiceRunning();
    auto& audio = app.GetAudioService();
    if (!audio.IsStarted()) {
        SetHint("音频服务启动失败");
        s_stop_req.store(true);
        return;
    }
    audio.ResetDecoder();
    {
        std::lock_guard<std::mutex> lock(s_pcm_mu);
        s_pcm.clear();
    }
    audio.SetPcmCaptureSink([](std::vector<int16_t>&& pcm) {
        std::lock_guard<std::mutex> lock(s_pcm_mu);
        if (s_pcm.size() < static_cast<size_t>(kSampleRate) * 5) s_pcm.insert(s_pcm.end(), pcm.begin(), pcm.end());
    });
    audio.EnableVoiceProcessing(true);
    app.SetDeviceState(kDeviceStateListening);
    ESP_LOGI(TAG, "capture start");
}

void StopCaptureOnMain() {
    auto& app = Application::GetInstance();
    auto& audio = app.GetAudioService();
    audio.EnableVoiceProcessing(false);
    audio.SetPcmCaptureSink(nullptr);
    app.SetDeviceState(kDeviceStateIdle);
    ESP_LOGI(TAG, "capture stop");
}

void SessionTask(void*) {
    PowerPolicy::GetInstance().Acquire(PowerNeed::LocalMic);
    const Cfg cfg = LoadCfg();
    int target = 1;
    {
        std::lock_guard<std::mutex> lock(S().mu);
        target = S().target;
        S().segs.clear();
        S().dst2src.clear();
        S().rebuild = true;
        S().start_us = esp_timer_get_time();
        S().last_speech_us = S().start_us;
        S().total_audio_ms = 0;
    }
    std::unique_ptr<WebSocket> ws;
    bool capturing = false;
    std::atomic<bool> ready{false}, finished{false}, broken{false};

    auto finish = [&](const std::string& why) {
        if (capturing) {
            Application::GetInstance().Schedule(StopCaptureOnMain);
            capturing = false;
        }
        if (!why.empty()) SetHint(why);
    };

    do {
        if (!cfg.ok()) {
            finish("缺少 Key：my_ai.json 加 dashscope_api_key");
            break;
        }
        auto& board = Board::GetInstance();
        if (!board.IsNetworkReady() && !board.EnsureNetworkReady(20000)) {
            finish("网络未连接");
            break;
        }
        SetHint("正在连接翻译服务…");
        ws = board.GetNetwork()->CreateWebSocket(3);
        if (!ws) {
            finish("创建连接失败");
            break;
        }
        ws->SetHeader("Authorization", ("Bearer " + cfg.key).c_str());
        ws->SetReceiveBufferSize(8192);
        ws->OnData([&](const char* data, size_t len, bool binary) {
            if (binary) return;
            // 先看类型，再交给通用处理
            if (len > 16 && std::strstr(std::string(data, std::min<size_t>(len, 120)).c_str(), "session.updated")) {
                ready.store(true);
            }
            if (len > 16 && std::strstr(std::string(data, std::min<size_t>(len, 120)).c_str(), "session.finished")) {
                finished.store(true);
            }
            HandleEvent(data, len);
        });
        ws->OnDisconnected([&]() { broken.store(true); });
        if (!ws->Connect(cfg.url.c_str())) {
            ESP_LOGW(TAG, "connect fail err=%d", ws->GetLastError());
            finish("连不上翻译服务（检查 Key / 网络）");
            break;
        }
        // 只要文字，不要语音（省钱，也不用占喇叭）
        std::string upd = std::string("{\"type\":\"session.update\",\"session\":{\"output_modalities\":[\"text\"],"
                                      "\"modalities\":[\"text\"],\"translation\":{\"language\":\"") +
                          kLangs[target].code + "\"}}}";
        ws->Send(upd);
        for (int i = 0; i < 100 && !ready.load() && !broken.load() && !s_stop_req.load(); ++i) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (!ready.load()) {
            finish(broken.load() ? "翻译服务断开了" : "翻译服务没响应");
            break;
        }
        Application::GetInstance().Schedule(StartCaptureOnMain);
        capturing = true;
        SetPhase(Phase::Running);
        SetHint(std::string("请说话，会译成") + kLangs[target].name);
        ESP_LOGI(TAG, "session ready -> %s", kLangs[target].code);

        // 发送循环
        std::vector<int16_t> chunk;
        while (!s_stop_req.load() && !broken.load()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            {
                std::lock_guard<std::mutex> lock(s_pcm_mu);
                chunk.swap(s_pcm);
                s_pcm.clear();
            }
            if (!chunk.empty()) {
                if (!ws->Send(MakeAppend(chunk.data(), chunk.size()))) {
                    broken.store(true);
                    break;
                }
                std::lock_guard<std::mutex> lock(S().mu);
                S().total_audio_ms += static_cast<int64_t>(chunk.size()) * 1000 / kSampleRate;
                chunk.clear();
            }
            int64_t last;
            {
                std::lock_guard<std::mutex> lock(S().mu);
                last = S().last_speech_us;
            }
            if (esp_timer_get_time() - last > static_cast<int64_t>(kIdleStopSec) * 1000000) {
                SetHint("一直没听到说话，已自动停止");
                break;
            }
        }
        finish("");
        if (!broken.load()) {
            SetPhase(Phase::Stopping);
            ws->Send(std::string("{\"type\":\"session.finish\"}"));
            for (int i = 0; i < 100 && !finished.load() && !broken.load(); ++i) vTaskDelay(pdMS_TO_TICKS(50));
        } else {
            SetHint("连接断开了，点开始重新连");
        }
    } while (false);

    if (ws) ws->Close();
    ws.reset();
    {
        std::lock_guard<std::mutex> lock(S().mu);
        S().phase = Phase::Idle;
        S().dirty = true;
        ESP_LOGI(TAG, "session end, audio %.1fs, %u segments", S().total_audio_ms / 1000.0,
                 static_cast<unsigned>(S().segs.size()));
    }
    PowerPolicy::GetInstance().Release(PowerNeed::LocalMic);
    s_task_running.store(false);
    PostUi();
    vTaskDeleteWithCaps(nullptr);
}

void StartSession() {
    if (s_task_running.exchange(true)) return;
    s_stop_req.store(false);
    SetPhase(Phase::Connecting);
    if (xTaskCreateWithCaps(SessionTask, "translate", 10 * 1024, nullptr, 4, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_task_running.store(false);
        SetPhase(Phase::Idle);
        SetHint("内存不足");
    }
    PostUi();
}

void StopSession() {
    if (!s_task_running.load()) return;
    s_stop_req.store(true);
    SetHint("正在停止…");
}

void Toggle() {
    if (s_task_running.load()) StopSession();
    else StartSession();
}

// ---------------------------------------------------------------- 界面

lv_obj_t* Box(lv_obj_t* p) {
    lv_obj_t* o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t* Label(lv_obj_t* p, const char* t, const lv_font_t* f) {
    lv_obj_t* l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_black(), 0);
    lv_label_set_text(l, t);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

lv_obj_t* Btn(lv_obj_t* p, const char* t, int w, int h, lv_event_cb_t cb, intptr_t ud, lv_obj_t** lbl_out = nullptr) {
    lv_obj_t* b = Box(p);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, lv_color_black(), 0);
    lv_obj_set_style_bg_color(b, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(b);
    lv_obj_t* l = Label(b, t, s_ui.f);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, reinterpret_cast<void*>(ud));
    if (lbl_out) *lbl_out = l;
    return b;
}

void Invert(lv_obj_t* b, bool on) {
    lv_obj_set_style_bg_color(b, on ? lv_color_black() : lv_color_white(), 0);
    if (lv_obj_t* l = lv_obj_get_child(b, 0)) lv_obj_set_style_text_color(l, on ? lv_color_white() : lv_color_black(), 0);
}

void RebuildRows(const std::deque<Segment>& segs) {
    lv_obj_clean(s_ui.subs);
    s_ui.rows.clear();
    s_ui.empty = nullptr;
    const size_t from = segs.size() > 8 ? segs.size() - 8 : 0;  // 只建最近 8 段，旧的被顶出屏幕
    for (size_t i = from; i < segs.size(); ++i) {
        lv_obj_t* src = Label(s_ui.subs, "", s_ui.f);
        lv_obj_set_width(src, LV_HOR_RES - 2 * kPad);
        lv_label_set_long_mode(src, LV_LABEL_LONG_WRAP);
        lv_obj_t* dst = Label(s_ui.subs, "", s_ui.ft);
        lv_obj_set_width(dst, LV_HOR_RES - 2 * kPad);
        lv_label_set_long_mode(dst, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_pad_bottom(dst, 14, 0);
        s_ui.rows.emplace_back(src, dst);
    }
    if (segs.empty()) {
        s_ui.empty = Label(s_ui.subs, "点下面「开始」或短按 BOOT，\n对着设备说话，字幕会出现在这里。", s_ui.f);
    }
}

void SetIfChanged(lv_obj_t* l, const std::string& t) {
    const char* cur = lv_label_get_text(l);
    if (cur == nullptr || t != cur) lv_label_set_text(l, t.c_str());
}

void Render() {
    if (!s_active.load() || !s_ui.subs) return;
    std::deque<Segment> segs;
    Phase phase;
    std::string hint;
    int target;
    int64_t start_us, audio_ms;
    bool rebuild;
    {
        std::lock_guard<std::mutex> lock(S().mu);
        if (!S().dirty && !S().rebuild) return;
        segs = S().segs;
        phase = S().phase;
        hint = S().hint;
        target = S().target;
        start_us = S().start_us;
        audio_ms = S().total_audio_ms;
        rebuild = S().rebuild;
        S().dirty = false;
        S().rebuild = false;
    }
    if (rebuild || s_ui.rows.size() != std::min<size_t>(segs.size(), 8)) RebuildRows(segs);
    const size_t from = segs.size() > 8 ? segs.size() - 8 : 0;
    for (size_t i = from, k = 0; i < segs.size() && k < s_ui.rows.size(); ++i, ++k) {
        std::string src = segs[i].src;
        if (segs[i].speaker >= 0) src = "[" + std::to_string(segs[i].speaker + 1) + "] " + src;
        SetIfChanged(s_ui.rows[k].first, src.empty() ? "…" : src);
        SetIfChanged(s_ui.rows[k].second, segs[i].dst);
    }
    for (int i = 0; i < kLangCount; ++i) {
        if (s_ui.lang_btns[i]) Invert(s_ui.lang_btns[i], i == target);
    }
    const bool running = phase != Phase::Idle;
    SetIfChanged(s_ui.btn_lbl, running ? "停止" : "开始");
    Invert(s_ui.btn, running);
    char st[96];
    if (phase == Phase::Running || (phase == Phase::Idle && audio_ms > 0)) {
        const int sec = static_cast<int>((phase == Phase::Running ? (esp_timer_get_time() - start_us) / 1000 : audio_ms) / 1000);
        std::snprintf(st, sizeof(st), "%s %02d:%02d", phase == Phase::Running ? "已听" : "本次", sec / 60, sec % 60);
    } else {
        st[0] = '\0';
    }
    std::string line = hint;
    if (st[0]) line = line.empty() ? st : (std::string(st) + "  " + line);
    SetIfChanged(s_ui.status, line);
    s_ui.last_ui_us = esp_timer_get_time();
}

void RenderAsync(void*) { Render(); }
void PostUi() {
    // 节流：太密的更新交给定时器合并
    if (esp_timer_get_time() - s_ui.last_ui_us < kUiMinGapMs * 1000LL) return;
    ScreenLvAsync(RenderAsync);
}

void OnTimer(lv_timer_t*) {
    {
        std::lock_guard<std::mutex> lock(S().mu);
        if (S().phase == Phase::Running) S().dirty = true;  // 刷新计时
    }
    Render();
}

void OnToggle(lv_event_t*) { Toggle(); }

void OnLang(lv_event_t* e) {
    const int i = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (!s_ui.lang_ok[i]) return;
    bool restart = false;
    {
        std::lock_guard<std::mutex> lock(S().mu);
        if (S().target == i) return;
        S().target = i;
        S().dirty = true;
        restart = S().phase == Phase::Running;
    }
    if (restart) SetHint("目标语言改了，停止后重新开始生效");
    Render();
}

bool OnBootClick() {
    ScreenLvAsync([](void*) { Toggle(); });
    return true;
}

bool OnVkKey(const char* key) {
    if (std::strcmp(key, "vk_prev") == 0) {
        ScreenLvAsync([](void*) {
            StopSession();
            ScreenNavigateBack();
        });
        return true;
    }
    return false;
}

void OnLifecycle(lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_SCREEN_LOADED) {
        s_active.store(true);
        if (auto* d = LVAdapterDisplay::Instance()) {
            d->SetStatusTitlePrefix(nullptr);
            d->UpdateStatusBar(true);
        }
        {
            std::lock_guard<std::mutex> lock(S().mu);
            S().rebuild = true;
            if (S().hint.empty()) S().hint = LoadCfg().ok() ? "" : "缺少 Key：my_ai.json 加 dashscope_api_key";
        }
        s_ui.timer = lv_timer_create(OnTimer, 1000, nullptr);
        Render();
    } else if (lv_event_get_target(e) == s_ui.scr) {
        s_active.store(false);
        StopSession();
        if (s_ui.timer) lv_timer_delete(s_ui.timer);
        s_ui.timer = nullptr;
        s_ui.subs = nullptr;
        s_ui.rows.clear();
    }
}

}  // namespace

lv_obj_t* TranslateScreen::Create() {
    s_ui = Ui{};
    s_ui.f = fontpack_lv_font_ui();
    s_ui.fb = fontpack_lv_font_get(30, 4);
    if (!s_ui.fb) s_ui.fb = s_ui.f;
    s_ui.ft = fontpack_lv_font_get(30, 2);
    if (!s_ui.ft) s_ui.ft = s_ui.f;

    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    s_ui.scr = scr;
    EpdStatusBar status = ScreenCreateStatusBar(scr);
    s_ui.top = status.height;

    lv_obj_t* body = Box(scr);
    lv_obj_set_size(body, LV_HOR_RES, LV_VER_RES - s_ui.top);
    lv_obj_set_pos(body, 0, s_ui.top);

    lv_obj_t* title = Label(body, "翻译", s_ui.fb);
    lv_obj_set_pos(title, kPad, 12);
    lv_obj_t* to = Label(body, "译成", s_ui.f);
    lv_obj_set_pos(to, 112, 18);
    int x = 170;
    for (int i = 0; i < kLangCount; ++i) {
        lv_font_glyph_dsc_t g;
        s_ui.lang_ok[i] = lv_font_get_glyph_dsc(s_ui.ft, &g, kLangs[i].probe, 0) && g.box_w > 0;
        if (!s_ui.lang_ok[i]) continue;  // 字体里没有这种文字就不提供
        if (x + 66 > LV_HOR_RES - kPad) break;
        s_ui.lang_btns[i] = Btn(body, kLangs[i].name, 66, 44, OnLang, i);
        lv_obj_set_pos(s_ui.lang_btns[i], x, 8);
        x += 70;
    }
    lv_obj_t* rule = Box(body);
    lv_obj_set_size(rule, LV_HOR_RES - 2 * kPad, 2);
    lv_obj_set_pos(rule, kPad, 62);
    lv_obj_set_style_bg_color(rule, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);

    constexpr int kBottomH = 96;
    s_ui.subs = Box(body);
    const int subs_h = LV_VER_RES - s_ui.top - 70 - kBottomH;
    lv_obj_set_size(s_ui.subs, LV_HOR_RES - 2 * kPad, subs_h);
    lv_obj_set_pos(s_ui.subs, kPad, 70);
    lv_obj_set_flex_flow(s_ui.subs, LV_FLEX_FLOW_COLUMN);
    // 最新的在最下面，旧的往上顶出去
    lv_obj_set_flex_align(s_ui.subs, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_ui.subs, 4, 0);

    lv_obj_t* rule2 = Box(body);
    lv_obj_set_size(rule2, LV_HOR_RES - 2 * kPad, 2);
    lv_obj_set_pos(rule2, kPad, LV_VER_RES - s_ui.top - kBottomH);
    lv_obj_set_style_bg_color(rule2, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(rule2, LV_OPA_COVER, 0);

    s_ui.btn = Btn(body, "开始", 140, 64, OnToggle, 0, &s_ui.btn_lbl);
    lv_obj_set_pos(s_ui.btn, LV_HOR_RES - kPad - 140, LV_VER_RES - s_ui.top - kBottomH + 16);
    s_ui.status = Label(body, "", s_ui.f);
    lv_obj_set_width(s_ui.status, LV_HOR_RES - 2 * kPad - 150);
    lv_label_set_long_mode(s_ui.status, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_ui.status, kPad, LV_VER_RES - s_ui.top - kBottomH + 8);

    lv_obj_move_foreground(status.bar);
    lv_obj_move_foreground(status.overlay);
    ScreenSetIsHome(false);
    VkKeyScreenDesc desc{};
    desc.factory = TranslateScreen::Create;
    desc.on_key = OnVkKey;
    desc.on_boot_click = OnBootClick;
    VkKey_AttachScreen(scr, kScreenName, desc);
    lv_obj_add_event_cb(scr, OnLifecycle, LV_EVENT_SCREEN_LOADED, nullptr);
    lv_obj_add_event_cb(scr, OnLifecycle, LV_EVENT_SCREEN_UNLOADED, nullptr);
    {
        std::lock_guard<std::mutex> lock(S().mu);
        S().rebuild = true;
        S().dirty = true;
    }
    return scr;
}

bool TranslateScreen::IsActive() { return s_active.load(); }

void TranslateScreen::DebugCmd(const char* cmd) {
    if (std::strcmp(cmd, "START") == 0) StartSession();
    else if (std::strcmp(cmd, "STOP") == 0) StopSession();
    else if (std::strncmp(cmd, "LANG ", 5) == 0) {
        for (int i = 0; i < kLangCount; ++i) {
            if (std::strcmp(cmd + 5, kLangs[i].code) == 0) {
                std::lock_guard<std::mutex> lock(S().mu);
                S().target = i;
                S().dirty = true;
            }
        }
        Render();
    } else if (std::strncmp(cmd, "EV ", 3) == 0) {
        HandleEvent(cmd + 3, std::strlen(cmd + 3));  // 注入一条服务端事件，测界面
        Render();
    }
}
