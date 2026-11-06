// my_ai_screen.cc — 「我的 AI」：对话式界面（顶栏 / 气泡消息流 / 按住说话输入栏），墨水屏分页不滚动
#include "standby_screen/standby_classic.h"
#include "my_ai_screen.h"

#include "my_ai_client.h"
#include "my_ai_config.h"
#include "my_ai_history.h"

#include "application.h"
#include "board.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "lv_adapter_display.h"
#include "power_policy.h"
#include "screen_common.h"
#include "text_encoding.h"
#include "vk_key_handler.h"
#include "boot_key_handler.h"
#include "image_to_jpeg.h"
#include "display_orient.h"
#include <mbedtls/base64.h>

#include <algorithm>
#include <ctime>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

namespace {

constexpr const char* TAG = "MyAiScreen";
constexpr const char* kScreenName = "my_ai";

constexpr int kSampleRate = 16000;
constexpr size_t kMaxRecordSamples = kSampleRate * 60;      // 最长 60 秒
constexpr size_t kMinRecordSamples = kSampleRate * 6 / 10;  // 少于 0.6 秒视为误触
constexpr size_t kMaxTurnsKept = 30;
constexpr uint32_t kRenderPeriodMs = 500;
constexpr uint32_t kMicHoldDelayMs = 180;
constexpr int64_t kHintShowUs = 3 * 1000 * 1000;
// 流式输出时最多每 1.5 秒重排一次：墨水屏每 15 次局刷会全刷一次（黑白闪），刷得越勤闪得越勤
constexpr int64_t kStreamRenderUs = 1500 * 1000;
constexpr int64_t kThinkUiUs = 4 * 1000 * 1000;

// 版式（竖屏 480×800）
constexpr lv_coord_t kPad = 16;
constexpr lv_coord_t kHeaderH = 64;
constexpr lv_coord_t kPagerH = 34;
constexpr lv_coord_t kInputH = 92;
constexpr lv_coord_t kPillH = 64;
constexpr lv_coord_t kBubblePadH = 14;
constexpr lv_coord_t kBubblePadV = 10;
constexpr lv_coord_t kBubbleGap = 14;
constexpr lv_coord_t kBubbleRadius = 18;
constexpr lv_coord_t kCaptionGap = 4;

constexpr const char* kSuggestions[] = {
    "用一句话介绍一下你自己",
    "讲个简短的冷笑话",
    "给我一个今天的小目标",
};

// ---------------------------------------------------------------- 共享状态

enum class Phase { Idle, Recording, Network, Asr, Thinking, Answering, Speaking };

struct Turn {
    uint32_t id = 0;
    bool asr_pending = false;  // 语音还在识别
    std::string user;
    std::string reply;
    std::string note;        // 出错等附注
    std::string model;       // 回答所用模型
    size_t think_bytes = 0;  // 已收到的思考内容字节数
    int web_refs = 0;        // 联网搜索引用的网页数
    bool web_note = false;   // 联网不可用，已按普通模式回答
    std::string searching;   // 正在搜索的关键词（GLM 工具调用）
    bool has_image = false;  // 这一问带了截图
    bool streaming = false;  // 回答进行中
};

struct TurnCtx {
    std::atomic<bool> cancel{false};
    uint32_t gen = 0;
    uint32_t turn_id = 0;
    my_ai::Config cfg;
    my_ai::ChatEndpoint ep;
    std::vector<int16_t> pcm;
    std::string preset_text;  // 非空：跳过语音识别（点了建议问题）
    std::string image;        // 本轮附带的截图（data URL）
    std::vector<my_ai::ChatMessage> history;

    // TTS 句子队列
    std::mutex q_mu;
    std::condition_variable q_cv;
    std::deque<std::string> sentences;
    bool chat_done = false;
    bool tts_started = false;
    bool tts_finished = false;
    bool speaking = false;
};

struct Shared {
    std::mutex mu;
    my_ai::Config cfg;
    std::string pending_image;  // 已截好、等下一句提问带上的截图（data URL）
    bool image_encoding = false;
    std::deque<Turn> turns;
    Phase phase = Phase::Idle;
    std::string hint;          // 短暂提示（底部页码处）
    int64_t hint_until_us = 0;
    bool dirty = true;
    bool urgent = false;       // 用户操作触发：立即重绘，不受流式节流
    int page = 0;
    int page_count = 1;
    bool follow_tail = true;
    std::shared_ptr<TurnCtx> current;
    std::string conv_id;       // 当前对话在 SD 上的 id；空 = 还没保存过
};

Shared& S() {
    static Shared s;
    return s;
}

std::atomic<bool> s_active{false};
std::atomic<bool> s_overlay{false};  // 浮层打开中（不在我的AI页面也能跑对话）
inline bool EngineOn() { return s_active.load() || s_overlay.load(); }
std::atomic<bool> s_listen_on_load{false};  // 由 BOOT 长按呼出：载入后若 BOOT 仍按着就开录
std::atomic<uint32_t> s_gen{0};
uint32_t s_next_turn_id = 1;  // 受 S().mu 保护

// 录音（AFE 输出任务写入）
std::mutex s_rec_mu;
std::vector<int16_t> s_rec;
bool s_recording = false;
int64_t s_rec_start_us = 0;

// PTT 源
std::atomic<bool> s_boot_ptt{false};
std::atomic<bool> s_touch_ptt{false};

// UI 对象（仅 LVGL 线程访问）
struct Ui {
    lv_obj_t* scr = nullptr;
    lv_obj_t* model_chip = nullptr;
    lv_obj_t* model_label = nullptr;
    lv_obj_t* msg_area = nullptr;
    lv_obj_t* pager = nullptr;
    lv_obj_t* pager_prev = nullptr;
    lv_obj_t* pager_next = nullptr;
    lv_obj_t* pager_label = nullptr;
    lv_obj_t* pill = nullptr;
    lv_obj_t* pill_label = nullptr;
    lv_obj_t* mic = nullptr;
    lv_obj_t* picker = nullptr;  // 模型选择浮层
    lv_obj_t* history = nullptr;  // 历史对话浮层
    int history_page = 0;
    lv_timer_t* render_timer = nullptr;
    lv_timer_t* mic_timer = nullptr;
    const lv_font_t* f_body = nullptr;   // 25@2 气泡正文
    const lv_font_t* f_small = nullptr;  // 说明文字（同 25@2）
    const lv_font_t* f_title = nullptr;  // 30@4 标题
    lv_coord_t area_w = 0;
    lv_coord_t area_h = 0;
    std::string last_sig;  // 上次渲染签名，内容不变不重建
    std::string last_chip;
    std::string last_pager;
    std::string last_pill;
    int last_pill_inv = -1;
    int64_t last_render_us = 0;
};
Ui s_ui;

void MarkUrgentLocked() {
    S().dirty = true;
    S().urgent = true;
}

void MarkDirty() {
    std::lock_guard<std::mutex> lock(S().mu);
    S().dirty = true;
}

void SetPhase(Phase p) {
    std::lock_guard<std::mutex> lock(S().mu);
    if (S().phase != p) MarkUrgentLocked();  // 输入栏状态变化要即时
    S().phase = p;
}

void ShowHint(const std::string& h) {
    std::lock_guard<std::mutex> lock(S().mu);
    S().hint = h;
    S().hint_until_us = esp_timer_get_time() + kHintShowUs;
    S().dirty = true;
    S().urgent = true;
}


bool TurnAlive(const TurnCtx& ctx) {
    return !ctx.cancel.load(std::memory_order_acquire) && ctx.gen == s_gen.load() && EngineOn();
}

// 在主循环里切 DeviceState（驱动 PA / 功耗策略）；仅当仍是本轮且在本页
void ScheduleDeviceState(DeviceState st, uint32_t gen) {
    Application::GetInstance().Schedule([st, gen]() {
        if (gen != s_gen.load() || !EngineOn()) {
            return;
        }
        Application::GetInstance().SetDeviceState(st);
    });
}

// ---------------------------------------------------------------- 文本处理

// 去掉 Markdown 记号，避免被朗读出来
std::string CleanForTts(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if (c == '*' || c == '#' || c == '`' || c == '>' || c == '|' || c == '_') {
            continue;
        }
        out.push_back(c);
    }
    return out;
}

bool HasSpeakable(const std::string& s) {
    for (unsigned char c : s) {
        if (c >= 0x80 || std::isalnum(c)) {
            return true;
        }
    }
    return false;
}

// 从 pending 中切出可朗读的完整句子
void SplitSentences(std::string& pending, bool first_pending, std::vector<std::string>& out) {
    static const char* kEnds[] = {"。", "！", "？", "；", "!", "?", ";", "\n", "…"};
    static const char* kSoft[] = {"，", "、", ",", "："};
    while (!pending.empty()) {
        size_t cut = std::string::npos;
        for (const char* e : kEnds) {
            const size_t p = pending.find(e);
            if (p != std::string::npos) {
                const size_t end = p + std::strlen(e);
                if (cut == std::string::npos || end < cut) cut = end;
            }
        }
        // 首句尽快出声：逗号处也切；过长句子在逗号处强切
        if (cut == std::string::npos && (first_pending ? pending.size() >= 24 : pending.size() >= 180)) {
            for (const char* e : kSoft) {
                const size_t p = pending.rfind(e);
                if (p != std::string::npos) {
                    const size_t end = p + std::strlen(e);
                    if (cut == std::string::npos || end > cut) cut = end;
                }
            }
        }
        if (cut == std::string::npos) {
            if (pending.size() < 300) {
                return;
            }
            // 没有标点也别无限攒：按 UTF-8 边界截断
            cut = 240;
            while (cut < pending.size() && (static_cast<unsigned char>(pending[cut]) & 0xC0) == 0x80) ++cut;
        }
        std::string s = pending.substr(0, cut);
        pending.erase(0, cut);
        first_pending = false;
        if (HasSpeakable(s)) {
            out.push_back(std::move(s));
        }
    }
}

// ---------------------------------------------------------------- TTS 任务

void TtsTask(void* arg) {
    auto* holder = static_cast<std::shared_ptr<TurnCtx>*>(arg);
    std::shared_ptr<TurnCtx> ctx = *holder;
    delete holder;
    auto& audio = Application::GetInstance().GetAudioService();
    for (int i = 0; i < 40 && !audio.IsStarted() && TurnAlive(*ctx); ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!audio.IsStarted()) {
        ESP_LOGW(TAG, "tts: audio service not running, skip playback");
    }

    while (true) {
        std::string sentence;
        {
            std::unique_lock<std::mutex> lock(ctx->q_mu);
            ctx->q_cv.wait_for(lock, std::chrono::milliseconds(200), [&] {
                return !ctx->sentences.empty() || ctx->chat_done || ctx->cancel.load();
            });
            if (ctx->cancel.load()) {
                break;
            }
            if (ctx->sentences.empty()) {
                if (ctx->chat_done) break;
                continue;
            }
            sentence = std::move(ctx->sentences.front());
            ctx->sentences.pop_front();
        }
        std::string err;
        const bool ok = my_ai::TtsStream(ctx->cfg, CleanForTts(sentence), [&](std::vector<int16_t>&& pcm) {
            if (!TurnAlive(*ctx)) {
                return false;
            }
            if (!ctx->speaking) {
                ctx->speaking = true;
                ScheduleDeviceState(kDeviceStateSpeaking, ctx->gen);
            }
            if (!audio.PushPcmToPlayback(std::move(pcm), my_ai::kTtsSampleRate, &ctx->cancel)) {
                if (TurnAlive(*ctx)) ESP_LOGW(TAG, "tts: push pcm failed (audio started=%d)", audio.IsStarted());
                return false;
            }
            return true;
        }, err, ctx->cancel);
        if (!ok && !TurnAlive(*ctx)) {
            break;
        }
        if (!ok) {
            ESP_LOGW(TAG, "tts failed: %s", err.c_str());
        }
    }

    // 等播放队列放完再回 Idle（关 PA）
    if (ctx->speaking) {
        for (int i = 0; i < 600 && TurnAlive(*ctx) && !audio.IsPlaybackIdle(); ++i) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        vTaskDelay(pdMS_TO_TICKS(150));
        ScheduleDeviceState(kDeviceStateIdle, ctx->gen);
    }
    {
        std::lock_guard<std::mutex> lock(ctx->q_mu);
        ctx->tts_finished = true;
    }
    ctx->q_cv.notify_all();
    vTaskDeleteWithCaps(nullptr);
}

void EnqueueSentence(const std::shared_ptr<TurnCtx>& ctx, std::string&& s) {
    bool start = false;
    {
        std::lock_guard<std::mutex> lock(ctx->q_mu);
        ctx->sentences.push_back(std::move(s));
        if (!ctx->tts_started) {
            ctx->tts_started = true;
            start = true;
        }
    }
    ctx->q_cv.notify_all();
    if (start) {
        auto* holder = new std::shared_ptr<TurnCtx>(ctx);
        if (xTaskCreateWithCaps(TtsTask, "my_ai_tts", 10 * 1024, holder, 3, nullptr,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            ESP_LOGE(TAG, "create tts task failed");
            delete holder;
            std::lock_guard<std::mutex> lock(ctx->q_mu);
            ctx->tts_finished = true;
        }
    }
}

// ---------------------------------------------------------------- 一轮对话

void PushTurnLocked(Turn&& t) {
    S().turns.push_back(std::move(t));
    while (S().turns.size() > kMaxTurnsKept) S().turns.pop_front();
    S().follow_tail = true;
    S().dirty = true;
}

void SaveHistoryNow() {
    std::string id;
    std::vector<my_ai::HistTurn> hist;
    {
        std::lock_guard<std::mutex> lock(S().mu);
        for (const auto& t : S().turns) {
            if (t.asr_pending || t.user.empty()) continue;
            hist.push_back({t.user, t.reply, t.model, t.note, t.web_refs});
        }
        if (hist.empty()) return;
        if (S().conv_id.empty()) S().conv_id = my_ai::NewConversationId();
        id = S().conv_id;
    }
    if (my_ai::SaveConversation(id, hist)) my_ai::CloudSyncAsync();
}

Turn* FindTurnLocked(uint32_t id) {
    for (auto it = S().turns.rbegin(); it != S().turns.rend(); ++it) {
        if (it->id == id) return &*it;
    }
    return nullptr;
}

void WorkerTask(void* arg) {
    auto* holder = static_cast<std::shared_ptr<TurnCtx>*>(arg);
    std::shared_ptr<TurnCtx> ctx = *holder;
    delete holder;

    {
        PowerNeedHold hold_net(PowerNeed::OtaDownload);
        std::string err;
        do {
            auto& board = Board::GetInstance();
            if (!board.IsNetworkReady()) {
                SetPhase(Phase::Network);
                if (!board.EnsureNetworkReady(20000)) {
                    err = "网络未连接，请检查 Wi-Fi";
                    break;
                }
            }
            if (!TurnAlive(*ctx)) break;

            std::string text = ctx->preset_text;
            if (text.empty()) {
                SetPhase(Phase::Asr);
                if (!my_ai::Asr(ctx->cfg, ctx->pcm, text, err, ctx->cancel)) {
                    break;
                }
                ctx->pcm.clear();
                ctx->pcm.shrink_to_fit();
                if (!TurnAlive(*ctx)) break;
                if (!HasSpeakable(text)) {
                    err = "没听清，请再说一次";
                    break;
                }
            }

            {
                std::lock_guard<std::mutex> lock(S().mu);
                if (Turn* t = FindTurnLocked(ctx->turn_id)) {
                    t->user = text;
                    t->asr_pending = false;
                }
                S().phase = ctx->ep.thinking ? Phase::Thinking : Phase::Answering;
                S().dirty = true;
            }

            std::vector<my_ai::ChatMessage> msgs = ctx->history;
            msgs.push_back({"user", text, ctx->image});
            ctx->image.clear();
            ctx->image.shrink_to_fit();

            std::string pending;
            bool first_sentence = true;
            int64_t last_think_ui = 0;
            my_ai::ChatCallbacks cb;
            cb.on_reasoning = [&](size_t bytes) {
                const int64_t now = esp_timer_get_time();
                if (now - last_think_ui < kThinkUiUs) return;
                last_think_ui = now;
                std::lock_guard<std::mutex> lock(S().mu);
                if (Turn* t = FindTurnLocked(ctx->turn_id)) t->think_bytes = bytes;
                S().dirty = true;
            };
            cb.on_searching = [&](const std::string& q) {
                std::lock_guard<std::mutex> lock(S().mu);
                if (Turn* t = FindTurnLocked(ctx->turn_id)) t->searching = q;
                MarkUrgentLocked();
            };
            cb.on_web_refs = [&](int refs) {
                std::lock_guard<std::mutex> lock(S().mu);
                if (Turn* t = FindTurnLocked(ctx->turn_id)) t->web_refs = refs;
                S().dirty = true;
            };
            cb.on_content = [&](const std::string& delta) {
                {
                    std::lock_guard<std::mutex> lock(S().mu);
                    if (Turn* t = FindTurnLocked(ctx->turn_id)) t->reply += delta;
                    S().phase = Phase::Answering;
                    S().dirty = true;
                }
                if (ctx->cfg.tts) {
                    pending += delta;
                    std::vector<std::string> out;
                    SplitSentences(pending, first_sentence, out);
                    for (auto& s : out) {
                        first_sentence = false;
                        EnqueueSentence(ctx, std::move(s));
                    }
                }
            };
            bool ok = my_ai::ChatStream(ctx->cfg, ctx->ep, msgs, cb, err, ctx->cancel);
            if (!ok && ctx->ep.web_search && err.find("webSearchEnabled") != std::string::npos &&
                TurnAlive(*ctx)) {
                // 账号没开通联网插件（或刚开通，平台有约 5 分钟缓存）：去掉联网重试一次
                ESP_LOGW(TAG, "web search not enabled on account, retry without it");
                ctx->ep.web_search = false;
                err.clear();
                {
                    std::lock_guard<std::mutex> lock(S().mu);
                    if (Turn* t = FindTurnLocked(ctx->turn_id)) t->web_note = true;
                }
                ok = my_ai::ChatStream(ctx->cfg, ctx->ep, msgs, cb, err, ctx->cancel);
            }
            if (ctx->cfg.tts && ok && HasSpeakable(pending)) {
                EnqueueSentence(ctx, std::move(pending));
            }
            {
                std::lock_guard<std::mutex> lock(S().mu);
                if (Turn* t = FindTurnLocked(ctx->turn_id)) t->streaming = false;
                S().dirty = true;
            }
            if (!ok) break;
        } while (false);

        {
            std::lock_guard<std::mutex> lock(ctx->q_mu);
            ctx->chat_done = true;
        }
        ctx->q_cv.notify_all();

        if (!err.empty() && TurnAlive(*ctx)) {
            ESP_LOGW(TAG, "turn failed: %s", err.c_str());
            std::lock_guard<std::mutex> lock(S().mu);
            if (Turn* t = FindTurnLocked(ctx->turn_id)) {
                if (t->asr_pending) {
                    t->asr_pending = false;
                    t->user = "（语音消息）";
                }
                t->note = err;
                t->streaming = false;
                S().dirty = true;
            }
        }

        // 等朗读结束
        bool tts_running = false;
        {
            std::lock_guard<std::mutex> lock(ctx->q_mu);
            tts_running = ctx->tts_started && !ctx->tts_finished;
        }
        if (tts_running && TurnAlive(*ctx)) {
            SetPhase(Phase::Speaking);
        }
        {
            std::unique_lock<std::mutex> lock(ctx->q_mu);
            ctx->q_cv.wait(lock, [&] { return !ctx->tts_started || ctx->tts_finished; });
        }
    }

    // 存历史：取消的轮次也照存（保留已出现的内容）
    {
        bool has = false;
        {
            std::lock_guard<std::mutex> lock(S().mu);
            if (Turn* t = FindTurnLocked(ctx->turn_id)) has = !t->asr_pending && !t->user.empty();
        }
        if (has) SaveHistoryNow();
    }
    {
        std::lock_guard<std::mutex> lock(S().mu);
        if (Turn* t = FindTurnLocked(ctx->turn_id)) {
            t->streaming = false;
            if (t->asr_pending) {  // 被取消
                t->asr_pending = false;
                t->user = "（语音消息）";
                if (t->note.empty()) t->note = "已取消";
            }
        }
        if (S().current == ctx) {
            S().current.reset();
            S().phase = Phase::Idle;
        }
        S().dirty = true;
    }
    ESP_LOGI(TAG, "turn done (internal free=%u psram free=%u)",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    vTaskDeleteWithCaps(nullptr);
}

void CancelCurrentTurn() {
    std::shared_ptr<TurnCtx> cur;
    {
        std::lock_guard<std::mutex> lock(S().mu);
        cur = S().current;
        S().current.reset();
        S().phase = Phase::Idle;
        if (cur) {
            if (Turn* t = FindTurnLocked(cur->turn_id)) {
                t->streaming = false;
                if (t->asr_pending) {
                    t->asr_pending = false;
                    t->user = "（语音消息）";
                }
                if (t->reply.empty() && t->note.empty()) t->note = "已取消";
            }
        }
        S().dirty = true;
    }
    s_gen.fetch_add(1);
    if (cur) {
        cur->cancel.store(true, std::memory_order_release);
        cur->q_cv.notify_all();
    }
}

void StartTurn(std::vector<int16_t>&& pcm, std::string preset = {}) {
    auto ctx = std::make_shared<TurnCtx>();
    ctx->gen = s_gen.load();
    ctx->pcm = std::move(pcm);
    ctx->preset_text = std::move(preset);
    {
        std::lock_guard<std::mutex> lock(S().mu);
        ctx->cfg = S().cfg;
        ctx->ep = S().cfg.chat();
        ctx->image.swap(S().pending_image);
        if (!ctx->image.empty() && !ctx->ep.SupportsVision()) {
            // 当前模型不能看图：这一问换成能看图的模型（同厂优先）
            int pick = -1;
            for (size_t i = 0; i < ctx->cfg.models.size(); ++i) {
                const auto& m = ctx->cfg.models[i];
                if (!m.SupportsVision()) continue;
                if (pick < 0 || (m.vendor == ctx->ep.vendor && ctx->cfg.models[pick].vendor != ctx->ep.vendor)) {
                    pick = static_cast<int>(i);
                }
            }
            if (pick >= 0) {
                ESP_LOGI(TAG, "image turn: %s -> %s", ctx->ep.model.c_str(), ctx->cfg.models[pick].model.c_str());
                ctx->ep = ctx->cfg.models[pick];
            }
        }
        ctx->ep.web_search = ctx->cfg.WebOn(ctx->ep) && ctx->ep.SupportsWebSearch();
        std::string sys = ctx->cfg.system_prompt;
        const time_t now = time(nullptr);
        struct tm tmv;
        if (now > 1700000000 && localtime_r(&now, &tmv) != nullptr) {
            static const char* kWeek[] = {"日", "一", "二", "三", "四", "五", "六"};
            char buf[64];
            std::snprintf(buf, sizeof(buf), "\n当前时间：%04d-%02d-%02d 星期%s %02d:%02d。",
                          tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, kWeek[tmv.tm_wday],
                          tmv.tm_hour, tmv.tm_min);
            sys += buf;
        }
        {
            char wx[200];
            if (StandbyClassic_LocationWeatherForAi(wx, sizeof(wx)) > 0) {
                sys += "\n";
                sys += wx;
                sys += "问到「附近」「这里」「今天天气」等时按这个位置回答，用户说了别的地方就以用户说的为准。";
            }
        }
        ctx->history.push_back({"system", sys});
        // 带最近 max_turns 轮已完成的对话
        std::vector<const Turn*> done;
        for (const auto& t : S().turns) {
            if (!t.user.empty() && !t.reply.empty()) done.push_back(&t);
        }
        const size_t keep = static_cast<size_t>(ctx->cfg.max_turns);
        const size_t from = done.size() > keep ? done.size() - keep : 0;
        for (size_t i = from; i < done.size(); ++i) {
            ctx->history.push_back({"user", done[i]->user});
            ctx->history.push_back({"assistant", done[i]->reply});
        }
        // 先放一条提问气泡：语音显示「识别中」，失败也保留
        Turn t;
        t.id = s_next_turn_id++;
        t.has_image = !ctx->image.empty();
        t.user = ctx->preset_text;
        t.asr_pending = ctx->preset_text.empty();
        t.model = ctx->ep.model;
        t.streaming = true;
        ctx->turn_id = t.id;
        PushTurnLocked(std::move(t));
        S().current = ctx;
        S().phase = ctx->preset_text.empty() ? Phase::Asr : Phase::Thinking;
    }
    // 朗读要用音频服务：语音提问时录音前已拉起，点建议问题时这里补一次
    // （开机收尾、离开百问等路径会停掉音频服务）
    if (ctx->cfg.tts) {
        Application::GetInstance().Schedule([]() {
            if (EngineOn()) Application::GetInstance().EnsureAudioServiceRunning();
        });
    }
    auto* holder = new std::shared_ptr<TurnCtx>(ctx);
    if (xTaskCreateWithCaps(WorkerTask, "my_ai_turn", 12 * 1024, holder, 3, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "create worker failed");
        delete holder;
        {
            std::lock_guard<std::mutex> lock(S().mu);
            S().current.reset();
            S().phase = Phase::Idle;
        }
        ShowHint("内存不足，请稍后再试");
    }
}

bool ConfigReady() {
    std::lock_guard<std::mutex> lock(S().mu);
    return S().cfg.loaded;
}

// ---------------------------------------------------------------- PTT

// 主循环执行
void BeginRecordingOnMain() {
    if (!EngineOn()) return;
    if (!ConfigReady()) {
        ShowHint("配置未就绪，见屏幕说明");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(s_rec_mu);
        if (s_recording) return;
    }
    CancelCurrentTurn();
    auto& app = Application::GetInstance();
    app.EnsureAudioServiceRunning();
    auto& audio = app.GetAudioService();
    if (!audio.IsStarted()) {
        ShowHint("音频服务启动失败");
        return;
    }
    audio.ResetDecoder();  // 停掉正在播的
    {
        std::lock_guard<std::mutex> lock(s_rec_mu);
        s_rec.clear();
        s_rec.reserve(kSampleRate * 15);
        s_recording = true;
        s_rec_start_us = esp_timer_get_time();
    }
    audio.SetPcmCaptureSink([](std::vector<int16_t>&& pcm) {
        std::lock_guard<std::mutex> lock(s_rec_mu);
        if (!s_recording || s_rec.size() >= kMaxRecordSamples) return;
        const size_t room = kMaxRecordSamples - s_rec.size();
        s_rec.insert(s_rec.end(), pcm.begin(), pcm.begin() + std::min(room, pcm.size()));
    });
    audio.EnableVoiceProcessing(true);
    app.SetDeviceState(kDeviceStateListening);
    SetPhase(Phase::Recording);
    ESP_LOGI(TAG, "PTT start");
}

void EndRecordingOnMain() {
    std::vector<int16_t> pcm;
    {
        std::lock_guard<std::mutex> lock(s_rec_mu);
        if (!s_recording) return;
        s_recording = false;
        pcm.swap(s_rec);
    }
    auto& app = Application::GetInstance();
    auto& audio = app.GetAudioService();
    audio.EnableVoiceProcessing(false);
    audio.SetPcmCaptureSink(nullptr);
    app.SetDeviceState(kDeviceStateIdle);
    SetPhase(Phase::Idle);
    ESP_LOGI(TAG, "PTT end: %.2fs", pcm.size() / static_cast<float>(kSampleRate));
    if (!EngineOn()) return;
    if (pcm.size() < kMinRecordSamples) {
        ShowHint("说话时间太短");
        return;
    }
    StartTurn(std::move(pcm));
}

void PttStart(std::atomic<bool>& src) {
    if (!EngineOn()) return;
    const bool was = s_boot_ptt.load() || s_touch_ptt.load();
    src.store(true);
    if (!was) {
        Application::GetInstance().Schedule(BeginRecordingOnMain);
    }
}

void PttStop(std::atomic<bool>& src) {
    if (!src.exchange(false)) return;
    if (s_boot_ptt.load() || s_touch_ptt.load()) return;
    Application::GetInstance().Schedule(EndRecordingOnMain);
}

bool IsBusy() {
    std::lock_guard<std::mutex> lock(S().mu);
    return S().current != nullptr;
}

void StopAll() {
    const bool was_busy = IsBusy();
    CancelCurrentTurn();
    Application::GetInstance().Schedule([was_busy]() {
        auto& app = Application::GetInstance();
        if (app.GetAudioService().IsStarted()) {
            app.GetAudioService().ResetDecoder();
        }
        if (EngineOn()) {
            app.SetDeviceState(kDeviceStateIdle);
            if (was_busy) ShowHint("已停止");
        }
    });
}

void NewChat() {
    CancelCurrentTurn();
    {
        std::lock_guard<std::mutex> lock(S().mu);
        S().pending_image.clear();
        S().turns.clear();
        S().conv_id.clear();
        S().page = 0;
        S().follow_tail = true;
        MarkUrgentLocked();
    }
    Application::GetInstance().Schedule([]() {
        auto& app = Application::GetInstance();
        if (app.GetAudioService().IsStarted()) app.GetAudioService().ResetDecoder();
        if (s_active.load()) app.SetDeviceState(kDeviceStateIdle);
    });
    ShowHint("已开始新对话");
}

void AskPreset(const std::string& text) {
    if (!ConfigReady()) {
        ShowHint("配置未就绪");
        return;
    }
    CancelCurrentTurn();
    StartTurn({}, text);
}

void TurnPage(int delta) {
    std::lock_guard<std::mutex> lock(S().mu);
    int p = S().page + delta;
    if (p < 0) p = 0;
    if (p > S().page_count - 1) p = S().page_count - 1;
    S().follow_tail = (p == S().page_count - 1);
    if (p != S().page) {
        S().page = p;
        MarkUrgentLocked();
    }
}

void SavePrefsLocked() {
    my_ai::Prefs p;
    const auto& c = S().cfg;
    if (!c.loaded) return;
    p.model = c.chat().model;
    p.web_mimo = c.web_mimo ? 1 : 0;
    p.web_glm = c.web_glm ? 1 : 0;
    for (const auto& m : c.models) p.thinking.emplace_back(m.model, m.thinking);
    my_ai::SavePrefs(p);
}

void ApplyPrefsLocked() {
    auto& c = S().cfg;
    if (!c.loaded) return;
    const my_ai::Prefs p = my_ai::LoadPrefs();
    if (!p.model.empty()) {
        const int idx = c.FindModel(p.model);
        if (idx >= 0) c.model_idx = idx;
    }
    if (p.web_mimo >= 0) c.web_mimo = p.web_mimo == 1;
    if (p.web_glm >= 0) c.web_glm = p.web_glm == 1;
    for (const auto& kv : p.thinking) {
        const int idx = c.FindModel(kv.first);
        if (idx >= 0 && !c.models[idx].ThinkingForced()) c.models[idx].thinking = kv.second;
    }
}

void SelectModel(int idx) {
    std::string hint;
    {
        std::lock_guard<std::mutex> lock(S().mu);
        auto& c = S().cfg;
        if (!c.loaded || idx < 0 || idx >= static_cast<int>(c.models.size())) return;
        if (idx != c.model_idx) {
            c.model_idx = idx;
            hint = "已切换到 " + c.chat().label;
            ESP_LOGI(TAG, "model -> %s", c.chat().model.c_str());
            SavePrefsLocked();
        }
        MarkUrgentLocked();
    }
    if (!hint.empty()) ShowHint(hint);
}

// ---------------------------------------------------------------- 排版（LVGL 线程）

lv_coord_t GlyphW(const lv_font_t* font, uint32_t cp) {
    const lv_coord_t w = static_cast<lv_coord_t>(lv_font_get_glyph_width(font, cp, 0));
    return w > 0 ? w : 1;
}

// 按宽度折行；返回各行像素宽度
void Wrap(const std::string& text, const lv_font_t* font, lv_coord_t max_w,
          std::vector<std::string>& lines, std::vector<lv_coord_t>& widths) {
    std::string line;
    lv_coord_t w = 0;
    const auto* data = reinterpret_cast<const uint8_t*>(text.data());
    size_t i = 0;
    auto flush = [&]() {
        lines.push_back(std::move(line));
        widths.push_back(w);
        line.clear();
        w = 0;
    };
    while (i < text.size()) {
        if (text[i] == '\r') {
            ++i;
            continue;
        }
        if (text[i] == '\n') {
            flush();
            ++i;
            continue;
        }
        uint32_t cp = 0;
        const size_t n = reader::Utf8Next(data + i, text.size() - i, &cp);
        if (n == 0) break;
        const lv_coord_t cw = GlyphW(font, cp);
        // 避头标点：逗号句号等不放到行首，宁可让本行多出一个字宽（气泡右侧留有余量）
        static const uint32_t kNoLead[] = {0xFF0C, 0x3002, 0x3001, 0xFF1B, 0xFF1A, 0xFF1F, 0xFF01, 0xFF09,
                                           0x300B, 0x300D, 0x300F, 0x201D, 0x2019, 0x2026, ',', '.', ';',
                                           ':', '?', '!', ')'};
        bool no_lead = false;
        for (uint32_t c : kNoLead) no_lead |= (c == cp);
        if (!line.empty() && w + cw > max_w && !(no_lead && w + cw <= max_w + cw)) flush();
        line.append(text.data() + i, n);
        w += cw;
        i += n;
    }
    flush();
    // 去掉尾部空行（模型常以换行结尾）
    while (lines.size() > 1 && lines.back().empty()) {
        lines.pop_back();
        widths.pop_back();
    }
}

enum class ItemKind { User, Ai, Note };

struct Item {
    ItemKind kind;
    std::string caption;  // AI 气泡上方小字（模型名）
    bool dim = false;     // 思考中占位
    std::vector<std::string> lines;
    std::vector<lv_coord_t> widths;
};

struct Seg {
    size_t item;
    size_t l0, l1;
    lv_coord_t y;
    bool caption;
};

lv_coord_t LineH(const lv_font_t* f) { return f->line_height > 0 ? f->line_height : 30; }

void Paginate(const std::vector<Item>& items, lv_coord_t page_h, std::vector<std::vector<Seg>>& pages) {
    pages.assign(1, {});
    const lv_coord_t lh = LineH(s_ui.f_body);
    const lv_coord_t nlh = LineH(s_ui.f_small);
    lv_coord_t y = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        const Item& it = items[i];
        const bool note = it.kind == ItemKind::Note;
        const lv_coord_t line_h = note ? nlh : lh;
        const lv_coord_t pad = note ? 0 : kBubblePadV;
        size_t l = 0;
        bool first = true;
        while (l < it.lines.size()) {
            const lv_coord_t cap = (first && !it.caption.empty()) ? LineH(s_ui.f_small) + kCaptionGap : 0;
            const lv_coord_t room = page_h - y - cap - pad * 2;
            int fit = room >= line_h ? static_cast<int>(room / line_h) : 0;
            if (fit <= 0 || (fit < 2 && it.lines.size() - l > 1 && y > 0)) {
                if (y == 0) fit = 1;  // 整页都放不下一行，强行放
                else {
                    pages.emplace_back();
                    y = 0;
                    continue;
                }
            }
            const size_t n = std::min<size_t>(static_cast<size_t>(fit), it.lines.size() - l);
            pages.back().push_back(Seg{i, l, l + n, y, cap > 0});
            y += cap + pad * 2 + static_cast<lv_coord_t>(n) * line_h + kBubbleGap;
            l += n;
            first = false;
            if (l < it.lines.size()) {
                pages.emplace_back();
                y = 0;
            }
        }
    }
}

std::string Join(const std::vector<std::string>& v, size_t b, size_t e) {
    std::string out;
    for (size_t i = b; i < e; ++i) {
        if (i > b) out.push_back('\n');
        out += v[i];
    }
    return out;
}

lv_obj_t* MakeLabel(lv_obj_t* parent, const lv_font_t* font, lv_color_t color = lv_color_black()) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_SCROLLABLE);
    lv_label_set_text(l, "");
    return l;
}

lv_obj_t* MakeBox(lv_obj_t* parent) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

void OnSuggestion(lv_event_t* e) {
    const auto idx = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    if (idx >= 0 && idx < static_cast<intptr_t>(sizeof(kSuggestions) / sizeof(kSuggestions[0]))) {
        HapticPulseIfEnabled();
        AskPreset(kSuggestions[idx]);
    }
}

void BuildEmptyState(lv_obj_t* area, const my_ai::Config& cfg) {
    lv_obj_t* col = MakeBox(area);
    lv_obj_set_size(col, s_ui.area_w, s_ui.area_h);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 14, 0);

    lv_obj_t* avatar = MakeBox(col);
    lv_obj_set_size(avatar, 96, 96);
    lv_obj_set_style_radius(avatar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(avatar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(avatar, LV_OPA_COVER, 0);
    lv_obj_t* av = MakeLabel(avatar, s_ui.f_title, lv_color_white());
    lv_label_set_text(av, "AI");
    lv_obj_center(av);

    if (!cfg.loaded) {
        lv_obj_t* t = MakeLabel(col, s_ui.f_body);
        lv_label_set_text(t, "还不能开始对话");
        lv_obj_t* d = MakeLabel(col, s_ui.f_small);
        lv_obj_set_width(d, s_ui.area_w - 40);
        lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text_fmt(d, "%s\n\n请在 SD 卡创建 metalio/e-ink/my_ai.json，至少填写 mimo_api_key。",
                              cfg.error.c_str());
        return;
    }

    lv_obj_t* t = MakeLabel(col, s_ui.f_title);
    lv_label_set_text(t, "有什么可以帮你？");
    lv_obj_t* d = MakeLabel(col, s_ui.f_small);
    lv_label_set_text(d, "按住下方按钮或 BOOT 键说话");

    lv_obj_t* spacer = MakeBox(col);
    lv_obj_set_size(spacer, 1, 10);

    for (intptr_t i = 0; i < static_cast<intptr_t>(sizeof(kSuggestions) / sizeof(kSuggestions[0])); ++i) {
        lv_obj_t* chip = MakeBox(col);
        lv_obj_set_size(chip, s_ui.area_w - 60, 52);
        lv_obj_set_style_radius(chip, 26, 0);
        lv_obj_set_style_border_width(chip, 2, 0);
        lv_obj_set_style_border_color(chip, lv_color_black(), 0);
        lv_obj_set_style_bg_color(chip, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(chip, OnSuggestion, LV_EVENT_CLICKED, reinterpret_cast<void*>(i));
        lv_obj_t* cl = MakeLabel(chip, s_ui.f_body);
        lv_label_set_text(cl, kSuggestions[i]);
        lv_obj_center(cl);
    }
}

void BuildBubble(lv_obj_t* area, const Item& it, const Seg& sg) {
    const lv_coord_t lh = LineH(s_ui.f_body);
    lv_coord_t y = sg.y;
    if (it.kind == ItemKind::Note) {
        lv_obj_t* n = MakeLabel(area, s_ui.f_small);
        lv_obj_set_width(n, s_ui.area_w);
        lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(n, Join(it.lines, sg.l0, sg.l1).c_str());
        lv_obj_set_pos(n, 0, y);
        return;
    }
    if (sg.caption) {
        lv_obj_t* c = MakeLabel(area, s_ui.f_small);
        lv_label_set_text(c, it.caption.c_str());
        lv_obj_set_pos(c, 4, y);
        y += LineH(s_ui.f_small) + kCaptionGap;
    }
    lv_coord_t text_w = 0;
    for (size_t i = sg.l0; i < sg.l1; ++i) text_w = std::max(text_w, it.widths[i]);
    text_w += 4;  // 余量，防止 LVGL 自行再折行
    const lv_coord_t w = text_w + kBubblePadH * 2;
    const lv_coord_t h = static_cast<lv_coord_t>(sg.l1 - sg.l0) * lh + kBubblePadV * 2;

    const bool user = it.kind == ItemKind::User;
    lv_obj_t* b = MakeBox(area);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, user ? s_ui.area_w - w : 0, y);
    lv_obj_set_style_radius(b, kBubbleRadius, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    if (user) {
        lv_obj_set_style_bg_color(b, lv_color_black(), 0);
    } else {
        lv_obj_set_style_bg_color(b, lv_color_white(), 0);
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_set_style_border_color(b, lv_color_black(), 0);
    }
    lv_obj_t* l = MakeLabel(b, s_ui.f_body, user ? lv_color_white() : lv_color_black());
    lv_obj_set_width(l, text_w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_label_set_text(l, Join(it.lines, sg.l0, sg.l1).c_str());
    lv_obj_set_pos(l, kBubblePadH, kBubblePadV);
}

const char* PillText(Phase p, bool recording_secs_known, int secs, std::string& buf) {
    switch (p) {
        case Phase::Recording:
            buf = "正在听… 松开发送";
            if (recording_secs_known) buf += "  " + std::to_string(secs) + "s";
            return buf.c_str();
        case Phase::Network: return "连接网络…   轻触停止";
        case Phase::Asr: return "识别中…   轻触停止";
        case Phase::Thinking: return "思考中…   轻触停止";
        case Phase::Answering: return "回答中…   轻触停止";
        case Phase::Speaking: return "朗读中…   轻触停止";
        default: return "按住  说话";
    }
}

void Render() {
    if (s_ui.msg_area == nullptr) return;

    my_ai::Config cfg_copy;
    std::vector<Item> items;
    Phase phase;
    std::string hint;
    bool has_shot = false;
    {
        std::lock_guard<std::mutex> lock(S().mu);
        const int64_t now_us = esp_timer_get_time();
        const bool hint_expired = S().hint_until_us && now_us > S().hint_until_us;
        if (!S().dirty && !hint_expired) {
            return;
        }
        const bool streaming = S().phase == Phase::Thinking || S().phase == Phase::Answering;
        if (!S().urgent && !hint_expired && streaming && now_us - s_ui.last_render_us < kStreamRenderUs) {
            return;  // 保留 dirty，下次再画
        }
        s_ui.last_render_us = now_us;
        S().dirty = false;
        S().urgent = false;
        if (S().hint_until_us && esp_timer_get_time() > S().hint_until_us) {
            S().hint.clear();
            S().hint_until_us = 0;
        }
        hint = S().hint;
        phase = S().phase;
        has_shot = !S().pending_image.empty() || S().image_encoding;
        cfg_copy.loaded = S().cfg.loaded;
        cfg_copy.error = S().cfg.error;
        if (S().cfg.loaded) {
            cfg_copy.models = S().cfg.models;
            cfg_copy.model_idx = S().cfg.model_idx;
            cfg_copy.web_mimo = S().cfg.web_mimo;
            cfg_copy.web_glm = S().cfg.web_glm;
        }

        const lv_coord_t user_max = s_ui.area_w * 3 / 4 - kBubblePadH * 2;
        const lv_coord_t ai_max = s_ui.area_w - 24 - kBubblePadH * 2;
        for (const auto& t : S().turns) {
            if (t.asr_pending || !t.user.empty()) {
                Item it{ItemKind::User, "", false, {}, {}};
                Wrap(t.asr_pending ? std::string(t.has_image ? "[截图] 语音识别中…" : "语音识别中…")
                                   : (t.has_image ? "[截图] " + t.user : t.user), s_ui.f_body, user_max, it.lines,
                     it.widths);
                items.push_back(std::move(it));
            }
            if (!t.asr_pending && !t.user.empty() && t.note.empty() && (t.streaming || !t.reply.empty())) {
                std::string cap = my_ai::PrettyModelName(t.model);
                if (t.web_refs > 0) cap += " · 参考 " + std::to_string(t.web_refs) + " 个网页";
                if (t.web_note) cap += " · 未联网：插件未开通";
                Item it{ItemKind::Ai, cap, false, {}, {}};
                std::string body = t.reply;
                if (body.empty() && !t.searching.empty()) {
                    body = "正在搜索：" + t.searching + "…";
                    if (t.web_refs > 0) body += "\n已找到 " + std::to_string(t.web_refs) + " 条结果，整理中…";
                    it.dim = true;
                } else if (body.empty()) {
                    body = t.think_bytes > 0 ? "思考中… 已想 " + std::to_string(t.think_bytes / 3) + " 字"
                                             : "思考中…";
                    it.dim = true;
                } else if (t.streaming) {
                    body += " …";
                }
                Wrap(body, s_ui.f_body, ai_max, it.lines, it.widths);
                items.push_back(std::move(it));
            }
            if (!t.note.empty() && !t.reply.empty()) {
                Item it{ItemKind::Ai, my_ai::PrettyModelName(t.model), false, {}, {}};
                Wrap(t.reply, s_ui.f_body, s_ui.area_w - 24 - kBubblePadH * 2, it.lines, it.widths);
                items.push_back(std::move(it));
            }
            if (!t.note.empty()) {
                Item it{ItemKind::Note, "", false, {}, {}};
                Wrap("— " + t.note + " —", s_ui.f_small, s_ui.area_w, it.lines, it.widths);
                items.push_back(std::move(it));
            }
        }
    }

    std::vector<std::vector<Seg>> pages;
    Paginate(items, s_ui.area_h, pages);
    int page;
    {
        std::lock_guard<std::mutex> lock(S().mu);
        S().page_count = static_cast<int>(pages.size());
        if (S().follow_tail || S().page >= S().page_count) S().page = S().page_count - 1;
        if (S().page < 0) S().page = 0;
        page = S().page;
    }

    // 签名：内容不变就不重建对象，避免墨水屏无谓局刷
    std::string sig = std::to_string(page) + "|" + std::to_string(items.size());
    for (const auto& sg : pages[page]) {
        const Item& it = items[sg.item];
        sig += "#" + std::to_string(sg.item) + ":" + std::to_string(sg.l0) + "-" + std::to_string(sg.l1);
        for (size_t i = sg.l0; i < sg.l1; ++i) sig += it.lines[i];
    }
    if (items.empty()) sig += cfg_copy.loaded ? "E1" : "E0" + cfg_copy.error;
    if (sig != s_ui.last_sig) {
        s_ui.last_sig = sig;
        lv_obj_clean(s_ui.msg_area);
        if (items.empty()) {
            BuildEmptyState(s_ui.msg_area, cfg_copy);
        } else {
            for (const auto& sg : pages[page]) BuildBubble(s_ui.msg_area, items[sg.item], sg);
        }
    }

    // 顶栏模型
    std::string chip;
    if (cfg_copy.loaded && !cfg_copy.models.empty()) {
        const auto& ce = cfg_copy.chat();
        // 默认开启的“思考”不显示，只标出与默认不同的状态，避免模型名被截断
        chip = ce.label;
        if (!ce.thinking && !ce.ThinkingForced()) chip += " · 快速";
        if (cfg_copy.WebOn(ce) && ce.SupportsWebSearch()) chip += " · 联网";
    }
    if (chip != s_ui.last_chip) {
        s_ui.last_chip = chip;
        lv_label_set_text(s_ui.model_label, chip.c_str());
        if (chip.empty()) lv_obj_add_flag(s_ui.model_chip, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(s_ui.model_chip, LV_OBJ_FLAG_HIDDEN);
    }

    // 翻页栏 / 提示
    const int pc = static_cast<int>(pages.size());
    std::string pager = hint;
    if (pager.empty() && pc > 1) pager = std::to_string(page + 1) + " / " + std::to_string(pc);
    if (pager != s_ui.last_pager) {
        s_ui.last_pager = pager;
        lv_label_set_text(s_ui.pager_label, pager.c_str());
    }
    auto set_vis = [](lv_obj_t* o, bool v) {
        if (v) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    };
    set_vis(s_ui.pager_prev, pc > 1 && page > 0);
    set_vis(s_ui.pager_next, pc > 1 && page < pc - 1);

    // 输入栏（录音中不跑秒：每次数字变化都是一次局刷）
    std::string buf;
    std::string pill = PillText(phase, false, 0, buf);
    if (has_shot && phase == Phase::Idle) pill += "（带截图）";
    const int inverted = phase == Phase::Recording ? 1 : 0;
    if (inverted != s_ui.last_pill_inv) {
        s_ui.last_pill_inv = inverted;
        lv_obj_set_style_bg_color(s_ui.pill, inverted ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_text_color(s_ui.pill_label, inverted ? lv_color_white() : lv_color_black(), 0);
        lv_obj_set_style_bg_color(s_ui.mic, inverted ? lv_color_white() : lv_color_black(), 0);
    }
    if (pill != s_ui.last_pill) {
        s_ui.last_pill = pill;
        lv_label_set_text(s_ui.pill_label, pill.c_str());
        set_vis(s_ui.mic, phase == Phase::Idle || phase == Phase::Recording);
    }
}

void OnRenderTimer(lv_timer_t*) { Render(); }

// ---------------------------------------------------------------- 交互（LVGL 线程）

void OnMicTimer(lv_timer_t* t) {
    lv_timer_delete(t);
    s_ui.mic_timer = nullptr;
    PttStart(s_touch_ptt);
}

void OnPillEvent(lv_event_t* e) {
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        if (s_ui.mic_timer == nullptr && !s_touch_ptt.load()) {
            s_ui.mic_timer = lv_timer_create(OnMicTimer, kMicHoldDelayMs, nullptr);
            lv_timer_set_repeat_count(s_ui.mic_timer, 1);
            lv_timer_set_auto_delete(s_ui.mic_timer, false);
        }
        return;
    }
    if (code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
    if (s_ui.mic_timer != nullptr) {
        // 轻触：忙时停止，闲时提示
        lv_timer_delete(s_ui.mic_timer);
        s_ui.mic_timer = nullptr;
        if (code == LV_EVENT_RELEASED) {
            if (IsBusy()) StopAll();
            else ShowHint("请按住按钮说话");
        }
        return;
    }
    PttStop(s_touch_ptt);
}

void ClosePicker() {
    if (s_ui.picker != nullptr) {
        lv_obj_delete(s_ui.picker);
        s_ui.picker = nullptr;
    }
}

void OnPickerBackdrop(lv_event_t* e) {
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) ClosePicker();
}

void OpenPicker();

void OnPickerItem(lv_event_t* e) {
    const int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    ClosePicker();
    SelectModel(idx);
}

void OnToggleThinking(lv_event_t*) {
    {
        std::lock_guard<std::mutex> lock(S().mu);
        auto& c = S().cfg;
        if (!c.loaded) return;
        auto& ep = c.models[c.model_idx];
        if (ep.ThinkingForced()) return;
        ep.thinking = !ep.thinking;
        ESP_LOGI(TAG, "thinking %s -> %d", ep.model.c_str(), ep.thinking ? 1 : 0);
        SavePrefsLocked();
        MarkUrgentLocked();
    }
    ClosePicker();
    OpenPicker();  // 重建以刷新开关状态
}

void OnToggleWeb(lv_event_t*) {
    {
        std::lock_guard<std::mutex> lock(S().mu);
        auto& c = S().cfg;
        if (!c.loaded || !c.chat().SupportsWebSearch()) return;
        c.SetWeb(c.chat(), !c.WebOn(c.chat()));
        ESP_LOGI(TAG, "web %s -> %d", c.chat().vendor.c_str(), c.WebOn(c.chat()) ? 1 : 0);
        SavePrefsLocked();
        MarkUrgentLocked();
    }
    ClosePicker();
    OpenPicker();
}

lv_obj_t* MakeButton(lv_obj_t* parent, lv_coord_t w, lv_coord_t h, lv_coord_t radius, bool border);

// 开关：圆角轨道 + 圆点；on 时黑底白点靠右
lv_obj_t* MakeSwitch(lv_obj_t* parent, bool on, bool enabled) {
    lv_obj_t* track = MakeBox(parent);
    lv_obj_set_size(track, 64, 34);
    lv_obj_set_style_radius(track, 17, 0);
    lv_obj_set_style_border_width(track, 2, 0);
    lv_obj_set_style_border_color(track, lv_color_black(), 0);
    lv_obj_set_style_bg_color(track, on ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(track, enabled ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_t* knob = MakeBox(track);
    lv_obj_set_size(knob, 24, 24);
    lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(knob, on && enabled ? lv_color_white() : lv_color_black(), 0);
    lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
    lv_obj_align(knob, on ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, on ? -3 : 3, 0);
    return track;
}

void OpenPicker() {
    if (s_ui.picker != nullptr || s_ui.scr == nullptr) return;
    std::vector<my_ai::ChatEndpoint> models;
    int cur = 0;
    bool web = false;
    {
        std::lock_guard<std::mutex> lock(S().mu);
        if (!S().cfg.loaded) return;
        if (S().current) {
            S().hint = "回答结束后再切换模型";
            S().hint_until_us = esp_timer_get_time() + kHintShowUs;
            MarkUrgentLocked();
            return;
        }
        models = S().cfg.models;
        cur = S().cfg.model_idx;
        web = S().cfg.WebOn(S().cfg.chat());
    }
    const auto& ce = models[cur];
    // 全屏透明背板：点空白处关闭
    lv_obj_t* bd = lv_obj_create(s_ui.scr);
    lv_obj_remove_style_all(bd);
    lv_obj_set_size(bd, LV_HOR_RES, LV_VER_RES);
    lv_obj_add_flag(bd, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(bd, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(bd, OnPickerBackdrop, LV_EVENT_CLICKED, nullptr);
    s_ui.picker = bd;

    const lv_coord_t row_h = 68;
    const lv_coord_t head_h = 50;
    const lv_coord_t n = static_cast<lv_coord_t>(models.size());
    const lv_coord_t card_w = LV_HOR_RES - kPad * 2;
    lv_obj_t* card = lv_obj_create(bd);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, card_w, head_h + row_h * n + 14 + head_h + row_h * 2 + 8);
    lv_obj_set_pos(card, kPad, lv_obj_get_y(s_ui.model_chip) + lv_obj_get_height(s_ui.model_chip) + 8);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 3, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);  // 吃掉点击，避免穿透关闭

    auto sep = [&](lv_coord_t y, lv_coord_t w, lv_coord_t x) {
        lv_obj_t* l = MakeBox(card);
        lv_obj_set_size(l, w, 1);
        lv_obj_set_pos(l, x, y);
        lv_obj_set_style_bg_color(l, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    };
    auto add_row = [&](lv_coord_t y) {
        lv_obj_t* row = lv_obj_create(card);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, card_w - 6, row_h);
        lv_obj_set_pos(row, 0, y);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        HapticAttachClick(row);
        return row;
    };

    lv_obj_t* title = MakeLabel(card, s_ui.f_small);
    lv_label_set_text(title, "模型");
    lv_obj_set_pos(title, 20, 12);
    for (lv_coord_t i = 0; i < n; ++i) {
        const auto& m = models[i];
        lv_obj_t* row = add_row(head_h + i * row_h);
        lv_obj_add_event_cb(row, OnPickerItem, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_obj_t* radio = MakeBox(row);
        lv_obj_set_size(radio, 26, 26);
        lv_obj_set_style_radius(radio, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(radio, 3, 0);
        lv_obj_set_style_border_color(radio, lv_color_black(), 0);
        lv_obj_align(radio, LV_ALIGN_LEFT_MID, 20, 0);
        if (i == cur) {
            lv_obj_t* dot = MakeBox(radio);
            lv_obj_set_size(dot, 12, 12);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(dot, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_center(dot);
        }
        lv_obj_t* name = MakeLabel(row, i == cur ? s_ui.f_title : s_ui.f_body);
        lv_label_set_text(name, m.label.c_str());
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 62, 0);
        lv_obj_t* tag = MakeLabel(row, s_ui.f_small);
        lv_label_set_text(tag, m.vendor == "glm" ? "GLM" : "MiMo");
        lv_obj_align(tag, LV_ALIGN_RIGHT_MID, -20, 0);
        if (i + 1 < n) sep(head_h + (i + 1) * row_h, card_w - 40, 20);
    }

    const lv_coord_t opt_top = head_h + n * row_h + 14;
    sep(opt_top - 7, card_w - 6, 0);
    lv_obj_t* t2 = MakeLabel(card, s_ui.f_small);
    lv_label_set_text_fmt(t2, "%s 的选项", ce.label.c_str());
    lv_obj_set_pos(t2, 20, opt_top + 12);

    // 深度思考
    {
        lv_obj_t* row = add_row(opt_top + head_h);
        const bool forced = ce.ThinkingForced();
        lv_obj_t* l = MakeLabel(row, s_ui.f_body);
        lv_label_set_text(l, forced ? "深度思考（该模型无法关闭）" : "深度思考");
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 20, 0);
        lv_obj_t* sw = MakeSwitch(row, ce.thinking || forced, !forced);
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -20, 0);
        if (!forced) lv_obj_add_event_cb(row, OnToggleThinking, LV_EVENT_CLICKED, nullptr);
    }
    sep(opt_top + head_h + row_h, card_w - 40, 20);
    // 联网搜索
    {
        lv_obj_t* row = add_row(opt_top + head_h + row_h);
        const bool ok = ce.SupportsWebSearch();
        lv_obj_t* l = MakeLabel(row, s_ui.f_body);
        lv_label_set_text(l, !ok ? "联网搜索（不支持）"
                             : (ce.vendor == "glm" ? "联网搜索（套餐内）" : "联网搜索（按次计费）"));
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 20, 0);
        lv_obj_t* sw = MakeSwitch(row, ok && web, ok);
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -20, 0);
        if (ok) lv_obj_add_event_cb(row, OnToggleWeb, LV_EVENT_CLICKED, nullptr);
    }
}

// ---------------------------------------------------------------- 历史对话浮层

void CloseHistory() {
    if (s_ui.history != nullptr) {
        lv_obj_delete(s_ui.history);
        s_ui.history = nullptr;
    }
}

void OpenHistory(int page);

void OnHistoryClose(lv_event_t*) { CloseHistory(); }

void OnHistoryPage(lv_event_t* e) {
    const int delta = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    const int p = s_ui.history_page + delta;
    CloseHistory();
    OpenHistory(p);
}

void LoadHistoryById(const std::string& id);

void FetchThenLoadTask(void* arg) {
    std::unique_ptr<std::string> id(static_cast<std::string*>(arg));
    const bool ok = my_ai::CloudFetch(*id);
    if (!ok) {
        ShowHint("云端取回失败，请检查网络");
    } else {
        auto* again = new std::string(*id);
        if (!ScreenLvAsync([](void* p) {
                std::unique_ptr<std::string> s(static_cast<std::string*>(p));
                if (s_active.load()) LoadHistoryById(*s);
            }, again)) {
            delete again;
        }
    }
    vTaskDeleteWithCaps(nullptr);
}

void LoadHistoryById(const std::string& id) {
    std::vector<my_ai::HistTurn> hist;
    if (!my_ai::LoadConversation(id, hist)) {
        bool cloud = false;
        for (const auto& m : my_ai::ListConversations()) cloud |= (m.id == id && m.cloud_only);
        if (cloud) {
            ShowHint("正在从云端取回…");
            auto* p = new std::string(id);
            if (xTaskCreateWithCaps(FetchThenLoadTask, "my_ai_fetch", 8 * 1024, p, 3, nullptr,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
                delete p;
            }
            return;
        }
        ShowHint("读取失败");
        return;
    }
    CancelCurrentTurn();
    {
        std::lock_guard<std::mutex> lock(S().mu);
        S().turns.clear();
        for (auto& h : hist) {
            Turn t;
            t.id = s_next_turn_id++;
            t.user = std::move(h.user);
            t.reply = std::move(h.reply);
            t.model = std::move(h.model);
            t.note = std::move(h.note);
            t.web_refs = h.web_refs;
            S().turns.push_back(std::move(t));
        }
        S().conv_id = id;
        S().follow_tail = true;
        MarkUrgentLocked();
    }
    Application::GetInstance().Schedule([]() {
        auto& app = Application::GetInstance();
        if (app.GetAudioService().IsStarted()) app.GetAudioService().ResetDecoder();
        if (s_active.load()) app.SetDeviceState(kDeviceStateIdle);
    });
}

struct HistRowData {
    std::string id;
};

void OnHistoryRow(lv_event_t* e) {
    auto* d = static_cast<HistRowData*>(lv_event_get_user_data(e));
    const std::string id = d->id;
    CloseHistory();
    LoadHistoryById(id);
}

void OnHistoryRowLong(lv_event_t* e) {
    auto* d = static_cast<HistRowData*>(lv_event_get_user_data(e));
    const std::string id = d->id;
    my_ai::DeleteConversation(id);
    {
        std::lock_guard<std::mutex> lock(S().mu);
        if (S().conv_id == id) S().conv_id.clear();
    }
    const int p = s_ui.history_page;
    CloseHistory();
    OpenHistory(p);
    ShowHint("已删除");
}

void OnHistoryRowDelete(lv_event_t* e) {
    delete static_cast<HistRowData*>(lv_event_get_user_data(e));
}

std::string FormatTs(int64_t ts) {
    if (ts <= 0) return "时间未知";
    const time_t t = static_cast<time_t>(ts);
    struct tm tmv;
    if (localtime_r(&t, &tmv) == nullptr) return "";
    const time_t now = time(nullptr);
    struct tm nowv;
    localtime_r(&now, &nowv);
    char buf[32];
    if (tmv.tm_year == nowv.tm_year && tmv.tm_yday == nowv.tm_yday) {
        std::snprintf(buf, sizeof(buf), "今天 %02d:%02d", tmv.tm_hour, tmv.tm_min);
    } else if (tmv.tm_year == nowv.tm_year) {
        std::snprintf(buf, sizeof(buf), "%d月%d日 %02d:%02d", tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
                      tmv.tm_min);
    } else {
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    }
    return buf;
}

void OpenHistory(int page) {
    if (s_ui.history != nullptr || s_ui.scr == nullptr) return;
    ClosePicker();
    const auto list = my_ai::ListConversations();
    std::string cur_id;
    {
        std::lock_guard<std::mutex> lock(S().mu);
        cur_id = S().conv_id;
    }

    const lv_coord_t top = lv_obj_get_y(s_ui.model_chip) - (kHeaderH - 40) / 2;  // 状态栏下沿
    lv_obj_t* panel = lv_obj_create(s_ui.scr);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, LV_HOR_RES, LV_VER_RES - top);
    lv_obj_set_pos(panel, 0, top);
    lv_obj_set_style_bg_color(panel, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    s_ui.history = panel;

    // 顶栏：返回 + 标题
    lv_obj_t* back = MakeButton(panel, 96, 44, 22, true);
    lv_obj_set_pos(back, kPad, (kHeaderH - 44) / 2);
    lv_obj_t* bl = MakeLabel(back, s_ui.f_body);
    lv_label_set_text(bl, "返回");
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, OnHistoryClose, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* title = MakeLabel(panel, s_ui.f_title);
    lv_label_set_text(title, "历史对话");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, (kHeaderH - LineH(s_ui.f_title)) / 2);
    lv_obj_t* line = MakeBox(panel);
    lv_obj_set_size(line, LV_HOR_RES, 2);
    lv_obj_set_pos(line, 0, kHeaderH);
    lv_obj_set_style_bg_color(line, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);

    const lv_coord_t list_top = kHeaderH + 2;
    const lv_coord_t foot_h = 56;
    const lv_coord_t row_h = 84;
    const lv_coord_t panel_h = LV_VER_RES - top;
    const int per_page = std::max<int>(1, (panel_h - list_top - foot_h) / row_h);
    const int pages = std::max<int>(1, (static_cast<int>(list.size()) + per_page - 1) / per_page);
    page = std::max(0, std::min(page, pages - 1));
    s_ui.history_page = page;

    if (list.empty()) {
        lv_obj_t* e = MakeLabel(panel, s_ui.f_body);
        lv_label_set_text(e, "还没有历史对话");
        lv_obj_align(e, LV_ALIGN_TOP_MID, 0, list_top + 120);
    }
    const int b = page * per_page;
    const int e = std::min<int>(static_cast<int>(list.size()), b + per_page);
    for (int i = b; i < e; ++i) {
        const auto& m = list[i];
        const lv_coord_t y = list_top + static_cast<lv_coord_t>(i - b) * row_h;
        lv_obj_t* row = lv_obj_create(panel);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_HOR_RES, row_h);
        lv_obj_set_pos(row, 0, y);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        HapticAttachClick(row);
        auto* d = new HistRowData{m.id};
        lv_obj_add_event_cb(row, OnHistoryRow, LV_EVENT_SHORT_CLICKED, d);
        lv_obj_add_event_cb(row, OnHistoryRowLong, LV_EVENT_LONG_PRESSED, d);
        lv_obj_add_event_cb(row, OnHistoryRowDelete, LV_EVENT_DELETE, d);

        const bool is_cur = m.id == cur_id;
        lv_obj_t* t = MakeLabel(row, is_cur ? s_ui.f_title : s_ui.f_body);
        lv_obj_set_size(t, LV_HOR_RES - kPad * 2, LineH(t == nullptr ? s_ui.f_body : (is_cur ? s_ui.f_title : s_ui.f_body)));
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_label_set_text(t, m.cloud_only ? ("[云] " + m.title).c_str() : m.title.c_str());
        lv_obj_set_pos(t, kPad, 10);
        lv_obj_t* sub = MakeLabel(row, s_ui.f_small);
        std::string st = FormatTs(m.ts) + " · " + std::to_string(m.turns) + " 轮";
        if (is_cur) st += " · 当前";
        lv_label_set_text(sub, st.c_str());
        lv_obj_set_pos(sub, kPad, 10 + LineH(s_ui.f_body) + 4);
        lv_obj_t* sl = MakeBox(row);
        lv_obj_set_size(sl, LV_HOR_RES - kPad * 2, 1);
        lv_obj_set_pos(sl, kPad, row_h - 1);
        lv_obj_set_style_bg_color(sl, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, 0);
    }

    // 底部：翻页 + 提示
    const lv_coord_t fy = panel_h - foot_h;
    lv_obj_t* tip = MakeLabel(panel, s_ui.f_small);
    if (pages > 1) {
        lv_label_set_text_fmt(tip, "%d / %d", page + 1, pages);
    } else {
        lv_label_set_text(tip, list.empty() ? "" : "长按一条可删除");
    }
    lv_obj_align(tip, LV_ALIGN_TOP_MID, 0, fy + (foot_h - LineH(s_ui.f_small)) / 2);
    if (page > 0) {
        lv_obj_t* pb = MakeButton(panel, 110, 44, 22, true);
        lv_obj_set_pos(pb, kPad, fy + (foot_h - 44) / 2);
        lv_obj_t* l = MakeLabel(pb, s_ui.f_small);
        lv_label_set_text(l, "上一页");
        lv_obj_center(l);
        lv_obj_add_event_cb(pb, OnHistoryPage, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(-1)));
    }
    if (page < pages - 1) {
        lv_obj_t* nb = MakeButton(panel, 110, 44, 22, true);
        lv_obj_set_pos(nb, LV_HOR_RES - kPad - 110, fy + (foot_h - 44) / 2);
        lv_obj_t* l = MakeLabel(nb, s_ui.f_small);
        lv_label_set_text(l, "下一页");
        lv_obj_center(l);
        lv_obj_add_event_cb(nb, OnHistoryPage, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(1)));
    }
}

void OnHistoryBtn(lv_event_t*) { OpenHistory(0); }

void OnModelChip(lv_event_t*) { OpenPicker(); }
void OnNewChat(lv_event_t*) { NewChat(); }
void OnPagerPrev(lv_event_t*) { TurnPage(-1); }
void OnPagerNext(lv_event_t*) { TurnPage(1); }

// ---------------------------------------------------------------- 按键

bool OnVkKey(const char* key) {
    if (key == nullptr) return false;
    if (s_ui.picker != nullptr) {
        ClosePicker();
        return true;
    }
    if (s_ui.history != nullptr) {
        if (std::strcmp(key, "vk_next") == 0 || std::strcmp(key, "vk_prev") == 0) {
            const int p = s_ui.history_page + (std::strcmp(key, "vk_next") == 0 ? 1 : -1);
            CloseHistory();
            OpenHistory(p);
        } else {
            CloseHistory();
        }
        return true;
    }
    if (std::strcmp(key, "vk_next") == 0) {
        TurnPage(1);
        return true;
    }
    if (std::strcmp(key, "vk_prev") == 0) {
        int page;
        {
            std::lock_guard<std::mutex> lock(S().mu);
            page = S().page;
        }
        if (page > 0) {
            TurnPage(-1);
            return true;
        }
        return false;  // 第一页再按：返回上一屏
    }
    return false;  // vk_home 走默认
}

bool OnBootLongPress() {
    PttStart(s_boot_ptt);
    return true;
}

bool OnBootPressUp() {
    PttStop(s_boot_ptt);
    return true;
}

bool OnBootPressDown() { return true; }

bool OnBootClick() {
    StopAll();
    return true;
}

// ---------------------------------------------------------------- 生命周期

void OverlayCloseQuiet();

void OnLifecycle(lv_event_t* e) {
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SCREEN_LOADED) {
        s_active.store(true);
        if (auto* disp = LVAdapterDisplay::Instance()) {
            disp->SetStatusTitlePrefix(nullptr);
            disp->SetIdleStatusMode(IdleStatusMode::kClock);
            disp->UpdateStatusBar(true);
        }
        Application::GetInstance().Schedule([]() {
            if (s_active.load()) Application::GetInstance().EnsureAudioServiceRunning();
        });
        if (s_listen_on_load.exchange(false) && BootKey_IsHeld()) {
            PttStart(s_boot_ptt);
        }
        OverlayCloseQuiet();
        MarkDirty();
    } else if (code == LV_EVENT_SCREEN_UNLOADED) {
        // 新页 LOADED 先于旧页 UNLOADED；若本页被自身新实例替换，不要清新实例的状态
        if (lv_event_get_target(e) != s_ui.scr) {
            return;
        }
        s_active.store(false);
        s_boot_ptt.store(false);
        s_touch_ptt.store(false);
        if (!s_overlay.load()) CancelCurrentTurn();
        if (s_ui.render_timer) lv_timer_delete(s_ui.render_timer);
        if (s_ui.mic_timer) lv_timer_delete(s_ui.mic_timer);
        s_ui = Ui{};
        Application::GetInstance().Schedule([]() {
            {
                std::lock_guard<std::mutex> lock(s_rec_mu);
                s_recording = false;
                s_rec.clear();
                s_rec.shrink_to_fit();
            }
            auto& app = Application::GetInstance();
            if (app.IsXiaozhiVoiceActive()) return;  // 已切到百问，别动它的音频
            app.GetAudioService().SetPcmCaptureSink(nullptr);
            app.StopAudioServiceIfRunning();
            app.SetDeviceState(kDeviceStateIdle);
        });
        ESP_LOGI(TAG, "leave");
    }
}

lv_obj_t* MakeButton(lv_obj_t* parent, lv_coord_t w, lv_coord_t h, lv_coord_t radius, bool border) {
    lv_obj_t* b = MakeBox(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, radius, 0);
    lv_obj_set_style_bg_color(b, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    if (border) {
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_set_style_border_color(b, lv_color_black(), 0);
    }
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_GESTURE_BUBBLE);
    HapticAttachClick(b);
    return b;
}

lv_obj_t* HLine(lv_obj_t* parent, lv_coord_t y) {
    lv_obj_t* l = MakeBox(parent);
    lv_obj_set_size(l, LV_HOR_RES, 2);
    lv_obj_set_pos(l, 0, y);
    lv_obj_set_style_bg_color(l, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    return l;
}

}  // namespace

bool MyAiScreen::IsActive() { return s_active.load(std::memory_order_acquire); }

void MyAiScreen::DebugOpenPicker() {
    if (s_active.load()) OpenPicker();
}

void MyAiScreen::DebugAskText(const char* text) {
    if (EngineOn() && text && text[0]) AskPreset(text);
}

void MyAiScreen::DebugSelectModel(int idx) {
    if (s_active.load()) SelectModel(idx);
}

void MyAiScreen::DebugLoadHistory(int idx) {
    if (!s_active.load()) return;
    const auto list = my_ai::ListConversations();
    if (idx >= 0 && idx < static_cast<int>(list.size())) LoadHistoryById(list[idx].id);
}

void MyAiScreen::DebugOpenHistory() {
    if (s_active.load()) OpenHistory(0);
}

void MyAiScreen::DebugAsk(int idx) {
    constexpr int n = static_cast<int>(sizeof(kSuggestions) / sizeof(kSuggestions[0]));
    if (s_active.load() && idx >= 0 && idx < n) AskPreset(kSuggestions[idx]);
}

lv_obj_t* MyAiScreen::Create() {
    ESP_LOGI(TAG, "launch MyAiScreen");
    my_ai::CloudSyncAsync();
    {
        my_ai::Config cfg = my_ai::LoadConfig();
        std::lock_guard<std::mutex> lock(S().mu);
        // 保留会话里手动切换过的模型
        S().cfg = std::move(cfg);
        ApplyPrefsLocked();  // 上次选的模型、思考 / 联网开关
        S().follow_tail = true;
        S().dirty = true;
    }

    s_ui = Ui{};
    s_ui.f_body = fontpack_lv_font_ui();
    // fontpack 的 18 号只有拉丁字符，中文小字也用 25 号
    s_ui.f_small = s_ui.f_body;
    s_ui.f_title = fontpack_lv_font_get(30, 4);
    if (s_ui.f_title == nullptr) s_ui.f_title = s_ui.f_body;

    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    EpdStatusBar status = ScreenCreateStatusBar(scr);
    const lv_coord_t top = status.height;

    // ---- 顶栏：历史 · 模型（居中，点开选择）· 新对话
    lv_obj_t* hist_btn = MakeButton(scr, 44, 44, LV_RADIUS_CIRCLE, true);
    lv_obj_set_pos(hist_btn, kPad, top + (kHeaderH - 44) / 2);
    for (int i = 0; i < 3; ++i) {  // 三道横线
        lv_obj_t* bar = MakeBox(hist_btn);
        lv_obj_set_size(bar, 20, 3);
        lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_align(bar, LV_ALIGN_CENTER, 0, (i - 1) * 7);
    }
    lv_obj_add_event_cb(hist_btn, OnHistoryBtn, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* new_btn = MakeButton(scr, 44, 44, LV_RADIUS_CIRCLE, true);
    lv_obj_set_pos(new_btn, LV_HOR_RES - kPad - 44, top + (kHeaderH - 44) / 2);
    lv_obj_t* plus = MakeLabel(new_btn, s_ui.f_title);
    lv_label_set_text(plus, "+");
    lv_obj_center(plus);
    lv_obj_add_event_cb(new_btn, OnNewChat, LV_EVENT_CLICKED, nullptr);

    const lv_coord_t chip_w = LV_HOR_RES - kPad * 2 - 44 * 2 - 24;
    s_ui.model_chip = MakeButton(scr, chip_w, 44, 22, true);
    lv_obj_set_pos(s_ui.model_chip, (LV_HOR_RES - chip_w) / 2, top + (kHeaderH - 44) / 2);
    s_ui.model_label = MakeLabel(s_ui.model_chip, s_ui.f_small);
    // 固定一行高度：LONG_DOT 只有在高度固定时才截断成省略号，否则会折成两行
    lv_obj_set_size(s_ui.model_label, chip_w - 24, LineH(s_ui.f_small));
    lv_label_set_long_mode(s_ui.model_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_ui.model_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_ui.model_label);
    lv_obj_add_event_cb(s_ui.model_chip, OnModelChip, LV_EVENT_CLICKED, nullptr);

    HLine(scr, top + kHeaderH);

    // ---- 消息区
    const lv_coord_t area_top = top + kHeaderH + 2 + 12;
    const lv_coord_t input_top = LV_VER_RES - kInputH;
    const lv_coord_t pager_top = input_top - kPagerH;
    s_ui.area_w = LV_HOR_RES - kPad * 2;
    s_ui.area_h = pager_top - area_top - 4;
    s_ui.msg_area = MakeBox(scr);
    lv_obj_set_size(s_ui.msg_area, s_ui.area_w, s_ui.area_h);
    lv_obj_set_pos(s_ui.msg_area, kPad, area_top);

    // ---- 翻页栏
    s_ui.pager = MakeBox(scr);
    lv_obj_set_size(s_ui.pager, LV_HOR_RES, kPagerH);
    lv_obj_set_pos(s_ui.pager, 0, pager_top);
    s_ui.pager_label = MakeLabel(s_ui.pager, s_ui.f_small);
    lv_obj_center(s_ui.pager_label);
    s_ui.pager_prev = MakeButton(s_ui.pager, 110, kPagerH, 8, false);
    lv_obj_set_pos(s_ui.pager_prev, kPad, 0);
    lv_obj_t* pl = MakeLabel(s_ui.pager_prev, s_ui.f_small);
    lv_label_set_text(pl, "上一页");
    lv_obj_align(pl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_event_cb(s_ui.pager_prev, OnPagerPrev, LV_EVENT_CLICKED, nullptr);
    s_ui.pager_next = MakeButton(s_ui.pager, 110, kPagerH, 8, false);
    lv_obj_set_pos(s_ui.pager_next, LV_HOR_RES - kPad - 110, 0);
    lv_obj_t* nl = MakeLabel(s_ui.pager_next, s_ui.f_small);
    lv_label_set_text(nl, "下一页");
    lv_obj_align(nl, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(s_ui.pager_next, OnPagerNext, LV_EVENT_CLICKED, nullptr);

    // ---- 输入栏：按住说话
    HLine(scr, input_top);
    s_ui.pill = MakeButton(scr, LV_HOR_RES - kPad * 2, kPillH, kPillH / 2, true);
    lv_obj_set_pos(s_ui.pill, kPad, input_top + (kInputH - kPillH) / 2 + 1);
    lv_obj_set_flex_flow(s_ui.pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_ui.pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_ui.pill, 14, 0);
    // 麦克风：胶囊 + 底座
    s_ui.mic = MakeBox(s_ui.pill);
    lv_obj_set_size(s_ui.mic, 14, 26);
    lv_obj_set_style_radius(s_ui.mic, 7, 0);
    lv_obj_set_style_bg_color(s_ui.mic, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ui.mic, LV_OPA_COVER, 0);
    s_ui.pill_label = MakeLabel(s_ui.pill, s_ui.f_body);
    lv_obj_add_event_cb(s_ui.pill, OnPillEvent, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(s_ui.pill, OnPillEvent, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(s_ui.pill, OnPillEvent, LV_EVENT_PRESS_LOST, nullptr);

    lv_obj_move_foreground(status.bar);
    lv_obj_move_foreground(status.overlay);

    s_ui.scr = scr;
    s_ui.render_timer = lv_timer_create(OnRenderTimer, kRenderPeriodMs, nullptr);
    Render();

    ScreenSetIsHome(false);
    VkKeyScreenDesc desc{};
    desc.factory = MyAiScreen::Create;
    desc.on_key = OnVkKey;
    desc.on_boot_click = OnBootClick;
    desc.on_boot_long_press = OnBootLongPress;
    desc.on_boot_press_down = OnBootPressDown;
    desc.on_boot_press_up = OnBootPressUp;
    VkKey_AttachScreen(scr, kScreenName, desc);
    lv_obj_add_event_cb(scr, OnLifecycle, LV_EVENT_SCREEN_LOADED, nullptr);
    lv_obj_add_event_cb(scr, OnLifecycle, LV_EVENT_SCREEN_UNLOADED, nullptr);
    return scr;
}

// ---------------------------------------------------------------- 全局入口：BOOT 短按带截图 / 长按直接听

namespace {

/** 面板帧（1bpp，1=白，MSB 在左）→ 竖屏灰度 → JPEG → data URL */
std::string EncodeShot(const std::vector<uint8_t>& fb, int w, int h, int orient) {
    // 与 tools/screen_shot.py 一致：orient 0 逆时针转 270°（= 顺时针 90°），1 不转，2 转 180°
    const bool cw90 = orient == 0;
    const bool r180 = orient == 2;
    const int ow = cw90 ? h : w;
    const int oh = cw90 ? w : h;
    const int stride = (w + 7) / 8;
    auto* gray = static_cast<uint8_t*>(heap_caps_malloc(static_cast<size_t>(ow) * oh, MALLOC_CAP_SPIRAM));
    if (gray == nullptr) return {};
    for (int y = 0; y < oh; ++y) {
        for (int x = 0; x < ow; ++x) {
            int sx, sy;
            if (cw90) {
                sx = y;
                sy = h - 1 - x;
            } else if (r180) {
                sx = w - 1 - x;
                sy = h - 1 - y;
            } else {
                sx = x;
                sy = y;
            }
            const bool white = fb[sy * stride + sx / 8] & (0x80 >> (sx % 8));
            gray[y * ow + x] = white ? 255 : 0;
        }
    }
    uint8_t* jpg = nullptr;
    size_t jpg_len = 0;
    const int64_t t_enc = esp_timer_get_time();
    const bool ok = image_to_jpeg(gray, static_cast<size_t>(ow) * oh, ow, oh, V4L2_PIX_FMT_GREY, 70, &jpg, &jpg_len);
    const int enc_ms = static_cast<int>((esp_timer_get_time() - t_enc) / 1000);
    heap_caps_free(gray);
    if (!ok || jpg == nullptr) return {};
    std::string out = "data:image/jpeg;base64,";
    size_t b64_len = 0;
    mbedtls_base64_encode(nullptr, 0, &b64_len, jpg, jpg_len);
    const size_t head = out.size();
    out.resize(head + b64_len);
    size_t written = 0;
    mbedtls_base64_encode(reinterpret_cast<unsigned char*>(&out[head]), b64_len, &written, jpg, jpg_len);
    out.resize(head + written);
    free(jpg);
    ESP_LOGI(TAG, "screenshot %dx%d -> jpeg %u B (%dms)", ow, oh, static_cast<unsigned>(jpg_len), enc_ms);
    return out;
}

struct ShotJob {
    std::vector<uint8_t> fb;
    int w = 0, h = 0, orient = 0;
};

void ShotTask(void* arg) {
    std::unique_ptr<ShotJob> job(static_cast<ShotJob*>(arg));
    std::string url = EncodeShot(job->fb, job->w, job->h, job->orient);
    {
        std::lock_guard<std::mutex> lock(S().mu);
        S().image_encoding = false;
        S().pending_image = std::move(url);
        S().dirty = true;
    }
    job.reset();
    ShowHint("已附上刚才页面的截图，按住说话提问");
    vTaskDeleteWithCaps(nullptr);
}

void ShotTaskQuiet(void* arg) {
    std::unique_ptr<ShotJob> job(static_cast<ShotJob*>(arg));
    std::string url = EncodeShot(job->fb, job->w, job->h, job->orient);
    {
        std::lock_guard<std::mutex> lock(S().mu);
        S().image_encoding = false;
        S().pending_image = std::move(url);
        S().dirty = true;
    }
    job.reset();
    vTaskDeleteWithCaps(nullptr);
}

void NavigateToMyAi(void*) {
    if (!s_active.load()) ScreenNavigateTo(MyAiScreen::Create);
}

}  // namespace

void MyAiScreen::RequestOpenWithShot() {
    auto* disp = LVAdapterDisplay::Instance();
    auto* job = new ShotJob();
    if (disp == nullptr || !disp->CopyPanelFrame(job->fb, &job->w, &job->h)) {
        delete job;
        ScreenLvAsync(NavigateToMyAi);
        return;
    }
    job->orient = DisplayUiGetOrient();
    {
        std::lock_guard<std::mutex> lock(S().mu);
        S().image_encoding = true;
        S().pending_image.clear();
    }
    if (xTaskCreateWithCaps(ShotTask, "my_ai_shot", 8 * 1024, job, 3, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        delete job;
        std::lock_guard<std::mutex> lock(S().mu);
        S().image_encoding = false;
    }
    ScreenLvAsync(NavigateToMyAi);
}

void MyAiScreen::RequestOpenListen() {
    if (s_active.load()) {
        if (BootKey_IsHeld()) PttStart(s_boot_ptt);
        return;
    }
    s_listen_on_load.store(true);
    ScreenLvAsync(NavigateToMyAi);
}

// ---------------------------------------------------------------- 浮层：任意页面叠一张底部对话卡片

namespace {

constexpr lv_coord_t kOvCardH = 470;
constexpr int64_t kOvNewChatUs = 10LL * 60 * 1000000;  // 距上次浮层对话超过 10 分钟：开新对话

struct Ov {
    lv_obj_t* mask = nullptr;
    lv_obj_t* card = nullptr;
    lv_obj_t* phase = nullptr;
    lv_obj_t* body = nullptr;
    lv_obj_t* pill = nullptr;
    lv_obj_t* pill_label = nullptr;
    lv_timer_t* timer = nullptr;
    std::string last_body, last_phase;
    lv_coord_t body_w = 0, body_h = 0;
    const lv_font_t* font = nullptr;
};
Ov s_ov;
int64_t s_ov_last_us = 0;
std::atomic<bool> s_ov_listen{false};  // 打开后若 BOOT 仍按着就开录

std::string OvPhaseText(Phase ph, bool has_shot, const std::string& hint) {
    if (!hint.empty() && ph == Phase::Idle) return hint;
    switch (ph) {
        case Phase::Recording: return "正在听…  松开 BOOT 发送";
        case Phase::Network: return "连接网络中…";
        case Phase::Asr: return "识别中…";
        case Phase::Thinking: return "思考中…";
        case Phase::Answering: return "回答中…";
        case Phase::Speaking: return "朗读中…  短按 BOOT 停止";
        default: return has_shot ? "长按 BOOT 说话（会带上当前页面）" : "长按 BOOT 说话";
    }
}

/** 只显示末尾放得下的几行（长回答在「继续对话」里看全文） */
std::string OvTail(const std::string& text) {
    std::vector<std::string> lines;
    std::vector<lv_coord_t> widths;
    Wrap(text, s_ov.font, s_ov.body_w, lines, widths);
    const int max_lines = std::max(1, static_cast<int>(s_ov.body_h / (s_ov.font->line_height + 4)));
    const size_t from = lines.size() > static_cast<size_t>(max_lines) ? lines.size() - max_lines : 0;
    std::string out = from > 0 ? "…" : "";
    for (size_t i = from; i < lines.size(); ++i) {
        out += lines[i];
        if (i + 1 < lines.size()) out += "\n";
    }
    return out;
}

void OvRender(lv_timer_t*) {
    if (s_ov.card == nullptr) return;
    Phase ph;
    std::string hint, user, reply, searching;
    bool has_shot, asr_pending = false, streaming = false, have_turn = false;
    size_t think = 0;
    {
        std::lock_guard<std::mutex> lock(S().mu);
        ph = S().phase;
        hint = S().hint;
        has_shot = !S().pending_image.empty() || S().image_encoding;
        if (!S().turns.empty() && s_ov_last_us > 0) {
            const Turn& t = S().turns.back();
            have_turn = true;
            user = t.user;
            reply = t.reply;
            searching = t.searching;
            asr_pending = t.asr_pending;
            streaming = t.streaming;
            think = t.think_bytes;
            if (!t.note.empty() && reply.empty()) reply = t.note;
        }
    }
    std::string body;
    if (have_turn) {
        body = "我：" + (asr_pending ? std::string("识别中…") : user) + "\n\n";
        if (!reply.empty()) {
            body += reply + (streaming ? " …" : "");
        } else if (!searching.empty()) {
            body += "正在搜索：" + searching + "…";
        } else if (streaming) {
            body += think > 0 ? "思考中… 已想 " + std::to_string(think / 3) + " 字" : "思考中…";
        }
    }
    body = OvTail(body);
    if (body != s_ov.last_body) {
        s_ov.last_body = body;
        lv_label_set_text(s_ov.body, body.c_str());
    }
    const std::string pt = OvPhaseText(ph, has_shot, hint);
    if (pt != s_ov.last_phase) {
        s_ov.last_phase = pt;
        lv_label_set_text(s_ov.phase, pt.c_str());
        const bool rec = ph == Phase::Recording;
        lv_obj_set_style_bg_color(s_ov.pill, rec ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_text_color(s_ov.pill_label, rec ? lv_color_white() : lv_color_black(), 0);
        lv_label_set_text(s_ov.pill_label, rec ? "松开 发送" : "按住 说话");
    }
}

void OvDestroy() {
    if (s_ov.timer) lv_timer_delete(s_ov.timer);
    if (s_ov.mask) lv_obj_delete(s_ov.mask);
    s_ov = Ov{};
}

void OvStopAudio() {
    Application::GetInstance().Schedule([]() {
        if (s_active.load() || s_overlay.load()) return;
        {
            std::lock_guard<std::mutex> lock(s_rec_mu);
            s_recording = false;
            s_rec.clear();
            s_rec.shrink_to_fit();
        }
        auto& app = Application::GetInstance();
        if (app.IsXiaozhiVoiceActive()) return;
        app.GetAudioService().SetPcmCaptureSink(nullptr);
        app.StopAudioServiceIfRunning();
        app.SetDeviceState(kDeviceStateIdle);
    });
}

/** 关闭浮层（停掉对话和朗读）；须在 LVGL 线程 */
void OvClose() {
    if (!s_overlay.load() && s_ov.card == nullptr) return;
    s_overlay.store(false);
    s_boot_ptt.store(false);
    s_touch_ptt.store(false);
    if (!s_active.load()) CancelCurrentTurn();
    {
        std::lock_guard<std::mutex> lock(S().mu);
        S().pending_image.clear();
    }
    OvDestroy();
    OvStopAudio();
}

void OnOvMask(lv_event_t* e) {
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) OvClose();
}

void OnOvClose(lv_event_t*) { OvClose(); }

void OnOvContinue(lv_event_t*) {
    // 去我的AI页面看全文 / 接着聊；页面载入后再静默收起浮层，不打断进行中的回答
    ScreenNavigateTo(MyAiScreen::Create);
}

void OnOvPill(lv_event_t* e) {
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        PttStart(s_touch_ptt);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        PttStop(s_touch_ptt);
    }
}

lv_obj_t* OvLabel(lv_obj_t* parent, const char* text) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, s_ov.font, 0);
    lv_obj_set_style_text_color(l, lv_color_black(), 0);
    lv_label_set_text(l, text);
    return l;
}

lv_obj_t* OvButton(lv_obj_t* parent, const char* text, lv_coord_t w, lv_event_cb_t cb) {
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, 48);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, lv_color_black(), 0);
    lv_obj_set_style_radius(b, 24, 0);
    lv_obj_set_style_bg_color(b, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_center(OvLabel(b, text));
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    HapticAttachClick(b);
    return b;
}

void OvBuild() {
    s_ov.font = fontpack_lv_font_ui();
    lv_obj_t* top = lv_layer_top();
    s_ov.mask = lv_obj_create(top);
    lv_obj_remove_style_all(s_ov.mask);
    lv_obj_set_size(s_ov.mask, LV_HOR_RES, LV_VER_RES);
    lv_obj_add_flag(s_ov.mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_ov.mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_ov.mask, OnOvMask, LV_EVENT_CLICKED, nullptr);

    s_ov.card = lv_obj_create(s_ov.mask);
    lv_obj_remove_style_all(s_ov.card);
    lv_obj_set_size(s_ov.card, LV_HOR_RES, kOvCardH);
    lv_obj_align(s_ov.card, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_ov.card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_ov.card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(s_ov.card, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(s_ov.card, 3, 0);
    lv_obj_set_style_border_color(s_ov.card, lv_color_black(), 0);
    lv_obj_set_style_radius(s_ov.card, 0, 0);
    lv_obj_add_flag(s_ov.card, LV_OBJ_FLAG_CLICKABLE);  // 吃掉点击，避免穿透到下层/遮罩
    lv_obj_clear_flag(s_ov.card, LV_OBJ_FLAG_SCROLLABLE);

    constexpr lv_coord_t pad = 20;
    lv_obj_t* title = lv_label_create(s_ov.card);
    const lv_font_t* tf = fontpack_lv_font_get(30, 4);
    lv_obj_set_style_text_font(title, tf ? tf : s_ov.font, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_label_set_text(title, "我的AI");
    lv_obj_set_pos(title, pad, 14);
    lv_obj_t* close = OvButton(s_ov.card, "关闭", 96, OnOvClose);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -pad, 10);
    lv_obj_t* cont = OvButton(s_ov.card, "继续对话", 140, OnOvContinue);
    lv_obj_align_to(cont, close, LV_ALIGN_OUT_LEFT_MID, -12, 0);

    s_ov.phase = OvLabel(s_ov.card, "");
    lv_obj_set_width(s_ov.phase, LV_HOR_RES - pad * 2);
    lv_obj_set_pos(s_ov.phase, pad, 70);

    s_ov.body_w = LV_HOR_RES - pad * 2;
    s_ov.body_h = kOvCardH - 70 - s_ov.font->line_height - 16 - 76;
    s_ov.body = OvLabel(s_ov.card, "");
    lv_obj_set_size(s_ov.body, s_ov.body_w, s_ov.body_h);
    lv_obj_set_style_text_line_space(s_ov.body, 4, 0);
    lv_label_set_long_mode(s_ov.body, LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(s_ov.body, pad, 70 + s_ov.font->line_height + 12);

    s_ov.pill = lv_button_create(s_ov.card);
    lv_obj_remove_style_all(s_ov.pill);
    lv_obj_set_size(s_ov.pill, LV_HOR_RES - pad * 2, 56);
    lv_obj_align(s_ov.pill, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_border_width(s_ov.pill, 2, 0);
    lv_obj_set_style_border_color(s_ov.pill, lv_color_black(), 0);
    lv_obj_set_style_radius(s_ov.pill, 28, 0);
    lv_obj_set_style_bg_color(s_ov.pill, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_ov.pill, LV_OPA_COVER, 0);
    s_ov.pill_label = OvLabel(s_ov.pill, "按住 说话");
    lv_obj_center(s_ov.pill_label);
    lv_obj_add_event_cb(s_ov.pill, OnOvPill, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(s_ov.pill, OnOvPill, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(s_ov.pill, OnOvPill, LV_EVENT_PRESS_LOST, nullptr);
    HapticAttachClick(s_ov.pill);

    s_ov.timer = lv_timer_create(OvRender, kRenderPeriodMs, nullptr);
    OvRender(nullptr);
}

/** 打开浮层（须在 LVGL 线程） */
void OvOpenOnLv(void*) {
    if (s_active.load()) {  // 已经在我的AI页面：不叠浮层
        if (s_ov_listen.exchange(false) && BootKey_IsHeld()) PttStart(s_boot_ptt);
        return;
    }
    const bool fresh = s_ov.card == nullptr;
    if (fresh) {
        // 配置（页面没打开过时还没加载）
        {
            my_ai::Config cfg = my_ai::LoadConfig();
            std::lock_guard<std::mutex> lock(S().mu);
            S().cfg = std::move(cfg);
            ApplyPrefsLocked();
            // 隔太久就开新对话，免得把不相干的上下文带进来
            const int64_t now = esp_timer_get_time();
            if (s_ov_last_us == 0 || now - s_ov_last_us > kOvNewChatUs) {
                S().turns.clear();
                S().conv_id.clear();
            }
            s_ov_last_us = now;
            S().hint.clear();
        }
        s_overlay.store(true);
        OvBuild();
        Application::GetInstance().Schedule([]() {
            if (EngineOn()) Application::GetInstance().EnsureAudioServiceRunning();
        });
    }
    if (s_ov_listen.exchange(false) && BootKey_IsHeld()) PttStart(s_boot_ptt);
}

void OverlayCloseQuiet() {
    // 我的AI 页面接管：引擎继续跑（s_active 已置位），只拆浮层界面
    if (s_ov.card == nullptr && !s_overlay.load()) return;
    s_overlay.store(false);
    OvDestroy();
}

}  // namespace

/** @brief 截图后打开浮层；listen=true 且 BOOT 还按着就直接开录（任意线程） */
void MyAiOverlayOpen(bool with_shot, bool listen) {
    if (with_shot && !s_active.load()) {
        auto* disp = LVAdapterDisplay::Instance();
        auto* job = new ShotJob();
        if (disp != nullptr && disp->CopyPanelFrame(job->fb, &job->w, &job->h)) {
            job->orient = DisplayUiGetOrient();
            {
                std::lock_guard<std::mutex> lock(S().mu);
                S().image_encoding = true;
                S().pending_image.clear();
            }
            if (xTaskCreateWithCaps(ShotTaskQuiet, "my_ai_shot", 8 * 1024, job, 3, nullptr,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
                delete job;
                std::lock_guard<std::mutex> lock(S().mu);
                S().image_encoding = false;
            }
        } else {
            delete job;
        }
    }
    s_ov_listen.store(listen);
    ScreenLvAsync(OvOpenOnLv);
}

bool MyAiScreen::OverlayOpen() { return s_overlay.load(); }

void MyAiScreen::RequestOverlay(bool listen) { MyAiOverlayOpen(true, listen); }

void MyAiScreen::RequestOverlayNoShot(bool listen) { MyAiOverlayOpen(false, listen); }

bool MyAiScreen::OverlayBootClick() {
    if (!s_overlay.load()) return false;
    // 正在回答/朗读：短按先停；空闲：关闭浮层
    if (IsBusy() || Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking) {
        StopAll();
    } else {
        ScreenLvAsync([](void*) { OvClose(); });
    }
    return true;
}

bool MyAiScreen::OverlayBootLongPress() {
    if (!s_overlay.load()) return false;
    PttStart(s_boot_ptt);
    return true;
}

bool MyAiScreen::OverlayBootPressUp() {
    if (!s_overlay.load()) return false;
    PttStop(s_boot_ptt);
    return true;
}

void MyAiScreen::RequestOverlayClose() { ScreenLvAsync([](void*) { OvClose(); }); }
