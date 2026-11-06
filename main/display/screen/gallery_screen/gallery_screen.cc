#include "gallery_screen.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>
#include <dirent.h>

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

#include "board.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "lv_adapter_display.h"
#include "power_policy.h"
#include "screen_common.h"
#include "sd_paths.h"
#include "vk_key_handler.h"

namespace {

constexpr const char* TAG = "Gallery";
constexpr const char* kScreenName = "gallery";
constexpr const char* kCfgPath = SD_APP_ROOT "/weread.json";  // 与微信读书共用服务器地址和口令
constexpr int kW = 480;

std::atomic<bool> s_active{false};
std::atomic<int> s_mode{2};  // 0 叠加+全刷底 1 叠加+局刷底 2 一次刷
int s_shown = 0;
const char* ModeName(int m) { return m == 0 ? "叠加·全刷底" : (m == 1 ? "叠加·局刷底" : "一次刷"); }
std::atomic<bool> s_busy{false};
std::atomic<int> s_gen{0};

struct Ui {
    lv_obj_t* scr = nullptr;
    lv_obj_t* img = nullptr;
    lv_obj_t* hint = nullptr;
    lv_image_dsc_t dsc{};
    uint8_t* buf = nullptr;  // 调色板 8 字节 + 1bpp 底图
    int top = 0;
    int h = 0;
};
Ui s_ui;

std::mutex s_mu;  // 保护下面这些（后台任务写、LVGL 读）
int s_idx = 0, s_count = 0;
std::string s_hint;
uint8_t* s_g2 = nullptr;  // 当前图的 2bpp（看门：重画灰度用）
bool s_base_pending = false;
std::atomic<bool> s_base_done{false};  // LVGL 已把底图画完（一次刷：进了帧缓冲；叠加：已送屏）

void WaitBase() {
    for (int i = 0; i < 60 && !s_base_done.load(); ++i) vTaskDelay(pdMS_TO_TICKS(50));
    vTaskDelay(pdMS_TO_TICKS(50));
}

constexpr const char* kLocalDir = SD_APP_ROOT "/gallery";

/** SD 卡上的 .g2 图（tools/gallery/make_g2.py 生成：8 字节头 "G2"+宽+高 + 2bpp） */
std::vector<std::string> LocalFiles() {
    std::vector<std::string> out;
    if (DIR* d = opendir(kLocalDir)) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 3 && n.compare(n.size() - 3, 3, ".g2") == 0) out.push_back(n);
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

/** 读第 idx 张本地图到 s_g2（高不够补白、超出截掉）；返回 true 成功 */
bool LoadLocal(int& idx, int& count, int h) {
    const auto files = LocalFiles();
    count = static_cast<int>(files.size());
    if (count == 0) return false;
    idx = (idx % count + count) % count;
    FILE* f = std::fopen((std::string(kLocalDir) + "/" + files[idx]).c_str(), "rb");
    if (!f) return false;
    uint8_t head[8] = {};
    bool ok = std::fread(head, 1, 8, f) == 8 && head[0] == 'G' && head[1] == '2';
    const int fw = head[2] | (head[3] << 8), fh = head[4] | (head[5] << 8);
    ok = ok && fw == kW && fh > 0;
    if (ok) {
        std::lock_guard<std::mutex> lock(s_mu);
        const size_t stride = kW / 4;
        std::memset(s_g2, 0xFF, stride * h);  // 3 = 白
        const int rows = fh < h ? fh : h;
        ok = std::fread(s_g2, 1, stride * rows, f) == stride * rows;
    }
    std::fclose(f);
    ESP_LOGI(TAG, "local %d/%d %s %s", idx + 1, count, files[idx].c_str(), ok ? "ok" : "bad");
    return ok;
}

bool LoadCfg(std::string& server, std::string& token) {
    FILE* f = std::fopen(kCfgPath, "rb");
    if (!f) return false;
    std::string t;
    char b[256];
    size_t n;
    while ((n = std::fread(b, 1, sizeof(b), f)) > 0 && t.size() < 4096) t.append(b, n);
    std::fclose(f);
    cJSON* r = cJSON_Parse(t.c_str());
    if (!r) return false;
    const cJSON* s = cJSON_GetObjectItemCaseSensitive(r, "server");
    const cJSON* k = cJSON_GetObjectItemCaseSensitive(r, "token");
    if (cJSON_IsString(s)) server = s->valuestring;
    if (cJSON_IsString(k)) token = k->valuestring;
    cJSON_Delete(r);
    while (!server.empty() && server.back() == '/') server.pop_back();
    return !server.empty();
}

void ApplyUi(void*) {
    if (!s_active.load() || s_ui.scr == nullptr) return;
    std::lock_guard<std::mutex> lock(s_mu);
    lv_label_set_text(s_ui.hint, s_hint.c_str());
    if (s_base_pending && s_g2 && s_ui.buf) {
        s_base_pending = false;
        // 一次刷、白底加深：灰在底图里画成白；黑底提亮：灰画成黑
        const bool one_shot = s_mode.load() == 2 || LVAdapterDisplay::GrayOverlayFromWhite();
        // 黑白底：黑和两级灰都画成黑（灰度叠加 LUT 再把灰像素提亮），白保持白。I1：1=黑（调色板 1）
        const int stride2 = kW / 4, stride1 = kW / 8;
        uint8_t* bits = s_ui.buf + 8;
        for (int y = 0; y < s_ui.h; ++y) {
            const uint8_t* src = s_g2 + y * stride2;
            uint8_t* dst = bits + y * stride1;
            for (int xb = 0; xb < stride1; ++xb) {
                uint8_t o = 0;
                for (int k = 0; k < 8; ++k) {
                    const int x = xb * 8 + k;
                    const int v = (src[x >> 2] >> (6 - 2 * (x & 3))) & 3;
                    // 叠加：灰先画成黑（灰度波形从黑往亮推）；一次刷：底图不上屏，灰直接记白，和刷完后一致
                    if (one_shot ? v == 0 : v != 3) o |= static_cast<uint8_t>(0x80 >> k);
                }
                dst[xb] = o;
            }
        }
        auto* d = LVAdapterDisplay::Instance();
        const int m = s_mode.load();
        ++s_shown;
        // 换图：先放开保灰。白底叠加时底图和屏上（灰已记成白）可能一模一样，靠「底图变了」放不开
        if (d) d->SetGrayHold(false);
        if (d && m == 2) d->SetAbsorbFlush(true);                       // 底图只进帧缓冲，等一次刷
        else if (d && (m == 0 || s_shown % 6 == 0)) d->RequestNextFullRefresh();  // 局刷底每 6 张全刷一次清残影
        lv_image_set_src(s_ui.img, &s_ui.dsc);
        lv_obj_invalidate(s_ui.img);
        lv_refr_now(nullptr);  // 当场画完，免得刷灰度时 LVGL 还在分块画底图，把灰又盖成黑
        s_base_done.store(true);
    }
}

void PostUi() { ScreenLvAsync(ApplyUi); }

/**
 * 灰度已上屏：把 LVGL 图里的灰像素改成白（只改内存不重画）。
 * 屏幕驱动里灰像素记的就是白；不改的话，之后任何整屏重画（通知条消失之类）都会把灰重新刷成黑。
 */
void AfterGrayShown(void*) {
    if (!s_active.load() || s_ui.buf == nullptr || s_g2 == nullptr) return;
    std::lock_guard<std::mutex> lock(s_mu);
    const int stride2 = kW / 4, stride1 = kW / 8;
    uint8_t* bits = s_ui.buf + 8;
    for (int y = 0; y < s_ui.h; ++y) {
        const uint8_t* src = s_g2 + y * stride2;
        uint8_t* dst = bits + y * stride1;
        for (int xb = 0; xb < stride1; ++xb) {
            uint8_t o = 0;
            for (int k = 0; k < 8; ++k) {
                const int x = xb * 8 + k;
                if (((src[x >> 2] >> (6 - 2 * (x & 3))) & 3) == 0) o |= static_cast<uint8_t>(0x80 >> k);
            }
            dst[xb] = o;
        }
    }
    // 状态栏的重画区域会比状态栏略高（对齐到 8 像素等），多留一点
    if (auto* d = LVAdapterDisplay::Instance()) d->SetGrayHold(true, s_ui.top + 24);  // 状态栏时钟别把灰刷掉
}

void SetHint(const std::string& t) {
    {
        std::lock_guard<std::mutex> lock(s_mu);
        s_hint = t;
    }
    PostUi();
}

/** 后台：取第 idx 张 → 上黑白底 → 等底图上屏 → 叠 4 级灰 */
void LoadTask(void* arg) {
    const int want = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    const int gen = s_gen.load();
    s_base_done.store(false);
    {
        PowerNeedHold hold(PowerNeed::OtaDownload);
        std::string server, token;
        if (want < 0) {
            // 测试图：上半 4 条色带（黑/深灰/浅灰/白），下半横向渐变（抖动到 4 级）
            const int h = s_ui.h, stride = kW / 4;
            std::lock_guard<std::mutex> lock(s_mu);
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < kW; ++x) {
                    int v;
                    if (y < h / 2) v = x * 4 / kW;
                    else {
                        const int g = x * 3 * 16 / kW;  // 0..47
                        const int bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
                        v = g / 16 + ((g % 16) > bayer[y & 3][x & 3] ? 1 : 0);
                        if (v > 3) v = 3;
                    }
                    uint8_t& b = s_g2[y * stride + (x >> 2)];
                    const int sh = 6 - 2 * (x & 3);
                    b = static_cast<uint8_t>((b & ~(3 << sh)) | (v << sh));
                }
            }
            s_hint.clear();
            s_base_pending = true;
        }
        int lidx = want, lcount = 0;
        if (want >= 0 && LoadLocal(lidx, lcount, s_ui.h)) {
            {
                std::lock_guard<std::mutex> lock(s_mu);
                s_idx = lidx;
                s_count = lcount;
                s_hint.clear();
                s_base_pending = true;
            }
            PostUi();
            WaitBase();
            if (gen == s_gen.load() && s_active.load()) {
                auto* d = LVAdapterDisplay::Instance();
                const int m = s_mode.load();
                const int rc = !d ? -1 : (m == 2 ? d->ApplyGray4Single(s_g2, 0, s_ui.top, kW, s_ui.h)
                                                 : d->ApplyGray4Overlay(s_g2, 0, s_ui.top, kW, s_ui.h));
                ESP_LOGI(TAG, "show local %d/%d mode=%d rc=%d", lidx + 1, lcount, m, rc);
                if (rc == 0) ScreenLvAsync(AfterGrayShown);
            }
        } else if (want < 0) {
            PostUi();
            WaitBase();
            if (auto* d = LVAdapterDisplay::Instance()) {
                const int rc = s_mode.load() == 2 ? d->ApplyGray4Single(s_g2, 0, s_ui.top, kW, s_ui.h)
                                                  : d->ApplyGray4Overlay(s_g2, 0, s_ui.top, kW, s_ui.h);
                ESP_LOGI(TAG, "test pattern mode=%d rc=%d", s_mode.load(), rc);
                if (rc == 0) ScreenLvAsync(AfterGrayShown);
            }
        } else if (!LoadCfg(server, token)) {
            SetHint("没找到服务器配置（weread.json）");
        } else {
            SetHint("加载中…");
            auto* network = Board::GetInstance().GetNetwork();
            auto http = network ? network->CreateHttp(0) : nullptr;
            const int h = s_ui.h;
            std::string body;
            int status = 0, idx = want, count = 0;
            if (http) {
                http->SetTimeout(30000);
                http->SetHeader("X-Token", token);
                char path[128];
                std::snprintf(path, sizeof(path), "/api/gallery/img?i=%d&w=%d&h=%d", want, kW, h);
                if (http->Open("GET", server + path)) {
                    status = http->GetStatusCode();
                    body = http->ReadAll();
                    idx = std::atoi(http->GetResponseHeader("X-I").c_str());
                    count = std::atoi(http->GetResponseHeader("X-N").c_str());
                    http->Close();
                }
            }
            const size_t need = static_cast<size_t>(kW / 4) * h;
            if (gen != s_gen.load() || !s_active.load()) {
                // 已切走
            } else if (status != 200 || body.size() != need) {
                SetHint(status == 404 ? "服务器 gallery 目录里还没有图片" :
                        "加载失败（" + std::to_string(status) + "）");
            } else {
                {
                    std::lock_guard<std::mutex> lock(s_mu);
                    std::memcpy(s_g2, body.data(), need);
                    s_idx = idx;
                    s_count = count;
                    s_hint.clear();
                    s_base_pending = true;
                }
                PostUi();
                WaitBase();
                if (gen == s_gen.load() && s_active.load()) {
                    auto* d = LVAdapterDisplay::Instance();
                    const int64_t t0 = esp_timer_get_time();
                    const int m = s_mode.load();
                    const int rc = !d ? -1 : (m == 2 ? d->ApplyGray4Single(s_g2, 0, s_ui.top, kW, h)
                                                     : d->ApplyGray4Overlay(s_g2, 0, s_ui.top, kW, h));
                    ESP_LOGI(TAG, "show %d/%d mode=%d rc=%d %dms", idx + 1, count, m, rc,
                             static_cast<int>((esp_timer_get_time() - t0) / 1000));
                    if (rc == 0) ScreenLvAsync(AfterGrayShown);
                }
            }
        }
    }
    s_busy.store(false);
    vTaskDeleteWithCaps(nullptr);
}

void Load(int idx) {
    if (s_busy.exchange(true)) return;
    s_gen.fetch_add(1);
    if (xTaskCreateWithCaps(LoadTask, "gallery", 8192, reinterpret_cast<void*>(static_cast<intptr_t>(idx)), 3, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_busy.store(false);
    }
}

void Step(int d) {
    int i;
    {
        std::lock_guard<std::mutex> lock(s_mu);
        i = s_idx + d;
        if (s_count > 0) i = (i % s_count + s_count) % s_count;
    }
    Load(i);
}

void OnTap(lv_event_t* e) {
    lv_point_t p{};
    lv_indev_get_point(lv_indev_active(), &p);
    Step(p.x >= kW / 2 ? 1 : -1);
}

void ShowModeTitle() {
    if (auto* d = LVAdapterDisplay::Instance()) {
        d->SetStatusTitlePrefix(ModeName(s_mode.load()));
        d->UpdateStatusBar(true);
    }
}

/** 长按：换显示方式，重画当前这张 */
void OnLong(lv_event_t*) {
    s_mode.store((s_mode.load() + 1) % 3);
    ESP_LOGW(TAG, "mode -> %d %s", s_mode.load(), ModeName(s_mode.load()));
    ShowModeTitle();
    int i;
    {
        std::lock_guard<std::mutex> lock(s_mu);
        i = s_idx;
    }
    Load(i);
}

void OnLifecycle(lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_SCREEN_LOADED) {
        s_active.store(true);
        ShowModeTitle();
        Load(s_idx);
    } else if (lv_event_get_target(e) == s_ui.scr) {
        s_active.store(false);
        s_gen.fetch_add(1);
        s_ui.img = nullptr;
        if (auto* d = LVAdapterDisplay::Instance()) {
            d->SetAbsorbFlush(false);
            d->SetGrayHold(false);
            d->SetStatusTitlePrefix(nullptr);
            // 灰阶图在屏上，局刷换到别的页面会留灰影：退出时全刷一次
            d->RequestFullRefreshNow();
        }
    }
}

}  // namespace

lv_obj_t* GalleryScreen::Create() {
    const int keep_idx = s_idx;
    s_ui = Ui{};
    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    s_ui.scr = scr;
    EpdStatusBar status = ScreenCreateStatusBar(scr);
    s_ui.top = status.height;
    s_ui.h = LV_VER_RES - s_ui.top;
    const size_t need2 = static_cast<size_t>(kW / 4) * s_ui.h;
    (void)need2;
    // 只分配一次（按整屏 800 行），之后反复进出复用
    static uint8_t* s_buf = nullptr;
    if (!s_g2) s_g2 = static_cast<uint8_t*>(heap_caps_calloc(1, (kW / 4) * 800, MALLOC_CAP_SPIRAM));
    if (!s_buf) s_buf = static_cast<uint8_t*>(heap_caps_calloc(1, 8 + (kW / 8) * 800, MALLOC_CAP_SPIRAM));
    s_ui.buf = s_buf;
    if (s_buf) std::memset(s_buf + 8, 0, (kW / 8) * 800);
    if (s_ui.buf) {
        lv_color32_t* pal = reinterpret_cast<lv_color32_t*>(s_ui.buf);
        pal[0] = lv_color32_make(0xFF, 0xFF, 0xFF, 0xFF);
        pal[1] = lv_color32_make(0x00, 0x00, 0x00, 0xFF);
        s_ui.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_ui.dsc.header.cf = LV_COLOR_FORMAT_I1;
        s_ui.dsc.header.w = kW;
        s_ui.dsc.header.h = s_ui.h;
        s_ui.dsc.header.stride = kW / 8;
        s_ui.dsc.data = s_ui.buf;
        s_ui.dsc.data_size = 8 + (kW / 8) * s_ui.h;
    }
    s_ui.img = lv_image_create(scr);
    lv_obj_set_pos(s_ui.img, 0, s_ui.top);
    lv_obj_set_size(s_ui.img, kW, s_ui.h);
    lv_obj_add_flag(s_ui.img, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(s_ui.img);
    lv_obj_add_event_cb(s_ui.img, OnTap, LV_EVENT_SHORT_CLICKED, nullptr);
    lv_obj_add_event_cb(s_ui.img, OnLong, LV_EVENT_LONG_PRESSED, nullptr);
    s_ui.hint = lv_label_create(scr);
    lv_obj_set_style_text_font(s_ui.hint, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(s_ui.hint, lv_color_black(), 0);
    lv_label_set_text(s_ui.hint, "");
    lv_obj_align(s_ui.hint, LV_ALIGN_CENTER, 0, 0);
    lv_obj_move_foreground(status.bar);
    lv_obj_move_foreground(status.overlay);
    ScreenSetIsHome(false);
    VkKeyScreenDesc desc{};
    desc.factory = GalleryScreen::Create;
    VkKey_AttachScreen(scr, kScreenName, desc);
    lv_obj_add_event_cb(scr, OnLifecycle, LV_EVENT_SCREEN_LOADED, nullptr);
    lv_obj_add_event_cb(scr, OnLifecycle, LV_EVENT_SCREEN_UNLOADED, nullptr);
    {
        std::lock_guard<std::mutex> lock(s_mu);
        s_idx = keep_idx;
        s_hint = "加载中…";
    }
    ESP_LOGI(TAG, "create");
    return scr;
}

bool GalleryScreen::IsActive() { return s_active.load(); }

void GalleryScreen::DebugCmd(char c) {
    if (c == 'n') Step(1);
    else if (c == 'p') Step(-1);
    else if (c == 'r') Load(s_idx);
    else if (c == 't') Load(-1);
    else if (c >= '0' && c <= '2') {
        s_mode.store(c - '0');
        ScreenLvAsync([](void*) { ShowModeTitle(); });
    }
}
