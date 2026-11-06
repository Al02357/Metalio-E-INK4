#include "weread_screen.h"
#include "src/draw/lv_image_decoder_private.h"
#include "src/draw/lv_draw_buf_private.h"
#include "ecs_power.h"
#include "net_mode.h"
#include "standby_screen/standby_card.h"
#include "standby_screen/standby_screen.h"
#include <esp_wifi.h>
#include "miniz.h"
#include <esp_netif.h>

#include "board.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "lv_adapter_display.h"
#include "power_policy.h"
#include "screen_common.h"
#include "sd_paths.h"
#include "vk_key_handler.h"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <deque>
#include <dirent.h>
#include <set>
#include <vector>

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_rom_crc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

namespace {

constexpr const char* TAG = "WeRead";
constexpr const char* kScreenName = "weread";
constexpr const char* kCfgPath = SD_APP_ROOT "/weread.json";
constexpr const char* kCoverDir = SD_APP_ROOT "/weread_covers";
constexpr int kImgW = 480;
constexpr int kMaxImgH = 800;
constexpr int kPad = 20;
constexpr int kCoverW = 112;
constexpr int kCoverH = 156;
constexpr int kCoverBytes = (kCoverW / 8) * kCoverH;
constexpr int kCols = 3;
constexpr int kPerPage = 9;
constexpr int kCellW = 160;
constexpr int kCellH = 204;
constexpr int kHeaderH = 56;
constexpr int kPagerH = 48;
constexpr int kListRowH = 54;
constexpr int kMenuH = 366;
constexpr int kHttpTimeoutMs = 60000;

using JsonPtr = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

struct Cfg {
    std::string server;  // http://IP:端口
    std::string token;
};

struct Book {
    std::string id;
    std::string title;
    int progress = -1;
};

struct Chapter {
    int uid = 0;
    std::string title;
    int level = 1;
};

struct TextItem {
    std::string head;  // 小标题（章节 / 人数），可空
    std::string body;
};

enum class Mode { Shelf, Login, Reading, Popup, Chapters, Texts, Accounts };

struct Account {
    std::string id, name;
    bool has_key = false;
};

// 跨线程共享（mu 保护）：后台任务写，LVGL 线程在 ApplyUi 里取
struct Shared {
    std::mutex mu;
    Cfg cfg;
    Mode mode = Mode::Shelf;
    std::string hint;                          // 书架 / 登录页说明文字
    std::string toast;                         // 底部浮条
    int64_t toast_until_us = 0;                // 0 = 一直显示（请求进行中）
    std::string img;                           // 待上屏的 1bpp 图（1=黑）
    int img_w = 0, img_h = 0;
    bool img_pending = false;
    std::vector<Book> books;
    bool books_dirty = false;
    bool cache_dirty = false;   // 只是缓存进度变了：原地改角标
    std::map<std::string, std::string> covers;  // bookId -> 1bpp 封面
    bool covers_dirty = false;
    std::string book_id, book_title;           // 正在读的书
    int font = 3;
    int face = 0;                              // 网页版字体序号
    bool list_fonts = false;                   // 目录列表此刻显示的是字体
    bool thoughts = true;                      // 书友想法（正文虚线）
    std::vector<Chapter> chapters;
    int cur_uid = 0;
    bool chapters_dirty = false;
    std::string texts_title;
    std::vector<TextItem> texts;
    bool texts_dirty = false;
    std::vector<Account> accounts;
    std::string cur_acc;
    bool accounts_dirty = false;
};

Shared& S() {
    static Shared s;
    return s;
}

std::atomic<bool> s_active{false};
std::atomic<bool> s_busy{false};
std::atomic<bool> s_cover_busy{false};
std::atomic<bool> s_local_busy{false};  // 不联网的本地任务（离线打开）
std::atomic<bool> s_opening{false};     // 正在联网打开一本书
std::atomic<int> s_gen{0};

// ---------------------------------------------------------------- 配置 / HTTP

Cfg LoadCfg() {
    Cfg c;
    FILE* f = std::fopen(kCfgPath, "rb");
    if (f == nullptr) return c;
    std::string text;
    char tmp[256];
    size_t n;
    while ((n = std::fread(tmp, 1, sizeof(tmp), f)) > 0 && text.size() < 4096) text.append(tmp, n);
    std::fclose(f);
    JsonPtr root(cJSON_Parse(text.c_str()), cJSON_Delete);
    if (!root) return c;
    const cJSON* s = cJSON_GetObjectItemCaseSensitive(root.get(), "server");
    const cJSON* t = cJSON_GetObjectItemCaseSensitive(root.get(), "token");
    if (cJSON_IsString(s)) c.server = s->valuestring;
    if (cJSON_IsString(t)) c.token = t->valuestring;
    while (!c.server.empty() && c.server.back() == '/') c.server.pop_back();
    return c;
}

Cfg GetCfg() {
    std::lock_guard<std::mutex> lock(S().mu);
    return S().cfg;
}

std::string UrlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

struct HttpResult {
    int status = 0;
    std::string body;
    int w = 0, h = 0;
    bool popup = false;
    int cpos = -2;   // X-CPos：这一页在离线缓存里的序号；-1 不在缓存里，-2 服务端没给
};

const char* JStr(const cJSON* o, const char* k);
std::atomic<bool> s_dbg_offline{false};  // 调试：假装连不上服务器（测离线阅读）

std::atomic<int64_t> s_last_ok_us{0};  // 最近一次服务器正常响应的时间

// 正在进行的前台请求（翻页、调字体、目录…）数。后台整本下载看到它就暂停读数据，把带宽和内存让出来：
// 两个一起跑时 RAM 被吃光，翻页一次要等一分多钟（用户实测「翻页特别卡、调字体也卡」）
std::atomic<int> s_fg_reqs{0};

bool RequestOnce(const char* method, const std::string& path, HttpResult& out, std::string& err) {
    struct FgGuard {
        FgGuard() { s_fg_reqs.fetch_add(1); }
        ~FgGuard() { s_fg_reqs.fetch_sub(1); }
    } fg_guard;
    if (s_dbg_offline.load() || net_mode::Offline()) {
        err = "连不上渲染服务";
        return false;
    }
    const Cfg cfg = GetCfg();
    auto* network = Board::GetInstance().GetNetwork();
    auto http = network ? network->CreateHttp(0) : nullptr;
    if (!http) {
        err = "无网络";
        return false;
    }
    http->SetTimeout(kHttpTimeoutMs);
    http->SetHeader("X-Token", cfg.token);
    if (std::strcmp(method, "POST") == 0) http->SetContent(std::string());
    const int64_t t0 = esp_timer_get_time();
    if (!http->Open(method, cfg.server + path)) {
        err = "连不上渲染服务";
        return false;
    }
    out.status = http->GetStatusCode();
    out.body = http->ReadAll();
    const std::string w = http->GetResponseHeader("X-W");
    const std::string h = http->GetResponseHeader("X-H");
    out.w = w.empty() ? 0 : std::atoi(w.c_str());
    out.h = h.empty() ? 0 : std::atoi(h.c_str());
    out.popup = http->GetResponseHeader("X-Popup") == "1";
    const std::string cp = http->GetResponseHeader("X-CPos");
    out.cpos = cp.empty() ? -2 : std::atoi(cp.c_str());
    http->Close();
    ESP_LOGI(TAG, "%s %s -> %d %uB %dms", method, path.substr(0, 60).c_str(), out.status,
             static_cast<unsigned>(out.body.size()), static_cast<int>((esp_timer_get_time() - t0) / 1000));
    if (out.status == 401) {
        err = "口令不对，检查 weread.json";
        return false;
    }
    if (out.status != 200) {
        // 服务端有中文说明就显示说明
        JsonPtr ej(cJSON_Parse(out.body.c_str()), cJSON_Delete);
        const char* em = ej ? JStr(ej.get(), "error") : "";
        err = (em && em[0]) ? std::string(em) : "服务返回 " + std::to_string(out.status);
        s_last_ok_us.store(esp_timer_get_time());  // 服务器是通的，只是这个请求出错
        return false;
    }
    s_last_ok_us.store(esp_timer_get_time());
    return true;
}

void SetToast(const std::string& t, int ms);
Cfg LoadCfg();

void WakeToast(const char* msg) { SetToast(msg, 0); }

/** 连不上服务器时：看是不是空闲自动关机了，是就开机，等服务起来后重试一次。 */
bool Request(const char* method, const std::string& path, HttpResult& out, std::string& err) {
    if (RequestOnce(method, path, out, err)) return true;
    if (err != "连不上渲染服务" || s_dbg_offline.load() || net_mode::Offline()) return false;
    if (esp_timer_get_time() - s_last_ok_us.load() < 60 * 1000000LL) {
        // 1 分钟内刚响应过：服务器肯定开着，是网络抖了一下，等 2 秒直接重试，不去查阿里云
        vTaskDelay(pdMS_TO_TICKS(2000));
        return RequestOnce(method, path, out, err);
    }
    if (!ecs::HasKey()) return false;
    static std::mutex wake_mu;  // 几个后台任务同时失败时只开一次机，其余的等着
    std::unique_lock<std::mutex> lock(wake_mu, std::try_to_lock);
    if (!lock.owns_lock()) {
        std::lock_guard<std::mutex> wait(wake_mu);
    } else {
        SetToast("连不上云服务器，正在查看是否关机…", 0);
        const bool woke = ecs::WakeServerIfStopped(180, WakeToast);
        if (!woke) return false;
        SetToast("云服务器已开机，继续…", 2000);
    }
    {
        Cfg c = LoadCfg();  // 开机后公网 IP 可能变了
        std::lock_guard<std::mutex> l2(S().mu);
        S().cfg = std::move(c);
    }
    return RequestOnce(method, path, out, err);
}

JsonPtr RequestJson(const std::string& path, std::string& err) {
    HttpResult r;
    if (!Request("GET", path, r, err)) return JsonPtr(nullptr, cJSON_Delete);
    JsonPtr j(cJSON_Parse(r.body.c_str()), cJSON_Delete);
    if (!j) err = "数据解析失败";
    return j;
}

const char* JStr(const cJSON* o, const char* k) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

int JInt(const cJSON* o, const char* k, int def = 0) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

// ---------------------------------------------------------------- 后台 → UI

void ApplyUi(void*);

void PostUi() {
    if (s_active.load()) ScreenLvAsync(ApplyUi);
}

void SetHint(const std::string& h) {
    {
        std::lock_guard<std::mutex> lock(S().mu);
        S().hint = h;
    }
    PostUi();
}

/** @param ms 0 = 一直显示，直到下一次 SetToast */
void SetToast(const std::string& t, int ms = 2500) {
    {
        std::lock_guard<std::mutex> lock(S().mu);
        S().toast = t;
        S().toast_until_us = (ms > 0 && !t.empty()) ? esp_timer_get_time() + ms * 1000LL : 0;
    }
    PostUi();
}

void SetMode(Mode m) {
    {
        std::lock_guard<std::mutex> lock(S().mu);
        S().mode = m;
    }
    PostUi();
}

void NoteOnlinePos(int cpos);

bool SetImage(Mode mode, HttpResult&& r) {
    if (mode == Mode::Reading && r.cpos >= 0) NoteOnlinePos(r.cpos);
    const int stride = (r.w + 7) / 8;
    if (r.w <= 0 || r.h <= 0 || r.w > kImgW || r.h > kMaxImgH ||
        r.body.size() != static_cast<size_t>(stride * r.h)) {
        ESP_LOGW(TAG, "bad image %dx%d %uB", r.w, r.h, static_cast<unsigned>(r.body.size()));
        SetToast("图片数据不对");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(S().mu);
        S().img = std::move(r.body);
        S().img_w = r.w;
        S().img_h = r.h;
        S().img_pending = true;
        S().mode = mode;
    }
    PostUi();
    return true;
}

/** 后台跑一个请求；gate 防止重复点击排队。toast 非空时请求期间显示。 */
void RunJobGated(std::atomic<bool>& gate, const char* toast, std::function<void()> fn,
                 std::function<void()> offline_fn = nullptr, bool need_net = true) {
    if (gate.exchange(true)) return;
    struct Job {
        std::atomic<bool>* gate;
        std::function<void()> fn;
        bool toast;
        std::function<void()> offline_fn;
        bool need_net;
    };
    auto* job = new Job{&gate, std::move(fn), toast != nullptr && toast[0] != '\0', std::move(offline_fn), need_net};
    if (job->toast) SetToast(toast, 0);
    auto task = [](void* arg) {
        auto* j = static_cast<Job*>(arg);
        {
            PowerNeedHold hold(PowerNeed::OtaDownload);
            auto& board = Board::GetInstance();
            if (j->need_net && !board.IsNetworkReady() && !board.EnsureNetworkReady(20000)) {
                if (j->offline_fn) j->offline_fn();
                else SetToast("网络未连接，请检查 Wi-Fi", 4000);
            } else {
                j->fn();
            }
        }
        {
            std::lock_guard<std::mutex> lock(S().mu);
            if (j->toast && S().toast_until_us == 0) S().toast.clear();  // 没被报错覆盖就收起
        }
        PostUi();
        j->gate->store(false);
        delete j;
        vTaskDeleteWithCaps(nullptr);
    };
    if (xTaskCreateWithCaps(task, "weread_job", 10 * 1024, job, 3, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        gate.store(false);
        delete job;
        SetToast("内存不足");
    }
}

void RunJob(const char* toast, std::function<void()> fn, std::function<void()> offline_fn = nullptr) {
    RunJobGated(s_busy, toast, std::move(fn), std::move(offline_fn));
}

// ---------------------------------------------------------------- 后台请求

std::string CoverPath(const std::string& id) {
    return std::string(kCoverDir) + "/" + id + "_" + std::to_string(kCoverW) + "x" + std::to_string(kCoverH) + ".bin";
}

/** 封面：SD 卡缓存优先，没有再向服务端要（服务端转成 1bpp 抖动图） */
void LoadCoversAsync(std::vector<std::string> ids) {
    // 不等网：SD 上的封面先全部显示，缺的再在有网时补（以前整个任务先等网 20 秒，没网就一张都不显示）
    RunJobGated(s_cover_busy, nullptr, [ids]() {
        mkdir(kCoverDir, 0775);
        std::vector<std::string> missing;
        for (const auto& id : ids) {
            {
                std::lock_guard<std::mutex> lock(S().mu);
                if (S().covers.count(id)) continue;
            }
            std::string data;
            if (FILE* f = std::fopen(CoverPath(id).c_str(), "rb")) {
                data.resize(kCoverBytes);
                if (std::fread(&data[0], 1, kCoverBytes, f) != static_cast<size_t>(kCoverBytes)) data.clear();
                std::fclose(f);
            }
            if (data.empty()) {
                missing.push_back(id);
                continue;
            }
            std::lock_guard<std::mutex> lock(S().mu);
            S().covers[id] = std::move(data);
            S().covers_dirty = true;
        }
        PostUi();
        if (missing.empty() || net_mode::Offline() || !Board::GetInstance().IsNetworkReady()) return;
        int fresh = 0;
        for (const auto& id : missing) {
            if (!s_active.load()) return;
            {
                std::lock_guard<std::mutex> lock(S().mu);
                if (S().covers.count(id)) continue;
            }
            std::string data;
            const std::string path = CoverPath(id);
            if (FILE* f = std::fopen(path.c_str(), "rb")) {
                data.resize(kCoverBytes);
                if (std::fread(&data[0], 1, kCoverBytes, f) != static_cast<size_t>(kCoverBytes)) data.clear();
                std::fclose(f);
            }
            if (data.empty()) {
                HttpResult r;
                std::string err;
                if (!Request("GET", "/api/cover?bookId=" + UrlEncode(id) + "&w=" + std::to_string(kCoverW) +
                                        "&h=" + std::to_string(kCoverH),
                             r, err) ||
                    r.body.size() != static_cast<size_t>(kCoverBytes)) {
                    continue;
                }
                data = std::move(r.body);
                if (FILE* f = std::fopen(path.c_str(), "wb")) {
                    std::fwrite(data.data(), 1, data.size(), f);
                    std::fclose(f);
                }
            }
            {
                std::lock_guard<std::mutex> lock(S().mu);
                S().covers[id] = std::move(data);
                if (++fresh % 3 == 0) S().covers_dirty = true;
            }
            if (fresh % 3 == 0) PostUi();
        }
        {
            std::lock_guard<std::mutex> lock(S().mu);
            S().covers_dirty = true;
        }
        PostUi();
    }, nullptr, false);
}

// ---------------------------------------------------------------- 离线缓存
// SD: weread_cache/<bookId>/{manifest.json, p00000.bin…, state.json}；书架列表另存 weread_shelf.json
constexpr const char* kCacheRoot = SD_APP_ROOT "/weread_cache";
constexpr const char* kShelfFile = SD_APP_ROOT "/weread_shelf.json";
std::atomic<bool> s_cache_sync_busy{false};

struct Offline {
    std::atomic<bool> on{false};  // 正在离线读
    std::string book;
    int pos = 0, n = 0;
} s_off;                       // 只在后台任务 / LVGL 线程里串行使用

std::string CacheDir(const std::string& id) { return std::string(kCacheRoot) + "/" + id; }

bool ReadAll(const std::string& path, std::string& out) {
    out.clear();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[1024];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}

bool WriteAll(const std::string& path, const std::string& data) {
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    if (!ok) return false;
    unlink(path.c_str());
    return rename(tmp.c_str(), path.c_str()) == 0;
}

struct CacheState {
    int n = 0;          // 已下载到 SD 的页数
    int pos = 0;        // 离线读到第几页
    bool dirty = false; // 离线翻过页，回到线上要同步进度
    bool done = false;  // 服务端缓存已完成且已全部下载
    bool exists = false; // 有 state.json（点过缓存）
    long gen = 0;        // 服务端这份缓存的创建时间：变了说明服务端重截过，本地旧页作废
};

// 离线翻页时阅读位置先记在内存，停手 1.5 秒后由后台任务写 state.json（写 SD 要 300 多 ms，以前卡在翻页路上）
std::mutex s_pending_mu;
std::string s_pending_book;  // 空 = 没有待写
int s_pending_pos = 0;
std::atomic<int> s_pending_gen{0};
std::atomic<bool> s_pending_task{false};

bool PendingPosFor(const std::string& id, int* pos) {
    std::lock_guard<std::mutex> lock(s_pending_mu);
    if (s_pending_book.empty() || s_pending_book != id) return false;
    *pos = s_pending_pos;
    return true;
}

CacheState LoadCacheStateRaw(const std::string& id);
CacheState LoadCacheState(const std::string& id) {
    CacheState c = LoadCacheStateRaw(id);
    int pos = 0;
    if (c.exists && PendingPosFor(id, &pos)) {  // 还没写盘的离线位置以内存为准
        c.pos = pos;
        c.dirty = true;
    }
    return c;
}

CacheState LoadCacheStateRaw(const std::string& id) {
    CacheState c;
    std::string t;
    if (!ReadAll(CacheDir(id) + "/state.json", t)) return c;
    JsonPtr r(cJSON_Parse(t.c_str()), cJSON_Delete);
    if (!r) return c;
    c.exists = true;
    c.gen = static_cast<long>(JInt(r.get(), "gen"));
    c.n = JInt(r.get(), "n");
    c.pos = JInt(r.get(), "pos");
    c.dirty = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r.get(), "dirty"));
    c.done = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r.get(), "done"));
    return c;
}

void SaveCacheState(const std::string& id, const CacheState& c) {
    {
        std::lock_guard<std::mutex> lock(s_pending_mu);
        if (s_pending_book == id && s_pending_pos == c.pos) s_pending_book.clear();  // 已含待写位置
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf), "{\"n\":%d,\"pos\":%d,\"dirty\":%s,\"done\":%s,\"gen\":%ld}", c.n, c.pos,
                  c.dirty ? "true" : "false", c.done ? "true" : "false", c.gen);
    WriteAll(CacheDir(id) + "/state.json", buf);
}

std::mutex s_state_mu;  // state.json 读改写

void FlushPendingPos() {
    std::string book;
    {
        std::lock_guard<std::mutex> lock(s_pending_mu);
        if (s_pending_book.empty()) return;
        book = s_pending_book;
    }
    std::lock_guard<std::mutex> lock(s_state_mu);
    CacheState c = LoadCacheState(book);  // 已叠加最新的待写位置
    SaveCacheState(book, c);
}

void QueuePosSave(const std::string& book, int pos) {
    {
        std::lock_guard<std::mutex> lock(s_pending_mu);
        if (!s_pending_book.empty() && s_pending_book != book) {
            // 换书了：旧书的位置先同步写掉（很少见）
            std::string old = s_pending_book;
            int old_pos = s_pending_pos;
            s_pending_book.clear();
            s_pending_mu.unlock();
            {
                std::lock_guard<std::mutex> l2(s_state_mu);
                CacheState c = LoadCacheState(old);
                c.pos = old_pos;
                c.dirty = true;
                SaveCacheState(old, c);
            }
            s_pending_mu.lock();
        }
        s_pending_book = book;
        s_pending_pos = pos;
    }
    s_pending_gen.fetch_add(1);
    if (s_pending_task.exchange(true)) return;
    if (xTaskCreateWithCaps(
            [](void*) {
                for (;;) {
                    int g;
                    do {
                        g = s_pending_gen.load();
                        vTaskDelay(pdMS_TO_TICKS(1500));
                    } while (g != s_pending_gen.load());
                    FlushPendingPos();
                    s_pending_task.store(false);
                    {
                        std::lock_guard<std::mutex> lock(s_pending_mu);
                        if (s_pending_book.empty()) break;
                    }
                    if (s_pending_task.exchange(true)) break;  // 另一个任务接手
                }
                vTaskDeleteWithCaps(nullptr);
            },
            "wr_possave", 6144, nullptr, 2, nullptr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_pending_task.store(false);
        FlushPendingPos();
    }
}

/** 只改下载进度（n/done），不碰阅读位置 */
void SaveCacheProgress(const std::string& id, int n, bool done) {
    std::lock_guard<std::mutex> lock(s_state_mu);
    CacheState c = LoadCacheState(id);
    c.n = n;
    c.done = done;
    SaveCacheState(id, c);
}

/** 在线读到缓存里的第 cpos 页：记下来，断网后从这一页接着读 */
void NoteOnlinePos(int cpos) {
    std::string id;
    {
        std::lock_guard<std::mutex> lock(S().mu);
        id = S().book_id;
    }
    if (id.empty()) return;
    std::lock_guard<std::mutex> lock(s_state_mu);
    CacheState c = LoadCacheState(id);
    if (c.n <= 0 || (c.pos == cpos && !c.dirty)) return;
    c.pos = cpos;
    c.dirty = false;
    SaveCacheState(id, c);
}

/** 读第 i 页缓存上屏 */
bool ShowCachedPage(const std::string& id, int i) {
    char name[24];
    std::snprintf(name, sizeof(name), "/p%05d.bin", i);
    HttpResult r;
    if (!ReadAll(CacheDir(id) + name, r.body) || r.body.empty()) return false;
    r.w = kImgW;
    r.h = static_cast<int>(r.body.size() / (kImgW / 8));
    r.status = 200;
    return SetImage(Mode::Reading, std::move(r));
}

/** 离线打开：从上次离线读到的位置开始 */
bool OpenOffline(const std::string& id) {
    CacheState c = LoadCacheState(id);
    if (c.n <= 0) return false;
    const int pos = std::clamp(c.pos, 0, c.n - 1);
    if (!ShowCachedPage(id, pos)) return false;
    s_off.on = true;
    s_off.book = id;
    s_off.pos = pos;
    s_off.n = c.n;
    char t[64];
    std::snprintf(t, sizeof(t), "离线阅读（缓存 %d/%d 页）", pos + 1, c.n);
    SetToast(t, 3000);
    return true;
}

int64_t s_flip_t0 = 0;  // 打点：翻页开始
void OfflineFlip(int dir) {
    s_flip_t0 = esp_timer_get_time();
    const int next = s_off.pos + dir;
    if (next < 0) {
        SetToast("已是缓存的第一页", 1500);
        return;
    }
    if (next >= s_off.n) {
        SetToast("缓存的部分读完了，联网后可以接着读", 3000);
        return;
    }
    if (!ShowCachedPage(s_off.book, next)) {
        SetToast("缓存页读取失败", 2000);
        return;
    }
    const int64_t t1 = esp_timer_get_time();
    s_off.pos = next;
    QueuePosSave(s_off.book, next);
    ESP_LOGD(TAG, "flip timing: read+set %dms", (int)((t1 - s_flip_t0) / 1000));
}

/** 离线跳到缓存第 page 页（目录用） */
void OfflineJump(int page) {
    if (page < 0 || page >= s_off.n) {
        SetToast("这一章还没下载到本机", 2500);
        return;
    }
    if (!ShowCachedPage(s_off.book, page)) {
        SetToast("缓存页读取失败", 2000);
        return;
    }
    s_off.pos = page;
    std::lock_guard<std::mutex> lock(s_state_mu);
    CacheState c = LoadCacheState(s_off.book);
    c.pos = page;
    c.dirty = true;
    SaveCacheState(s_off.book, c);
}

// 缓存队列：一本一本来。后台任务从队列取书 → 让服务端开始截图 → 边截边下载 → 下一本
struct CacheReq {
    std::string id, title;
    int chapters = 0;
    int h = 0;
    bool fresh = true;   // false = 接着上次没下载完的继续
};
std::deque<CacheReq> s_cq;          // S().mu 保护
std::string s_cq_cur, s_cq_cur_title;
int s_cq_cur_n = 0;
std::string s_cq_early;             // 已提前让服务端开截的下一本（上一本服务端截完、设备还在下载时）

/** 排队的书存到 SD：重启/升级后接着排（S().mu 持有时调用） */
void SaveQueueLocked() {
    cJSON* arr = cJSON_CreateArray();
    for (const auto& q : s_cq) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", q.id.c_str());
        cJSON_AddStringToObject(o, "title", q.title.c_str());
        cJSON_AddNumberToObject(o, "chapters", q.chapters);
        cJSON_AddNumberToObject(o, "h", q.h);
        cJSON_AddBoolToObject(o, "fresh", q.fresh);
        cJSON_AddItemToArray(arr, o);
    }
    char* txt = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (txt) {
        WriteAll(std::string(kCacheRoot) + "/queue.json", txt);
        cJSON_free(txt);
    }
}

void MarkShelfDirty() {
    {
        std::lock_guard<std::mutex> lock(S().mu);
        S().cache_dirty = true;
    }
    PostUi();
}

bool WifiUp() {
    if (s_dbg_offline.load() || net_mode::Offline()) return false;  // 调试 WR NET 0：当作断网
    wifi_ap_record_t ap{};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;
    esp_netif_t* nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip{};
    return nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr != 0;
}

enum class SyncRes { Done, Failed, NoNet };

void LockedTick();
std::atomic<bool> s_bundle_ok{true};  // 服务端支持 /api/cache/bundle（旧服务端 404 就退回一页一页下）

/**
 * 一个连接连续收多页：[uint32 序号][uint32 长度][zlib]...，解压后写 SD。
 * 返回写入的页数；-1 = 服务端不支持。c.n 随之前进。
 */
int DownloadBundle(const std::string& id, CacheState& c, const std::function<void()>& on_page) {
    const Cfg cfg = GetCfg();
    auto* network = Board::GetInstance().GetNetwork();
    auto http = network ? network->CreateHttp(0) : nullptr;
    if (!http) return 0;
    http->SetTimeout(30000);
    http->SetHeader("X-Token", cfg.token);
    const std::string path = "/api/cache/bundle?bookId=" + UrlEncode(id) + "&from=" + std::to_string(c.n) + "&max=500";
    const int64_t t0 = esp_timer_get_time();
    if (!http->Open("GET", cfg.server + path)) return 0;
    const int status = http->GetStatusCode();
    if (status != 200) {
        http->Close();
        ESP_LOGW(TAG, "bundle -> %d", status);
        return status == 404 ? -1 : 0;
    }
    s_last_ok_us.store(esp_timer_get_time());
    static tinfl_decompressor* dec = nullptr;
    if (!dec) dec = static_cast<tinfl_decompressor*>(heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM));
    constexpr size_t kOutCap = (kImgW / 8) * kMaxImgH;
    std::unique_ptr<uint8_t, void (*)(void*)> out(static_cast<uint8_t*>(heap_caps_malloc(kOutCap, MALLOC_CAP_SPIRAM)),
                                                  heap_caps_free);
    std::unique_ptr<char[]> rd(new char[4096]);
    if (!dec || !out) {
        http->Close();
        return 0;
    }
    std::string buf;
    int got = 0;
    size_t zbytes = 0;
    bool bad = false;
    while (!bad) {
        // 前台有请求：先停一停（最多 20 秒，再久服务器那边会断）
        for (int waited = 0; s_fg_reqs.load() > 0 && waited < 20000; waited += 100) vTaskDelay(pdMS_TO_TICKS(100));
        const int n = http->Read(rd.get(), 4096);
        if (n <= 0) break;  // 0 = 发完；<0 = 断了，下一轮从 c.n 接着要

        buf.append(rd.get(), static_cast<size_t>(n));
        size_t off = 0;
        while (buf.size() - off >= 8) {
            uint32_t idx = 0, zlen = 0;
            std::memcpy(&idx, buf.data() + off, 4);
            std::memcpy(&zlen, buf.data() + off + 4, 4);
            if (zlen == 0 || zlen > 400000 || static_cast<int>(idx) != c.n) {
                ESP_LOGW(TAG, "bundle bad record idx=%u want=%d zlen=%u", static_cast<unsigned>(idx), c.n,
                         static_cast<unsigned>(zlen));
                bad = true;
                break;
            }
            if (buf.size() - off - 8 < zlen) break;  // 这页还没收全
            tinfl_init(dec);
            size_t in_sz = zlen, out_sz = kOutCap;
            const tinfl_status ts =
                tinfl_decompress(dec, reinterpret_cast<const mz_uint8*>(buf.data() + off + 8), &in_sz, out.get(), out.get(),
                                 &out_sz, TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
            if (ts != TINFL_STATUS_DONE || out_sz == 0 || out_sz % (kImgW / 8) != 0) {
                ESP_LOGW(TAG, "bundle inflate fail page %d st=%d out=%u", c.n, static_cast<int>(ts),
                         static_cast<unsigned>(out_sz));
                bad = true;
                break;
            }
            char name[24];
            std::snprintf(name, sizeof(name), "/p%05d.bin", c.n);
            if (!WriteAll(CacheDir(id) + name, std::string(reinterpret_cast<const char*>(out.get()), out_sz))) {
                SetToast("SD 卡写入失败", 3000);
                bad = true;
                break;
            }
            ++c.n;
            ++got;
            zbytes += zlen;
            off += 8 + zlen;
            on_page();
        }
        buf.erase(0, off);
    }
    http->Close();
    const int ms = static_cast<int>((esp_timer_get_time() - t0) / 1000);
    ESP_LOGI(TAG, "bundle %s +%d pages %uKB %dms (%d KB/s)", id.c_str(), got, static_cast<unsigned>(zbytes / 1024), ms,
             ms > 0 ? static_cast<int>(zbytes * 1000 / 1024 / ms) : 0);
    return got;
}

/** 锁屏后还在下载（没进真正待机）：屏幕像待机那样冻结，时钟靠这里每分钟走一次 */
void LockedTick() {
    static int64_t active_since = 0;
    static bool drew = false;
    if (!StandbyScreen::IsActive()) {
        active_since = 0;
        drew = false;
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (active_since == 0) active_since = now;
    if (now - active_since < 150000 || !StandbyScreen::IsPaintReady()) return;  // 等锁屏画面画好（同正常待机 80ms+）
    StandbyScreen::EnterEpdSleep();  // 已冻结就直接返回
    StandbyCard_ClockTick(!drew);
    drew = true;
}

/** 下载一本 */
SyncRes SyncOneBook(const std::string& id) {
    mkdir(kCacheRoot, 0775);
    mkdir(CacheDir(id).c_str(), 0775);
    CacheState c = LoadCacheState(id);
    {
        std::lock_guard<std::mutex> lock(S().mu);
        s_cq_cur_n = c.n;  // 续传：显示已有页数
    }
    int fails = 0;
    for (int round = 0; round < 4000; ++round) {
        std::string err;
        JsonPtr st = RequestJson("/api/cache/status?bookId=" + UrlEncode(id), err);
        if (!st) {
            if (!WifiUp()) return SyncRes::NoNet;  // 断网（锁屏、出门）：等网回来接着下
            if (++fails > 6) return SyncRes::Failed;
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }
        fails = 0;
        const std::string state = JStr(st.get(), "state");
        const int total = JInt(st.get(), "pages");
        const long created = static_cast<long>(JInt(st.get(), "created"));
        if (created > 0 && c.gen != created) {
            std::lock_guard<std::mutex> lock(s_state_mu);
            CacheState now = LoadCacheState(id);
            if (now.gen != 0 && now.n > 0) {
                // 服务端重截过：本地的页是旧那份的，混着用页码会对不上，从头重下
                ESP_LOGW(TAG, "cache %s server regenerated (%ld -> %ld), drop %d local pages", id.c_str(), now.gen,
                         created, now.n);
                now.n = 0;
                now.done = false;
                now.pos = 0;
                now.dirty = false;
            }
            now.gen = created;
            SaveCacheState(id, now);
            c = now;
        }
        const int before = c.n;
        auto maybe_early = [&]() {
            if (state != "done" || c.n + 20 >= total) return;
            // 服务端这本截完了、设备还在下载：让服务端先开截队里下一本
            CacheReq next;
            bool has = false;
            {
                std::lock_guard<std::mutex> lock(S().mu);
                if (!s_cq.empty() && s_cq.front().fresh && s_cq_early != s_cq.front().id) {
                    next = s_cq.front();
                    has = true;
                }
            }
            HttpResult r2;
            std::string e2;
            if (has && Request("POST", "/api/cache/start?reuse=1&bookId=" + UrlEncode(next.id) + "&chapters=" +
                                           std::to_string(next.chapters) + "&h=" + std::to_string(next.h) +
                                           (next.chapters == 0 ? "&start=1" : ""), r2, e2)) {
                ESP_LOGI(TAG, "cache early start %s", next.id.c_str());
                mkdir(CacheDir(next.id).c_str(), 0775);
                SaveCacheState(next.id, CacheState{});
                std::lock_guard<std::mutex> lock(S().mu);
                s_cq_early = next.id;
            }
        };
        maybe_early();
        if (s_bundle_ok.load() && c.n < total) {
            const int got = DownloadBundle(id, c, [&]() {
                {
                    std::lock_guard<std::mutex> lock(S().mu);
                    s_cq_cur_n = c.n;
                }
                if (c.n % 10 == 0) {
                    SaveCacheProgress(id, c.n, false);
                    MarkShelfDirty();
                    if (c.n % 50 == 0) maybe_early();
                }
            });
            if (got < 0) s_bundle_ok.store(false);
        }
        for (; !s_bundle_ok.load() && c.n < total; ++c.n) {
            HttpResult r;
            if (!Request("GET", "/api/cache/page?bookId=" + UrlEncode(id) + "&i=" + std::to_string(c.n), r, err)) {
                if (r.status == 404) {
                    // 服务端缓存没了（被重新截过 / 删了）：本地这本作废，下次重新缓存
                    ESP_LOGW(TAG, "cache page %d gone on server", c.n);
                    return SyncRes::Failed;
                }
                break;
            }
            char name[24];
            std::snprintf(name, sizeof(name), "/p%05d.bin", c.n);
            if (!WriteAll(CacheDir(id) + name, r.body)) {
                SetToast("SD 卡写入失败", 3000);
                return SyncRes::Failed;
            }
            {
                std::lock_guard<std::mutex> lock(S().mu);
                s_cq_cur_n = c.n + 1;
            }
            if (c.n % 10 == 0) {
                SaveCacheProgress(id, c.n, false);
                MarkShelfDirty();
                if (c.n % 50 == 0) maybe_early();
            }
        }
        SaveCacheProgress(id, c.n, false);
        if (c.n != before) MarkShelfDirty();
        const bool finished = state == "done" || state == "error" || state == "cancelled" || state == "none" ||
                              state == "partial";
        if (finished && c.n >= total) {
            HttpResult r;
            if (Request("GET", "/api/cache/manifest?bookId=" + UrlEncode(id), r, err)) {
                WriteAll(CacheDir(id) + "/manifest.json", r.body);
            }
            c.done = state == "done";
            SaveCacheProgress(id, c.n, c.done);
            ESP_LOGI(TAG, "cache sync %s done n=%d state=%s", id.c_str(), c.n, state.c_str());
            return state == "done" ? SyncRes::Done : SyncRes::Failed;
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    return SyncRes::Failed;
}

/** 处理队列直到空或断网；断网时把当前这本放回队首，返回 false */
bool DrainCacheQueue() {
    PowerNeedHold hold(PowerNeed::OtaDownload);  // 期间不进无操作待机
    PowerNeedHold bg(PowerNeed::BgDownload);     // 锁屏了也先保网，下载完才真正待机
    for (;;) {
        CacheReq q;
        {
            std::lock_guard<std::mutex> lock(S().mu);
            if (s_cq.empty()) {
                s_cq_cur.clear();
                s_cq_cur_title.clear();
                return true;
            }
            q = s_cq.front();
            s_cq.pop_front();
            s_cq_cur = q.id;
            s_cq_cur_title = q.title;
            s_cq_cur_n = 0;
            if (s_cq_early == q.id) {
                q.fresh = false;  // 服务端已经提前开截了，只续传
                s_cq_early.clear();
            }
            // 当前这本也留在文件里（排第一），下载完才删
            s_cq.push_front(q);
            SaveQueueLocked();
            s_cq.pop_front();
        }
        MarkShelfDirty();
        auto requeue = [&q]() {
            std::lock_guard<std::mutex> lock(S().mu);
            s_cq.push_front(q);
            s_cq_cur.clear();
            SaveQueueLocked();
        };
        HttpResult r;
        std::string err;
        JsonPtr st = RequestJson("/api/cache/status?bookId=" + UrlEncode(q.id), err);
        if (!st && !WifiUp()) {
            requeue();
            return false;
        }
        const std::string sstate = st ? JStr(st.get(), "state") : "";
        // 服务端正在截这本（比如设备重启过），或者是续传：不重新开始，只把没下完的接着下
        const bool resume = sstate == "running" || sstate == "starting" || (!q.fresh && sstate == "done");
        if (!resume) {
            // 续传但服务端已经没有这本了：跳过；服务端那份截到一半断了（服务重启/自动关机）：重新截
            if (!q.fresh && sstate != "partial") continue;
            if (sstate == "partial") ESP_LOGW(TAG, "cache %s partial on server, restart", q.id.c_str());
            if (!Request("POST", "/api/cache/start?reuse=1&bookId=" + UrlEncode(q.id) + "&chapters=" +
                                     std::to_string(q.chapters) + "&h=" + std::to_string(q.h) +
                                     (q.chapters == 0 ? "&start=1" : ""), r, err)) {
                if (!WifiUp()) {
                    requeue();
                    return false;
                }
                SetToast("《" + q.title + "》缓存失败：" + err, 3000);
                continue;
            }
            mkdir(kCacheRoot, 0775);
            mkdir(CacheDir(q.id).c_str(), 0775);
            SaveCacheState(q.id, CacheState{});  // 服务端从当前进度重新截，本地计数清零
        }
        ESP_LOGI(TAG, "cache %s %s (server %s)", resume ? "resume" : "start", q.id.c_str(), sstate.c_str());
        const SyncRes res = SyncOneBook(q.id);
        if (res == SyncRes::NoNet) {
            q.fresh = false;  // 服务端已经在截了，回来只续传
            requeue();
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(S().mu);
            SaveQueueLocked();  // 这本完了，从文件里去掉
        }
        const CacheState c = LoadCacheState(q.id);
        char t[120];
        std::snprintf(t, sizeof(t), "%s《%s》%d 页", res == SyncRes::Done ? "已缓存" : "缓存没完成", q.title.c_str(),
                      c.n);
        SetToast(t, 3500);
    }
}

void CacheWorker(void*) {
    // 断网时不占网不拦待机，每分钟看一眼；没锁屏就试着连一次（Request 会自己拉起 WiFi），最多等 6 小时
    for (int waited = 0; waited < 6 * 3600;) {
        if (!StandbyScreen::IsActive() && !net_mode::Offline()) {
            if (DrainCacheQueue()) break;
            ESP_LOGW(TAG, "cache: no network, wait and retry");
            MarkShelfDirty();
        }
        vTaskDelay(pdMS_TO_TICKS(60000));
        waited += 60;
    }
    {
        std::lock_guard<std::mutex> lock(S().mu);
        s_cq_cur.clear();
    }
    MarkShelfDirty();
    s_cache_sync_busy.store(false);
    vTaskDeleteWithCaps(nullptr);
}

/** 后台下载期间每 0.1 秒看一眼：锁屏了就马上切锁屏画面、走时钟（下载线程可能卡在网络上） */
void LockWatchTask(void*) {
    while (s_cache_sync_busy.load()) {
        vTaskDelay(pdMS_TO_TICKS(100));
        LockedTick();
    }
    vTaskDeleteWithCaps(nullptr);
}

void StartCacheWorker() {
    if (s_cache_sync_busy.exchange(true)) return;
    xTaskCreateWithCaps(LockWatchTask, "wr_lockw", 6144, nullptr, 3, nullptr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (xTaskCreateWithCaps(CacheWorker, "wr_cache", 10 * 1024, nullptr, 2, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_cache_sync_busy.store(false);
        SetToast("内存不足，缓存没启动");
    }
}

/** 入队；已在队列或正在缓存的跳过。返回实际新加的数量。 */
int ReadH();

int EnqueueCache(const std::vector<std::pair<std::string, std::string>>& books, int chapters, int h,
                 bool fresh = true) {
    if (h <= 0) h = ReadH();  // 续传时不知道高度：用当前阅读区高度（以前传 0，服务端按 600 截，页面变矮）
    int added = 0;
    {
        std::lock_guard<std::mutex> lock(S().mu);
        for (const auto& b : books) {
            bool dup = b.first == s_cq_cur;
            for (const auto& q : s_cq) dup = dup || q.id == b.first;
            if (dup) continue;
            s_cq.push_back(CacheReq{b.first, b.second, chapters, h, fresh});
            ++added;
        }
        if (added) {
            mkdir(kCacheRoot, 0775);
            SaveQueueLocked();
        }
        S().cache_dirty = true;
    }
    if (added) StartCacheWorker();
    PostUi();
    return added;
}

/** 书在缓存里的状态文字（书架角标 / 菜单用）；空串 = 没缓存 */
std::string CacheBadge(const std::string& id, bool short_text = false) {
    {
        std::lock_guard<std::mutex> lock(S().mu);
        if (id == s_cq_cur) return short_text ? std::string("缓存中") : "缓存中 " + std::to_string(s_cq_cur_n) + "页";
        for (const auto& q : s_cq) {
            if (q.id == id) return "排队中";
        }
    }
    const CacheState c = LoadCacheState(id);
    if (c.n <= 0) return "";
    if (c.done) return "已缓存";
    return short_text ? std::string("没缓存完") : "缓存了" + std::to_string(c.n) + "页（没完）";
}

void RemoveDirTask(void* arg) {
    std::unique_ptr<std::string> d(static_cast<std::string*>(arg));
    int n = 0;
    if (DIR* dir = opendir(d->c_str())) {
        while (dirent* e = readdir(dir)) {
            if (e->d_name[0] == '.') continue;
            unlink((*d + "/" + e->d_name).c_str());
            if (++n % 50 == 0) vTaskDelay(1);
        }
        closedir(dir);
    }
    rmdir(d->c_str());
    ESP_LOGI(TAG, "removed %s (%d files)", d->c_str(), n);
    vTaskDeleteWithCaps(nullptr);
}

/** 先改名（瞬间完成，书马上显示没缓存），几千个文件在后台慢慢删，别卡界面 */
void DeleteCache(const std::string& id) {
    const std::string d = CacheDir(id);
    const std::string trash = std::string(kCacheRoot) + "/.del_" + id + "_" + std::to_string(esp_timer_get_time() / 1000);
    const std::string* target = new std::string(rename(d.c_str(), trash.c_str()) == 0 ? trash : d);
    if (xTaskCreateWithCaps(RemoveDirTask, "wr_rmdir", 4096, const_cast<std::string*>(target), 1, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        delete target;
    }
}

void CacheJob(int chapters, int h) {
    std::string id, title;
    {
        std::lock_guard<std::mutex> lock(S().mu);
        id = S().book_id;
        title = S().book_title;
    }
    if (id.empty()) return;
    if (EnqueueCache({{id, title}}, chapters, h) == 0) {
        SetToast("这本已经在缓存了", 2000);
        return;
    }
    SetToast(s_cq_cur == id || s_cq.empty() ? "开始缓存，可以继续读" : "已加入缓存队列", 2500);
}

/** 离线时的书架：用上次存下的列表 */
void OfflineShelfJob() {
    std::string t;
    if (!ReadAll(kShelfFile, t)) {
        SetHint("没有网络，也没有离线缓存的书");
        return;
    }
    JsonPtr root(cJSON_Parse(t.c_str()), cJSON_Delete);
    if (!root) return;
    std::vector<Book> books;
    std::vector<std::string> ids;
    const cJSON* it = nullptr;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root.get(), "books")) {
        Book b;
        b.id = JStr(it, "bookId");
        b.title = JStr(it, "title");
        b.progress = JInt(it, "progress", -1);
        if (b.id.empty() || LoadCacheState(b.id).n <= 0) continue;  // 离线只列有缓存的书
        ids.push_back(b.id);
        books.push_back(std::move(b));
    }
    {
        std::lock_guard<std::mutex> lock(S().mu);
        S().books = std::move(books);
        S().books_dirty = true;
        S().hint = S().books.empty() ? "离线中：没有已缓存的书" : "";
        if (S().mode == Mode::Login) S().mode = Mode::Shelf;
    }
    SetToast("离线中：只显示已缓存的书", 3000);
    PostUi();
    LoadCoversAsync(std::move(ids));  // 封面在 SD 上有缓存
}

void LoadShelfJob(bool refresh) {
    std::string err;
    HttpResult raw;
    if (!Request("GET", refresh ? "/api/shelf?refresh=1" : "/api/shelf", raw, err)) {
        OfflineShelfJob();
        return;
    }
    JsonPtr root(cJSON_Parse(raw.body.c_str()), cJSON_Delete);
    if (!root) {
        SetHint("数据解析失败");
        return;
    }
    WriteAll(kShelfFile, raw.body);
    std::vector<Book> books;
    std::vector<std::string> ids;
    const cJSON* it = nullptr;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root.get(), "books")) {
        Book b;
        b.id = JStr(it, "bookId");
        b.title = JStr(it, "title");
        b.progress = JInt(it, "progress", -1);
        if (b.id.empty()) continue;
        {
            // 没 API Key 时书的 id 是阅读地址里那串；配了 Key 后变成数字 id：把旧缓存目录挪过来
            const std::string href = JStr(it, "href");
            const size_t sl = href.rfind('/');
            const std::string old = sl == std::string::npos ? "" : href.substr(sl + 1);
            struct stat st_old, st_new;
            if (!old.empty() && old != b.id && stat(CacheDir(old).c_str(), &st_old) == 0 &&
                stat(CacheDir(b.id).c_str(), &st_new) != 0) {
                const int rc = rename(CacheDir(old).c_str(), CacheDir(b.id).c_str());
                ESP_LOGW(TAG, "cache dir %s -> %s rc=%d", old.c_str(), b.id.c_str(), rc);
            }
        }
        ids.push_back(b.id);
        books.push_back(std::move(b));
    }
    {
        std::lock_guard<std::mutex> lock(S().mu);
        S().books = std::move(books);
        S().books_dirty = true;
        S().hint = S().books.empty() ? "书架是空的" : "";
        if (S().mode == Mode::Login) S().mode = Mode::Shelf;
    }
    PostUi();
    {
        // 先读上次存的队列（入队后后台任务会改写这个文件），再把没下载完的和排队的一起放回去
        std::string qt;
        ReadAll(std::string(kCacheRoot) + "/queue.json", qt);
        std::vector<std::pair<std::string, std::string>> all, pending;
        {
            std::lock_guard<std::mutex> lock(S().mu);
            for (const auto& b : S().books) all.emplace_back(b.id, b.title);
        }
        for (const auto& b : all) {
            const CacheState c = LoadCacheState(b.first);
            if ((c.n > 0 || c.exists) && !c.done) pending.push_back(b);
        }
        if (!pending.empty()) EnqueueCache(pending, 0, 0, false);
        JsonPtr qa(cJSON_Parse(qt.c_str()), cJSON_Delete);
        const cJSON* o = nullptr;
        cJSON_ArrayForEach(o, qa.get()) {
            const std::string qid = JStr(o, "id");
            if (qid.empty()) continue;
            const CacheState qc = LoadCacheState(qid);
            if (qc.done) continue;
            const bool fresh = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "fresh")) && !qc.exists;
            EnqueueCache({{qid, JStr(o, "title")}}, JInt(o, "chapters"), JInt(o, "h"), fresh);
        }
    }
    LoadCoversAsync(std::move(ids));
}

void LoginJob() {
    HttpResult r;
    std::string err;
    SetHint("正在获取登录二维码…");
    if (!Request("POST", "/api/login", r, err)) {
        SetHint(err);
        return;
    }
    if (r.body.find("\"logged_in\": true") != std::string::npos) {
        LoadShelfJob(false);
        return;
    }
    HttpResult q;
    if (!Request("GET", "/api/login/qr?fmt=mono1", q, err)) {
        SetHint(err);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(S().mu);
        S().hint = "请用手机微信扫码登录微信读书";
    }
    SetImage(Mode::Login, std::move(q));
    const int gen = s_gen.load();
    for (int i = 0; i < 100 && s_active.load() && gen == s_gen.load(); ++i) {  // 最多约 5 分钟
        vTaskDelay(pdMS_TO_TICKS(3000));
        HttpResult st;
        if (!Request("GET", "/api/login/state", st, err)) continue;
        if (st.body.find("logged_in") != std::string::npos) {
            LoadShelfJob(true);
            return;
        }
        if (st.body.find("expired") != std::string::npos) {
            SetHint("二维码过期了，按 BOOT 键重新获取");
            return;
        }
    }
}

void WakeProgress(const char* msg) { SetHint(msg); }

void InitJob() {
    std::string err;
    SetHint("正在连接…");
    JsonPtr st = RequestJson("/api/status", err);
    if (!st) {
        SetHint(err);
        OfflineShelfJob();
        return;
    }
    ecs::SyncKeyFromServer(false);
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(st.get(), "logged_in"))) {
        LoadShelfJob(false);
    } else {
        LoginJob();
    }
}

/** 返回阅读页图片的请求（翻页 / 字号 / 跳章 / 关弹层）统一走这里 */
void PageJob(const char* toast, std::string path) {
    RunJob(toast, [path]() {
        HttpResult r;
        std::string err;
        if (!Request("POST", path, r, err)) {
            SetToast(err, 3000);
            return;
        }
        SetImage(Mode::Reading, std::move(r));
    });
}

void FetchSettings() {
    std::string err;
    JsonPtr st = RequestJson("/api/settings", err);
    if (!st) return;
    std::lock_guard<std::mutex> lock(S().mu);
    S().font = JInt(st.get(), "font", S().font);
    S().face = JInt(st.get(), "face", S().face);
    S().thoughts = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(st.get(), "hot"));
}

void ClearOfflineMark() {
    s_off.on = false;
    PostUi();
}

void OpenBookJob(std::string id, int h) {
    ClearOfflineMark();
    auto offline = [id]() {
        if (!OpenOffline(id)) SetToast("没有网络，这本书也没有离线缓存", 3500);
    };
    RunJob("正在打开…", [id, h, offline]() {
        s_opening = true;
        struct Done { ~Done() { s_opening = false; } } done;
        HttpResult r;
        std::string err;
        CacheState c = LoadCacheState(id);
        std::string path = "/api/open?fmt=mono1&h=" + std::to_string(h) + "&bookId=" + UrlEncode(id);
        bool synced = false;
        if (c.dirty) {
            // 离线读过：按缓存里那一页的章节地址 + 页序号，把官方进度同步过去
            std::string mt;
            if (ReadAll(CacheDir(id) + "/manifest.json", mt)) {
                JsonPtr m(cJSON_Parse(mt.c_str()), cJSON_Delete);
                const cJSON* pages = m ? cJSON_GetObjectItemCaseSensitive(m.get(), "pages") : nullptr;
                const cJSON* pg = cJSON_IsArray(pages) ? cJSON_GetArrayItem(pages, c.pos) : nullptr;
                if (pg) {
                    path = "/api/open_at?fmt=mono1&h=" + std::to_string(h) + "&href=" + UrlEncode(JStr(pg, "href")) +
                           "&skip=" + std::to_string(JInt(pg, "cp")) + "&bookId=" + UrlEncode(id) +
                           "&i=" + std::to_string(c.pos);
                    synced = true;
                }
            }
        }
        if (synced) SetToast("正在同步离线进度…", 0);
        // 有缓存的书：服务器连不上就直接离线读，不在这里等开云服务器（书架刷新会去开）
        const bool ok = c.n > 0 ? RequestOnce("POST", path, r, err) : Request("POST", path, r, err);
        if (!ok) {
            if (err == "连不上渲染服务" && c.n > 0) {
                offline();
                return;
            }
            SetToast(err, 3000);
            return;
        }
        if (synced) {
            std::lock_guard<std::mutex> lock(s_state_mu);
            c = LoadCacheState(id);
            c.dirty = false;
            SaveCacheState(id, c);
            SetToast("已同步到离线读到的位置", 2500);
        }
        SetImage(Mode::Reading, std::move(r));
        FetchSettings();
    }, offline);
}

void FontsJob() {
    RunJob("正在加载字体…", []() {
        std::string err;
        JsonPtr root = RequestJson("/api/fontfaces", err);
        if (!root) {
            SetToast(err, 3000);
            return;
        }
        std::vector<Chapter> list;
        const cJSON* it = nullptr;
        int i = 0;
        const cJSON* arr = cJSON_GetObjectItemCaseSensitive(root.get(), "faces");
        cJSON_ArrayForEach(it, arr) {
            if (cJSON_IsString(it)) list.push_back({i, it->valuestring, 1});
            ++i;
        }
        {
            std::lock_guard<std::mutex> lock(S().mu);
            S().chapters = std::move(list);
            S().cur_uid = JInt(root.get(), "current");
            S().face = S().cur_uid;
            S().chapters_dirty = true;
            S().mode = Mode::Chapters;
            S().list_fonts = true;
        }
        PostUi();
    });
}

std::atomic<bool> s_toc_offline{false};  // 当前目录列表来自离线缓存（uid = 缓存页号）

/** 离线目录：缓存 manifest 里的 toc（章节名 → 缓存页号） */
bool OfflineToc(const std::string& id) {
    std::string mt;
    if (!ReadAll(CacheDir(id) + "/manifest.json", mt)) return false;
    JsonPtr m(cJSON_Parse(mt.c_str()), cJSON_Delete);
    const cJSON* toc = m ? cJSON_GetObjectItemCaseSensitive(m.get(), "toc") : nullptr;
    if (!cJSON_IsArray(toc) || cJSON_GetArraySize(toc) == 0) return false;
    std::vector<Chapter> chs;
    int cur = 0;
    const cJSON* it = nullptr;
    cJSON_ArrayForEach(it, toc) {
        Chapter c;
        c.uid = JInt(it, "page");
        c.title = JStr(it, "title");
        c.level = 1;
        if (c.uid <= s_off.pos) cur = c.uid;
        chs.push_back(std::move(c));
    }
    {
        std::lock_guard<std::mutex> lock(S().mu);
        S().chapters = std::move(chs);
        S().cur_uid = cur;
        S().chapters_dirty = true;
        S().mode = Mode::Chapters;
        S().list_fonts = false;
    }
    s_toc_offline.store(true);
    PostUi();
    return true;
}

void ChaptersJob() {
    std::string id;
    {
        std::lock_guard<std::mutex> lock(S().mu);
        id = S().book_id;
    }
    if (s_off.on.load()) {
        RunJob(nullptr, [id]() {
            if (!OfflineToc(id)) SetToast("这份缓存没有目录。联网后重新「缓存整本」就有了", 3500);
        });
        return;
    }
    s_toc_offline.store(false);
    RunJob("正在加载目录…", [id]() {
        std::string err;
        JsonPtr root = RequestJson("/api/chapters?bookId=" + UrlEncode(id), err);
        if (!root) {
            SetToast(err, 3000);
            return;
        }
        std::vector<Chapter> chs;
        const cJSON* it = nullptr;
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root.get(), "chapters")) {
            Chapter c;
            c.uid = JInt(it, "uid");
            c.title = JStr(it, "title");
            c.level = JInt(it, "level", 1);
            chs.push_back(std::move(c));
        }
        {
            std::lock_guard<std::mutex> lock(S().mu);
            S().chapters = std::move(chs);
            S().cur_uid = JInt(root.get(), "current");
            S().chapters_dirty = true;
            S().mode = Mode::Chapters;
            S().list_fonts = false;
        }
        PostUi();
    });
}

/** hot=true：本章热门划线；false：我的划线和想法 */
void MarksJob(bool hot) {
    std::string id, title;
    {
        std::lock_guard<std::mutex> lock(S().mu);
        id = S().book_id;
        title = S().book_title;
    }
    RunJob("正在加载划线…", [id, title, hot]() {
        std::string err;
        JsonPtr root = RequestJson(hot ? "/api/hotmarks?bookId=" + UrlEncode(id) : "/api/marks?bookId=" + UrlEncode(id), err);
        if (!root) {
            SetToast(err, 3000);
            return;
        }
        std::vector<TextItem> items;
        const cJSON* it = nullptr;
        if (hot) {
            cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root.get(), "items")) {
                items.push_back({std::to_string(JInt(it, "count")) + " 人划线", JStr(it, "text")});
            }
        } else {
            cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root.get(), "marks")) {
                const bool thought = std::strcmp(JStr(it, "type"), "thought") == 0;
                std::string head = JStr(it, "chapter");
                if (thought) head = head.empty() ? "想法" : head + " · 想法";
                std::string body = JStr(it, "text");
                const char* quote = JStr(it, "quote");
                if (thought && quote[0]) body += std::string("\n「") + quote + "」";
                items.push_back({head, body});
            }
        }
        {
            std::lock_guard<std::mutex> lock(S().mu);
            S().texts_title = hot ? "本章热门划线" : "我的划线 · " + title;
            if (items.empty()) items.push_back({"", hot ? "这一章还没有热门划线" : "这本书还没有划线和想法"});
            S().texts = std::move(items);
            S().texts_dirty = true;
            S().mode = Mode::Texts;
        }
        PostUi();
    });
}

void InitJob();
void LoginJob();

void AccountsJob() {
    RunJob("正在加载账号…", []() {
        std::string err;
        JsonPtr root = RequestJson("/api/accounts", err);
        if (!root) {
            SetToast(err, 3000);
            return;
        }
        std::vector<Account> accs;
        const cJSON* it = nullptr;
        const cJSON* arr = cJSON_GetObjectItemCaseSensitive(root.get(), "accounts");
        cJSON_ArrayForEach(it, arr) {
            Account a;
            a.id = JStr(it, "id");
            a.name = JStr(it, "name");
            a.has_key = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "has_key"));
            accs.push_back(std::move(a));
        }
        {
            std::lock_guard<std::mutex> lock(S().mu);
            S().accounts = std::move(accs);
            S().cur_acc = JStr(root.get(), "current");
            S().accounts_dirty = true;
            S().mode = Mode::Accounts;
        }
        PostUi();
    });
}

void ClearShelfLocked() {
    S().books.clear();
    S().books_dirty = true;
    S().book_id.clear();
    S().book_title.clear();
}

void SwitchAccountJob(std::string id) {
    RunJob("正在切换账号…", [id]() {
        HttpResult r;
        std::string err;
        if (!Request("POST", "/api/account/switch?id=" + UrlEncode(id), r, err)) {
            SetToast(err, 3000);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(S().mu);
            ClearShelfLocked();
            S().mode = Mode::Shelf;
        }
        PostUi();
        InitJob();
    });
}

void AddAccountJob() {
    RunJob("正在新建账号…", []() {
        HttpResult r;
        std::string err;
        if (!Request("POST", "/api/account/add", r, err)) {
            SetToast(err, 3000);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(S().mu);
            ClearShelfLocked();
        }
        LoginJob();
    });
}

void RemoveAccountJob(std::string id) {
    RunJob("正在删除账号…", [id]() {
        HttpResult r;
        std::string err;
        if (!Request("POST", "/api/account/remove?id=" + UrlEncode(id), r, err)) {
            SetToast(err, 3000);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(S().mu);
            ClearShelfLocked();
        }
        HttpResult a;
        // 重新列出账号
        JsonPtr root = RequestJson("/api/accounts", err);
        if (!root) return;
        std::vector<Account> accs;
        const cJSON* it = nullptr;
        const cJSON* arr = cJSON_GetObjectItemCaseSensitive(root.get(), "accounts");
        cJSON_ArrayForEach(it, arr) {
            accs.push_back({JStr(it, "id"), JStr(it, "name"),
                            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "has_key")) != 0});
        }
        {
            std::lock_guard<std::mutex> lock(S().mu);
            S().accounts = std::move(accs);
            S().cur_acc = JStr(root.get(), "current");
            S().accounts_dirty = true;
            S().mode = Mode::Accounts;
        }
        PostUi();
    });
}

/** 长按正文：在网页同一位置点一下，弹出书友想法就显示弹层截图（服务端已放大字号） */
void TapJob(int x, int y) {
    RunJob("正在查看书友想法…", [x, y]() {
        HttpResult r;
        std::string err;
        if (!Request("POST", "/api/tap?x=" + std::to_string(x) + "&y=" + std::to_string(y), r, err)) {
            SetToast(err, 3000);
            return;
        }
        const bool popup = r.popup;
        SetImage(popup ? Mode::Popup : Mode::Reading, std::move(r));
        if (!popup) SetToast("这里没有书友想法，长按带虚线的句子试试", 2500);
    });
}

void PopupScrollJob(int dir) {
    RunJob(nullptr, [dir]() {
        HttpResult r;
        std::string err;
        if (Request("POST", dir > 0 ? "/api/popup/scroll?dir=1" : "/api/popup/scroll?dir=-1", r, err)) {
            SetImage(Mode::Popup, std::move(r));
        }
    });
}

// ---------------------------------------------------------------- 界面

struct Cell {
    lv_image_dsc_t dsc{};
    uint8_t* buf = nullptr;  // 调色板 8 字节 + 封面位图
};

struct Ui {
    lv_obj_t* scr = nullptr;
    lv_coord_t top = 0;
    const lv_font_t* f_body = nullptr;
    const lv_font_t* f_title = nullptr;
    Mode mode = Mode::Shelf;
    // 书架 / 登录
    lv_obj_t* shelf = nullptr;
    lv_obj_t* hint = nullptr;
    lv_obj_t* grid = nullptr;
    lv_obj_t* shelf_pager = nullptr;
    int shelf_page = 0;
    Cell cells[kPerPage];
    // 阅读 / 二维码 / 弹层
    lv_obj_t* img = nullptr;
    lv_image_dsc_t dsc{};
    uint8_t* buf = nullptr;
    lv_obj_t* menu = nullptr;
    lv_obj_t* font_label = nullptr;
    int font_pending = 0;                // 0 = 没有待提交的字号
    lv_timer_t* font_timer = nullptr;
    // 目录 / 划线列表
    lv_obj_t* list = nullptr;
    lv_obj_t* list_title = nullptr;
    lv_obj_t* list_body = nullptr;
    lv_obj_t* list_pager = nullptr;
    lv_coord_t list_h = 0;
    int ch_page = 0;
    int text_start = 0, text_next = 0;
    std::vector<int> text_back;
    // 浮条
    lv_obj_t* toast = nullptr;
    lv_timer_t* timer = nullptr;
    // 书架：缓存状态 / 长按菜单 / 多选
    lv_obj_t* cache_lbl = nullptr;
    lv_obj_t* off_tag = nullptr;   // 阅读页右上角「离线」
    lv_obj_t* badge_box[kPerPage] = {};
    lv_obj_t* badge_lbl[kPerPage] = {};
    std::string badge_id[kPerPage];
    lv_obj_t* sheet = nullptr;
    std::string sheet_book;
    bool select_mode = false;
    std::set<std::string> selected;
    lv_obj_t* select_bar = nullptr;
    lv_obj_t* select_lbl = nullptr;
};

Ui s_ui;

void InitPalette(uint8_t* buf) {
    lv_color32_t* pal = reinterpret_cast<lv_color32_t*>(buf);
    pal[0] = lv_color32_make(0xFF, 0xFF, 0xFF, 0xFF);
    pal[1] = lv_color32_make(0x00, 0x00, 0x00, 0xFF);
}

/**
 * 本页 1bpp 图（阅读页/封面/二维码，0=白 1=黑）的专用解码器：只取反成显示用的 I1（1=白），直接按字节拷进画面。
 * LVGL 默认把索引图先展开成 ARGB8888（整页 1.5MB）再逐像素混合，翻一页光这一步就要约 190ms。
 */
lv_result_t WrImgInfo(lv_image_decoder_t*, lv_image_decoder_dsc_t* dsc, lv_image_header_t* header) {
    if (dsc->src_type != LV_IMAGE_SRC_VARIABLE) return LV_RESULT_INVALID;
    const auto* img = static_cast<const lv_image_dsc_t*>(dsc->src);
    if (img->header.cf != LV_COLOR_FORMAT_I1 || !(img->header.flags & LV_IMAGE_FLAGS_USER1)) return LV_RESULT_INVALID;
    *header = img->header;
    return LV_RESULT_OK;
}

lv_result_t WrImgOpen(lv_image_decoder_t*, lv_image_decoder_dsc_t* dsc) {
    const auto* img = static_cast<const lv_image_dsc_t*>(dsc->src);
    const uint32_t stride = img->header.stride, h = img->header.h;
    const size_t n = static_cast<size_t>(stride) * h;
    if (img->data_size < 8 + n) return LV_RESULT_INVALID;
    auto* db = static_cast<lv_draw_buf_t*>(heap_caps_malloc(sizeof(lv_draw_buf_t), MALLOC_CAP_SPIRAM));
    auto* px = static_cast<uint8_t*>(heap_caps_malloc((n + 3) & ~size_t{3}, MALLOC_CAP_SPIRAM));
    if (!db || !px) {
        heap_caps_free(db);
        heap_caps_free(px);
        return LV_RESULT_INVALID;
    }
    const uint8_t* src = img->data + 8;  // 跳过 2 色调色板
    size_t i = 0;
    if ((reinterpret_cast<uintptr_t>(src) & 3) == 0) {
        for (; i + 4 <= n; i += 4) {
            *reinterpret_cast<uint32_t*>(px + i) = ~*reinterpret_cast<const uint32_t*>(src + i);
        }
    }
    for (; i < n; ++i) px[i] = static_cast<uint8_t>(~src[i]);
    lv_draw_buf_init(db, img->header.w, h, LV_COLOR_FORMAT_I1, stride, px, n);
    dsc->decoded = db;
    dsc->user_data = px;
    return LV_RESULT_OK;
}

void WrImgClose(lv_image_decoder_t*, lv_image_decoder_dsc_t* dsc) {
    heap_caps_free(dsc->user_data);
    heap_caps_free(const_cast<lv_draw_buf_t*>(dsc->decoded));
    dsc->user_data = nullptr;
    dsc->decoded = nullptr;
}

void EnsureWrImgDecoder() {
    static bool done = false;
    if (done) return;
    done = true;
    lv_image_decoder_t* d = lv_image_decoder_create();
    lv_image_decoder_set_info_cb(d, WrImgInfo);
    lv_image_decoder_set_open_cb(d, WrImgOpen);
    lv_image_decoder_set_close_cb(d, WrImgClose);
}

void InitDsc(lv_image_dsc_t& d, uint8_t* buf, int w, int h) {
    d = lv_image_dsc_t{};
    d.header.magic = LV_IMAGE_HEADER_MAGIC;
    d.header.cf = LV_COLOR_FORMAT_I1;
    d.header.flags = LV_IMAGE_FLAGS_USER1;  // 走 WrImg 快速解码
    d.header.w = w;
    d.header.h = h;
    d.header.stride = (w + 7) / 8;
    d.data = buf;
    d.data_size = 8 + d.header.stride * h;
}

lv_obj_t* MakeBox(lv_obj_t* parent) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t* MakeLabel(lv_obj_t* parent, const lv_font_t* f, const char* text = "") {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_black(), 0);
    lv_label_set_text(l, text);
    return l;
}

/** 单行省略：自己截断加「…」，不用 LV_LABEL_LONG_DOT（它会改写 label 内部缓冲） */
void SetTextFit(lv_obj_t* label, const std::string& text, lv_coord_t max_w) {
    const lv_font_t* f = static_cast<const lv_font_t*>(lv_obj_get_style_text_font(label, LV_PART_MAIN));
    auto width = [f](const std::string& t) {
        lv_point_t sz{0, 0};
        lv_text_get_size(&sz, t.c_str(), f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        return sz.x;
    };
    if (width(text) <= max_w) {
        lv_label_set_text(label, text.c_str());
        return;
    }
    std::string t = text;
    while (!t.empty()) {
        size_t cut = t.size() - 1;
        while (cut > 0 && (static_cast<unsigned char>(t[cut]) & 0xC0) == 0x80) --cut;  // 退到 UTF-8 字符开头
        t.resize(cut);
        if (width(t + "…") <= max_w) break;
    }
    lv_label_set_text(label, (t + "…").c_str());
}

lv_obj_t* MakeBtn(lv_obj_t* parent, int w, int h, const char* text, lv_event_cb_t cb, intptr_t id) {
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, lv_color_black(), 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_bg_color(b, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_t* l = MakeLabel(b, s_ui.f_body, text);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, reinterpret_cast<void*>(id));
    HapticAttachClick(b);
    return b;
}

void Invert(lv_obj_t* o, bool on) {
    lv_obj_set_style_bg_color(o, on ? lv_color_black() : lv_color_white(), 0);
    lv_obj_t* l = lv_obj_get_child(o, 0);
    if (l) lv_obj_set_style_text_color(l, on ? lv_color_white() : lv_color_black(), 0);
}

// ---- 书架：3×3 封面网格

void OnCellClick(lv_event_t* e);
void OnCellLong(lv_event_t* e);
void OnShelfPager(lv_event_t* e);

void UpdateBadges() {
    for (int k = 0; k < kPerPage; ++k) {
        if (!s_ui.badge_box[k]) continue;
        const std::string t = CacheBadge(s_ui.badge_id[k], true);
        if (t.empty()) {
            lv_obj_add_flag(s_ui.badge_box[k], LV_OBJ_FLAG_HIDDEN);
        } else {
            if (std::strcmp(lv_label_get_text(s_ui.badge_lbl[k]), t.c_str()) != 0) lv_label_set_text(s_ui.badge_lbl[k], t.c_str());
            lv_obj_clear_flag(s_ui.badge_box[k], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_ui.cache_lbl) {
        std::string cs;
        {
            std::lock_guard<std::mutex> lock(S().mu);
            if (!s_cq_cur.empty()) {
                cs = "缓存中 " + std::to_string(s_cq_cur_n) + "页";
                if (!s_cq.empty()) cs += " (+" + std::to_string(s_cq.size()) + ")";
            }
        }
        if (std::strcmp(lv_label_get_text(s_ui.cache_lbl), cs.c_str()) != 0) lv_label_set_text(s_ui.cache_lbl, cs.c_str());
    }
}

void RebuildGrid() {
    lv_obj_clean(s_ui.grid);
    for (int k = 0; k < kPerPage; ++k) {
        s_ui.badge_box[k] = s_ui.badge_lbl[k] = nullptr;
        s_ui.badge_id[k].clear();
    }
    std::vector<Book> books;
    std::map<std::string, std::string> covers;
    {
        std::lock_guard<std::mutex> lock(S().mu);
        books = S().books;
        covers = S().covers;
    }
    const int pages = std::max(1, static_cast<int>((books.size() + kPerPage - 1) / kPerPage));
    s_ui.shelf_page = std::clamp(s_ui.shelf_page, 0, pages - 1);
    const int first = s_ui.shelf_page * kPerPage;
    for (int k = 0; k < kPerPage && first + k < static_cast<int>(books.size()); ++k) {
        const Book& b = books[first + k];
        lv_obj_t* cell = MakeBox(s_ui.grid);
        lv_obj_set_size(cell, kCellW, kCellH);
        lv_obj_set_pos(cell, (k % kCols) * kCellW, (k / kCols) * kCellH);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(cell);
        lv_obj_add_event_cb(cell, OnCellClick, LV_EVENT_SHORT_CLICKED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(first + k)));
        lv_obj_add_event_cb(cell, OnCellLong, LV_EVENT_LONG_PRESSED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(first + k)));
        const int cx = (kCellW - kCoverW) / 2;
        auto it = covers.find(b.id);
        Cell& c = s_ui.cells[k];
        if (it != covers.end() && c.buf && it->second.size() == static_cast<size_t>(kCoverBytes)) {
            std::memcpy(c.buf + 8, it->second.data(), kCoverBytes);
            InitDsc(c.dsc, c.buf, kCoverW, kCoverH);
            lv_obj_t* im = lv_image_create(cell);
            lv_image_set_src(im, &c.dsc);
            lv_obj_set_pos(im, cx, 0);
        } else {  // 封面还没到：画个框，里面写书名
            lv_obj_t* ph = MakeBox(cell);
            lv_obj_set_size(ph, kCoverW, kCoverH);
            lv_obj_set_pos(ph, cx, 0);
            lv_obj_set_style_border_width(ph, 2, 0);
            lv_obj_set_style_border_color(ph, lv_color_black(), 0);
            lv_obj_t* t = MakeLabel(ph, s_ui.f_body, b.title.c_str());
            lv_obj_set_width(t, kCoverW - 12);
            lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_max_height(t, kCoverH - 12, 0);
            lv_obj_center(t);
        }
        // 缓存角标：封面底部黑条白字（没缓存就藏起来；进度变了 UpdateBadges 原地改字）
        {
            lv_obj_t* bd = MakeBox(cell);
            lv_obj_set_size(bd, kCoverW, 30);
            lv_obj_set_pos(bd, cx, kCoverH - 30);
            lv_obj_set_style_bg_color(bd, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(bd, LV_OPA_COVER, 0);
            lv_obj_t* bl = MakeLabel(bd, s_ui.f_body, "");
            lv_obj_set_style_text_color(bl, lv_color_white(), 0);
            lv_obj_set_align(bl, LV_ALIGN_CENTER);
            s_ui.badge_box[k] = bd;
            s_ui.badge_lbl[k] = bl;
            s_ui.badge_id[k] = b.id;
        }
        // 多选：选中的书加粗框 + 右上角「✓」
        if (s_ui.select_mode) {
            const bool sel = s_ui.selected.count(b.id) > 0;
            lv_obj_t* fr = MakeBox(cell);
            lv_obj_set_size(fr, kCoverW + 8, kCoverH + 8);
            lv_obj_set_pos(fr, cx - 4, -4);
            lv_obj_set_style_border_width(fr, sel ? 5 : 1, 0);
            lv_obj_set_style_border_color(fr, lv_color_black(), 0);
            lv_obj_t* ck = MakeBox(cell);
            lv_obj_set_size(ck, 40, 40);
            lv_obj_set_pos(ck, cx + kCoverW - 44, 4);
            lv_obj_set_style_radius(ck, 20, 0);
            lv_obj_set_style_border_width(ck, 2, 0);
            lv_obj_set_style_border_color(ck, lv_color_black(), 0);
            lv_obj_set_style_bg_color(ck, sel ? lv_color_black() : lv_color_white(), 0);
            lv_obj_set_style_bg_opa(ck, LV_OPA_COVER, 0);
            if (sel) {
                lv_obj_t* cl = MakeLabel(ck, s_ui.f_body, "选");
                lv_obj_set_style_text_color(cl, lv_color_white(), 0);
                lv_obj_center(cl);
            }
        }
        // 进度条
        lv_obj_t* bar = MakeBox(cell);
        lv_obj_set_size(bar, kCoverW, 6);
        lv_obj_set_pos(bar, cx, kCoverH + 6);
        lv_obj_set_style_border_width(bar, 1, 0);
        lv_obj_set_style_border_color(bar, lv_color_black(), 0);
        if (b.progress > 0) {
            lv_obj_t* fill = MakeBox(bar);
            lv_obj_set_size(fill, std::max(2, kCoverW * std::min(b.progress, 100) / 100), 6);
            lv_obj_set_style_bg_color(fill, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
        }
        lv_obj_t* t = MakeLabel(cell, s_ui.f_body);
        lv_obj_set_size(t, kCellW - 8, s_ui.f_body->line_height);
        SetTextFit(t, b.title, kCellW - 8);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(t, 4, kCoverH + 16);
    }
    UpdateBadges();
    lv_obj_t* pl = lv_obj_get_child(s_ui.shelf_pager, 0);
    if (pages > 1) {
        lv_label_set_text_fmt(pl, "‹     %d / %d     ›", s_ui.shelf_page + 1, pages);
        lv_obj_clear_flag(s_ui.shelf_pager, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.shelf_pager, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---- 目录 / 划线列表

void OnChapterClick(lv_event_t* e);

int ChapterRowsPerPage() { return std::max(1, static_cast<int>(s_ui.list_h / kListRowH)); }

bool ListFonts() {
    std::lock_guard<std::mutex> lock(S().mu);
    return S().list_fonts;
}

void RebuildChapters(bool jump_to_current) {
    lv_obj_clean(s_ui.list_body);
    std::vector<Chapter> chs;
    int cur = 0;
    {
        std::lock_guard<std::mutex> lock(S().mu);
        chs = S().chapters;
        cur = S().cur_uid;
    }
    const int per = ChapterRowsPerPage();
    const int pages = std::max(1, static_cast<int>((chs.size() + per - 1) / per));
    if (jump_to_current) {
        for (size_t i = 0; i < chs.size(); ++i) {
            if (chs[i].uid == cur) s_ui.ch_page = static_cast<int>(i) / per;
        }
    }
    s_ui.ch_page = std::clamp(s_ui.ch_page, 0, pages - 1);
    SetTextFit(s_ui.list_title, ListFonts() ? "字体（切换后要重新排版，约 10 秒）" : "目录",
               LV_HOR_RES - kPad * 2 - 96 - 16);
    for (int k = 0; k < per; ++k) {
        const int i = s_ui.ch_page * per + k;
        if (i >= static_cast<int>(chs.size())) break;
        lv_obj_t* row = MakeBox(s_ui.list_body);
        lv_obj_set_size(row, LV_HOR_RES - kPad * 2, kListRowH);
        lv_obj_set_pos(row, 0, k * kListRowH);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(row);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, chs[i].uid == cur ? lv_color_black() : lv_color_white(), 0);
        lv_obj_t* l = MakeLabel(row, s_ui.f_body);
        lv_obj_set_style_text_color(l, chs[i].uid == cur ? lv_color_white() : lv_color_black(), 0);
        const int indent = 6 + std::min(chs[i].level - 1, 3) * 24;
        lv_obj_set_size(l, LV_HOR_RES - kPad * 2 - indent - 6, s_ui.f_body->line_height);
        SetTextFit(l, chs[i].title, LV_HOR_RES - kPad * 2 - indent - 6);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, indent, 0);
        lv_obj_add_event_cb(row, OnChapterClick, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    }
    lv_label_set_text_fmt(lv_obj_get_child(s_ui.list_pager, 0), "‹     %d / %d     ›", s_ui.ch_page + 1, pages);
}

/** 从第 start 条开始排一页文字，直到放不下 */
void RenderTexts(int start) {
    lv_obj_clean(s_ui.list_body);
    std::vector<TextItem> items;
    std::string title;
    {
        std::lock_guard<std::mutex> lock(S().mu);
        items = S().texts;
        title = S().texts_title;
    }
    SetTextFit(s_ui.list_title, title, LV_HOR_RES - kPad * 2 - 96 - 16);
    const lv_coord_t w = LV_HOR_RES - kPad * 2;
    lv_coord_t y = 0;
    int i = start;
    for (; i < static_cast<int>(items.size()); ++i) {
        lv_point_t hs{0, 0}, bs{0, 0};
        if (!items[i].head.empty()) {
            lv_text_get_size(&hs, items[i].head.c_str(), s_ui.f_body, 0, 0, w, LV_TEXT_FLAG_NONE);
        }
        lv_text_get_size(&bs, items[i].body.c_str(), s_ui.f_body, 0, 4, w, LV_TEXT_FLAG_NONE);
        const lv_coord_t h = hs.y + (hs.y ? 6 : 0) + bs.y + 22;
        if (y + h > s_ui.list_h && i > start) break;
        if (!items[i].head.empty()) {
            lv_obj_t* hl = MakeLabel(s_ui.list_body, s_ui.f_body);
            lv_obj_set_width(hl, w);
            SetTextFit(hl, "— " + items[i].head, w);
            lv_obj_set_height(hl, s_ui.f_body->line_height);
            lv_obj_set_pos(hl, 0, y);
            y += hs.y + 6;
        }
        lv_obj_t* bl = MakeLabel(s_ui.list_body, s_ui.f_body, items[i].body.c_str());
        lv_obj_set_width(bl, w);
        lv_obj_set_style_text_line_space(bl, 4, 0);
        lv_label_set_long_mode(bl, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(bl, 0, y);
        y += bs.y + 10;
        lv_obj_t* sep = MakeBox(s_ui.list_body);
        lv_obj_set_size(sep, w, 1);
        lv_obj_set_pos(sep, 0, y);
        lv_obj_set_style_bg_color(sep, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
        y += 12;
    }
    s_ui.text_start = start;
    s_ui.text_next = i;
    const bool more = i < static_cast<int>(items.size());
    lv_label_set_text_fmt(lv_obj_get_child(s_ui.list_pager, 0), "%s     第 %d 页     %s",
                          s_ui.text_back.empty() ? " " : "‹", static_cast<int>(s_ui.text_back.size()) + 1, more ? "›" : " ");
}

void TextsPage(int dir) {
    if (dir > 0) {
        std::lock_guard<std::mutex> lock(S().mu);
        if (s_ui.text_next >= static_cast<int>(S().texts.size())) return;
    }
    if (dir > 0) {
        s_ui.text_back.push_back(s_ui.text_start);
        RenderTexts(s_ui.text_next);
    } else if (!s_ui.text_back.empty()) {
        const int p = s_ui.text_back.back();
        s_ui.text_back.pop_back();
        RenderTexts(p);
    }
}

// ---- 账号列表

void OnAccountRow(lv_event_t* e);
void OnAccountDelete(lv_event_t* e);
void OnAccountAdd(lv_event_t* e);

void RebuildAccounts() {
    lv_obj_clean(s_ui.list_body);
    std::vector<Account> accs;
    std::string cur;
    {
        std::lock_guard<std::mutex> lock(S().mu);
        accs = S().accounts;
        cur = S().cur_acc;
    }
    SetTextFit(s_ui.list_title, "账号", LV_HOR_RES - kPad * 2 - 96 - 16);
    const lv_coord_t w = LV_HOR_RES - kPad * 2;
    constexpr int kRowH = 72;
    int y = 0;
    for (size_t i = 0; i < accs.size() && y + kRowH <= s_ui.list_h - kRowH; ++i) {
        const bool on = accs[i].id == cur;
        lv_obj_t* row = MakeBox(s_ui.list_body);
        lv_obj_set_size(row, w, kRowH);
        lv_obj_set_pos(row, 0, y);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(row);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_black(), 0);
        lv_obj_add_event_cb(row, OnAccountRow, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_obj_t* name = MakeLabel(row, s_ui.f_body);
        lv_obj_set_size(name, w - 130, s_ui.f_body->line_height);
        SetTextFit(name, (on ? "● " : "○ ") + accs[i].name, w - 130);
        lv_obj_set_pos(name, 4, 6);
        lv_obj_t* sub = MakeLabel(row, s_ui.f_body,
                                  on ? (accs[i].has_key ? "当前账号" : "当前账号 · 未设 API Key")
                                     : (accs[i].has_key ? "点击切换" : "点击切换 · 未设 API Key"));
        lv_obj_set_pos(sub, 4, 6 + s_ui.f_body->line_height);
        if (accs.size() > 1) {
            lv_obj_t* del = MakeBtn(row, 100, 46, "删除", OnAccountDelete, static_cast<intptr_t>(i));
            lv_obj_align(del, LV_ALIGN_RIGHT_MID, 0, 0);
        }
        y += kRowH;
    }
    lv_obj_t* add = MakeBtn(s_ui.list_body, w, 60, "+ 添加账号（扫码登录）", OnAccountAdd, 0);
    lv_obj_set_pos(add, 0, y + 16);
    lv_label_set_text(lv_obj_get_child(s_ui.list_pager, 0), "切换后书架会重新加载");
}

// ---- 阅读菜单（点正文中间弹出）

enum MenuId { kMShelf = 1, kMCatalog, kMMine, kMHot, kMFontDown, kMFontUp, kMThoughts, kMClose, kMFace,
              kMCache3, kMCache10, kMCacheAll };

void OnMenu(lv_event_t* e);

void CloseMenu() {
    if (s_ui.menu) {
        lv_obj_delete(s_ui.menu);
        s_ui.menu = nullptr;
    }
    s_ui.font_label = nullptr;
}

void OpenMenu() {
    CloseMenu();
    std::string title;
    int font;
    bool thoughts;
    {
        std::lock_guard<std::mutex> lock(S().mu);
        title = S().book_title;
        font = S().font;
        thoughts = S().thoughts;
    }
    // 全屏透明层：点菜单外面收起
    s_ui.menu = MakeBox(s_ui.scr);
    lv_obj_set_size(s_ui.menu, LV_HOR_RES, LV_VER_RES - s_ui.top);
    lv_obj_set_pos(s_ui.menu, 0, s_ui.top);
    lv_obj_add_flag(s_ui.menu, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_ui.menu, OnMenu, LV_EVENT_CLICKED, reinterpret_cast<void*>(kMClose));

    lv_obj_t* bar = MakeBox(s_ui.menu);
    lv_obj_set_size(bar, LV_HOR_RES, 60);
    lv_obj_set_style_bg_color(bar, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 2, 0);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE);  // 吃掉点击，避免穿透收起
    lv_obj_t* bt = MakeLabel(bar, s_ui.f_title);
    lv_obj_set_size(bt, LV_HOR_RES - kPad * 2, s_ui.f_title->line_height);
    SetTextFit(bt, title, LV_HOR_RES - kPad * 2);
    lv_obj_align(bt, LV_ALIGN_LEFT_MID, kPad, 0);

    lv_obj_t* p = MakeBox(s_ui.menu);
    lv_obj_set_size(p, LV_HOR_RES, kMenuH);
    lv_obj_set_pos(p, 0, LV_VER_RES - s_ui.top - kMenuH);
    lv_obj_set_style_bg_color(p, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(p, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);

    static const struct { const char* t; MenuId id; } kRow1[] = {
        {"书架", kMShelf}, {"目录", kMCatalog}, {"我的划线", kMMine}, {"热门划线", kMHot}};
    const int bw = 104, gap = (LV_HOR_RES - kPad * 2 - bw * 4) / 3;
    for (int i = 0; i < 4; ++i) {
        lv_obj_t* b = MakeBtn(p, bw, 58, kRow1[i].t, OnMenu, kRow1[i].id);
        lv_obj_set_pos(b, kPad + i * (bw + gap), 18);
    }
    lv_obj_t* fl = MakeLabel(p, s_ui.f_body, "字号");
    lv_obj_set_pos(fl, kPad, 100 + 16);
    lv_obj_t* dn = MakeBtn(p, 96, 58, "A-", OnMenu, kMFontDown);
    lv_obj_set_pos(dn, 150, 100);
    lv_obj_t* lv = MakeLabel(p, s_ui.f_title);
    if (s_ui.font_pending > 0) font = s_ui.font_pending;
    lv_label_set_text_fmt(lv, "%d", font);
    s_ui.font_label = lv;
    lv_obj_set_pos(lv, 150 + 96 + 30, 100 + 12);
    lv_obj_t* up = MakeBtn(p, 96, 58, "A+", OnMenu, kMFontUp);
    lv_obj_set_pos(up, LV_HOR_RES - kPad - 96, 100);

    lv_obj_t* tl = MakeLabel(p, s_ui.f_body, "书友想法");
    lv_obj_set_pos(tl, kPad, 166 + 16);
    lv_obj_t* tb = MakeBtn(p, 150, 58, thoughts ? "显示中" : "已隐藏", OnMenu, kMThoughts);
    lv_obj_set_pos(tb, LV_HOR_RES - kPad - 150, 166);
    Invert(tb, thoughts);

    lv_obj_t* fl2 = MakeLabel(p, s_ui.f_body, "字体");
    lv_obj_set_pos(fl2, kPad, 232 + 16);
    lv_obj_t* fb = MakeBtn(p, 270, 58, "选择字体 ›", OnMenu, kMFace);
    lv_obj_set_pos(fb, LV_HOR_RES - kPad - 270, 232);

    std::string ctitle = "离线缓存";
    {
        std::string id;
        {
            std::lock_guard<std::mutex> lock(S().mu);
            id = S().book_id;
        }
        const std::string bd = CacheBadge(id);
        if (!bd.empty()) ctitle = bd;
    }
    lv_obj_t* cl = MakeLabel(p, s_ui.f_body, ctitle.c_str());
    lv_obj_set_pos(cl, kPad, 298 + 16);
    static const struct { const char* t; MenuId id; } kCache[] = {{"3章", kMCache3}, {"10章", kMCache10}, {"整本", kMCacheAll}};
    for (int i = 0; i < 3; ++i) {
        lv_obj_t* b = MakeBtn(p, 86, 58, kCache[i].t, OnMenu, kCache[i].id);
        lv_obj_set_pos(b, LV_HOR_RES - kPad - 86 * (3 - i) - 10 * (2 - i), 298);
    }
}

// ---- 模式切换 / 统一刷新

void CloseSheet();
void ExitSelectMode();

void ShowMode(Mode m) {
    if (m != Mode::Reading) CloseMenu();
    if (m != Mode::Shelf) {
        CloseSheet();
        if (s_ui.select_mode) {
            s_ui.select_mode = false;
            s_ui.selected.clear();
            if (s_ui.select_bar) {
                lv_obj_delete_async(s_ui.select_bar);
                s_ui.select_bar = nullptr;
                s_ui.select_lbl = nullptr;
            }
        }
    }
    s_ui.mode = m;
    const bool shelf = m == Mode::Shelf || m == Mode::Login;
    const bool img = m == Mode::Reading || m == Mode::Popup || m == Mode::Login;
    const bool list = m == Mode::Chapters || m == Mode::Texts || m == Mode::Accounts;
    auto vis = [](lv_obj_t* o, bool on) {
        if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    };
    vis(s_ui.shelf, shelf);
    vis(s_ui.grid, m == Mode::Shelf);
    if (m == Mode::Login) vis(s_ui.shelf_pager, false);
    vis(s_ui.img, img);
    vis(s_ui.list, list);
    if (m == Mode::Reading || m == Mode::Popup) lv_obj_set_pos(s_ui.img, 0, s_ui.top);
}

void ApplyUi(void*) {
    if (!s_active.load() || s_ui.scr == nullptr) return;
    std::string hint, toast, img;
    int w = 0, h = 0;
    bool has_img = false, books = false, covers = false, chapters = false, texts = false, accounts = false,
         cache_prog = false;
    Mode mode;
    {
        std::lock_guard<std::mutex> lock(S().mu);
        hint = S().hint;
        toast = S().toast;
        mode = S().mode;
        std::swap(books, S().books_dirty);
        std::swap(covers, S().covers_dirty);
        std::swap(chapters, S().chapters_dirty);
        std::swap(texts, S().texts_dirty);
        std::swap(accounts, S().accounts_dirty);
        std::swap(cache_prog, S().cache_dirty);
        if (S().img_pending) {
            img.swap(S().img);
            w = S().img_w;
            h = S().img_h;
            has_img = true;
            S().img_pending = false;
        }
    }
    lv_label_set_text(s_ui.hint, hint.c_str());
    if (s_ui.off_tag) {
        const bool show = s_off.on.load() && mode == Mode::Reading;
        if (show != !lv_obj_has_flag(s_ui.off_tag, LV_OBJ_FLAG_HIDDEN)) {
            if (show) lv_obj_clear_flag(s_ui.off_tag, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(s_ui.off_tag, LV_OBJ_FLAG_HIDDEN);
        }
        if (show) lv_obj_move_foreground(s_ui.off_tag);
    }
    if (toast.empty()) {
        lv_obj_add_flag(s_ui.toast, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_ui.toast, toast.c_str());
        lv_obj_clear_flag(s_ui.toast, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_ui.toast);
    }
    if (has_img && s_ui.buf) {
        if (s_flip_t0) ESP_LOGD(TAG, "flip timing: apply at +%dms", (int)((esp_timer_get_time() - s_flip_t0) / 1000));
        std::memcpy(s_ui.buf + 8, img.data(), img.size());
        InitDsc(s_ui.dsc, s_ui.buf, w, h);
        lv_image_set_src(s_ui.img, &s_ui.dsc);
        lv_obj_set_size(s_ui.img, w, h);
        if (mode == Mode::Login) lv_obj_set_pos(s_ui.img, (LV_HOR_RES - w) / 2, s_ui.top + 150);
        lv_obj_invalidate(s_ui.img);
    }
    if (mode != s_ui.mode) ShowMode(mode);
    if ((books || covers) && s_ui.mode == Mode::Shelf) RebuildGrid();
    else if (cache_prog && s_ui.mode == Mode::Shelf) UpdateBadges();
    if (chapters) RebuildChapters(true);
    if (accounts) RebuildAccounts();
    if (texts) {
        s_ui.text_back.clear();
        RenderTexts(0);
    }
}

void OnTimer(lv_timer_t*) {
    bool clear = false;
    {
        std::lock_guard<std::mutex> lock(S().mu);
        if (!S().toast.empty() && S().toast_until_us != 0 && esp_timer_get_time() > S().toast_until_us) {
            S().toast.clear();
            S().toast_until_us = 0;
            clear = true;
        }
    }
    if (clear) ApplyUi(nullptr);
}

// ---------------------------------------------------------------- 动作

int ReadH() { return kMaxImgH - s_ui.top; }

void OpenBook(int idx) {
    std::string id;
    {
        std::lock_guard<std::mutex> lock(S().mu);
        if (idx < 0 || idx >= static_cast<int>(S().books.size())) return;
        id = S().books[idx].id;
        S().book_id = id;
        S().book_title = S().books[idx].title;
    }
    // 没网，或者后台正忙着连网（刷新书架、开云服务器可能要几十秒）：有缓存就直接离线打开，别让点击没反应
    const bool no_net = net_mode::Offline() || !Board::GetInstance().IsNetworkReady();
    if ((no_net || s_busy.load()) && !s_opening.load()) {
        ESP_LOGI(TAG, "open %s locally (no_net=%d busy=%d)", id.c_str(), no_net, s_busy.load());
        RunJobGated(s_local_busy, nullptr, [id]() {
            ClearOfflineMark();
            if (!OpenOffline(id)) SetToast(s_busy.load() ? "正在联网，请稍候再点" : "没有网络，这本书也没有离线缓存", 3500);
        }, nullptr, false);
        return;
    }
    OpenBookJob(id, ReadH());
}

void Flip(int dir) {
    if (s_ui.mode != Mode::Reading) return;
    if (s_off.on) {
        OfflineFlip(dir);
        return;
    }
    PageJob(nullptr, dir > 0 ? "/api/flip/next?fmt=mono1" : "/api/flip/prev?fmt=mono1");
}

void BackToShelf() {
    SetMode(Mode::Shelf);
    ShowMode(Mode::Shelf);
    RebuildGrid();
    if (s_off.on) ClearOfflineMark();
    RunJob(nullptr, []() { LoadShelfJob(false); }, OfflineShelfJob);  // 刷新进度
}

void BackToReading() {
    {
        std::lock_guard<std::mutex> lock(S().mu);
        S().mode = Mode::Reading;
    }
    ShowMode(Mode::Reading);
}

void SetFont(int level) {
    level = std::clamp(level, 1, 7);
    RunJob("正在调整字号…", [level]() {
        HttpResult r;
        std::string err;
        if (!Request("POST", "/api/font?fmt=mono1&level=" + std::to_string(level), r, err)) {
            SetToast(err, 3000);
            FetchSettings();  // 失败时以服务端为准，避免设备显示的字号和实际不一致
            return;
        }
        {
            std::lock_guard<std::mutex> lock(S().mu);
            S().font = level;
        }
        SetImage(Mode::Reading, std::move(r));
    });
}

/** A+/A-：先只改菜单里的数字，停手 1.2 秒后收起菜单、只提交最终字号（每次改字号服务端要重排十几秒） */
void FontTimerCb(lv_timer_t*) {
    s_ui.font_timer = nullptr;
    const int level = s_ui.font_pending;
    s_ui.font_pending = 0;
    int cur;
    {
        std::lock_guard<std::mutex> lock(S().mu);
        cur = S().font;
    }
    CloseMenu();
    if (level > 0 && level != cur) SetFont(level);
}

void BumpFont(int delta) {
    int base = s_ui.font_pending;
    if (base == 0) {
        std::lock_guard<std::mutex> lock(S().mu);
        base = S().font;
    }
    s_ui.font_pending = std::clamp(base + delta, 1, 7);
    if (s_ui.font_label) lv_label_set_text_fmt(s_ui.font_label, "%d", s_ui.font_pending);
    if (s_ui.font_timer) lv_timer_delete(s_ui.font_timer);
    s_ui.font_timer = lv_timer_create(FontTimerCb, 1200, nullptr);
    lv_timer_set_repeat_count(s_ui.font_timer, 1);
}

void SetThoughts(bool on) {
    {
        std::lock_guard<std::mutex> lock(S().mu);
        S().thoughts = on;
    }
    PageJob(on ? "正在显示书友想法…" : "正在隐藏书友想法…", std::string("/api/hot?fmt=mono1&on=") + (on ? "1" : "0"));
}

void SetFontFace(int idx) {
    BackToReading();
    PageJob("正在切换字体…", "/api/fontface?fmt=mono1&i=" + std::to_string(idx));
    std::lock_guard<std::mutex> lock(S().mu);
    S().face = idx;
}

void GotoChapter(int idx) {
    std::string title;
    int page = 0;
    {
        std::lock_guard<std::mutex> lock(S().mu);
        if (idx < 0 || idx >= static_cast<int>(S().chapters.size())) return;
        title = S().chapters[idx].title;
        page = S().chapters[idx].uid;
    }
    if (s_toc_offline.load() && s_off.on.load()) {
        BackToReading();
        RunJob(nullptr, [page]() { OfflineJump(page); });
        return;
    }
    BackToReading();
    PageJob("正在跳转…", "/api/goto?fmt=mono1&title=" + UrlEncode(title));
}

void ListPage(int dir) {
    if (s_ui.mode == Mode::Chapters) {
        s_ui.ch_page += dir;
        RebuildChapters(false);
    } else if (s_ui.mode == Mode::Texts) {
        TextsPage(dir);
    }
}

void ShelfPage(int dir) {
    s_ui.shelf_page += dir;
    RebuildGrid();
}

// ---------------------------------------------------------------- 事件

void CloseSheet() {
    if (s_ui.sheet) {
        lv_obj_delete_async(s_ui.sheet);  // 可能在它自己的按钮回调里调用
        s_ui.sheet = nullptr;
    }
}

void UpdateSelectBar() {
    if (!s_ui.select_mode) {
        if (s_ui.select_bar) {
            lv_obj_delete_async(s_ui.select_bar);
            s_ui.select_bar = nullptr;
            s_ui.select_lbl = nullptr;
        }
        return;
    }
    if (s_ui.select_lbl) lv_label_set_text_fmt(s_ui.select_lbl, "已选 %d 本", static_cast<int>(s_ui.selected.size()));
}

enum SheetId { kSCache3 = 1, kSCache10, kSCacheAll, kSDelete, kSMulti, kSClose, kSelAll, kSel3, kSelCancel };

void OnSheet(lv_event_t* e);

void EnterSelectMode() {
    CloseSheet();
    s_ui.select_mode = true;
    s_ui.selected.clear();
    if (!s_ui.sheet_book.empty()) s_ui.selected.insert(s_ui.sheet_book);
    // 底部操作条，盖在翻页条上
    s_ui.select_bar = MakeBox(s_ui.scr);
    lv_obj_set_size(s_ui.select_bar, LV_HOR_RES, 76);
    lv_obj_set_pos(s_ui.select_bar, 0, LV_VER_RES - 76);
    lv_obj_set_style_bg_color(s_ui.select_bar, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_ui.select_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(s_ui.select_bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(s_ui.select_bar, 2, 0);
    lv_obj_add_flag(s_ui.select_bar, LV_OBJ_FLAG_CLICKABLE);
    s_ui.select_lbl = MakeLabel(s_ui.select_bar, s_ui.f_body);
    lv_obj_set_pos(s_ui.select_lbl, kPad, 24);
    lv_obj_t* b1 = MakeBtn(s_ui.select_bar, 112, 54, "缓存3章", OnSheet, kSel3);
    lv_obj_set_pos(b1, 132, 11);
    lv_obj_t* b2 = MakeBtn(s_ui.select_bar, 112, 54, "缓存整本", OnSheet, kSelAll);
    lv_obj_set_pos(b2, 132 + 118, 11);
    lv_obj_t* b3 = MakeBtn(s_ui.select_bar, 92, 54, "取消", OnSheet, kSelCancel);
    lv_obj_set_pos(b3, LV_HOR_RES - kPad - 92, 11);
    UpdateSelectBar();
    RebuildGrid();
}

void ExitSelectMode() {
    s_ui.select_mode = false;
    s_ui.selected.clear();
    UpdateSelectBar();
    RebuildGrid();
}

std::vector<std::pair<std::string, std::string>> PickBooks(const std::set<std::string>& ids) {
    std::vector<std::pair<std::string, std::string>> out;
    std::lock_guard<std::mutex> lock(S().mu);
    for (const auto& b : S().books) {
        if (ids.count(b.id)) out.emplace_back(b.id, b.title);
    }
    return out;
}

void SheetAction(int id) {
    const std::string book = s_ui.sheet_book;
    switch (id) {
        case kSCache3:
        case kSCache10:
        case kSCacheAll: {
            CloseSheet();
            const int ch = id == kSCache3 ? 3 : (id == kSCache10 ? 10 : 0);
            const int n = EnqueueCache(PickBooks({book}), ch, ReadH());
            SetToast(n ? "已加入缓存队列" : "这本已经在缓存了", 2000);
            RebuildGrid();
            break;
        }
        case kSDelete:
            CloseSheet();
            if (book == s_cq_cur) {
                SetToast("正在缓存这本，等它完成再删", 2500);
                break;
            }
            DeleteCache(book);
            SetToast("已删除这本的离线缓存", 2000);
            RebuildGrid();
            break;
        case kSMulti: EnterSelectMode(); break;
        case kSelAll:
        case kSel3: {
            if (s_ui.selected.empty()) {
                SetToast("先点封面选书", 2000);
                break;
            }
            const int n = EnqueueCache(PickBooks(s_ui.selected), id == kSel3 ? 3 : 0, ReadH());
            char t[64];
            std::snprintf(t, sizeof(t), "已加入缓存队列：%d 本", n);
            SetToast(t, 2500);
            ExitSelectMode();
            break;
        }
        case kSelCancel: ExitSelectMode(); break;
        default: CloseSheet(); break;
    }
}

void OnSheet(lv_event_t* e) { SheetAction(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)))); }

void OpenSheet(int idx) {
    std::string id, title;
    {
        std::lock_guard<std::mutex> lock(S().mu);
        if (idx < 0 || idx >= static_cast<int>(S().books.size())) return;
        id = S().books[idx].id;
        title = S().books[idx].title;
    }
    CloseSheet();
    s_ui.sheet_book = id;
    s_ui.sheet = MakeBox(s_ui.scr);
    lv_obj_set_size(s_ui.sheet, LV_HOR_RES, LV_VER_RES - s_ui.top);
    lv_obj_set_pos(s_ui.sheet, 0, s_ui.top);
    lv_obj_add_flag(s_ui.sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_ui.sheet, OnSheet, LV_EVENT_CLICKED, reinterpret_cast<void*>(kSClose));
    constexpr int kSheetH = 250;
    lv_obj_t* p = MakeBox(s_ui.sheet);
    lv_obj_set_size(p, LV_HOR_RES, kSheetH);
    lv_obj_set_pos(p, 0, LV_VER_RES - s_ui.top - kSheetH);
    lv_obj_set_style_bg_color(p, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(p, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* t = MakeLabel(p, s_ui.f_title);
    lv_obj_set_size(t, LV_HOR_RES - kPad * 2, s_ui.f_title->line_height);
    SetTextFit(t, title, LV_HOR_RES - kPad * 2);
    lv_obj_set_pos(t, kPad, 14);
    std::string st = CacheBadge(id);
    lv_obj_t* sl = MakeLabel(p, s_ui.f_body, st.empty() ? "离线缓存：还没有" : ("离线缓存：" + st).c_str());
    lv_obj_set_pos(sl, kPad, 58);
    static const struct { const char* t; int id; } kRow[] = {{"缓存3章", kSCache3}, {"缓存10章", kSCache10}, {"缓存整本", kSCacheAll}};
    const int bw = 136, gap = (LV_HOR_RES - kPad * 2 - bw * 3) / 2;
    for (int i = 0; i < 3; ++i) {
        lv_obj_t* b = MakeBtn(p, bw, 58, kRow[i].t, OnSheet, kRow[i].id);
        lv_obj_set_pos(b, kPad + i * (bw + gap), 96);
    }
    lv_obj_t* del = MakeBtn(p, 200, 58, "删除缓存", OnSheet, kSDelete);
    lv_obj_set_pos(del, kPad, 172);
    lv_obj_t* multi = MakeBtn(p, 200, 58, "多选几本 ›", OnSheet, kSMulti);
    lv_obj_set_pos(multi, LV_HOR_RES - kPad - 200, 172);
}

void OnCellLong(lv_event_t* e) {
    if (s_ui.select_mode) return;
    HapticPulseIfEnabled();
    OpenSheet(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
}

void OnCellClick(lv_event_t* e) {
    const int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (s_ui.select_mode) {
        std::string id;
        {
            std::lock_guard<std::mutex> lock(S().mu);
            if (idx < 0 || idx >= static_cast<int>(S().books.size())) return;
            id = S().books[idx].id;
        }
        if (!s_ui.selected.erase(id)) s_ui.selected.insert(id);
        UpdateSelectBar();
        RebuildGrid();
        return;
    }
    OpenBook(idx);
}

void OnChapterClick(lv_event_t* e) {
    const int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (ListFonts()) {
        int uid = idx;
        {
            std::lock_guard<std::mutex> lock(S().mu);
            if (idx >= 0 && idx < static_cast<int>(S().chapters.size())) uid = S().chapters[idx].uid;
        }
        SetFontFace(uid);
    } else {
        GotoChapter(idx);
    }
}

int TapX() {
    lv_point_t p{0, 0};
    if (lv_indev_t* in = lv_indev_active()) lv_indev_get_point(in, &p);
    return p.x;
}

int TapY() {
    lv_point_t p{0, 0};
    if (lv_indev_t* in = lv_indev_active()) lv_indev_get_point(in, &p);
    return p.y;
}

void OnShelfPager(lv_event_t*) { ShelfPage(TapX() < LV_HOR_RES / 2 ? -1 : 1); }

void OnListPager(lv_event_t*) { ListPage(TapX() < LV_HOR_RES / 2 ? -1 : 1); }

void BackFromList() {
    if (s_ui.mode == Mode::Accounts) {
        BackToShelf();
    } else {
        BackToReading();
    }
}

void OnListBack(lv_event_t*) { BackFromList(); }

std::string AccountIdAt(lv_event_t* e) {
    const int i = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    std::lock_guard<std::mutex> lock(S().mu);
    return (i >= 0 && i < static_cast<int>(S().accounts.size())) ? S().accounts[i].id : std::string();
}

void OnAccountRow(lv_event_t* e) {
    const std::string id = AccountIdAt(e);
    std::string cur;
    {
        std::lock_guard<std::mutex> lock(S().mu);
        cur = S().cur_acc;
    }
    if (id.empty()) return;
    if (id == cur) {
        BackToShelf();
    } else {
        SwitchAccountJob(id);
    }
}

void OnAccountDelete(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    const std::string id = AccountIdAt(e);
    if (!id.empty()) RemoveAccountJob(id);
}

void OnAccountAdd(lv_event_t*) { AddAccountJob(); }

void OnAccountsBtn(lv_event_t*) { AccountsJob(); }

void MenuAction(int id) {
    int font;
    bool thoughts;
    {
        std::lock_guard<std::mutex> lock(S().mu);
        font = S().font;
        thoughts = S().thoughts;
    }
    if (id == kMFontDown || id == kMFontUp) {  // 菜单保持打开，只改数字
        BumpFont(id == kMFontUp ? 1 : -1);
        return;
    }
    CloseMenu();
    switch (id) {
        case kMShelf: BackToShelf(); break;
        case kMCatalog: ChaptersJob(); break;
        case kMMine: MarksJob(false); break;
        case kMHot: MarksJob(true); break;
        case kMFace: FontsJob(); break;
        case kMCache3: CacheJob(3, ReadH()); break;
        case kMCache10: CacheJob(10, ReadH()); break;
        case kMCacheAll: CacheJob(0, ReadH()); break;
        case kMThoughts: SetThoughts(!thoughts); break;
        default: break;
    }
}

void OnMenu(lv_event_t* e) { MenuAction(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)))); }

void OnImgEvent(lv_event_t* e) {
    const lv_event_code_t code = lv_event_get_code(e);
    const int x = TapX(), y = TapY();
    if (s_ui.mode == Mode::Popup) {
        if (code != LV_EVENT_SHORT_CLICKED) return;
        const int third = (LV_VER_RES - s_ui.top) / 3;
        if (y < s_ui.top + third) {
            PopupScrollJob(-1);
        } else if (y > s_ui.top + third * 2) {
            PopupScrollJob(1);
        } else {
            PageJob(nullptr, "/api/dismiss?fmt=mono1");
        }
        return;
    }
    if (s_ui.mode != Mode::Reading) return;
    if (code == LV_EVENT_LONG_PRESSED) {
        TapJob(x, y - s_ui.top);  // 图片坐标 = 网页坐标
        return;
    }
    if (code != LV_EVENT_SHORT_CLICKED) return;
    if (x < LV_HOR_RES / 3) {
        Flip(-1);
    } else if (x > LV_HOR_RES * 2 / 3) {
        Flip(1);
    } else {
        OpenMenu();
    }
}

bool OnBootClick();
void BootStep();

/** 按键回调可能来自按键任务（不在 LVGL 线程）：界面操作一律投递到 LVGL 线程执行，避免踩坏 LVGL 状态死机 */
void VkKeyOnLv(void* arg);

bool OnVkKey(const char* key) {
    if (key == nullptr) return false;
    const Mode mode = s_ui.mode;
    intptr_t code = 0;
    if (std::strcmp(key, "vk_home") == 0) {
        if (mode == Mode::Shelf || mode == Mode::Login) return false;  // 书架/登录页：交给系统回桌面
        code = 3;
    } else if (std::strcmp(key, "vk_next") == 0) {
        code = 1;
    } else if (std::strcmp(key, "vk_prev") == 0) {
        code = 2;
    } else {
        return false;
    }
    ScreenLvAsync(VkKeyOnLv, reinterpret_cast<void*>(code));
    return true;
}

void VkKeyOnLv(void* arg) {
    if (!s_active.load() || s_ui.scr == nullptr) return;
    const intptr_t code = reinterpret_cast<intptr_t>(arg);
    if (code == 3) {
        BootStep();
        return;
    }
    const int dir = code == 1 ? 1 : -1;
    switch (s_ui.mode) {
        case Mode::Reading: CloseMenu(); Flip(dir); break;
        case Mode::Popup: PopupScrollJob(dir); break;
        case Mode::Shelf: ShelfPage(dir); break;
        case Mode::Chapters:
        case Mode::Texts: ListPage(dir); break;
        default: break;
    }
}

/** 逐级返回（须在 LVGL 线程） */
void BootStep() {
    if (s_ui.sheet) {
        CloseSheet();
        return;
    }
    if (s_ui.select_mode) {
        ExitSelectMode();
        return;
    }
    switch (s_ui.mode) {
        case Mode::Reading:
            if (s_ui.menu) CloseMenu();
            else BackToShelf();
            break;
        case Mode::Popup: PageJob(nullptr, "/api/dismiss?fmt=mono1"); break;
        case Mode::Chapters:
        case Mode::Texts: BackToReading(); break;
        case Mode::Accounts: BackToShelf(); break;
        case Mode::Login:
            s_gen.fetch_add(1);
            RunJob(nullptr, LoginJob);
            break;
        default: break;
    }
}

void BootStepOnLv(void*) {
    if (s_active.load() && s_ui.scr != nullptr) BootStep();
}

/** BOOT 短按：来自按键任务，投递到 LVGL 线程再处理 */
bool OnBootClick() {
    if (s_ui.mode == Mode::Shelf) return false;
    ScreenLvAsync(BootStepOnLv);
    return true;
}

void OnLifecycle(lv_event_t* e) {
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SCREEN_LOADED) {
        s_active.store(true);
        if (auto* disp = LVAdapterDisplay::Instance()) {
            disp->SetStatusTitlePrefix(nullptr);
            disp->SetIdleStatusMode(IdleStatusMode::kClock);
            disp->UpdateStatusBar(true);
        }
        PowerPolicy::GetInstance().Acquire(PowerNeed::UiKeepNet);  // 阅读时保网，翻页不用重连
        const Cfg cfg = GetCfg();
        if (cfg.server.empty() || cfg.token.empty()) {
            lv_label_set_text(s_ui.hint, "未配置：SD 卡 metalio/e-ink/weread.json\n需要 server 和 token 两项");
        } else {
            RunJob(nullptr, InitJob, OfflineShelfJob);
        }
    } else if (code == LV_EVENT_SCREEN_UNLOADED) {
        if (lv_event_get_target(e) != s_ui.scr) return;
        s_active.store(false);
        s_gen.fetch_add(1);
        PowerPolicy::GetInstance().Release(PowerNeed::UiKeepNet);
        // 读完退出：后台刷新待机信息卡，让「在读」马上换成这本
        xTaskCreateWithCaps(
            [](void*) {
                vTaskDelay(pdMS_TO_TICKS(1500));
                {
                    PowerNeedHold hold(PowerNeed::OtaDownload);
                    StandbyCard_FetchBlocking();
                }
                vTaskDeleteWithCaps(nullptr);
            },
            "card_refresh", 8192, nullptr, 2, nullptr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_ui.timer) lv_timer_delete(s_ui.timer);
        if (s_ui.font_timer) lv_timer_delete(s_ui.font_timer);
        if (s_ui.buf) heap_caps_free(s_ui.buf);
        for (auto& c : s_ui.cells) {
            if (c.buf) heap_caps_free(c.buf);
        }
        s_ui = Ui{};
    }
}

}  // namespace

// ---------------------------------------------------------------- 对外

bool WeReadScreen::IsActive() { return s_active.load(); }

void WeReadScreen::OnNetworkBack() {
    if (!s_active.load()) return;  // 不在本页：下次进书架时自然会接着下
    ScreenLvAsync([](void*) {
        if (!s_active.load() || s_ui.scr == nullptr) return;
        if (s_ui.mode == Mode::Shelf) RunJob(nullptr, []() { LoadShelfJob(false); }, OfflineShelfJob);
        else RunJob(nullptr, []() { LoadShelfJob(false); });  // 读书时只在后台恢复缓存队列，不切页面
    });
}

/** 调试命令：OPEN n / NEXT / PREV / MENU / M n(菜单项) / CH / PICK n(目录项) / LONG x y / TAP x y / BACK / PAGE ±1 */
void WeReadScreen::DebugCmd(const char* cmd) {
    if (!s_active.load() || cmd == nullptr) return;
    int a = 0, b = 0;
    if (std::sscanf(cmd, "OPEN %d", &a) == 1) {
        OpenBook(a);
    } else if (std::strcmp(cmd, "NEXT") == 0 || std::strcmp(cmd, "PREV") == 0) {
        OnVkKey(cmd[0] == 'N' ? "vk_next" : "vk_prev");
    } else if (std::strcmp(cmd, "MENU") == 0) {
        if (s_ui.mode == Mode::Reading) OpenMenu();
    } else if (std::sscanf(cmd, "M %d", &a) == 1) {
        MenuAction(a);
    } else if (std::sscanf(cmd, "PICK %d", &a) == 1) {
        GotoChapter(a);
    } else if (std::sscanf(cmd, "LONG %d %d", &a, &b) == 2) {
        if (s_ui.mode == Mode::Reading) TapJob(a, b - s_ui.top);
    } else if (std::sscanf(cmd, "SHEET %d", &a) == 1) {  // 调试：长按第 a 本 / SA n = 点菜单项 / SEL a = 多选点第 a 本
        if (s_ui.mode == Mode::Shelf) OpenSheet(a);
    } else if (std::sscanf(cmd, "SA %d", &a) == 1) {
        SheetAction(a);
    } else if (std::sscanf(cmd, "SEL %d", &a) == 1) {
        if (s_ui.select_mode) {
            std::string id;
            {
                std::lock_guard<std::mutex> lock(S().mu);
                if (a >= 0 && a < static_cast<int>(S().books.size())) id = S().books[a].id;
            }
            if (!id.empty() && !s_ui.selected.erase(id)) s_ui.selected.insert(id);
            UpdateSelectBar();
            RebuildGrid();
        }
    } else if (std::strcmp(cmd, "ACC") == 0) {
        AccountsJob();
    } else if (std::sscanf(cmd, "ACCSEL %d", &a) == 1) {
        std::string id;
        {
            std::lock_guard<std::mutex> lock(S().mu);
            if (a >= 0 && a < static_cast<int>(S().accounts.size())) id = S().accounts[a].id;
        }
        if (!id.empty()) SwitchAccountJob(id);
    } else if (std::strcmp(cmd, "BACK") == 0) {
        OnBootClick();
    } else if (std::sscanf(cmd, "NET %d", &a) == 1) {
        s_dbg_offline.store(a == 0);
        ESP_LOGW(TAG, "debug offline=%d", a == 0 ? 1 : 0);
    } else if (std::strncmp(cmd, "PHASH ", 6) == 0) {
        // 调试：本地缓存页 CRC32，和服务端逐页比对。PHASH <bookId> <from> <n>
        char bid[40] = {};
        int from = 0, n = 0;
        if (std::sscanf(cmd + 6, "%39s %d %d", bid, &from, &n) == 3) {
            std::string line;
            for (int i = from; i < from + n; ++i) {
                char name[24];
                std::snprintf(name, sizeof(name), "/p%05d.bin", i);
                std::string d;
                char h[12];
                if (ReadAll(CacheDir(bid) + name, d)) {
                    std::snprintf(h, sizeof(h), "%08x ", static_cast<unsigned>(esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(d.data()), d.size())));
                } else {
                    std::snprintf(h, sizeof(h), "-------- ");
                }
                line += h;
                if ((i - from) % 16 == 15 || i == from + n - 1) {
                    ESP_LOGW("PHASH", "%d %s", i - (i - from) % 16, line.c_str());
                    line.clear();
                }
            }
            const CacheState c = LoadCacheState(bid);
            ESP_LOGW("PHASH", "state n=%d pos=%d done=%d", c.n, c.pos, c.done ? 1 : 0);
        }
    } else if (std::strcmp(cmd, "CACHEST") == 0) {
        std::string id;
        {
            std::lock_guard<std::mutex> lock(S().mu);
            id = S().book_id;
        }
        const CacheState c = LoadCacheState(id);
        ESP_LOGW(TAG, "cache %s n=%d pos=%d dirty=%d done=%d off=%d busy=%d", id.c_str(), c.n, c.pos, c.dirty ? 1 : 0,
                 c.done ? 1 : 0, s_off.on ? 1 : 0, s_cache_sync_busy.load() ? 1 : 0);
    } else if (std::sscanf(cmd, "PAGE %d", &a) == 1) {
        OnVkKey(a > 0 ? "vk_next" : "vk_prev");
    }
}

lv_obj_t* WeReadScreen::Create() {
    ESP_LOGI(TAG, "launch WeReadScreen (last reset reason=%d)", static_cast<int>(esp_reset_reason()));
    {
        Cfg cfg = LoadCfg();
        std::lock_guard<std::mutex> lock(S().mu);
        S().cfg = std::move(cfg);
        S().mode = Mode::Shelf;
        S().hint.clear();
        S().toast.clear();
        S().img_pending = false;
        S().books_dirty = !S().books.empty();
    }
    s_ui = Ui{};
    s_ui.f_body = fontpack_lv_font_ui();
    s_ui.f_title = fontpack_lv_font_get(30, 4);
    if (s_ui.f_title == nullptr) s_ui.f_title = s_ui.f_body;

    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    s_ui.scr = scr;
    EpdStatusBar status = ScreenCreateStatusBar(scr);
    s_ui.top = status.height;

    // ---- 书架 / 登录
    s_ui.shelf = MakeBox(scr);
    lv_obj_set_size(s_ui.shelf, LV_HOR_RES, LV_VER_RES - s_ui.top);
    lv_obj_set_pos(s_ui.shelf, 0, s_ui.top);
    lv_obj_t* title = MakeLabel(s_ui.shelf, s_ui.f_title, "微信读书");
    lv_obj_t* acc_btn = MakeBtn(s_ui.shelf, 96, 44, "账号", OnAccountsBtn, 0);
    lv_obj_set_pos(acc_btn, LV_HOR_RES - kPad - 96, 6);
    s_ui.cache_lbl = MakeLabel(s_ui.shelf, s_ui.f_body);
    lv_obj_set_pos(s_ui.cache_lbl, 150, 18);
    lv_obj_set_pos(title, kPad, 12);
    s_ui.hint = MakeLabel(s_ui.shelf, s_ui.f_body);
    lv_obj_set_width(s_ui.hint, LV_HOR_RES - kPad * 2);
    lv_label_set_long_mode(s_ui.hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_ui.hint, kPad, kHeaderH + 8);
    s_ui.grid = MakeBox(s_ui.shelf);
    lv_obj_set_size(s_ui.grid, kCellW * kCols, kCellH * 3);
    lv_obj_set_pos(s_ui.grid, 0, kHeaderH + 4);
    s_ui.shelf_pager = MakeBox(s_ui.shelf);
    lv_obj_set_size(s_ui.shelf_pager, LV_HOR_RES, kPagerH);
    lv_obj_set_pos(s_ui.shelf_pager, 0, LV_VER_RES - s_ui.top - kPagerH);
    lv_obj_add_flag(s_ui.shelf_pager, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(s_ui.shelf_pager);
    lv_obj_center(MakeLabel(s_ui.shelf_pager, s_ui.f_body));
    lv_obj_add_event_cb(s_ui.shelf_pager, OnShelfPager, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(s_ui.shelf_pager, LV_OBJ_FLAG_HIDDEN);
    for (auto& c : s_ui.cells) {
        c.buf = static_cast<uint8_t*>(heap_caps_calloc(1, 8 + kCoverBytes, MALLOC_CAP_SPIRAM));
        if (c.buf) InitPalette(c.buf);
    }

    EnsureWrImgDecoder();
    // ---- 阅读页 / 二维码 / 弹层：1bpp 索引图（0=白 1=黑）
    s_ui.buf = static_cast<uint8_t*>(heap_caps_calloc(1, 8 + (kImgW / 8) * kMaxImgH, MALLOC_CAP_SPIRAM));
    if (s_ui.buf) InitPalette(s_ui.buf);
    InitDsc(s_ui.dsc, s_ui.buf, 8, 1);
    s_ui.img = lv_image_create(scr);
    lv_obj_add_flag(s_ui.img, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(s_ui.img);
    lv_obj_add_flag(s_ui.img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_ui.img, OnImgEvent, LV_EVENT_SHORT_CLICKED, nullptr);
    s_ui.off_tag = MakeBox(scr);
    lv_obj_set_size(s_ui.off_tag, 132, 36);
    lv_obj_set_pos(s_ui.off_tag, LV_HOR_RES - 132 - 8, s_ui.top + 30);
    lv_obj_set_style_bg_color(s_ui.off_tag, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ui.off_tag, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_ui.off_tag, 6, 0);
    {
        lv_obj_t* l = MakeLabel(s_ui.off_tag, s_ui.f_body, "离线缓存");
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_obj_center(l);
    }
    lv_obj_add_flag(s_ui.off_tag, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_ui.img, OnImgEvent, LV_EVENT_LONG_PRESSED, nullptr);

    // ---- 目录 / 划线列表
    s_ui.list = MakeBox(scr);
    lv_obj_set_size(s_ui.list, LV_HOR_RES, LV_VER_RES - s_ui.top);
    lv_obj_set_pos(s_ui.list, 0, s_ui.top);
    lv_obj_add_flag(s_ui.list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* back = MakeBtn(s_ui.list, 96, 44, "返回", nullptr, 0);
    lv_obj_set_pos(back, kPad, 6);
    lv_obj_add_event_cb(back, OnListBack, LV_EVENT_CLICKED, nullptr);
    s_ui.list_title = MakeLabel(s_ui.list, s_ui.f_body);
    lv_obj_set_size(s_ui.list_title, LV_HOR_RES - kPad * 2 - 96 - 16, s_ui.f_body->line_height);
    lv_obj_set_pos(s_ui.list_title, kPad + 96 + 16, 6 + (44 - s_ui.f_body->line_height) / 2);
    lv_obj_t* hl = MakeBox(s_ui.list);
    lv_obj_set_size(hl, LV_HOR_RES, 2);
    lv_obj_set_pos(hl, 0, kHeaderH);
    lv_obj_set_style_bg_color(hl, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(hl, LV_OPA_COVER, 0);
    s_ui.list_h = LV_VER_RES - s_ui.top - kHeaderH - 12 - kPagerH;
    s_ui.list_body = MakeBox(s_ui.list);
    lv_obj_set_size(s_ui.list_body, LV_HOR_RES - kPad * 2, s_ui.list_h);
    lv_obj_set_pos(s_ui.list_body, kPad, kHeaderH + 10);
    s_ui.list_pager = MakeBox(s_ui.list);
    lv_obj_set_size(s_ui.list_pager, LV_HOR_RES, kPagerH);
    lv_obj_set_pos(s_ui.list_pager, 0, LV_VER_RES - s_ui.top - kPagerH);
    lv_obj_add_flag(s_ui.list_pager, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(s_ui.list_pager);
    lv_obj_center(MakeLabel(s_ui.list_pager, s_ui.f_body));
    lv_obj_add_event_cb(s_ui.list_pager, OnListPager, LV_EVENT_CLICKED, nullptr);

    // ---- 底部浮条
    s_ui.toast = MakeLabel(scr, s_ui.f_body);
    lv_obj_set_style_bg_color(s_ui.toast, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ui.toast, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_ui.toast, lv_color_white(), 0);
    lv_obj_set_style_radius(s_ui.toast, 10, 0);
    lv_obj_set_style_pad_hor(s_ui.toast, 18, 0);
    lv_obj_set_style_pad_ver(s_ui.toast, 10, 0);
    lv_obj_align(s_ui.toast, LV_ALIGN_BOTTOM_MID, 0, -70);
    lv_obj_add_flag(s_ui.toast, LV_OBJ_FLAG_HIDDEN);
    s_ui.timer = lv_timer_create(OnTimer, 500, nullptr);

    lv_obj_move_foreground(status.bar);
    lv_obj_move_foreground(status.overlay);
    s_ui.mode = Mode::Shelf;
    if (!S().books.empty()) RebuildGrid();

    ScreenSetIsHome(false);
    VkKeyScreenDesc desc{};
    desc.factory = WeReadScreen::Create;
    desc.on_key = OnVkKey;
    VkKey_AttachScreen(scr, kScreenName, desc);
    lv_obj_add_event_cb(scr, OnLifecycle, LV_EVENT_SCREEN_LOADED, nullptr);
    lv_obj_add_event_cb(scr, OnLifecycle, LV_EVENT_SCREEN_UNLOADED, nullptr);
    return scr;
}
